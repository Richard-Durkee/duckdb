#include "duckdb/optimizer/group_key_encoding.hpp"
#include "duckdb/optimizer/group_key_encoding_cost.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/context.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/partial_aggregate_pushdown.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/common/printer.hpp"
#include <functional>
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

//! The memory that the dictionaries of one query may take together. A dictionary cannot spill, so the optimizer leaves
//! the dictionaries only a small share of the memory limit: it declines to encode a key whose estimated dictionary does
//! not fit into what the other dictionaries of the query have left, and at run time the dictionaries fail with an error
//! that names the memory limit when the estimates were wrong
struct GroupKeyBudget {
	explicit GroupKeyBudget(idx_t memory_limit) : planned_limit(memory_limit / 4), runtime_limit(memory_limit / 3) {
	}

	//! The estimated bytes of the dictionaries that were planned so far (optimizer thread only)
	idx_t planned_bytes = 0;
	idx_t planned_limit;
	//! The bytes the dictionaries hold, from all threads of the query
	atomic<idx_t> used_bytes {0};
	idx_t runtime_limit;

	bool Fits(idx_t estimated_bytes) const {
		return estimated_bytes <= planned_limit && planned_bytes <= planned_limit - estimated_bytes;
	}
};

namespace {

//! The dictionary of one encoded group column. Every distinct string gets an id. The dictionary is split into shards
//! by the hash of the string, so that many threads can intern the strings of the dimension at the same time. The id
//! combines the shard with the position in the shard. Every shard is an open-addressing hash table that stores the
//! hash next to the position of the string, and a string heap that owns the strings that are not inlined.
struct GroupKeyDictionary {
	static constexpr idx_t SHARD_COUNT = 64;
	static constexpr idx_t INITIAL_CAPACITY = 256;

	struct Slot {
		hash_t hash;
		//! The position of the string in the shard plus one, zero for an empty slot
		uint32_t position_plus_one;
	};

	//! The memory of a shard is allocated through the allocator of the buffer manager, so that it counts towards the
	//! memory limit
	struct Shard {
		explicit Shard(Allocator &allocator_p) : allocator(allocator_p), heap(allocator_p) {
		}

		mutex lock;
		Allocator &allocator;
		StringHeap heap;
		AllocatedData values_data;
		idx_t value_count = 0;
		idx_t value_capacity = 0;
		AllocatedData slots_data;
		idx_t slot_capacity = 0;

		string_t *Values() {
			return reinterpret_cast<string_t *>(values_data.get());
		}
		Slot *Slots() {
			return reinterpret_cast<Slot *>(slots_data.get());
		}
		void GrowValues() {
			const auto new_capacity = MaxValue<idx_t>(value_capacity * 2, INITIAL_CAPACITY);
			auto new_data = allocator.Allocate(new_capacity * sizeof(string_t));
			if (value_count > 0) {
				memcpy(new_data.get(), values_data.get(), value_count * sizeof(string_t));
			}
			values_data = std::move(new_data);
			value_capacity = new_capacity;
		}
		void GrowSlots() {
			const auto new_capacity = MaxValue<idx_t>(slot_capacity * 2, INITIAL_CAPACITY);
			auto new_data = allocator.Allocate(new_capacity * sizeof(Slot));
			auto new_slots = reinterpret_cast<Slot *>(new_data.get());
			for (idx_t i = 0; i < new_capacity; i++) {
				new_slots[i] = Slot {0, 0};
			}
			const auto mask = new_capacity - 1;
			for (idx_t i = 0; i < slot_capacity; i++) {
				auto &slot = Slots()[i];
				if (slot.position_plus_one == 0) {
					continue;
				}
				auto index = (slot.hash / SHARD_COUNT) & mask;
				while (new_slots[index].position_plus_one != 0) {
					index = (index + 1) & mask;
				}
				new_slots[index] = slot;
			}
			slots_data = std::move(new_data);
			slot_capacity = new_capacity;
		}
	};

	//! The dictionary cannot spill: it declines to grow beyond this many bytes (an estimate: strings that do not fit
	//! inline, the string references and twice the hash slots), with an error that says what to do
	GroupKeyDictionary(Allocator &allocator, shared_ptr<GroupKeyBudget> budget_p) : budget(std::move(budget_p)) {
		for (idx_t i = 0; i < SHARD_COUNT; i++) {
			shards.push_back(make_uniq<Shard>(allocator));
		}
	}

	vector<unique_ptr<Shard>> shards;
	shared_ptr<GroupKeyBudget> budget;
	//! What a serialized plan names the dictionary by, see GroupKeyDictionaryRegistry
	uint64_t registry_nonce = 0;
	uint64_t registry_sequence = 0;

	void Account(idx_t string_length) {
		const auto bytes =
		    sizeof(string_t) + 2 * sizeof(Slot) + (string_length > string_t::INLINE_LENGTH ? string_length : 0);
		if (budget->used_bytes.fetch_add(bytes) + bytes > budget->runtime_limit) {
			throw OutOfMemoryException(
			    "The dictionaries of the encoded group keys need more than %s, a third of the memory_limit. The "
			    "optimizer estimated the relations of the group keys to be smaller. Run the query again with "
			    "SET disabled_optimizers='group_key_encoding', or raise the memory_limit",
			    StringUtil::BytesToHumanReadableString(budget->runtime_limit));
		}
	}

	int32_t Intern(const string_t &input) {
		const auto hash = Hash(input);
		const auto shard_index = hash % SHARD_COUNT;
		auto &shard = *shards[shard_index];
		lock_guard<mutex> guard(shard.lock);
		if (shard.slot_capacity == 0) {
			shard.GrowSlots();
		}
		const auto mask = shard.slot_capacity - 1;
		auto index = (hash / SHARD_COUNT) & mask;
		while (shard.Slots()[index].position_plus_one != 0) {
			auto &slot = shard.Slots()[index];
			if (slot.hash == hash && shard.Values()[slot.position_plus_one - 1] == input) {
				return UnsafeNumericCast<int32_t>((slot.position_plus_one - 1) * SHARD_COUNT + shard_index);
			}
			index = (index + 1) & mask;
		}
		const auto position = shard.value_count;
		if (position >= static_cast<idx_t>(NumericLimits<int32_t>::Maximum()) / SHARD_COUNT) {
			throw OutOfRangeException("Too many distinct group keys to encode");
		}
		if (shard.value_count == shard.value_capacity) {
			shard.GrowValues();
		}
		Account(input.GetSize());
		shard.Values()[shard.value_count++] = shard.heap.AddBlob(input);
		shard.Slots()[index] = Slot {hash, UnsafeNumericCast<uint32_t>(position + 1)};
		if (shard.value_count * 2 > shard.slot_capacity) {
			shard.GrowSlots();
		}
		return UnsafeNumericCast<int32_t>(position * SHARD_COUNT + shard_index);
	}

