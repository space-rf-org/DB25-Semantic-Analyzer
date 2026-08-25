// DB25 Semantic Analyzer - Analyzer implementation

#include "db25/semantic/analyzer.hpp"

#include "db25/ast/node_types.hpp"
#include "db25/parser/parser.hpp"
#include "db25/semantic/ast_helpers.hpp"
#include "db25/semantic/check_eval.hpp"
#include "db25/semantic/identifier.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace db25::semantic {

using ast::NodeType;

namespace {

[[nodiscard]] std::string to_upper(std::string_view s) {
    std::string r{s};
    for (char& c : r) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return r;
}

// The set of function names treated as aggregates. Kept as a single table so it
// is trivially extensible: add a name here and it is recognized everywhere
// (grouping legality and result typing). Registering a name here is what makes
// the grouping logic treat a call as an aggregate (so a bare non-grouped column
// alongside it is flagged) and lets an empty-group result be typed as nullable.
constexpr std::array<std::string_view, 15> kAggregateNames = {
    // Core SQL aggregates.
    "COUNT", "SUM", "AVG", "MIN", "MAX",
    // Statistical aggregates (all yield an approximate DOUBLE result).
    "STDDEV", "STDDEV_POP", "STDDEV_SAMP", "VARIANCE", "VAR_POP", "VAR_SAMP",
    // Collection / boolean aggregates.
    "STRING_AGG", "ARRAY_AGG", "BOOL_AND", "BOOL_OR",
};

[[nodiscard]] bool is_aggregate_name(std::string_view upper_name) {
    for (const std::string_view a : kAggregateNames) {
        if (a == upper_name) {
            return true;
        }
    }
    return false;
}

// True if `node` is a column-reference-like leaf. The parser emits ColumnRef
// for a top-level column and Identifier for a bare column that is the direct
// argument of a function call; both carry the (possibly dotted) name in
// primary_text and resolve the same way.
[[nodiscard]] bool is_column_ref_node(NodeType t) {
    return t == NodeType::ColumnRef || t == NodeType::Identifier;
}

[[nodiscard]] bool is_numeric(DataType t) {
    switch (t) {
        case DataType::TinyInt:
        case DataType::SmallInt:
        case DataType::Integer:
        case DataType::BigInt:
        case DataType::Decimal:
        case DataType::Real:
        case DataType::Double:
            return true;
        default:
            return false;
    }
}

// The exact-integer numeric types. Distinct from is_numeric, which also admits
// the approximate/exact-fractional kinds (Decimal/Real/Double). Date +/- N day
// arithmetic is defined only for integer N: `date + 2.5` has no meaning in SQL.
[[nodiscard]] bool is_integer_type(DataType t) {
    switch (t) {
        case DataType::TinyInt:
        case DataType::SmallInt:
        case DataType::Integer:
        case DataType::BigInt:
            return true;
        default:
            return false;
    }
}

// Widen two numeric types to a common arithmetic result type.
[[nodiscard]] DataType promote_numeric(DataType a, DataType b) {
    if (!is_numeric(a) || !is_numeric(b)) {
        return DataType::Unknown;
    }
    // Real (float4) has an exact operator only with itself; mixed with ANY other
    // numeric type PostgreSQL promotes to double precision (float8) - the numeric
    // category's preferred type - never to real. So `real + real` -> real, but
    // `real + {int, bigint, decimal, double}` -> double. Ranking Real between
    // Decimal and Double (as before) wrongly made `numeric + real` -> real, a
    // narrower/lossier type than the reference.
    if (a == DataType::Real || b == DataType::Real) {
        return (a == b) ? DataType::Real : DataType::Double;
    }
    // The remaining exact/inexact numerics widen along a single ladder.
    auto rank = [](DataType t) -> int {
        switch (t) {
            case DataType::TinyInt: return 1;
            case DataType::SmallInt: return 2;
            case DataType::Integer: return 3;
            case DataType::BigInt: return 4;
            case DataType::Decimal: return 5;
            case DataType::Double: return 6;
            default: return 0;
        }
    };
    return rank(a) >= rank(b) ? a : b;
}

[[nodiscard]] bool is_comparison_op(std::string_view op) {
    return op == "=" || op == "==" || op == "<>" || op == "!=" || op == "<" ||
           op == ">" || op == "<=" || op == ">=";
}

// `a IS [NOT] DISTINCT FROM b`: a null-safe comparison. Same operand
// compatibility as an ordinary comparison, but the result is ALWAYS a plain
// boolean, never NULL (two NULLs are "not distinct"; NULL vs non-NULL is
// "distinct"). The parser emits these exact uppercase operator texts.
[[nodiscard]] bool is_null_safe_comparison_op(std::string_view op) {
    return op == "IS DISTINCT FROM" || op == "IS NOT DISTINCT FROM";
}

// A quantified comparison `x <cmp> ALL|ANY|SOME (subquery|array)`. The parser
// packs the quantifier into the operator text - "> ALL", ">= ALL", "= ANY",
// "<> ALL", "= SOME", ... - as a single BinaryExpr op (see parser
// test_subqueries.cpp AnyOperator). It is boolean-valued in SQL / PostgreSQL
// (SOME is a synonym for ANY). The leading token before the quantifier must be
// an ordinary comparison operator.
[[nodiscard]] bool is_quantified_comparison_op(std::string_view op) {
    const std::size_t sp = op.rfind(' ');
    if (sp == std::string_view::npos) {
        return false;
    }
    const std::string_view quant = op.substr(sp + 1);
    if (quant != "ALL" && quant != "ANY" && quant != "SOME") {
        return false;
    }
    return is_comparison_op(op.substr(0, sp));
}

[[nodiscard]] bool is_logical_op(std::string_view op) {
    return op == "AND" || op == "OR" || op == "and" || op == "or";
}

[[nodiscard]] bool is_arithmetic_op(std::string_view op) {
    return op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
}

[[nodiscard]] bool is_setop(NodeType t) {
    return t == NodeType::UnionStmt || t == NodeType::IntersectStmt ||
           t == NodeType::ExceptStmt;
}

// True if a function call carries an OVER clause (a WindowSpec child), i.e. it
// is a window function. A windowed call — even an aggregate name like SUM — is
// evaluated after grouping and so is NOT a grouping aggregate.
[[nodiscard]] bool has_window_spec(const ASTNode* call) {
    for (const ASTNode* c = call->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type == NodeType::WindowSpec) {
            return true;
        }
    }
    return false;
}

// A FROM/JOIN child that contributes relations to the scope (as opposed to a
// join predicate such as an ON expression or a USING clause).
[[nodiscard]] bool is_relation_node(NodeType t) {
    switch (t) {
        case NodeType::TableRef:
        case NodeType::Subquery:
        case NodeType::SubqueryExpr:
        case NodeType::JoinClause:
        case NodeType::InnerJoin:
        case NodeType::LeftJoin:
        case NodeType::RightJoin:
        case NodeType::FullJoin:
        case NodeType::CrossJoin:
        case NodeType::LateralJoin:
            return true;
        default:
            return false;
    }
}

// Which side of a join supplies NULLs for unmatched rows (and so makes the
// other side's columns nullable). Determined from the join node: dedicated
// Left/Right/Full join node types, or a JoinClause whose primary_text names the
// join ("LEFT JOIN", "RIGHT OUTER JOIN", "FULL JOIN", …).
enum class JoinNullSide {
    None,   // INNER / CROSS: no side is null-supplied
    Right,  // LEFT [OUTER] JOIN: right side becomes nullable
    Left,   // RIGHT [OUTER] JOIN: left side becomes nullable
    Both,   // FULL [OUTER] JOIN: both sides become nullable
};

[[nodiscard]] JoinNullSide join_null_side(const ASTNode* join) {
    switch (join->node_type) {
        case NodeType::LeftJoin: return JoinNullSide::Right;
        case NodeType::RightJoin: return JoinNullSide::Left;
        case NodeType::FullJoin: return JoinNullSide::Both;
        case NodeType::InnerJoin:
        case NodeType::CrossJoin:
        case NodeType::LateralJoin: return JoinNullSide::None;
        default: break;
    }
    // JoinClause carries the kind in primary_text, e.g. "LEFT JOIN".
    const std::string kind = to_upper(join->primary_text);
    if (kind.find("FULL") != std::string::npos) {
        return JoinNullSide::Both;
    }
    if (kind.find("LEFT") != std::string::npos) {
        return JoinNullSide::Right;
    }
    if (kind.find("RIGHT") != std::string::npos) {
        return JoinNullSide::Left;
    }
    return JoinNullSide::None;  // INNER / CROSS / unqualified
}

// ---------------------------------------------------------------------------
// Type coercion model
// ---------------------------------------------------------------------------
// A single, conservative, well-commented model that answers "given two types
// and the syntactic context they meet in, what is the common/result type, and
// is the pairing clean, an implicit conversion, or incompatible?". Comparison,
// arithmetic, and set-operation (UNION/…) reconciliation are all routed through
// it so the rules live in one place.

// Broad type families the coercion rules reason over.
enum class TypeCategory { Numeric, String, Temporal, Boolean, Wildcard, Other };

[[nodiscard]] TypeCategory category_of(DataType t) {
    switch (t) {
        case DataType::TinyInt:
        case DataType::SmallInt:
        case DataType::Integer:
        case DataType::BigInt:
        case DataType::Decimal:
        case DataType::Real:
        case DataType::Double:
            return TypeCategory::Numeric;
        case DataType::Char:
        case DataType::VarChar:
        case DataType::Text:
            return TypeCategory::String;
        case DataType::Date:
        case DataType::Time:
        case DataType::Timestamp:
        case DataType::Interval:
            return TypeCategory::Temporal;
        case DataType::Boolean:
            return TypeCategory::Boolean;
        // NULL / Unknown / ANY unify with anything (a wildcard).
        case DataType::Null:
        case DataType::Unknown:
        case DataType::Any:
            return TypeCategory::Wildcard;
        default:
            return TypeCategory::Other;
    }
}

// The syntactic context two types meet in. It only changes what a *non-unifiable*
// pairing means: a soft implicit coercion in a comparison, but an error in
// arithmetic or set-operation reconciliation.
//   * Assignment: storing a value into a target column (INSERT value -> column,
//     UPDATE SET column = value). A numeric<->string pairing is accepted as a
//     soft implicit conversion (ImplicitCoercion); any other cross-category
//     pairing is an error (Incompatible).
enum class CoercionKind { Comparison, Arithmetic, UnionReconcile, Assignment };

enum class CoercionStatus {
    Ok,                // unify cleanly (identical, wildcard, promotion, char/varchar/text)
    ImplicitCoercion,  // cross-category but permitted in a comparison (soft)
    Incompatible,      // cannot be reconciled in this context
};

struct Coercion {
    CoercionStatus status;
    DataType type;  // common/result type (Unknown when Incompatible)
};

[[nodiscard]] Coercion coerce(DataType a, DataType b, CoercionKind kind) {
    // Arithmetic unifies ONLY numeric operands (or NULL/Unknown). Two operands
    // of the same non-numeric type - text + text, boolean + boolean - are a hard
    // type error in PostgreSQL ("operator does not exist: text + text"), not a
    // clean unification; without this guard the identical-type shortcut and the
    // same-category (string/string) branch below would wrongly return Ok and hand
    // a bogus result type to the binder. Temporal/interval arithmetic is resolved
    // by temporal_arith() before coerce() is ever consulted for those operands.
    if (kind == CoercionKind::Arithmetic) {
        const auto arith_ok = [](DataType t) {
            return category_of(t) == TypeCategory::Numeric ||
                   category_of(t) == TypeCategory::Wildcard;
        };
        if (!arith_ok(a) || !arith_ok(b)) {
            return {CoercionStatus::Incompatible, DataType::Unknown};
        }
    }
    // Identical types: trivially compatible.
    if (a == b) {
        return {CoercionStatus::Ok, a};
    }
    // NULL / Unknown / ANY unify with anything, taking the other operand's type.
    if (category_of(a) == TypeCategory::Wildcard) {
        return {CoercionStatus::Ok, b};
    }
    if (category_of(b) == TypeCategory::Wildcard) {
        return {CoercionStatus::Ok, a};
    }
    const TypeCategory ca = category_of(a);
    const TypeCategory cb = category_of(b);
    // Numeric promotion: Integer < BigInt < Decimal < Double (see promote_numeric).
    if (ca == TypeCategory::Numeric && cb == TypeCategory::Numeric) {
        return {CoercionStatus::Ok, promote_numeric(a, b)};
    }
    // char / varchar / text collapse to text.
    if (ca == TypeCategory::String && cb == TypeCategory::String) {
        return {CoercionStatus::Ok, DataType::Text};
    }
    // Everything else (cross-category, or two different temporals, boolean vs
    // non-boolean, …) is not cleanly unifiable. In a comparison we still allow
    // it as a soft implicit coercion; in arithmetic / set reconciliation it is
    // an error.
    if (kind == CoercionKind::Comparison) {
        return {CoercionStatus::ImplicitCoercion, DataType::Boolean};
    }
    if (kind == CoercionKind::Assignment) {
        // A string assigned to a temporal or boolean column (e.g.
        // `INSERT INTO t(d) VALUES ('2020-01-01')`, DATE d) is a standard implicit
        // conversion: PostgreSQL/SQLite parse the string to the column's type. DB25
        // is PostgreSQL-canon, so accept it *silently* (no diagnostic) with the
        // target column's type `a`.
        if (cb == TypeCategory::String &&
            (ca == TypeCategory::Temporal || ca == TypeCategory::Boolean)) {
            return {CoercionStatus::Ok, a};
        }
        // Across the numeric/string boundary (e.g. a text literal into an integer
        // column) is a permitted implicit conversion, flagged softly; the stored
        // value takes the target column's type `a`.
        if ((ca == TypeCategory::Numeric && cb == TypeCategory::String) ||
            (ca == TypeCategory::String && cb == TypeCategory::Numeric)) {
            return {CoercionStatus::ImplicitCoercion, a};
        }
        // Any other cross-category assignment (e.g. temporal vs numeric) is a hard
        // type mismatch.
        return {CoercionStatus::Incompatible, DataType::Unknown};
    }
    return {CoercionStatus::Incompatible, DataType::Unknown};
}

// Temporal arithmetic. The plain numeric coercion model (`coerce`) cannot type
// date/time arithmetic because the result depends on the *operator*, not just
// the operand categories: `date - date` is an Interval, `date + interval` is a
// Date, `date + date` is meaningless. This helper captures those operator-aware
// rules; the caller consults it before falling back to numeric coercion.
enum class TemporalArithStatus {
    NotTemporal,  // neither operand is temporal: not our concern (use `coerce`)
    Ok,           // a valid temporal operation; `type` is the result type
    Invalid,      // a temporal operand combined in a way with no meaning
};

struct TemporalArith {
    TemporalArithStatus status;
    DataType type;  // result type when Ok (Unknown otherwise)
};

// Result of `lt <op> rt` when at least one operand is temporal. Supported forms:
//   * temporal ± interval        -> same temporal   (date/time/timestamp shift)
//   * interval + temporal        -> that temporal   (+ is commutative)
//   * interval ± interval        -> interval
//   * date ± integer             -> date            (day arithmetic)
//   * integer + date             -> date
//   * temporal - temporal (same) -> interval        (elapsed span)
// Anything else touching a temporal (date + date, date + timestamp, date * n,
// interval - date, timestamp + integer, …) is Invalid.
[[nodiscard]] TemporalArith temporal_arith(DataType a, DataType b,
                                           std::string_view op) {
    const bool a_temporal = category_of(a) == TypeCategory::Temporal;
    const bool b_temporal = category_of(b) == TypeCategory::Temporal;
    if (!a_temporal && !b_temporal) {
        return {TemporalArithStatus::NotTemporal, DataType::Unknown};
    }
    const bool a_interval = (a == DataType::Interval);
    const bool b_interval = (b == DataType::Interval);
    const bool a_numeric = category_of(a) == TypeCategory::Numeric;
    const bool b_numeric = category_of(b) == TypeCategory::Numeric;

    // Interval scaling by a number is valid and yields an interval:
    //   `interval * n`, `n * interval`, `interval / n`. Only INTERVAL scales this
    //   way - multiplying or dividing any other temporal (date/time/timestamp) by
    //   a number is meaningless and stays Invalid below.
    if (op == "*" && ((a_interval && b_numeric) || (a_numeric && b_interval))) {
        return {TemporalArithStatus::Ok, DataType::Interval};
    }
    if (op == "/" && a_interval && b_numeric) {
        return {TemporalArithStatus::Ok, DataType::Interval};
    }

    // Beyond interval scaling, only + and - carry temporal meaning; other
    // operators over a temporal are invalid.
    const bool is_plus = (op == "+");
    const bool is_minus = (op == "-");
    if (!is_plus && !is_minus) {
        return {TemporalArithStatus::Invalid, DataType::Unknown};
    }

    // interval ± interval -> interval.
    if (a_interval && b_interval) {
        return {TemporalArithStatus::Ok, DataType::Interval};
    }
    // temporal ± interval -> same temporal (the non-interval temporal is shifted).
    if (a_temporal && !a_interval && b_interval) {
        return {TemporalArithStatus::Ok, a};
    }
    // interval + temporal -> that temporal (+ only; `interval - date` is invalid).
    if (a_interval && b_temporal && !b_interval) {
        return is_plus ? TemporalArith{TemporalArithStatus::Ok, b}
                       : TemporalArith{TemporalArithStatus::Invalid, DataType::Unknown};
    }
    // date ± integer -> date (day arithmetic); integer + date -> date. Only an
    // INTEGER shifts a date by whole days; `date + double` / `date + numeric`
    // have no SQL operator (PostgreSQL: "operator does not exist: date + double
    // precision") and must stay Invalid rather than be silently typed Date.
    if (a == DataType::Date && is_integer_type(b)) {
        return {TemporalArithStatus::Ok, DataType::Date};
    }
    if (is_integer_type(a) && b == DataType::Date && is_plus) {
        return {TemporalArithStatus::Ok, DataType::Date};
    }
    // temporal - temporal of the *same* kind -> interval (elapsed span).
    if (is_minus && a_temporal && b_temporal && a == b) {
        return {TemporalArithStatus::Ok, DataType::Interval};
    }
    // Any other combination involving a temporal has no defined meaning.
    return {TemporalArithStatus::Invalid, DataType::Unknown};
}

// SUM widens integer inputs to BigInt (avoids overflow of the accumulator);
// exact/approximate numerics keep their kind. Non-numeric inputs pass through
// unchanged (degrading conservatively rather than guessing).
[[nodiscard]] DataType sum_result(DataType arg) {
    switch (arg) {
        case DataType::TinyInt:
        case DataType::SmallInt:
        case DataType::Integer:
        case DataType::BigInt:
            return DataType::BigInt;
        default:
            return arg;
    }
}

