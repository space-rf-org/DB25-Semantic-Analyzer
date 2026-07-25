// DB25 Semantic Analyzer - SQL type name -> DataType mapping
//
// A single, self-contained mapping from a SQL type name (as written in a CAST
// target or a DDL column definition) to a DataType. Length/precision (the 10 in
// VARCHAR(10)) does not change the category and is ignored. Unrecognized names
// degrade to Unknown rather than guessing.
//
// NOTE: analyzer.cpp still carries a private copy used by query type inference;
// the two are intended to be unified onto this header (kept in sync until then).

#pragma once

#include "db25/ast/node_types.hpp"

#include <string>
#include <string_view>

namespace db25::semantic {

using ast::DataType;

[[nodiscard]] inline DataType data_type_from_name(std::string_view name) {
    std::string u;
    u.reserve(name.size());
    for (const char c : name) {
        u.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c);
    }
    // Strip a trailing "(...)" if a length/precision rode along in the name.
    std::string_view base = u;
    if (const auto p = base.find('('); p != std::string_view::npos) {
        base = base.substr(0, p);
    }
    while (!base.empty() && base.back() == ' ') base.remove_suffix(1);

    if (base == "INT" || base == "INTEGER" || base == "INT4") return DataType::Integer;
    if (base == "BIGINT" || base == "INT8") return DataType::BigInt;
    if (base == "SMALLINT" || base == "INT2") return DataType::SmallInt;
    if (base == "TINYINT") return DataType::TinyInt;
    if (base == "DECIMAL" || base == "NUMERIC" || base == "NUMBER") return DataType::Decimal;
    if (base == "REAL" || base == "FLOAT4") return DataType::Real;
    if (base == "DOUBLE" || base == "DOUBLE PRECISION" || base == "FLOAT" ||
        base == "FLOAT8")
        return DataType::Double;
    if (base == "BOOLEAN" || base == "BOOL") return DataType::Boolean;
    if (base == "CHAR" || base == "CHARACTER" || base == "BPCHAR") return DataType::Char;
    if (base == "VARCHAR" || base == "CHARACTER VARYING") return DataType::VarChar;
    if (base == "TEXT" || base == "STRING" || base == "CLOB") return DataType::Text;
    if (base == "DATE") return DataType::Date;
    if (base == "TIME") return DataType::Time;
    if (base == "TIMESTAMP" || base == "DATETIME") return DataType::Timestamp;
    if (base == "INTERVAL") return DataType::Interval;
    if (base == "BLOB" || base == "BYTEA" || base == "BINARY") return DataType::Blob;
    return DataType::Unknown;
}

// A short, human-readable name for a DataType, for diagnostics. Not a SQL type
// token (see catalog_snapshot's type_to_token for the persisted spelling).
[[nodiscard]] inline std::string_view data_type_name(DataType t) {
    switch (t) {
        case DataType::TinyInt:   return "TINYINT";
        case DataType::SmallInt:  return "SMALLINT";
        case DataType::Integer:   return "INTEGER";
        case DataType::BigInt:    return "BIGINT";
        case DataType::Decimal:   return "DECIMAL";
        case DataType::Real:      return "REAL";
        case DataType::Double:    return "DOUBLE";
        case DataType::Boolean:   return "BOOLEAN";
        case DataType::Char:      return "CHAR";
        case DataType::VarChar:   return "VARCHAR";
        case DataType::Text:      return "TEXT";
        case DataType::Date:      return "DATE";
        case DataType::Time:      return "TIME";
        case DataType::Timestamp: return "TIMESTAMP";
        case DataType::Interval:  return "INTERVAL";
        case DataType::Blob:      return "BLOB";
        case DataType::Null:      return "NULL";
        case DataType::Any:       return "ANY";
        default:                  return "UNKNOWN";
    }
}

}  // namespace db25::semantic
