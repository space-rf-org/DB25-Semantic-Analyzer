// DB25 Semantic Analyzer - persistent catalog snapshot store (implementation).
//
// See catalog_snapshot.hpp for the on-disk format and the crash-safety contract.

#include "db25/semantic/catalog_snapshot.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>  // fsync, fileno

namespace db25::semantic {

namespace {

// ---- DataType <-> token. Explicit and self-contained so the on-disk spelling
//   is stable regardless of any display-name changes elsewhere. --------------

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

// ---- Minimal line tokenizer. Splits on single spaces; `rest_after(line, k)`
//   returns everything past the k-th space (so a trailing name may contain
//   spaces). ------------------------------------------------------------------

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

// Everything after the k-th space-delimited field of `line` (the trailing name).
// Returns empty if the line has fewer than k fields.
std::string_view rest_after(std::string_view line, int k) {
    std::size_t i = 0;
    int fields = 0;
    while (i < line.size() && fields < k) {
        while (i < line.size() && line[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i > start) ++fields;
    }
    while (i < line.size() && line[i] == ' ') ++i;  // skip the single delimiter run
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

bool name_is_representable(const std::string& name) {
    return name.find('\n') == std::string::npos &&
           name.find('\r') == std::string::npos;
}

}  // namespace

std::string serialize_catalog(const InMemoryCatalog& catalog) {
    std::string out;
    out += "DB25CATALOG 1\n";
    out += "version " + std::to_string(catalog.schema_version()) + "\n";
    out += "next_table_id " + std::to_string(catalog.next_table_id()) + "\n";
    for (const TableInfo* t : catalog.tables()) {
        out += "table " + std::to_string(t->table_id) + " " + t->name + "\n";
        for (const ColumnInfo& c : t->columns) {
            out += "col " + std::to_string(c.column_id) + " ";
            out += type_to_token(c.type);
            out += c.nullable ? " null" : " notnull";
            out += c.has_default ? " default" : " nodefault";
            out += " " + c.name + "\n";
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
    TableInfo current;
    bool have_current = false;

    auto flush_table = [&]() {
        if (have_current) {
            cat.restore_table(std::move(current));
            current = TableInfo{};
            have_current = false;
        }
    };

    std::size_t pos = 0;
    int line_no = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string_view line(text.data() + pos, end - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        ++line_no;
        if (line.empty()) {
            if (nl == std::string::npos) break;
            continue;  // tolerate blank lines
        }

        const std::vector<std::string_view> tok = split_ws(line);
        if (tok.empty()) continue;
        const std::string_view kind = tok[0];

        if (!header_seen) {
            if (kind != "DB25CATALOG" || tok.size() != 2 || tok[1] != "1") {
                error = "bad snapshot header (line " + std::to_string(line_no) + ")";
                return std::nullopt;
            }
            header_seen = true;
            continue;
        }

        if (kind == "version") {
            std::uint32_t v = 0;
            if (tok.size() != 2 || !parse_u32(tok[1], v)) {
                error = "bad version (line " + std::to_string(line_no) + ")";
                return std::nullopt;
            }
            cat.set_schema_version(v);
            have_version = true;
        } else if (kind == "next_table_id") {
            std::uint32_t n = 0;
            if (tok.size() != 2 || !parse_u32(tok[1], n)) {
                error = "bad next_table_id (line " + std::to_string(line_no) + ")";
                return std::nullopt;
            }
            cat.set_next_table_id(n);
            have_next_id = true;
        } else if (kind == "table") {
            std::uint32_t id = 0;
            if (tok.size() < 3 || !parse_u32(tok[1], id)) {
                error = "bad table record (line " + std::to_string(line_no) + ")";
                return std::nullopt;
            }
            flush_table();
            current = TableInfo{};
            current.table_id = id;
            current.name = std::string(rest_after(line, 2));
            have_current = true;
        } else if (kind == "col") {
            std::uint32_t id = 0;
            DataType type = DataType::Unknown;
            if (!have_current || tok.size() < 6 || !parse_u32(tok[1], id) ||
                !token_to_type(tok[2], type) ||
                (tok[3] != "null" && tok[3] != "notnull") ||
                (tok[4] != "default" && tok[4] != "nodefault")) {
                error = "bad col record (line " + std::to_string(line_no) + ")";
                return std::nullopt;
            }
            ColumnInfo c;
            c.column_id = id;
            c.type = type;
            c.nullable = (tok[3] == "null");
            c.has_default = (tok[4] == "default");
            c.name = std::string(rest_after(line, 5));
            current.columns.push_back(std::move(c));
        } else {
            error = "unknown record '" + std::string(kind) + "' (line " +
                    std::to_string(line_no) + ")";
            return std::nullopt;
        }

        if (nl == std::string::npos) break;
    }
    flush_table();

    if (!header_seen || !have_version || !have_next_id) {
        error = "snapshot missing required header fields";
        return std::nullopt;
    }
    return cat;
}

bool save_catalog_snapshot(const InMemoryCatalog& catalog, const std::string& path,
                           std::string& error) {
    // Names are emitted at end-of-line; a newline in a name would corrupt the
    // format. Reject it rather than write an unparseable snapshot.
    for (const TableInfo* t : catalog.tables()) {
        if (!name_is_representable(t->name)) {
            error = "table name contains a newline: " + t->name;
            return false;
        }
        for (const ColumnInfo& c : t->columns) {
            if (!name_is_representable(c.name)) {
                error = "column name contains a newline: " + c.name;
                return false;
            }
        }
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
    // Durability: flush userspace, then fsync the fd before the rename so the
    // bytes are on disk. (A fully-correct implementation also fsyncs the parent
    // directory so the rename itself is durable; deferred for the interim.)
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
