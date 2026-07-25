// DB25 Semantic Analyzer - persistent catalog snapshot store (implementation).
//
// See catalog_snapshot.hpp for the crash-safety contract. The on-disk format is
// line-oriented and self-describing; format 2 extends format 1 with table
// constraints, secondary indexes, and per-column DEFAULT expression text.
// Format 1 snapshots still load (their tables simply carry no constraints,
// indexes, or default text). Records with more than one free-text field use
// one-value-per-line sub-records so a trailing name/expression may contain
// spaces:
//
//   DB25CATALOG 2
//   version <schema_version>
//   next_table_id <n>
//   next_index_id <n>
//   table <table_id> <name...>
//   col <column_id> <TYPE> <null|notnull> <default|nodefault> <name...>
//   coldefault <expr...>                 (follows its col; only when default)
//   constraint <PK|UNIQUE|FK|CHECK>      (belongs to the current table)
//   cname <name...>                      (optional)
//   ccol <column_id>                     (repeatable: local columns)
//   creftable <table name...>            (FK)
//   crefcol <column_id>                  (repeatable: FK referenced columns)
//   cexpr <expr...>                      (CHECK)
//   index <index_id> <unique|plain>
//   iname <index name...>
//   itable <table name...>
//   icol <column_id>                     (repeatable)

#include "db25/semantic/catalog_snapshot.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>  // fsync, fileno

