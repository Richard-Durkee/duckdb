//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/aggregate/aggregate_topk_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/order_type.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/shared_ptr.hpp"

#include <algorithm>

namespace duckdb {
struct DynamicFilterData;

//! Config attached by the Top-N optimizer to a HashAggregate whose grouping key is the Top-N sort key.
//! It lets the aggregate drive a dynamic filter on its own scan during the build phase, so the scan can
//! prune the group keys that cannot appear in the final Top-N (intra-pipeline pruning).
struct AggregateTopKFilterInfo {
	//! The shared dynamic filter that is also pushed onto the scan below the aggregate
	shared_ptr<DynamicFilterData> filter_data;
	//! Position within the aggregate's group list that the sort key refers to
	idx_t group_index = 0;
	//! Sort order: DESCENDING keeps the largest keys, ASCENDING keeps the smallest
	OrderType order = OrderType::DESCENDING;
	//! Number of distinct groups to retain (limit + offset)
	idx_t k = 0;
	//! The type the dynamic filter constant expects
	LogicalType boundary_type;
};

//! A bounded set of the K best DISTINCT group keys seen so far. The K-th best key is the filter boundary.
//! Distinctness is essential for correctness: a high-cardinality group must not flood the set with
//! duplicates and push the boundary past keys that still belong in the result.
struct AggregateTopKBoundary {
	AggregateTopKBoundary(OrderType order, idx_t k) : order(order), k(k) {
	}

	OrderType order;
	idx_t k;
	//! Distinct keys, kept sorted ascending, size <= k
	vector<Value> keys;

	bool Full() const {
		return keys.size() >= k;
	}
	bool Empty() const {
		return keys.empty();
	}

	//! The K-th best key. Only meaningful once Full().
	const Value &Boundary() const {
		return order == OrderType::DESCENDING ? keys.front() : keys.back();
	}

	//! Insert a single key, maintaining distinctness and the K-best invariant.
	void Insert(const Value &key) {
		if (key.IsNull()) {
			return;
		}
		if (Full()) {
			// quick reject before the (more expensive) dedup search
			if (order == OrderType::DESCENDING) {
				if (!(keys.front() < key)) {
					return; // key <= smallest kept -> cannot improve the top-K
				}
			} else {
				if (!(key < keys.back())) {
					return; // key >= largest kept -> cannot improve the top-K
				}
			}
		}
		auto entry =
		    std::lower_bound(keys.begin(), keys.end(), key, [](const Value &a, const Value &b) { return a < b; });
		if (entry != keys.end() && !(key < *entry)) {
			return; // duplicate
		}
		keys.insert(entry, key);
		if (keys.size() > k) {
			if (order == OrderType::DESCENDING) {
				keys.erase(keys.begin()); // drop the smallest
			} else {
				keys.pop_back(); // drop the largest
			}
		}
	}

	//! Merge another boundary's keys into this one.
	void Merge(const AggregateTopKBoundary &other) {
		for (auto &key : other.keys) {
			Insert(key);
		}
	}
};

} // namespace duckdb
