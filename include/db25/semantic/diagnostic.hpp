// DB25 Semantic Analyzer - Diagnostics

#pragma once

#include <cstdint>
#include <string>

namespace db25::semantic {

enum class Severity : std::uint8_t {
    Error,
    Warning,
};

enum class DiagnosticCode : std::uint16_t {
    UnresolvedTable,
    UnresolvedColumn,
    UnresolvedQualifier,
    AmbiguousColumn,
    // `SELECT *` used without a FROM clause (nothing to expand).
    StarWithoutFrom,
    // A `JOIN ... USING (col)` names a column missing from one/both sides.
    UsingColumnMissing,
    // Set-operation branches project a different number of columns.
    SetOpArityMismatch,
    // Set-operation branches have incompatible column types pairwise.
    SetOpTypeMismatch,
    // In a grouped query, a column referenced outside any aggregate that is not
    // one of the GROUP BY keys (SELECT list, ORDER BY, or HAVING).
    NonGroupedColumn,
    // An aggregate function call nested directly inside another aggregate.
    NestedAggregate,
    // A function call whose name is not in the signature table; result type
    // degrades to Unknown. Emitted as a Warning (not an error).
    UnknownFunction,
    // A cross-category comparison (e.g. text vs integer) that the coercion model
    // permits by implicit conversion. Emitted as a Warning (not an error).
    ImplicitCoercion,
    // Operands of an arithmetic operator whose types cannot be coerced to a
    // common numeric type (e.g. text + integer). An error.
    TypeMismatch,
    // A subquery used in a scalar position (SELECT-list item, comparison operand)
    // projects more than one column.
    ScalarSubqueryColumns,
    // The subquery on the right of `expr IN (subquery)` does not project exactly
    // one column.
    InSubqueryColumns,
    // An INSERT row (VALUES row or the projection of INSERT ... SELECT) has a
    // different number of values than the target column list.
    InsertArityMismatch,
    // An INSERT omits a NOT NULL target column that has no default value, so the
    // row would violate the NOT NULL constraint.
    NotNullViolation,
    // A LIMIT / OFFSET operand that is a literal but is negative or not an
    // integer (e.g. `LIMIT -1`, `LIMIT 1.5`).
    InvalidLimit,
};

// A diagnostic carries the parser node's source range so callers can point at
// the offending text (source_start / source_end are byte offsets into the SQL).
struct Diagnostic {
    Severity severity = Severity::Error;
    DiagnosticCode code = DiagnosticCode::UnresolvedColumn;
    std::string message;
    std::uint32_t source_start = 0;
    std::uint32_t source_end = 0;
};

}  // namespace db25::semantic
