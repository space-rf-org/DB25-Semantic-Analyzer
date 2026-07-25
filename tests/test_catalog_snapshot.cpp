// DB25 Semantic Analyzer - persistent catalog snapshot tests.
//
// Self-contained assertion harness (same style as test_analyzer.cpp): every
// check updates a tally and the process exits non-zero if anything fails.

#include "db25/semantic/catalog.hpp"
#include "db25/semantic/catalog_snapshot.hpp"

#include <cstdio>
#include <optional>
#include <string>

using namespace db25;
using namespace db25::semantic;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL: %s (%s:%d)\n", expr, file, line);
    }
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

// Deep structural equality: version, id allocator, and every table/column field.
bool catalogs_equal(const InMemoryCatalog& a, const InMemoryCatalog& b) {
    if (a.schema_version() != b.schema_version()) return false;
    if (a.next_table_id() != b.next_table_id()) return false;
    const auto ta = a.tables();
    const auto tb = b.tables();
    if (ta.size() != tb.size()) return false;
    for (std::size_t i = 0; i < ta.size(); ++i) {
        if (ta[i]->table_id != tb[i]->table_id) return false;
        if (ta[i]->name != tb[i]->name) return false;
        if (ta[i]->columns.size() != tb[i]->columns.size()) return false;
        for (std::size_t j = 0; j < ta[i]->columns.size(); ++j) {
            const ColumnInfo& ca = ta[i]->columns[j];
            const ColumnInfo& cb = tb[i]->columns[j];
            if (ca.column_id != cb.column_id) return false;
            if (ca.name != cb.name) return false;
            if (ca.type != cb.type) return false;
            if (ca.nullable != cb.nullable) return false;
            if (ca.has_default != cb.has_default) return false;
        }
    }
    return true;
}

// A catalog exercising mixed types, null/notnull, with/without default, and a
// name containing a space (to pin the end-of-line name parsing).
InMemoryCatalog sample_catalog() {
    InMemoryCatalog cat;
    cat.add_table("users", {
        ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"name", DataType::VarChar, /*nullable=*/true},
        ColumnInfo{"created at", DataType::Timestamp, /*nullable=*/true,
                   /*has_default=*/true},
    });
    cat.add_table("order lines", {
        ColumnInfo{"order_id", DataType::BigInt, /*nullable=*/false},
        ColumnInfo{"amount", DataType::Double, /*nullable=*/true},
    });
    cat.set_schema_version(7);
    return cat;
}

std::string scratch_path(const char* name) {
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir != nullptr && *dir != '\0') ? dir : "/tmp";
    return base + "/db25_" + name;
}

// ---- Tests ----------------------------------------------------------------

void test_roundtrip_in_memory() {
    const InMemoryCatalog cat = sample_catalog();
    const std::string text = serialize_catalog(cat);
    std::string err;
    const std::optional<InMemoryCatalog> back = deserialize_catalog(text, err);
    CHECK(back.has_value());
    if (back) CHECK(catalogs_equal(cat, *back));
    // Serialization is deterministic: re-serializing the reload is byte-identical.
    if (back) CHECK(serialize_catalog(*back) == text);
}

void test_save_load_file() {
    const std::string path = scratch_path("snap_saveload.db25cat");
    std::remove(path.c_str());
    const InMemoryCatalog cat = sample_catalog();
    std::string err;
    CHECK(save_catalog_snapshot(cat, path, err));
    const std::optional<InMemoryCatalog> loaded = load_catalog_snapshot(path, err);
    CHECK(loaded.has_value());
    if (loaded) CHECK(catalogs_equal(cat, *loaded));
    std::remove(path.c_str());
}

void test_missing_file_is_empty_catalog() {
    const std::string path = scratch_path("snap_absent.db25cat");
    std::remove(path.c_str());
    std::string err;
    const std::optional<InMemoryCatalog> loaded = load_catalog_snapshot(path, err);
    CHECK(loaded.has_value());               // missing != error
    if (loaded) CHECK(loaded->tables().empty());
    if (loaded) CHECK(loaded->schema_version() == 0);
}

void test_crash_safety_ignores_temp() {
    // A crash between write(tmp) and rename leaves a stale .tmp and the previous
    // snapshot at `path`. load() must read `path` and ignore the .tmp entirely.
    const std::string path = scratch_path("snap_crash.db25cat");
    std::remove(path.c_str());
    std::string err;

    InMemoryCatalog v1 = sample_catalog();
    v1.set_schema_version(1);
    CHECK(save_catalog_snapshot(v1, path, err));

    // Simulate an interrupted next save: a half-written temp file with version 2.
    InMemoryCatalog v2 = sample_catalog();
    v2.set_schema_version(2);
    const std::string tmp = path + ".tmp";
    if (std::FILE* f = std::fopen(tmp.c_str(), "wb")) {
        const std::string data = serialize_catalog(v2);
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }

    const std::optional<InMemoryCatalog> loaded = load_catalog_snapshot(path, err);
    CHECK(loaded.has_value());
    if (loaded) CHECK(loaded->schema_version() == 1);  // the committed version, not the temp
    std::remove(path.c_str());
    std::remove(tmp.c_str());
}

void test_malformed_is_rejected() {
    std::string err;
    // Missing header.
    CHECK(!deserialize_catalog("version 1\nnext_table_id 1\n", err).has_value());
    // Unknown type token.
    err.clear();
    const char* bad_type =
        "DB25CATALOG 1\nversion 1\nnext_table_id 2\n"
        "table 1 t\ncol 1 NOTATYPE null nodefault x\n";
    CHECK(!deserialize_catalog(bad_type, err).has_value());
    CHECK(!err.empty());
    // Missing required next_table_id.
    err.clear();
    CHECK(!deserialize_catalog("DB25CATALOG 1\nversion 1\n", err).has_value());
}

void test_newline_name_rejected() {
    // A name that cannot be represented in the line format is refused at save,
    // rather than writing an unparseable snapshot.
    InMemoryCatalog cat;
    TableInfo t;
    t.table_id = 1;
    t.name = "bad\nname";
    cat.restore_table(std::move(t));
    cat.set_next_table_id(2);
    const std::string path = scratch_path("snap_badname.db25cat");
    std::remove(path.c_str());
    std::string err;
    CHECK(!save_catalog_snapshot(cat, path, err));
    CHECK(!err.empty());
    // Nothing was written to the real path.
    CHECK(std::fopen(path.c_str(), "rb") == nullptr);
}

}  // namespace

int main() {
    test_roundtrip_in_memory();
    test_save_load_file();
    test_missing_file_is_empty_catalog();
    test_crash_safety_ignores_temp();
    test_malformed_is_rejected();
    test_newline_name_rejected();

    std::printf("catalog_snapshot: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
