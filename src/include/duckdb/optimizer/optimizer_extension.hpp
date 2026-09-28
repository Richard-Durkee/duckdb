//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/optimizer_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

namespace duckdb {
struct DBConfig;
class Optimizer;
class ClientContext;

//! The OptimizerExtensionInfo holds static information relevant to the optimizer extension
struct OptimizerExtensionInfo {
	virtual ~OptimizerExtensionInfo() {
	}
};

struct OptimizerExtensionInput {
	ClientContext &context;
	Optimizer &optimizer;
	optional_ptr<OptimizerExtensionInfo> info;
};

//! Whether an optimizer step callback is invoked before or after the step
enum class OptimizerStepPhase : uint8_t { BEFORE, AFTER };

struct OptimizerStepInput {
	ClientContext &context;
	//! The optimizer step that is about to run, or just ran
	OptimizerType type;
	OptimizerStepPhase phase;
	//! The plan before or after the step
	const LogicalOperator &plan;
	optional_ptr<OptimizerExtensionInfo> info;
};

typedef void (*optimize_function_t)(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);
typedef void (*pre_optimize_function_t)(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);
typedef void (*optimizer_step_function_t)(OptimizerStepInput &input);

class OptimizerExtension {
public:
	//! The optimize function of the optimizer extension.
	//! Takes a logical query plan as an input, which it can modify in place
	//! This runs, after the DuckDB optimizers have run
	optimize_function_t optimize_function = nullptr;
	//! The pre-optimize function of the optimizer extension.
	//! Takes a logical query plan as an input, which it can modify in place
	//! This runs, before the DuckDB optimizers have run
	pre_optimize_function_t pre_optimize_function = nullptr;
	//! The optimizer step function of the optimizer extension.
	//! Invoked before and after every optimizer step that runs, with read-only access to the plan
	optimizer_step_function_t optimizer_step_function = nullptr;

	//! Additional optimizer info passed to the optimize functions
	shared_ptr<OptimizerExtensionInfo> optimizer_info;

	static void Register(DBConfig &config, OptimizerExtension extension);
	static ExtensionCallbackIteratorHelper<OptimizerExtension> Iterate(ClientContext &context) {
		return ExtensionCallbackManager::Get(context).OptimizerExtensions();
	}
};

} // namespace duckdb