struct FunctionType {
    DataType type;
    bool known;  // false => name not in the signature table (soft-diagnose)
    // COALESCE / GREATEST / LEAST reconcile their arguments to a common type; set
    // when that reconciliation hit an incompatible pair (text vs integer, …), so
    // the caller can emit a TypeMismatch as the set-op / VALUES / CASE reconcilers
    // already do. Kept separate from `known` (the name IS known).
    bool arg_type_mismatch = false;
};

// Result type for a function call given its (already inferred) argument types.
// Aggregates and a small, extensible table of common scalar functions are
// modeled; anything else degrades to Unknown with known=false.
[[nodiscard]] FunctionType function_result_type(std::string_view upper_name,
                                                const std::vector<DataType>& args) {
    const DataType arg0 = args.empty() ? DataType::Unknown : args.front();

    if (is_aggregate_name(upper_name)) {
        if (upper_name == "COUNT") {
            return {DataType::BigInt, true};  // COUNT(*) / COUNT(expr)
        }
        if (upper_name == "SUM") {
            return {sum_result(arg0), true};
        }
        // AVG and the statistical aggregates produce an approximate (DOUBLE)
        // result regardless of the (numeric) input kind.
        if (upper_name == "AVG" || upper_name == "STDDEV" ||
            upper_name == "STDDEV_POP" || upper_name == "STDDEV_SAMP" ||
            upper_name == "VARIANCE" || upper_name == "VAR_POP" ||
            upper_name == "VAR_SAMP") {
            return {DataType::Double, true};
        }
        // STRING_AGG concatenates the group's values into one string.
        if (upper_name == "STRING_AGG") {
            return {DataType::Text, true};
        }
        // ARRAY_AGG collects the group's values into an array.
        if (upper_name == "ARRAY_AGG") {
            return {DataType::Array, true};
        }
        // BOOL_AND / BOOL_OR reduce a group of booleans to one boolean.
        if (upper_name == "BOOL_AND" || upper_name == "BOOL_OR") {
            return {DataType::Boolean, true};
        }
        // MIN / MAX preserve the argument type.
        return {arg0, true};
    }

    // -----------------------------------------------------------------------
    // Scalar function signature table (grouped by category; extend here).
    // -----------------------------------------------------------------------
    // We only classify the *result type* here and mark the name as recognized
    // (known=true); argument nullability is combined separately in
    // function_nullability. A recognized name never raises a spurious
    // UnknownFunction warning even when its result type has to degrade to
    // Unknown (e.g. a numeric function applied to a non-numeric argument).

    // --- String functions -> Text ------------------------------------------
    // Case conversion, substring extraction, trimming, concatenation,
    // padding, and replacement all yield character data.
    if (upper_name == "UPPER" || upper_name == "LOWER" ||
        upper_name == "INITCAP" || upper_name == "SUBSTRING" ||
        upper_name == "SUBSTR" || upper_name == "TRIM" ||
        upper_name == "LTRIM" || upper_name == "RTRIM" ||
        upper_name == "CONCAT" || upper_name == "REPLACE" ||
        upper_name == "LEFT" || upper_name == "RIGHT" ||
        upper_name == "LPAD" || upper_name == "RPAD") {
        return {DataType::Text, true};
    }
    // --- String functions -> Integer (length / position) -------------------
    if (upper_name == "LENGTH" || upper_name == "CHAR_LENGTH" ||
        upper_name == "CHARACTER_LENGTH" || upper_name == "POSITION" ||
        upper_name == "STRPOS") {
        return {DataType::Integer, true};
    }

    // --- Numeric functions: preserve the (numeric) argument type -----------
    // ABS/CEIL/FLOOR/ROUND/TRUNC/MOD keep the numeric kind of their first
    // argument (e.g. ROUND(DECIMAL) -> DECIMAL); a non-numeric argument
    // degrades to Unknown while the name stays recognized.
    if (upper_name == "ABS" || upper_name == "CEIL" ||
        upper_name == "CEILING" || upper_name == "FLOOR" ||
        upper_name == "ROUND" || upper_name == "TRUNC" ||
        upper_name == "MOD") {
        return {is_numeric(arg0) ? arg0 : DataType::Unknown, true};
    }
    // SIGN returns -1 / 0 / 1.
    if (upper_name == "SIGN") {
        return {DataType::Integer, true};
    }
    // --- Numeric functions: approximate (DOUBLE) result --------------------
    if (upper_name == "POWER" || upper_name == "POW" ||
        upper_name == "SQRT" || upper_name == "EXP" ||
        upper_name == "LN" || upper_name == "LOG") {
        return {DataType::Double, true};
    }

    // --- Date/time functions -----------------------------------------------
    // The niladic current-value functions have fixed result types.
    if (upper_name == "NOW" || upper_name == "CURRENT_TIMESTAMP") {
        return {DataType::Timestamp, true};
    }
    if (upper_name == "CURRENT_DATE") {
        return {DataType::Date, true};
    }
    if (upper_name == "CURRENT_TIME") {
        return {DataType::Time, true};
    }
    // DATE_TRUNC rounds a timestamp down to a field boundary.
    if (upper_name == "DATE_TRUNC") {
        return {DataType::Timestamp, true};
    }
    // EXTRACT / DATE_PART return the requested field as a numeric value.
    if (upper_name == "EXTRACT" || upper_name == "DATE_PART") {
        return {DataType::Double, true};
    }
    // AGE returns the difference between two timestamps as an interval.
    if (upper_name == "AGE") {
        return {DataType::Interval, true};
    }

    // --- Conditional functions ---------------------------------------------
    // NULLIF(a, b) is `a` when a<>b else NULL: it takes the first argument's
    // type (and is always nullable, handled in function_nullability).
    if (upper_name == "NULLIF") {
        return {arg0, true};
    }
    // GREATEST / LEAST and COALESCE reconcile all argument types to a common
    // type (the same UnionReconcile rule as a set operation / VALUES / CASE). When
    // a pair is incompatible (e.g. text vs integer, which PostgreSQL rejects) we
    // must NOT re-concretize a later step's type - the 3+-argument fold
    // `unify(unify(text,int)->Unknown, int)->int` would otherwise mask the error
    // and hand back a bogus Integer. Track the mismatch and degrade to Unknown so
    // the caller flags it, honouring "Unknown when the arguments are incompatible".
    if (upper_name == "GREATEST" || upper_name == "LEAST" ||
        upper_name == "COALESCE") {
        DataType unified = DataType::Unknown;
        bool seeded = false;
        bool mismatch = false;
        for (const DataType a : args) {
            if (!seeded) {
                unified = a;
                seeded = true;
                continue;
            }
            const Coercion r = coerce(unified, a, CoercionKind::UnionReconcile);
            if (r.status == CoercionStatus::Incompatible) {
                mismatch = true;  // sticky: a later compatible pair must not clear it
            } else {
                unified = r.type;
            }
        }
        if (mismatch) {
            return {DataType::Unknown, true, true};
        }
        return {unified, true, false};
    }
    return {DataType::Unknown, false};
}

// ---------------------------------------------------------------------------
// Nullability
// ---------------------------------------------------------------------------
// Nullability uses the parser's 2-bit encoding throughout: 0 = unknown,
// 1 = not-null, 2 = nullable.

// Combine operand nullabilities under the "nullable if any operand is nullable"
// rule that governs arithmetic and most scalar functions: nullable (2) if any
// part is nullable; not-null (1) if every part is known not-null; otherwise
// unknown (0).
[[nodiscard]] int combine_nullable_any(const std::vector<int>& parts) {
    bool all_not_null = !parts.empty();
    for (const int p : parts) {
        if (p == 2) {
            return 2;  // any nullable operand makes the result nullable
        }
        if (p != 1) {
            all_not_null = false;  // an unknown operand blocks a not-null claim
        }
    }
    return all_not_null ? 1 : 0;
}

// Nullability of a function-call result given its argument nullabilities.
[[nodiscard]] int function_nullability(std::string_view upper_name,
                                       const std::vector<int>& arg_nulls) {
    if (upper_name == "COUNT") {
        return 1;  // COUNT never returns NULL
    }
    if (is_aggregate_name(upper_name)) {
        return 2;  // any other aggregate over an empty group yields NULL
    }
    // The niladic current-value date/time functions always return a value.
    if (upper_name == "NOW" || upper_name == "CURRENT_TIMESTAMP" ||
        upper_name == "CURRENT_DATE" || upper_name == "CURRENT_TIME") {
        return 1;
    }
    // NULLIF can always return NULL (when its two arguments are equal),
    // independent of its arguments' own nullability.
    if (upper_name == "NULLIF") {
        return 2;
    }
    if (upper_name == "COALESCE" || upper_name == "GREATEST" || upper_name == "LEAST") {
        // COALESCE returns its first non-NULL argument; GREATEST / LEAST skip NULL
        // arguments and yield NULL only when EVERY argument is NULL (Postgres). All
        // three are therefore NOT NULL iff any argument is known not-null - not the
        // default "nullable if any argument is nullable", which over-reported
        // GREATEST(not_null, nullable) as nullable.
        bool any_not_null = false;
        bool any_nullable = false;
        for (const int p : arg_nulls) {
            if (p == 1) any_not_null = true;
            if (p == 2) any_nullable = true;
        }
        if (any_not_null) return 1;
        return any_nullable ? 2 : 0;
    }
    // Ordinary scalar functions propagate nullability from their arguments.
    return combine_nullable_any(arg_nulls);
}

// ---------------------------------------------------------------------------
// CAST target types
// ---------------------------------------------------------------------------
// Map a SQL type name (as written after AS in a CAST, carried in the type
// node's primary_text) to a DataType. Length/precision (e.g. the 10 in
// VARCHAR(10)) is irrelevant to the category and ignored. Unrecognized names
// degrade to Unknown rather than guessing.
[[nodiscard]] DataType data_type_from_name(std::string_view name) {
    const std::string u = to_upper(name);
    // Strip a trailing "(...)" if the length rode along in the name itself.
    std::string_view base = u;
    if (const auto p = base.find('('); p != std::string_view::npos) {
        base = base.substr(0, p);
    }
    while (!base.empty() && base.back() == ' ') base.remove_suffix(1);

    if (base == "INT" || base == "INTEGER" || base == "INT4") return DataType::Integer;
    if (base == "BIGINT" || base == "INT8") return DataType::BigInt;
    if (base == "SMALLINT" || base == "INT2") return DataType::SmallInt;
    if (base == "TINYINT") return DataType::TinyInt;
    if (base == "DECIMAL" || base == "NUMERIC" || base == "NUMBER") return DataType::Decimal;
    if (base == "REAL" || base == "FLOAT4") return DataType::Real;
    if (base == "DOUBLE" || base == "DOUBLE PRECISION" || base == "FLOAT" ||
        base == "FLOAT8")
        return DataType::Double;
    if (base == "BOOLEAN" || base == "BOOL") return DataType::Boolean;
    if (base == "CHAR" || base == "CHARACTER" || base == "BPCHAR") return DataType::Char;
    if (base == "VARCHAR" || base == "CHARACTER VARYING") return DataType::VarChar;
    if (base == "TEXT" || base == "STRING" || base == "CLOB") return DataType::Text;
    if (base == "DATE") return DataType::Date;
    if (base == "TIME") return DataType::Time;
    if (base == "TIMESTAMP" || base == "DATETIME") return DataType::Timestamp;
    if (base == "INTERVAL") return DataType::Interval;
    if (base == "BLOB" || base == "BYTEA" || base == "BINARY") return DataType::Blob;
    return DataType::Unknown;
}

// ---------------------------------------------------------------------------
// Window (OVER) functions
// ---------------------------------------------------------------------------
// A window function is emitted as a FunctionCall carrying a WindowSpec child
// (the OVER clause). Pure ranking/analytic functions have fixed result types;
// an ordinary aggregate used with OVER keeps its aggregate result type.

// The rank-style window functions whose result is a row-number/rank integer.
[[nodiscard]] bool is_rank_window_fn(std::string_view upper) {
    return upper == "ROW_NUMBER" || upper == "RANK" || upper == "DENSE_RANK" ||
           upper == "NTILE";
}
// The distribution window functions whose result is a fraction in [0,1].
[[nodiscard]] bool is_ratio_window_fn(std::string_view upper) {
    return upper == "PERCENT_RANK" || upper == "CUME_DIST";
}
// The value-navigation window functions whose result takes the first argument's
// type (LAG/LEAD/FIRST_VALUE/LAST_VALUE/NTH_VALUE).
[[nodiscard]] bool is_value_window_fn(std::string_view upper) {
    return upper == "LAG" || upper == "LEAD" || upper == "FIRST_VALUE" ||
           upper == "LAST_VALUE" || upper == "NTH_VALUE";
}

// Result type of a call that carries an OVER clause. Ranking functions are
// integer (BIGINT), distribution functions are DOUBLE, value functions take
// the first argument's type; anything else (an aggregate used as a window
// function, e.g. SUM(x) OVER (...)) is typed by the ordinary function table.
[[nodiscard]] FunctionType window_function_result_type(std::string_view upper,
                                                       const std::vector<DataType>& args) {
    if (is_rank_window_fn(upper)) return {DataType::BigInt, true};
    if (is_ratio_window_fn(upper)) return {DataType::Double, true};
    if (is_value_window_fn(upper)) {
        return {args.empty() ? DataType::Unknown : args.front(), true};
    }
    return function_result_type(upper, args);
}

// Nullability of a windowed call. Ranking and distribution functions never
// return NULL; value-navigation functions can (a frame boundary yields no row);
// an aggregate-over-window follows the aggregate's nullability.
[[nodiscard]] int window_function_nullability(std::string_view upper,
                                              const std::vector<int>& arg_nulls) {
    if (is_rank_window_fn(upper) || is_ratio_window_fn(upper)) return 1;
    if (is_value_window_fn(upper)) return 2;
    return function_nullability(upper, arg_nulls);
}

}  // namespace

void Analyzer::analyze(ASTNode* root) {
    if (root == nullptr) {
        return;
    }
    expr_depth_ = 0;
    expr_depth_reported_ = false;
    if (root->node_type == NodeType::SelectStmt || is_setop(root->node_type)) {
        analyze_stmt(root, nullptr);
        return;
    }
    switch (root->node_type) {
        case NodeType::InsertStmt:
            analyze_insert(root);
            return;
        case NodeType::UpdateStmt:
            analyze_update(root);
            return;
        case NodeType::DeleteStmt:
            analyze_delete(root);
            return;
        default:
            // Other statement kinds are not yet analyzed; see docs/DESIGN.md.
            return;
    }
}

std::vector<ResolvedColumn> Analyzer::analyze_stmt(ASTNode* node, Scope* parent) {
    if (node == nullptr) {
        return {};
    }
    if (is_setop(node->node_type)) {
        return analyze_setop(node, parent);
    }
    if (node->node_type == NodeType::SelectStmt) {
        return analyze_query(node, parent);
    }
    return {};
}

const TableInfo* Analyzer::bind_base_table(ASTNode* table_ref, Scope& scope) {
    if (table_ref == nullptr) {
        return nullptr;
    }
    const std::string_view name = table_ref->primary_text;
    const std::string_view alias = alias_of(table_ref);
    const TableInfo* table = catalog_.find_table(name);
    if (table == nullptr) {
        add_diagnostic(DiagnosticCode::UnresolvedTable,
                       "unresolved table '" + std::string{name} + "'", table_ref);
        return nullptr;
    }
    RelationBinding binding;
    binding.name = std::string{name};
    binding.alias = std::string{alias};
    binding.table_id = table->table_id;
    for (const auto& c : table->columns) {
        binding.columns.push_back(
            ResolvedColumn{c.name, c.type, c.nullable, table->table_id, c.column_id});
    }
    scope.add_relation(std::move(binding));
    return table;
}

void Analyzer::check_assignment(DataType target, DataType value, const ASTNode* at) {
    const Coercion c = coerce(target, value, CoercionKind::Assignment);
    if (c.status == CoercionStatus::Incompatible) {
        add_diagnostic(DiagnosticCode::TypeMismatch,
                       "value type is not compatible with the target column type", at);
    } else if (c.status == CoercionStatus::ImplicitCoercion) {
        add_diagnostic(DiagnosticCode::ImplicitCoercion,
                       "implicit conversion assigning a value to a column of a "
                       "different type category",
                       at, Severity::Warning);
    }
}

void Analyzer::check_not_null_literal(const ColumnInfo* col, ASTNode* value) {
    if (col != nullptr && value != nullptr && !col->nullable &&
        value->node_type == NodeType::NullLiteral) {
        add_diagnostic(DiagnosticCode::NotNullViolation,
                       "cannot assign NULL to NOT NULL column '" + col->name + "'", value);
    }
}

