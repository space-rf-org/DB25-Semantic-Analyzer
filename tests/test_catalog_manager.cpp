// DB25 Semantic Analyzer - catalog manager (DDL) tests.
//
// These are state-transition assertions: apply a DDL op, then assert (a) the
// resulting in-memory catalog, (b) the persisted snapshot, and (c) the schema
// version transition. This is the verification shape for stateful work, where
// the read-query differential oracle does not apply.

#include "db25/semantic/catalog.hpp"
#include "db25/semantic/catalog_manager.hpp"
#include "db25/semantic/catalog_snapshot.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

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

std::string scratch_path(const char* name) {
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir != nullptr && *dir != '\0') ? dir : "/tmp";
    return base + "/db25_" + name;
}

std::vector<ColumnInfo> user_cols() {
    return {
        ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"name", DataType::VarChar, /*nullable=*/true},
    };
}

// Read the on-disk snapshot back and confirm a table's presence matches memory.
bool disk_has_table(const std::string& path, const std::string& table) {
    std::string err;
    const std::optional<InMemoryCatalog> ondisk = load_catalog_snapshot(path, err);
    return ondisk && ondisk->find_table(table) != nullptr;
}

std::uint32_t disk_version(const std::string& path) {
    std::string err;
    const std::optional<InMemoryCatalog> ondisk = load_catalog_snapshot(path, err);
    return ondisk ? ondisk->schema_version() : 0xFFFFFFFFu;
}

void test_create_persists_and_bumps_version() {
    const std::string path = scratch_path("mgr_create.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    CHECK(load_err.empty());
    CHECK(mgr.schema_version() == 0);

    const DdlResult r = mgr.create_table("users", user_cols());
    CHECK(r.ok);
    CHECK(r.schema_version == 1);
    CHECK(mgr.schema_version() == 1);
    CHECK(mgr.catalog().find_table("users") != nullptr);
    // Durable BEFORE it is observable: the snapshot on disk already reflects it.
    CHECK(disk_has_table(path, "users"));
    CHECK(disk_version(path) == 1);
    std::remove(path.c_str());
}

void test_duplicate_create_is_rejected_atomically() {
    const std::string path = scratch_path("mgr_dup.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    CHECK(mgr.create_table("users", user_cols()).ok);

    const DdlResult dup = mgr.create_table("users", user_cols());
    CHECK(!dup.ok);
    CHECK(!dup.error.empty());
    // Neither memory nor disk moved on the failed op.
    CHECK(mgr.schema_version() == 1);
    CHECK(disk_version(path) == 1);
    std::remove(path.c_str());
}

void test_drop_semantics() {
    const std::string path = scratch_path("mgr_drop.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    CHECK(mgr.create_table("users", user_cols()).ok);   // v1
    CHECK(mgr.create_table("orders", user_cols()).ok);  // v2

    const DdlResult d = mgr.drop_table("users");
    CHECK(d.ok);
    CHECK(d.schema_version == 3);
    CHECK(mgr.catalog().find_table("users") == nullptr);
    CHECK(mgr.catalog().find_table("orders") != nullptr);
    CHECK(!disk_has_table(path, "users"));

    // DROP of a missing table: hard error by default, no-op success with if_exists.
    CHECK(!mgr.drop_table("users").ok);
    CHECK(mgr.schema_version() == 3);
    const DdlResult ie = mgr.drop_table("users", /*if_exists=*/true);
    CHECK(ie.ok);
    CHECK(ie.schema_version == 3);  // no bump for a no-op
    std::remove(path.c_str());
}

void test_durability_across_restart() {
    const std::string path = scratch_path("mgr_restart.db25cat");
    std::remove(path.c_str());
    std::uint32_t version_before = 0;
    {
        std::string load_err;
        CatalogManager mgr(path, load_err);
        CHECK(mgr.create_table("users", user_cols()).ok);
        CHECK(mgr.create_table("orders", user_cols()).ok);
        version_before = mgr.schema_version();
    }
    // "Restart": a fresh manager on the same path recovers the exact state.
    {
        std::string load_err;
        CatalogManager mgr2(path, load_err);
        CHECK(load_err.empty());
        CHECK(mgr2.schema_version() == version_before);
        CHECK(mgr2.catalog().find_table("users") != nullptr);
        CHECK(mgr2.catalog().find_table("orders") != nullptr);
        // A new CREATE after restart does not collide with recovered ids.
        const DdlResult r = mgr2.create_table("sessions", user_cols());
        CHECK(r.ok);
        const TableInfo* s = mgr2.catalog().find_table("sessions");
        const TableInfo* u = mgr2.catalog().find_table("users");
        CHECK(s != nullptr && u != nullptr && s->table_id != u->table_id);
    }
    std::remove(path.c_str());
}

void test_watcher_fires_on_commit_only() {
    const std::string path = scratch_path("mgr_watch.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);

    std::vector<std::uint32_t> seen;
    mgr.add_watcher([&](std::uint32_t v) { seen.push_back(v); });

    CHECK(mgr.create_table("users", user_cols()).ok);   // commit -> notify(1)
    CHECK(!mgr.create_table("users", user_cols()).ok);  // failed -> no notify
    CHECK(mgr.create_table("orders", user_cols()).ok);  // commit -> notify(2)

    CHECK(seen.size() == 2);
    if (seen.size() == 2) {
        CHECK(seen[0] == 1 && seen[1] == 2);
    }
    std::remove(path.c_str());
}

}  // namespace

int main() {
    test_create_persists_and_bumps_version();
    test_duplicate_create_is_rejected_atomically();
    test_drop_semantics();
    test_durability_across_restart();
    test_watcher_fires_on_commit_only();

    std::printf("catalog_manager: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
