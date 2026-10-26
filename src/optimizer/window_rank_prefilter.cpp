#include "duckdb/optimizer/window_rank_prefilter.hpp"

#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

#include "duckdb/common/mutex.hpp"

#include <queue>

namespace duckdb {

namespace {

//! A partition's tightest known boundary: the N-th best order key of a full heap, so N kept rows are at least this good
struct RankPrefilterBoundary {
	mutex lock;
	string key;
	//! Bumped on every tightening; 0 while there is no boundary yet
	atomic<idx_t> version {0};
};

//! What all threads and tasks of the query share: the tightest known boundary of each partition, and whether
//! filtering pays off
struct RankPrefilterShared {
	//! Decide after this many rows, more when the heaps need longer to fill
	static constexpr idx_t MIN_WARMUP_ROWS = 32768;
	static constexpr idx_t MAX_WARMUP_ROWS = 262144;

	//! The partitions' boundaries by partition key, sharded by its hash so tasks rarely contend
	static constexpr idx_t SHARD_COUNT = 64;
	struct Shard {
		mutex lock;
		unordered_map<string, unique_ptr<RankPrefilterBoundary>> boundaries;
	};
	Shard shards[SHARD_COUNT];

	//! The rows filtered so far, and of those the ones that met a boundary and how many of them were kept
	atomic<idx_t> rows_seen {0};
	atomic<idx_t> bounded_seen {0};
	atomic<idx_t> bounded_kept {0};
	atomic<bool> decided {false};
	atomic<bool> disabled {false};

	//! Decides once whether filtering pays: only rows that meet a boundary can be dropped, so pruning is judged on
	//! those; if few rows meet one, the partitions are too small to prune
	void Update(idx_t seen, idx_t seen_bounded, idx_t kept_bounded, idx_t limit, idx_t partitions) {
		const auto total_seen = rows_seen.fetch_add(seen) + seen;
		const auto total_bounded = bounded_seen.fetch_add(seen_bounded) + seen_bounded;
		const auto total_kept = bounded_kept.fetch_add(kept_bounded) + kept_bounded;
		const auto fill_rows = 4 * limit * MaxValue<idx_t>(partitions, 1);
		const auto warmup = MinValue<idx_t>(MaxValue<idx_t>(MIN_WARMUP_ROWS, fill_rows), MAX_WARMUP_ROWS);
		if (decided || total_seen < warmup) {
			return;
		}
		decided = true;
		disabled = total_bounded * 4 < total_seen || total_kept * 2 > total_bounded;
	}
};

struct RankPrefilterBindData : public FunctionData {
	RankPrefilterBindData(idx_t limit_p, vector<OrderModifiers> order_modifiers_p, idx_t partition_count_p,
	                      shared_ptr<RankPrefilterShared> shared_p)
	    : limit(limit_p), order_modifiers(std::move(order_modifiers_p)), partition_count(partition_count_p),
	      shared(std::move(shared_p)) {
	}

	idx_t limit;
	vector<OrderModifiers> order_modifiers;
	idx_t partition_count;
	shared_ptr<RankPrefilterShared> shared;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RankPrefilterBindData>(limit, order_modifiers, partition_count, shared);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<RankPrefilterBindData>();
		return limit == other.limit && order_modifiers == other.order_modifiers &&
		       partition_count == other.partition_count;
	}
};

struct RankPrefilterPartition {
	//! The partition's sort key
	string partition_key;
	//! A max-heap of the N best (smallest) order keys of the partition kept by this task
	std::priority_queue<string> keys;
	//! The partition's shared boundary, and the copy of it this task last read
	optional_ptr<RankPrefilterBoundary> shared;
	idx_t version = 0;
	string boundary;
	bool has_boundary = false;
	//! Whether the heap got tighter than what this task published
	bool dirty = false;
};

struct RankPrefilterLocalState : public FunctionLocalState {
	//! Stop tracking new partitions beyond this many, and let their rows through
	static constexpr idx_t MAX_PARTITIONS = 16384;
	//! The partitions seen by this task, by the hash of their sort key
	unordered_map<hash_t, vector<unique_ptr<RankPrefilterPartition>>> partitions;
	idx_t partition_count = 0;
	vector<reference<RankPrefilterPartition>> dirty;
	DataChunk order_chunk;
	DataChunk partition_chunk;
	DataChunk keys;
	Vector hashes {LogicalType::HASH};
	vector<OrderModifiers> partition_modifiers;
};

unique_ptr<FunctionLocalState> RankPrefilterInit(ExpressionState &state, const BoundFunctionExpression &expr,
                                                 FunctionData *bind_data) {
	auto &info = bind_data->Cast<RankPrefilterBindData>();
	auto result = make_uniq<RankPrefilterLocalState>();
	auto &children = expr.GetChildren();
	const auto order_count = info.order_modifiers.size();
	vector<LogicalType> order_types, partition_types;
	for (idx_t i = 0; i < children.size(); i++) {
		(i < order_count ? order_types : partition_types).push_back(children[i]->GetReturnType());
	}
	result->order_chunk.InitializeEmpty(order_types);
	if (!partition_types.empty()) {
		result->partition_chunk.InitializeEmpty(partition_types);
		result->partition_modifiers.assign(partition_types.size(),
		                                   OrderModifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST));
	}
	result->keys.Initialize(Allocator::Get(state.GetContext()), {LogicalType::BLOB, LogicalType::BLOB});
	return std::move(result);
}

