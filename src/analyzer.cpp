// DB25 Semantic Analyzer - Analyzer implementation

#include "db25/semantic/analyzer.hpp"

#include "db25/ast/node_types.hpp"
#include "db25/semantic/ast_helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
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

// Widen two numeric types to a common arithmetic result type.
[[nodiscard]] DataType promote_numeric(DataType a, DataType b) {
    if (!is_numeric(a) || !is_numeric(b)) {
        return DataType::Unknown;
    }
    auto rank = [](DataType t) -> int {
        switch (t) {
            case DataType::TinyInt: return 1;
            case DataType::SmallInt: return 2;
            case DataType::Integer: return 3;
            case DataType::BigInt: return 4;
            case DataType::Decimal: return 5;
            case DataType::Real: return 6;
            case DataType::Double: return 7;
            default: return 0;
        }
    };
    return rank(a) >= rank(b) ? a : b;
}

[[nodiscard]] bool is_comparison_op(std::string_view op) {
    return op == "=" || op == "==" || op == "<>" || op == "!=" || op == "<" ||
           op == ">" || op == "<=" || op == ">=";
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
        // Assigning across the numeric/string boundary (e.g. a text literal into
        // an integer column) is a permitted implicit conversion, flagged softly;
        // the stored value takes the target column's type `a`.
        if ((ca == TypeCategory::Numeric && cb == TypeCategory::String) ||
            (ca == TypeCategory::String && cb == TypeCategory::Numeric)) {
            return {CoercionStatus::ImplicitCoercion, a};
        }
        // Any other cross-category assignment (boolean/temporal vs numeric, …) is
        // a hard type mismatch.
        return {CoercionStatus::Incompatible, DataType::Unknown};
    }
    return {CoercionStatus::Incompatible, DataType::Unknown};
}

