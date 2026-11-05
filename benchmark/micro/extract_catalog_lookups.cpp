#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

//! The tables a remote catalog has loaded, and the latency of a request to the remote service
struct RemoteCatalogState : StorageExtensionInfo {
	int64_t latency_us = 0;
	set<string> loaded;
	set<string> deferred;

	void Request() const {
		std::this_thread::sleep_for(std::chrono::microseconds(latency_us));
	}
};

//! A catalog that loads each table it has not loaded with one request, unless the lookup can defer the load
class RemoteCatalog : public DuckCatalog {
public:
	RemoteCatalog(AttachedDatabase &db, RemoteCatalogState &state) : DuckCatalog(db), state(state) {
	}

private:
	CatalogEntryLookup TryLookupEntryInternal(CatalogTransaction transaction,
	                                          const EntryLookupInfo &lookup_info) override {
		auto name = lookup_info.GetEntryIdentifier().GetIdentifierName();
		if (lookup_info.GetCatalogType() == CatalogType::TABLE_ENTRY && !state.loaded.count(name)) {
			if (lookup_info.CanDeferLoad()) {
				state.deferred.insert(name);
				return {nullptr, nullptr, ErrorData()};
			}
			state.Request();
			state.loaded.insert(name);
		}
		auto &path = lookup_info.GetQualifiedName().Path();
		vector<Identifier> schema_path(path.begin(), path.end() - 1);
		auto schema_lookup = EntryLookupInfo::SchemaLookup(lookup_info, std::move(schema_path));
		auto schema = LookupSchema(transaction, schema_lookup, OnEntryNotFound::RETURN_NULL);
		if (!schema) {
			return {nullptr, nullptr, ErrorData()};
		}
		return {schema, schema->LookupEntry(transaction, lookup_info), ErrorData()};
	}

	RemoteCatalogState &state;
};

struct RemoteStorageExtension : StorageExtension {
	explicit RemoteStorageExtension(shared_ptr<RemoteCatalogState> state) {
		storage_info = std::move(state);
		attach = [](optional_ptr<StorageExtensionInfo> storage_info, ClientContext &, AttachedDatabase &db,
		            const string &, AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			info.path = IN_MEMORY_PATH;
			return make_uniq_base<Catalog, RemoteCatalog>(db, static_cast<RemoteCatalogState &>(*storage_info));
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

//! Loads the tables a statement needs together, with one request per round
struct LoadTogether : PlannerExtensionInfo {
	explicit LoadTogether(RemoteCatalogState &state) : state(state) {
	}
	RemoteCatalogState &state;

	static void PreBind(PlannerExtensionInput &input, SQLStatement &statement) {
		auto &state = static_cast<LoadTogether &>(*input.info).state;
		while (true) {
			state.deferred.clear();
			Binder::ExtractCatalogLookups(input.context, statement);
			if (state.deferred.empty()) {
				return;
			}
			state.Request();
			state.loaded.insert(state.deferred.begin(), state.deferred.end());
		}
	}
};

constexpr idx_t TABLE_COUNT = 8;
constexpr idx_t QUERY_COUNT = 20;

//! A database with TABLE_COUNT tables in a remote catalog, and optionally a pre-bind hook that loads them together
struct RemoteCatalogBenchmarkState : public DuckDBBenchmarkState {
	shared_ptr<RemoteCatalogState> state;
	duckdb::unique_ptr<DuckDB> remote_db;
	duckdb::unique_ptr<Connection> remote_conn;
	string query;

	RemoteCatalogBenchmarkState(int64_t latency_us, bool load_together, const string &catalog)
	    : DuckDBBenchmarkState(string()), state(make_shared_ptr<RemoteCatalogState>()) {
		DBConfig config;
		StorageExtension::Register(config, "remote", make_shared_ptr<RemoteStorageExtension>(state));
		remote_db = make_uniq<DuckDB>(nullptr, &config);
		remote_conn = make_uniq<Connection>(*remote_db);
		remote_conn->Query("ATTACH 'remote' AS remote (TYPE remote)");
		// a single thread keeps the query cheap next to the binding it measures
		remote_conn->Query("SET threads = 1");
		query = "SELECT count(*) FROM ";
		for (idx_t i = 0; i < TABLE_COUNT; i++) {
			auto table = catalog + ".t" + to_string(i);
			remote_conn->Query("CREATE TABLE " + table + " AS SELECT 1 AS i");
			query += (i == 0 ? "" : " JOIN ") + table + (i == 0 ? "" : " USING (i)");
		}
		state->latency_us = latency_us;
		if (load_together) {
			PlannerExtension extension;
			extension.planner_info = make_shared_ptr<LoadTogether>(*state);
			extension.pre_bind_function = LoadTogether::PreBind;
			PlannerExtension::Register(DBConfig::GetConfig(*remote_conn->context), std::move(extension));
		}
	}
};

void RunQueries(DuckDBBenchmarkState *state_p) {
	auto &bench = static_cast<RemoteCatalogBenchmarkState &>(*state_p);
	for (idx_t i = 0; i < QUERY_COUNT; i++) {
		// every query starts without any loaded tables, as in a new transaction
		bench.state->loaded.clear();
		bench.remote_conn->Query(bench.query);
	}
}

} // namespace

#define REMOTE_CATALOG_BENCHMARK(LATENCY_US, LOAD_TOGETHER, CATALOG, INFO)                                             \
	duckdb::unique_ptr<DuckDBBenchmarkState> CreateBenchmarkState() override {                                         \
		return make_uniq<RemoteCatalogBenchmarkState>(LATENCY_US, LOAD_TOGETHER, CATALOG);                             \
	}                                                                                                                  \
	void Load(DuckDBBenchmarkState *state) override {                                                                  \
	}                                                                                                                  \
	void RunBenchmark(DuckDBBenchmarkState *state) override {                                                          \
		RunQueries(state);                                                                                             \
	}                                                                                                                  \
	string VerifyResult(QueryResult *result) override {                                                                \
		return string();                                                                                               \
	}                                                                                                                  \
	string BenchmarkInfo() override {                                                                                  \
		return INFO;                                                                                                   \
	}

// 20 queries joining 8 tables of a remote catalog with 1ms per request: one request per table, or per round
DUCKDB_BENCHMARK(RemoteCatalogLoadEachTable, "[catalog]")
REMOTE_CATALOG_BENCHMARK(1000, false, "remote", "load each table with its own request")
FINISH_BENCHMARK(RemoteCatalogLoadEachTable)

DUCKDB_BENCHMARK(RemoteCatalogLoadTablesTogether, "[catalog]")
REMOTE_CATALOG_BENCHMARK(1000, true, "remote", "load the tables of each query together")
FINISH_BENCHMARK(RemoteCatalogLoadTablesTogether)

// the same queries over local tables, with and without the hook: the cost of the extra bind
DUCKDB_BENCHMARK(LocalCatalogWithoutExtraction, "[catalog]")
REMOTE_CATALOG_BENCHMARK(0, false, "memory", "bind each query once")
FINISH_BENCHMARK(LocalCatalogWithoutExtraction)

DUCKDB_BENCHMARK(LocalCatalogWithExtraction, "[catalog]")
REMOTE_CATALOG_BENCHMARK(0, true, "memory", "extract the catalog lookups of each query before binding it")
FINISH_BENCHMARK(LocalCatalogWithExtraction)