//! Compares a sort key to a stored one byte-wise, as sort keys are ordered
int CompareKeys(const string_t &key, const string &stored) {
	const auto key_size = key.GetSize();
	auto cmp = memcmp(key.GetData(), stored.data(), MinValue<idx_t>(key_size, stored.size()));
	if (cmp != 0) {
		return cmp;
	}
	return key_size < stored.size() ? -1 : (key_size > stored.size() ? 1 : 0);
}

bool KeysEqual(const string_t &key, const string &stored) {
	return key.GetSize() == stored.size() && memcmp(key.GetData(), stored.data(), stored.size()) == 0;
}

struct RankPrefilterCounts {
	idx_t bounded = 0;
	idx_t kept_bounded = 0;
};

//! Whether a row can still rank N or better: false once N kept rows of its partition are strictly better, as known
//! from this task's heap or the shared boundary
bool KeepRow(RankPrefilterLocalState &lstate, RankPrefilterPartition &partition, const string_t &key, idx_t limit,
             RankPrefilterCounts &counts) {
	auto &heap = partition.keys;
	if (partition.shared->version.load(std::memory_order_relaxed) != partition.version) {
		lock_guard<mutex> guard(partition.shared->lock);
		partition.boundary = partition.shared->key;
		partition.has_boundary = true;
		partition.version = partition.shared->version;
	}
	const bool full = heap.size() >= limit;
	if (full || partition.has_boundary) {
		counts.bounded++;
		if ((full && CompareKeys(key, heap.top()) > 0) ||
		    (partition.has_boundary && CompareKeys(key, partition.boundary) > 0)) {
			return false;
		}
		counts.kept_bounded++;
	}
	if (!full) {
		heap.push(key.GetString());
	} else if (CompareKeys(key, heap.top()) < 0) {
		heap.pop();
		heap.push(key.GetString());
	} else {
		return true;
	}
	if (heap.size() >= limit && !partition.dirty &&
	    (!partition.has_boundary || heap.top() < partition.boundary)) {
		partition.dirty = true;
		lstate.dirty.push_back(partition);
	}
	return true;
}

optional_ptr<RankPrefilterPartition> FindPartition(RankPrefilterLocalState &lstate, RankPrefilterShared &shared,
                                                   hash_t hash, const string_t &key) {
	auto &bucket = lstate.partitions[hash];
	for (auto &partition : bucket) {
		if (KeysEqual(key, partition->partition_key)) {
			return partition.get();
		}
	}
	if (lstate.partition_count >= RankPrefilterLocalState::MAX_PARTITIONS) {
		// too many partitions to keep heaps for: the rows per partition are too few to prune
		shared.disabled = true;
		return nullptr;
	}
	lstate.partition_count++;
	bucket.push_back(make_uniq<RankPrefilterPartition>());
	auto &partition = *bucket.back();
	partition.partition_key = key.GetString();
	auto &shard = shared.shards[hash % RankPrefilterShared::SHARD_COUNT];
	lock_guard<mutex> guard(shard.lock);
	auto &boundary = shard.boundaries[partition.partition_key];
	if (!boundary) {
		boundary = make_uniq<RankPrefilterBoundary>();
	}
	partition.shared = boundary.get();
	return &partition;
}

