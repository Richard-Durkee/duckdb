#include "duckdb/optimizer/join_order/query_graph.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/assert.hpp"

namespace duckdb {

using QueryEdge = QueryGraphEdges::QueryEdge;

NeighborInfo::NeighborInfo(optional_ptr<JoinRelationSet> neighbor, optional_ptr<JoinOrderOperator> join_operator,
                           bool generated_cross_product)
    : neighbor(neighbor), join_operator(join_operator), generated_cross_product(generated_cross_product) {
}

// LCOV_EXCL_START
static string QueryEdgeToString(const QueryEdge *info, vector<RelationIndex> prefix) {
	string result = "";
	string source = "[";
	for (idx_t i = 0; i < prefix.size(); i++) {
		source += to_string(prefix[i].index) + (i < prefix.size() - 1 ? ", " : "");
	}
	source += "]";
	for (auto &entry : info->neighbors) {
		result += StringUtil::Format("%s -> %s\n", source.c_str(), entry->neighbor->ToString().c_str());
	}
	for (auto &entry : info->children) {
		vector<RelationIndex> new_prefix = prefix;
		new_prefix.push_back(entry.first);
		result += QueryEdgeToString(entry.second.get(), new_prefix);
	}
	return result;
}

string QueryGraphEdges::ToString() const {
	return QueryEdgeToString(&root, {});
}

void QueryGraphEdges::Print() {
	Printer::Print(ToString());
}
// LCOV_EXCL_STOP

optional_ptr<QueryEdge> QueryGraphEdges::GetQueryEdge(JoinRelationSet &left) {
	D_ASSERT(left.count > 0);
	// find the EdgeInfo corresponding to the left set
	optional_ptr<QueryEdge> info(&root);
	for (idx_t i = 0; i < left.count; i++) {
		auto entry = info.get()->children.find(left.relations[i]);
		if (entry == info.get()->children.end()) {
			// node not found, create it
			auto insert_it = info.get()->children.insert(make_pair(left.relations[i], make_uniq<QueryEdge>()));
			entry = insert_it.first;
		}
		// move to the next node
		info = entry->second;
	}
	return info;
}

void QueryGraphEdges::CreateEdge(JoinRelationSet &left, JoinRelationSet &right, optional_ptr<JoinPredicate> predicate,
                                 optional_ptr<JoinOrderOperator> join_operator, bool generated_cross_product) {
	D_ASSERT(left.count > 0 && right.count > 0);
	// find the EdgeInfo corresponding to the left set
	auto info = GetQueryEdge(left);
	// now insert the edge to the right relation, if it does not exist
	for (idx_t i = 0; i < info->neighbors.size(); i++) {
		if (info->neighbors[i]->neighbor == right && info->neighbors[i]->join_operator == join_operator &&
		    info->neighbors[i]->generated_cross_product == generated_cross_product) {
			if (predicate) {
				// neighbor already exists, just add the predicate if we have one
				info->neighbors[i]->predicates.push_back(*predicate);
			}
			return;
		}
	}
	// neighbor does not exist, create it
	neighbor_mask_cache.clear();
	auto n = make_uniq<NeighborInfo>(&right, join_operator, generated_cross_product);
	if (info && predicate) {
		n->predicates.push_back(*predicate);
	}
	info->neighbors.push_back(std::move(n));
}

void QueryGraphEdges::EnumerateNeighborsDFS(JoinRelationSet &node, reference<QueryEdge> info, idx_t index,
                                            const std::function<bool(NeighborInfo &)> &callback) const {
	for (auto &neighbor : info.get().neighbors) {
		if (callback(*neighbor)) {
			return;
		}
	}

	for (idx_t node_index = index; node_index < node.count; ++node_index) {
		auto iter = info.get().children.find(node.relations[node_index]);
		if (iter != info.get().children.end()) {
			reference<QueryEdge> new_info = *iter->second;
			EnumerateNeighborsDFS(node, new_info, node_index + 1, callback);
		}
	}
}

void QueryGraphEdges::EnumerateNeighbors(JoinRelationSet &node,
                                         const std::function<bool(NeighborInfo &)> &callback) const {
	for (idx_t j = 0; j < node.count; j++) {
		auto iter = root.children.find(node.relations[j]);
		if (iter != root.children.end()) {
			reference<QueryEdge> new_info = *iter->second;
			EnumerateNeighborsDFS(node, new_info, j + 1, callback);
		}
	}
}

idx_t QueryGraphEdges::GetNeighbors(JoinRelationSet &node, idx_t exclusion_mask) const {
	auto entry = neighbor_mask_cache.find(node);
	if (entry == neighbor_mask_cache.end()) {
		idx_t mask = 0;
		EnumerateNeighbors(node, [&](NeighborInfo &info) -> bool {
			// add the smallest node of the neighbor to the set
			auto index = info.neighbor->relations[0].index;
			D_ASSERT(index < JoinRelationSet::BITMASK_WORD_SIZE);
			mask |= idx_t(1) << index;
			return false;
		});
		entry = neighbor_mask_cache.emplace(node, mask).first;
	}
	return entry->second & ~exclusion_mask;
}

const vector<reference<NeighborInfo>> QueryGraphEdges::GetConnections(JoinRelationSet &node,
                                                                      JoinRelationSet &other) const {
	vector<reference<NeighborInfo>> connections;
	EnumerateNeighbors(node, [&](NeighborInfo &info) -> bool {
		if (JoinRelationSet::IsSubset(other, *info.neighbor)) {
			connections.push_back(info);
		}
		return false;
	});
	return connections;
}

} // namespace duckdb
