#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

namespace {

//! Runs Binder::ExtractCatalogLookups before each statement is bound, and keeps the names of the tables it found
struct ExtractTableNames : PlannerExtensionInfo {
	set<string> table_names;
	ErrorData extract_error;

	static void PreBind(PlannerExtensionInput &input, SQLStatement &statement) {
		auto &info = static_cast<ExtractTableNames &>(*input.info);
		info.extract_error = ErrorData();
		auto names = Binder::ExtractCatalogLookups(input.context, statement, &info.extract_error);
		info.table_names = set<string>(names.begin(), names.end());
	}
};

//! What the test catalog loaded, was asked to defer, and had to load one entry at a time
struct DeferringState : StorageExtensionInfo {
	bool tracking = false;
	set<string> loaded;
	set<string> deferred;
	idx_t single_loads = 0;
	vector<string> at_values;
};

//! A catalog that defers loading the tables it has not loaded when the lookup allows it
class TestDeferringCatalog : public DuckCatalog {
public:
	TestDeferringCatalog(AttachedDatabase &db, DeferringState &state) : DuckCatalog(db), state(state) {
	}

	bool SupportsTimeTravel() const override {
		return true;
	}

private:
	CatalogEntryLookup TryLookupEntryInternal(CatalogTransaction transaction,
	                                          const EntryLookupInfo &lookup_info) override {
		auto name = lookup_info.GetEntryIdentifier().GetIdentifierName();
		if (state.tracking && lookup_info.GetCatalogType() == CatalogType::TABLE_ENTRY && !state.loaded.count(name)) {
			if (lookup_info.CanDeferLoad()) {
				state.deferred.insert(name);
				auto at_clause = lookup_info.GetAtClause();
				if (at_clause) {
					state.at_values.push_back(at_clause->GetValue().ToString());
				}
				return {nullptr, nullptr, ErrorData()};
			}
			state.single_loads++;
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

	DeferringState &state;
};

struct TestDeferringStorageExtension : StorageExtension {
	TestDeferringStorageExtension() {
		storage_info = make_shared_ptr<DeferringState>();
		attach = [](optional_ptr<StorageExtensionInfo> storage_info, ClientContext &, AttachedDatabase &db,
		            const string &, AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			info.path = IN_MEMORY_PATH;
			return make_uniq_base<Catalog, TestDeferringCatalog>(db, static_cast<DeferringState &>(*storage_info));
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

//! Loads the tables of each statement together, round by round, the way a catalog extension would
struct ExtractThenLoad : PlannerExtensionInfo {
	explicit ExtractThenLoad(DeferringState &state) : state(state) {
	}
	DeferringState &state;
	//! The tables loaded together in each round, for the last statement
	vector<set<string>> rounds;

	static void PreBind(PlannerExtensionInput &input, SQLStatement &statement) {
		auto &info = static_cast<ExtractThenLoad &>(*input.info);
		auto &state = info.state;
		info.rounds.clear();
		state.loaded.clear();
		state.single_loads = 0;
		for (idx_t round = 0; round < 4; round++) {
			state.deferred.clear();
			Binder::ExtractCatalogLookups(input.context, statement);
			if (state.deferred.empty()) {
				break;
			}
			state.loaded.insert(state.deferred.begin(), state.deferred.end());
			info.rounds.push_back(state.deferred);
		}
	}
};

} // namespace

TEST_CASE("Bind a statement to find the tables it looks up", "[api][binder]") {
	DuckDB db(nullptr);
	Connection con(db);
	for (auto table : {"a", "b", "c"}) {
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("CREATE TABLE %s AS SELECT 1 AS i, 'x' AS j", table)));
	}
	REQUIRE_NO_FAIL(con.Query("CREATE VIEW v AS SELECT j, i + 1 AS k FROM c"));
	REQUIRE_NO_FAIL(con.Query("CREATE VIEW w AS SELECT * FROM v, a"));

	auto info = make_shared_ptr<ExtractTableNames>();
	PlannerExtension extension;
	extension.planner_info = info;
	extension.pre_bind_function = ExtractTableNames::PreBind;
	PlannerExtension::Register(DBConfig::GetConfig(*con.context), std::move(extension));

	auto extracts = [&](const string &sql, set<string> expected) {
		INFO(sql);
		REQUIRE_NO_FAIL(con.Query(sql));
		INFO(info->extract_error.Message());
		REQUIRE(!info->extract_error.HasError());
		REQUIRE(info->table_names == expected);
	};
	extracts("SELECT * FROM a JOIN b USING (i)", {"a", "b"});
	extracts("SELECT * FROM a NATURAL JOIN b", {"a", "b"});
	// a CTE is not a catalog lookup, even when it shadows a table
	extracts("WITH a AS (SELECT 42 AS i) SELECT * FROM a, b", {"b"});
	// views are bound, so the tables they read are found too
	extracts("SELECT * FROM w", {"a", "c"});
	extracts("SELECT {'x': a.i}.x FROM a WHERE list_transform([1], lambda x: x + a.i)[1] > 0", {"a"});
	extracts("SELECT * FROM a WHERE i IN (SELECT i FROM b)", {"a", "b"});
	// the sides of a set operation, and the aliases of a subquery, can count different columns than the dummy scans
	extracts("SELECT * FROM (SELECT i, j FROM a UNION ALL SELECT * FROM b) u, c", {"a", "b", "c"});
	extracts("WITH RECURSIVE r(n, m) AS (SELECT * FROM a UNION ALL SELECT n + 1, m FROM r WHERE n < 3) "
	         "SELECT * FROM r, c",
	         {"a", "c"});
	extracts("SELECT * FROM (SELECT * FROM a) s(x, y), b", {"a", "b"});
	extracts("SELECT i, j FROM a UNION SELECT * FROM b", {"a", "b"});
	extracts("SELECT i, j FROM a UNION ALL SELECT * FROM b LIMIT 1", {"a", "b"});
	extracts("WITH RECURSIVE r AS (SELECT i, j FROM a UNION ALL SELECT b.* FROM b, r WHERE false) SELECT * FROM r, c",
	         {"a", "b", "c"});
	extracts("EXPLAIN SELECT * FROM a, b", {"a", "b"});
	// the arguments of a table function are bound, but the function itself is not, so the file is not read
	extracts("SELECT * FROM generate_series(1, (SELECT max(i) FROM b)), a", {"a", "b"});
	REQUIRE_FAIL(con.Query("SELECT * FROM read_csv('does_not_exist.csv'), a"));
	REQUIRE(!info->extract_error.HasError());
	REQUIRE(info->table_names == set<string> {"a"});
	// functions that are rewritten to a query are bound
	extracts("SELECT * FROM query_table('b')", {"b"});
	extracts("SELECT * FROM query('SELECT * FROM b')", {"b"});
	// COPY binds its table or query, but not its copy function
	REQUIRE_FAIL(con.Query("COPY a FROM 'does_not_exist.csv'"));
	REQUIRE(!info->extract_error.HasError());
	REQUIRE(info->table_names == set<string> {"a"});
	extracts("COPY (SELECT * FROM a, b) TO '" + TestCreatePath("extract_catalog_lookups.csv") + "'", {"a", "b"});
	// statements built with the relational API
	REQUIRE_NO_FAIL(con.Table("a")->Filter("i > 0")->Execute());
	REQUIRE(info->table_names == set<string> {"a"});

	// DML statements: the tables they read and write, and the subqueries in their expressions
	extracts("INSERT INTO a SELECT * FROM b WHERE i IN (SELECT i FROM c)", {"a", "b", "c"});
	extracts("UPDATE a SET j = (SELECT max(j) FROM c) FROM b WHERE a.i = b.i", {"a", "b", "c"});
	extracts("DELETE FROM a USING c WHERE a.i IN (SELECT i FROM b)", {"a", "b", "c"});
	extracts("MERGE INTO a USING b ON a.i = b.i WHEN MATCHED AND a.i IN (SELECT i FROM c) THEN UPDATE SET j = b.j",
	         {"a", "b", "c"});
	extracts("CREATE VIEW x(p, q) AS SELECT * FROM a, b", {"a", "b"});
	extracts("CREATE TABLE y AS SELECT * FROM a, b", {"y", "a", "b"});
	extracts("INSERT INTO a VALUES ((SELECT max(i) FROM b), 'x')", {"a", "b"});
	// a CTE of the same name does not replace the INSERT target
	extracts("WITH b AS (SELECT 2 AS i, 'y' AS j) INSERT INTO b SELECT * FROM b", {"b"});
	extracts("SELECT i, j FROM a UNION ALL SELECT * FROM b ORDER BY (SELECT count(*) FROM c) + i", {"a", "b", "c"});

	// prepared parameters do not stop the binding
	auto prepared = con.Prepare("SELECT * FROM a WHERE i = ? AND i IN (SELECT i FROM b)");
	REQUIRE(!prepared->HasError());
	REQUIRE(!info->extract_error.HasError());
	REQUIRE(info->table_names == set<string> {"a", "b"});

	// a table that does not exist is found too, and binding a view for its references keeps the view's types
	REQUIRE_FAIL(con.Query("SELECT * FROM missing, v"));
	REQUIRE(!info->extract_error.HasError());
	REQUIRE(info->table_names == set<string> {"missing", "c"});
	auto result = con.Query("SELECT column_name, data_type FROM duckdb_columns() WHERE table_name = 'v' ORDER BY ALL");
	REQUIRE(CHECK_COLUMN(result, 0, {"j", "k"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"VARCHAR", "INTEGER"}));
}

TEST_CASE("A catalog loads the tables of a statement together before the statement is bound", "[api][binder]") {
	DBConfig config;
	auto storage = make_shared_ptr<TestDeferringStorageExtension>();
	auto &state = static_cast<DeferringState &>(*storage->storage_info);
	StorageExtension::Register(config, "test_deferring", storage);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH 'remote' AS remote (TYPE test_deferring)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE remote.t1 AS SELECT 1 AS i"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE remote.t2 AS SELECT 1 AS i"));
	REQUIRE_NO_FAIL(con.Query("CREATE VIEW remote.v AS SELECT * FROM t2"));
	REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE seq"));
	REQUIRE_NO_FAIL(con.Query("PREPARE p AS SELECT * FROM remote.t1 WHERE i = $1"));