//! Publishes the boundaries this task tightened
void PublishBoundaries(RankPrefilterLocalState &lstate) {
	for (auto &partition_ref : lstate.dirty) {
		auto &partition = partition_ref.get();
		partition.dirty = false;
		auto &boundary = *partition.shared;
		const auto &key = partition.keys.top();
		lock_guard<mutex> guard(boundary.lock);
		if (boundary.version == 0 || key < boundary.key) {
			boundary.key = key;
			boundary.version++;
		}
	}
	lstate.dirty.clear();
}

void RankPrefilterFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<RankPrefilterBindData>();
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<RankPrefilterLocalState>();
	auto &shared = *info.shared;
	const auto count = args.size();
	if (shared.disabled) {
		result.Reference(Value::BOOLEAN(true), count_t(count));
		return;
	}
	auto writer = FlatVector::Writer<bool>(result, count);
	const auto order_count = info.order_modifiers.size();

	lstate.keys.Reset();
	for (idx_t i = 0; i < order_count; i++) {
		lstate.order_chunk.data[i].Reference(args.data[i]);
	}
	lstate.order_chunk.SetChildCardinality(count);
	auto &order_keys = lstate.keys.data[0];
	CreateSortKeyHelpers::CreateSortKey(lstate.order_chunk, info.order_modifiers, order_keys);
	auto order_values = order_keys.Values<string_t>();

	RankPrefilterCounts counts;
	if (info.partition_count == 0) {
		if (lstate.partitions.empty()) {
			FindPartition(lstate, shared, 0, string_t());
		}
		auto &partition = *lstate.partitions[0][0];
		for (idx_t row = 0; row < count; row++) {
			writer.WriteValue(KeepRow(lstate, partition, order_values[row].GetValue(), info.limit, counts));
		}
	} else {
		for (idx_t i = 0; i < info.partition_count; i++) {
			lstate.partition_chunk.data[i].Reference(args.data[order_count + i]);
		}
		lstate.partition_chunk.SetChildCardinality(count);
		auto &partition_keys = lstate.keys.data[1];
		CreateSortKeyHelpers::CreateSortKey(lstate.partition_chunk, lstate.partition_modifiers, partition_keys);
		VectorOperations::Hash(partition_keys, lstate.hashes, count);
		auto partition_values = partition_keys.Values<string_t>();
		auto hash_values = lstate.hashes.Values<hash_t>();
		for (idx_t row = 0; row < count; row++) {
			auto partition =
			    FindPartition(lstate, shared, hash_values[row].GetValue(), partition_values[row].GetValue());
			writer.WriteValue(!partition ||
			                  KeepRow(lstate, *partition, order_values[row].GetValue(), info.limit, counts));
		}
	}
	PublishBoundaries(lstate);
	shared.Update(count, counts.bounded, counts.kept_bounded, info.limit, lstate.partition_count);
}

ScalarFunction GetRankPrefilterFunction(const vector<LogicalType> &argument_types) {
	ScalarFunction function(Identifier("__internal_rank_prefilter"), argument_types, LogicalType::BOOLEAN,
	                        RankPrefilterFunction, nullptr, nullptr, RankPrefilterInit);
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	function.SetStability(FunctionStability::VOLATILE);
	return function;
}

//! Follows a column binding down through projections to the window that produces it
optional_ptr<LogicalWindow> FindWindow(LogicalOperator &start, ColumnBinding binding, idx_t &window_column) {
	reference<LogicalOperator> current = start;
	while (current.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = current.get().Cast<LogicalProjection>();
		if (projection.table_index != binding.table_index) {
			return nullptr;
		}
		auto &expr = *projection.expressions[binding.column_index];
		if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return nullptr;
		}
		binding = expr.Cast<BoundColumnRefExpression>().Binding();
		current = *current.get().children[0];
	}
	if (current.get().type != LogicalOperatorType::LOGICAL_WINDOW) {
		return nullptr;
	}
	auto &window = current.get().Cast<LogicalWindow>();
	if (window.window_index != binding.table_index) {
		return nullptr;
	}
	window_column = binding.column_index;
	return &window;
}

} // namespace

WindowRankPrefilter::WindowRankPrefilter(ClientContext &context_p,
                                         optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats_p)
    : context(context_p), stats(stats_p) {
}

