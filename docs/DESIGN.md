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

## Roadmap / not yet implemented

* Subquery cardinality checks and correlated-aggregate scoping.
* Expression-valued GROUP BY keys matched to SELECT expressions (currently
  keyed by resolved column identity, or reference text as a fallback).
* Set operations or qualified stars nested inside a FROM subquery / CTE body
  (the current derived-table path analyzes a `SelectStmt` body only).
