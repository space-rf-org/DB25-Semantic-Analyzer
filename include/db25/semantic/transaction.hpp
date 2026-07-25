// DB25 Semantic Analyzer - Transaction control
//
// Transaction-control statements (BEGIN / COMMIT / ROLLBACK) are session
// control-flow, not relational algebra, so - like DDL (Option C) - they do NOT
// become logical-plan nodes. They drive a session state machine instead.
//
// This layer is deliberately storage-independent: no storage or execution
// engine exists yet, so the manager models only the *control contract* - when a
// given statement is legal given the session's current transaction state. It is
// the single place that decides "you are already in a transaction" or "there is
// no transaction to commit", so that misuse is caught here, consistently,
// before it ever reaches the future executor. Durability, isolation, and the
// actual work a COMMIT flushes are layered on later; the state machine they hang
// off is defined here.
//
// Semantics follow PostgreSQL, the well-understood reference: a nested BEGIN and
// a COMMIT/ROLLBACK with no open transaction are WARNINGS (the statement is a
// no-op), not errors, so a script does not abort on them.

#pragma once

#include "db25/ast/ast_node.hpp"

#include <string>

namespace db25::semantic {

// A session's transaction state. Savepoints (a stack within Active) are a
// separate, later change; this first slice models the top-level block only.
enum class TxnState : std::uint8_t {
    Idle,    // no transaction block open (autocommit)
    Active,  // inside an explicit BEGIN ... block
};

// The outcome of applying one transaction-control statement.
struct TxnResult {
    enum class Status : std::uint8_t { Ok, Warning, Error };

    Status status = Status::Ok;
    std::string message;             // empty for a clean Ok
    TxnState state = TxnState::Idle;  // the session state AFTER applying

    // A Warning is a legal no-op (e.g. a redundant COMMIT); only an Error means
    // the statement was rejected. Callers that treat warnings as advisory can
    // gate on ok().
    [[nodiscard]] bool ok() const noexcept { return status != Status::Error; }
    [[nodiscard]] bool is_warning() const noexcept { return status == Status::Warning; }
};

class TransactionManager {
public:
    [[nodiscard]] TxnState state() const noexcept { return state_; }
    [[nodiscard]] bool in_transaction() const noexcept { return state_ == TxnState::Active; }

    // Validate and apply a transaction-control statement, advancing the session
    // state and returning the outcome. Accepts the statement node itself or any
    // ancestor that wraps exactly one transaction-control statement. A node that
    // is not a transaction-control statement is an Error (the state is unchanged).
    //
    // Handled: BEGIN / START TRANSACTION, COMMIT, ROLLBACK. A ROLLBACK TO
    // SAVEPOINT (a ROLLBACK carrying a savepoint name) is reported as an Error
    // rather than silently performing a full rollback - savepoints are a later
    // change. SAVEPOINT / RELEASE are likewise not yet routed here.
    [[nodiscard]] TxnResult apply(const ast::ASTNode* stmt);

private:
    TxnState state_ = TxnState::Idle;
};

}  // namespace db25::semantic
