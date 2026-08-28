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
    // An aggregate function call appears in a WHERE clause. Aggregates are
    // evaluated after grouping, so they belong in HAVING, not WHERE.
    AggregateInWhere,
    // An aggregate function call appears in a JOIN ON condition. A join
    // predicate is a filter evaluated BEFORE aggregation - the same position as
    // WHERE - so an aggregate is illegal there (Postgres: "aggregate functions
    // are not allowed in JOIN conditions").
    AggregateInJoinCondition,
    // A GROUP BY key is (or contains) a non-windowed aggregate, e.g.
    // `GROUP BY MAX(age)`. Grouping is what produces aggregates, so an aggregate
    // cannot itself be a grouping key (Postgres: "aggregate functions are not
    // allowed in GROUP BY").
    AggregateInGroupBy,
    // A window function appears where it is not allowed: a WHERE or HAVING
    // predicate, or a JOIN ON condition. Windows are computed AFTER the WHERE
    // filter and AFTER grouping / HAVING, so a window call can never be a WHERE,
    // HAVING, or join-condition term (Postgres: "window functions are not
    // allowed in WHERE" / "... in HAVING" / "... in JOIN conditions").
    WindowNotAllowed,
    // The same correlation name (table alias, or table name when unaliased) is
    // specified more than once in a single FROM clause.
    DuplicateRelation,
    // A target column is named more than once in an INSERT column list.
    DuplicateColumn,
    // An expression is nested more deeply than the analyzer will recurse (e.g. a
    // chain of many thousands of operators, which the parser does not bound).
    // Analysis of the over-deep subtree is abandoned to avoid a stack overflow.
    ExpressionTooComplex,
    // A DML row makes a table CHECK constraint evaluate to FALSE (a definite
    // violation): an INSERT whose supplied and defaulted values are all constant,
    // or an UPDATE whose SET assignments are. Only reported when every column the
    // predicate references folds to a constant, so the result is certain.
    CheckViolation,
    // A derived table's column-alias list "(a, b, ...)" names more columns than
    // the derived table produces.
    ColumnAliasCountMismatch,
    // A VALUES list has rows of differing lengths (e.g. `(VALUES (1, 2), (3))`).
    // Every row must supply the same number of values; the relation's width is
    // set by the first row.
    ValuesRowArityMismatch,
    // A column of a multi-row VALUES has incompatible types across rows (e.g.
    // `(VALUES (1), ('x'))`). A multi-row VALUES is a UNION ALL of its rows, so
    // each column's type must reconcile across every row.
    ValuesColumnTypeMismatch,
    // The recursive term of a WITH RECURSIVE CTE produces a column whose type is
    // not the same as (nor coercible to) the anchor (non-recursive) term's type
    // for that column - the recursive term would WIDEN the column past the type
    // the anchor fixes. SQL takes the CTE's column types from the anchor term and
    // requires the recursive term to conform (Postgres: `recursive query "t"
    // column N has type X in non-recursive term but type Y overall`).
    RecursiveTypeMismatch,
    // The recursive term of a WITH RECURSIVE CTE references the CTE itself more
    // than once (non-linear recursion, e.g. `FROM t a, t b`). SQL permits only a
    // single self-reference (Postgres: `recursive reference to query "t" must not
    // appear more than once`); a non-linear reference has no defined fixpoint.
    RecursiveReferenceNotLinear,
    // A constant division or modulo by zero (`1 / 0`, `x % 0`) in a reachable
    // expression position. PostgreSQL folds such a constant at plan time and
    // errors; DB25 deliberately DIVERGES - it treats `1/0` as a legal runtime
    // operation the optimizer preserves - so this is a soft WARNING, not an
    // error, and the statement still analyzes/binds/optimizes cleanly. Suppressed
    // inside a CASE arm that constant-fold analysis proves unreachable (a
    // constant-false guard, or an arm after a constant-true guard), so
    // `CASE WHEN 1=0 THEN 1/0 ...` is silent while `CASE WHEN i>100 THEN 1/0 ...`
    // gets the advisory warning.
    DivisionByZero,
    // A constant integer arithmetic expression (+ - *) whose value overflows the
    // range of its result integer type, e.g. `2147483647 + 1` (int -> out of int4
    // range) or `9223372036854775807 + 1` (bigint -> out of int8 range).
    // PostgreSQL does NOT implicitly widen intN arithmetic and raises "integer /
    // bigint out of range" at runtime; DB25 has no executor, so it diagnoses the
    // provable (constant) cases. Kept a soft WARNING that flows through, exactly
    // like DivisionByZero (the same constant-arithmetic-fault family and the same
    // dead-CASE-arm suppression), rather than a hard error.
    IntegerOverflow,
    // A positional ORDER BY item (`ORDER BY n`) whose ordinal is <= 0 or greater
    // than the number of SELECT output columns. SQL numbers output columns from 1
    // (Postgres: "ORDER BY position N is not in select list" / "ORDER BY position
    // must be > 0"). GROUP BY positions are validated the same way.
    InvalidOrderByPosition,
    // EXISTS / NOT EXISTS applied to something other than a subquery (e.g.
    // `EXISTS 5`, `EXISTS ((SELECT 1) + 2)`). The parser is deliberately lenient
    // about the operand shape, so the "EXISTS requires a subquery" rule is
    // enforced here: without it a scalar EXISTS analyzed clean but could not be
    // lowered (Postgres: `EXISTS` takes a `(subquery)`).
    ExistsWithoutSubquery,
    // A FILTER (WHERE ...) clause on a non-aggregate function, e.g.
    // `abs(x) FILTER (WHERE ...)`. FILTER is only defined for aggregates (and
    // aggregates used as window functions) (Postgres: "FILTER specified, but X
    // is not an aggregate function").
    FilterOnNonAggregate,
    // A window function appears inside an aggregate function's argument, e.g.
    // `sum(row_number() OVER ())`. Windowing runs AFTER aggregation, so a window
    // result can never feed an aggregate (Postgres: "aggregate function calls
    // cannot contain window function calls").
    WindowInAggregate,
    // An aggregate or window function appears in a RETURNING list, e.g.
    // `INSERT ... RETURNING count(*)`. RETURNING projects the individual
    // affected rows; it has no grouping or window framing, so set functions are
    // not allowed there (Postgres: "aggregate/window functions are not allowed
    // in RETURNING").
    AggregateInReturning,
    WindowInReturning,
    // Under SELECT DISTINCT, an ORDER BY item references a column that is not in
    // the select list, e.g. `SELECT DISTINCT dept FROM emp ORDER BY sal`.
    // DISTINCT collapses the visible row to the select list, so a sort key must
    // be composed only of selected items - a hidden sort column would change
    // the distinct key (Postgres: "for SELECT DISTINCT, ORDER BY expressions
    // must appear in select list"). The analyzer's plain-SELECT ORDER BY
    // fallback resolves such a key against the FROM scope with no DISTINCT
    // awareness, so the binder would reject what the analyzer accepted.
    OrderByNotInSelectDistinct,
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