	auto info = make_shared_ptr<ExtractThenLoad>(state);
	PlannerExtension extension;
	extension.planner_info = info;
	extension.pre_bind_function = ExtractThenLoad::PreBind;
	PlannerExtension::Register(DBConfig::GetConfig(*con.context), std::move(extension));
	state.tracking = true;
	using rounds_t = vector<set<string>>;

	// both tables are loaded in one round, so the bind loads none on its own
	auto result = con.Query("SELECT * FROM remote.t1 JOIN remote.t2 USING (i)");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(info->rounds == rounds_t {{"t1", "t2"}});
	REQUIRE(state.single_loads == 0);

	// a view is "not found" until it is loaded: the tables it reads are loaded in the next round
	result = con.Query("SELECT * FROM remote.v, remote.t1");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(info->rounds == rounds_t {{"t1", "v"}, {"t2"}});
	REQUIRE(state.single_loads == 0);

	// the tables of a DML statement are loaded together too
	REQUIRE_NO_FAIL(con.Query("UPDATE remote.t1 SET i = 1 WHERE i IN (SELECT i FROM remote.t2)"));
	REQUIRE(info->rounds == rounds_t {{"t1", "t2"}});
	REQUIRE(state.single_loads == 0);

	// a CTE is not looked up
	REQUIRE_NO_FAIL(con.Query("WITH t1 AS (SELECT 1) SELECT * FROM t1"));
	REQUIRE(info->rounds.empty());