namespace {
// True when `n` is built purely from literals and constant operators, so the
// CHECK evaluator can fold it to a certain value. Crucially this excludes
// ColumnRef/Identifier and function calls: a value supplied in an INSERT row or
// an UPDATE SET, or a column DEFAULT, is evaluated with no row in scope, so a
// bare identifier there is not a constant we may fold (in UPDATE it would be the
// column's OLD value, which we do not know). A unary minus of a literal
// (`-5`) and constant arithmetic/comparison are constants and are folded.
[[nodiscard]] bool is_constant_expr(const ASTNode* n) {
    if (n == nullptr) return false;
    switch (n->node_type) {
        case NodeType::IntegerLiteral:
        case NodeType::FloatLiteral:
        case NodeType::StringLiteral:
        case NodeType::BooleanLiteral:
        case NodeType::NullLiteral:
            return true;
        case NodeType::UnaryExpr:
        case NodeType::BinaryExpr:
        case NodeType::BetweenExpr:
        case NodeType::IsNullExpr: {
            if (n->first_child == nullptr) return false;
            for (const ASTNode* c = n->first_child; c != nullptr; c = c->next_sibling) {
                if (!is_constant_expr(c)) return false;
            }
            return true;
        }
        default:
            return false;  // ColumnRef, Identifier, FunctionCall, subquery, ...
    }
}

// Fold an integer-valued constant subtree to its value, or nullopt if it is not
// a compile-time integer constant. Deliberately small: literals, unary +/-, and
// +/-/* of constants (NOT / or %, to avoid re-introducing the division-by-zero
// and overflow concerns this feeds). Used only to reason about CASE guards and
// zero divisors, so it need not cover the whole grammar. Overflow-safe (returns
// nullopt rather than invoking signed-overflow UB).
[[nodiscard]] std::optional<std::int64_t> eval_const_int(const ASTNode* n) {
    if (n == nullptr) {
        return std::nullopt;
    }
    switch (n->node_type) {
        case NodeType::IntegerLiteral: {
            std::int64_t v = 0;
            const std::string_view t = n->primary_text;
            const auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
            if (ec == std::errc() && ptr == t.data() + t.size()) {
                return v;
            }
            return std::nullopt;
        }
        case NodeType::UnaryExpr: {
            const auto cv = eval_const_int(first_child(n));
            if (!cv) return std::nullopt;
            if (n->primary_text == "-") {
                if (*cv == std::numeric_limits<std::int64_t>::min()) return std::nullopt;
                return -*cv;
            }
            if (n->primary_text == "+") return *cv;
            return std::nullopt;
        }
        case NodeType::BinaryExpr: {
            const ASTNode* l = first_child(n);
            const ASTNode* r = l != nullptr ? l->next_sibling : nullptr;
            const auto lv = eval_const_int(l);
            const auto rv = eval_const_int(r);
            if (!lv || !rv) return std::nullopt;
            std::int64_t out = 0;
            const std::string_view op = n->primary_text;
            if (op == "+" && !__builtin_add_overflow(*lv, *rv, &out)) return out;
            if (op == "-" && !__builtin_sub_overflow(*lv, *rv, &out)) return out;
            if (op == "*" && !__builtin_mul_overflow(*lv, *rv, &out)) return out;
            return std::nullopt;
        }
        default:
            return std::nullopt;
    }
}

// Fold a boolean-valued constant subtree (a BooleanLiteral, or a comparison of
// two integer constants) to true/false, or nullopt if it is not a compile-time
// constant boolean. Used to decide whether a CASE guard is constant-true /
// constant-false (and hence which arms are dead). `2020` etc. that are not
// constants (a column comparison) correctly return nullopt.
[[nodiscard]] std::optional<bool> eval_const_bool(const ASTNode* n) {
    if (n == nullptr) {
        return std::nullopt;
    }
    if (n->node_type == NodeType::BooleanLiteral) {
        const std::string u = to_upper(n->primary_text);
        if (u == "TRUE") return true;
        if (u == "FALSE") return false;
        return std::nullopt;
    }
    if (n->node_type == NodeType::BinaryExpr) {
        const ASTNode* l = first_child(n);
        const ASTNode* r = l != nullptr ? l->next_sibling : nullptr;
        const auto lv = eval_const_int(l);
        const auto rv = eval_const_int(r);
        if (!lv || !rv) return std::nullopt;
        const std::string_view op = n->primary_text;
        if (op == "=" || op == "==") return *lv == *rv;
        if (op == "<>" || op == "!=") return *lv != *rv;
        if (op == "<") return *lv < *rv;
        if (op == "<=") return *lv <= *rv;
        if (op == ">") return *lv > *rv;
        if (op == ">=") return *lv >= *rv;
    }
    return std::nullopt;
}

[[nodiscard]] const ColumnInfo* column_by_id(const TableInfo& t, std::uint32_t id) {
    for (const ColumnInfo& c : t.columns) {
        if (c.column_id == id) return &c;
    }
    return nullptr;
}

// May the constant CHECK evaluator soundly reason about column `col_type` bound
// to the literal `value`? A temporal column whose value is written as a STRING
// literal ('2020-01-01 09:30:00', '06/15/2020') cannot: the evaluator has no date
// parser and would order it LEXICALLY - so '2020-01-01 9:30:00' compares GREATER
// than '2020-01-01 10:00:00' (at the hour, '9' > '1'), flipping a chronologically
// legal row to a false CheckViolation. Leaving such a value unbound keeps any
// CHECK over it Unknown (never a false False), honouring the evaluator's
// no-false-positive contract. A NULL value still binds (it is not a string
// literal), so IS [NOT] NULL folding on temporal columns is unaffected.
[[nodiscard]] bool check_binding_is_sound(DataType col_type, const ASTNode* value) {
    return !(category_of(col_type) == TypeCategory::Temporal && value != nullptr &&
             value->node_type == NodeType::StringLiteral);
}
}  // namespace

void Analyzer::check_row_against_checks(const TableInfo& table,
                                        const std::vector<const ColumnInfo*>& target_cols,
                                        const std::vector<ASTNode*>& vals, const ASTNode* at,
                                        bool apply_defaults) {
    // Bind each column given a constant value in this row to its value node. Only
    // constants are bound (see is_constant_expr): a value the evaluator cannot
    // fold leaves the column unbound, so any CHECK referencing it stays Unknown.
    CheckBindings binds;
    std::unordered_set<std::string> supplied;  // columns given an explicit value
    const std::size_t n = std::min(target_cols.size(), vals.size());
    for (std::size_t i = 0; i < target_cols.size(); ++i) {
        if (target_cols[i] != nullptr) supplied.insert(target_cols[i]->name);
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (target_cols[i] != nullptr && is_constant_expr(vals[i]) &&
            check_binding_is_sound(target_cols[i]->type, vals[i])) {
            binds.emplace(target_cols[i]->name, vals[i]);
        }
    }

    // For INSERT (apply_defaults), a column the row omits takes its DEFAULT. When
    // that default is a constant, bind its folded value so a default-sourced
    // violation is caught too - e.g. CHECK (status <> 'x') with DEFAULT 'x' and a
    // row that omits status. The parsers own the default ASTs and must outlive
    // the evaluate_check calls below (binds holds raw pointers into their arenas).
    // A non-constant default (now(), nextval(...)) simply stays unbound.
    std::deque<parser::Parser> default_parsers;
    if (apply_defaults) {
        for (const ColumnInfo& col : table.columns) {
            if (supplied.count(col.name) != 0) continue;   // an explicit value wins
            if (!col.has_default || col.default_expr.empty()) continue;
            parser::Parser& dp = default_parsers.emplace_back();
            auto parsed = dp.parse("SELECT " + col.default_expr);
            if (!parsed.has_value()) continue;
            ASTNode* list = nullptr;
            for (ASTNode* ch = parsed.value()->first_child; ch != nullptr;
                 ch = ch->next_sibling) {
                if (ch->node_type == NodeType::SelectList) { list = ch; break; }
            }
            ASTNode* item = list != nullptr ? list->first_child : nullptr;
            if (is_constant_expr(item) && check_binding_is_sound(col.type, item)) {
                binds.emplace(col.name, item);
            }
        }
    }

    for (const Constraint& c : table.constraints) {
        if (c.kind != Constraint::Kind::Check || c.expr.empty()) continue;
        // Only decide a CHECK whose every referenced column is bound to a literal;
        // otherwise the outcome is not certain and we stay silent.
        bool all_bound = true;
        for (const std::uint32_t cid : c.columns) {
            const ColumnInfo* col = column_by_id(table, cid);
            if (col == nullptr || binds.find(col->name) == binds.end()) {
                all_bound = false;
                break;
            }
        }
        if (!all_bound) continue;
        if (evaluate_check(c.expr, binds) == CheckResult::False) {
            const std::string named = c.name.empty() ? std::string{} : " '" + c.name + "'";
            add_diagnostic(DiagnosticCode::CheckViolation,
                           "row violates CHECK constraint" + named + " (" + c.expr + ") on '" +
                               table.name + "'",
                           at);
        }
    }
}

void Analyzer::analyze_insert(ASTNode* insert_stmt) {
    // Target table (always the first TableRef child).
    ASTNode* table_ref = find_child(insert_stmt, NodeType::TableRef);
    const std::string_view table_name =
        table_ref != nullptr ? table_ref->primary_text : std::string_view{};
    const TableInfo* table = table_name.empty() ? nullptr : catalog_.find_table(table_name);
    if (table == nullptr) {
        add_diagnostic(DiagnosticCode::UnresolvedTable,
                       "unresolved table '" + std::string{table_name} + "'",
                       table_ref != nullptr ? table_ref : insert_stmt);
        return;  // without a resolved table there is nothing to check against
    }

    // INSERT ... DEFAULT VALUES (a DefaultClause marker child): every column
    // takes its default. A NOT NULL column with no default would be inserted as
    // NULL, so flag it - the same rule as an omitted column, but here it applies
    // to the whole row and there is no value list to check.
    if (ASTNode* dv = find_child(insert_stmt, NodeType::DefaultClause);
        dv != nullptr && dv->primary_text == "DEFAULT VALUES") {
        for (const ColumnInfo& col : table->columns) {
            if (!col.nullable && !col.has_default) {
                add_diagnostic(DiagnosticCode::NotNullViolation,
                               "NOT NULL column '" + col.name +
                                   "' has no default in INSERT ... DEFAULT VALUES into '" +
                                   std::string{table_name} + "'",
                               table_ref != nullptr ? table_ref : insert_stmt);
            }
        }
        return;
    }

    // Target column list: an explicit ColumnList child, else the table's columns
    // in declaration order. `covered` tracks which table columns receive a value
    // (for the NOT NULL check).
    std::vector<const ColumnInfo*> target_cols;
    std::vector<bool> covered(table->columns.size(), false);
    if (ASTNode* col_list = find_child(insert_stmt, NodeType::ColumnList)) {
        std::vector<std::string_view> named;  // target columns already listed
        for (ASTNode* c = first_child(col_list); c != nullptr; c = c->next_sibling) {
            const std::string_view col_name = split_column_ref(c->primary_text).column;
            // A target column may be named only once: repeating it would assign
            // the same column twice ("column specified more than once").
            bool repeated = false;
            for (const std::string_view prior : named) {
                if (prior == col_name) {
                    repeated = true;
                    break;
                }
            }
            if (repeated) {
                add_diagnostic(DiagnosticCode::DuplicateColumn,
                               "column '" + std::string{col_name} +
                                   "' specified more than once in INSERT into '" +
                                   std::string{table_name} + "'",
                               c);
            } else {
                named.push_back(col_name);
            }
            const ColumnInfo* info = table->find_column(col_name);
            if (info == nullptr) {
                add_diagnostic(DiagnosticCode::UnresolvedColumn,
                               "unresolved column '" + std::string{col_name} +
                                   "' in target of INSERT into '" +
                                   std::string{table_name} + "'",
                               c);
                target_cols.push_back(nullptr);  // keep positional alignment
                continue;
            }
            target_cols.push_back(info);
            for (std::size_t i = 0; i < table->columns.size(); ++i) {
                if (&table->columns[i] == info) {
                    covered[i] = true;
                }
            }
        }
    } else {
        for (std::size_t i = 0; i < table->columns.size(); ++i) {
            target_cols.push_back(&table->columns[i]);
            covered[i] = true;
        }
    }

    // Source of values: a VALUES clause (one or more rows) or a subquery
    // (INSERT ... SELECT). Values are literals/expressions with no table scope.
    Scope scope(nullptr);
    if (ASTNode* values = find_child(insert_stmt, NodeType::ValuesClause)) {
        for (ASTNode* row = first_child(values); row != nullptr; row = row->next_sibling) {
            std::vector<ASTNode*> vals;
            for (ASTNode* v = first_child(row); v != nullptr; v = v->next_sibling) {
                vals.push_back(v);
            }
            if (vals.size() != target_cols.size()) {
                add_diagnostic(DiagnosticCode::InsertArityMismatch,
                               "INSERT row has " + std::to_string(vals.size()) +
                                   " values but the target has " +
                                   std::to_string(target_cols.size()) + " columns",
                               row);
            }
            const std::size_t n = std::min(vals.size(), target_cols.size());
            for (std::size_t i = 0; i < n; ++i) {
                const DataType vt = infer_expr(vals[i], scope);
                if (target_cols[i] != nullptr) {
                    check_assignment(target_cols[i]->type, vt, vals[i]);
                    check_not_null_literal(target_cols[i], vals[i]);
                }
            }
            check_row_against_checks(*table, target_cols, vals, row, /*apply_defaults=*/true);
        }
    } else {
        // INSERT ... SELECT: analyze the source query and check its projection.
        ASTNode* source = find_child(insert_stmt, NodeType::SelectStmt);
        if (source == nullptr) {
            for (ASTNode* c = first_child(insert_stmt); c != nullptr; c = c->next_sibling) {
                if (is_setop(c->node_type)) {
                    source = c;
                    break;
                }
            }
        }
        if (source != nullptr) {
            const std::vector<ResolvedColumn> proj = analyze_stmt(source, nullptr);
            if (proj.size() != target_cols.size()) {
                add_diagnostic(DiagnosticCode::InsertArityMismatch,
                               "INSERT ... SELECT projects " +
                                   std::to_string(proj.size()) +
                                   " columns but the target has " +
                                   std::to_string(target_cols.size()) + " columns",
                               source);
            }
            const std::size_t n = std::min(proj.size(), target_cols.size());
            for (std::size_t i = 0; i < n; ++i) {
                if (target_cols[i] != nullptr) {
                    check_assignment(target_cols[i]->type, proj[i].type, source);
                }
            }
        }
    }

    // NOT NULL check: a NOT NULL column that receives no value and has no default
    // would be inserted as NULL.
    for (std::size_t i = 0; i < table->columns.size(); ++i) {
        const ColumnInfo& col = table->columns[i];
        if (!covered[i] && !col.nullable && !col.has_default) {
            add_diagnostic(DiagnosticCode::NotNullViolation,
                           "NOT NULL column '" + col.name +
                               "' has no value and no default in INSERT into '" +
                               std::string{table_name} + "'",
                           table_ref != nullptr ? table_ref : insert_stmt);
        }
    }
}

void Analyzer::analyze_update(ASTNode* update_stmt) {
    Scope scope(nullptr);
    ASTNode* table_ref = find_child(update_stmt, NodeType::TableRef);
    const TableInfo* table = bind_base_table(table_ref, scope);

    // UPDATE ... FROM extra_relations: bring those relations into scope so SET
    // values and the WHERE predicate can reference them (e.g. `SET x = o.status
    // ... FROM orders o WHERE ...`). Without this every such column was reported
    // unresolved.
    if (ASTNode* from = find_child(update_stmt, NodeType::FromClause)) {
        resolve_from(from, scope);
    }

    // SET assignments: each is a BinaryExpr whose primary_text is the target
    // column name and whose (first) child is the value expression.
    if (ASTNode* set_clause = find_child(update_stmt, NodeType::SetClause)) {
        // The columns this UPDATE assigns and their value nodes, gathered so the
        // table's CHECK constraints can be evaluated against the post-update row.
        std::vector<const ColumnInfo*> set_cols;
        std::vector<ASTNode*> set_vals;
        for (ASTNode* asgn = first_child(set_clause); asgn != nullptr;
             asgn = asgn->next_sibling) {
            const std::string_view col_name = split_column_ref(asgn->primary_text).column;
            const ColumnInfo* info =
                table != nullptr ? table->find_column(col_name) : nullptr;
            if (table != nullptr && info == nullptr) {
                add_diagnostic(DiagnosticCode::UnresolvedColumn,
                               "unresolved column '" + std::string{col_name} +
                                   "' in SET clause",
                               asgn);
            }
            ASTNode* value = first_child(asgn);
            const DataType vt = infer_expr(value, scope);
            if (info != nullptr && value != nullptr) {
                check_assignment(info->type, vt, value);
                check_not_null_literal(info, value);
                set_cols.push_back(info);
                set_vals.push_back(value);
            }
        }

        // Evaluate CHECK constraints over the assigned columns. Defaults are NOT
        // substituted for unset columns: an UPDATE leaves them at their existing
        // values, which are unknown here, so a CHECK that references any unset
        // column stays Unknown. Only a CHECK whose every column is SET to a
        // constant is decided - e.g. UPDATE t SET age = -1 vs CHECK (age >= 0).
        if (table != nullptr && !set_cols.empty()) {
            check_row_against_checks(*table, set_cols, set_vals, set_clause,
                                     /*apply_defaults=*/false);
        }
    }

    // WHERE predicate: resolve against the target-table scope.
    if (ASTNode* where = find_child(update_stmt, NodeType::WhereClause)) {
        if (ASTNode* pred = first_child(where)) {
            infer_expr(pred, scope);
        }
    }
}

void Analyzer::analyze_delete(ASTNode* delete_stmt) {
    Scope scope(nullptr);
    ASTNode* table_ref = find_child(delete_stmt, NodeType::TableRef);
    bind_base_table(table_ref, scope);

    // DELETE ... USING extra_relations: bring those relations into scope so the
    // WHERE predicate can join against them (e.g. `... USING orders o WHERE
    // t.id = o.user_id`). resolve_from iterates the clause's TableRef children.
    if (ASTNode* using_clause = find_child(delete_stmt, NodeType::UsingClause)) {
        resolve_from(using_clause, scope);
    }

    if (ASTNode* where = find_child(delete_stmt, NodeType::WhereClause)) {
        if (ASTNode* pred = first_child(where)) {
            infer_expr(pred, scope);
        }
    }
}

void Analyzer::check_limit(ASTNode* limit_clause) {
    for (ASTNode* op = first_child(limit_clause); op != nullptr; op = op->next_sibling) {
        // Only literal operands are constrained: a LIMIT / OFFSET value must be a
        // non-negative integer. A FloatLiteral is fractional by construction and
        // always rejected; an IntegerLiteral is validated by text so a negative
        // literal (e.g. "-1") is rejected. Non-literal operands (parameters,
        // expressions) are not constrained.
        const bool is_int = op->node_type == NodeType::IntegerLiteral;
        const bool is_float = op->node_type == NodeType::FloatLiteral;
        if (!is_int && !is_float) {
            continue;
        }
        const std::string_view t = op->primary_text;
        bool valid = is_int && !t.empty();
        if (valid) {
            for (const char ch : t) {
                if (ch < '0' || ch > '9') {  // rejects '-', '.', etc.
                    valid = false;
                    break;
                }
            }
        }
        if (!valid) {
            add_diagnostic(DiagnosticCode::InvalidLimit,
                           "LIMIT/OFFSET operand must be a non-negative integer",
                           op);
        }
    }
}

const std::vector<ResolvedColumn>* Analyzer::projection_of(const ASTNode* query) const {
    const auto it = projections_.find(query);
    return it == projections_.end() ? nullptr : &it->second;
}

bool Analyzer::has_errors() const {
    for (const auto& d : diagnostics_) {
        if (d.severity == Severity::Error) {
            return true;
        }
    }
    return false;
}

DataType Analyzer::type_of(const ASTNode* node) const {
    const auto it = inferred_.find(node);
    return it == inferred_.end() ? DataType::Unknown : it->second;
}

DataType Analyzer::infer_scalar(ASTNode* expr, Scope& scope) {
    return infer_expr(expr, scope);
}

bool Analyzer::assignment_compatible(DataType target, DataType value) {
    return coerce(target, value, CoercionKind::Assignment).status !=
           CoercionStatus::Incompatible;
}

int Analyzer::nullability_of(const ASTNode* node) const {
    const auto it = nullability_.find(node);
    return it == nullability_.end() ? 0 : it->second;
}

int Analyzer::null_of(const ASTNode* node) const {
    return nullability_of(node);
}

bool Analyzer::is_correlated(const ASTNode* subquery) const {
    const auto it = correlated_.find(subquery);
    return it != correlated_.end() && it->second;
}

void Analyzer::record_type(ASTNode* node, DataType type) {
    if (node == nullptr) {
        return;
    }
    inferred_[node] = type;
    node->data_type = type;  // mirror onto the parser node
}

void Analyzer::record_nullability(ASTNode* node, int nullability) {
    if (node == nullptr) {
        return;
    }
    nullability_[node] = nullability;
    // Mirror onto the parser node's analysis context (2-bit field).
    node->context.analysis.nullability = static_cast<std::uint8_t>(nullability & 0x3);
}

