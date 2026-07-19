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
