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
// no-op), not errors, so a script does not abort on them. Savepoint misuse (a
// SAVEPOINT / RELEASE / ROLLBACK TO outside a block, or naming a savepoint that
// does not exist) is a genuine ERROR, matching PostgreSQL.

#pragma once

#include "db25/ast/ast_node.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace db25::semantic {

// A session's transaction state. Savepoints form a stack within Active (see
// TransactionManager); this enum tracks only whether a block is open.
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

    // Number of live savepoints in the current transaction (0 when Idle).
    [[nodiscard]] std::size_t savepoint_depth() const noexcept { return savepoints_.size(); }

    // Whether a savepoint of this name is currently live. Duplicate names are
    // permitted (PostgreSQL): the most recent one shadows the older, and RELEASE
    // / ROLLBACK TO act on the most recent match.
    [[nodiscard]] bool has_savepoint(std::string_view name) const noexcept;

    // Validate and apply a transaction-control statement, advancing the session
    // state and returning the outcome. Accepts the statement node itself or any
    // ancestor that wraps exactly one transaction-control statement. A node that
    // is not a transaction-control statement is an Error (the state is unchanged).
    //
    // Handled: BEGIN / START TRANSACTION, COMMIT, ROLLBACK [TO SAVEPOINT name],
    // SAVEPOINT name, RELEASE [SAVEPOINT] name. A COMMIT or a full ROLLBACK ends
    // the block and discards every savepoint. Per PostgreSQL, RELEASE and
    // ROLLBACK TO also discard the savepoints established AFTER the named one;
    // ROLLBACK TO keeps the named savepoint (you can roll back to it again),
    // while RELEASE removes it too.
    [[nodiscard]] TxnResult apply(const ast::ASTNode* stmt);

private:
    TxnState state_ = TxnState::Idle;
    // Live savepoints in establishment order (bottom .. top). Duplicate names
    // are allowed; lookups scan from the top so the most recent match wins.
    std::vector<std::string> savepoints_;
};

}  // namespace db25::semantic
