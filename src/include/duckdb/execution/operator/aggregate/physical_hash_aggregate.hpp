//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/aggregate/distinct_aggregate_data.hpp"
#include "duckdb/execution/operator/aggregate/grouped_aggregate_data.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/radix_partitioned_hashtable.hpp"
#include "duckdb/parser/group_by_node.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

namespace duckdb {

class ClientContext;
class BufferManager;

struct DynamicFilterData;

//! Bounded int64 heap for tracking top-K group keys during accumulation.
struct TopKGroupKeyHeap {
public:
	TopKGroupKeyHeap() : capacity(0), order(OrderType::DESCENDING), merge_counter(0) {}
	TopKGroupKeyHeap(idx_t cap, OrderType ord) : capacity(cap), order(ord), merge_counter(0) {
		heap.reserve(cap + 1);
	}

	idx_t capacity;
	OrderType order;
	vector<int64_t> heap;
	idx_t merge_counter;

	bool Empty() const { return heap.empty(); }
	bool Full() const { return heap.size() >= capacity; }

	bool Insert(int64_t key) {
		if (!Full()) {
			heap.push_back(key);
			std::push_heap(heap.begin(), heap.end(), Cmp{order});
			return Full();
		}
		if (IsBetter(key, heap.front())) {
			std::pop_heap(heap.begin(), heap.end(), Cmp{order});
			heap.back() = key;
			std::push_heap(heap.begin(), heap.end(), Cmp{order});
			return true;
		}
		return false;
	}

	int64_t Boundary() const { D_ASSERT(Full()); return heap.front(); }

	void MergeFrom(TopKGroupKeyHeap &other) {
		for (auto v : other.heap) { Insert(v); }
		other.heap.clear();
	}

private:
	bool IsBetter(int64_t a, int64_t b) const {
		return (order == OrderType::DESCENDING) ? (a > b) : (a < b);
	}
	struct Cmp {
		OrderType order;
		bool operator()(int64_t a, int64_t b) const {
			return (order == OrderType::DESCENDING) ? (a < b) : (a > b);
		}
	};
};

struct TopKFilterConfig {
	idx_t group_key_index = DConstants::INVALID_INDEX;
	idx_t limit = 0;
	OrderType order = OrderType::DESCENDING;
	shared_ptr<DynamicFilterData> filter_data;
	LogicalType value_type;  //! The type the DynamicFilterData constant expects
	bool IsEnabled() const { return group_key_index != DConstants::INVALID_INDEX && filter_data; }
};

class PhysicalHashAggregate;

struct HashAggregateGroupingData {
public:
	HashAggregateGroupingData(GroupingSet &grouping_set_p, const GroupedAggregateData &grouped_aggregate_data,
	                          unique_ptr<DistinctAggregateCollectionInfo> &info, TupleDataValidityType group_validity,
	                          TupleDataValidityType distinct_validity);

public:
	RadixPartitionedHashTable table_data;
	unique_ptr<DistinctAggregateData> distinct_data;

public:
	bool HasDistinct() const;
};

struct HashAggregateGroupingGlobalState {
public:
	HashAggregateGroupingGlobalState(const HashAggregateGroupingData &data, ClientContext &context);
	// Radix state of the GROUPING_SET ht
	unique_ptr<GlobalSinkState> table_state;
	// State of the DISTINCT aggregates of this GROUPING_SET
	unique_ptr<DistinctAggregateState> distinct_state;
};

struct HashAggregateGroupingLocalState {
public:
	HashAggregateGroupingLocalState(const PhysicalHashAggregate &op, const HashAggregateGroupingData &data,
	                                ExecutionContext &context);

public:
	// Radix state of the GROUPING_SET ht
	unique_ptr<LocalSinkState> table_state;
	// Local states of the DISTINCT aggregates hashtables
	vector<unique_ptr<LocalSinkState>> distinct_states;
};

//! PhysicalHashAggregate is a group-by and aggregate implementation that uses a hash table to perform the grouping
//! This only contains read-only variables, anything that is stateful instead gets stored in the Global/Local states
class PhysicalHashAggregate : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::HASH_GROUP_BY;

public:
	PhysicalHashAggregate(PhysicalPlan &physical_plan, ClientContext &context, vector<LogicalType> types,
	                      vector<unique_ptr<Expression>> expressions, idx_t estimated_cardinality);
	PhysicalHashAggregate(PhysicalPlan &physical_plan, ClientContext &context, vector<LogicalType> types,
	                      vector<unique_ptr<Expression>> expressions, vector<unique_ptr<Expression>> groups,
	                      idx_t estimated_cardinality);
	PhysicalHashAggregate(PhysicalPlan &physical_plan, ClientContext &context, vector<LogicalType> types,
	                      vector<unique_ptr<Expression>> expressions, vector<unique_ptr<Expression>> groups,
	                      vector<GroupingSet> grouping_sets, vector<unsafe_vector<ProjectionIndex>> grouping_functions,
	                      idx_t estimated_cardinality, TupleDataValidityType group_validity,
	                      TupleDataValidityType distinct_validity);

	//! The grouping sets
	GroupedAggregateData grouped_aggregate_data;

	vector<GroupingSet> grouping_sets;
	//! The radix partitioned hash tables (one per grouping set)
	vector<HashAggregateGroupingData> groupings;
	unique_ptr<DistinctAggregateCollectionInfo> distinct_collection_info;
	//! A recreation of the input chunk, with nulls for everything that isnt a group
	vector<LogicalType> input_group_types;

	//! Filters given to Sink and friends
	unsafe_vector<idx_t> non_distinct_filter;
	unsafe_vector<idx_t> distinct_filter;

	reference_map_t<const Expression, size_t> filter_indexes;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                 GlobalSourceState &gstate) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;

	bool IsSource() const override {
		return true;
	}
	bool ParallelSource() const override {
		return true;
	}

	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::NO_ORDER;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	SinkFinalizeType FinalizeInternal(Pipeline &pipeline, Event &event, ClientContext &context, GlobalSinkState &gstate,
	                                  bool check_distinct) const;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return true;
	}
	PipelineExternalInputSupport GetExternalInputSupport() const override {
		return PipelineExternalInputSupport::SUPPORTED;
	}

	bool SinkOrderDependent() const override {
		return false;
	}

public:
	//! Top-K filter config (set by optimizer when TopN orders by a grouping key)
	TopKFilterConfig topk_config;

	InsertionOrderPreservingMap<string> ParamsToString() const override;
	//! Toggle multi-scan capability on a hash table, which prevents the scan of the aggregate from being destructive
	//! If this is not toggled the GetData method will destroy the hash table as it is scanning it
	static void SetMultiScan(GlobalSinkState &state);

private:
	//! When we only have distinct aggregates, we can delay adding groups to the main ht
	bool CanSkipRegularSink() const;

	//! Finalize the distinct aggregates
	SinkFinalizeType FinalizeDistinct(Pipeline &pipeline, Event &event, ClientContext &context,
	                                  GlobalSinkState &gstate) const;
	//! Combine the distinct aggregates
	void CombineDistinct(ExecutionContext &context, OperatorSinkCombineInput &input) const;
	//! Sink the distinct aggregates for a single grouping
	void SinkDistinctGrouping(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input,
	                          idx_t grouping_idx) const;
	//! Sink the distinct aggregates
	void SinkDistinct(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const;
	//! Create groups in the main ht for groups that would otherwise get filtered out completely
	SinkResultType SinkGroupsOnly(ExecutionContext &context, GlobalSinkState &state, LocalSinkState &lstate,
	                              DataChunk &input) const;
};

} // namespace duckdb
