//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/join_relation_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/optimizer/join_order/relation_index.hpp"

namespace duckdb {

//! Set of relations, used in the join graph.
struct JoinRelationSet {
public:
	static constexpr idx_t BITMASK_WORD_SIZE = sizeof(idx_t) * 8;

public:
	JoinRelationSet(unsafe_unique_array<RelationIndex> relations, idx_t count);

public:
	string ToString() const;
	bool Empty() const;
	//! Returns true if every relation index in the set is below BITMASK_WORD_SIZE, i.e. the set fits in a single word
	bool FitsInWord() const;
	//! Returns the bitmask as a single word, only valid when FitsInWord() holds
	idx_t WordMask() const;
	static bool IsSubset(const JoinRelationSet &super, const JoinRelationSet &sub);
	static bool Intersects(const JoinRelationSet &left, const JoinRelationSet &right);

public:
	unsafe_unique_array<RelationIndex> relations;
	idx_t count;
	//! Bitmask over the relation indexes for fast subset/intersection checks
	vector<idx_t> bitmask;
};

//! The JoinRelationSetManager owns all JoinRelationSet objects and guarantees that each distinct set of relations is
//! represented by exactly one object, so sets can be compared and hashed by address
class JoinRelationSetManager {
public:
	//! Sets that do not fit in a single bitmask word are stored in a prefix tree over their sorted relation indexes
	struct JoinRelationTreeNode {
		unique_ptr<JoinRelationSet> relation;
		unordered_map<RelationIndex, unique_ptr<JoinRelationTreeNode>> children;
	};

public:
	//! Get an empty JoinRelationSet
	JoinRelationSet &GetEmptyJoinRelationSet();
	//! Create or get a JoinRelationSet from a single node with the given index
	JoinRelationSet &GetJoinRelation(RelationIndex index);
	//! Create or get a JoinRelationSet from a set of relation bindings
	JoinRelationSet &GetJoinRelation(const unordered_set<RelationIndex> &bindings);
	//! Create or get a JoinRelationSet from a (sorted, duplicate-free!) list of relations
	JoinRelationSet &GetJoinRelation(unsafe_unique_array<RelationIndex> relations, idx_t count);
	//! Create or get a JoinRelationSet from a single-word bitmask over relation indexes below BITMASK_WORD_SIZE
	JoinRelationSet &GetJoinRelationFromMask(idx_t mask);
	//! Union two sets of relations together and create a new relation set
	JoinRelationSet &Union(JoinRelationSet &left, JoinRelationSet &right);
	string ToString() const;
	void Print();

private:
	//! Sets whose relation indexes all fit in one word, keyed by that word
	unordered_map<idx_t, unique_ptr<JoinRelationSet>> word_sets;
	//! Single-relation sets indexed by relation index
	vector<optional_ptr<JoinRelationSet>> single_relation_sets;
	//! All other sets
	JoinRelationTreeNode root;
	optional_ptr<JoinRelationSet> empty_relation_set;
};

} // namespace duckdb
