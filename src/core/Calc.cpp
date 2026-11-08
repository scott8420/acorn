#include "core/Calc.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>

namespace acorn::core {

namespace {

std::string curly(const std::string& s) { return "“" + s + "”"; }

std::string lower_ascii(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

enum class Op { Add, Sub, Mul, Div, Join, Eq, Ne, Lt, Gt, Le, Ge, And, Or };

struct Fn {
    const char* name;
    int min_args, max_args;
};
constexpr Fn kFns[] = {
    {"length", 1, 1}, {"upper", 1, 1}, {"lower", 1, 1}, {"left", 2, 2}, {"right", 2, 2},
    {"trim", 1, 1},   {"if", 2, 3},    {"round", 1, 2}, {"today", 0, 0},
};
const Fn* find_fn(const std::string& name) {
    const auto l = lower_ascii(name);
    for (const auto& f : kFns) if (l == f.name) return &f;
    return nullptr;
}
bool is_word(const std::string& name) {
    const auto l = lower_ascii(name);
    return l == "and" || l == "or" || l == "not" || find_fn(name) != nullptr;
}

}  // namespace

struct CalcNode {
    enum class Kind { Number, Text, Name, Neg, Not, Binary, Call } kind = Kind::Number;
    double      number = 0;
    std::string text;          // Text: the words; Name: the field; Call: the function (lower case)
    Op          op = Op::Add;
    std::vector<std::shared_ptr<const CalcNode>> kids;
};

namespace {

using NodeP = std::shared_ptr<const CalcNode>;

// ── the lexer ────────────────────────────────────────────────────────────────
struct Tok {
    enum class K { End, Number, Text, Name, Op, LParen, RParen, Comma } k = K::End;
    std::string s;     // the text / name / operator
    double      n = 0;
    std::size_t at = 0;
};

bool name_start(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool name_char(unsigned char c) { return name_start(c) || std::isdigit(c); }

std::variant<std::vector<Tok>, std::string> lex(const std::string& src) {
    std::vector<Tok> out;
    std::size_t i = 0;
    while (i < src.size()) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        if (std::isspace(c)) { ++i; continue; }
        Tok t;
        t.at = i;
        if (std::isdigit(c) || (c == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            std::size_t j = i;
            while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) ++j;
            if (j < src.size() && src[j] == '.') {
                ++j;
                while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) ++j;
            }
            if (j < src.size() && name_start(static_cast<unsigned char>(src[j])))
                return "a name can't start with a digit: " + curly(src.substr(i, j + 1 - i));
            t.k = Tok::K::Number;
            t.s = src.substr(i, j - i);
            t.n = std::strtod(t.s.c_str(), nullptr);
            i = j;
        } else if (c == '"') {
            std::string s;
            std::size_t j = i + 1;
            bool closed = false;
            while (j < src.size()) {
                if (src[j] == '\\' && j + 1 < src.size()) { s += src[j + 1]; j += 2; continue; }
                if (src[j] == '"') { closed = true; ++j; break; }
                s += src[j++];
            }
            if (!closed) return "the text starting " + curly(src.substr(i, 12)) + " has no closing \"";
            t.k = Tok::K::Text;
            t.s = std::move(s);
            i = j;
        } else if (src.compare(i, 3, "\u2260") == 0 || src.compare(i, 3, "\u2264") == 0 || src.compare(i, 3, "\u2265") == 0) {
            // ≠ ≤ ≥ before names: their bytes would read as a name's
            const std::string os = src.substr(i, 3);
            t.k = Tok::K::Op;
            t.s = os == "\u2260" ? "<>" : os == "\u2264" ? "<=" : ">=";
            i += 3;
        } else if (name_start(c) ||
                   (c == '$' && i + 1 < src.size() && name_start(static_cast<unsigned char>(src[i + 1])))) {
            // s023a: a script's variable is a name led by $ ($count).
            std::size_t j = i + 1;
            while (j < src.size() && name_char(static_cast<unsigned char>(src[j]))) ++j;
            t.k = Tok::K::Name;
            t.s = src.substr(i, j - i);
            i = j;
        } else if (c == '(') { t.k = Tok::K::LParen; ++i; }
        else if (c == ')') { t.k = Tok::K::RParen; ++i; }
        else if (c == ',') { t.k = Tok::K::Comma; ++i; }
        else {
            static const char* ops[] = {"<=", ">=", "<>", "≠", "≤", "≥", "+", "-", "*", "/", "&", "=", "<", ">"};
            bool found = false;
            for (const char* o : ops) {
                const std::string os = o;
                if (src.compare(i, os.size(), os) == 0) {
                    t.k = Tok::K::Op;
                    t.s = os == "≠" ? "<>" : os == "≤" ? "<=" : os == "≥" ? ">=" : os;
                    i += os.size();
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::size_t len = 1;
                if (c >= 0xC0) len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
                return "Acorn doesn't know " + curly(src.substr(i, len)) + " here";
            }
        }
        out.push_back(std::move(t));
    }
    Tok end;
    end.at = src.size();
    out.push_back(end);
    return out;
}

// ── the parser: precedence climbing, lowest first ────────────────────────────
//   or < and < not < compare < & < + - < * / < unary - < primary
class Parser {
public:
    Parser(const std::string& src, std::vector<Tok> toks) : m_src(src), m_t(std::move(toks)) {}

