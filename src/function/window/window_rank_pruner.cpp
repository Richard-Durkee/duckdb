#include "duckdb/function/window/window_rank_pruner.hpp"

#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

namespace {

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

struct WindowRankPrunerCounts {
	idx_t bounded = 0;
	idx_t kept_bounded = 0;
};

//! Whether a row can still rank N or better: false once N kept rows of its partition are strictly better, as known
//! from this task's heap or the shared boundary
bool KeepRow(WindowRankPrunerLocalState &lstate, WindowRankPrunerPartition &partition, const string_t &key, idx_t limit,
             WindowRankPrunerCounts &counts) {
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
		if ((full && CompareKeys(key, heap.front()) > 0) ||
		    (partition.has_boundary && CompareKeys(key, partition.boundary) > 0)) {
			return false;
		}
		counts.kept_bounded++;
	}
	if (!full) {
		if (lstate.key_count >= WindowRankPrunerLocalState::MAX_KEYS) {
			return true;
		}
		lstate.key_count++;
		heap.emplace_back(key.GetData(), key.GetSize());
		std::push_heap(heap.begin(), heap.end());
	} else if (CompareKeys(key, heap.front()) < 0) {
		std::pop_heap(heap.begin(), heap.end());
		heap.back().assign(key.GetData(), key.GetSize());
		std::push_heap(heap.begin(), heap.end());
	} else {
		return true;
	}
	if (heap.size() >= limit && !partition.dirty && (!partition.has_boundary || heap.front() < partition.boundary)) {
		partition.dirty = true;
		lstate.dirty.push_back(partition);
	}
	return true;
}

optional_ptr<WindowRankPrunerPartition> FindPartition(WindowRankPrunerLocalState &lstate,
                                                      WindowRankPrunerGlobalState &shared, hash_t hash,
                                                      const string_t &key) {
	auto &bucket = lstate.partitions[hash];
	for (auto &partition : bucket) {
		if (KeysEqual(key, partition->partition_key)) {
			return partition.get();
		}
	}
	if (lstate.partition_count >= WindowRankPrunerLocalState::MAX_PARTITIONS) {
		// too many partitions to keep heaps for: the rows per partition are too few to prune
		shared.disabled = true;
		return nullptr;
	}
	lstate.partition_count++;
	bucket.push_back(make_uniq<WindowRankPrunerPartition>());
	auto &partition = *bucket.back();
	partition.partition_key = key.GetString();
	auto &shard = shared.shards[hash % WindowRankPrunerGlobalState::SHARD_COUNT];
	lock_guard<mutex> guard(shard.lock);
	auto &boundary = shard.boundaries[partition.partition_key];
	if (!boundary) {
		boundary = make_uniq<WindowRankPrunerBoundary>();
	}
	partition.shared = boundary.get();
	return &partition;
}

//! Publishes the boundaries this task tightened
void PublishBoundaries(WindowRankPrunerLocalState &lstate) {
	for (auto &partition_ref : lstate.dirty) {
		auto &partition = partition_ref.get();
		partition.dirty = false;
		auto &boundary = *partition.shared;
		const auto &key = partition.keys.front();
		lock_guard<mutex> guard(boundary.lock);
		if (boundary.version == 0 || key < boundary.key) {
			boundary.key = key;
			boundary.version++;
		}
	}
	lstate.dirty.clear();
}

} // namespace

WindowRankPruner::WindowRankPruner(const BoundWindowExpression &wexpr, idx_t limit_p)
    : limit(limit_p), partition_count(wexpr.Partitions().size()) {
	for (auto &order : wexpr.OrderBy()) {
		key_expressions.push_back(order.expression->Copy());
		order_modifiers.emplace_back(order.type, order.null_order);
	}
	for (auto &partition : wexpr.Partitions()) {
		key_expressions.push_back(partition->Copy());
	}
}

unique_ptr<WindowRankPrunerGlobalState> WindowRankPruner::GetGlobalState() const {
	return make_uniq<WindowRankPrunerGlobalState>();
}