namespace db25::semantic {

namespace {

const char* type_to_token(DataType t) {
    switch (t) {
        case DataType::Unknown:   return "UNKNOWN";
        case DataType::Boolean:   return "BOOLEAN";
        case DataType::TinyInt:   return "TINYINT";
        case DataType::SmallInt:  return "SMALLINT";
        case DataType::Integer:   return "INTEGER";
        case DataType::BigInt:    return "BIGINT";
        case DataType::Decimal:   return "DECIMAL";
        case DataType::Real:      return "REAL";
        case DataType::Double:    return "DOUBLE";
        case DataType::Char:      return "CHAR";
        case DataType::VarChar:   return "VARCHAR";
        case DataType::Text:      return "TEXT";
        case DataType::Date:      return "DATE";
        case DataType::Time:      return "TIME";
        case DataType::Timestamp: return "TIMESTAMP";
        case DataType::Interval:  return "INTERVAL";
        case DataType::Blob:      return "BLOB";
        case DataType::Array:     return "ARRAY";
        case DataType::Struct:    return "STRUCT";
        case DataType::Map:       return "MAP";
        case DataType::Null:      return "NULL";
        case DataType::Any:       return "ANY";
    }
    return "UNKNOWN";
}

bool token_to_type(std::string_view s, DataType& out) {
    struct Entry { std::string_view tok; DataType type; };
    static constexpr Entry table[] = {
        {"UNKNOWN", DataType::Unknown},   {"BOOLEAN", DataType::Boolean},
        {"TINYINT", DataType::TinyInt},   {"SMALLINT", DataType::SmallInt},
        {"INTEGER", DataType::Integer},   {"BIGINT", DataType::BigInt},
        {"DECIMAL", DataType::Decimal},   {"REAL", DataType::Real},
        {"DOUBLE", DataType::Double},     {"CHAR", DataType::Char},
        {"VARCHAR", DataType::VarChar},   {"TEXT", DataType::Text},
        {"DATE", DataType::Date},         {"TIME", DataType::Time},
        {"TIMESTAMP", DataType::Timestamp}, {"INTERVAL", DataType::Interval},
        {"BLOB", DataType::Blob},         {"ARRAY", DataType::Array},
        {"STRUCT", DataType::Struct},     {"MAP", DataType::Map},
        {"NULL", DataType::Null},         {"ANY", DataType::Any},
    };
    for (const Entry& e : table) {
        if (e.tok == s) { out = e.type; return true; }
    }
    return false;
}

const char* constraint_kind_token(Constraint::Kind k) {
    switch (k) {
        case Constraint::Kind::PrimaryKey: return "PK";
        case Constraint::Kind::Unique:     return "UNIQUE";
        case Constraint::Kind::ForeignKey: return "FK";
        case Constraint::Kind::Check:      return "CHECK";
    }
    return "CHECK";
}

bool token_to_constraint_kind(std::string_view s, Constraint::Kind& out) {
    if (s == "PK") { out = Constraint::Kind::PrimaryKey; return true; }
    if (s == "UNIQUE") { out = Constraint::Kind::Unique; return true; }
    if (s == "FK") { out = Constraint::Kind::ForeignKey; return true; }
    if (s == "CHECK") { out = Constraint::Kind::Check; return true; }
    return false;
}

std::vector<std::string_view> split_ws(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

// Everything after the k-th space-delimited field (the trailing name/expr).
std::string_view rest_after(std::string_view line, int k) {
    std::size_t i = 0;
    int fields = 0;
    while (i < line.size() && fields < k) {
        while (i < line.size() && line[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i > start) ++fields;
    }
    while (i < line.size() && line[i] == ' ') ++i;
    return line.substr(i);
}

bool parse_u32(std::string_view s, std::uint32_t& out) {
    if (s.empty()) return false;
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
        if (v > 0xFFFFFFFFULL) return false;
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

bool representable(const std::string& s) {
    return s.find('\n') == std::string::npos && s.find('\r') == std::string::npos;
}

}  // namespace

std::string serialize_catalog(const InMemoryCatalog& catalog) {
    std::string out;
    out += "DB25CATALOG 2\n";
    out += "version " + std::to_string(catalog.schema_version()) + "\n";
    out += "next_table_id " + std::to_string(catalog.next_table_id()) + "\n";
    out += "next_index_id " + std::to_string(catalog.next_index_id()) + "\n";
    for (const TableInfo* t : catalog.tables()) {
        out += "table " + std::to_string(t->table_id) + " " + t->name + "\n";
        for (const ColumnInfo& c : t->columns) {
            out += "col " + std::to_string(c.column_id) + " ";
            out += type_to_token(c.type);
            out += c.nullable ? " null" : " notnull";
            out += c.has_default ? " default" : " nodefault";
            out += " " + c.name + "\n";
            if (c.has_default && !c.default_expr.empty()) {
                out += "coldefault " + c.default_expr + "\n";
            }
        }
        for (const Constraint& k : t->constraints) {
            out += "constraint ";
            out += constraint_kind_token(k.kind);
            out += "\n";
            if (!k.name.empty()) out += "cname " + k.name + "\n";
            for (const std::uint32_t id : k.columns) {
                out += "ccol " + std::to_string(id) + "\n";
            }
            if (k.kind == Constraint::Kind::ForeignKey) {
                out += "creftable " + k.ref_table + "\n";
                for (const std::uint32_t id : k.ref_columns) {
                    out += "crefcol " + std::to_string(id) + "\n";
                }
            }
            if (k.kind == Constraint::Kind::Check && !k.expr.empty()) {
                out += "cexpr " + k.expr + "\n";
            }
        }
    }
    for (const IndexInfo* idx : catalog.indexes()) {
        out += "index " + std::to_string(idx->index_id);
        out += idx->unique ? " unique\n" : " plain\n";
        out += "iname " + idx->name + "\n";
        out += "itable " + idx->table + "\n";
        for (const std::uint32_t id : idx->columns) {
            out += "icol " + std::to_string(id) + "\n";
        }
    }
    return out;
}

std::optional<InMemoryCatalog> deserialize_catalog(const std::string& text,
                                                   std::string& error) {
    InMemoryCatalog cat;
    bool header_seen = false;
    bool have_version = false;
    bool have_next_id = false;

    TableInfo current;             // table under construction
    bool have_table = false;
    Constraint constraint;         // constraint under construction
    bool have_constraint = false;
    IndexInfo index;               // index under construction
    bool have_index = false;

    auto flush_constraint = [&]() {
        if (have_constraint) {
            current.constraints.push_back(std::move(constraint));
            constraint = Constraint{};
            have_constraint = false;
        }
    };
    auto flush_table = [&]() {
        flush_constraint();
        if (have_table) {
            cat.restore_table(std::move(current));
            current = TableInfo{};
            have_table = false;
        }
    };
    auto flush_index = [&]() {
        if (have_index) {
            cat.restore_index(std::move(index));
            index = IndexInfo{};
            have_index = false;
        }
    };

    auto fail = [&](const std::string& msg, int line) -> std::optional<InMemoryCatalog> {
        error = msg + " (line " + std::to_string(line) + ")";
        return std::nullopt;
    };

    std::size_t pos = 0;
    int line_no = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string_view line(text.data() + pos, end - pos);
        const bool last = (nl == std::string::npos);
        pos = last ? text.size() + 1 : nl + 1;
        ++line_no;
        if (line.empty()) { if (last) break; else continue; }

        const std::vector<std::string_view> tok = split_ws(line);
        if (tok.empty()) { if (last) break; else continue; }
        const std::string_view kind = tok[0];

        if (!header_seen) {
            if (kind != "DB25CATALOG" || tok.size() != 2 ||
                (tok[1] != "1" && tok[1] != "2")) {
                return fail("bad snapshot header", line_no);
            }
            header_seen = true;
            if (last) break;
            continue;
        }

        std::uint32_t n = 0;
        if (kind == "version") {
            if (tok.size() != 2 || !parse_u32(tok[1], n)) return fail("bad version", line_no);
            cat.set_schema_version(n);
            have_version = true;
        } else if (kind == "next_table_id") {
            if (tok.size() != 2 || !parse_u32(tok[1], n)) return fail("bad next_table_id", line_no);
            cat.set_next_table_id(n);
            have_next_id = true;
        } else if (kind == "next_index_id") {
            if (tok.size() != 2 || !parse_u32(tok[1], n)) return fail("bad next_index_id", line_no);
            cat.set_next_index_id(n);
        } else if (kind == "table") {
            if (tok.size() < 3 || !parse_u32(tok[1], n)) return fail("bad table record", line_no);
            flush_table();
            current = TableInfo{};
            current.table_id = n;
            current.name = std::string(rest_after(line, 2));
            have_table = true;
        } else if (kind == "col") {
            DataType type = DataType::Unknown;
            if (!have_table || tok.size() < 6 || !parse_u32(tok[1], n) ||
                !token_to_type(tok[2], type) ||
                (tok[3] != "null" && tok[3] != "notnull") ||
                (tok[4] != "default" && tok[4] != "nodefault")) {
                return fail("bad col record", line_no);
            }
            flush_constraint();  // cols never follow constraints, but stay safe
            ColumnInfo c;
            c.column_id = n;
            c.type = type;
            c.nullable = (tok[3] == "null");
            c.has_default = (tok[4] == "default");
            c.name = std::string(rest_after(line, 5));
            current.columns.push_back(std::move(c));
        } else if (kind == "coldefault") {
            if (!have_table || current.columns.empty()) return fail("orphan coldefault", line_no);
            current.columns.back().default_expr = std::string(rest_after(line, 1));
        } else if (kind == "constraint") {
            Constraint::Kind ck = Constraint::Kind::Check;
            if (!have_table || tok.size() != 2 || !token_to_constraint_kind(tok[1], ck)) {
                return fail("bad constraint record", line_no);
            }
            flush_constraint();
            constraint = Constraint{};
            constraint.kind = ck;
            have_constraint = true;
        } else if (kind == "cname") {
            if (!have_constraint) return fail("orphan cname", line_no);
            constraint.name = std::string(rest_after(line, 1));
        } else if (kind == "ccol") {
            if (!have_constraint || tok.size() != 2 || !parse_u32(tok[1], n)) return fail("bad ccol", line_no);
            constraint.columns.push_back(n);
        } else if (kind == "creftable") {
            if (!have_constraint) return fail("orphan creftable", line_no);
            constraint.ref_table = std::string(rest_after(line, 1));
        } else if (kind == "crefcol") {
            if (!have_constraint || tok.size() != 2 || !parse_u32(tok[1], n)) return fail("bad crefcol", line_no);
            constraint.ref_columns.push_back(n);
        } else if (kind == "cexpr") {
            if (!have_constraint) return fail("orphan cexpr", line_no);
            constraint.expr = std::string(rest_after(line, 1));
        } else if (kind == "index") {
            if (tok.size() != 3 || !parse_u32(tok[1], n) ||
                (tok[2] != "unique" && tok[2] != "plain")) {
                return fail("bad index record", line_no);
            }
            flush_table();  // all tables precede indexes
            flush_index();
            index = IndexInfo{};
            index.index_id = n;
            index.unique = (tok[2] == "unique");
            have_index = true;
        } else if (kind == "iname") {
            if (!have_index) return fail("orphan iname", line_no);
            index.name = std::string(rest_after(line, 1));
        } else if (kind == "itable") {
            if (!have_index) return fail("orphan itable", line_no);
            index.table = std::string(rest_after(line, 1));
        } else if (kind == "icol") {
            if (!have_index || tok.size() != 2 || !parse_u32(tok[1], n)) return fail("bad icol", line_no);
            index.columns.push_back(n);
        } else {
            return fail("unknown record '" + std::string(kind) + "'", line_no);
        }

        if (last) break;
    }
    flush_table();
    flush_index();

    if (!header_seen || !have_version || !have_next_id) {
        error = "snapshot missing required header fields";
        return std::nullopt;
    }
    return cat;
}

bool save_catalog_snapshot(const InMemoryCatalog& catalog, const std::string& path,
                           std::string& error) {
    // Names/expressions are emitted at end-of-line; a newline in any of them
    // would corrupt the format. Reject rather than write an unparseable snapshot.
    auto bad = [&](const std::string& what, const std::string& v) {
        error = what + " contains a newline: " + v;
        return false;
    };
    for (const TableInfo* t : catalog.tables()) {
        if (!representable(t->name)) return bad("table name", t->name);
        for (const ColumnInfo& c : t->columns) {
            if (!representable(c.name)) return bad("column name", c.name);
            if (!representable(c.default_expr)) return bad("default expr", c.default_expr);
        }
        for (const Constraint& k : t->constraints) {
            if (!representable(k.name)) return bad("constraint name", k.name);
            if (!representable(k.ref_table)) return bad("constraint ref table", k.ref_table);
            if (!representable(k.expr)) return bad("check expr", k.expr);
        }
    }
    for (const IndexInfo* idx : catalog.indexes()) {
        if (!representable(idx->name)) return bad("index name", idx->name);
        if (!representable(idx->table)) return bad("index table", idx->table);
    }

    const std::string data = serialize_catalog(catalog);
    const std::string tmp = path + ".tmp";

    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        error = "cannot open temp file: " + tmp;
        return false;
    }
    const std::size_t written = std::fwrite(data.data(), 1, data.size(), f);
    if (written != data.size()) {
        std::fclose(f);
        std::remove(tmp.c_str());
        error = "short write to " + tmp;
        return false;
    }
    // Durability: flush userspace, then fsync the fd before the rename. (A fully
    // correct implementation also fsyncs the parent directory; deferred.)
    std::fflush(f);
    ::fsync(::fileno(f));
    std::fclose(f);

    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        error = "rename failed: " + tmp + " -> " + path;
        return false;
    }
    return true;
}

std::optional<InMemoryCatalog> load_catalog_snapshot(const std::string& path,
                                                     std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return InMemoryCatalog{};  // no snapshot yet = fresh, empty catalog
    }
    std::string data;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        data.append(buf, n);
    }
    std::fclose(f);
    return deserialize_catalog(data, error);
}

}  // namespace db25::semantic