    std::variant<NodeP, std::string> parse() {
        auto n = expr();
        if (!m_err.empty()) return m_err;
        if (peek().k != Tok::K::End) return "Acorn didn't expect " + curly(shown(peek())) + after();
        return n;
    }
    std::vector<std::string> names;

private:
    const Tok& peek() const { return m_t[m_i]; }
    Tok take() { return m_t[m_i++]; }
    bool is_op(const char* o) const { return peek().k == Tok::K::Op && peek().s == o; }
    bool is_word(const char* w) const { return peek().k == Tok::K::Name && lower_ascii(peek().s) == w; }
    static std::string shown(const Tok& t) {
        switch (t.k) {
            case Tok::K::LParen: return "(";
            case Tok::K::RParen: return ")";
            case Tok::K::Comma:  return ",";
            case Tok::K::Text:   return "\"" + t.s + "\"";
            default:             return t.s;
        }
    }
    std::string after() const {
        const auto at = peek().at;
        if (at == 0) return " at the start";
        auto from = at > 16 ? at - 16 : 0;
        return " after " + curly(m_src.substr(from, at - from));
    }
    NodeP fail(std::string why) {
        if (m_err.empty()) m_err = std::move(why);
        return nullptr;
    }
    static NodeP make(CalcNode n) { return std::make_shared<const CalcNode>(std::move(n)); }
    static NodeP bin(Op op, NodeP a, NodeP b) {
        CalcNode n;
        n.kind = CalcNode::Kind::Binary;
        n.op = op;
        n.kids = {std::move(a), std::move(b)};
        return make(std::move(n));
    }

