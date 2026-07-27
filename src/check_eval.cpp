// DB25 Semantic Analyzer - constant CHECK evaluation (implementation).

#include "db25/semantic/check_eval.hpp"

#include "db25/ast/node_types.hpp"
#include "db25/parser/parser.hpp"

#include <charconv>
#include <cstdlib>
#include <string>

namespace db25::semantic {

using ast::ASTNode;
using ast::NodeType;

namespace {

// A folded constant value, or Fail (cannot evaluate) / Null (SQL NULL).
struct V {
    enum K { Fail, Null, Int, Dbl, Str, Bool } k = Fail;
    long long i = 0;
    double d = 0;
    std::string s;
    bool b = false;

    static V fail() { return {}; }
    static V null_() { V v; v.k = Null; return v; }
    static V I(long long x) { V v; v.k = Int; v.i = x; return v; }
    static V D(double x) { V v; v.k = Dbl; v.d = x; return v; }
    static V S(std::string x) { V v; v.k = Str; v.s = std::move(x); return v; }
    static V B(bool x) { V v; v.k = Bool; v.b = x; return v; }

    [[nodiscard]] bool numeric() const { return k == Int || k == Dbl; }
    [[nodiscard]] double as_d() const { return k == Int ? static_cast<double>(i) : d; }
};

[[nodiscard]] std::string upper(std::string_view t) {
    std::string u;
    u.reserve(t.size());
    for (char c : t) u.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c);
    return u;
}

[[nodiscard]] std::string strip_quotes(std::string_view t) {
    if (t.size() >= 2 && (t.front() == '\'' || t.front() == '"') && t.back() == t.front()) {
        return std::string(t.substr(1, t.size() - 2));
    }
    return std::string(t);
}

[[nodiscard]] V parse_int(std::string_view t) {
    long long x = 0;
    const auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), x);
    if (ec != std::errc{} || ptr != t.data() + t.size()) return V::fail();
    return V::I(x);
}

class Evaluator {
public:
    explicit Evaluator(const CheckBindings& b) : binds_(b) {}

    V eval(const ASTNode* n) {
        if (n == nullptr || depth_ > 200) return V::fail();
        ++depth_;
        const V r = eval_inner(n);
        --depth_;
        return r;
    }

private:
    const CheckBindings& binds_;
    int depth_ = 0;

    V eval_inner(const ASTNode* n) {
        switch (n->node_type) {
            case NodeType::IntegerLiteral:
                return parse_int(n->primary_text);
            case NodeType::FloatLiteral:
                return V::D(std::strtod(std::string(n->primary_text).c_str(), nullptr));
            case NodeType::StringLiteral:
                return V::S(strip_quotes(n->primary_text));
            case NodeType::BooleanLiteral: {
                const std::string u = upper(n->primary_text);
                if (u == "TRUE") return V::B(true);
                if (u == "FALSE") return V::B(false);
                return V::fail();
            }
            case NodeType::NullLiteral:
                return V::null_();
            case NodeType::ColumnRef:
            case NodeType::Identifier: {
                const auto it = binds_.find(n->primary_text);  // transparent, case-insensitive
                if (it == binds_.end()) return V::fail();
                return eval(it->second);
            }
            case NodeType::UnaryExpr:
                return eval_unary(n);
            case NodeType::BinaryExpr:
                return eval_binary(n);
            case NodeType::IsNullExpr:
                return eval_is_null(n);
            case NodeType::BetweenExpr:
                return eval_between(n);
            default:
                return V::fail();
        }
    }

    static const ASTNode* child(const ASTNode* n, int idx) {
        const ASTNode* c = n->first_child;
        for (int i = 0; i < idx && c != nullptr; ++i) c = c->next_sibling;
        return c;
    }

    V eval_unary(const ASTNode* n) {
        const std::string op = upper(n->primary_text);
        const V x = eval(child(n, 0));
        if (op == "NOT") {
            if (x.k == V::Bool) return V::B(!x.b);
            if (x.k == V::Null) return V::null_();
            return V::fail();
        }
        if (op == "-") {
            if (x.k == V::Int) return V::I(-x.i);
            if (x.k == V::Dbl) return V::D(-x.d);
            if (x.k == V::Null) return V::null_();
        }
        return V::fail();
    }

    V eval_is_null(const ASTNode* n) {
        const V x = eval(child(n, 0));
        if (x.k == V::Fail) return V::fail();
        const bool is_null = (x.k == V::Null);
        return upper(n->primary_text).find("NOT") != std::string::npos ? V::B(!is_null)
                                                                        : V::B(is_null);
    }

    V eval_between(const ASTNode* n) {
        const V val = eval(child(n, 0));
        const V lo = eval(child(n, 1));
        const V hi = eval(child(n, 2));
        if (val.k == V::Fail || lo.k == V::Fail || hi.k == V::Fail) return V::fail();
        if (val.k == V::Null || lo.k == V::Null || hi.k == V::Null) return V::null_();
        const V ge = compare(">=", val, lo);
        const V le = compare("<=", val, hi);
        if (ge.k != V::Bool || le.k != V::Bool) return V::fail();
        const bool in = ge.b && le.b;
        return upper(n->primary_text).find("NOT") != std::string::npos ? V::B(!in) : V::B(in);
    }