bool WindowRankPrefilter::PartitionsTooSmall(LogicalOperator &input, const vector<unique_ptr<Expression>> &partitions,
                                             idx_t limit) {
	if (partitions.empty() || !stats) {
		return false;
	}
	const auto rows = input.EstimateCardinality(context);
	idx_t partition_count = 1;
	for (auto &partition : partitions) {
		if (partition->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false;
		}
		auto entry = stats->find(partition->Cast<BoundColumnRefExpression>().Binding());
		if (entry == stats->end() || !entry->second) {
			return false;
		}
		const auto distinct = entry->second->GetDistinctCount();
		if (distinct == 0) {
			return false;
		}
		partition_count = MinValue<idx_t>(rows, partition_count * distinct);
	}
	// a row is only dropped once N better rows of its partition were seen, which needs a few times N rows each
	return rows < 4 * limit * partition_count;
}

unique_ptr<LogicalOperator> WindowRankPrefilter::Optimize(unique_ptr<LogicalOperator> op) {
	Visit(op);
	return op;
}

void WindowRankPrefilter::Visit(unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		TryInsert(*op);
	}
	for (auto &child : op->children) {
		Visit(child);
	}
}

bool WindowRankPrefilter::TryInsert(LogicalOperator &filter_op) {
	auto &filter = filter_op.Cast<LogicalFilter>();
	for (auto &filter_expr : filter.expressions) {
		const auto comparison = filter_expr->GetExpressionType();
		if (comparison != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
		    comparison != ExpressionType::COMPARE_LESSTHAN) {
			continue;
		}
		auto &compare = filter_expr->Cast<BoundFunctionExpression>();
		auto &left = BoundComparisonExpression::Left(compare);
		auto &right = BoundComparisonExpression::Right(compare);
		if (left.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
		    right.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			continue;
		}
		auto &limit_value = right.Cast<BoundConstantExpression>().GetValue();
		if (limit_value.IsNull() || limit_value.type() != LogicalType::BIGINT) {
			continue;
		}
		auto limit = limit_value.GetValue<int64_t>();
		if (comparison == ExpressionType::COMPARE_LESSTHAN) {
			limit--;
		}
		if (limit < 1 || limit > MAX_LIMIT) {
			continue;
		}

		idx_t window_column;
		auto window = FindWindow(*filter.children[0], left.Cast<BoundColumnRefExpression>().Binding(), window_column);
		if (!window) {
			continue;
		}
		auto &rank_expr = *window->expressions[window_column];
		if (rank_expr.GetExpressionType() != ExpressionType::WINDOW_RANK) {
			continue;
		}
		// every window computed here must see the same rows: only rank() over the same partition and order
		bool same_windows = true;
		for (auto &expr : window->expressions) {
			if (!expr->Equals(rank_expr)) {
				same_windows = false;
			}
		}
		auto &window_expr = rank_expr.Cast<BoundWindowExpression>();
		if (!same_windows || window_expr.OrderBy().empty() || !window_expr.ArgOrders().empty()) {
			continue;
		}
		if (window->children[0]->type == LogicalOperatorType::LOGICAL_FILTER &&
		    window->children[0]->expressions.size() == 1 &&
		    window->children[0]->expressions[0]->GetExpressionType() == ExpressionType::BOUND_FUNCTION &&
		    window->children[0]->expressions[0]->Cast<BoundFunctionExpression>().Function().GetName() ==
		        "__internal_rank_prefilter") {
			return false; // already inserted
		}

		if (PartitionsTooSmall(*window->children[0], window_expr.Partitions(), NumericCast<idx_t>(limit))) {
			continue;
		}
		vector<unique_ptr<Expression>> arguments;
		vector<OrderModifiers> order_modifiers;
		for (auto &order : window_expr.OrderBy()) {
			arguments.push_back(order.expression->Copy());
			order_modifiers.emplace_back(order.type, order.null_order);
		}
		for (auto &partition : window_expr.Partitions()) {
			arguments.push_back(partition->Copy());
		}
		vector<LogicalType> argument_types;
		for (auto &argument : arguments) {
			argument_types.push_back(argument->GetReturnType());
		}
		BoundScalarFunction bound_function(GetRankPrefilterFunction(argument_types));
		auto bind_data =
		    make_uniq<RankPrefilterBindData>(NumericCast<idx_t>(limit), std::move(order_modifiers),
		                                     window_expr.Partitions().size(), make_shared_ptr<RankPrefilterShared>());
		auto predicate =
		    make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(arguments), std::move(bind_data));
		auto prefilter = make_uniq<LogicalFilter>(std::move(predicate));
		prefilter->children.push_back(std::move(window->children[0]));
		prefilter->ResolveOperatorTypes();
		window->children[0] = std::move(prefilter);
		return true;
	}
	return false;
}

} // namespace duckdb
