# DB25 Semantic Analyzer - Design

The semantic analyzer is a small C++23 library that consumes the AST produced
by the [DB25 SQL parser](https://github.com/space-rf-org/db25-sql-parser) and
performs **name resolution** and **basic type inference** over it. It links the
parser through its installed CMake package (`find_package(DB25Parser REQUIRED)`
→ `DB25::db25parser`).

## Pass pipeline

The analyzer is organized as a sequence of passes over the parser's
`ast::ASTNode` tree. Each pass consumes the tree (and the catalog) and enriches
the nodes and/or produces diagnostics.

```
              +------------------+
   SQL text ->|   DB25 parser    |-> ast::ASTNode* (arena-owned)
              +------------------+
                       |
                       v
   +-------------------------------------------------------------+
   | Pass 0: Catalog / schema interface        (semantic::Catalog)|
   |   In-memory, injectable. Maps table name -> typed columns.   |
   +-------------------------------------------------------------+
                       |
                       v
   +-------------------------------------------------------------+
   | Pass 1: Name & scope resolution           (semantic::Scope)  |
   |   * FROM tables / aliases -> relation bindings               |
   |   * derived tables (subquery in FROM) -> computed columns    |
   |   * CTE names -> named relations                             |
   |   * column refs (bare and alias.col) -> ResolvedColumn       |
   |   * child scope per subquery (correlation resolves outward)  |
   |   Emits diagnostics for unresolved tables / columns.         |
   +-------------------------------------------------------------+
                       |
                       v
   +-------------------------------------------------------------+
   | Pass 2: Type inference                                       |
   |   * literals -> Integer / Double / Text / Boolean / Null     |
   |   * column refs -> catalog column type                       |
   |   * comparison / logical ops -> Boolean                      |
   |   * arithmetic on numerics -> promoted numeric type          |
   +-------------------------------------------------------------+
                       |
                       v
   +-------------------------------------------------------------+
   | Pass 3: Validation                                          |
   |   * set-op arity / type reconciliation                       |
   |   * aggregate / GROUP BY / HAVING legality                   |
   |   * function-signature typing + implicit numeric coercion    |
   |   (subquery cardinality etc. still future)                   |
   +-------------------------------------------------------------+
```

In the current implementation passes 1 and 2 are fused into a single recursive
walk (`Analyzer::analyze_query`): resolving a column reference and inferring its
type happen together, because the inferred type of a column *is* its resolved
catalog type. They are described separately above because they are conceptually
distinct and will separate as validation grows.

## Where results are recorded

Analysis results are written in **two** places, deliberately:

1. **On the parser node** (`ast::ASTNode`), mapping onto the fields the parser
   reserves for semantic analysis:
   * `data_type` (`ast::DataType`) - the inferred type of any typed expression
     node (literal, column reference, comparison, arithmetic, ...).
   * the `ContextData::AnalysisContext` union, for resolved **column
     references**:
     * `table_id`  - catalog id of the owning table,
     * `column_id` - catalog id of the column within that table,
     * `nullability` - `1` = NOT NULL, `2` = nullable (`0` = unknown), matching
       the parser's documented 2-bit encoding.

   Writing into `context.analysis` is valid because the parser is constructed in
   `ParserMode::Production` (the default), which selects the analysis arm of the
   modal context union.

2. **In a side map** (`std::unordered_map<const ASTNode*, DataType>`) inside the
   `Analyzer`, read back via `Analyzer::type_of(node)`. The side map is the
   authoritative, allocation-free-to-query record used by tests and callers; the
   on-node fields are a convenience mirror for consumers that walk the AST
   directly (e.g. an optimizer). Keeping both means we never depend on the
   parser leaving those node fields writable, while still populating them when
   they are.

Diagnostics are collected in a flat `std::vector<Diagnostic>`; each diagnostic
carries the offending node's `source_start` / `source_end` byte range so a
front-end can underline the exact text.

### Resolved projections (`SELECT *` / set operations)

A query block's **output projection** - the ordered list of columns it produces,
with `SELECT *` / `table.*` already expanded and set-operation column types
reconciled - is recorded in a side map keyed by the query node
(`std::unordered_map<const ASTNode*, std::vector<ResolvedColumn>>`), read back
via `Analyzer::projection_of(node)`. The key is the `SelectStmt` for a plain
query and the `UnionStmt` / `IntersectStmt` / `ExceptStmt` node for a set
operation. This is a *side structure*: we deliberately do not synthesize extra
`ColumnRef` nodes into the parser's arena AST to represent expanded stars, both
because the arena is parser-owned and because a resolved `(name, type,
nullable, table_id, column_id)` list is exactly what downstream consumers
(derived-table column lists, optimizers) need. `analyze_query` / `analyze_setop`
also *return* this list so it flows into derived-table and CTE column sets.

## AST conventions relied upon (KNOWN COUPLINGS)

These are conventions of the *current* parser AST that the analyzer depends on.
They are called out here because they are the things most likely to change.

### Aliases live in `schema_name` (primary coupling)

The parser stores the alias of a **table reference** and of a **derived table**
(a subquery in `FROM`) in `ASTNode::schema_name`, and is intended to flag it
with `NodeFlags::HasAlias` in `semantic_flags`. Examples (verified against the
consumed parser build):

```
SELECT u.id FROM users u
  FromClause
    TableRef  primary_text="users"  schema_name="u"     <- alias in schema_name

SELECT t.x FROM (SELECT id AS x FROM users) t
  FromClause
    Subquery  schema_name="t"                            <- alias in schema_name
      SelectStmt ...
```

Two caveats we handle:

* The consumed parser build does **not** reliably set `NodeFlags::HasAlias`
  (observed `flags == 0` even when an alias is present). We therefore treat any
  non-empty `schema_name` on a table/subquery reference as the alias.
* `schema_name` is genuinely overloaded: for a *schema-qualified* table
  (`myschema.users`) it would hold the schema, not an alias. Disambiguating that
  from an alias is exactly what `HasAlias` is meant for. Until the flag is
  reliable (or a dedicated alias field exists), the analyzer assumes
  `schema_name` on a `FROM` item is a correlation alias.

**This coupling is centralized in exactly one place:** `alias_of(node)` in
`include/db25/semantic/ast_helpers.hpp`. If the parser moves aliases to a
dedicated field, only that function changes. **Reconciliation TODO:** once the
parser guarantees `HasAlias` or adds a dedicated alias field, update
`alias_of()` to read it and to distinguish schema-qualification from aliasing.

### Column references carry the dotted name in `primary_text`

A column reference stores its (possibly qualified) name as text in
`primary_text`: `"u.id"`, `"t.x"`, or bare `"id"`. The analyzer splits on the
last `.` (`split_column_ref()` in `ast_helpers.hpp`) into an optional qualifier
and the column name. The `AS`-alias of a *select item* is likewise carried in
`schema_name` (`SELECT id AS x` → `ColumnRef primary_text="id" schema_name="x"`),
which is how derived-table output column names are recovered.

### Other conventions

* Nodes expose `node_type` (`NodeType` enum), `first_child` / `next_sibling`
  (children traversed via the sibling chain), `source_start` / `source_end`.
* Literals are **not** typed by the parser (`data_type == Unknown`); the
  analyzer assigns their types.
* **Lifetime:** node text (`primary_text`, `schema_name`) is `string_view` into
  the `Parser`'s arena. The `Parser` instance must outlive any use of its AST
  and of an `Analyzer` built over it. The analyzer copies nothing it does not
  need but stores node *pointers* in its side map, so it too is only valid while
  the parser lives.

## Components

| Component | Header | Role |
|-----------|--------|------|
| `Catalog` / `InMemoryCatalog` | `catalog.hpp` | Injectable schema: table name → typed columns, with table/column ids. |
| `Scope` | `scope.hpp` | Lexical scope stack: visible relations (+aliases), CTE registry, qualified/bare column resolution walking outward through parents. |
| `Diagnostic` | `diagnostic.hpp` | Error record with code, message, and source range. |
| `Analyzer` | `analyzer.hpp` | Drives the walk; produces diagnostics + inferred types. |
| AST helpers | `ast_helpers.hpp` | `alias_of`, `split_column_ref`, child traversal - the coupling surface. |

## Scoping model

Each SELECT query block gets its own `Scope`, whose `parent` is the enclosing
block's scope (or `nullptr` at the top). Resolution walks **outward**: a column
is looked up in the current scope's relations, then the parent's, and so on,
which is what makes correlated subqueries resolve. CTEs are registered on the
scope in which the `WITH` appears and are found by `FROM` items via
`Scope::find_cte`, also walking outward. A derived table is analyzed as a nested
query block whose projected columns become the columns of the enclosing relation
binding.

## Projection, joins, and set operations

Three capabilities built on the passes above:

### `SELECT *` / `table.*` expansion

A `Star` select-list item is expanded, in FROM/JOIN order, into the concrete
columns of the relations visible in the *current* query block
(`Scope::relations()`, not outer scopes). An unqualified `*` expands every
visible relation; a qualified `table.*` expands only the relation whose alias /
name matches the qualifier. The expanded columns land in the query's resolved
projection (above). Diagnostics: `StarWithoutFrom` when `*` appears with no FROM
relations; `UnresolvedQualifier` when `table.*`'s qualifier names no visible
relation.

> **Parser limitation (qualified star).** The consumed parser build does not
> represent `table.*` in a usable way. Immediately before `FROM` it misparses
> `t.*` as a `*` (multiplication) `BinaryExpr` and *drops the entire FROM
> clause*; before a comma it collapses `t.*` to a bare `ColumnRef` holding only
> the qualifier text, discarding the `.*`. Neither yields a qualified-star node.
> The analyzer's qualified-star path is therefore written against the *correct*
> forward-compatible shape - a `Star` node whose `schema_name` holds the
> qualifier - so it works unchanged once the parser emits that shape. Unqualified
> `SELECT *` parses correctly (a clean `Star` node) and is fully supported.

### JOIN `ON` / `USING` resolution

For a `JoinClause`, the left side is the FROM sibling already in scope; the
right side's relations are brought into scope first, then the predicate is
resolved against both. An `ON` expression is resolved with the ordinary
expression walk (`infer_expr`), so its column references emit the same
unresolved-column / ambiguous diagnostics. For `USING (col, ...)`, each named
column must be present in *both* the left relation set and the right relation
set (tracked by relation index ranges); a `UsingColumnMissing` diagnostic is
emitted otherwise. A resolved USING column is recorded once (the merged output
column).

### Set-operation type reconciliation

`UnionStmt` / `IntersectStmt` / `ExceptStmt` analyze each branch (a SELECT block
or a nested set operation), then fold the branches left-to-right: branches must
have equal **arity** (`SetOpArityMismatch` otherwise) and pairwise **compatible
types** (`SetOpTypeMismatch` otherwise). Compatibility is conservative - exact
match, `NULL`/`Unknown` unify with anything, and any two numeric types unify by
numeric promotion; everything else is incompatible. The reconciled types become
the set operation's output projection.

## Aggregates, GROUP BY / HAVING legality, and function typing

Built on the passes above and driven from `analyze_query` after FROM / SELECT /
WHERE have been resolved.

### Function & coercion typing

A `FunctionCall` (the parser also has `FunctionExpr`) carries its name in
`primary_text` and its arguments as children; `COUNT(*)` has a single `Star`
child and `DISTINCT` sets `NodeFlags::Distinct`. A bare column that is the direct
argument of a function is emitted as an `Identifier` (not a `ColumnRef`), so
`infer_expr` resolves both node kinds the same way. `infer_expr` types a call
from a small, extensible signature table (`function_result_type`):

* **Aggregates** (`kAggregateNames` = COUNT, SUM, AVG, MIN, MAX — add a name
  there to extend recognition everywhere): `COUNT → BigInt`; `SUM →` input type,
  integer inputs widened to `BigInt`; `AVG → Double`; `MIN`/`MAX →` argument
  type.
* **Scalars**: `UPPER`/`LOWER → Text`, `LENGTH → Integer`, `ABS →` numeric arg
  type, `COALESCE →` its arguments unified via `reconcile_types` (the same
  conservative numeric-promotion / NULL-unification rule used for set
  operations).
* **Unknown** names degrade to `Unknown` and raise a soft `UnknownFunction`
  **Warning** (never an error, so `has_errors()` is unaffected).

### GROUP BY / HAVING legality

A query is *grouped* if it has a `GroupByClause` or any aggregate in the SELECT
list. `analyze_grouping` collects the grouping keys (as resolved
`(table_id, column_id)` identity, with the reference text as a fallback for
unresolved / derived keys) and walks the SELECT list, ORDER BY, and HAVING:

* a column reference **outside** any aggregate that is not a grouping key →
  `NonGroupedColumn`;
* an aggregate nested directly inside another aggregate → `NestedAggregate`.

Columns beneath an aggregate are exempt from the grouping rule. GROUP BY and
ORDER BY expressions are resolved against the FROM scope through the ordinary
`infer_expr` path, so they emit the usual unresolved-column diagnostics.

## Nullability propagation

Every typed expression node also carries a **nullability** in the parser's 2-bit
encoding (`0` = unknown, `1` = not-null, `2` = nullable), written both onto
`context.analysis.nullability` and into an analyzer side map read back via
`Analyzer::nullability_of(node)`. `infer_expr` computes it alongside the type:

* a non-NULL literal is not-null; a `NULL` literal is nullable;
* a column reference takes its catalog column's `nullable` flag;
* arithmetic, comparison, and ordinary scalar functions are *nullable if any
  operand is nullable* (`combine_nullable_any`), not-null only when every operand
  is known not-null;
* `COUNT` is not-null; `SUM`/`AVG`/`MIN`/`MAX` are nullable (empty group → NULL);
  `COALESCE` is not-null iff **any** argument is not-null (`function_nullability`);
* a scalar subquery is nullable (it may return no rows); `EXISTS` is not-null.

**Outer-join nullability.** A relation on the null-supplying side of an outer
join has every column made nullable regardless of its base `NOT NULL`. This is
tracked with a `RelationBinding::nullable_from_join` flag set when the join is
resolved (`join_null_side`: `LEFT` → right side, `RIGHT` → left side, `FULL` →
both, `INNER`/`CROSS` → neither), keyed off the join node type or, for a
`JoinClause`, its `primary_text` (`"LEFT JOIN"`, …). `Scope::resolve_bare` /
`resolve_qualified` OR that flag into the resolved column's nullability, and
`SELECT *` expansion does the same, so `o.id` in
`users u LEFT JOIN orders o` resolves nullable even when `orders.id` is `NOT NULL`,
while the preserved side (`u.id`) stays not-null.

## Type coercion model

The previous conservative set-op reconcile helper is generalized into a single
`coerce(a, b, kind)` model (`CoercionKind` = `Comparison` / `Arithmetic` /
`UnionReconcile` / `Assignment`) returning `{status, type}` where status is `Ok`,
`ImplicitCoercion`, or `Incompatible`. Rules: identical types and
`NULL`/`Unknown`/`ANY` wildcards unify cleanly; numeric types unify by promotion
(`Integer < BigInt < Decimal < Double`); `char`/`varchar`/`text` unify to `text`;
different temporals, boolean-vs-other, and other cross-category pairings do not.
The *kind* only changes what a non-unifiable pairing means:

* **comparison** → allowed as a soft `ImplicitCoercion` **warning** (never trips
  `has_errors()`); numeric-vs-numeric and string-vs-string stay clean;
* **arithmetic** → a hard `TypeMismatch` **error** (e.g. `text + integer`);
* **union-reconcile** → `SetOpTypeMismatch` (unchanged behavior);
* **assignment** (INSERT value → column, UPDATE `SET` value) → a numeric↔string
  pairing is a soft `ImplicitCoercion` **warning** (e.g. a text literal into an
  integer column); any other cross-category pairing is a `TypeMismatch` **error**
  (e.g. a boolean into an integer column).


`BinaryExpr` comparison/arithmetic, set-operation column reconciliation, `IN`
type-compatibility, and `COALESCE` unification are all routed through it.

## Subquery correlation, scalar & IN subqueries

Subqueries appearing in **expression** position (a `Subquery`/`SubqueryExpr` used
as a scalar, on the right of `IN`, or under `EXISTS`) are analyzed by
`analyze_subquery`, which runs the inner query block under a **child scope** of
the enclosing query so unresolved-in-self references resolve outward (the scope
stack already walks parents). A column that resolves in an enclosing scope
(`ColumnResolution::from_outer`) marks the subquery **correlated** — recorded in a
side map (`Analyzer::is_correlated`) and mirrored as `NodeFlags::IsCorrelated`,
with **no** diagnostic; a reference that resolves in neither scope is the usual
`UnresolvedColumn`.

* **Scalar subquery** (SELECT-list item / comparison operand): takes the type of
  its single projected column; projecting more than one column →
  `ScalarSubqueryColumns`.
* **`EXISTS (subquery)`** (parsed as a `UnaryExpr` with `primary_text` `EXISTS`):
  always `Boolean`, not-null, arity irrelevant, correlation allowed.
* **`expr IN (subquery)`** (an `InExpr` with a `Subquery` right child): the
  subquery must project exactly one column (`InSubqueryColumns` otherwise),
  type-compatible with the left operand via the coercion model (a cross-category
  pairing raises the `ImplicitCoercion` warning).

## DML statements (INSERT / UPDATE / DELETE)

`analyze()` dispatches `InsertStmt` / `UpdateStmt` / `DeleteStmt` alongside
`SelectStmt` and the set-operation nodes. All checks are additive and degrade to
"skip" on shapes not modeled.

* **INSERT** (`analyze_insert`): the target `TableRef` is resolved in the catalog
  (unknown → `UnresolvedTable`, and analysis stops — nothing to check against).
  The target column list is an explicit `ColumnList` child when present (each
  entry must be a column of the table, else `UnresolvedColumn`) or, implicitly,
  the table's columns in declaration order. Values come from either a
  `ValuesClause` (each row is a `Subquery`/`RowExpr` whose direct children are the
  value expressions) or a source query for `INSERT … SELECT`. Per row / per
  projection: the value count must equal the target column count
  (`InsertArityMismatch`), and each value's type is checked against its target
  column via the assignment coercion model. Finally, a NOT NULL target column
  that receives no value and has no default (`ColumnInfo::has_default`) →
  `NotNullViolation`.
* **UPDATE** (`analyze_update`): the target table is bound into a fresh scope
  (`bind_base_table`). Each `SetClause` assignment is a `BinaryExpr` whose
  `primary_text` is the target column (unknown → `UnresolvedColumn`) and whose
  first child is the value expression (type-checked against the column via the
  assignment model). The `WhereClause` predicate is resolved against the table
  scope.
* **DELETE** (`analyze_delete`): the target table is bound into scope and the
  `WhereClause` predicate is resolved against it.

`ColumnInfo` gained an optional `has_default` flag (defaults to `false`, placed
before `column_id` so `{name, type, nullable}` brace-init call sites are
unaffected).

### Parser-shape limitations (this consumed build)

The analyzer is written against the correct/forward-compatible shapes, but the
consumed parser build mis-parses three DML/clause constructs; the tests
synthesize the intended node shape to exercise the analyzer (as the qualified-
star tests already do):

* an **explicit INSERT column list** `INSERT INTO t (a, b) …` drops both the
  column list and the VALUES/SELECT source (leaving only the `TableRef`); the
  implicit-column-list forms parse correctly and are used for the parse-driven
  INSERT tests;
* a **DML `WHERE` predicate** is mis-parsed (the predicate collapses to a single
  `ColumnRef` whose text is the keyword `WHERE`), so WHERE-resolution tests
  synthesize the predicate.

## Roadmap / not yet implemented

* Subquery cardinality checks beyond the scalar/IN arity rules above, and
  correlated-aggregate scoping.
* Expression-valued GROUP BY keys matched to SELECT expressions (currently
  keyed by resolved column identity, or reference text as a fallback).
* Qualified stars nested inside a FROM subquery / CTE body.
