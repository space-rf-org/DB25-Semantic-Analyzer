// DB25 Semantic Analyzer - Transaction control (implementation).

#include "db25/semantic/transaction.hpp"

#include "db25/ast/node_types.hpp"

#include <string>

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
        case NodeType::SavepointStmt:
        case NodeType::ReleaseSavepointStmt:
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

bool TransactionManager::has_savepoint(std::string_view name) const noexcept {
    for (const std::string& s : savepoints_) {
        if (s == name) return true;
    }
    return false;
}

TxnResult TransactionManager::apply(const ASTNode* stmt) {
    const ASTNode* t = find_txn_stmt(stmt);
    if (t == nullptr) {
        return {TxnResult::Status::Error, "not a transaction-control statement", state_};
    }

    // Index one past the most recent savepoint named `name`, or 0 if there is
    // none. The stack is scanned from the top so a duplicate name resolves to the
    // newest establishment (PostgreSQL shadowing).
    auto find_from_top = [this](std::string_view name) -> std::size_t {
        for (std::size_t i = savepoints_.size(); i > 0; --i) {
            if (savepoints_[i - 1] == name) return i;
        }
        return 0;
    };

    switch (t->node_type) {
        case NodeType::BeginStmt:
            // A nested BEGIN is a no-op warning (PostgreSQL): the existing block
            // stays open rather than the script aborting.
            if (state_ == TxnState::Active) {
                return {TxnResult::Status::Warning,
                        "there is already a transaction in progress", state_};
            }
            state_ = TxnState::Active;
            savepoints_.clear();  // a fresh block starts with no savepoints
            return {TxnResult::Status::Ok, {}, state_};

        case NodeType::CommitStmt:
            // COMMIT with no open block is a no-op warning, not an error.
            if (state_ != TxnState::Active) {
                return {TxnResult::Status::Warning,
                        "there is no transaction in progress", state_};
            }
            state_ = TxnState::Idle;
            savepoints_.clear();  // the block (and its savepoints) is gone
            return {TxnResult::Status::Ok, {}, state_};

        case NodeType::RollbackStmt: {
            // A ROLLBACK carrying a name is ROLLBACK TO SAVEPOINT <name>; a bare
            // ROLLBACK ends the whole block.
            if (t->primary_text.empty()) {
                if (state_ != TxnState::Active) {
                    return {TxnResult::Status::Warning,
                            "there is no transaction in progress", state_};
                }
                state_ = TxnState::Idle;
                savepoints_.clear();
                return {TxnResult::Status::Ok, {}, state_};
            }
            // ROLLBACK TO SAVEPOINT <name>.
            if (state_ != TxnState::Active) {
                return {TxnResult::Status::Error,
                        "ROLLBACK TO SAVEPOINT can only be used in transaction blocks",
                        state_};
            }
            const std::size_t at = find_from_top(t->primary_text);
            if (at == 0) {
                return {TxnResult::Status::Error,
                        "savepoint \"" + std::string(t->primary_text) + "\" does not exist",
                        state_};
            }
            // Roll back to it: savepoints established AFTER the named one are
            // destroyed; the named savepoint itself is kept (you may roll back to
            // it again). The block stays open.
            savepoints_.resize(at);
            return {TxnResult::Status::Ok, {}, state_};
        }

        case NodeType::SavepointStmt: {
            if (state_ != TxnState::Active) {
                return {TxnResult::Status::Error,
                        "SAVEPOINT can only be used in transaction blocks", state_};
            }
            if (t->primary_text.empty()) {
                return {TxnResult::Status::Error, "SAVEPOINT requires a name", state_};
            }
            savepoints_.emplace_back(t->primary_text);
            return {TxnResult::Status::Ok, {}, state_};
        }

        case NodeType::ReleaseSavepointStmt: {
            if (state_ != TxnState::Active) {
                return {TxnResult::Status::Error,
                        "RELEASE SAVEPOINT can only be used in transaction blocks", state_};
            }
            const std::size_t at = find_from_top(t->primary_text);
            if (at == 0) {
                return {TxnResult::Status::Error,
                        "savepoint \"" + std::string(t->primary_text) + "\" does not exist",
                        state_};
            }
            // RELEASE destroys the named savepoint and every savepoint established
            // after it; the transaction's effects are untouched (that is what
            // ROLLBACK TO is for). The block stays open.
            savepoints_.resize(at - 1);
            return {TxnResult::Status::Ok, {}, state_};
        }

        default:
            // find_txn_stmt only returns the kinds handled above.
            return {TxnResult::Status::Error, "unsupported transaction-control statement",
                    state_};
    }
}

}  // namespace db25::semantic
