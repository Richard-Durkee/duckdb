#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"

namespace duckdb {

bool Binder::BindsReferencesOnly() {
	auto mode = GetBindingMode();
	return mode == BindingMode::EXTRACT_NAMES || mode == BindingMode::EXTRACT_QUALIFIED_NAMES ||
	       mode == BindingMode::EXTRACT_CATALOG_LOOKUPS;
}

BoundStatement Binder::BindDummyScan(optional_ptr<const Identifier> alias) {
	auto table_index = GenerateTableIndex();
	BoundStatement result;
	result.types = {LogicalType::INTEGER};
	result.names = {Identifier("__dummy_col" + to_string(table_index.index))};
	result.plan = make_uniq<LogicalDummyScan>(table_index);
	if (alias) {
		bind_context.AddGenericBinding(table_index, *alias, result.names, result.types);
	}
	return result;
}

BoundStatement Binder::BindDMLReferences(const vector<reference<TableRef>> &refs,
                                         const vector<reference<unique_ptr<ParsedExpression>>> &expressions) {
	for (auto &ref : refs) {
		Bind(ref.get());
	}
	for (auto &expression : expressions) {
		auto &expr = expression.get();
		if (!expr || expr->GetExpressionClass() == ExpressionClass::DEFAULT ||
		    expr->GetExpressionClass() == ExpressionClass::STAR) {
			continue;
		}
		WhereBinder expression_binder(*this, context);
		expression_binder.Bind(expr);
	}
	return BindDummyScan();
}

static bool ExtractsCatalogLookups(const SQLStatement &statement) {
	switch (statement.type) {
	case StatementType::SELECT_STATEMENT:
	case StatementType::INSERT_STATEMENT:
	case StatementType::UPDATE_STATEMENT:
	case StatementType::DELETE_STATEMENT:
	case StatementType::MERGE_INTO_STATEMENT:
	case StatementType::RELATION_STATEMENT:
	case StatementType::COPY_STATEMENT:
		return true;
	case StatementType::EXPLAIN_STATEMENT:
		return ExtractsCatalogLookups(*statement.Cast<ExplainStatement>().stmt);
	case StatementType::CREATE_STATEMENT: {
		auto &info = *statement.Cast<CreateStatement>().info;
		return info.type == CatalogType::VIEW_ENTRY ||
		       (info.type == CatalogType::TABLE_ENTRY && info.Cast<CreateTableInfo>().query);
	}
	default:
		// e.g. binding EXECUTE evaluates its parameters
		return false;
	}
}

unordered_set<string> Binder::ExtractTableNames(ClientContext &context, SQLStatement &statement, BindingMode mode) {
	auto binder = Binder::CreateBinder(context);
	binder->SetBindingMode(mode);
	binder->Bind(statement);
	return binder->GetTableNames();
}

void Binder::RethrowIfInvalidatesTransaction(const ErrorData &error) {
	if (Exception::InvalidatesTransaction(error.Type())) {
		error.Throw();
	}
}

unordered_set<string> Binder::ExtractCatalogLookups(ClientContext &context, const SQLStatement &statement,
                                                    optional_ptr<ErrorData> error) {
	if (!ExtractsCatalogLookups(statement)) {
		return {};
	}
	try {
		auto copy = statement.Copy();
		return ExtractTableNames(context, *copy, BindingMode::EXTRACT_CATALOG_LOOKUPS);
	} catch (std::exception &ex) {
		ErrorData caught(ex);
		RethrowIfInvalidatesTransaction(caught);
		if (error) {
			*error = std::move(caught);
		}
		return {};
	}
}

} // namespace duckdb
