#include "duckdb/optimizer/join_order/join_relation_set.hpp"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"

#include <algorithm>

namespace duckdb {

using JoinRelationTreeNode = JoinRelationSetManager::JoinRelationTreeNode;

JoinRelationSet::JoinRelationSet(unsafe_unique_array<RelationIndex> relations, idx_t count)
    : relations(std::move(relations)), count(count) {
	if (count != 0) {
		// the relations are sorted, so the last one determines the number of words
		bitmask.resize(this->relations[count - 1].index / BITMASK_WORD_SIZE + 1, 0);
		for (idx_t i = 0; i < count; i++) {
			auto index = this->relations[i].index;
			bitmask[index / BITMASK_WORD_SIZE] |= idx_t(1) << (index % BITMASK_WORD_SIZE);
		}
	}
}

// LCOV_EXCL_START
string JoinRelationSet::ToString() const {
	string result = "[";
	result += StringUtil::Join(relations, count, ", ",
	                           [](const RelationIndex &relation) { return to_string(relation.index); });
	result += "]";
	return result;
}
// LCOV_EXCL_STOP

bool JoinRelationSet::Empty() const {
	return count == 0;
}

bool JoinRelationSet::FitsInWord() const {
	return bitmask.size() <= 1;
}

idx_t JoinRelationSet::WordMask() const {
	D_ASSERT(FitsInWord());
	return bitmask.empty() ? 0 : bitmask[0];
}

//! Returns true if sub is a subset of super
bool JoinRelationSet::IsSubset(const JoinRelationSet &super, const JoinRelationSet &sub) {
	D_ASSERT(sub.count > 0);
	if (sub.count > super.count || sub.bitmask.size() > super.bitmask.size()) {
		return false;
	}
	for (idx_t i = 0; i < sub.bitmask.size(); i++) {
		if (sub.bitmask[i] & ~super.bitmask[i]) {
			return false;
		}
	}
	return true;
}

bool JoinRelationSet::Intersects(const JoinRelationSet &left, const JoinRelationSet &right) {
	const auto words = MinValue<idx_t>(left.bitmask.size(), right.bitmask.size());
	for (idx_t i = 0; i < words; i++) {
		if (left.bitmask[i] & right.bitmask[i]) {
			return true;
		}
	}
	return false;
}

JoinRelationSet &JoinRelationSetManager::GetJoinRelation(unsafe_unique_array<RelationIndex> relations, idx_t count) {
	if (count == 0 || relations[count - 1].index < JoinRelationSet::BITMASK_WORD_SIZE) {
		idx_t mask = 0;
		for (idx_t i = 0; i < count; i++) {
			mask |= idx_t(1) << relations[i].index;
		}
		auto entry = word_sets.find(mask);
		if (entry != word_sets.end()) {
			return *entry->second;
		}
		auto set = make_uniq<JoinRelationSet>(std::move(relations), count);
		auto &result = *set;
		word_sets.emplace(mask, std::move(set));
		return result;
	}
	// the set does not fit in a single word: look it up in the tree
	reference<JoinRelationTreeNode> info(root);
	for (idx_t i = 0; i < count; i++) {
		auto entry = info.get().children.find(relations[i]);
		if (entry == info.get().children.end()) {
			// node not found, create it
			auto insert_it = info.get().children.insert(make_pair(relations[i], make_uniq<JoinRelationTreeNode>()));
			entry = insert_it.first;
		}
		// move to the next node
		info = *entry->second;
	}
	// now check if the JoinRelationSet has already been created
	if (!info.get().relation) {
		// if it hasn't we need to create it
		info.get().relation = make_uniq<JoinRelationSet>(std::move(relations), count);
	}
	return *info.get().relation;
}

JoinRelationSet &JoinRelationSetManager::GetJoinRelationFromMask(idx_t mask) {
	auto entry = word_sets.find(mask);
	if (entry != word_sets.end()) {
		return *entry->second;
	}
	idx_t count = 0;
	for (auto remaining = mask; remaining != 0; remaining &= remaining - 1) {
		count++;
	}
	unsafe_unique_array<RelationIndex> relations = count == 0 ? nullptr : make_unsafe_uniq_array<RelationIndex>(count);
	idx_t i = 0;
	for (auto remaining = mask; remaining != 0; remaining &= remaining - 1) {
		relations[i++] = RelationIndex(CountZeros<uint64_t>::Trailing(remaining));
	}
	return GetJoinRelation(std::move(relations), count);
}

//! Create or get a JoinRelationSet from a single node with the given index
JoinRelationSet &JoinRelationSetManager::GetJoinRelation(RelationIndex index) {
	if (index.index >= single_relation_sets.size()) {
		single_relation_sets.resize(index.index + 1);
	}
	auto &entry = single_relation_sets[index.index];
	if (!entry) {
		auto relations = make_unsafe_uniq_array<RelationIndex>(1);
		relations[0] = index;
		entry = GetJoinRelation(std::move(relations), 1);
	}
	return *entry;
}

JoinRelationSet &JoinRelationSetManager::GetEmptyJoinRelationSet() {
	if (!empty_relation_set) {
		const unordered_set<RelationIndex> empty_bindings = {};
		empty_relation_set = GetJoinRelation(empty_bindings);
	}
	return *empty_relation_set.get();
}

JoinRelationSet &JoinRelationSetManager::GetJoinRelation(const unordered_set<RelationIndex> &bindings) {
	// create a sorted vector of the relations
	unsafe_unique_array<RelationIndex> relations =
	    bindings.empty() ? nullptr : make_unsafe_uniq_array<RelationIndex>(bindings.size());
	idx_t count = 0;
	for (auto &entry : bindings) {
		relations[count++] = entry;
	}
	std::sort(relations.get(), relations.get() + count);
	return GetJoinRelation(std::move(relations), count);
}

JoinRelationSet &JoinRelationSetManager::Union(JoinRelationSet &left, JoinRelationSet &right) {
	if (left.FitsInWord() && right.FitsInWord()) {
		return GetJoinRelationFromMask(left.WordMask() | right.WordMask());
	}
	auto relations = make_unsafe_uniq_array<RelationIndex>(left.count + right.count);
	idx_t count = 0;
	// move through the left and right relations, eliminating duplicates
	idx_t i = 0, j = 0;
	while (true) {
		if (i == left.count) {
			// exhausted left relation, add remaining of right relation
			for (; j < right.count; j++) {
				relations[count++] = right.relations[j];
			}
			break;
		} else if (j == right.count) {
			// exhausted right relation, add remaining of left
			for (; i < left.count; i++) {
				relations[count++] = left.relations[i];
			}
			break;
		} else if (left.relations[i] < right.relations[j]) {
			// left is smaller, progress left and add it to the set
			relations[count++] = left.relations[i];
			i++;
		} else if (left.relations[i] > right.relations[j]) {
			// right is smaller, progress right and add it to the set
			relations[count++] = right.relations[j];
			j++;
		} else {
			D_ASSERT(left.relations[i] == right.relations[j]);
			relations[count++] = left.relations[i];
			i++;
			j++;
		}
	}
	return GetJoinRelation(std::move(relations), count);
}

// JoinRelationSet *JoinRelationSetManager::Difference(JoinRelationSet *left, JoinRelationSet *right) {
// 	auto relations = unsafe_unique_array<idx_t>(new idx_t[left->count]);
// 	idx_t count = 0;
// 	// move through the left and right relations
// 	idx_t i = 0, j = 0;
// 	while (true) {
// 		if (i == left->count) {
// 			// exhausted left relation, we are done
// 			break;
// 		} else if (j == right->count) {
// 			// exhausted right relation, add remaining of left
// 			for (; i < left->count; i++) {
// 				relations[count++] = left->relations[i];
// 			}
// 			break;
// 		} else if (left->relations[i] == right->relations[j]) {
// 			// equivalent, add nothing
// 			i++;
// 			j++;
// 		} else if (left->relations[i] < right->relations[j]) {
// 			// left is smaller, progress left and add it to the set
// 			relations[count++] = left->relations[i];
// 			i++;
// 		} else {
// 			// right is smaller, progress right
// 			j++;
// 		}
// 	}
// 	return GetJoinRelation(std::move(relations), count);
// }

// LCOV_EXCL_START
static string JoinRelationTreeNodeToString(const JoinRelationTreeNode *node) {
	string result = "";
	if (node->relation) {
		result += node->relation.get()->ToString() + "\n";
	}
	for (auto &child : node->children) {
		result += JoinRelationTreeNodeToString(child.second.get());
	}
	return result;
}

string JoinRelationSetManager::ToString() const {
	string result;
	for (auto &entry : word_sets) {
		result += entry.second->ToString() + "\n";
	}
	result += JoinRelationTreeNodeToString(&root);
	return result;
}

void JoinRelationSetManager::Print() {
	Printer::Print(ToString());
}
// LCOV_EXCL_STOP

} // namespace duckdb