	string_t Resolve(int32_t id) {
		if (id < 0) {
			throw InternalException("Invalid group key id");
		}
		auto &shard = *shards[static_cast<idx_t>(id) % SHARD_COUNT];
		lock_guard<mutex> guard(shard.lock);
		const auto position = static_cast<idx_t>(id) / SHARD_COUNT;
		if (position >= shard.value_count) {
			throw InternalException("Invalid group key id");
		}
		return shard.Values()[position];
	}
};

struct GroupKeyDictionaryBindData : public FunctionData {
	explicit GroupKeyDictionaryBindData(shared_ptr<GroupKeyDictionary> dictionary_p)
	    : dictionary(std::move(dictionary_p)) {
	}

	shared_ptr<GroupKeyDictionary> dictionary;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<GroupKeyDictionaryBindData>(dictionary);
	}
	bool Equals(const FunctionData &other_p) const override {
		return dictionary == other_p.Cast<GroupKeyDictionaryBindData>().dictionary;
	}
};

//! Keeps the dictionary alive for as long as a vector refers to its strings
struct GroupKeyDictionaryHolder : public AuxiliaryDataHolder {
	explicit GroupKeyDictionaryHolder(shared_ptr<GroupKeyDictionary> dictionary_p)
	    : dictionary(std::move(dictionary_p)) {
	}
	shared_ptr<GroupKeyDictionary> dictionary;
};

void GroupKeyInternFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<GroupKeyDictionaryBindData>();
	auto &dictionary = *bind_data.dictionary;
	UnaryExecutor::Execute<string_t, int32_t>(
	    args.data[0], result, args.size(), [&](const string_t &input) { return dictionary.Intern(input); },
	    FunctionErrors::CAN_THROW_RUNTIME_ERROR);
}

void GroupKeyResolveFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<GroupKeyDictionaryBindData>();
	auto &dictionary = *bind_data.dictionary;
	UnaryExecutor::Execute<int32_t, string_t>(
	    args.data[0], result, args.size(), [&](int32_t id) { return dictionary.Resolve(id); },
	    FunctionErrors::CANNOT_ERROR);
	result.AddAuxiliaryData(make_uniq<GroupKeyDictionaryHolder>(bind_data.dictionary));
}

//! The dictionaries that the plans of one connection refer to. A dictionary lives in the bind data of the functions
//! of the plan, and a plan that is serialized and deserialized again by the same connection (a copy of a plan, or the
//! plan verification) has to end up with the very same dictionary: the ids of the two sides of a comparison are only
//! comparable when they come from one dictionary. So a serialized function holds the name of its dictionary, not the
//! dictionary. The name is a pair of numbers: a random number that the registry of the connection draws once, and the
//! position of the dictionary in the registry. A plan that is deserialized by another connection, process or
//! database finds no such dictionary, and fails with an error instead of resolving ids against another dictionary.
//! The registry only holds weak references: the plan owns its dictionaries, and a dictionary is gone with its plan.
class GroupKeyDictionaryRegistry : public ClientContextState {
public:
	static constexpr const char *STATE_KEY = "group_key_dictionaries";

	GroupKeyDictionaryRegistry() {
		// not the random engine of the connection, which a seed makes predictable
		RandomEngine engine;
		nonce = engine.NextRandomInteger64();
	}

	//! Names the dictionary and records it in the registry of the connection
	static void Register(ClientContext &context, GroupKeyDictionary &dictionary,
	                     const shared_ptr<GroupKeyDictionary> &shared) {
		auto registry = context.GetContext().GetRegisteredState().GetOrCreate<GroupKeyDictionaryRegistry>(STATE_KEY);
		registry->Add(dictionary, shared);
	}

	static shared_ptr<GroupKeyDictionary> Find(ClientContext &context, uint64_t nonce, uint64_t sequence) {
		auto registry = context.GetContext().GetRegisteredState().Get<GroupKeyDictionaryRegistry>(STATE_KEY);
		return registry ? registry->Lookup(nonce, sequence) : nullptr;
	}

private:
	void Add(GroupKeyDictionary &dictionary, const shared_ptr<GroupKeyDictionary> &shared) {
		lock_guard<mutex> guard(lock);
		if (entries.size() >= purge_threshold) {
			// the dictionaries of the plans that are gone leave their entries behind: sweep when the registry has
			// doubled, so that the cost of a sweep is paid for by the registrations since the last one
			for (auto entry = entries.begin(); entry != entries.end();) {
				entry = entry->second.expired() ? entries.erase(entry) : std::next(entry);
			}
			purge_threshold = MaxValue<idx_t>(MIN_PURGE_THRESHOLD, 2 * entries.size());
		}
		dictionary.registry_nonce = nonce;
		dictionary.registry_sequence = next_sequence++;
		entries[dictionary.registry_sequence] = shared;
	}

	shared_ptr<GroupKeyDictionary> Lookup(uint64_t requested_nonce, uint64_t sequence) {
		if (requested_nonce != nonce) {
			return nullptr;
		}
		lock_guard<mutex> guard(lock);
		auto entry = entries.find(sequence);
		return entry == entries.end() ? nullptr : entry->second.lock();
	}

private:
	static constexpr idx_t MIN_PURGE_THRESHOLD = 64;

	mutex lock;
	uint64_t nonce;
	uint64_t next_sequence = 0;
	idx_t purge_threshold = MIN_PURGE_THRESHOLD;
	unordered_map<uint64_t, weak_ptr<GroupKeyDictionary>> entries;
};

void GroupKeySerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                       const BoundScalarFunction &) {
	if (!serializer.GetOptions().process_local) {
		// a reader in another connection or process cannot find the dictionary: the "not implemented" kind of error
		// is what the callers that serialize a plan treat as "this plan cannot be serialized", and fall back
		throw NotImplementedException("A plan with dictionary encoded group keys can only be serialized for the "
		                              "connection that created it");
	}
	auto &dictionary = *bind_data_p->Cast<GroupKeyDictionaryBindData>().dictionary;
	serializer.WriteProperty(100, "registry_nonce", dictionary.registry_nonce);
	serializer.WriteProperty(101, "registry_sequence", dictionary.registry_sequence);
}