    NodeP expr() { return or_(); }
    NodeP or_() {
        auto a = and_();
        while (m_err.empty() && is_word("or")) { take(); a = bin(Op::Or, a, and_()); }
        return a;
    }
    NodeP and_() {
        auto a = not_();
        while (m_err.empty() && is_word("and")) { take(); a = bin(Op::And, a, not_()); }
        return a;
    }
    NodeP not_() {
        if (is_word("not")) {
            take();
            CalcNode n;
            n.kind = CalcNode::Kind::Not;
            n.kids = {not_()};
            return make(std::move(n));
        }
        return compare();
    }
    NodeP compare() {
        auto a = join();
        static const std::pair<const char*, Op> cmp[] = {{"=", Op::Eq}, {"<>", Op::Ne}, {"<", Op::Lt},
                                                         {">", Op::Gt}, {"<=", Op::Le}, {">=", Op::Ge}};
        while (m_err.empty()) {
            const Op* op = nullptr;
            for (const auto& [s, o] : cmp) if (is_op(s)) op = &o;
            if (!op) break;
            const Op o = *op;
            take();
            a = bin(o, a, join());
        }
        return a;
    }
    NodeP join() {
        auto a = add();
        while (m_err.empty() && is_op("&")) { take(); a = bin(Op::Join, a, add()); }
        return a;
    }
    NodeP add() {
        auto a = mul();
        while (m_err.empty() && (is_op("+") || is_op("-"))) {
            const Op o = take().s == "+" ? Op::Add : Op::Sub;
            a = bin(o, a, mul());
        }
        return a;
    }
    NodeP mul() {
        auto a = unary();
        while (m_err.empty() && (is_op("*") || is_op("/"))) {
            const Op o = take().s == "*" ? Op::Mul : Op::Div;
            a = bin(o, a, unary());
        }
        return a;
    }
    NodeP unary() {
        if (is_op("-")) {
            take();
            CalcNode n;
            n.kind = CalcNode::Kind::Neg;
            n.kids = {unary()};
            return make(std::move(n));
        }
        if (is_op("+")) { take(); return unary(); }
        return primary();
    }
    NodeP primary() {
        const Tok& t = peek();
        switch (t.k) {
            case Tok::K::Number: {
                CalcNode n;
                n.kind = CalcNode::Kind::Number;
                n.number = take().n;
                return make(std::move(n));
            }
            case Tok::K::Text: {
                CalcNode n;
                n.kind = CalcNode::Kind::Text;
                n.text = take().s;
                return make(std::move(n));
            }
            case Tok::K::LParen: {
                const auto open_at = t.at;
                take();
                auto e = expr();
                if (!m_err.empty()) return nullptr;
                if (peek().k != Tok::K::RParen)
                    return fail("a closing ) is missing after " + curly(m_src.substr(open_at, peek().at - open_at)));
                take();
                return e;
            }
            case Tok::K::Name: {
                const Tok name = take();
                if (peek().k == Tok::K::LParen) return call(name);
                if (find_fn(name.s)) return fail(curly(name.s) + " is a function -- it needs ( )");
                if (core::is_word(name.s)) return fail(curly(name.s) + " needs something on each side" );
                CalcNode n;
                n.kind = CalcNode::Kind::Name;
                n.text = name.s;
                if (std::find(names.begin(), names.end(), name.s) == names.end()) names.push_back(name.s);
                return make(std::move(n));
            }
            case Tok::K::End:
                return fail(m_i == 0 ? std::string("the calculation is empty") : "something is missing at the end");
            default:
                return fail("Acorn didn't expect " + curly(shown(t)) + after());
        }
    }
    NodeP call(const Tok& name) {
        const Fn* f = find_fn(name.s);
        if (!f) return fail("there's no function " + curly(name.s));
        const auto open_at = name.at;
        take();   // (
        CalcNode n;
        n.kind = CalcNode::Kind::Call;
        n.text = f->name;
        if (peek().k != Tok::K::RParen) {
            while (true) {
                auto a = expr();
                if (!m_err.empty()) return nullptr;
                n.kids.push_back(a);
                if (peek().k == Tok::K::Comma) { take(); continue; }
                break;
            }
        }
        if (peek().k != Tok::K::RParen)
            return fail("a closing ) is missing after " + curly(m_src.substr(open_at, peek().at - open_at)));
        take();
        const int got = static_cast<int>(n.kids.size());
        if (got < f->min_args || got > f->max_args) {
            const auto want = f->min_args == f->max_args ? std::to_string(f->min_args)
                                                          : std::to_string(f->min_args) + " or " + std::to_string(f->max_args);
            return fail(curly(name.s) + " takes " + want + (f->max_args == 1 ? " value" : " values") + ", not " +
                        std::to_string(got));
        }
        return make(std::move(n));
    }

