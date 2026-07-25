// DB25 Semantic Analyzer - Transaction control (implementation).

#include "db25/semantic/transaction.hpp"

#include "db25/ast/node_types.hpp"

namespace db25::semantic {

using ast::ASTNode;
using ast::NodeType;

namespace {

// Locate the single transaction-control statement in a (possibly wrapping)
// parse tree, mirroring how DDL is located. Returns nullptr if there is none.
const ASTNode* find_txn_stmt(const ASTNode* n) {
    if (n == nullptr) return nullptr;
    switch (n->node_type) {
        case NodeType::BeginStmt:
        case NodeType::CommitStmt:
        case NodeType::RollbackStmt:
            return n;
        default:
            break;
    }
    for (const ASTNode* c = n->first_child; c != nullptr; c = c->next_sibling) {
        if (const ASTNode* h = find_txn_stmt(c)) return h;
    }
    return nullptr;
}

}  // namespace

TxnResult TransactionManager::apply(const ASTNode* stmt) {
    const ASTNode* t = find_txn_stmt(stmt);
    if (t == nullptr) {
        return {TxnResult::Status::Error, "not a transaction-control statement", state_};
    }

    switch (t->node_type) {
        case NodeType::BeginStmt:
            // A nested BEGIN is a no-op warning (PostgreSQL): the existing block
            // stays open rather than the script aborting.
            if (state_ == TxnState::Active) {
                return {TxnResult::Status::Warning,
                        "there is already a transaction in progress", state_};
            }
            state_ = TxnState::Active;
            return {TxnResult::Status::Ok, {}, state_};

        case NodeType::CommitStmt:
            // COMMIT with no open block is a no-op warning, not an error.
            if (state_ != TxnState::Active) {
                return {TxnResult::Status::Warning,
                        "there is no transaction in progress", state_};
            }
            state_ = TxnState::Idle;
            return {TxnResult::Status::Ok, {}, state_};

        case NodeType::RollbackStmt:
            // ROLLBACK TO SAVEPOINT <name> arrives as a RollbackStmt carrying the
            // savepoint name. Savepoints are a later change; refuse it explicitly
            // rather than silently discarding the whole transaction.
            if (!t->primary_text.empty()) {
                return {TxnResult::Status::Error,
                        "ROLLBACK TO SAVEPOINT is not yet supported", state_};
            }
            if (state_ != TxnState::Active) {
                return {TxnResult::Status::Warning,
                        "there is no transaction in progress", state_};
            }
            state_ = TxnState::Idle;
            return {TxnResult::Status::Ok, {}, state_};

        default:
            // find_txn_stmt only returns the three handled kinds.
            return {TxnResult::Status::Error, "unsupported transaction-control statement",
                    state_};
    }
}

}  // namespace db25::semantic
