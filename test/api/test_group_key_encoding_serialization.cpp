#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/planner/logical_operator.hpp"

#include <thread>

using namespace duckdb;

static const char *GROUP_KEY_QUERY = "SELECT a, sum(v) FROM fact, dim WHERE fact.k = dim.k GROUP BY a ORDER BY ALL";

static void CreateGroupKeyTables(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE dim AS SELECT i AS k, 'category_' || (i % 5)::VARCHAR AS a FROM "
	                          "range(40) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE fact AS SELECT i % 40 AS k, i AS v FROM range(4000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("SET debug_force_group_key_encoding = true"));
}

static unique_ptr<LogicalOperator> DeserializePlan(MemoryStream &stream, Connection &con) {
	stream.Rewind();
	bound_parameter_map_t parameters;
	return BinaryDeserializer::Deserialize<LogicalOperator>(stream, *con.context, parameters);
}

TEST_CASE("Test serializing plans with dictionary encoded group keys", "[api]") {
	DuckDB db(nullptr);
	Connection con(db), other(db);
	CreateGroupKeyTables(con);

	con.BeginTransaction();
	auto plan = con.ExtractPlan(GROUP_KEY_QUERY);
	REQUIRE(plan->ToString().find("__internal_group_key_intern") != string::npos);

	MemoryStream stream(Allocator::Get(*con.context));
	SerializationOptions options;
	options.storage_compatibility = StorageCompatibility::Latest();
	options.process_local = true;
	BinarySerializer::Serialize(*plan, stream, options);

	// the connection of the plan reads the plan again, onto the dictionaries of the original
	auto copy = DeserializePlan(stream, con);
	REQUIRE(copy->ToString().find("__internal_group_key_resolve") != string::npos);
	REQUIRE_NOTHROW(plan->Copy(*con.context));

	// the plan is not written for any reader but its connection, unless the writer says so
	MemoryStream foreign_stream(Allocator::Get(*con.context));
	REQUIRE_THROWS_AS(BinarySerializer::Serialize(*plan, foreign_stream, SerializationOptions()),
	                  NotImplementedException);

	// another connection of the same database has no such dictionaries
	other.BeginTransaction();
	REQUIRE_THROWS_AS(DeserializePlan(stream, other), SerializationException);
	REQUIRE_THROWS_WITH(DeserializePlan(stream, other), Catch::Contains("does not exist in this connection"));
	REQUIRE_THROWS_AS(plan->Copy(*other.context), SerializationException);
	other.Rollback();

	// neither has the connection of the plan, once the plans that own the dictionaries are gone
	copy.reset();
	plan.reset();
	REQUIRE_THROWS_AS(DeserializePlan(stream, con), SerializationException);
	con.Rollback();
}

TEST_CASE("Test serializing plans with dictionary encoded group keys across databases", "[api]") {
	DuckDB db(nullptr), other_db(nullptr);
	Connection con(db), other(other_db);
	CreateGroupKeyTables(con);
	CreateGroupKeyTables(other);

	con.BeginTransaction();
	auto plan = con.ExtractPlan(GROUP_KEY_QUERY);
	other.BeginTransaction();
	// the plan of another database names a dictionary that this database does not have: it must not find another one
	auto other_plan = other.ExtractPlan(GROUP_KEY_QUERY);
	MemoryStream stream(Allocator::Get(*con.context));
	SerializationOptions options;
	options.storage_compatibility = StorageCompatibility::Latest();
	options.process_local = true;
	BinarySerializer::Serialize(*plan, stream, options);
	REQUIRE_THROWS_AS(DeserializePlan(stream, other), SerializationException);
	other.Rollback();
	con.Rollback();
}

TEST_CASE("Test dictionary encoded group keys on many connections that verify the serialization", "[api]") {
	DuckDB db(nullptr);
	Connection setup(db);
	CreateGroupKeyTables(setup);
	REQUIRE_NO_FAIL(setup.Query("SET disabled_optimizers='group_key_encoding'"));
	auto expected_result = setup.Query(GROUP_KEY_QUERY);
	REQUIRE_NO_FAIL(*expected_result);
	const auto expected = expected_result->ToString();
	REQUIRE_NO_FAIL(setup.Query("RESET disabled_optimizers"));

	static constexpr idx_t CONNECTION_COUNT = 200;
	static constexpr idx_t ROUND_COUNT = 5;
	vector<idx_t> failures(CONNECTION_COUNT, 0);
	vector<std::thread> threads;
	for (idx_t t = 0; t < CONNECTION_COUNT; t++) {
		threads.emplace_back([&, t]() {
			try {
				Connection con(db);
				con.Query("SET debug_force_group_key_encoding = true");
				con.Query("SET debug_verify_serializer = true");
				auto prepared = con.Prepare(GROUP_KEY_QUERY);
				for (idx_t round = 0; round < ROUND_COUNT; round++) {
					unique_ptr<QueryResult> results[2];
					results[0] = con.Query(GROUP_KEY_QUERY);
					results[1] = prepared->Execute();
					for (auto &result : results) {
						if (result->HasError() || result->ToString() != expected) {
							failures[t]++;
						}
					}
				}
			} catch (std::exception &) {
				failures[t]++;
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	for (idx_t t = 0; t < CONNECTION_COUNT; t++) {
		REQUIRE(failures[t] == 0);
	}
}
