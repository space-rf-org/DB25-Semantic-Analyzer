# DB25 Semantic Analyzer

A C++23 library that consumes the [DB25 SQL parser](https://github.com/space-rf-org/db25-sql-parser)
AST and resolves it into a typed, checked query, in place on the AST nodes:

- **Name resolution** — `FROM` tables and aliases, derived tables, CTEs
  (including recursive), and correlated / LATERAL scopes; column references
  resolved against an injectable catalog, with ambiguity and unresolved-column
  diagnostics.
- **Type + nullability inference** — a `DataType` and a 2-bit nullability for
  every expression node: literals (typed by magnitude), comparisons, arithmetic
  (with a constant integer-overflow diagnostic), CASE / COALESCE, casts,
  set-operation and `USING`/`NATURAL` merged-column reconciliation, and the
  output **projection** of each query block (`projection_of`, which a downstream
  binder must agree with).
- **Legality checks** — aggregate/window placement (WHERE / HAVING / JOIN ON),
  grouped-column and `DISTINCT`-`ORDER BY` rules, grouping-set (ROLLUP / CUBE /
  GROUPING SETS) key nullability and the `GROUPING()` indicator, `RETURNING`
  legality, and more — reported as structured `Diagnostic`s.

It also snapshots DDL into the catalog so later statements resolve against it.

See [`docs/DESIGN.md`](docs/DESIGN.md) for the pass pipeline, how results map
onto the parser's node fields, and the AST conventions this analyzer relies on.
To *see* the analyzer's resolved AST (types + nullability + catalog ids) as a
canonical s-expression end to end, use the umbrella
[`db25`](https://github.com/space-rf-org/db25) harness — its staged fixtures
carry a `-- resolved` section per statement.

## Building

The analyzer consumes the parser through its installed CMake package. First
build and install the parser to a prefix, then point this project at it:

```sh
# 1. Build + install the parser (portable / non-native build)
git clone https://github.com/space-rf-org/db25-sql-parser
cd db25-sql-parser && git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
      -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_INSTALL_PREFIX=/path/to/parser-install
cmake --build build -j && cmake --install build

# 2. Build the analyzer + tests
cd /path/to/db25-semantic-analyzer
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 \
      -DCMAKE_PREFIX_PATH=/path/to/parser-install
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires C++23 (g++-14 or newer).

## Usage sketch

```cpp
#include "db25/parser/parser.hpp"
#include "db25/semantic/analyzer.hpp"
#include "db25/semantic/catalog.hpp"

using namespace db25;

semantic::InMemoryCatalog catalog;
catalog.add_table("users", {
    {"id", ast::DataType::Integer, /*nullable=*/false},
    {"name", ast::DataType::Text},
});

parser::Parser parser;                       // keep alive while using the AST
auto parsed = parser.parse("SELECT u.id FROM users u WHERE u.id = 1");

semantic::Analyzer analyzer(catalog);
analyzer.analyze(parsed.value());

for (const auto& d : analyzer.diagnostics()) {
    // report d.message at [d.source_start, d.source_end)
}
```

## Layout

```
include/db25/semantic/   public headers (catalog, scope, diagnostic, analyzer, ast_helpers, ...)
src/analyzer.cpp         analyzer core (name resolution, type/nullability, legality)
src/ddl.cpp              DDL -> catalog snapshotting
src/check_eval.cpp       CHECK-constraint / constant folding evaluator
src/{catalog_snapshot,transaction}.cpp
tests/                   assertion-based suites (semantic, catalog, ddl, transaction);
                         run all via `ctest --test-dir build`
docs/DESIGN.md           design and AST-convention notes
```
