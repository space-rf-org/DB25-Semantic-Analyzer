// DB25 Semantic Analyzer - transaction-control state-machine tests.
//
// Drives the TransactionManager with parsed BEGIN / COMMIT / ROLLBACK
// statements and asserts the resulting session state and outcome status.

#include "db25/parser/parser.hpp"
#include "db25/semantic/transaction.hpp"

#include <cstdio>
#include <string>

using namespace db25;
using namespace db25::semantic;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL: %s (%s:%d)\n", expr, file, line);
    }
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

const ast::ASTNode* parse(parser::Parser& p, const std::string& sql) {
    auto r = p.parse(sql);
    return r.has_value() ? r.value() : nullptr;
}

TxnResult apply(parser::Parser& p, TransactionManager& mgr, const std::string& sql) {
    const ast::ASTNode* ast = parse(p, sql);
    if (ast == nullptr) return {TxnResult::Status::Error, "parse failed", mgr.state()};
    return mgr.apply(ast);
}

// The happy path: BEGIN opens a block, COMMIT / ROLLBACK closes it.
void test_basic_lifecycle() {
    parser::Parser p;

    {
        TransactionManager mgr;
        CHECK(!mgr.in_transaction());
        const TxnResult b = apply(p, mgr, "BEGIN");
        CHECK(b.status == TxnResult::Status::Ok);
        CHECK(mgr.in_transaction() && mgr.state() == TxnState::Active);

        const TxnResult c = apply(p, mgr, "COMMIT");
        CHECK(c.status == TxnResult::Status::Ok);
        CHECK(!mgr.in_transaction() && mgr.state() == TxnState::Idle);
    }
    {
        TransactionManager mgr;
        CHECK(apply(p, mgr, "START TRANSACTION").status == TxnResult::Status::Ok);
        CHECK(mgr.in_transaction());
        const TxnResult r = apply(p, mgr, "ROLLBACK");
        CHECK(r.status == TxnResult::Status::Ok);
        CHECK(!mgr.in_transaction());
    }
}

// Misuse is a legal no-op WARNING (PostgreSQL semantics), and crucially the
// session state is left unchanged in every case.
void test_misuse_is_warning_not_error() {
    parser::Parser p;

    // Nested BEGIN: still one open block afterwards.
    {
        TransactionManager mgr;
        CHECK(apply(p, mgr, "BEGIN").status == TxnResult::Status::Ok);
        const TxnResult again = apply(p, mgr, "BEGIN");
        CHECK(again.status == TxnResult::Status::Warning);
        CHECK(again.ok());  // a warning is not a rejection
        CHECK(mgr.state() == TxnState::Active);  // unchanged
    }
    // COMMIT with no open block: no-op warning, stays Idle.
    {
        TransactionManager mgr;
        const TxnResult c = apply(p, mgr, "COMMIT");
        CHECK(c.status == TxnResult::Status::Warning);
        CHECK(mgr.state() == TxnState::Idle);
    }
    // ROLLBACK with no open block: no-op warning, stays Idle.
    {
        TransactionManager mgr;
        const TxnResult r = apply(p, mgr, "ROLLBACK");
        CHECK(r.status == TxnResult::Status::Warning);
        CHECK(mgr.state() == TxnState::Idle);
    }
}

// A double COMMIT: the second is a warning, not a crash or a spurious error.
void test_double_close() {
    parser::Parser p;
    TransactionManager mgr;
    CHECK(apply(p, mgr, "BEGIN").status == TxnResult::Status::Ok);
    CHECK(apply(p, mgr, "COMMIT").status == TxnResult::Status::Ok);
    CHECK(apply(p, mgr, "COMMIT").status == TxnResult::Status::Warning);
    CHECK(mgr.state() == TxnState::Idle);
}

// ROLLBACK TO SAVEPOINT must be refused (not silently downgraded to a full
// rollback) until savepoints land, and it must NOT change the session state.
void test_rollback_to_savepoint_refused() {
    parser::Parser p;
    TransactionManager mgr;
    CHECK(apply(p, mgr, "BEGIN").status == TxnResult::Status::Ok);
    const TxnResult r = apply(p, mgr, "ROLLBACK TO SAVEPOINT sp1");
    CHECK(r.status == TxnResult::Status::Error);
    CHECK(!r.ok());
    CHECK(mgr.state() == TxnState::Active);  // transaction is still open
}

// A non-transaction statement handed to the manager is an error, state intact.
void test_non_transaction_statement() {
    parser::Parser p;
    TransactionManager mgr;
    const TxnResult r = apply(p, mgr, "SELECT 1");
    CHECK(r.status == TxnResult::Status::Error);
    CHECK(mgr.state() == TxnState::Idle);
}

}  // namespace

int main() {
    test_basic_lifecycle();
    test_misuse_is_warning_not_error();
    test_double_close();
    test_rollback_to_savepoint_refused();
    test_non_transaction_statement();

    std::printf("transaction: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