unique_ptr<FunctionData> GroupKeyDeserialize(Deserializer &deserializer) {
	auto nonce = deserializer.ReadProperty<uint64_t>(100, "registry_nonce");
	auto sequence = deserializer.ReadProperty<uint64_t>(101, "registry_sequence");
	auto dictionary = GroupKeyDictionaryRegistry::Find(deserializer.Get<ClientContext &>(), nonce, sequence);
	if (!dictionary) {
		throw SerializationException("The dictionary of the encoded group keys of this plan does not exist in this "
		                             "connection: a plan with encoded group keys can only be deserialized by the "
		                             "connection that created it, while the query that created it is alive");
	}
	return make_uniq<GroupKeyDictionaryBindData>(std::move(dictionary));
}

unique_ptr<FunctionData> GroupKeyInternDeserialize(Deserializer &deserializer, BoundScalarFunction &) {
	return GroupKeyDeserialize(deserializer);
}

unique_ptr<FunctionData> GroupKeyResolveDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	// the strings have the type of the key, which the catalog entry does not know
	function.SetReturnType(deserializer.Get<const LogicalType &>());
	return GroupKeyDeserialize(deserializer);
}

//! The functions only exist to be found by the deserialization: the optimizer creates the expressions itself
unique_ptr<FunctionData> GroupKeyBind(BindScalarFunctionInput &) {
	throw BinderException("The functions of the dictionary encoded group keys are for internal use only");
}

ScalarFunction GroupKeyInternFunctionDefinition() {
	// the function is volatile because it assigns ids: this keeps the optimizer from moving the interning above a join
	ScalarFunction function(Identifier("__internal_group_key_intern"), {LogicalType::VARCHAR}, LogicalType::INTEGER,
	                        GroupKeyInternFunction, GroupKeyBind, nullptr, nullptr, LogicalType(LogicalTypeId::INVALID),
	                        FunctionStability::VOLATILE);
	function.SetSerializeCallback(GroupKeySerialize);
	function.SetDeserializeCallback(GroupKeyInternDeserialize);
	return function;
}

ScalarFunction GroupKeyResolveFunctionDefinition(const LogicalType &type) {
	ScalarFunction function(Identifier("__internal_group_key_resolve"), {LogicalType::INTEGER}, type,
	                        GroupKeyResolveFunction, GroupKeyBind);
	function.SetSerializeCallback(GroupKeySerialize);
	function.SetDeserializeCallback(GroupKeyResolveDeserialize);
	return function;
}

unique_ptr<Expression> CreateInternExpression(const shared_ptr<GroupKeyDictionary> &dictionary,
                                              unique_ptr<Expression> child) {
	BoundScalarFunction bound_function(GroupKeyInternFunctionDefinition());
	bound_function.SetReturnType(LogicalType::INTEGER);
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(child));
	return make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(arguments),
	                                          make_uniq<GroupKeyDictionaryBindData>(dictionary));
}

//! The strings have the type of the group key: a VARCHAR that carries an alias (such as JSON) keeps it
unique_ptr<Expression> CreateResolveExpression(const shared_ptr<GroupKeyDictionary> &dictionary,
                                               unique_ptr<Expression> child, const LogicalType &type) {
	BoundScalarFunction bound_function(GroupKeyResolveFunctionDefinition(type));
	bound_function.SetReturnType(type);
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(child));
	return make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(arguments),
	                                          make_uniq<GroupKeyDictionaryBindData>(dictionary));
}

idx_t Scaled(idx_t value, double scale) {
	return static_cast<idx_t>(static_cast<double>(value) * scale);
}

//! The dictionary of a relation with more rows than this is not worth building
constexpr idx_t MAX_DICTIONARY_RELATION_ROWS = 100000000;
//! The memory that a dictionary entry takes: the string, its slot in the hash table, and the string reference
constexpr idx_t ESTIMATED_BYTES_PER_KEY = 64;

//! The memory that a dictionary entry of a column of a scan takes: the default, or more if the statistics of the column
//! say that its strings are long (not every source knows the length of its longest string)
idx_t EstimateBytesPerKey(ClientContext &context, LogicalGet &get, idx_t column_index) {
	auto &column_ids = get.GetColumnIds();
	if (column_index >= column_ids.size()) {
		return ESTIMATED_BYTES_PER_KEY;
	}
	// the string is copied into the dictionary, next to its reference and its slot in the hash table
	constexpr idx_t OVERHEAD_PER_KEY = 48;
	idx_t result = ESTIMATED_BYTES_PER_KEY;
	unique_ptr<BaseStatistics> stats;
	if (get.function.statistics_extended) {
		TableFunctionGetStatisticsInput input(get.bind_data.get(), column_ids[column_index]);
		input.request_average_width = true;
		stats = get.function.statistics_extended(context, input);
		// sources that store their data in files know how many bytes a value takes on average, even where they have no
		// bound on the length of the longest string
		result = MaxValue<idx_t>(result, input.average_width + OVERHEAD_PER_KEY);
	} else if (get.function.statistics) {
		stats = get.function.statistics(context, get.bind_data.get(), column_ids[column_index].GetPrimaryIndex());
	}
	if (stats && stats->GetType().InternalType() == PhysicalType::VARCHAR && StringStats::HasMaxStringLength(*stats)) {
		result = MaxValue<idx_t>(result, StringStats::MaxStringLength(*stats) + OVERHEAD_PER_KEY);
	}
	return result;
}

//! The largest estimated scan below this operator
idx_t LargestScan(ClientContext &context, LogicalOperator &op) {
	idx_t result = 0;
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		result = op.Cast<LogicalGet>().EstimateCardinality(context);
	}
	for (auto &child : op.children) {
		result = MaxValue<idx_t>(result, LargestScan(context, *child));
	}
	return result;
}

//! Whether the operator is a chain of scans, filters and projections without joins
bool IsScanChain(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
		return op.children.empty();
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
		return op.children.size() == 1 && IsScanChain(*op.children[0]);
	default:
		return false;
	}
}