void Analyzer::add_diagnostic(DiagnosticCode code, std::string message, const ASTNode* at,
                              Severity severity) {
    Diagnostic d;
    d.severity = severity;
    d.code = code;
    d.message = std::move(message);
    if (at != nullptr) {
        d.source_start = at->source_start;
        d.source_end = at->source_end;
    }
    diagnostics_.push_back(std::move(d));
}

namespace {

// Count the table references named `name` (identifiers fold case) in `node`'s
// subtree. Used to detect NON-LINEAR recursion: the recursive term of a WITH
// RECURSIVE CTE may reference the CTE at most once, so a count > 1 is illegal.
int count_table_refs(const ASTNode* node, std::string_view name) {
    if (node == nullptr) {
        return 0;
    }
    int n = (node->node_type == NodeType::TableRef &&
             iequals(node->primary_text, name))
                ? 1
                : 0;
    for (const ASTNode* c = first_child(node); c != nullptr; c = c->next_sibling) {
        n += count_table_refs(c, name);
    }
    return n;
}

}  // namespace

std::vector<ResolvedColumn> Analyzer::analyze_query(ASTNode* select_stmt, Scope* parent) {
    Scope scope(parent);

    // 1. CTEs: register each definition as a named relation before resolving
    //    FROM, so the body can reference the current scope and earlier CTEs.
    if (ASTNode* cte_clause = find_child(select_stmt, NodeType::CTEClause)) {
        // `WITH RECURSIVE` (parser sets NodeFlags::IsRecursive on the clause) lets
        // a CTE reference itself inside its own body.
        const bool clause_recursive = cte_clause->has_flag(ast::NodeFlags::IsRecursive);
        for (ASTNode* def = first_child(cte_clause); def != nullptr; def = def->next_sibling) {
            if (def->node_type != NodeType::CTEDefinition) {
                continue;
            }
            // A CTE body is a SELECT *or* a set operation (UNION/INTERSECT/EXCEPT).
            // find_child only matched SelectStmt, so a set-op body -
            // `WITH t AS (SELECT .. UNION SELECT ..)` - registered the CTE with no
            // columns, and a later `SELECT * FROM t` resolved to nothing. Take the
            // first query child (select or set-op) and analyze it via analyze_stmt,
            // which dispatches both.
            ASTNode* body = nullptr;
            for (ASTNode* c = first_child(def); c != nullptr; c = c->next_sibling) {
                if (c->node_type == NodeType::SelectStmt || is_setop(c->node_type)) {
                    body = c;
                    break;
                }
            }
            ASTNode* col_list = find_child(def, NodeType::ColumnList);
            NamedRelation cte;
            cte.name = std::string{def->primary_text};

            // A recursive CTE references itself inside its own body, so it must be
            // visible in scope BEFORE that body is analyzed. Its shape is
            // `<non-recursive anchor> UNION [ALL] <recursive term>`; per SQL /
            // Postgres the CTE's column names and types are taken from the ANCHOR
            // term alone. Analyze the anchor first, register the CTE with the
            // anchor's (column-list-renamed) columns, then analyze the whole UNION
            // body so the recursive term's self-reference resolves and the two
            // branches reconcile (a genuine arity/type mismatch is still reported
            // by analyze_setop). Without this the self-reference was a false
            // UnresolvedTable and each recursive column a false UnresolvedColumn.
            // The CTE is registered only once (with anchor columns); no push into
            // scope's CTE list happens during the body analysis, so the
            // self-reference's find_cte pointer stays valid.
            if (clause_recursive && body != nullptr && is_setop(body->node_type)) {
                ASTNode* anchor = first_child(body);
                ASTNode* rec_term = anchor != nullptr ? anchor->next_sibling : nullptr;
                // Derive the CTE's columns from the ANCHOR term alone. The whole
                // UNION body is analyzed again below (analyze_setop re-walks the
                // anchor and re-emits its diagnostics), so DISCARD the diagnostics
                // this pre-pass produces - otherwise every anchor-level diagnostic
                // is reported twice. Types recorded here are overwritten
                // idempotently by the body pass, so only the diagnostics leak.
                if (anchor != nullptr) {
                    const std::size_t diag_mark = diagnostics_.size();
                    cte.columns = analyze_stmt(anchor, &scope);
                    diagnostics_.resize(diag_mark);
                }
                if (col_list != nullptr) {
                    apply_column_aliases(cte.columns, col_list, def);
                }
                // Keep the CTE name and the anchor column types before the CTE is
                // moved into scope: the reconciled UNION body is validated against
                // the anchor types afterwards.
                const std::string cte_name = cte.name;
                std::vector<DataType> anchor_types;
                anchor_types.reserve(cte.columns.size());
                for (const auto& c : cte.columns) {
                    anchor_types.push_back(c.type);
                }
                // Non-linear recursion is illegal: the recursive term may
                // reference the CTE at most once (a self-join of the working table
                // has no defined fixpoint). Postgres: "recursive reference to
                // query "t" must not appear more than once".
                if (rec_term != nullptr &&
                    count_table_refs(rec_term, cte_name) > 1) {
                    add_diagnostic(DiagnosticCode::RecursiveReferenceNotLinear,
                                   "recursive reference to '" + cte_name +
                                       "' must not appear more than once",
                                   rec_term);
                }
                scope.add_cte(std::move(cte));
                // Analyze the whole UNION body: the self-reference resolves and
                // the branches reconcile. The reconciled column types are the
                // return value.
                const std::vector<ResolvedColumn> body_cols =
                    analyze_stmt(body, &scope);
                // The recursive term must be COERCIBLE to the anchor's column
                // types, not WIDEN them: SQL fixes the CTE's column types from the
                // anchor (non-recursive) term. If reconciling the recursive branch
                // produced a wider type for any column, reject it (Postgres:
                // `recursive query "t" column N has type X in non-recursive term
                // but type Y overall`). Without this the analyzer reported the
                // outer reference with the anchor type but projection_of(body)
                // with the wider type - a silently inconsistent result type.
                for (std::size_t i = 0;
                     i < anchor_types.size() && i < body_cols.size(); ++i) {
                    if (anchor_types[i] != DataType::Unknown &&
                        body_cols[i].type != DataType::Unknown &&
                        body_cols[i].type != anchor_types[i]) {
                        add_diagnostic(
                            DiagnosticCode::RecursiveTypeMismatch,
                            "recursive query '" + cte_name + "' column " +
                                std::to_string(i + 1) + " has type " +
                                ast::data_type_to_string(anchor_types[i]) +
                                " in the non-recursive term but type " +
                                ast::data_type_to_string(body_cols[i].type) +
                                " overall",
                            def);
                    }
                }
                continue;
            }

            if (body != nullptr) {
                cte.columns = analyze_stmt(body, &scope);
            }
            // Optional column-list rename `WITH t(a, b) AS (...)`: rename the CTE's
            // output columns positionally so a later `SELECT a FROM t` resolves.
            // Reuse apply_column_aliases (the derived-table path) so the CTE gets
            // the SAME arity check: naming MORE columns than the body projects is
            // illegal (Postgres: `WITH query "t" has N columns available but M
            // columns specified`). The bare rename loop here silently truncated the
            // extra aliases, so `WITH t(a,b,c) AS (SELECT id ...)` registered just
            // `a` and a later `SELECT c FROM t` failed with a misleading
            // UnresolvedColumn instead of the real ColumnAliasCountMismatch.
            if (col_list != nullptr) {
                apply_column_aliases(cte.columns, col_list, def);
            }
            scope.add_cte(std::move(cte));
        }
    }

    // 2. FROM: populate the scope with visible relations.
    if (ASTNode* from = find_child(select_stmt, NodeType::FromClause)) {
        resolve_from(from, scope);
    }

    // 3. SELECT list: resolve/ infer each projected item and build the output
    //    column list (used when this block is a derived table or CTE body).
    std::vector<ResolvedColumn> output;
    // The SELECT-list node that produced each output column, aligned 1:1 with
    // `output` (nullptr for `*`-expanded columns, which have no single source
    // expression). analyze_grouping uses this to re-mark expression columns that
    // read a ROLLUP/CUBE/GROUPING SETS key as nullable.
    std::vector<ASTNode*> output_sources;
    if (ASTNode* select_list = find_child(select_stmt, NodeType::SelectList)) {
        for (ASTNode* item = first_child(select_list); item != nullptr;
             item = item->next_sibling) {
            if (item->node_type == NodeType::Star) {
                // `*` / `table.*` -> concrete columns in FROM/JOIN order.
                expand_star(item, scope, output);
                output_sources.resize(output.size(), nullptr);
                continue;
            }
            const DataType type = infer_expr(item, scope);
            ResolvedColumn out;
            const std::string_view alias = alias_of(item);
            if (!alias.empty()) {
                out.name = std::string{alias};
            } else if (item->node_type == NodeType::ColumnRef) {
                out.name = std::string{split_column_ref(item->primary_text).column};
            } else {
                out.name = std::string{item->primary_text};
            }
            out.type = type;
            // Not-null only when inference proved it; otherwise conservatively
            // nullable. Flows into derived-table / CTE / set-op column lists.
            out.nullable = (null_of(item) != 1);
            // A direct column reference carries its resolved base-column identity
            // into the projection, matching what `expand_star` records for `*`.
            // (An expression / function result has no single base column, so the
            // ids stay 0.)
            if (item->node_type == NodeType::ColumnRef ||
                item->node_type == NodeType::Identifier) {
                out.table_id = item->context.analysis.table_id;
                out.column_id = item->context.analysis.column_id;
            }
            output.push_back(std::move(out));
            output_sources.push_back(item);
        }
    }

    // 4. WHERE / HAVING: infer the predicate expression (drives type checking
    //    and column resolution inside the predicate).
    if (ASTNode* where = find_child(select_stmt, NodeType::WhereClause)) {
        if (ASTNode* pred = first_child(where)) {
            infer_expr(pred, scope);
        }
    }
    // 5. GROUP BY: resolve each grouping expression against the FROM scope
    //    (emitting the usual unresolved diagnostics) before legality checking.
    ASTNode* group_by = find_child(select_stmt, NodeType::GroupByClause);
    if (group_by != nullptr) {
        ASTNode* group_select_list = find_child(select_stmt, NodeType::SelectList);
        for (ASTNode* key = first_child(group_by); key != nullptr; key = key->next_sibling) {
            // A GROUP BY key that names a SELECT-list output alias (and is not an
            // input column) resolves against the projection, not the FROM scope -
            // otherwise `SELECT id AS x FROM t GROUP BY x` falsely reports the
            // alias as an unresolved column. Type/nullability flow from the
            // aliased item (already inferred above).
            if (ASTNode* item = group_key_alias_item(key, group_select_list, scope)) {
                record_type(key, type_of(item));
                record_nullability(key, null_of(item));
                continue;
            }
            infer_expr(key, scope);
        }
    }

    // 6. HAVING: resolve/infer the predicate (column refs resolve here too).
    if (ASTNode* having = find_child(select_stmt, NodeType::HavingClause)) {
        if (ASTNode* pred = first_child(having)) {
            infer_expr(pred, scope);
        }
    }

    // 7. ORDER BY: resolve each sort expression first against the SELECT-list
    //    output names/aliases (SQL lets ORDER BY reference an output column by
    //    its alias), then, failing that, against the FROM scope.
    if (ASTNode* order_by = find_child(select_stmt, NodeType::OrderByClause)) {
        for (ASTNode* item = first_child(order_by); item != nullptr; item = item->next_sibling) {
            const ResolvedColumn* out_match = nullptr;
            if (is_column_ref_node(item->node_type)) {
                const QualifiedRef qref = split_column_ref(item->primary_text);
                // Only an UNQUALIFIED order-by ref may name a SELECT output column
                // by its alias/name. A qualified `t.col` always names a base/input
                // column - never an output alias - so it must resolve against the
                // FROM scope (this mirrors the grouping-legality exemption below).
                // Matching a qualified ref to an output column by bare name skipped
                // infer_expr, leaving table_id/column_id 0: a legal `GROUP BY dept
                // ... ORDER BY emp.dept` was then falsely NonGroupedColumn (the key
                // match fell back to text and failed), and `SELECT sal AS id ...
                // ORDER BY e.id` took the output column's type (Double) instead of
                // base emp.id (Integer).
                if (qref.qualifier.empty()) {
                    for (const auto& col : output) {
                        if (iequals(col.name, qref.column)) {
                            out_match = &col;
                            break;
                        }
                    }
                }
            }
            if (out_match != nullptr) {
                // Resolved against the projection; type flows from the output
                // column and no FROM-scope lookup (or diagnostic) is needed.
                record_type(item, out_match->type);
                record_nullability(item, out_match->nullable ? 2 : 1);
            } else {
                infer_expr(item, scope);
            }
        }
    }

    // 8. LIMIT / OFFSET: literal operands must be non-negative integers.
    if (ASTNode* limit = find_child(select_stmt, NodeType::LimitClause)) {
        check_limit(limit);
    }

    // 9. Aggregate / GROUP BY / HAVING legality (validation pass).
    analyze_grouping(select_stmt, group_by, scope, output, output_sources);

    projections_[select_stmt] = output;
    return output;
}

void Analyzer::expand_star(ASTNode* star, Scope& scope,
                           std::vector<ResolvedColumn>& output) {
    // The parser emits a qualified star `table.*` as a Star node whose
    // schema_name holds the qualifier; an unqualified `*` has an empty
    // schema_name. The qualified branch below expands to exactly that relation's
    // columns, and reports UnresolvedQualifier when the qualifier names no
    // visible relation. See docs/DESIGN.md.
    const std::string_view qualifier = alias_of(star);

    if (qualifier.empty()) {
        if (scope.relations().empty()) {
            add_diagnostic(DiagnosticCode::StarWithoutFrom,
                           "'SELECT *' requires a FROM clause", star);
            return;
        }
        for (const auto& rel : scope.relations()) {
            for (const auto& col : rel.columns) {
                // A USING / NATURAL join coalesces a shared column to ONE output
                // column: the right-hand copy is marked coalesced, so `SELECT *`
                // emits it once (the surviving left copy). The flag is a property
                // of this join's scope, so clear it on the projected column - the
                // result is a fresh relation where the column is unqualified-plain.
                if (col.coalesced) {
                    continue;
                }
                ResolvedColumn out = col;
                out.nullable = out.nullable || rel.nullable_from_join;
                out.coalesced = false;
                output.push_back(std::move(out));
            }
        }
        return;
    }

    bool matched = false;
    for (const auto& rel : scope.relations()) {
        if (rel.matches_qualifier(qualifier)) {
            matched = true;
            // A qualified `t.*` is table-scoped: it lists every column of `t`,
            // including a coalesced shared column (still addressable as `t.col`).
            for (const auto& col : rel.columns) {
                ResolvedColumn out = col;
                out.nullable = out.nullable || rel.nullable_from_join;
                out.coalesced = false;  // the projection is a fresh relation
                output.push_back(std::move(out));
            }
        }
    }
    if (!matched) {
        add_diagnostic(DiagnosticCode::UnresolvedQualifier,
                       "unresolved table qualifier '" + std::string{qualifier} +
                           "' in '" + std::string{qualifier} + ".*'",
                       star);
    }
}

std::vector<ResolvedColumn> Analyzer::analyze_setop(ASTNode* setop, Scope* parent) {
    // Analyze every branch (each a SELECT block or a nested set operation) and
    // collect its projected columns.
    std::vector<std::vector<ResolvedColumn>> branches;
    for (ASTNode* child = first_child(setop); child != nullptr;
         child = child->next_sibling) {
        if (child->node_type == NodeType::SelectStmt || is_setop(child->node_type)) {
            branches.push_back(analyze_stmt(child, parent));
        }
    }

    if (branches.empty()) {
        return {};
    }

    // Fold the branches left-to-right, reconciling arity and types pairwise.
    std::vector<ResolvedColumn> result = branches.front();
    for (std::size_t i = 1; i < branches.size(); ++i) {
        const auto& branch = branches[i];
        if (branch.size() != result.size()) {
            add_diagnostic(DiagnosticCode::SetOpArityMismatch,
                           "set operation branches have different column counts (" +
                               std::to_string(result.size()) + " vs " +
                               std::to_string(branch.size()) + ")",
                           setop);
            continue;  // arity differs: pairwise type check is meaningless
        }
        for (std::size_t j = 0; j < result.size(); ++j) {
            const Coercion r =
                coerce(result[j].type, branch[j].type, CoercionKind::UnionReconcile);
            if (r.status == CoercionStatus::Incompatible) {
                add_diagnostic(DiagnosticCode::SetOpTypeMismatch,
                               "incompatible types in set operation for output column " +
                                   std::to_string(j + 1),
                               setop);
            }
            result[j].type = r.type;      // reconciled output type
            result[j].nullable = result[j].nullable || branch[j].nullable;
        }
    }

    projections_[setop] = result;
    return result;
}

// A FROM item begins a new comma-separated table_reference when it is a
// relation that is not itself a JOIN node: a bare table, a derived table /
// subquery, or a parenthesized join group. A JOIN node instead EXTENDS the
// current table_reference (its left operand is that group).
[[nodiscard]] bool starts_comma_group(NodeType t) {
    switch (t) {
        case NodeType::TableRef:
        case NodeType::Subquery:
        case NodeType::SubqueryExpr:
        case NodeType::FromClause:
            return true;
        default:
            return false;
    }
}