unique_ptr<WindowRankPrunerLocalState> WindowRankPruner::GetLocalState(ClientContext &context) const {
	auto result = make_uniq<WindowRankPrunerLocalState>(context);
	vector<LogicalType> key_types, order_types, partition_types;
	for (idx_t i = 0; i < key_expressions.size(); i++) {
		result->executor.AddExpression(*key_expressions[i]);
		key_types.push_back(key_expressions[i]->GetReturnType());
		(i < order_modifiers.size() ? order_types : partition_types).push_back(key_expressions[i]->GetReturnType());
	}
	result->key_inputs.Initialize(Allocator::Get(context), key_types);
	result->order_chunk.InitializeEmpty(order_types);
	if (!partition_types.empty()) {
		result->partition_chunk.InitializeEmpty(partition_types);
		result->partition_modifiers.assign(partition_types.size(),
		                                   OrderModifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST));
	}
	result->keys.Initialize(Allocator::Get(context), {LogicalType::BLOB, LogicalType::BLOB});
	result->sel.Initialize(STANDARD_VECTOR_SIZE);
	return result;
}

DataChunk &WindowRankPruner::Prune(WindowRankPrunerGlobalState &gstate, WindowRankPrunerLocalState &lstate,
                                   DataChunk &chunk) const {
	if (gstate.disabled) {
		return chunk;
	}
	const auto count = chunk.size();
	lstate.key_inputs.Reset();
	lstate.executor.Execute(chunk, lstate.key_inputs);
	const auto order_count = order_modifiers.size();

	lstate.keys.Reset();
	for (idx_t i = 0; i < order_count; i++) {
		lstate.order_chunk.data[i].Reference(lstate.key_inputs.data[i]);
	}
	lstate.order_chunk.SetChildCardinality(count);
	auto &order_keys = lstate.keys.data[0];
	CreateSortKeyHelpers::CreateSortKey(lstate.order_chunk, order_modifiers, order_keys);
	auto order_values = order_keys.Values<string_t>();

	WindowRankPrunerCounts counts;
	idx_t kept = 0;
	if (partition_count == 0) {
		if (lstate.partitions.empty()) {
			FindPartition(lstate, gstate, 0, string_t());
		}
		auto &partition = *lstate.partitions[0][0];
		for (idx_t row = 0; row < count; row++) {
			if (KeepRow(lstate, partition, order_values[row].GetValue(), limit, counts)) {
				lstate.sel.set_index(kept++, row);
			}
		}
	} else {
		for (idx_t i = 0; i < partition_count; i++) {
			lstate.partition_chunk.data[i].Reference(lstate.key_inputs.data[order_count + i]);
		}
		lstate.partition_chunk.SetChildCardinality(count);
		auto &partition_keys = lstate.keys.data[1];
		CreateSortKeyHelpers::CreateSortKey(lstate.partition_chunk, lstate.partition_modifiers, partition_keys);
		VectorOperations::Hash(partition_keys, lstate.hashes, count);
		auto partition_values = partition_keys.Values<string_t>();
		auto hash_values = lstate.hashes.Values<hash_t>();
		for (idx_t row = 0; row < count; row++) {
			auto partition =
			    FindPartition(lstate, gstate, hash_values[row].GetValue(), partition_values[row].GetValue());
			if (!partition || KeepRow(lstate, *partition, order_values[row].GetValue(), limit, counts)) {
				lstate.sel.set_index(kept++, row);
			}
		}
	}
	PublishBoundaries(lstate);
	gstate.Update(count, counts.bounded, counts.kept_bounded, limit, lstate.partition_count);
	if (kept == count) {
		return chunk;
	}
	if (lstate.pruned.ColumnCount() == 0) {
		lstate.pruned.InitializeEmpty(chunk.GetTypes());
	}
	lstate.pruned.Slice(chunk, lstate.sel, kept);
	return lstate.pruned;
}

} // namespace duckdb
