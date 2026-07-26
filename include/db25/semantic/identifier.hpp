// DB25 Semantic Analyzer - Identifier case handling for name resolution
//
// DB25 resolves SQL identifiers (table, column, alias, CTE and index names)
// CASE-INSENSITIVELY over ASCII. This is a deliberate, engine-wide rule, and it
// is the only self-consistent one available at this layer: the tokenizer strips
// the surrounding quotes from a delimited identifier (`"Foo"`) and emits it as a
// plain identifier token, byte-for-byte indistinguishable from a bare `Foo`.
// Because the quoted/unquoted distinction never reaches the analyzer, DB25 cannot
// honor PostgreSQL's split rule (bare identifiers fold to lower case, quoted ones
// stay case-sensitive); instead every identifier compares case-insensitively,
// which matches the common case of PostgreSQL and SQLite alike. Folding is ASCII
// only - identifiers outside [A-Za-z0-9_] are rare and locale-dependent case
// rules are deliberately out of scope.
//
// One helper, applied at every resolution chokepoint: `iequals` for linear
// scans, and the `IdentifierHash`/`IdentifierEqual` pair for the hash-based
// lookups (the catalog's table/index maps, a wide relation's name index, the
// CHECK-evaluator's bindings). Both are transparent, so a std::string-keyed
// container can be probed with a std::string_view without materializing a key.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace db25::semantic {

// ASCII lower-case one byte. Bytes outside 'A'..'Z' pass through unchanged, so
// digits, '_' and non-ASCII bytes are preserved verbatim.
[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// Case-insensitive (ASCII) equality of two identifiers.
[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

// Hash consistent with iequals (identifiers equal under iequals hash equal): an
// FNV-1a over the lower-cased bytes, since std::hash has no case-folding variant.
struct IdentifierHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept {
        std::uint64_t h = 14695981039346656037ULL;  // FNV-1a 64-bit offset basis
        for (const char c : s) {
            h ^= static_cast<unsigned char>(ascii_lower(c));
            h *= 1099511628211ULL;  // FNV-1a 64-bit prime
        }
        return static_cast<std::size_t>(h);
    }
};

// Equality functor pairing with IdentifierHash for a case-insensitive container.
struct IdentifierEqual {
    using is_transparent = void;
    [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept {
        return iequals(a, b);
    }
};

}  // namespace db25::semantic
