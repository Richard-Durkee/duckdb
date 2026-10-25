//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/query_graph.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/optimizer/join_order/join_relation_set.hpp"
#include "duckdb/optimizer/join_order/join_node.hpp"
#include "duckdb/optimizer/join_order/relation_manager.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/planner/column_binding.hpp"

#include <functional>

namespace duckdb {

class JoinPredicate;
struct JoinOrderOperator;

struct NeighborInfo {
public:
	NeighborInfo(optional_ptr<JoinRelationSet> neighbor, optional_ptr<JoinOrderOperator> join_operator,
	             bool generated_cross_product);

public:
	optional_ptr<JoinRelationSet> neighbor;
	optional_ptr<JoinOrderOperator> join_operator;
	bool generated_cross_product;
	vector<reference<JoinPredicate>> predicates;
};

//! The QueryGraph contains edges between relations and allows edges to be created/queried
class QueryGraphEdges {
public:
	//! Contains a node with info about neighboring relations and child edge infos
	struct QueryEdge {
		vector<unique_ptr<NeighborInfo>> neighbors;
		unordered_map<RelationIndex, unique_ptr<QueryEdge>> children;
	};

public:
	string ToString() const;
	void Print();

	//! Returns a connection if there is an edge that connects these two sets, or nullptr otherwise
	const vector<reference<NeighborInfo>> GetConnections(JoinRelationSet &node, JoinRelationSet &other) const;
	//! Returns a bitmask of the neighbors of a specific node that are not in the exclusion mask. Note that if a
	//! neighbor has multiple nodes, the bit of the lowest entry in that set is used. Only valid when every relation
	//! index is below JoinRelationSet::BITMASK_WORD_SIZE.
	idx_t GetNeighbors(JoinRelationSet &node, idx_t exclusion_mask) const;

	//! Enumerate all neighbors of a given JoinRelationSet node
	void EnumerateNeighbors(JoinRelationSet &node, const std::function<bool(NeighborInfo &)> &callback) const;
	//! Create an edge in the edge_set
	void CreateEdge(JoinRelationSet &left, JoinRelationSet &right, optional_ptr<JoinPredicate> predicate,
	                optional_ptr<JoinOrderOperator> join_operator = nullptr, bool generated_cross_product = false);

private:
	//! Get the QueryEdge of a specific node
	optional_ptr<QueryEdge> GetQueryEdge(JoinRelationSet &left);

	void EnumerateNeighborsDFS(JoinRelationSet &node, reference<QueryEdge> info, idx_t index,
	                           const std::function<bool(NeighborInfo &)> &callback) const;

private:
	QueryEdge root;
	//! Cache of the neighbor bitmask of a node before applying any exclusion mask
	mutable reference_map_t<JoinRelationSet, idx_t> neighbor_mask_cache;
};

} // namespace duckdb
