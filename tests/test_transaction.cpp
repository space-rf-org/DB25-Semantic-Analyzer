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

// SAVEPOINT establishes a nested rollback point; the block stays open.
void test_savepoint_establish() {
    parser::Parser p;
    TransactionManager mgr;
    CHECK(apply(p, mgr, "BEGIN").status == TxnResult::Status::Ok);
    CHECK(apply(p, mgr, "SAVEPOINT sp1").status == TxnResult::Status::Ok);
    CHECK(apply(p, mgr, "SAVEPOINT sp2").status == TxnResult::Status::Ok);
    CHECK(mgr.state() == TxnState::Active);
    CHECK(mgr.savepoint_depth() == 2);
    CHECK(mgr.has_savepoint("sp1") && mgr.has_savepoint("sp2"));
    CHECK(!mgr.has_savepoint("nope"));
}

// ROLLBACK TO keeps the named savepoint but discards those established after it;
// the block stays open. RELEASE discards the named one and any after it.
void test_rollback_to_and_release() {
    parser::Parser p;
    {
        TransactionManager mgr;
        CHECK(apply(p, mgr, "BEGIN").status == TxnResult::Status::Ok);
        apply(p, mgr, "SAVEPOINT a");
        apply(p, mgr, "SAVEPOINT b");
        apply(p, mgr, "SAVEPOINT c");
        CHECK(mgr.savepoint_depth() == 3);
        // Roll back to 'a': b and c are destroyed, 'a' remains.
        const TxnResult r = apply(p, mgr, "ROLLBACK TO SAVEPOINT a");
        CHECK(r.status == TxnResult::Status::Ok);
        CHECK(mgr.state() == TxnState::Active);
        CHECK(mgr.savepoint_depth() == 1);
        CHECK(mgr.has_savepoint("a") && !mgr.has_savepoint("b") && !mgr.has_savepoint("c"));
        // Can roll back to the retained savepoint again.
        CHECK(apply(p, mgr, "ROLLBACK TO SAVEPOINT a").status == TxnResult::Status::Ok);
        CHECK(mgr.savepoint_depth() == 1);
    }
    {
        TransactionManager mgr;
        apply(p, mgr, "BEGIN");
        apply(p, mgr, "SAVEPOINT a");
        apply(p, mgr, "SAVEPOINT b");
        // RELEASE a: both a and the later b are destroyed.
        const TxnResult r = apply(p, mgr, "RELEASE SAVEPOINT a");
        CHECK(r.status == TxnResult::Status::Ok);
        CHECK(mgr.savepoint_depth() == 0);
        CHECK(!mgr.has_savepoint("a") && !mgr.has_savepoint("b"));
    }
}

// COMMIT / full ROLLBACK end the block and discard every savepoint.
void test_close_clears_savepoints() {
    parser::Parser p;
    TransactionManager mgr;
    apply(p, mgr, "BEGIN");
    apply(p, mgr, "SAVEPOINT a");
    apply(p, mgr, "SAVEPOINT b");
    CHECK(mgr.savepoint_depth() == 2);
    CHECK(apply(p, mgr, "COMMIT").status == TxnResult::Status::Ok);
    CHECK(mgr.state() == TxnState::Idle);
    CHECK(mgr.savepoint_depth() == 0);
}

// Savepoint misuse is a genuine ERROR (PostgreSQL), and never changes state.
void test_savepoint_errors() {
    parser::Parser p;
    // SAVEPOINT / RELEASE / ROLLBACK TO outside a transaction block.
    {
        TransactionManager mgr;
        CHECK(apply(p, mgr, "SAVEPOINT a").status == TxnResult::Status::Error);
        CHECK(mgr.state() == TxnState::Idle && mgr.savepoint_depth() == 0);
        CHECK(apply(p, mgr, "RELEASE SAVEPOINT a").status == TxnResult::Status::Error);
        CHECK(apply(p, mgr, "ROLLBACK TO SAVEPOINT a").status == TxnResult::Status::Error);
    }
    // Naming a savepoint that does not exist inside a block.
    {
        TransactionManager mgr;
        apply(p, mgr, "BEGIN");
        CHECK(apply(p, mgr, "ROLLBACK TO SAVEPOINT ghost").status == TxnResult::Status::Error);
        CHECK(apply(p, mgr, "RELEASE SAVEPOINT ghost").status == TxnResult::Status::Error);
        CHECK(mgr.state() == TxnState::Active);  // block still open, untouched
    }
}

// Duplicate savepoint names: the most recent shadows; RELEASE/ROLLBACK TO act on
// the newest, and after it is gone the older one is visible again.
void test_duplicate_savepoint_names() {
    parser::Parser p;
    TransactionManager mgr;
    apply(p, mgr, "BEGIN");
    apply(p, mgr, "SAVEPOINT s");   // older
    apply(p, mgr, "SAVEPOINT s");   // newer, shadows
    CHECK(mgr.savepoint_depth() == 2);
    // RELEASE removes the newest 's' (and nothing after it); the older remains.
    CHECK(apply(p, mgr, "RELEASE SAVEPOINT s").status == TxnResult::Status::Ok);
    CHECK(mgr.savepoint_depth() == 1);
    CHECK(mgr.has_savepoint("s"));
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
    test_savepoint_establish();
    test_rollback_to_and_release();
    test_close_clears_savepoints();
    test_savepoint_errors();
    test_duplicate_savepoint_names();
    test_non_transaction_statement();

    std::printf("transaction: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