    const std::string& m_src;
    std::vector<Tok>   m_t;
    std::size_t        m_i = 0;
    std::string        m_err;
};

// ── evaluation ───────────────────────────────────────────────────────────────
struct Fail { std::string why; };
using Eval = std::variant<CalcValue, Fail>;

bool reads_number(const std::string& s, double& out) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    if (a == b) return false;
    const std::string t = s.substr(a, b - a);
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

std::string as_text(const CalcValue& v) {
    if (std::holds_alternative<std::monostate>(v)) return "";
    if (const auto* s = std::get_if<std::string>(&v)) return *s;
    return calc_text(v);
}
bool truthy(const CalcValue& v) {
    if (std::holds_alternative<std::monostate>(v)) return false;
    if (const auto* d = std::get_if<double>(&v)) return *d != 0;
    return !std::get<std::string>(v).empty();
}
std::size_t utf8_len(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}
// The byte offset of code point `cp` (clamped to the end).
std::size_t utf8_at(const std::string& s, std::size_t cp) {
    std::size_t i = 0, n = 0;
    while (i < s.size()) {
        if (n == cp) return i;
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        ++n;
    }
    return s.size();
}

class Evaluator {
public:
    Evaluator(const CalcLookup& f, const std::string& today) : m_field(f), m_today(today) {}

    Eval eval(const CalcNode& n) {
        using K = CalcNode::Kind;
        switch (n.kind) {
            case K::Number: return CalcValue{n.number};
            case K::Text:   return CalcValue{n.text};
            case K::Name: {
                auto v = m_field(n.text);
                if (!v) return Fail{"there's no field " + curly(n.text)};
                return *v;
            }
            case K::Neg: {
                auto a = eval(*n.kids[0]);
                if (auto* f = std::get_if<Fail>(&a)) return *f;
                const auto& v = std::get<CalcValue>(a);
                if (std::holds_alternative<std::monostate>(v)) return CalcValue{};
                double d;
                if (!number(v, d)) return Fail{curly(as_text(v)) + " is not a number"};
                return CalcValue{-d};
            }
            case K::Not: {
                auto a = eval(*n.kids[0]);
                if (auto* f = std::get_if<Fail>(&a)) return *f;
                return CalcValue{truthy(std::get<CalcValue>(a)) ? 0.0 : 1.0};
            }
            case K::Binary: return binary(n);
            case K::Call:   return call(n);
        }
        return Fail{"internal: unknown node"};
    }

private:
    static bool number(const CalcValue& v, double& out) {
        if (const auto* d = std::get_if<double>(&v)) { out = *d; return true; }
        if (const auto* s = std::get_if<std::string>(&v)) return reads_number(*s, out);
        return false;
    }

