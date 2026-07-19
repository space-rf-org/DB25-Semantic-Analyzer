# DB25 Semantic Analyzer

A small C++23 library that consumes the [DB25 SQL parser](https://github.com/space-rf-org/db25-sql-parser)
AST and performs **name resolution** and **basic type inference**: it resolves
`FROM` tables and aliases, derived tables and CTEs, and column references
against an injectable catalog, then infers types for literals, column
references, comparisons and arithmetic.

See [`docs/DESIGN.md`](docs/DESIGN.md) for the pass pipeline, how results map
onto the parser's node fields, and the AST conventions this analyzer relies on.

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
include/db25/semantic/   public headers (catalog, scope, diagnostic, analyzer, ast_helpers)
src/analyzer.cpp         analyzer implementation
tests/test_analyzer.cpp  assertion-based test suite
docs/DESIGN.md           design and AST-convention notes
```