void Analyzer::resolve_from(ASTNode* from_clause, Scope& scope) {
    std::size_t comma_group_start = scope.relations().size();
    for (ASTNode* item = first_child(from_clause); item != nullptr; item = item->next_sibling) {
        // A new comma item resets the group start to the relations it is about to
        // add; a JOIN keeps the current group start so its left operand spans the
        // whole preceding table_reference, not a comma-joined sibling.
        if (starts_comma_group(item->node_type)) {
            comma_group_start = scope.relations().size();
        }
        resolve_from_item(item, scope, comma_group_start);
    }

    // Every relation brought into scope must have a distinct correlation name:
    // the alias when one is given, otherwise the base-table name (this is what a
    // qualified reference addresses). A repeated one is ambiguous - the same
    // qualifier would name two relations - and SQL rejects it ("table name/alias
    // specified more than once"). Unnamed relations (empty qualifier) are skipped.
    std::vector<std::string_view> seen;
    for (const auto& rel : scope.relations()) {
        const std::string_view qualifier = rel.alias.empty() ? rel.name : rel.alias;
        if (qualifier.empty()) {
            continue;
        }
        bool duplicate = false;
        for (const std::string_view prior : seen) {
            if (iequals(prior, qualifier)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            add_diagnostic(DiagnosticCode::DuplicateRelation,
                           "table name or alias '" + std::string{qualifier} +
                               "' specified more than once in FROM",
                           from_clause);
        } else {
            seen.push_back(qualifier);
        }
    }
}

std::vector<ResolvedColumn> Analyzer::columns_from_values(ASTNode* values_stmt,
                                                          Scope& scope) {
    // ValuesStmt -> ValuesClause -> one row per child, each row a list of value
    // expressions. The relation's columns are the first row's positions; their
    // types are inferred from that row (VALUES columns are nullable). Names are
    // empty here - a column-alias list supplies them.
    std::vector<ResolvedColumn> cols;
    ASTNode* clause = find_child(values_stmt, NodeType::ValuesClause);
    ASTNode* first_row = clause != nullptr ? first_child(clause) : nullptr;
    if (first_row == nullptr) {
        return cols;
    }
    for (ASTNode* v = first_child(first_row); v != nullptr; v = v->next_sibling) {
        const DataType t = infer_expr(v, scope);
        cols.push_back(ResolvedColumn{std::string{}, t, /*nullable=*/true, 0, 0});
    }
    // Every subsequent row must supply the same number of values as the first
    // (which sets the relation's width). A ragged VALUES list - accepted before,
    // as a derived table - would leave a row narrower/wider than the schema, a
    // malformed relation. INSERT already rejects this shape; do the same here.
    //
    // A multi-row VALUES is a UNION ALL of its rows, so each column's type is
    // reconciled ACROSS every row (exactly as analyze_setop folds set-op
    // branches), not taken from the first row alone. Without this a legal
    // `(VALUES (1), (2.5))` was mis-typed Integer instead of numeric, and an
    // illegal `(VALUES (1), ('x'))` was silently accepted. Reconcile each
    // in-range position and flag an incompatible column; the last row's width
    // still drives the separate arity check.
    for (ASTNode* row = first_row->next_sibling; row != nullptr; row = row->next_sibling) {
        std::size_t width = 0;
        for (ASTNode* v = first_child(row); v != nullptr; v = v->next_sibling, ++width) {
            const DataType t = infer_expr(v, scope);
            if (width < cols.size()) {
                const Coercion r =
                    coerce(cols[width].type, t, CoercionKind::UnionReconcile);
                if (r.status == CoercionStatus::Incompatible) {
                    add_diagnostic(DiagnosticCode::ValuesColumnTypeMismatch,
                                   "incompatible types in VALUES for column " +
                                       std::to_string(width + 1),
                                   v);
                }
                cols[width].type = r.type;  // reconciled column type
            }
        }
        if (width != cols.size()) {
            add_diagnostic(DiagnosticCode::ValuesRowArityMismatch,
                           "VALUES row has " + std::to_string(width) +
                               " values but the first row has " +
                               std::to_string(cols.size()),
                           row);
        }
    }
    return cols;
}

void Analyzer::apply_column_aliases(std::vector<ResolvedColumn>& cols,
                                    ASTNode* alias_list, ASTNode* at) {
    std::vector<std::string_view> names;
    for (ASTNode* id = first_child(alias_list); id != nullptr; id = id->next_sibling) {
        names.push_back(id->primary_text);
    }
    // A relation (derived table or CTE) may not be given more column aliases than
    // it has columns (fewer is allowed - the trailing columns keep their inferred
    // names).
    if (names.size() > cols.size()) {
        add_diagnostic(DiagnosticCode::ColumnAliasCountMismatch,
                       "relation has " + std::to_string(cols.size()) +
                           " columns but " + std::to_string(names.size()) +
                           " column aliases were specified",
                       at);
    }
    for (std::size_t i = 0; i < names.size() && i < cols.size(); ++i) {
        cols[i].name = std::string{names[i]};
    }
}

void Analyzer::resolve_from_item(ASTNode* item, Scope& scope,
                                 std::size_t comma_group_start) {
    if (item == nullptr) {
        return;
    }
    switch (item->node_type) {
        case NodeType::TableRef: {
            const std::string_view name = item->primary_text;
            const std::string_view alias = alias_of(item);

            RelationBinding binding;
            binding.name = std::string{name};
            binding.alias = std::string{alias};

            // A FROM name may refer to a CTE before a base table.
            if (const NamedRelation* cte = scope.find_cte(name)) {
                binding.columns = cte->columns;
                scope.add_relation(std::move(binding));
                return;
            }
            if (const TableInfo* table = catalog_.find_table(name)) {
                binding.table_id = table->table_id;
                for (const auto& c : table->columns) {
                    binding.columns.push_back(
                        ResolvedColumn{c.name, c.type, c.nullable, table->table_id, c.column_id});
                }
                scope.add_relation(std::move(binding));
                return;
            }
            add_diagnostic(DiagnosticCode::UnresolvedTable,
                           "unresolved table '" + std::string{name} + "'", item);
            return;
        }
        case NodeType::Subquery:
        case NodeType::SubqueryExpr: {
            const std::string_view alias = alias_of(item);
            RelationBinding binding;
            binding.name = std::string{alias};
            binding.alias = std::string{alias};
            // The derived-table body is a query: a SELECT, a set operation
            // (UNION / INTERSECT / EXCEPT), or a VALUES list. Dispatch a SELECT or
            // set-op body through analyze_stmt (which reconciles set-op branch
            // columns) - previously only a direct SelectStmt child was handled, so
            // a set-op body registered ZERO columns and every reference to the
            // derived relation was falsely UnresolvedColumn (the CTE path already
            // accepts a set-op body; this is the derived-table sibling of that).
            ASTNode* query_body = nullptr;
            for (ASTNode* c = first_child(item); c != nullptr; c = c->next_sibling) {
                if (c->node_type == NodeType::SelectStmt || is_setop(c->node_type)) {
                    query_body = c;
                    break;
                }
            }
            if (query_body != nullptr) {
                binding.columns = analyze_stmt(query_body, &scope);
            } else if (ASTNode* values = find_child(item, NodeType::ValuesStmt)) {
                // A VALUES list used as a derived table: its columns are the
                // per-position value types; they are anonymous until named by the
                // column-alias list below.
                binding.columns = columns_from_values(values, scope);
            }
            // Column-alias list "(a, b)" on the derived table renames its output
            // columns positionally (the parser attaches it as a ColumnList child;
            // the row lists of a VALUES body sit under ValuesStmt, so find_child -
            // direct children only - never confuses them).
            if (ASTNode* col_aliases = find_child(item, NodeType::ColumnList)) {
                apply_column_aliases(binding.columns, col_aliases, item);
            }
            scope.add_relation(std::move(binding));
            return;
        }
        // A parenthesized join group `( a JOIN b ... )` in table-reference
        // position: the parser represents it as a nested FromClause. Resolve its
        // items into the SAME scope so every relation of the group is visible to
        // any outer join written over it (its inner join predicates resolve as
        // they are visited, just like a top-level FROM).
        case NodeType::FromClause: {
            // A parenthesized join group is its own comma/JOIN sequence; track the
            // group start within it exactly as the top-level FROM does, based at
            // the relations this group introduces.
            std::size_t inner_group_start = scope.relations().size();
            for (ASTNode* child = first_child(item); child != nullptr;
                 child = child->next_sibling) {
                if (starts_comma_group(child->node_type)) {
                    inner_group_start = scope.relations().size();
                }
                resolve_from_item(child, scope, inner_group_start);
            }
            return;
        }
        // Joins: bring every relation on the right side into scope, then
        // resolve the join predicate now that both sides are visible. The left
        // side is the FROM-clause sibling processed before this join, so it is
        // already in scope.
        case NodeType::JoinClause:
        case NodeType::InnerJoin:
        case NodeType::LeftJoin:
        case NodeType::RightJoin:
        case NodeType::FullJoin:
        case NodeType::CrossJoin:
        case NodeType::LateralJoin: {
            const std::size_t left_end = scope.relations().size();
            // Pass 1: make the right-hand relation(s) visible.
            for (ASTNode* child = first_child(item); child != nullptr;
                 child = child->next_sibling) {
                if (is_relation_node(child->node_type)) {
                    resolve_from_item(child, scope);
                }
            }
            const std::size_t right_end = scope.relations().size();
            // Outer-join nullability: the null-supplying side's columns become
            // nullable regardless of their base NOT NULL constraint. The left
            // side is this join's own left operand - the current comma-separated
            // table_reference, [comma_group_start, left_end) - NOT every relation
            // in scope: comma binds looser than JOIN, so `A, B RIGHT JOIN C` is
            // `A CROSS (B RIGHT JOIN C)` and A must keep its base nullability. The
            // right side is the relations just added ([left_end, right_end)).
            // Marked before predicates / SELECT resolve.
            switch (join_null_side(item)) {
                case JoinNullSide::Right:
                    scope.mark_join_nullable(left_end, right_end);
                    break;
                case JoinNullSide::Left:
                    scope.mark_join_nullable(comma_group_start, left_end);
                    break;
                case JoinNullSide::Both:
                    scope.mark_join_nullable(comma_group_start, right_end);
                    break;
                case JoinNullSide::None:
                    break;
            }
            // Pass 2: resolve the ON expression / USING columns against the now
            // fully-populated scope.
            for (ASTNode* child = first_child(item); child != nullptr;
                 child = child->next_sibling) {
                if (child->node_type == NodeType::UsingClause) {
                    resolve_using(child, scope, left_end, right_end);
                } else if (!is_relation_node(child->node_type)) {
                    // The ON predicate: a normal expression whose column refs
                    // resolve (and emit diagnostics) through infer_expr.
                    infer_expr(child, scope);
                }
            }
            // A NATURAL join (recorded in the JoinClause primary_text) has no ON
            // or USING clause; coalesce its common columns so bare references to
            // them resolve unambiguously.
            if (to_upper(item->primary_text).find("NATURAL") != std::string::npos) {
                resolve_natural(scope, left_end, right_end);
            }
            return;
        }
        default:
            // Not a relation-producing node (e.g. a join predicate); ignore.
            return;
    }
}

void Analyzer::resolve_using(ASTNode* using_clause, Scope& scope,
                             std::size_t left_end, std::size_t right_end) {
    const auto& relations = scope.relations();
    for (ASTNode* col = first_child(using_clause); col != nullptr;
         col = col->next_sibling) {
        // A USING item names a bare column; primary_text is the column name.
        const std::string_view name = split_column_ref(col->primary_text).column;

        const ResolvedColumn* left_hit = nullptr;
        for (std::size_t i = 0; i < left_end && i < relations.size(); ++i) {
            if (const auto* c = relations[i].find_column(name)) {
                left_hit = c;
            }
        }
        bool right_found = false;
        for (std::size_t i = left_end; i < right_end && i < relations.size(); ++i) {
            if (relations[i].find_column(name) != nullptr) {
                right_found = true;
            }
        }

        if (left_hit == nullptr || !right_found) {
            add_diagnostic(DiagnosticCode::UsingColumnMissing,
                           "USING column '" + std::string{name} +
                               "' is not present in both joined relations",
                           col);
            continue;
        }
        // A USING column collapses to a single merged output column; record its
        // resolved type on the node so downstream consumers see it, and coalesce
        // the right-hand copy so a bare reference is not ambiguous. The bare
        // merged column keeps the LEFT copy's (table_id, column_id): the binder's
        // input contract - it builds the visible COALESCE(left, right) column
        // with the left identity and routes a bare reference to it by that id, and
        // computes the merged column's true value/nullability itself. (An RIGHT/
        // FULL merged column has no single base column; the left identity is a
        // stable routing key, not a value claim.)
        record_type(col, left_hit->type);
        col->context.analysis.table_id = left_hit->table_id;
        col->context.analysis.column_id = left_hit->column_id;
        scope.mark_column_coalesced(left_end, right_end, name);
    }
}

// A NATURAL join is USING over every column common to both inputs: coalesce each
// column that appears in both the left ([0, left_end)) and right
// ([left_end, right_end)) relations, so a bare reference to it resolves to the
// single left copy rather than reporting ambiguity. (An out-of-scope subtlety:
// a common column that is itself ambiguous on the left stays ambiguous - the
// left copy is not coalesced, so resolve_bare still sees more than one.)
void Analyzer::resolve_natural(Scope& scope, std::size_t left_end,
                               std::size_t right_end) {
    const auto& relations = scope.relations();
    for (std::size_t li = 0; li < left_end && li < relations.size(); ++li) {
        for (const auto& lc : relations[li].columns) {
            bool in_right = false;
            for (std::size_t ri = left_end; ri < right_end && ri < relations.size(); ++ri) {
                if (relations[ri].find_column(lc.name) != nullptr) {
                    in_right = true;
                    break;
                }
            }
            if (in_right) {
                scope.mark_column_coalesced(left_end, right_end, lc.name);
            }
        }
    }
}

ResolvedColumn Analyzer::resolve_column_ref(ASTNode* col_ref, Scope& scope) {
    const QualifiedRef qref = split_column_ref(col_ref->primary_text);

    ColumnResolution res;
    if (!qref.qualifier.empty()) {
        res = scope.resolve_qualified(qref.qualifier, qref.column);
    } else {
        res = scope.resolve_bare(qref.column);
    }

    if (res.found) {
        // Record resolution onto the parser node's analysis context.
        record_type(col_ref, res.column.type);
        col_ref->context.analysis.table_id = res.column.table_id;
        col_ref->context.analysis.column_id = res.column.column_id;
        // Nullability comes from the catalog column, already adjusted for the
        // null-supplying side of an outer join by the scope resolver.
        record_nullability(col_ref, res.column.nullable ? 2 : 1);
        // A reference that resolved in an enclosing scope makes correlated EVERY
        // active subquery that sits between this reference's block and the scope
        // that owns the matched relation (no diagnostic: correlation is legal).
        // An intermediate subquery in a multiply-nested chain cannot be evaluated
        // independently of the owning row either, so it must be flagged too - not
        // only the innermost. A subquery qualifies when the owning scope is at or
        // outside its enclosing scope (i.e. the owning scope is an ancestor of,
        // or equal to, that subquery's enclosing scope).
        if (res.from_outer && res.owner != nullptr) {
            for (const auto& [enclosing, flag] : corr_stack_) {
                for (const Scope* s = enclosing; s != nullptr; s = s->parent()) {
                    if (s == res.owner) {
                        *flag = true;
                        break;
                    }
                }
            }
        }
        return res.column;
    }

    if (res.ambiguous) {
        add_diagnostic(DiagnosticCode::AmbiguousColumn,
                       "ambiguous column '" + std::string{qref.column} + "'", col_ref);
    } else if (res.bad_qualifier) {
        add_diagnostic(DiagnosticCode::UnresolvedQualifier,
                       "unresolved table alias '" + std::string{qref.qualifier} + "'", col_ref);
    } else {
        add_diagnostic(DiagnosticCode::UnresolvedColumn,
                       "unresolved column '" + std::string{col_ref->primary_text} + "'", col_ref);
    }
    return ResolvedColumn{};
}

namespace {
// Maximum expression-tree recursion depth. SQL operator chains (a AND b AND ...,
// a+b+c+..., etc.) parse to left-deep trees the parser does not bound, so a long
// chain would overflow the stack during analysis. Real expressions nest only a
// few levels; this cap is far above any genuine query yet well below the stack
// limit. Shared by infer_expr, check_grouping_expr, and contains_aggregate.
constexpr int kMaxExprDepth = 1000;

// RAII: increment a depth counter for the current recursion level, decrement on
// any exit (including the many early returns in infer_expr).
struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& d) noexcept : depth(d) { ++depth; }
    ~DepthGuard() { --depth; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
};
}  // namespace

