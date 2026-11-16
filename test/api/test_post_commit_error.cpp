#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

namespace {

atomic<idx_t> errors_after_commit {0};

class PostCommitErrorTransactionManager : public DuckTransactionManager {
public:
	explicit PostCommitErrorTransactionManager(AttachedDatabase &db) : DuckTransactionManager(db) {
	}

	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override {
		auto error = DuckTransactionManager::CommitTransaction(context, transaction);
		for (idx_t i = 0; !error.HasError() && i < errors_after_commit; i++) {
			context.transaction.AddPostCommitError(ErrorData(IOException("work %llu after the commit failed", i + 1)));
		}
		return error;
	}
};

struct PostCommitErrorStorageExtension : StorageExtension {
	PostCommitErrorStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq_base<Catalog, DuckCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<PostCommitErrorTransactionManager>(db);
		};
	}
};

} // namespace

TEST_CASE("Test an error reported after a successful commit", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "post_commit_error_test", make_shared_ptr<PostCommitErrorStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);
	Connection other(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS reporting (TYPE post_commit_error_test)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE reporting.t (i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE tmp (i INTEGER)"));

	errors_after_commit = 1;
	// temp is used first, so reporting commits first and temp after it
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO tmp VALUES (1)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO reporting.t VALUES (1)"));
	auto result = con.Query("COMMIT");
	errors_after_commit = 0;
	REQUIRE(result->HasError());
	REQUIRE(result->GetErrorType() == ExceptionType::IO);
	REQUIRE(StringUtil::Contains(result->GetError(), "work 1 after the commit failed"));

	// both databases committed, another connection sees the row, and no transaction is left open
	result = other.Query("SELECT count(*) FROM reporting.t");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	result = con.Query("SELECT count(*) FROM tmp");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(!con.context->transaction.HasActiveTransaction());
	REQUIRE_NO_FAIL(con.Query("INSERT INTO reporting.t VALUES (2)"));
	result = other.Query("SELECT count(*) FROM reporting.t");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
}

TEST_CASE("Test that the first error reported after a commit is raised", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "post_commit_error_test", make_shared_ptr<PostCommitErrorStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS reporting (TYPE post_commit_error_test)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE reporting.t (i INTEGER)"));

	errors_after_commit = 2;
	auto result = con.Query("INSERT INTO reporting.t VALUES (1)");
	errors_after_commit = 0;
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "work 1 after the commit failed"));
	REQUIRE(!StringUtil::Contains(result->GetError(), "work 2 after the commit failed"));

	result = con.Query("SELECT count(*) FROM reporting.t");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	// the next commit starts without the errors of the previous one
	REQUIRE_NO_FAIL(con.Query("INSERT INTO reporting.t VALUES (2)"));
}