//! The estimated output of a chain of scans, filters and projections, with the filters the join order optimizer
//! would apply. Zero if some operator has no relation statistics.
idx_t ChainRows(ClientContext &context, LogicalOperator &op) {
	vector<unique_ptr<RelationStats>> keep;
	vector<reference<const RelationStats>> child_stats;
	// recompute bottom up: collect the chain, then extract its statistics from the scan upwards
	vector<LogicalOperator *> chain;
	for (auto current = &op;; current = current->children[0].get()) {
		chain.push_back(current);
		if (current->children.empty()) {
			break;
		}
	}
	for (idx_t i = chain.size(); i > 0; i--) {
		auto result = RelationStatisticsHelper::ExtractOperatorStats(*chain[i - 1], context, child_stats);
		if (!result) {
			return 0;
		}
		keep.push_back(make_uniq<RelationStats>(std::move(*result)));
		child_stats.clear();
		child_stats.emplace_back(*keep.back());
	}
	return MaxValue<idx_t>(static_cast<idx_t>(keep.back()->cardinality), 1);
}

//! The rows that a star join below an aggregate delivers before the join order is known: the rows of the largest
//! relation, reduced by the selectivity of the filters on every other relation (each join is assumed to look up one
//! row of the smaller relation per row of the largest). Falls back to the largest scan when the plan has other
//! operators or a relation without statistics.
idx_t StarJoinRows(ClientContext &context, LogicalOperator &root) {
	struct Leaf {
		idx_t base;
		idx_t filtered;
	};
	vector<Leaf> leaves;
	bool supported = true;
	std::function<void(LogicalOperator &)> collect = [&](LogicalOperator &op) {
		if (IsScanChain(op)) {
			Leaf leaf;
			leaf.base = MaxValue<idx_t>(LargestScan(context, op), 1);
			leaf.filtered = ChainRows(context, op);
			if (leaf.filtered == 0) {
				supported = false;
				return;
			}
			leaves.push_back(leaf);
			return;
		}
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_FILTER:
		case LogicalOperatorType::LOGICAL_PROJECTION:
		case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
			break;
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
			switch (op.Cast<LogicalComparisonJoin>().join_type) {
			case JoinType::INNER:
				break;
			case JoinType::SEMI:
			case JoinType::ANTI:
			case JoinType::MARK:
			case JoinType::LEFT:
			case JoinType::SINGLE:
				// only the preserved side belongs to the star: the other side can only reduce the rows further, which
				// is not counted
				collect(*op.children[0]);
				return;
			default:
				supported = false;
			}
			break;
		default:
			// not part of the star: its selectivity is unknown, count it as one
			return;
		}
		for (auto &child : op.children) {
			collect(*child);
		}
	};
	collect(root);
	if (!supported || leaves.empty()) {
		return LargestScan(context, root);
	}
	idx_t largest = 0;
	for (idx_t i = 0; i < leaves.size(); i++) {
		if (leaves[i].base > leaves[largest].base) {
			largest = i;
		}
	}
	if (leaves[largest].base < LargestScan(context, root)) {
		// the largest scan lies below an operator that is not part of the star
		return LargestScan(context, root);
	}
	double rows = static_cast<double>(leaves[largest].filtered);
	for (idx_t i = 0; i < leaves.size(); i++) {
		if (i != largest) {
			rows *=
			    MinValue<double>(1.0, static_cast<double>(leaves[i].filtered) / static_cast<double>(leaves[i].base));
		}
	}
	return MaxValue<idx_t>(static_cast<idx_t>(rows), 1);
}

struct Producer {
	//! The slot of the scan (or of the projection that computes the string) in its parent
	unique_ptr<LogicalOperator> *slot = nullptr;
	//! The projections that only pass the column on, from the top down
	vector<LogicalProjection *> passing;
	//! The binding of the column at the producer
	ColumnBinding binding;
	//! If the group is an expression of columns of the producer: the expression, over the bindings of the producer
	unique_ptr<Expression> expression;
	//! Whether an outer join lies between the producer and the group
	bool crossed_outer_join = false;
};

//! Find the scan or projection that produces a column, looking only through operators that pass their columns on
//! unchanged, and through projections that pass the column on as it is
bool FindProducer(unique_ptr<LogicalOperator> &op, ColumnBinding binding, Producer &result);
bool IsSimpleScanChain(const LogicalOperator &op);

//! Find the scan chain that supplies all columns of an expression that is evaluated above joins. The expression is
//! rewritten to refer to the columns of that chain, where it can be evaluated once per row of the chain instead of once
//! per row that reaches the aggregate
bool FindExpressionProducer(unique_ptr<LogicalOperator> &child, const Expression &expr, Producer &result) {
	if (expr.IsVolatile() || expr.CanThrow() || expr.HasSubquery() || expr.IsWindow() || expr.IsAggregate() ||
	    expr.HasParameter()) {
		return false;
	}
	bool found = true;
	for (auto lambda_class : {ExpressionClass::BOUND_LAMBDA, ExpressionClass::BOUND_LAMBDA_REF}) {
		ExpressionIterator::VisitExpressionClass(expr, lambda_class, [&](const Expression &) { found = false; });
	}
	if (!found) {
		return false;
	}
	unique_ptr<LogicalOperator> *slot = nullptr;
	column_binding_map_t<ColumnBinding> bindings;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &ref) {
		if (!found) {
			return;
		}
		Producer input;
		if (ref.Depth() != 0 || !FindProducer(child, ref.Binding(), input) || input.expression ||
		    input.crossed_outer_join || (slot && slot != input.slot)) {
			found = false;
			return;
		}
		slot = input.slot;
		bindings.emplace(ref.Binding(), input.binding);
	});
	if (!found || !slot) {
		return false;
	}
	result.slot = slot;
	result.expression = ExpressionIterator::PreorderReplace<BoundColumnRefExpression>(
	    expr.Copy(), [&](const BoundColumnRefExpression &ref) -> unique_ptr<Expression> {
		    return ref.WithBinding(bindings.at(ref.Binding()));
	    });
	return true;
}

bool FindProducer(unique_ptr<LogicalOperator> &op, ColumnBinding binding, Producer &result) {
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_GET:
		if (op->Cast<LogicalGet>().table_index != binding.table_index) {
			return false;
		}
		result.slot = &op;
		result.binding = binding;
		return true;
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = op->Cast<LogicalProjection>();
		if (projection.table_index != binding.table_index) {
			return false;
		}
		auto &expr = *projection.expressions[binding.column_index.GetIndex()];
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
		    expr.Cast<BoundColumnRefExpression>().Depth() == 0) {
			// the projection passes the column on: the producer is further down
			result.passing.push_back(&projection);
			return FindProducer(op->children[0], expr.Cast<BoundColumnRefExpression>().Binding(), result);
		}
		if (IsSimpleScanChain(*op->children[0])) {
			result.slot = &op;
			result.binding = binding;
			return true;
		}
		// the projection computes the column from the columns of a relation below joins
		result.passing.push_back(&projection);
		if (!FindExpressionProducer(op->children[0], expr, result)) {
			return false;
		}
		result.binding = binding;
		return true;
	}
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
		for (auto &child : op->children) {
			if (FindProducer(child, binding, result)) {
				if (op->type == LogicalOperatorType::LOGICAL_ANY_JOIN ||
				    (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
				     op->Cast<LogicalComparisonJoin>().join_type != JoinType::INNER)) {
					result.crossed_outer_join = true;
				}
				return true;
			}
		}
		return false;
	default:
		return false;
	}
}

