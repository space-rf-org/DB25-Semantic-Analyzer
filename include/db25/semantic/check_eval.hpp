// DB25 Semantic Analyzer - constant CHECK evaluation
//
// Evaluates a CHECK predicate (stored in the catalog as verbatim text) over a
// row of all-constant INSERT values, so a definite violation can be caught
// statically - e.g. CHECK (age >= 0) with VALUES (..., -5).
//
// The evaluation is deliberately conservative: it uses SQL's three-valued
// logic, and any construct it cannot fold to a certain boolean (an unsupported
// operator, a referenced column not bound to a literal, a cross-type
// comparison, division by zero, ...) yields Unknown. Only a result of False is
// a violation, so there are no false positives.

#pragma once

#include "db25/ast/ast_node.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace db25::semantic {

// Each column the predicate may reference, bound to the literal AST node it
// receives in the INSERT row. Columns not given a literal are simply absent.
using CheckBindings = std::unordered_map<std::string, const ast::ASTNode*>;

enum class CheckResult { True, False, Unknown };

// Re-parse `expr_text` and evaluate it against `bindings`. Returns False only
// when the predicate is certainly violated; True or Unknown otherwise.
[[nodiscard]] CheckResult evaluate_check(std::string_view expr_text,
                                         const CheckBindings& bindings);

}  // namespace db25::semantic