    Eval binary(const CalcNode& n) {
        // and / or look at the right only when they must.
        if (n.op == Op::And || n.op == Op::Or) {
            auto a = eval(*n.kids[0]);
            if (auto* f = std::get_if<Fail>(&a)) return *f;
            const bool l = truthy(std::get<CalcValue>(a));
            if (n.op == Op::And && !l) return CalcValue{0.0};
            if (n.op == Op::Or && l) return CalcValue{1.0};
            auto b = eval(*n.kids[1]);
            if (auto* f = std::get_if<Fail>(&b)) return *f;
            return CalcValue{truthy(std::get<CalcValue>(b)) ? 1.0 : 0.0};
        }
        auto ea = eval(*n.kids[0]);
        if (auto* f = std::get_if<Fail>(&ea)) return *f;
        auto eb = eval(*n.kids[1]);
        if (auto* f = std::get_if<Fail>(&eb)) return *f;
        const auto& a = std::get<CalcValue>(ea);
        const auto& b = std::get<CalcValue>(eb);
        switch (n.op) {
            case Op::Join: return CalcValue{as_text(a) + as_text(b)};
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: {
                if (std::holds_alternative<std::monostate>(a) || std::holds_alternative<std::monostate>(b))
                    return CalcValue{};
                double x, y;
                if (!number(a, x)) return Fail{curly(as_text(a)) + " is not a number"};
                if (!number(b, y)) return Fail{curly(as_text(b)) + " is not a number"};
                switch (n.op) {
                    case Op::Add: return CalcValue{x + y};
                    case Op::Sub: return CalcValue{x - y};
                    case Op::Mul: return CalcValue{x * y};
                    default:
                        if (y == 0) return Fail{"divided by zero"};
                        return CalcValue{x / y};
                }
            }
            default: {   // compare
                int c;
                const auto* da = std::get_if<double>(&a);
                const auto* db = std::get_if<double>(&b);
                if (da && db) c = *da < *db ? -1 : *da > *db ? 1 : 0;
                else {
                    const auto sa = as_text(a), sb = as_text(b);
                    c = sa < sb ? -1 : sa > sb ? 1 : 0;
                }
                bool r = false;
                switch (n.op) {
                    case Op::Eq: r = c == 0; break;
                    case Op::Ne: r = c != 0; break;
                    case Op::Lt: r = c < 0; break;
                    case Op::Gt: r = c > 0; break;
                    case Op::Le: r = c <= 0; break;
                    default:     r = c >= 0; break;
                }
                return CalcValue{r ? 1.0 : 0.0};
            }
        }
    }

    Eval arg(const CalcNode& n, std::size_t i) { return eval(*n.kids[i]); }

    Eval call(const CalcNode& n) {
        const std::string& f = n.text;
        if (f == "today") return CalcValue{m_today};
        if (f == "if") {
            auto t = arg(n, 0);
            if (auto* x = std::get_if<Fail>(&t)) return *x;
            if (truthy(std::get<CalcValue>(t))) return arg(n, 1);
            return n.kids.size() > 2 ? arg(n, 2) : Eval{CalcValue{}};
        }
        std::vector<CalcValue> a;
        for (std::size_t i = 0; i < n.kids.size(); ++i) {
            auto v = arg(n, i);
            if (auto* x = std::get_if<Fail>(&v)) return *x;
            a.push_back(std::get<CalcValue>(v));
        }
        const auto count = [&](std::size_t i, double& out) -> std::optional<Fail> {
            if (std::holds_alternative<std::monostate>(a[i])) { out = 0; return std::nullopt; }
            if (!number(a[i], out)) return Fail{curly(as_text(a[i])) + " is not a number"};
            return std::nullopt;
        };
        if (f == "length") return CalcValue{static_cast<double>(utf8_len(as_text(a[0])))};
        if (f == "upper" || f == "lower") {
            if (std::holds_alternative<std::monostate>(a[0])) return CalcValue{};
            auto s = as_text(a[0]);
            for (auto& ch : s) {
                const auto u = static_cast<unsigned char>(ch);
                if (u < 0x80) ch = static_cast<char>(f == "upper" ? std::toupper(u) : std::tolower(u));
            }
            return CalcValue{s};
        }
        if (f == "trim") {
            if (std::holds_alternative<std::monostate>(a[0])) return CalcValue{};
            auto s = as_text(a[0]);
            std::size_t x = 0, y = s.size();
            while (x < y && std::isspace(static_cast<unsigned char>(s[x]))) ++x;
            while (y > x && std::isspace(static_cast<unsigned char>(s[y - 1]))) --y;
            return CalcValue{s.substr(x, y - x)};
        }
        if (f == "left" || f == "right") {
            if (std::holds_alternative<std::monostate>(a[0])) return CalcValue{};
            double d;
            if (auto x = count(1, d)) return *x;
            const auto s = as_text(a[0]);
            const auto len = utf8_len(s);
            const std::size_t k = d <= 0 ? 0 : std::min<std::size_t>(len, static_cast<std::size_t>(std::floor(d)));
            if (f == "left") return CalcValue{s.substr(0, utf8_at(s, k))};
            return CalcValue{s.substr(utf8_at(s, len - k))};
        }
        if (f == "round") {
            if (std::holds_alternative<std::monostate>(a[0])) return CalcValue{};
            double x;
            if (!number(a[0], x)) return Fail{curly(as_text(a[0])) + " is not a number"};
            double places = 0;
            if (a.size() > 1) if (auto e = count(1, places)) return *e;
            const double p = std::pow(10.0, std::floor(places));
            return CalcValue{std::round(x * p) / p};
        }
        return Fail{"internal: no function " + f};
    }

