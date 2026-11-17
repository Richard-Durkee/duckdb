//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/or_equality_to_in.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

//! Rewrites equalities between one expression and constants that are joined by OR into an IN list:
//! x = 1 OR x = 2 OR ... OR x = 9 => x IN (1, 2, ..., 9)
//! The IN clause rewriter evaluates a long IN list as a join against its values, so each row is looked up once
//! instead of being compared with every value. Lists shorter than the rewriter's threshold are left alone, because
//! it would expand them back into equalities.
class OrEqualityToInRule : public Rule {
public:
	explicit OrEqualityToInRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;
};

} // namespace duckdb