	// when the extraction stops early, the bind loads the remaining tables on its own
	result = con.Query("SELECT t1.i FROM remote.t1 ASOF JOIN remote.t2 ON t1.i >= t2.i, remote.v");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(info->rounds == rounds_t {{"t1", "t2"}});
	REQUIRE(state.single_loads == 1);

	// a constant AT clause is evaluated for the lookup, any other leaves the load to the bind
	state.at_values.clear();
	con.Query("SELECT * FROM remote.t1 AT (VERSION => 1 + 1)");
	REQUIRE(state.at_values == vector<string> {"2"});
	con.Prepare("SELECT * FROM remote.t1 AT (VERSION => ?)");
	REQUIRE(state.at_values == vector<string> {"2"});
	REQUIRE(info->rounds.empty());

	// EXECUTE is not bound for its lookups, also within EXPLAIN: binding it evaluates its parameters
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("EXECUTE p(nextval('seq'))"));
	REQUIRE_NO_FAIL(con.Query("EXPLAIN EXECUTE p(nextval('seq'))"));
	result = con.Query("SELECT currval('seq')");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));

	// creating a view looks up its tables in the view's catalog
	REQUIRE_NO_FAIL(con.Query("CREATE VIEW remote.w AS SELECT * FROM t1, remote.t2"));
	REQUIRE(info->rounds == rounds_t {{"t1", "t2"}});
	REQUIRE(state.single_loads == 0);
}