    const CalcLookup&  m_field;
    const std::string& m_today;
};

}  // namespace

std::variant<CalcExpr, std::string> parse_calc(const std::string& src) {
    auto toks = lex(src);
    if (auto* why = std::get_if<std::string>(&toks)) return *why;
    Parser p(src, std::move(std::get<std::vector<Tok>>(toks)));
    auto r = p.parse();
    if (auto* why = std::get_if<std::string>(&r)) return *why;
    return CalcExpr{std::get<NodeP>(r), std::move(p.names)};
}

const char* summary_op_name(SummaryOp op) {
    switch (op) {
        case SummaryOp::Count:   return "count";
        case SummaryOp::Total:   return "total";
        case SummaryOp::Average: return "average";
        case SummaryOp::Min:     return "min";
        case SummaryOp::Max:     return "max";
    }
    return "count";
}
const char* summary_op_label(SummaryOp op) {
    switch (op) {
        case SummaryOp::Count:   return "Count";
        case SummaryOp::Total:   return "Total";
        case SummaryOp::Average: return "Average";
        case SummaryOp::Min:     return "Minimum";
        case SummaryOp::Max:     return "Maximum";
    }
    return "Count";
}
std::optional<SummaryOp> parse_summary_op(const std::string& s) {
    for (auto op : {SummaryOp::Count, SummaryOp::Total, SummaryOp::Average, SummaryOp::Min, SummaryOp::Max})
        if (s == summary_op_name(op)) return op;
    return std::nullopt;
}
std::string summary_says(const Summary& s) { return std::string(summary_op_label(s.op)) + " of " + s.field; }

std::string check_summary(const Summary& s, const std::vector<std::string>& fields, const std::vector<std::string>& taken) {
    if (auto why = check_calc_name(s.name); !why.empty())
        return why.find("A calculation") == 0 ? "A summary" + why.substr(13) : why;
    if (std::find(fields.begin(), fields.end(), s.name) != fields.end())
        return curly(s.table) + " already has a field " + curly(s.name);
    if (std::find(taken.begin(), taken.end(), s.name) != taken.end())
        return curly(s.table) + " already has a calculation or summary " + curly(s.name);
    if (s.field.empty()) return "Pick the field to summarise";
    if (std::find(fields.begin(), fields.end(), s.field) == fields.end())
        return "there's no field " + curly(s.field) + " in " + curly(s.table);
    return "";
}

std::string check_calc_name(const std::string& name) {
    if (name.empty()) return "A calculation needs a name";
    if (name.size() > 64) return "A name can be at most 64 bytes";
    if (std::isdigit(static_cast<unsigned char>(name[0]))) return "A name can't start with a digit";
    for (unsigned char c : name)
        if (!name_char(c)) return "A calculation's name is letters, digits and _ only -- " + curly(name) + " isn't";
    if (is_word(name)) return curly(name) + " is a word of the calculation language";
    return "";
}

std::string check_calc(const Calc& c, const std::vector<std::string>& fields, const std::map<std::string, Calc>& calcs) {
    if (auto why = check_calc_name(c.name); !why.empty()) return why;
    if (std::find(fields.begin(), fields.end(), c.name) != fields.end())
        return curly(c.table) + " already has a field " + curly(c.name);
    auto p = parse_calc(c.expr);
    if (auto* why = std::get_if<std::string>(&p)) return *why;
    // The table's calculations as they would be with `c` kept.
    std::map<std::string, CalcExpr> parsed;
    for (const auto& [n, other] : calcs) {
        if (n == c.name) continue;
        auto q = parse_calc(other.expr);
        if (auto* e = std::get_if<CalcExpr>(&q)) parsed[n] = std::move(*e);
    }
    parsed[c.name] = std::get<CalcExpr>(p);
    for (const auto& n : parsed[c.name].names)
        if (std::find(fields.begin(), fields.end(), n) == fields.end() && !parsed.contains(n))
            return "there's no field " + curly(n) + " in " + curly(c.table);
    // A loop: does anything `c` reads lead back to it?
    std::set<std::string> seen;
    std::vector<std::string> todo = parsed[c.name].names;
    while (!todo.empty()) {
        const auto n = todo.back();
        todo.pop_back();
        if (n == c.name) return curly(c.name) + " would end up reading itself";
        if (!seen.insert(n).second) continue;
        if (auto it = parsed.find(n); it != parsed.end())
            for (const auto& m : it->second.names) todo.push_back(m);
    }
    return "";
}

CalcOutcome evaluate(const CalcExpr& e, const CalcLookup& field, const std::string& today) {
    if (!e.root) return {{}, "the calculation is empty"};
    Evaluator ev(field, today);
    auto r = ev.eval(*e.root);
    if (auto* f = std::get_if<Fail>(&r)) return {{}, f->why};
    auto v = std::get<CalcValue>(r);
    if (const auto* d = std::get_if<double>(&v); d && !std::isfinite(*d)) return {{}, "the number is too big"};
    return {std::move(v), ""};
}

std::map<std::string, CalcOutcome> evaluate_all(const std::map<std::string, Calc>& calcs, const CalcLookup& field,
                                                const std::string& today) {
    std::map<std::string, CalcOutcome> done;
    std::map<std::string, std::variant<CalcExpr, std::string>> parsed;
    for (const auto& [n, c] : calcs) parsed.emplace(n, parse_calc(c.expr));
    std::set<std::string> busy;   // being worked out now: meeting one again is a loop
    std::function<CalcOutcome(const std::string&)> work = [&](const std::string& n) -> CalcOutcome {
        if (auto it = done.find(n); it != done.end()) return it->second;
        if (busy.contains(n)) return {{}, curly(n) + " ends up reading itself"};
        busy.insert(n);
        CalcOutcome o;
        const auto& p = parsed.at(n);
        if (const auto* why = std::get_if<std::string>(&p)) o = {{}, *why};
        else {
            std::string inner_fail;
            CalcLookup look = [&](const std::string& name) -> std::optional<CalcValue> {
                if (auto v = field(name)) return v;
                if (!calcs.contains(name)) return std::nullopt;
                auto r = work(name);
                if (!r.ok()) { if (inner_fail.empty()) inner_fail = curly(name) + ": " + r.error; return CalcValue{}; }
                return r.value;
            };
            o = evaluate(std::get<CalcExpr>(p), look, today);
            if (!inner_fail.empty()) o = {{}, inner_fail};
        }
        busy.erase(n);
        done[n] = o;
        return o;
    };
    for (const auto& [n, c] : calcs) work(n);
    return done;
}

std::string calc_text(const CalcValue& v) {
    if (std::holds_alternative<std::monostate>(v)) return "";
    if (const auto* s = std::get_if<std::string>(&v)) return *s;
    const double d = std::get<double>(v);
    char buf[64];
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(d));
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%.10g", d);
    return buf;
}

std::string calc_text(const CalcOutcome& o) { return o.ok() ? calc_text(o.value) : "?"; }

std::string today_iso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

// s023a: a script's If -- the language's own truth (empty, 0 and "" are false).
bool calc_truthy(const CalcValue& v) { return truthy(v); }

}  // namespace acorn::core