DataType Analyzer::infer_expr(ASTNode* expr, Scope& scope) {
    if (expr == nullptr) {
        return DataType::Unknown;
    }
    if (expr_depth_ >= kMaxExprDepth) {
        // Over-deep subtree: abandon analysis of it rather than overflow the
        // stack. Report once so a pathological expression yields a diagnostic
        // instead of a crash or a flood of messages.
        if (!expr_depth_reported_) {
            expr_depth_reported_ = true;
            add_diagnostic(DiagnosticCode::ExpressionTooComplex,
                           "expression nesting is too deep to analyze", expr);
        }
        return DataType::Unknown;
    }
    const DepthGuard depth_guard{expr_depth_};

    switch (expr->node_type) {
        // A non-NULL literal is, by construction, not-null; a NULL literal is
        // nullable.
        case NodeType::IntegerLiteral: {
            // Type an integer literal by its MAGNITUDE, matching PostgreSQL's
            // int4 -> int8 -> numeric widening: Integer if it fits signed 32-bit,
            // BigInt if it fits signed 64-bit, else Decimal (numeric). Always
            // not-null. The narrowest type that holds the literal keeps a reconciled
            // set-op / VALUES column (and any downstream plan slot) wide enough for
            // the value; typing every literal Integer silently narrowed a bigint
            // constant to 32 bits. The magnitude is read from the literal's decimal
            // text; a non-decimal spelling (hex / binary) or unparseable text
            // conservatively stays Integer. `promote_numeric` ranks
            // Integer < BigInt < Decimal, so a wider type reconciles cleanly.
            //
            // The parser folds a leading sign into the literal's text (a negative
            // literal is a single IntegerLiteral "-3000000000", not unary-minus of
            // a positive), so strip an optional +/- before reading the magnitude:
            // from_chars into an unsigned type rejects the sign and would otherwise
            // leave every out-of-int32 negative typed Integer. A negative value
            // reaches one further at each width (two's complement: INT32_MIN is
            // -2147483648, INT64_MIN is -2^63), so the bounds are sign-aware.
            DataType t = DataType::Integer;
            std::string_view txt = expr->primary_text;
            bool negative = false;
            if (!txt.empty() && (txt.front() == '-' || txt.front() == '+')) {
                negative = (txt.front() == '-');
                txt.remove_prefix(1);
            }
            unsigned long long mag = 0;
            const char* const end = txt.data() + txt.size();
            const auto [ptr, ec] = std::from_chars(txt.data(), end, mag);
            // `ptr == end` (with a non-empty digit run) means the WHOLE text after
            // the sign was decimal digits (so a hex / binary `0x..` / `0b..`
            // spelling, whose parse stops at the first non-digit, conservatively
            // stays Integer).
            if (ptr == end && !txt.empty()) {
                // Largest magnitude each signed width holds: negatives reach one
                // further than positives (|INT_MIN| = INT_MAX + 1).
                const unsigned long long int32_max = negative ? 2147483648ULL
                                                              : 2147483647ULL;
                const unsigned long long int64_max = negative ? 9223372036854775808ULL
                                                              : 9223372036854775807ULL;
                if (ec == std::errc::result_out_of_range) {
                    t = DataType::Decimal;      // more digits than fit in uint64
                } else if (ec == std::errc{}) {
                    if (mag > int64_max) {
                        t = DataType::Decimal;  // beyond signed int64
                    } else if (mag > int32_max) {
                        t = DataType::BigInt;   // beyond signed int32, within int64
                    }
                }
            }
            record_type(expr, t);
            record_nullability(expr, 1);
            return t;
        }
        case NodeType::FloatLiteral:
            record_type(expr, DataType::Double);
            record_nullability(expr, 1);
            return DataType::Double;
        case NodeType::StringLiteral:
            record_type(expr, DataType::Text);
            record_nullability(expr, 1);
            return DataType::Text;
        case NodeType::BooleanLiteral:
            record_type(expr, DataType::Boolean);
            record_nullability(expr, 1);
            return DataType::Boolean;
        case NodeType::NullLiteral:
            record_type(expr, DataType::Null);
            record_nullability(expr, 2);
            return DataType::Null;

        case NodeType::ColumnRef:
        // A bare column that is the direct argument of a function call is emitted
        // as an Identifier; resolve it the same way as a ColumnRef.
        case NodeType::Identifier:
            // resolve_column_ref records both the type and the nullability.
            return resolve_column_ref(expr, scope).type;

        case NodeType::FunctionCall:
        case NodeType::FunctionExpr: {
            // Infer argument types (recursion also resolves nested column refs).
            // A COUNT(*) star argument contributes no type and is skipped. A
            // WindowSpec child is the OVER clause, not an argument: it is set
            // aside and resolved separately so its PARTITION BY / ORDER BY
            // columns bind, without being counted as a function argument.
            std::vector<DataType> arg_types;
            std::vector<int> arg_nulls;
            ASTNode* window_spec = nullptr;
            const std::string upper = to_upper(expr->primary_text);
            // EXTRACT(field FROM ts) / DATE_PART(field, ts): the leading field is
            // a date-part keyword (YEAR, MONTH, DAY, ...) that the parser emits as
            // a bare Identifier. It is NOT a column reference, so skip resolving it
            // - otherwise it is wrongly reported as an unresolved column and the
            // whole EXTRACT call fails to analyze.
            const bool datepart_head = (upper == "EXTRACT" || upper == "DATE_PART");
            bool first_arg = true;
            for (ASTNode* arg = first_child(expr); arg != nullptr; arg = arg->next_sibling) {
                if (arg->node_type == NodeType::WindowSpec) {
                    window_spec = arg;
                    continue;
                }
                if (arg->node_type == NodeType::WhereClause) {
                    // The FILTER (WHERE p) clause of an aggregate. Resolve the
                    // predicate so its column references bind, but it is not a
                    // function argument and does not contribute to arg types.
                    infer_expr(first_child(arg), scope);
                    continue;
                }
                if (datepart_head && first_arg &&
                    (arg->node_type == NodeType::Identifier ||
                     arg->node_type == NodeType::ColumnRef)) {
                    first_arg = false;
                    record_type(arg, DataType::Unknown);  // keyword, not a value
                    continue;
                }
                first_arg = false;
                const DataType t = infer_expr(arg, scope);
                if (arg->node_type != NodeType::Star) {
                    arg_types.push_back(t);
                    arg_nulls.push_back(null_of(arg));
                }
            }
            if (window_spec != nullptr) {
                // Resolve the OVER clause's partition/order expressions (the
                // default recursion binds their column references) and type the
                // call with window-aware rules.
                infer_expr(window_spec, scope);
                const FunctionType wf = window_function_result_type(upper, arg_types);
                if (!wf.known) {
                    add_diagnostic(DiagnosticCode::UnknownFunction,
                                   "unknown window function '" +
                                       std::string{expr->primary_text} +
                                       "'; result type is Unknown",
                                   expr, Severity::Warning);
                }
                record_type(expr, wf.type);
                record_nullability(expr, window_function_nullability(upper, arg_nulls));
                return wf.type;
            }
            const FunctionType ft = function_result_type(upper, arg_types);
            if (!ft.known) {
                add_diagnostic(DiagnosticCode::UnknownFunction,
                               "unknown function '" + std::string{expr->primary_text} +
                                   "'; result type is Unknown",
                               expr, Severity::Warning);
            }
            if (ft.arg_type_mismatch) {
                add_diagnostic(DiagnosticCode::TypeMismatch,
                               "incompatible argument types to " + upper +
                                   "(): the arguments cannot be reconciled to a "
                                   "common type",
                               expr);
            }
            // NULLIF(a, b) is defined as `CASE WHEN a = b THEN NULL ELSE a END`,
            // i.e. a comparison; a cross-category operand pair is the same soft
            // implicit coercion the other comparison sites (=, IN, BETWEEN, IS
            // [NOT] DISTINCT FROM) already warn on. (Postgres rejects it outright;
            // the project models these as warnings.)
            if (upper == "NULLIF" && arg_types.size() == 2 &&
                coerce(arg_types[0], arg_types[1], CoercionKind::Comparison).status !=
                    CoercionStatus::Ok) {
                add_diagnostic(DiagnosticCode::ImplicitCoercion,
                               "implicit coercion in NULLIF between operands of "
                               "different type categories",
                               expr, Severity::Warning);
            }
            record_type(expr, ft.type);
            record_nullability(expr, function_nullability(upper, arg_nulls));
            return ft.type;
        }

        case NodeType::BinaryExpr: {
            ASTNode* lhs = first_child(expr);
            ASTNode* rhs = lhs != nullptr ? lhs->next_sibling : nullptr;
            const DataType lt = infer_expr(lhs, scope);
            const DataType rt = infer_expr(rhs, scope);
            const std::string_view op = expr->primary_text;

            DataType result = DataType::Unknown;
            if (is_comparison_op(op)) {
                // Comparisons yield Boolean. A cross-category comparison (e.g.
                // text vs integer) is permitted but flagged as a soft implicit
                // coercion; numeric-vs-numeric and string-vs-string are clean.
                const Coercion c = coerce(lt, rt, CoercionKind::Comparison);
                if (c.status != CoercionStatus::Ok) {
                    add_diagnostic(DiagnosticCode::ImplicitCoercion,
                                   "implicit coercion in comparison between operands of "
                                   "different type categories",
                                   expr, Severity::Warning);
                }
                result = DataType::Boolean;
            } else if (is_null_safe_comparison_op(op)) {
                // IS [NOT] DISTINCT FROM: same operand-compatibility rules as a
                // plain comparison (cross-category is a soft coercion), but the
                // result is a plain boolean that is never NULL (handled below).
                const Coercion c = coerce(lt, rt, CoercionKind::Comparison);
                if (c.status != CoercionStatus::Ok) {
                    add_diagnostic(DiagnosticCode::ImplicitCoercion,
                                   "implicit coercion in IS [NOT] DISTINCT FROM between "
                                   "operands of different type categories",
                                   expr, Severity::Warning);
                }
                result = DataType::Boolean;
            } else if (is_quantified_comparison_op(op)) {
                // `x <cmp> ALL|ANY|SOME (subquery|array)` is boolean. The right
                // operand was already analyzed above: a single-column subquery
                // yields its column type via the scalar-subquery path (which also
                // flags a multi-column subquery - ALL/ANY require one column); an
                // ARRAY / array constructor yields Array/Unknown. Apply the same
                // cross-category coercion warning as a plain comparison when the
                // right side has a concrete scalar type, and skip it for an
                // array/unknown RHS where there is no single element type to
                // compare against. Typing this Boolean (it was left Unknown with
                // NO diagnostic) stops the analyzer handing the binder an
                // analyze-clean but mis-typed predicate.
                if (rt != DataType::Array && rt != DataType::Unknown &&
                    lt != DataType::Unknown) {
                    const Coercion c = coerce(lt, rt, CoercionKind::Comparison);
                    if (c.status != CoercionStatus::Ok) {
                        add_diagnostic(DiagnosticCode::ImplicitCoercion,
                                       "implicit coercion in quantified comparison "
                                       "between operands of different type categories",
                                       expr, Severity::Warning);
                    }
                }
                result = DataType::Boolean;
            } else if (is_logical_op(op)) {
                result = DataType::Boolean;
            } else if (op == "||") {
                // `||` is overloaded: ARRAY CONCATENATION when either operand is
                // an array (array||array, element||array, array||element all yield
                // an array), otherwise STRING concatenation (operands rendered to
                // their text form -> text). Without the array case, `ARRAY[...] ||
                // x::text[]` was typed Text and then failed to reconcile with a
                // sibling array - e.g. a CASE whose other branch is an ARRAY[...] -
                // producing a spurious "incompatible CASE result types" error.
                // Nullability is combined below like any other binary operator.
                result = (lt == DataType::Array || rt == DataType::Array)
                             ? DataType::Array
                             : DataType::Text;
            } else if (is_arithmetic_op(op)) {
                // Temporal arithmetic (date/time ± interval, temporal − temporal,
                // date ± integer) is operator-aware and handled first; a genuinely
                // invalid temporal mix is a hard type mismatch.
                const TemporalArith ta = temporal_arith(lt, rt, op);
                if (ta.status == TemporalArithStatus::Ok) {
                    result = ta.type;
                } else if (ta.status == TemporalArithStatus::Invalid) {
                    add_diagnostic(DiagnosticCode::TypeMismatch,
                                   "incompatible operand types for arithmetic operator '" +
                                       std::string{op} + "'",
                                   expr);
                    result = DataType::Unknown;
                } else {
                    // Plain numeric arithmetic requires numerically-compatible
                    // operands; otherwise it is a hard type mismatch (e.g.
                    // text + integer).
                    const Coercion c = coerce(lt, rt, CoercionKind::Arithmetic);
                    if (c.status == CoercionStatus::Incompatible) {
                        add_diagnostic(DiagnosticCode::TypeMismatch,
                                       "incompatible operand types for arithmetic operator '" +
                                           std::string{op} + "'",
                                       expr);
                    }
                    result = c.type;
                }
                // A constant division / modulo by a zero divisor is a soft
                // WARNING, not an error: DB25 deliberately treats `1/0` as a legal
                // runtime operation that flows through the pipeline (the optimizer
                // PRESERVES it rather than folding it, and the harness relies on it
                // analyzing clean) - a divergence from PostgreSQL, which folds and
                // rejects at plan time. Suppressed inside a provably-dead CASE arm
                // (see the CaseExpr handler), so `CASE WHEN 1=0 THEN 1/0 ...` is
                // silent while a reachable `1/0` gets the advisory warning.
                if (div0_suppress_ == 0 && (op == "/" || op == "%")) {
                    if (const auto d = eval_const_int(rhs); d.has_value() && *d == 0) {
                        add_diagnostic(DiagnosticCode::DivisionByZero,
                                       std::string{"division by zero in constant '"} +
                                           std::string{op} + "' expression",
                                       expr, Severity::Warning);
                    }
                }
            }
            record_type(expr, result);
            // A binary result is nullable if either operand can be NULL - except a
            // null-safe comparison (IS [NOT] DISTINCT FROM), which is never NULL.
            record_nullability(expr, is_null_safe_comparison_op(op)
                                         ? 1
                                         : combine_nullable_any({null_of(lhs), null_of(rhs)}));
            return result;
        }

        case NodeType::UnaryExpr: {
            const std::string_view op = expr->primary_text;
            const std::string upper = to_upper(op);
            // EXISTS (subquery) / NOT EXISTS (subquery): always Boolean and never
            // NULL; arity of the subquery is irrelevant, correlation is allowed.
            if (upper == "EXISTS" || upper == "NOT EXISTS") {
                for (ASTNode* c = first_child(expr); c != nullptr; c = c->next_sibling) {
                    if (c->node_type == NodeType::Subquery ||
                        c->node_type == NodeType::SubqueryExpr) {
                        analyze_subquery(c, scope);
                    }
                }
                record_type(expr, DataType::Boolean);
                record_nullability(expr, 1);
                return DataType::Boolean;
            }
            ASTNode* operand = first_child(expr);
            const DataType ot = infer_expr(operand, scope);
            const DataType result =
                (upper == "NOT") ? DataType::Boolean : ot;
            record_type(expr, result);
            record_nullability(expr, null_of(operand));
            return result;
        }

        // A subquery used in a scalar position (a SELECT-list item or one side of
        // a comparison) takes the type of its single projected column; projecting
        // more than one column is a ScalarSubqueryColumns diagnostic.
        case NodeType::Subquery:
        case NodeType::SubqueryExpr: {
            const std::vector<ResolvedColumn> proj = analyze_subquery(expr, scope);
            if (proj.size() > 1) {
                add_diagnostic(DiagnosticCode::ScalarSubqueryColumns,
                               "scalar subquery projects " + std::to_string(proj.size()) +
                                   " columns; exactly one is required",
                               expr);
            }
            const DataType result = proj.empty() ? DataType::Unknown : proj.front().type;
            record_type(expr, result);
            // A scalar subquery may return no rows, so its value is nullable.
            record_nullability(expr, 2);
            return result;
        }

        case NodeType::CaseExpr: {
            // Parser layout (confirmed by probing real trees):
            //   * searched CASE: children are WHEN branches, each a BinaryExpr
            //     whose primary_text is "WHEN" with two children [condition,
            //     THEN-result], optionally followed by a bare ELSE expression.
            //   * simple CASE `CASE op WHEN v THEN r ...`: an extra leading
            //     operand child precedes the WHEN branches; each WHEN branch's
            //     first child is the compare value (not a boolean condition).
            // We unify the THEN results and the ELSE (UnionReconcile) for the
            // result type; a searched-CASE WHEN condition that is clearly not
            // boolean gets a soft warning.
            std::vector<ASTNode*> children;
            for (ASTNode* c = first_child(expr); c != nullptr; c = c->next_sibling) {
                children.push_back(c);
            }
            auto is_when = [](const ASTNode* n) {
                return n->node_type == NodeType::BinaryExpr &&
                       to_upper(n->primary_text) == "WHEN";
            };
            // A leading non-WHEN child is the simple-CASE operand; a trailing
            // non-WHEN child (after the WHEN branches) is the ELSE result.
            const bool simple =
                !children.empty() && !is_when(children.front());
            if (simple) {
                infer_expr(children.front(), scope);  // resolve the operand
            }
            std::vector<DataType> branch_types;
            std::vector<int> branch_nulls;
            bool has_else = false;
            // Constant-fold analysis for the division-by-zero warning: an arm
            // whose guard is a constant FALSE is dead, and once an arm's guard is
            // a constant TRUE every later arm (and the ELSE) is dead. A dead arm's
            // body is still analyzed (for type reconciliation) but with div0
            // warnings suppressed, matching PostgreSQL's plan-time CASE arm
            // elimination.
            ASTNode* const case_operand =
                (simple && !children.empty()) ? children.front() : nullptr;
            bool prior_taken = false;
            for (std::size_t i = 0; i < children.size(); ++i) {
                ASTNode* c = children[i];
                if (simple && i == 0) {
                    continue;  // operand, already handled
                }
                if (is_when(c)) {
                    ASTNode* cond = first_child(c);
                    ASTNode* then_res = cond != nullptr ? cond->next_sibling : nullptr;
                    const DataType ct = infer_expr(cond, scope);
                    // In a searched CASE the WHEN operand is a predicate; warn if
                    // it is clearly a non-boolean, non-wildcard type.
                    if (!simple && cond != nullptr) {
                        const TypeCategory cc = category_of(ct);
                        if (cc != TypeCategory::Boolean && cc != TypeCategory::Wildcard) {
                            add_diagnostic(DiagnosticCode::ImplicitCoercion,
                                           "CASE WHEN condition is not a boolean expression",
                                           c, Severity::Warning);
                        }
                    }
                    // Is this arm's guard a compile-time constant? For a simple
                    // CASE the guard is `operand = value`; for a searched CASE it
                    // is the condition itself.
                    std::optional<bool> guard;
                    if (simple) {
                        const auto ov = eval_const_int(case_operand);
                        const auto vv = eval_const_int(cond);
                        if (ov && vv) guard = (*ov == *vv);
                    } else {
                        guard = eval_const_bool(cond);
                    }
                    const bool arm_dead =
                        prior_taken || (guard.has_value() && !*guard);
                    if (arm_dead) ++div0_suppress_;
                    const DataType tt = infer_expr(then_res, scope);
                    if (arm_dead) --div0_suppress_;
                    branch_types.push_back(tt);
                    branch_nulls.push_back(null_of(then_res));
                    if (guard.has_value() && *guard) prior_taken = true;
                } else {
                    // Trailing bare expression: the ELSE result (dead once a prior
                    // arm's guard was constant-true).
                    has_else = true;
                    if (prior_taken) ++div0_suppress_;
                    const DataType et = infer_expr(c, scope);
                    if (prior_taken) --div0_suppress_;
                    branch_types.push_back(et);
                    branch_nulls.push_back(null_of(c));
                }
            }
            // Unify the branch result types (THEN + ELSE).
            DataType result = DataType::Unknown;
            bool mismatch = false;
            for (const DataType bt : branch_types) {
                const Coercion r = coerce(result, bt, CoercionKind::UnionReconcile);
                if (r.status == CoercionStatus::Incompatible) {
                    mismatch = true;
                }
                result = r.type;
            }
            if (mismatch) {
                add_diagnostic(DiagnosticCode::TypeMismatch,
                               "incompatible result types among CASE branches", expr);
            }
            record_type(expr, result);
            // Nullable if any branch can be NULL, or if there is no ELSE (an
            // unmatched CASE yields NULL).
            int null_state = combine_nullable_any(branch_nulls);
            if (!has_else) {
                null_state = 2;
            }
            record_nullability(expr, null_state);
            return result;
        }

        case NodeType::InExpr: {
            ASTNode* left = first_child(expr);
            const DataType lt = infer_expr(left, scope);
            ASTNode* right = left != nullptr ? left->next_sibling : nullptr;
            // Under three-valued logic `x IN (...)` is NULL when no element
            // equals x but some element (or x itself) is NULL - so the result is
            // nullable if the left operand OR any element/subquery column is
            // nullable, not just when the left operand is. Inferring NOT NULL
            // from the left alone (as before) is the unsafe direction: a
            // consumer could drop a needed NULL check. `x IN (1, NULL)` is the
            // canonical case. (NOT IN has the same nullability.)
            std::vector<int> nulls{null_of(left)};
            if (right != nullptr && (right->node_type == NodeType::Subquery ||
                                     right->node_type == NodeType::SubqueryExpr)) {
                // `expr IN (subquery)` (scalar) or `(a, b, ...) IN (subquery)`
                // (row): the subquery must project as many columns as the left
                // operand is WIDE - one for a scalar left, N for an N-element
                // RowConstructor - and be pairwise type-compatible. Requiring
                // exactly one column here wrongly rejected the legal row form
                // `(a, b) IN (SELECT x, y FROM t)` (Postgres accepts it).
                const std::vector<ResolvedColumn> proj = analyze_subquery(right, scope);
                std::vector<ASTNode*> row_elems;
                if (left != nullptr && left->node_type == NodeType::RowConstructor) {
                    for (ASTNode* c = first_child(left); c != nullptr; c = c->next_sibling) {
                        row_elems.push_back(c);
                    }
                }
                const std::size_t lhs_width = row_elems.empty() ? 1 : row_elems.size();
                if (proj.size() != lhs_width) {
                    add_diagnostic(DiagnosticCode::InSubqueryColumns,
                                   "subquery on the right of IN projects " +
                                       std::to_string(proj.size()) + " columns; " +
                                       std::to_string(lhs_width) + " required",
                                   expr);
                    nulls.push_back(2);  // shape mismatch: conservatively nullable
                } else {
                    // Pairwise type compatibility (one warning per IN, matching
                    // the sibling comparison paths). For a scalar left the single
                    // element type is `lt`; for a row it is each element's type.
                    bool coercion_warned = false;
                    for (std::size_t i = 0; i < proj.size(); ++i) {
                        const DataType et =
                            row_elems.empty() ? lt : infer_expr(row_elems[i], scope);
                        if (!coercion_warned &&
                            coerce(et, proj[i].type, CoercionKind::Comparison).status !=
                                CoercionStatus::Ok) {
                            add_diagnostic(DiagnosticCode::ImplicitCoercion,
                                           "implicit coercion between IN operand and "
                                           "subquery column of a different type category",
                                           expr, Severity::Warning);
                            coercion_warned = true;
                        }
                        // A NULL in any projected column makes a non-matching row
                        // yield NULL rather than FALSE.
                        nulls.push_back(proj[i].nullable ? 2 : 1);
                    }
                }
            } else {
                // `expr IN (list)`: infer each list element (resolves columns),
                // fold its nullability, AND apply the SAME cross-type comparison
                // check the other comparison forms do - `x = e`, `x BETWEEN`, and
                // `x IN (subquery)` all run coerce(...Comparison) and warn on a
                // cross-category pair; the value-list branch previously skipped it,
                // so `id IN (name)` was silently accepted while `id = name` warned.
                // One warning per IN (a list of N incompatible elements is one type
                // problem, not N), matching the single-diagnostic sibling paths.
                bool coercion_warned = false;
                for (ASTNode* c = right; c != nullptr; c = c->next_sibling) {
                    const DataType et = infer_expr(c, scope);
                    nulls.push_back(null_of(c));
                    if (!coercion_warned &&
                        coerce(lt, et, CoercionKind::Comparison).status !=
                            CoercionStatus::Ok) {
                        add_diagnostic(DiagnosticCode::ImplicitCoercion,
                                       "implicit coercion between IN operand and a "
                                       "list value of a different type category",
                                       expr, Severity::Warning);
                        coercion_warned = true;
                    }
                }
            }
            record_type(expr, DataType::Boolean);
            record_nullability(expr, combine_nullable_any(nulls));
            return DataType::Boolean;
        }

        // CAST(operand AS type): the operand is the first child, the target type
        // name is carried in the second child's primary_text. The result takes
        // the named type (Unknown if unrecognized); a cast preserves the
        // operand's nullability (CAST(NULL) is NULL, CAST(not-null) is not-null).
        case NodeType::CastExpr: {
            ASTNode* operand = first_child(expr);
            ASTNode* type_node = operand != nullptr ? operand->next_sibling : nullptr;
            infer_expr(operand, scope);  // resolve nested columns
            DataType result = DataType::Unknown;
            if (type_node != nullptr) {
                result = data_type_from_name(type_node->primary_text);
                record_type(type_node, result);
            }
            record_type(expr, result);
            record_nullability(expr, null_of(operand));
            return result;
        }

        // x BETWEEN low AND high: children [value, low, high]. Boolean result,
        // nullable if any operand is nullable. The value is compared against each
        // bound, so a cross-category pairing is flagged as a soft coercion.
        case NodeType::BetweenExpr: {
            ASTNode* value = first_child(expr);
            ASTNode* low = value != nullptr ? value->next_sibling : nullptr;
            ASTNode* high = low != nullptr ? low->next_sibling : nullptr;
            const DataType vt = infer_expr(value, scope);
            const DataType lt = infer_expr(low, scope);
            const DataType ht = infer_expr(high, scope);
            if (coerce(vt, lt, CoercionKind::Comparison).status != CoercionStatus::Ok ||
                coerce(vt, ht, CoercionKind::Comparison).status != CoercionStatus::Ok) {
                add_diagnostic(DiagnosticCode::ImplicitCoercion,
                               "implicit coercion in BETWEEN between operands of "
                               "different type categories",
                               expr, Severity::Warning);
            }
            record_type(expr, DataType::Boolean);
            record_nullability(expr,
                               combine_nullable_any({null_of(value), null_of(low),
                                                     null_of(high)}));
            return DataType::Boolean;
        }

        // x LIKE pattern: children [value, pattern]. Boolean result, nullable if
        // either operand is nullable. LIKE is a string operation; a clearly
        // non-string, non-wildcard operand is flagged as a soft coercion.
        case NodeType::LikeExpr: {
            ASTNode* value = first_child(expr);
            ASTNode* pattern = value != nullptr ? value->next_sibling : nullptr;
            const DataType vt = infer_expr(value, scope);
            const DataType pt = infer_expr(pattern, scope);
            auto non_string = [](DataType t) {
                const TypeCategory c = category_of(t);
                return c != TypeCategory::String && c != TypeCategory::Wildcard;
            };
            if (non_string(vt) || non_string(pt)) {
                add_diagnostic(DiagnosticCode::ImplicitCoercion,
                               "LIKE applied to a non-string operand", expr,
                               Severity::Warning);
            }
            record_type(expr, DataType::Boolean);
            record_nullability(expr,
                               combine_nullable_any({null_of(value), null_of(pattern)}));
            return DataType::Boolean;
        }

        // x IS [NOT] NULL: a single operand. The test itself is always a defined
        // boolean, so the result is Boolean and never NULL regardless of the
        // operand's nullability.
        case NodeType::IsNullExpr: {
            infer_expr(first_child(expr), scope);  // resolve the operand
            record_type(expr, DataType::Boolean);
            record_nullability(expr, 1);
            return DataType::Boolean;
        }

        // x IS [NOT] TRUE / FALSE / UNKNOWN: like IsNull, a single operand whose
        // 3VL truth value is tested. The result is always a defined boolean
        // (never NULL) regardless of the operand's nullability.
        case NodeType::BooleanTestExpr: {
            infer_expr(first_child(expr), scope);  // resolve the operand
            record_type(expr, DataType::Boolean);
            record_nullability(expr, 1);
            return DataType::Boolean;
        }

        // A bind parameter ($1 / ?): its type is not known until the statement is
        // bound, so it is Unknown and unifies with anything (wildcard). Marked
        // nullable since a parameter may be bound to NULL.
        case NodeType::Parameter: {
            record_type(expr, DataType::Unknown);
            record_nullability(expr, 2);
            return DataType::Unknown;
        }

        // ARRAY[elem, ...]: a constructed array. Each element is inferred (which
        // also resolves nested column refs); the constructor itself has array
        // type and is never NULL, even when an element is. Element-type unification
        // is left to a later pass - a flat DataType has no element parameter, so
        // the element types remain recoverable from the children.
        case NodeType::ArrayConstructor: {
            for (ASTNode* el = first_child(expr); el != nullptr; el = el->next_sibling) {
                infer_expr(el, scope);
            }
            record_type(expr, DataType::Array);
            record_nullability(expr, 1);
            return DataType::Array;
        }

        // ROW(a, b, ...) / (a, b, ...): a row/tuple constructor. Each element is
        // inferred (which also resolves nested column refs). A flat DataType has
        // no row type, so the constructor itself is typed Unknown; it is only
        // meaningful inside a row comparison, which yields a Boolean. Marked
        // nullable since any element may be NULL.
        case NodeType::RowConstructor: {
            for (ASTNode* el = first_child(expr); el != nullptr; el = el->next_sibling) {
                infer_expr(el, scope);
            }
            record_type(expr, DataType::Unknown);
            record_nullability(expr, 2);
            return DataType::Unknown;
        }

        // <value> COLLATE <name>: a collation annotation. It changes how the
        // value compares/sorts, not its type or nullability, so the CollateClause
        // takes the operand's type and nullability. Inferring the operand also
        // resolves it (a bare column argument).
        case NodeType::CollateClause: {
            ASTNode* operand = first_child(expr);
            const DataType t = infer_expr(operand, scope);
            record_type(expr, t);
            record_nullability(expr, null_of(operand));
            return t;
        }

        default: {
            // Unknown expression form: recurse so nested column refs still get
            // resolved, but do not claim a type.
            for (ASTNode* c = first_child(expr); c != nullptr; c = c->next_sibling) {
                infer_expr(c, scope);
            }
            return DataType::Unknown;
        }
    }
}

