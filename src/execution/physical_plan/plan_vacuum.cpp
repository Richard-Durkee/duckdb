#include "duckdb/execution/operator/helper/physical_vacuum.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_vacuum.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"

namespace duckdb {

static PhysicalOperator &PlanPhysicalVacuum(PhysicalPlanGenerator &planner, LogicalVacuum &op) {
	auto &vacuum = planner.Make<PhysicalVacuum>(unique_ptr_cast<ParseInfo, VacuumInfo>(std::move(op.info)), op.table,
	                                            std::move(op.column_id_map), op.estimated_cardinality);
	if (!op.children.empty()) {
		auto &plan = planner.CreatePlan(*op.children[0]);
		vacuum.children.push_back(plan);
	}
	return vacuum;
}

PhysicalOperator &DuckCatalog::PlanVacuum(ClientContext &context, PhysicalPlanGenerator &planner, LogicalVacuum &op) {
	return PlanPhysicalVacuum(planner, op);
}

PhysicalOperator &Catalog::PlanVacuum(ClientContext &context, PhysicalPlanGenerator &planner, LogicalVacuum &op) {
	throw NotImplementedException("%s is not supported for tables of database type \"%s\"",
	                              op.GetInfo().options.vacuum ? "VACUUM" : "ANALYZE", GetCatalogType());
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalVacuum &op) {
	if (op.HasTable()) {
		auto &table = op.GetTable();
		dependencies.AddDependency(table);
		return table.catalog.PlanVacuum(context, *this, op);
	}
	return PlanPhysicalVacuum(*this, op);
}

} // namespace duckdb
