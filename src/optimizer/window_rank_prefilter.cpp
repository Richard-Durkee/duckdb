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

#include <queue>

namespace duckdb {

namespace {

//! Whether the prefilter pays off, decided once for all threads and tasks of the query
struct RankPrefilterDecision {
	//! Decide after this many rows, more when the heaps need longer to fill
	static constexpr idx_t MIN_WARMUP_ROWS = 32768;
	static constexpr idx_t MAX_WARMUP_ROWS = 262144;

	atomic<idx_t> rows_seen {0};
	atomic<idx_t> rows_kept {0};
	atomic<bool> disabled {false};

	//! Called after each chunk: stops filtering once most rows survive although the heaps could have filled
	void Update(idx_t seen, idx_t kept, idx_t limit, idx_t partitions) {
		const auto total_seen = rows_seen.fetch_add(seen) + seen;
		const auto total_kept = rows_kept.fetch_add(kept) + kept;
		const auto fill_rows = 4 * limit * MaxValue<idx_t>(partitions, 1);
		const auto warmup = MinValue<idx_t>(MaxValue<idx_t>(MIN_WARMUP_ROWS, fill_rows), MAX_WARMUP_ROWS);
		if (total_seen >= warmup && total_kept * 2 > total_seen) {
			disabled = true;
		}
	}
};

struct RankPrefilterBindData : public FunctionData {
	RankPrefilterBindData(idx_t limit_p, vector<OrderModifiers> order_modifiers_p, idx_t partition_count_p,
	                      shared_ptr<RankPrefilterDecision> decision_p)
	    : limit(limit_p), order_modifiers(std::move(order_modifiers_p)), partition_count(partition_count_p),
	      decision(std::move(decision_p)) {
	}

	idx_t limit;
	vector<OrderModifiers> order_modifiers;
	idx_t partition_count;
	shared_ptr<RankPrefilterDecision> decision;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RankPrefilterBindData>(limit, order_modifiers, partition_count, decision);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<RankPrefilterBindData>();
		return limit == other.limit && order_modifiers == other.order_modifiers &&
		       partition_count == other.partition_count;
	}
};

struct RankPrefilterHeap {
	//! The partition's sort key
	string partition_key;
	//! A max-heap of the N best (smallest) order keys of the partition seen by this thread
	std::priority_queue<string> keys;
};

struct RankPrefilterLocalState : public FunctionLocalState {
	//! Stop tracking new partitions beyond this many, and let their rows through
	static constexpr idx_t MAX_PARTITIONS = 16384;
	//! The partitions seen by this thread, by the hash of their sort key
	unordered_map<hash_t, vector<RankPrefilterHeap>> partitions;
	idx_t partition_count = 0;
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

//! Whether a row can still rank N or better: false once N rows of its partition seen here are strictly better
bool KeepRow(std::priority_queue<string> &heap, const string_t &key, idx_t limit) {
	if (heap.size() < limit) {
		heap.push(key.GetString());
		return true;
	}
	const auto cmp = CompareKeys(key, heap.top());
	if (cmp > 0) {
		return false;
	}
	if (cmp < 0) {
		heap.pop();
		heap.push(key.GetString());
	}
	return true;
}

optional_ptr<RankPrefilterHeap> FindPartition(RankPrefilterLocalState &lstate, RankPrefilterDecision &decision,
                                              hash_t hash, const string_t &key) {
	auto &bucket = lstate.partitions[hash];
	for (auto &partition : bucket) {
		if (KeysEqual(key, partition.partition_key)) {
			return &partition;
		}
	}
	if (lstate.partition_count >= RankPrefilterLocalState::MAX_PARTITIONS) {
		// too many partitions to keep heaps for: the rows per partition are too few to prune
		decision.disabled = true;
		return nullptr;
	}
	lstate.partition_count++;
	bucket.emplace_back();
	bucket.back().partition_key = key.GetString();
	return &bucket.back();
}

void RankPrefilterFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<RankPrefilterBindData>();
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<RankPrefilterLocalState>();
	const auto count = args.size();
	if (info.decision->disabled) {
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

	idx_t kept = 0;
	if (info.partition_count == 0) {
		if (lstate.partitions.empty()) {
			lstate.partitions[0].emplace_back();
		}
		auto &heap = lstate.partitions[0][0].keys;
		for (idx_t row = 0; row < count; row++) {
			const auto keep = KeepRow(heap, order_values[row].GetValue(), info.limit);
			kept += keep;
			writer.WriteValue(keep);
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
			    FindPartition(lstate, *info.decision, hash_values[row].GetValue(), partition_values[row].GetValue());
			const auto keep = !partition || KeepRow(partition->keys, order_values[row].GetValue(), info.limit);
			kept += keep;
			writer.WriteValue(keep);
		}
	}

	info.decision->Update(count, kept, info.limit, lstate.partition_count);
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

WindowRankPrefilter::WindowRankPrefilter(ClientContext &context_p) : context(context_p) {
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
		                                     window_expr.Partitions().size(), make_shared_ptr<RankPrefilterDecision>());
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