std::vector<ResolvedColumn> Analyzer::analyze_subquery(ASTNode* subquery, Scope& enclosing) {
    if (subquery == nullptr) {
        return {};
    }
    // The subquery's inner query block. A plain subquery wraps a SelectStmt; a
    // set operation (UNION/…) is handled through analyze_stmt.
    ASTNode* inner = find_child(subquery, NodeType::SelectStmt);
    if (inner == nullptr) {
        for (ASTNode* c = first_child(subquery); c != nullptr; c = c->next_sibling) {
            if (is_setop(c->node_type)) {
                inner = c;
                break;
            }
        }
    }

    // Track correlation for this subquery: push it onto the active-subquery
    // stack (paired with its enclosing scope) while its body is analyzed, so a
    // column that resolves in an enclosing scope can mark this subquery - and
    // every intervening one - correlated. Popped afterward. Keeping the whole
    // stack (rather than a single innermost sink) is what lets an intermediate
    // subquery be flagged when a reference reaches two-or-more scopes out.
    bool correlated = false;
    corr_stack_.emplace_back(&enclosing, &correlated);
    std::vector<ResolvedColumn> proj;
    if (inner != nullptr) {
        proj = analyze_stmt(inner, &enclosing);
    }
    corr_stack_.pop_back();

    correlated_[subquery] = correlated;
    if (correlated) {
        subquery->set_flag(ast::NodeFlags::IsCorrelated);
    }
    return proj;
}

namespace {

// Does the subtree rooted at `node` contain an aggregate function call?
[[nodiscard]] bool contains_aggregate(const ASTNode* node, int depth = 0) {
    if (node == nullptr) {
        return false;
    }
    // Bail on a pathologically deep tree (same bound as the analyzer's expression
    // walkers) so this grouped-ness probe cannot overflow the stack. A subtree
    // beyond this depth is treated as containing no aggregate; the query is
    // already over-depth and will get an ExpressionTooComplex diagnostic from
    // infer_expr.
    if (depth >= kMaxExprDepth) {
        return false;
    }
    // A nested subquery is a separate query block: an aggregate inside it does
    // not collapse the enclosing query's rows and so does not make the enclosing
    // query grouped. Stop at the boundary (the subquery runs its own grouping
    // analysis independently).
    if (node->node_type == NodeType::Subquery ||
        node->node_type == NodeType::SubqueryExpr) {
        return false;
    }
    if (node->node_type == NodeType::FunctionCall ||
        node->node_type == NodeType::FunctionExpr) {
        // A windowed aggregate (e.g. SUM(x) OVER (...)) does not collapse rows,
        // so it does not make the query grouped.
        if (is_aggregate_name(to_upper(node->primary_text)) && !has_window_spec(node)) {
            return true;
        }
    }
    for (const ASTNode* c = node->first_child; c != nullptr; c = c->next_sibling) {
        if (contains_aggregate(c, depth + 1)) {
            return true;
        }
    }
    return false;
}

// Does the subtree rooted at `node` contain a WINDOW function call (any call
// carrying an OVER / WindowSpec)? Mirrors contains_aggregate: bounded by the
// shared expression-depth limit and stopping at a nested subquery boundary (a
// window inside a subquery belongs to that inner block). Used to reject a
// window function used as a GROUP BY key (Postgres: "window functions are not
// allowed in GROUP BY") - grouping happens before windowing, so a window call
// can never be a grouping key.
[[nodiscard]] bool contains_window(const ASTNode* node, int depth = 0) {
    if (node == nullptr || depth >= kMaxExprDepth) {
        return false;
    }
    if (node->node_type == NodeType::Subquery ||
        node->node_type == NodeType::SubqueryExpr) {
        return false;
    }
    if ((node->node_type == NodeType::FunctionCall ||
         node->node_type == NodeType::FunctionExpr) &&
        has_window_spec(node)) {
        return true;
    }
    for (const ASTNode* c = node->first_child; c != nullptr; c = c->next_sibling) {
        if (contains_window(c, depth + 1)) {
            return true;
        }
    }
    return false;
}

// True if `key` is illegal as a GROUP BY key because it is (or contains) a
// non-windowed aggregate or a window function. Writes the matching Postgres
// message into `msg`. Grouping is what PRODUCES aggregates and precedes
// windowing, so neither can itself be a grouping key.
[[nodiscard]] bool illegal_group_key(const ASTNode* key, const char*& msg) {
    if (contains_aggregate(key)) {
        msg = "aggregate functions are not allowed in GROUP BY";
        return true;
    }
    if (contains_window(key)) {
        msg = "window functions are not allowed in GROUP BY";
        return true;
    }
    return false;
}

// Two resolved column references can share a (table_id, column_id) yet name
// DIFFERENT relation instances: a self-join (`users a JOIN users b`) binds both
// aliases to the same catalog table, so `a.id` and `b.id` carry the identical
// (table_id, column_id). A grouping key on one alias does not cover the same
// column of the other, so their qualifiers must also agree. Self-join instances
// are always aliased (a bare reference in a self-join is ambiguous and diagnosed
// separately), so it is enough to reject the case where both references are
// qualified and their qualifiers differ; a bare reference falls through to the
// (table_id, column_id) test, which is unambiguous outside a self-join.
[[nodiscard]] bool same_relation_instance(std::string_view a_text,
                                          std::string_view b_text) {
    const std::string_view qa = split_column_ref(a_text).qualifier;
    const std::string_view qb = split_column_ref(b_text).qualifier;
    if (!qa.empty() && !qb.empty()) {
        return iequals(qa, qb);
    }
    return true;
}

// Two resolved column references can share a (table_id, column_id) yet denote
// DIFFERENT columns of a derived relation: a derived table / CTE whose body
// projects the same base column under two names - `(SELECT id AS a, id AS b
// FROM users) t` - gives t.a and t.b the base column's (users, id) identity,
// because a derived column carries through the identity of the expression it
// projects. Grouping by t.a does not cover t.b (Postgres rejects the second),
// so an id match alone is not enough: the referenced column NAME must also
// agree. For a plain base column the name is invariant for a given id, so this
// is a no-op there; it only separates same-base derived aliases.
[[nodiscard]] bool same_column_name(std::string_view a_text, std::string_view b_text) {
    return iequals(split_column_ref(a_text).column, split_column_ref(b_text).column);
}

// Does a column reference `ref` match the grouping key `k`? Prefer resolved
// (table_id, column_id) identity; fall back to the reference text when either
// side is unresolved (e.g. derived columns or expression keys).
[[nodiscard]] bool key_matches(const GroupKey& k, const ASTNode* ref) {
    const std::uint32_t tid = ref->context.analysis.table_id;
    const std::uint32_t cid = ref->context.analysis.column_id;
    if (cid != 0 && k.column_id != 0) {
        return tid == k.table_id && cid == k.column_id &&
               same_relation_instance(ref->primary_text, k.text) &&
               same_column_name(ref->primary_text, k.text);
    }
    return iequals(ref->primary_text, k.text);  // identifiers compare case-insensitively
}

// Structural equality of a SELECT/ORDER BY/HAVING expression `e` against a GROUP
// BY key expression `k`: true when `e` "is" that key. Column-ref leaves compare
// by resolved (table_id, column_id) identity, falling back to case-insensitive
// text; function names compare case-insensitively (SQL identifiers); literals
// and operators compare EXACTLY (a different literal is a different key - matching
// them would wrongly exempt a non-grouped column); children must match in order
// and arity. This lets a whole-expression grouping key such as
// `GROUP BY date_trunc('month', ts)` or `GROUP BY a + b` cover the same
// expression in the projection, not only single-column keys. Bounded by the
// shared expression-depth limit.
[[nodiscard]] bool group_expr_equal(const ASTNode* e, const ASTNode* k, int depth = 0) {
    if (e == nullptr || k == nullptr) {
        return e == k;
    }
    if (depth >= static_cast<int>(kMaxExprDepth)) {
        return false;  // over-deep: refuse to claim equality rather than recurse unbounded
    }
    if (is_column_ref_node(e->node_type) && is_column_ref_node(k->node_type)) {
        const std::uint32_t ec = e->context.analysis.column_id;
        const std::uint32_t kc = k->context.analysis.column_id;
        if (ec != 0 && kc != 0) {
            return e->context.analysis.table_id == k->context.analysis.table_id && ec == kc &&
                   same_relation_instance(e->primary_text, k->primary_text) &&
                   same_column_name(e->primary_text, k->primary_text);
        }
        return iequals(e->primary_text, k->primary_text);
    }
    if (e->node_type != k->node_type) {
        return false;
    }
    const bool name_like = (e->node_type == NodeType::FunctionCall ||
                            e->node_type == NodeType::FunctionExpr);
    if (name_like ? !iequals(e->primary_text, k->primary_text)
                  : e->primary_text != k->primary_text) {
        return false;
    }
    const ASTNode* ec = first_child(e);
    const ASTNode* kc = first_child(k);
    while (ec != nullptr && kc != nullptr) {
        if (!group_expr_equal(ec, kc, depth + 1)) {
            return false;
        }
        ec = ec->next_sibling;
        kc = kc->next_sibling;
    }
    return ec == nullptr && kc == nullptr;  // same arity
}

}  // namespace

ASTNode* Analyzer::group_key_alias_item(ASTNode* key, ASTNode* select_list,
                                        Scope& scope) const {
    if (key == nullptr || select_list == nullptr) {
        return nullptr;
    }
    if (!is_column_ref_node(key->node_type)) {
        return nullptr;
    }
    const QualifiedRef qref = split_column_ref(key->primary_text);
    // A qualified `t.c` names a base column, never an output alias.
    if (!qref.qualifier.empty()) {
        return nullptr;
    }
    // The input column wins: only fall through to the output alias when the FROM
    // scope neither resolves this name NOR finds it ambiguous. An AMBIGUOUS bare
    // name has found == false but ambiguous == true; reinterpreting it as an
    // output alias silently suppressed the AmbiguousColumn error and changed the
    // query's meaning. Returning nullptr lets normal resolution raise the
    // ambiguity (`GROUP BY id` over a join where two inputs expose `id`).
    const auto res = scope.resolve_bare(qref.column);
    if (res.found || res.ambiguous) {
        return nullptr;
    }
    // Match the name against each projected item's output name (its alias, or
    // the column name for a bare column ref).
    for (ASTNode* item = first_child(select_list); item != nullptr;
         item = item->next_sibling) {
        if (item->node_type == NodeType::Star) {
            continue;
        }
        std::string_view out_name;
        const std::string_view a = alias_of(item);
        if (!a.empty()) {
            out_name = a;
        } else if (item->node_type == NodeType::ColumnRef) {
            out_name = split_column_ref(item->primary_text).column;
        } else {
            out_name = item->primary_text;
        }
        if (iequals(out_name, qref.column)) {
            // GROUP BY on an alias that names an aggregate expression is illegal
            // (Postgres: "aggregate functions are not allowed in GROUP BY").
            // Decline the alias so the key falls to normal resolution and is
            // rejected, rather than silently accepting a nonsensical grouping.
            if (contains_aggregate(item)) {
                return nullptr;
            }
            return item;
        }
    }
    return nullptr;
}

