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
   | Pass 3: Validation (FUTURE)                                  |
   |   aggregate/group-by consistency, set-op arity, coercion     |
   |   legality, subquery cardinality, etc. Not yet implemented.  |
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

## Roadmap / not yet implemented

* `SELECT *` expansion across visible relations (stubbed; skipped today).
* `JOIN` ON/USING predicate resolution (join relations are made visible, but the
  predicate is not yet resolved as its own pass).
* Set operations (UNION/INTERSECT/EXCEPT) column-arity/type reconciliation.
* Aggregate / GROUP BY / HAVING legality checking (validation pass).
* Implicit type coercion rules and function-signature typing.