// Unify two types for a value-producing context (set-operation branch column,
// COALESCE argument list): the common type, or Unknown when incompatible.
[[nodiscard]] DataType unify_type(DataType a, DataType b) {
    return coerce(a, b, CoercionKind::UnionReconcile).type;
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
    // Only + and - carry temporal meaning; *, /, % over a temporal are invalid.
    const bool is_plus = (op == "+");
    const bool is_minus = (op == "-");
    if (!is_plus && !is_minus) {
        return {TemporalArithStatus::Invalid, DataType::Unknown};
    }

    const bool a_interval = (a == DataType::Interval);
    const bool b_interval = (b == DataType::Interval);
    const bool a_numeric = category_of(a) == TypeCategory::Numeric;
    const bool b_numeric = category_of(b) == TypeCategory::Numeric;

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
    // date ± integer -> date (day arithmetic); integer + date -> date.
    if (a == DataType::Date && b_numeric) {
        return {TemporalArithStatus::Ok, DataType::Date};
    }
    if (a_numeric && b == DataType::Date && is_plus) {
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
    // type (Unknown when the arguments are incompatible).
    if (upper_name == "GREATEST" || upper_name == "LEAST" ||
        upper_name == "COALESCE") {
        DataType unified = DataType::Unknown;
        for (const DataType a : args) {
            unified = unify_type(unified, a);
        }
        return {unified, true};
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
    if (upper_name == "COALESCE") {
        // NOT NULL iff any argument is known not-null (in particular a not-null
        // final default guarantees a non-NULL result).
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

    // Target column list: an explicit ColumnList child, else the table's columns
    // in declaration order. `covered` tracks which table columns receive a value
    // (for the NOT NULL check).
    std::vector<const ColumnInfo*> target_cols;
    std::vector<bool> covered(table->columns.size(), false);
    if (ASTNode* col_list = find_child(insert_stmt, NodeType::ColumnList)) {
        for (ASTNode* c = first_child(col_list); c != nullptr; c = c->next_sibling) {
            const std::string_view col_name = split_column_ref(c->primary_text).column;
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
                }
            }
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

    // SET assignments: each is a BinaryExpr whose primary_text is the target
    // column name and whose (first) child is the value expression.
    if (ASTNode* set_clause = find_child(update_stmt, NodeType::SetClause)) {
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
            }
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

std::vector<ResolvedColumn> Analyzer::analyze_query(ASTNode* select_stmt, Scope* parent) {
    Scope scope(parent);

    // 1. CTEs: register each definition as a named relation before resolving
    //    FROM, so the body can reference the current scope and earlier CTEs.
    if (ASTNode* cte_clause = find_child(select_stmt, NodeType::CTEClause)) {
        for (ASTNode* def = first_child(cte_clause); def != nullptr; def = def->next_sibling) {
            if (def->node_type != NodeType::CTEDefinition) {
                continue;
            }
            ASTNode* body = find_child(def, NodeType::SelectStmt);
            NamedRelation cte;
            cte.name = std::string{def->primary_text};
            if (body != nullptr) {
                cte.columns = analyze_query(body, &scope);
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
    if (ASTNode* select_list = find_child(select_stmt, NodeType::SelectList)) {
        for (ASTNode* item = first_child(select_list); item != nullptr;
             item = item->next_sibling) {
            if (item->node_type == NodeType::Star) {
                // `*` / `table.*` -> concrete columns in FROM/JOIN order.
                expand_star(item, scope, output);
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
        for (ASTNode* key = first_child(group_by); key != nullptr; key = key->next_sibling) {
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
                const std::string_view name = split_column_ref(item->primary_text).column;
                for (const auto& col : output) {
                    if (col.name == name) {
                        out_match = &col;
                        break;
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
    analyze_grouping(select_stmt, group_by, scope);

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

void Analyzer::resolve_from(ASTNode* from_clause, Scope& scope) {
    for (ASTNode* item = first_child(from_clause); item != nullptr; item = item->next_sibling) {
        resolve_from_item(item, scope);
    }
}

void Analyzer::resolve_from_item(ASTNode* item, Scope& scope) {
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
            if (ASTNode* body = find_child(item, NodeType::SelectStmt)) {
                binding.columns = analyze_query(body, &scope);
            }
            scope.add_relation(std::move(binding));
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
            // side is the relations already in scope before this join
            // ([0, left_end)); the right side is those just added
            // ([left_end, right_end)). Marked before predicates / SELECT resolve.
            switch (join_null_side(item)) {
                case JoinNullSide::Right:
                    scope.mark_join_nullable(left_end, right_end);
                    break;
                case JoinNullSide::Left:
                    scope.mark_join_nullable(0, left_end);
                    break;
                case JoinNullSide::Both:
                    scope.mark_join_nullable(0, right_end);
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
        // the right-hand copy so a bare reference is not ambiguous.
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
        // A reference that resolved in an enclosing scope makes the subquery we
        // are currently analyzing correlated (no diagnostic: this is legal).
        if (res.from_outer && corr_sink_ != nullptr) {
            *corr_sink_ = true;
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

DataType Analyzer::infer_expr(ASTNode* expr, Scope& scope) {
    if (expr == nullptr) {
        return DataType::Unknown;
    }

    switch (expr->node_type) {
        // A non-NULL literal is, by construction, not-null; a NULL literal is
        // nullable.
        case NodeType::IntegerLiteral:
            record_type(expr, DataType::Integer);
            record_nullability(expr, 1);
            return DataType::Integer;
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
            for (ASTNode* arg = first_child(expr); arg != nullptr; arg = arg->next_sibling) {
                if (arg->node_type == NodeType::WindowSpec) {
                    window_spec = arg;
                    continue;
                }
                const DataType t = infer_expr(arg, scope);
                if (arg->node_type != NodeType::Star) {
                    arg_types.push_back(t);
                    arg_nulls.push_back(null_of(arg));
                }
            }
            const std::string upper = to_upper(expr->primary_text);
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
            } else if (is_logical_op(op)) {
                result = DataType::Boolean;
            } else if (op == "||") {
                // String concatenation always yields text (operands are rendered
                // to their string form). Nullability is combined below like any
                // other binary operator.
                result = DataType::Text;
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
            }
            record_type(expr, result);
            // A binary result is nullable if either operand can be NULL.
            record_nullability(expr, combine_nullable_any({null_of(lhs), null_of(rhs)}));
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
                    const DataType tt = infer_expr(then_res, scope);
                    branch_types.push_back(tt);
                    branch_nulls.push_back(null_of(then_res));
                } else {
                    // Trailing bare expression: the ELSE result.
                    has_else = true;
                    const DataType et = infer_expr(c, scope);
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
            if (right != nullptr && (right->node_type == NodeType::Subquery ||
                                     right->node_type == NodeType::SubqueryExpr)) {
                // `expr IN (subquery)`: the subquery must project exactly one
                // column, type-compatible with the left operand.
                const std::vector<ResolvedColumn> proj = analyze_subquery(right, scope);
                if (proj.size() != 1) {
                    add_diagnostic(DiagnosticCode::InSubqueryColumns,
                                   "subquery on the right of IN projects " +
                                       std::to_string(proj.size()) +
                                       " columns; exactly one is required",
                                   expr);
                } else {
                    const Coercion c = coerce(lt, proj.front().type,
                                              CoercionKind::Comparison);
                    if (c.status != CoercionStatus::Ok) {
                        add_diagnostic(DiagnosticCode::ImplicitCoercion,
                                       "implicit coercion between IN operand and "
                                       "subquery column of a different type category",
                                       expr, Severity::Warning);
                    }
                }
            } else {
                // `expr IN (list)`: infer each list element (resolves columns).
                for (ASTNode* c = right; c != nullptr; c = c->next_sibling) {
                    infer_expr(c, scope);
                }
            }
            record_type(expr, DataType::Boolean);
            record_nullability(expr, null_of(left));
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

        // A bind parameter ($1 / ?): its type is not known until the statement is
        // bound, so it is Unknown and unifies with anything (wildcard). Marked
        // nullable since a parameter may be bound to NULL.
        case NodeType::Parameter: {
            record_type(expr, DataType::Unknown);
            record_nullability(expr, 2);
            return DataType::Unknown;
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

    // Track correlation for this subquery: while analyzing its body, point the
    // correlation sink at a local flag so a column that resolves in an enclosing
    // scope sets it. Saved/restored so nested subqueries mark their own flag.
    bool correlated = false;
    bool* saved = corr_sink_;
    corr_sink_ = &correlated;
    std::vector<ResolvedColumn> proj;
    if (inner != nullptr) {
        proj = analyze_stmt(inner, &enclosing);
    }
    corr_sink_ = saved;

    correlated_[subquery] = correlated;
    if (correlated) {
        subquery->set_flag(ast::NodeFlags::IsCorrelated);
    }
    return proj;
}

namespace {

// Does the subtree rooted at `node` contain an aggregate function call?
[[nodiscard]] bool contains_aggregate(const ASTNode* node) {
    if (node == nullptr) {
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
        if (contains_aggregate(c)) {
            return true;
        }
    }
    return false;
}

// Does a column reference `ref` match the grouping key `k`? Prefer resolved
// (table_id, column_id) identity; fall back to the reference text when either
// side is unresolved (e.g. derived columns or expression keys).
[[nodiscard]] bool key_matches(const GroupKey& k, const ASTNode* ref) {
    const std::uint32_t tid = ref->context.analysis.table_id;
    const std::uint32_t cid = ref->context.analysis.column_id;
    if (cid != 0 && k.column_id != 0) {
        return tid == k.table_id && cid == k.column_id;
    }
    return ref->primary_text == k.text;
}

}  // namespace

void Analyzer::analyze_grouping(ASTNode* select_stmt, ASTNode* group_by, Scope& scope) {
    (void)scope;  // resolution already happened; this pass is structural.

    ASTNode* select_list = find_child(select_stmt, NodeType::SelectList);

    // The query is "grouped" if it has a GROUP BY clause or any aggregate in the
    // SELECT list. Only grouped queries are subject to the legality rules.
    bool grouped = group_by != nullptr;
    if (!grouped && select_list != nullptr) {
        for (ASTNode* item = first_child(select_list); item != nullptr;
             item = item->next_sibling) {
            if (contains_aggregate(item)) {
                grouped = true;
                break;
            }
        }
    }
    if (!grouped) {
        return;
    }

    // Collect the grouping keys (their resolved identity + text).
    std::vector<GroupKey> keys;
    if (group_by != nullptr) {
        for (ASTNode* key = first_child(group_by); key != nullptr; key = key->next_sibling) {
            GroupKey k;
            k.table_id = key->context.analysis.table_id;
            k.column_id = key->context.analysis.column_id;
            k.text = key->primary_text;
            keys.push_back(k);
        }
    }

    // SELECT list and ORDER BY: every non-aggregated column must be a group key.
    if (select_list != nullptr) {
        for (ASTNode* item = first_child(select_list); item != nullptr;
             item = item->next_sibling) {
            check_grouping_expr(item, keys, /*in_aggregate=*/false);
        }
    }
    if (ASTNode* order_by = find_child(select_stmt, NodeType::OrderByClause)) {
        for (ASTNode* item = first_child(order_by); item != nullptr; item = item->next_sibling) {
            check_grouping_expr(item, keys, /*in_aggregate=*/false);
        }
    }
    // HAVING may reference grouping keys and aggregates; a bare non-grouped
    // column outside an aggregate is illegal here too.
    if (ASTNode* having = find_child(select_stmt, NodeType::HavingClause)) {
        if (ASTNode* pred = first_child(having)) {
            check_grouping_expr(pred, keys, /*in_aggregate=*/false);
        }
    }
}

void Analyzer::check_grouping_expr(ASTNode* expr, const std::vector<GroupKey>& keys,
                                   bool in_aggregate) {
    if (expr == nullptr) {
        return;
    }

    // Do not descend into a nested subquery: it is a distinct query block with
    // its own FROM scope and its own grouping analysis. Its columns (including a
    // correlated reference to an OUTER block's column) are not columns of THIS
    // grouped relation, so the GROUP BY legality rule here does not apply to
    // them. Crossing this boundary would spuriously flag them as non-grouped.
    if (expr->node_type == NodeType::Subquery ||
        expr->node_type == NodeType::SubqueryExpr) {
        return;
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
        // Columns beneath an aggregate or inside a window function are exempt.
        const bool inside = in_aggregate || is_agg || windowed;
        for (ASTNode* c = first_child(expr); c != nullptr; c = c->next_sibling) {
            check_grouping_expr(c, keys, inside);
        }
        return;
    }

    if (is_column_ref_node(expr->node_type)) {
        if (!in_aggregate) {
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
        check_grouping_expr(c, keys, in_aggregate);
    }
}

}  // namespace db25::semantic