//! The projections between an operator and a descendant slot, from the top down
bool CollectProjectionsToSlot(unique_ptr<LogicalOperator> &op, const unique_ptr<LogicalOperator> *target,
                              vector<LogicalProjection *> &result) {
	if (&op == target) {
		return true;
	}
	for (auto &child : op->children) {
		if (CollectProjectionsToSlot(child, target, result)) {
			if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
				result.insert(result.begin(), &op->Cast<LogicalProjection>());
			}
			return true;
		}
	}
	return false;
}

//! Whether the operator is a chain of scans, filters and projections without joins
bool IsSimpleScanChain(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
		return op.children.empty();
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
		return op.children.size() == 1 && IsSimpleScanChain(*op.children[0]);
	default:
		return false;
	}
}

//! Whether the column or expression comes from the relation in the slot. A view puts a projection of its own between
//! the scan and the aggregate, so the columns are followed down to the operator that produces them. If that does not
//! work, the table indexes of the relation decide.
bool ComesFromSlot(unique_ptr<LogicalOperator> &child, const Expression &expr, const unique_ptr<LogicalOperator> *slot,
                   const unordered_set<TableIndex> &dimension_tables) {
	Producer producer;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		if (ref.Depth() == 0 && FindProducer(child, ref.Binding(), producer)) {
			return producer.slot == slot;
		}
	} else if (FindExpressionProducer(child, expr, producer)) {
		return producer.slot == slot;
	}
	unordered_set<TableIndex> bindings;
	LogicalJoin::GetExpressionBindings(expr, bindings);
	if (bindings.empty()) {
		return false;
	}
	for (auto &table : bindings) {
		if (dimension_tables.find(table) == dimension_tables.end()) {
			return false;
		}
	}
	return true;
}

//! Whether the partial aggregate pushdown can pre-aggregate the aggregate below its join once the join order is known:
//! all groups are columns of the one relation that the dimension is, no aggregate reads that relation, and the
//! aggregates can be computed in two steps. The pushdown has more conditions (such as the share of the rows that the
//! join keeps), but they depend on the join order. A candidate that the pushdown declines is only encoded less often.
bool IsPushdownCandidate(LogicalAggregate &aggr, unique_ptr<LogicalOperator> *slot) {
	if (aggr.groups.size() < 2 || !aggr.grouping_functions.empty() || aggr.expressions.empty()) {
		return false;
	}
	unordered_set<TableIndex> dimension_tables;
	LogicalJoin::GetTableReferences(**slot, dimension_tables);
	auto &child = aggr.children[0];
	for (auto &group : aggr.groups) {
		if (!ComesFromSlot(child, *group, slot, dimension_tables)) {
			return false;
		}
	}
	for (auto &expr : aggr.expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return false;
		}
		auto &aggregate = expr->Cast<BoundAggregateExpression>();
		if (!PartialAggregatePushdown::CanPreAggregate(aggregate)) {
			return false;
		}
		bool reads_dimension = false;
		for (auto &aggregate_child : aggregate.GetChildren()) {
			ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
			    *aggregate_child, [&](const BoundColumnRefExpression &ref) {
				    if (ComesFromSlot(child, ref, slot, dimension_tables)) {
					    reads_dimension = true;
				    }
			    });
		}
		if (reads_dimension) {
			return false;
		}
	}
	return true;
}

//! The dictionary of an expression that looks a string up again, if it is one
shared_ptr<GroupKeyDictionary> GetResolveDictionary(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION ||
	    expr.GetReturnType().id() != LogicalTypeId::VARCHAR) {
		return nullptr;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	if (!function.BindInfo()) {
		return nullptr;
	}
	auto bind_data = dynamic_cast<const GroupKeyDictionaryBindData *>(function.BindInfo().get());
	if (!bind_data || function.GetChildren().size() != 1 ||
	    function.GetChildren()[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	return bind_data->dictionary;
}

idx_t CountReferences(const Expression &expr, const column_binding_map_t<ColumnBinding> &bindings) {
	idx_t count = 0;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &ref) {
		if (bindings.find(ref.Binding()) != bindings.end()) {
			count++;
		}
	});
	return count;
}

struct EncodedGroup {
	idx_t group_idx;
	//! The column at the producer
	ProjectionIndex column;
	shared_ptr<GroupKeyDictionary> dictionary;
	//! The projections that pass the column on to the aggregate, from the top down
	vector<LogicalProjection *> passing;
	//! The expression of the columns of the producer that the group is, if it is not a column of the producer
	unique_ptr<Expression> expression;
};

} // namespace

GroupKeyEncoding::GroupKeyEncoding(Optimizer &optimizer_p, bool after_join_order_p, shared_ptr<GroupKeyBudget> budget_p)
    : optimizer(optimizer_p), after_join_order(after_join_order_p), budget(std::move(budget_p)) {
}

shared_ptr<GroupKeyBudget> GroupKeyEncoding::CreateBudget(ClientContext &context) {
	return make_shared_ptr<GroupKeyBudget>(BufferManager::GetBufferManager(context).GetMaxMemory());
}

unique_ptr<Expression> GroupKeyEncoding::VisitReplace(BoundColumnRefExpression &expr,
                                                      unique_ptr<Expression> *expr_ptr) {
	auto entry = replacement_map.find(expr.Binding());
	if (entry != replacement_map.end()) {
		return expr.WithBinding(entry->second);
	}
	return nullptr;
}

void GroupKeyEncoding::VisitOperator(unique_ptr<LogicalOperator> &op) {
	LogicalOperatorVisitor::VisitOperator(op);
	if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		TryEncode(op);
	} else {
		TryPullUpResolve(op);
	}
}