    V eval_binary(const ASTNode* n) {
        const std::string op = upper(n->primary_text);
        const ASTNode* l = child(n, 0);
        const ASTNode* r = child(n, 1);

        if (op == "AND" || op == "OR") {
            const V lv = eval(l);
            const V rv = eval(r);
            const bool lf = (lv.k == V::Bool && !lv.b);
            const bool rf = (rv.k == V::Bool && !rv.b);
            const bool lt = (lv.k == V::Bool && lv.b);
            const bool rt = (rv.k == V::Bool && rv.b);
            if (op == "AND") {
                if (lf || rf) return V::B(false);          // FALSE AND x = FALSE
                if (lv.k == V::Fail || rv.k == V::Fail) return V::fail();
                if (lv.k == V::Null || rv.k == V::Null) return V::null_();
                return V::B(lt && rt);
            }
            if (lt || rt) return V::B(true);               // TRUE OR x = TRUE
            if (lv.k == V::Fail || rv.k == V::Fail) return V::fail();
            if (lv.k == V::Null || rv.k == V::Null) return V::null_();
            return V::B(false);                            // both FALSE
        }

        const V lv = eval(l);
        const V rv = eval(r);
        if (lv.k == V::Fail || rv.k == V::Fail) return V::fail();

        if (op == "=" || op == "==" || op == "<>" || op == "!=" || op == "<" || op == ">" ||
            op == "<=" || op == ">=") {
            if (lv.k == V::Null || rv.k == V::Null) return V::null_();
            return compare(op, lv, rv);
        }
        // Arithmetic: numeric only; NULL propagates.
        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            if (lv.k == V::Null || rv.k == V::Null) return V::null_();
            if (!lv.numeric() || !rv.numeric()) return V::fail();
            const bool ints = (lv.k == V::Int && rv.k == V::Int);
            if (op == "/") {
                if (rv.as_d() == 0.0) return V::fail();     // undefined -> bail
                return ints ? V::I(lv.i / rv.i) : V::D(lv.as_d() / rv.as_d());
            }
            if (op == "%") {
                if (!ints || rv.i == 0) return V::fail();
                return V::I(lv.i % rv.i);
            }
            // Two integer operands: compute + - * EXACTLY in int64. Going through
            // double (as this used to) silently loses precision past 2^53 - so a
            // CHECK like `a + b = 9007199254740993` would compare a rounded sum
            // and flip the result - and casting an out-of-range double back to
            // long long is undefined behavior. Overflow can't be folded to a
            // definite value, so bail (the CHECK stays Unknown - never a false
            // verdict). A real operand keeps the double path.
            if (ints) {
                long long r = 0;
                const bool overflow = op == "+"   ? __builtin_add_overflow(lv.i, rv.i, &r)
                                      : op == "-" ? __builtin_sub_overflow(lv.i, rv.i, &r)
                                                  : __builtin_mul_overflow(lv.i, rv.i, &r);
                if (overflow) return V::fail();
                return V::I(r);
            }
            const double a = lv.as_d(), b = rv.as_d();
            const double res = op == "+" ? a + b : op == "-" ? a - b : a * b;
            return V::D(res);
        }
        return V::fail();
    }

    static V compare(const std::string& op, const V& l, const V& r) {
        int cmp = 0;  // -1, 0, 1
        if (l.k == V::Int && r.k == V::Int) {
            cmp = l.i < r.i ? -1 : (l.i > r.i ? 1 : 0);  // exact for large int64
        } else if (l.numeric() && r.numeric()) {
            const double a = l.as_d(), b = r.as_d();
            cmp = a < b ? -1 : (a > b ? 1 : 0);
        } else if (l.k == V::Str && r.k == V::Str) {
            cmp = l.s < r.s ? -1 : (l.s > r.s ? 1 : 0);
        } else if (l.k == V::Bool && r.k == V::Bool) {
            cmp = (l.b == r.b) ? 0 : (!l.b ? -1 : 1);
        } else {
            return V::fail();  // cross-type comparison: don't guess
        }
        if (op == "=" || op == "==") return V::B(cmp == 0);
        if (op == "<>" || op == "!=") return V::B(cmp != 0);
        if (op == "<") return V::B(cmp < 0);
        if (op == ">") return V::B(cmp > 0);
        if (op == "<=") return V::B(cmp <= 0);
        if (op == ">=") return V::B(cmp >= 0);
        return V::fail();
    }
};

}  // namespace

CheckResult evaluate_check(std::string_view expr_text, const CheckBindings& bindings) {
    parser::Parser p;
    const std::string sql = "SELECT " + std::string(expr_text);
    auto r = p.parse(sql);
    if (!r.has_value()) return CheckResult::Unknown;

    // SELECT <expr> -> SelectStmt / SelectList / <expr as first item>.
    ASTNode* list = nullptr;
    for (ASTNode* c = r.value()->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type == NodeType::SelectList) { list = c; break; }
    }
    if (list == nullptr || list->first_child == nullptr) return CheckResult::Unknown;

    Evaluator ev(bindings);
    const V v = ev.eval(list->first_child);
    if (v.k == V::Bool) return v.b ? CheckResult::True : CheckResult::False;
    return CheckResult::Unknown;  // Null / Fail -> not a certain violation
}

}  // namespace db25::semantic