void Analyzer::analyze_grouping(ASTNode* select_stmt, ASTNode* group_by, Scope& scope,
                                std::vector<ResolvedColumn>& output,
                                const std::vector<ASTNode*>& output_sources) {

    // An aggregate in the WHERE clause is illegal regardless of grouping:
    // aggregates are computed after the WHERE filter has selected rows, so they
    // belong in HAVING. `contains_aggregate` stops at subquery boundaries, so an
    // aggregate that legitimately lives in a WHERE subquery is not flagged here.
    if (ASTNode* where = find_child(select_stmt, NodeType::WhereClause)) {
        if (ASTNode* pred = first_child(where); pred != nullptr && contains_aggregate(pred)) {
            add_diagnostic(DiagnosticCode::AggregateInWhere,
                           "aggregate functions are not allowed in WHERE (use HAVING)",
                           pred);
        }
    }

    ASTNode* select_list = find_child(select_stmt, NodeType::SelectList);

    // The query is "grouped" (subject to the legality rules) if it has a GROUP BY
    // clause, a HAVING clause, or any non-windowed aggregate in a clause that
    // forces aggregation - the SELECT list or the ORDER BY. Per SQL, the presence
    // of HAVING alone collapses the whole table into a single group even without
    // GROUP BY, and an aggregate anywhere in SELECT / HAVING / ORDER BY makes the
    // query an aggregate query (so `SELECT id FROM emp HAVING COUNT(*) > 0` and
    // `SELECT id FROM emp ORDER BY COUNT(*)` must flag the bare `id`, matching
    // Postgres). Previously only GROUP BY and a SELECT-list aggregate set this, so
    // an aggregate confined to HAVING / ORDER BY silently accepted a non-grouped
    // column. contains_aggregate stops at subquery boundaries and excludes
    // windowed calls, so neither forces grouping of THIS block.
    ASTNode* having_clause = find_child(select_stmt, NodeType::HavingClause);
    ASTNode* order_by_clause = find_child(select_stmt, NodeType::OrderByClause);
    const auto clause_has_aggregate = [](ASTNode* clause) {
        if (clause == nullptr) {
            return false;
        }
        for (ASTNode* item = first_child(clause); item != nullptr;
             item = item->next_sibling) {
            if (contains_aggregate(item)) {
                return true;
            }
        }
        return false;
    };
    const bool grouped = group_by != nullptr || having_clause != nullptr ||
                         clause_has_aggregate(select_list) ||
                         clause_has_aggregate(order_by_clause);
    if (!grouped) {
        return;
    }

    // Collect the grouping keys (their resolved identity + text).
    std::vector<GroupKey> keys;
    if (group_by != nullptr) {
        // Flatten the GROUP BY list into its effective grouping columns. A
        // GroupingElement (ROLLUP / CUBE / GROUPING SETS) is not itself a key -
        // its ARGUMENT columns are the grouping columns (Postgres makes the
        // arguments of ROLLUP()/CUBE()/GROUPING SETS() grouping columns), so
        // descend into it (recursively, for nested GROUPING SETS) and register
        // each argument. Without this the GroupingElement's own empty identity
        // was taken as the key and the real columns never registered, so e.g.
        // `SELECT dept, SUM(sal) FROM emp GROUP BY ROLLUP(dept)` wrongly flagged
        // dept as "must appear in the GROUP BY clause".
        // Flatten the GROUP BY list into its effective grouping columns. A key
        // may be wrapped in one or more of: a GroupingElement (ROLLUP / CUBE /
        // GROUPING SETS), a ColumnList (a parenthesized GROUPING SETS member,
        // e.g. `GROUPING SETS ((dept), (region))`), or a RowConstructor (a
        // parenthesized element/list, e.g. `ROLLUP((dept, region))` or a bare
        // `GROUP BY (dept, region)`). Postgres makes every column inside any such
        // wrapper a grouping column, so descend through all of them (recursively,
        // via the worklist) and register the leaf argument nodes as keys. Without
        // this a wrapper node's own empty identity was taken as the key and the
        // real columns were never registered - a spurious NonGroupedColumn error.
        const auto is_group_wrapper = [](NodeType t) {
            return t == NodeType::GroupingElement || t == NodeType::ColumnList ||
                   t == NodeType::RowConstructor;
        };
        std::vector<ASTNode*> effective_keys;
        // Keys reached THROUGH a GroupingElement (ROLLUP / CUBE / GROUPING SETS)
        // are nullable in the result - see the projection re-marking below.
        std::vector<ASTNode*> grouping_set_keys;
        struct WorkItem {
            ASTNode* node;
            bool in_grouping_set;
        };
        std::vector<WorkItem> stack;
        for (ASTNode* key = first_child(group_by); key != nullptr; key = key->next_sibling) {
            stack.push_back({key, false});
        }
        while (!stack.empty()) {
            const WorkItem it = stack.back();
            stack.pop_back();
            if (is_group_wrapper(it.node->node_type)) {
                // Only a GroupingElement introduces the super-aggregate NULL rows;
                // a bare ColumnList / RowConstructor parenthesization (e.g.
                // `GROUP BY (a, b)`) is just several ordinary grouping columns.
                const bool child_in_gs =
                    it.in_grouping_set || it.node->node_type == NodeType::GroupingElement;
                for (ASTNode* arg = first_child(it.node); arg != nullptr;
                     arg = arg->next_sibling) {
                    stack.push_back({arg, child_in_gs});
                }
            } else {
                effective_keys.push_back(it.node);
                if (it.in_grouping_set) {
                    grouping_set_keys.push_back(it.node);
                }
            }
        }
        for (ASTNode* key : effective_keys) {
            // A GROUP BY key may not be (or contain) a non-windowed aggregate or
            // a window function: grouping is what PRODUCES aggregates and runs
            // before windowing, so `GROUP BY MAX(age)` / `GROUP BY RANK() OVER
            // (...)` is illegal (Postgres: "aggregate/window functions are not
            // allowed in GROUP BY"). This catches a directly-written aggregate /
            // window key; the positional and alias spellings are re-checked on
            // their resolved SELECT item below. (contains_aggregate / _window
            // stop at subquery boundaries, so one inside a scalar subquery key is
            // not flagged here.)
            if (const char* why = nullptr; illegal_group_key(key, why)) {
                add_diagnostic(DiagnosticCode::AggregateInGroupBy, why, key);
                continue;  // not a valid grouping key; do not register it
            }
            // Positional GROUP BY: `GROUP BY n` refers to the n-th (1-based)
            // output column of the SELECT list (legal in Postgres / MySQL /
            // SQLite / DuckDB). The key node is an IntegerLiteral carrying the
            // ordinal in primary_text; take the identity of that SELECT item so
            // a SELECT reference to the same column matches the key. When n is
            // out of range we leave the literal key as-is (no crash).
            const ASTNode* identity = key;
            if (key->node_type == NodeType::IntegerLiteral && select_list != nullptr) {
                std::size_t ordinal = 0;
                bool valid = !key->primary_text.empty();
                for (const char c : key->primary_text) {
                    if (c < '0' || c > '9') {
                        valid = false;
                        break;
                    }
                    ordinal = ordinal * 10 + static_cast<std::size_t>(c - '0');
                }
                if (valid && ordinal >= 1) {
                    std::size_t i = 1;
                    for (ASTNode* item = first_child(select_list); item != nullptr;
                         item = item->next_sibling, ++i) {
                        if (i == ordinal) {
                            identity = item;
                            break;
                        }
                    }
                }
            } else if (ASTNode* alias_item =
                           group_key_alias_item(key, select_list, scope)) {
                // GROUP BY <output-alias>: the query groups by the aliased SELECT
                // item's expression, so take that item's identity. A SELECT / ORDER
                // BY / HAVING reference to the same expression then matches this key
                // (single column or a compound expression alike), and the aliased
                // item itself is not flagged as non-grouped.
                identity = alias_item;
            }
            // A positional (`GROUP BY 1`) or alias (`GROUP BY c`) key resolves to
            // a SELECT item; that item is just as illegal a grouping key as a
            // directly-written aggregate / window would be. The raw-key check
            // above only saw the ordinal literal or the alias name, so re-test the
            // RESOLVED item - otherwise `SELECT COUNT(*) FROM emp GROUP BY 1` (or
            // `... GROUP BY <alias-of-a-window>`) slips through and the binder is
            // handed an aggregate / window node as a group key. (group_key_alias_item
            // already declines an aggregate alias, but not a window alias; the
            // positional path had no such guard at all.)
            if (identity != key) {
                if (const char* why = nullptr; illegal_group_key(identity, why)) {
                    add_diagnostic(DiagnosticCode::AggregateInGroupBy, why, key);
                    continue;
                }
            }
            GroupKey k;
            k.table_id = identity->context.analysis.table_id;
            k.column_id = identity->context.analysis.column_id;
            k.text = identity->primary_text;
            k.node = identity;
            keys.push_back(k);
        }

        // A grouping column that appears inside ROLLUP / CUBE / GROUPING SETS is
        // NULL in the super-aggregate (subtotal / grand-total) rows, so it - and
        // any SELECT-list expression that READS it outside an aggregate - is
        // nullable in the result even when the base column is NOT NULL (Postgres).
        // The projection / node nullability was set from the base column before
        // analyze_grouping runs, so re-mark here. A plain GROUP BY key, or a bare
        // parenthesized GROUP BY list, is NOT nulled: only a GroupingElement.
        if (!grouping_set_keys.empty()) {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> gs_ids;
            for (const ASTNode* gk : grouping_set_keys) {
                const std::uint32_t tid = gk->context.analysis.table_id;
                const std::uint32_t cid = gk->context.analysis.column_id;
                if (tid == 0 && cid == 0) {
                    continue;  // unresolved / expression key: no column identity
                }
                gs_ids.emplace_back(tid, cid);
                // (a) A `*`-expanded output column of the key (which has no single
                //     source node) is nulled by base-column identity; the
                //     expression pass below covers every column with a source node.
                for (ResolvedColumn& oc : output) {
                    if (oc.table_id == tid && oc.column_id == cid) {
                        oc.nullable = true;
                    }
                }
            }
            // (b) Any SELECT-list column whose source expression reads a
            //     grouping-set key outside an aggregate - a bare `key`, `key + 0`,
            //     `key || 'x'`, `CASE key ...` - is nullable. Aggregates over the
            //     key (`SUM(key)`) are NOT: they read the present raw value.
            for (std::size_t i = 0; i < output.size() && i < output_sources.size();
                 ++i) {
                ASTNode* src = output_sources[i];
                if (src == nullptr) {
                    continue;  // `*`-expanded: handled by identity match in (a)
                }
                if (expr_reads_grouping_key(src, gs_ids, /*in_aggregate=*/false)) {
                    output[i].nullable = true;
                    record_nullability(src, 2);
                }
            }
        }
    }

    // SELECT list and ORDER BY: every non-aggregated column must be a group key.
    if (select_list != nullptr) {
        for (ASTNode* item = first_child(select_list); item != nullptr;
             item = item->next_sibling) {
            check_grouping_expr(item, keys, /*grouping_exempt=*/false, /*in_aggregate=*/false);
        }
    }
    if (ASTNode* order_by = find_child(select_stmt, NodeType::OrderByClause)) {
        for (ASTNode* item = first_child(order_by); item != nullptr; item = item->next_sibling) {
            // An UNQUALIFIED ORDER BY item that names a SELECT output column (by
            // name or alias) references the already-grouped projection, not an
            // input column, so it is exempt from the grouping rule. Otherwise
            // "SELECT dept, COUNT(*) AS c ... GROUP BY dept ORDER BY c" is falsely
            // flagged (the alias 'c' text-mismatches every group key). The
            // reference must be unqualified: a qualified `t.col` names a base
            // column and resolves against the FROM scope (an output alias is only
            // visible to ORDER BY by a bare name), so it must still obey the
            // grouping rule - exempting it by column name alone would silently
            // accept e.g. "SELECT dept, MAX(age) AS salary ... GROUP BY dept
            // ORDER BY emp.salary".
            if (is_column_ref_node(item->node_type)) {
                const QualifiedRef qref = split_column_ref(item->primary_text);
                if (qref.qualifier.empty()) {
                    bool is_output_ref = false;
                    for (const ResolvedColumn& col : output) {
                        if (iequals(col.name, qref.column)) { is_output_ref = true; break; }
                    }
                    if (is_output_ref) {
                        continue;
                    }
                }
            }
            check_grouping_expr(item, keys, /*grouping_exempt=*/false, /*in_aggregate=*/false);
        }
    }
    // HAVING may reference grouping keys and aggregates; a bare non-grouped
    // column outside an aggregate is illegal here too.
    if (ASTNode* having = find_child(select_stmt, NodeType::HavingClause)) {
        if (ASTNode* pred = first_child(having)) {
            check_grouping_expr(pred, keys, /*grouping_exempt=*/false, /*in_aggregate=*/false);
        }
    }
}

void Analyzer::check_grouping_expr(ASTNode* expr, const std::vector<GroupKey>& keys,
                                   bool grouping_exempt, bool in_aggregate) {
    if (expr == nullptr) {
        return;
    }
    if (expr_depth_ >= kMaxExprDepth) {
        return;  // over-deep; infer_expr already reported ExpressionTooComplex.
    }
    const DepthGuard depth_guard{expr_depth_};

    // Do not descend into a nested subquery: it is a distinct query block with
    // its own FROM scope and its own grouping analysis. Its columns (including a
    // correlated reference to an OUTER block's column) are not columns of THIS
    // grouped relation, so the GROUP BY legality rule here does not apply to
    // them. Crossing this boundary would spuriously flag them as non-grouped.
    if (expr->node_type == NodeType::Subquery ||
        expr->node_type == NodeType::SubqueryExpr) {
        return;
    }

    // If this whole expression is itself a grouping key, the query IS grouped on
    // it: it is exempt and we stop descending (its inner columns are covered by
    // the key). Covers expression keys - GROUP BY date_trunc('month', ts),
    // GROUP BY a + b - not only single-column keys. Checked before the column /
    // function descent so `SELECT date_trunc('month', ts) ... GROUP BY
    // date_trunc('month', ts)` is not falsely flagged on its inner `ts`.
    if (!grouping_exempt) {
        for (const GroupKey& k : keys) {
            if (k.node != nullptr && group_expr_equal(expr, k.node)) {
                return;
            }
        }
    }

    if (expr->node_type == NodeType::FunctionCall ||
        expr->node_type == NodeType::FunctionExpr) {
        // A windowed call is evaluated after grouping: it is not itself a
        // grouping aggregate, and its arguments / OVER expressions are exempt
        // from the grouping rule (like columns beneath an aggregate).
        const bool windowed = has_window_spec(expr);
        const bool is_agg = !windowed && is_aggregate_name(to_upper(expr->primary_text));
        if (is_agg && in_aggregate) {
            add_diagnostic(DiagnosticCode::NestedAggregate,
                           "aggregate function '" + std::string{expr->primary_text} +
                               "' nested inside another aggregate",
                           expr);
        }
        // Columns beneath an aggregate are exempt from the grouping-key rule.
        // Columns inside a WINDOW function are NOT: a window function runs AFTER
        // grouping, so its arguments and OVER (PARTITION BY / ORDER BY) expressions
        // see only the grouped output - a group key or an aggregate is fine, but a
        // bare ungrouped column is illegal (Postgres flags e.g. `ROW_NUMBER() OVER
        // (ORDER BY sal)` / `SUM(sal) OVER (...)` in a query grouped by dept). So a
        // window does not exempt its children; the normal rule applies, and an
        // aggregate nested in the OVER clause still exempts its own arguments when
        // the recursion reaches it. A window boundary is NOT aggregate nesting: an
        // aggregate inside an OVER clause (RANK() OVER (ORDER BY SUM(x))) is legal,
        // so `in_aggregate` is reset to false through a window and only set through
        // a plain aggregate.
        const bool child_exempt = grouping_exempt || is_agg;
        const bool child_in_aggregate = is_agg ? true : (windowed ? false : in_aggregate);
        for (ASTNode* c = first_child(expr); c != nullptr; c = c->next_sibling) {
            check_grouping_expr(c, keys, child_exempt, child_in_aggregate);
        }
        return;
    }

    if (is_column_ref_node(expr->node_type)) {
        if (!grouping_exempt) {
            bool ok = false;
            for (const GroupKey& k : keys) {
                if (key_matches(k, expr)) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                add_diagnostic(DiagnosticCode::NonGroupedColumn,
                               "column '" + std::string{expr->primary_text} +
                                   "' must appear in the GROUP BY clause or be used "
                                   "in an aggregate function",
                               expr);
            }
        }
        return;
    }

    for (ASTNode* c = first_child(expr); c != nullptr; c = c->next_sibling) {
        check_grouping_expr(c, keys, grouping_exempt, in_aggregate);
    }
}

bool Analyzer::expr_reads_grouping_key(
    ASTNode* expr,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& keys,
    bool in_aggregate) {
    if (expr == nullptr || keys.empty()) {
        return false;
    }
    if (expr_depth_ >= kMaxExprDepth) {
        return false;  // over-deep: infer_expr already reported it.
    }
    const DepthGuard depth_guard{expr_depth_};

    // A nested subquery is its own query block; a correlated reference to this
    // block's grouping key inside it is that block's concern, not this column's
    // nullability. (Mirrors check_grouping_expr's boundary.)
    if (expr->node_type == NodeType::Subquery ||
        expr->node_type == NodeType::SubqueryExpr) {
        return false;
    }

    if (expr->node_type == NodeType::FunctionCall ||
        expr->node_type == NodeType::FunctionExpr) {
        // A plain aggregate reads the raw input rows, where the grouping key is
        // present - so a key read INSIDE it is not nulled. A window function runs
        // after grouping and a non-aggregate call is transparent, so both keep the
        // current context (mirrors check_grouping_expr's aggregate tracking).
        const bool windowed = has_window_spec(expr);
        const bool is_agg = !windowed && is_aggregate_name(to_upper(expr->primary_text));
        const bool child_in_aggregate = is_agg ? true : (windowed ? false : in_aggregate);
        for (ASTNode* c = first_child(expr); c != nullptr; c = c->next_sibling) {
            if (expr_reads_grouping_key(c, keys, child_in_aggregate)) {
                return true;
            }
        }
        return false;
    }

    if (is_column_ref_node(expr->node_type)) {
        if (in_aggregate) {
            return false;
        }
        const std::uint32_t tid = expr->context.analysis.table_id;
        const std::uint32_t cid = expr->context.analysis.column_id;
        for (const auto& [ktid, kcid] : keys) {
            if (ktid == tid && kcid == cid) {
                return true;
            }
        }
        return false;
    }

    for (ASTNode* c = first_child(expr); c != nullptr; c = c->next_sibling) {
        if (expr_reads_grouping_key(c, keys, in_aggregate)) {
            return true;
        }
    }
    return false;
}

}  // namespace db25::semantic