bool GroupKeyEncoding::TryEncode(unique_ptr<LogicalOperator> &op) {
	auto &aggr = op->Cast<LogicalAggregate>();
	if (aggr.groups.empty() || aggr.children.size() != 1) {
		return false;
	}
	// grouping sets are encoded before the join order is known (their expansion needs the ids), all other
	// aggregates afterwards, when the number of rows that reach the aggregate has been estimated
	if ((aggr.grouping_sets.size() > 1) == after_join_order) {
		return false;
	}
	for (auto &expr : aggr.expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE ||
		    expr->Cast<BoundAggregateExpression>().StateExportMode() != AggregateStateExportMode::NONE) {
			// the output of aggregates that export their state is consumed by a cascade of grouping sets
			return false;
		}
	}
	const double dim_scale = Settings::Get<DebugGroupKeyEncodingDimensionScaleSetting>(optimizer.context);
	bool has_candidate = false;
	for (auto &group : aggr.groups) {
		if (group->GetReturnType().id() == LogicalTypeId::VARCHAR &&
		    StringType::GetCollation(group->GetReturnType()).empty()) {
			has_candidate = true;
			break;
		}
	}
	if (!has_candidate) {
		return false;
	}
	// tests (and experiments) can force the encoding of every group key of a smaller scan
	const bool force = Settings::Get<DebugForceGroupKeyEncodingSetting>(optimizer.context);
	const auto threads = NumericCast<idx_t>(TaskScheduler::GetScheduler(optimizer.context).NumberOfThreads());
	auto decide = [&](idx_t dimension_rows, idx_t input_rows) {
		GroupKeyEncodingCostInput cost_input;
		cost_input.early = !after_join_order;
		cost_input.input_rows = input_rows;
		cost_input.dimension_rows = dimension_rows;
		cost_input.group_rows =
		    aggr.has_estimated_cardinality ? aggr.estimated_cardinality : MinValue<idx_t>(input_rows, dimension_rows);
		cost_input.grouping_sets = MaxValue<idx_t>(aggr.grouping_sets.size(), 1);
		cost_input.threads = threads;
		return DecideGroupKeyEncoding(cost_input);
	};
	// The rows that reach the aggregate are estimated in steps, each more expensive and more precise than the one
	// before, and a step that already declines the encoding spares the others (the benefit grows with the rows):
	// the largest scan, then after the join order the join order's estimate (bounded by what the inputs of the joins
	// produce), and then the relation statistics of the plan (filters and join conditions below the aggregate are not
	// yet ordered, but their selectivity is known). The join order estimate is only as good as the statistics of the
	// join keys (a Hive partition column has none): the encoding has to pay off under the star join estimate as well
	idx_t largest_scan = 0;
	idx_t input_rows = 0;
	bool scans_estimated = false;
	bool input_estimated = false;
	double encoded_benefit_ns = 0;
	// The partial aggregate pushdown runs after the join order. When all groups come from one relation, it
	// pre-aggregates the other side of the join by the join key, and the aggregate only reads about as many rows as
	// that relation has. Every one of those rows was interned, so such a key cannot pay for its dictionary. The
	// grouping sets are encoded before the pushdown, so their input rows have to be estimated with it in mind.
	const bool pushdown_enabled = !optimizer.OptimizerDisabled(OptimizerType::PARTIAL_AGGREGATE_PUSHDOWN);
	auto is_worthwhile = [&](idx_t dimension_rows_p, idx_t bytes_per_key, idx_t group_idx, bool free_rider_round,
	                         bool pre_aggregated) {
		const auto dimension_rows = Scaled(dimension_rows_p, dim_scale);
		if (dimension_rows == 0 || dimension_rows > MAX_DICTIONARY_RELATION_ROWS) {
			return false;
		}
		// the dictionary stays in memory until the query ends, it cannot spill: all dictionaries of the query together
		// get a small share of the memory
		if (!budget->Fits(dimension_rows * bytes_per_key)) {
			return false;
		}
		if (free_rider_round && encoded_benefit_ns == 0) {
			return false;
		}
		if (!scans_estimated) {
			scans_estimated = true;
			largest_scan = LargestScan(optimizer.context, *aggr.children[0]);
		}
		if (!force && !free_rider_round && !input_estimated) {
			if (!decide(dimension_rows, largest_scan).encode) {
				return false;
			}
			idx_t bounded = largest_scan;
			if (after_join_order) {
				bounded = BoundedInputRows(optimizer.context, *aggr.children[0]);
				if (!decide(dimension_rows, bounded).encode) {
					return false;
				}
			}
			input_rows = MinValue<idx_t>(bounded, StarJoinRows(optimizer.context, *aggr.children[0]));
			if (pre_aggregated) {
				input_rows = MinValue<idx_t>(input_rows, PreAggregatedJoinRows(dimension_rows));
			}
			input_estimated = true;
		} else if (!input_estimated) {
			// forced: the estimates are not used
			input_estimated = true;
			input_rows = largest_scan;
		}
		const auto decision = decide(dimension_rows, input_rows);
		bool encode = force ? dimension_rows < largest_scan : decision.encode;
		if (!encode && free_rider_round && IsFreeRider(decision, encoded_benefit_ns)) {
			encode = true;
		}
		if (encode && !free_rider_round) {
			encoded_benefit_ns += decision.benefit_cpu_ns;
		}
		return encode;
	};
	// the scan slot (in its parent) and the encoded groups of that scan
	vector<pair<unique_ptr<LogicalOperator> *, vector<EncodedGroup>>> scans;
	vector<bool> encoded_group(aggr.groups.size(), false);
	// a second round takes in the keys that cost next to nothing, once the aggregate is encoded anyway: the strings
	// of the dimension are small, and no string is left in the rows of the hash table
	for (idx_t round = 0; round < 2; round++) {
		for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
			if (encoded_group[group_idx]) {
				continue;
			}
			auto &group = *aggr.groups[group_idx];
			if (group.GetReturnType().id() != LogicalTypeId::VARCHAR ||
			    !StringType::GetCollation(group.GetReturnType()).empty()) {
				continue;
			}
			Producer found;
			bool found_producer;
			if (group.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				found_producer =
				    group.Cast<BoundColumnRefExpression>().Depth() == 0 &&
				    FindProducer(aggr.children[0], group.Cast<BoundColumnRefExpression>().Binding(), found);
			} else {
				// an expression of the columns of one relation, below joins
				found_producer = FindExpressionProducer(aggr.children[0], group, found);
			}
			if (!found_producer) {
				continue;
			}
			auto slot = found.slot;
			auto binding = found.binding;
			auto &producer = **slot;
			idx_t producer_cardinality;
			idx_t bytes_per_key = ESTIMATED_BYTES_PER_KEY;
			if (producer.type == LogicalOperatorType::LOGICAL_GET) {
				auto &get = producer.Cast<LogicalGet>();
				if (!get.projection_ids.empty() || !get.projected_input.empty() || !get.children.empty() ||
				    get.GetColumnIds().empty()) {
					continue;
				}
				producer_cardinality = get.EstimateCardinality(optimizer.context);
				if (!found.expression) {
					bytes_per_key = EstimateBytesPerKey(optimizer.context, get, binding.column_index.GetIndex());
				}
			} else {
				if (!IsSimpleScanChain(*producer.children[0])) {
					continue;
				}
				producer_cardinality = LargestScan(optimizer.context, producer);
			}
			const bool pre_aggregated = !after_join_order && pushdown_enabled && IsPushdownCandidate(aggr, slot);
			if (!is_worthwhile(producer_cardinality, bytes_per_key, group_idx, round == 1, pre_aggregated)) {
				continue;
			}
			vector<EncodedGroup> *encoded = nullptr;
			for (auto &scan : scans) {
				if (scan.first == slot) {
					encoded = &scan.second;
				}
			}
			if (!encoded) {
				scans.emplace_back(slot, vector<EncodedGroup>());
				encoded = &scans.back().second;
			}
			if (found.expression) {
				// the id has to be handed on by every projection between the relation and the aggregate
				found.passing.clear();
				CollectProjectionsToSlot(aggr.children[0], slot, found.passing);
			}
			encoded_group[group_idx] = true;
			budget->planned_bytes += Scaled(producer_cardinality, dim_scale) * bytes_per_key;
			auto dictionary = make_shared_ptr<GroupKeyDictionary>(BufferAllocator::Get(optimizer.context), budget);
			GroupKeyDictionaryRegistry::Register(optimizer.context, *dictionary, dictionary);
			encoded->push_back(EncodedGroup {group_idx, binding.column_index, std::move(dictionary),
			                                 std::move(found.passing), std::move(found.expression)});
		}
	}
	if (scans.empty()) {
		return false;
	}

	// remember the layout of the aggregate output before it changes
	aggr.ResolveOperatorTypes();
	const auto old_types = aggr.types;
	const auto old_bindings = aggr.GetColumnBindings();
	vector<shared_ptr<GroupKeyDictionary>> group_dictionaries(aggr.groups.size());

	// the group refers to the interned column, and to the columns the passing projections hand it on in
	auto point_group_to = [&](EncodedGroup &encoded, ColumnBinding binding) {
		for (idx_t i = encoded.passing.size(); i > 0; i--) {
			auto &passing = *encoded.passing[i - 1];
			const ColumnBinding passed(passing.table_index, ProjectionIndex(passing.expressions.size()));
			passing.expressions.push_back(make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, binding));
			binding = passed;
		}
		aggr.groups[encoded.group_idx] = make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, binding);
		group_dictionaries[encoded.group_idx] = encoded.dictionary;
	};

	for (auto &scan : scans) {
		auto &slot = *scan.first;
		if (slot->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			// append the interned columns to the existing projection
			auto &projection = slot->Cast<LogicalProjection>();
			for (auto &encoded : scan.second) {
				auto input = encoded.expression ? std::move(encoded.expression)
				                                : projection.expressions[encoded.column.GetIndex()]->Copy();
				const ColumnBinding interned(projection.table_index, ProjectionIndex(projection.expressions.size()));
				projection.expressions.push_back(CreateInternExpression(encoded.dictionary, std::move(input)));
				point_group_to(encoded, interned);
			}
			continue;
		}
		auto &get = slot->Cast<LogicalGet>();
		get.ResolveOperatorTypes();
		const auto get_types = get.types;
		const auto get_bindings = get.GetColumnBindings();
		const auto old_index = get.table_index;
		const auto scan_index = optimizer.binder.GenerateTableIndex();

		// the projection takes over the table index of the scan, so that all references to the scan stay valid
		vector<unique_ptr<Expression>> expressions;
		for (idx_t i = 0; i < get_bindings.size(); i++) {
			expressions.push_back(make_uniq<BoundColumnRefExpression>(
			    get_types[i], ColumnBinding(scan_index, get_bindings[i].column_index)));
		}
		for (auto &encoded : scan.second) {
			unique_ptr<Expression> input;
			if (encoded.expression) {
				// the expression refers to the scan by its old table index
				input = ExpressionIterator::PreorderReplace<BoundColumnRefExpression>(
				    std::move(encoded.expression), [&](const BoundColumnRefExpression &ref) -> unique_ptr<Expression> {
					    return ref.WithBinding(ColumnBinding(scan_index, ref.Binding().column_index));
				    });
			} else {
				const auto column = encoded.column.GetIndex();
				input = make_uniq<BoundColumnRefExpression>(get_types[column],
				                                            ColumnBinding(scan_index, ProjectionIndex(column)));
			}
			const ColumnBinding interned(old_index, ProjectionIndex(expressions.size()));
			expressions.push_back(CreateInternExpression(encoded.dictionary, std::move(input)));
			point_group_to(encoded, interned);
		}
		get.table_index = scan_index;
		auto projection = make_uniq<LogicalProjection>(old_index, std::move(expressions));
		if (get.has_estimated_cardinality) {
			projection->SetEstimatedCardinality(get.estimated_cardinality);
		}
		projection->children.push_back(std::move(slot));
		slot = std::move(projection);
	}
	aggr.group_stats.clear();

	// look the strings up again right above the aggregate
	const auto output_index = optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> output_expressions;
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		if (i < group_dictionaries.size() && group_dictionaries[i]) {
			output_expressions.push_back(CreateResolveExpression(
			    group_dictionaries[i], make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, old_bindings[i]),
			    old_types[i]));
		} else {
			output_expressions.push_back(make_uniq<BoundColumnRefExpression>(old_types[i], old_bindings[i]));
		}
		replacement_map[old_bindings[i]] = ColumnBinding(output_index, ProjectionIndex(i));
	}
	auto output = make_uniq<LogicalProjection>(output_index, std::move(output_expressions));
	if (aggr.has_estimated_cardinality) {
		output->SetEstimatedCardinality(aggr.estimated_cardinality);
	}
	output->children.push_back(std::move(op));
	op = std::move(output);
	return true;
}

//! Move the lookup of the strings above this operator, if the operator only needs the ids. This keeps the strings out
//! of everything between the aggregate and the operators that really need them (such as a final sort). The lookup keeps
//! the columns it had, and the columns that a window adds are handed on through it, so that every operator above can
//! still read them.
bool GroupKeyEncoding::TryPullUpResolve(unique_ptr<LogicalOperator> &op) {
	if (op->children.size() != 1 || op->HasProjectionMap()) {
		return false;
	}
	if (op->type != LogicalOperatorType::LOGICAL_FILTER && op->type != LogicalOperatorType::LOGICAL_WINDOW &&
	    op->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return false;
	}
	if (op->children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return false;
	}
	auto &lookup = op->children[0]->Cast<LogicalProjection>();
	// the lookup projection only renames columns and looks strings up
	column_binding_map_t<ColumnBinding> resolved; // output of the lookup -> the id it is looked up from
	column_binding_map_t<shared_ptr<GroupKeyDictionary>> dictionaries;
	column_binding_map_t<ColumnBinding> passed; // other output of the lookup -> its input
	for (idx_t i = 0; i < lookup.expressions.size(); i++) {
		const ColumnBinding output(lookup.table_index, ProjectionIndex(i));
		auto &expr = *lookup.expressions[i];
		auto dictionary = GetResolveDictionary(expr);
		if (dictionary) {
			auto input =
			    expr.Cast<BoundFunctionExpression>().GetChildren()[0]->Cast<BoundColumnRefExpression>().Binding();
			resolved[output] = input;
			dictionaries[output] = std::move(dictionary);
		} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			passed[output] = expr.Cast<BoundColumnRefExpression>().Binding();
		} else {
			return false;
		}
	}
	if (resolved.empty()) {
		return false;
	}

	ColumnBindingReplacer replacer;
	for (auto &entry : passed) {
		replacer.replacement_bindings.emplace_back(entry.first, entry.second);
	}
	for (auto &entry : resolved) {
		replacer.replacement_bindings.emplace_back(entry.first, entry.second, LogicalType::INTEGER);
	}

	// the output columns of this operator that hold an id instead of a string
	vector<pair<idx_t, shared_ptr<GroupKeyDictionary>>> resolved_outputs;
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_FILTER: {
		for (auto &expr : op->expressions) {
			if (CountReferences(*expr, resolved) != 0) {
				return false;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_WINDOW: {
		for (auto &expr : op->expressions) {
			if (expr->GetExpressionClass() != ExpressionClass::BOUND_WINDOW) {
				return false;
			}
			// a string may only be used as a partition (the ids partition identically)
			idx_t partition_references = 0;
			for (auto &partition : expr->Cast<BoundWindowExpression>().Partitions()) {
				if (partition->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
				    resolved.find(partition->Cast<BoundColumnRefExpression>().Binding()) != resolved.end()) {
					partition_references++;
				}
			}
			if (CountReferences(*expr, resolved) != partition_references) {
				return false;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = op->Cast<LogicalProjection>();
		for (idx_t i = 0; i < projection.expressions.size(); i++) {
			auto &expr = *projection.expressions[i];
			if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				auto entry = dictionaries.find(expr.Cast<BoundColumnRefExpression>().Binding());
				if (entry != dictionaries.end()) {
					resolved_outputs.emplace_back(i, entry->second);
					continue;
				}
			}
			if (CountReferences(expr, resolved) != 0) {
				return false;
			}
		}
		break;
	}
	default:
		return false;
	}

	if (op->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		replacer.VisitOperatorBindings(*op);
		auto lookup_op = std::move(op->children[0]);
		op->children[0] = std::move(lookup_op->children[0]);
		if (op->type == LogicalOperatorType::LOGICAL_WINDOW) {
			// the columns of the window sit next to the columns of its child, but the lookup that now sits above the
			// window only has its own columns: hand the columns of the window on through the lookup and let the
			// operators above read them there
			auto &window = op->Cast<LogicalWindow>();
			auto &lookup_projection = lookup_op->Cast<LogicalProjection>();
			for (idx_t i = 0; i < window.expressions.size(); i++) {
				const ColumnBinding window_binding(window.window_index, ProjectionIndex(i));
				const ColumnBinding lookup_binding(lookup_projection.table_index,
				                                   ProjectionIndex(lookup_projection.expressions.size()));
				lookup_projection.expressions.push_back(
				    make_uniq<BoundColumnRefExpression>(window.expressions[i]->GetReturnType(), window_binding));
				replacement_map[window_binding] = lookup_binding;
			}
		}
		lookup_op->children[0] = std::move(op);
		op = std::move(lookup_op);
		return true;
	}

	// a projection above the lookup: let the projection hand the ids on, and look them up in a projection above it
	auto &projection = op->Cast<LogicalProjection>();
	projection.ResolveOperatorTypes();
	const auto old_types = projection.types;
	const auto old_index = projection.table_index;
	const auto new_index = optimizer.binder.GenerateTableIndex();
	replacer.VisitOperatorBindings(projection);
	projection.table_index = new_index;
	vector<unique_ptr<Expression>> expressions;
	for (idx_t i = 0; i < old_types.size(); i++) {
		const ColumnBinding binding(new_index, ProjectionIndex(i));
		shared_ptr<GroupKeyDictionary> dictionary;
		for (auto &entry : resolved_outputs) {
			if (entry.first == i) {
				dictionary = entry.second;
			}
		}
		if (dictionary) {
			expressions.push_back(CreateResolveExpression(
			    dictionary, make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, binding), old_types[i]));
		} else {
			expressions.push_back(make_uniq<BoundColumnRefExpression>(old_types[i], binding));
		}
	}
	auto lookup_op = std::move(op->children[0]);
	op->children[0] = std::move(lookup_op->children[0]);
	auto new_lookup = make_uniq<LogicalProjection>(old_index, std::move(expressions));
	if (op->has_estimated_cardinality) {
		new_lookup->SetEstimatedCardinality(op->estimated_cardinality);
	}
	new_lookup->children.push_back(std::move(op));
	op = std::move(new_lookup);
	return true;
}

ScalarFunction InternalGroupKeyInternFun::GetFunction() {
	return GroupKeyInternFunctionDefinition();
}

ScalarFunction InternalGroupKeyResolveFun::GetFunction() {
	return GroupKeyResolveFunctionDefinition(LogicalType::VARCHAR);
}

} // namespace duckdb
