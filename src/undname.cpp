// OpenMSDIA's undecorator for Microsoft C++ decorated names.
//
// Written from the published description of the MSVC name-mangling scheme
// and from the output of the real msdia140.dll, recorded by
// walker/name_probe.cpp in oracle/undname and compared by
// tools/undname_test.py. The spelling (spaces, keyword order, the quirks
// of each UNDNAME_* option) follows that output.
//
// The decoder parses a decorated name into a small tree and renders it.
// Types are rendered around a declarator, C style: a pointer to an array
// of ten ints named p is "int (* p)[10]".
#include "undname.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <memory>
#include <string>
#include <vector>

namespace om {
namespace {

enum class K { Basic, Udt, Ptr, Func, Array, CvType, Empty };

enum : unsigned { CV_CONST = 1, CV_VOLATILE = 2 };

struct Node {
    K k = K::Basic;
    std::string text;     // Basic: the type name; Udt: the qualified name
    std::string keyword;  // Udt: class, struct, union, enum...
    unsigned cv = 0;      // const/volatile of the value (Basic, Udt)
    bool unaligned = false;  // __unaligned value (pointee of an F pointer)
    // Ptr
    std::string sym;            // "*", "&", "&&"
    std::string cls;            // a pointer to member's class
    unsigned ptrCv = 0;         // the pointer's own const/volatile
    std::vector<std::string> mods;  // __ptr64, __restrict, __unaligned
    Node *target = nullptr;     // Ptr: pointee; Array: element; CvType: the type
    // Func
    std::string cc;
    Node *ret = nullptr;        // null: no return type (constructors)
    std::vector<Node *> args;
    bool argsVoid = false, variadic = false, noexcept_ = false;
    unsigned thisCv = 0;
    std::vector<std::string> thisMods;
    int thisRef = 0;            // 1: &, 2: &&
    bool thisUnaligned = false;
    // Array
    std::vector<int64_t> dims;
};

// A qualified name: components innermost first, as decorated.
struct Name {
    std::vector<std::string> parts;
    int special = 0;         // 1: constructor, 2: destructor, 3: conversion operator
    bool isOperator = false;  // the first component is an operator or special name
    std::string tmplArgs;    // template arguments of a special name ("<int>")
    std::string str() const
    {
        std::string s;
        for (size_t i = parts.size(); i-- > 0;) {
            s += parts[i];
            if (i) s += "::";
        }
        return s;
    }
};

class Undecorator {
public:
    Undecorator(const std::string &s, unsigned flags)
        : s(s), f(flags), arena(&ownArena), nameOnly((flags & UND_NAME_ONLY) != 0) {}
    // A symbol nested at the current position (template arguments, scopes,
    // dynamic initializers): shares the back-reference tables.
    std::string subSymbol()
    {
        Undecorator sub(s, f);
        sub.arena = arena;
        sub.outermost = false;
        sub.i = i;
        sub.names = names;
        sub.types = types;
        std::string r = sub.symbol();
        if (sub.err) return err = true, "";
        i = sub.i;
        names = sub.names;
        types = sub.types;
        return r;
    }
    std::string run();

private:
    const std::string &s;
    size_t i = 0;
    unsigned f;
    bool err = false;
    std::vector<std::unique_ptr<Node>> ownArena, *arena;
    std::vector<std::string> names;  // name back-references
    std::vector<Node *> types;       // argument type back-references

    // ---------------------------------------------------------------- input
    char peek(size_t o = 0) const { return i + o < s.size() ? s[i + o] : '\0'; }
    char get() { return i < s.size() ? s[i++] : (err = true, '\0'); }
    bool eat(char c)
    {
        if (peek() != c) return false;
        i++;
        return true;
    }
    bool eat(const char *p)
    {
        size_t n = 0;
        while (p[n]) n++;
        if (s.compare(i, n, p) != 0) return false;
        i += n;
        return true;
    }
    Node *node(K k)
    {
        arena->emplace_back(new Node);
        arena->back()->k = k;
        return arena->back().get();
    }

    // ---------------------------------------------------------------- options
    bool has(unsigned o) const { return (f & o) != 0; }
    std::string kw(const char *k) const
    {
        if (has(UND_NO_MS_KEYWORDS)) return "";
        if (has(UND_NO_LEADING_UNDERSCORES))
            while (*k == '_') k++;
        return k;
    }
    std::string mod(const std::string &m) const
    {
        if (m == "__ptr64" && has(UND_NO_PTR64)) return "";
        return kw(m.c_str());
    }
    bool ecsu() const { return !has(UND_NO_ECSU) && !nameOnly; }
    bool nameOnly;  // UND_NAME_ONLY: no class/struct/union/enum keywords
    bool outermost = true;  // not a symbol nested in another one

    // ---------------------------------------------------------------- numbers
    // A decorated number: [?] then 0-9 for 1-10, or hex digits A-P and '@'.
    bool number(uint64_t &v, bool &neg)
    {
        neg = eat('?');
        char c = peek();
        if (c >= '0' && c <= '9') {
            i++;
            v = (uint64_t)(c - '0') + 1;
            return true;
        }
        v = 0;
        while (peek() >= 'A' && peek() <= 'P') v = v * 16 + (uint64_t)(get() - 'A');
        if (!eat('@')) return err = true, false;
        return true;
    }
    std::string signedNumber()
    {
        uint64_t v;
        bool neg;
        if (!number(v, neg)) return "";
        return (neg ? "-" : "") + std::to_string(v);
    }
    std::string unsignedNumber()
    {
        uint64_t v;
        bool neg;
        if (!number(v, neg)) return "";
        return std::to_string(neg ? (uint64_t)(-(int64_t)v) : v);
    }

    // ---------------------------------------------------------------- names
    std::string identifier()
    {
        size_t at = s.find('@', i);
        if (at == std::string::npos) return err = true, "";
        std::string id = s.substr(i, at - i);
        i = at + 1;
        return id;
    }
    void memorize(const std::string &n)
    {
        for (auto &x : names)
            if (x == n) return;
        if (names.size() < 10) names.push_back(n);
    }
    std::string backref(char c)
    {
        size_t k = (size_t)(c - '0');
        if (k >= names.size()) return err = true, "";
        return names[k];
    }
    std::string templateName(Name *special);
    std::string templateArgs();
    std::string classValue();
    std::string operatorName(Name &n, bool inTemplate);
    std::string scopePiece();
    Name qualifiedName(bool symbol);
    void finishSpecial(Name &n);

    // ---------------------------------------------------------------- types
    Node *type(bool memo, bool allowVoid = true);
    Node *basicOrUdt(char c);
    Node *pointerTo(Node *p, unsigned cvLetterOnly = 0);
    Node *functionType(bool member, const std::string &cls);
    void thisQualifiers(Node *fn);
    void argList(Node *fn);
    bool callingConvention(std::string &cc);
    unsigned cvLetter(char c, bool &member, bool &ok);

    std::string cvText(unsigned cv) const
    {
        if (cv == (CV_CONST | CV_VOLATILE)) return "const volatile";
        if (cv == CV_CONST) return "const";
        if (cv == CV_VOLATILE) return "volatile";
        return "";
    }
    static std::string join(const std::string &a, const std::string &b)
    {
        if (b.empty()) return a;
        if (a.empty()) return b;
        if (a.back() == ' ' || b[0] == ' ') return a + b;
        return a + " " + b;
    }
    std::string render(Node *t, const std::string &decl, bool top = false);
    std::string renderArgs(Node *fn);
    std::string thisText(Node *fn);

    // ---------------------------------------------------------------- symbols
    std::string symbol();
    std::string encoding(Name &n);
    std::string typeOnly();
};

// ======================================================================== names

static const char *const kOps[] = {
    // ?0 .. ?9
    nullptr, nullptr, "operator new", "operator delete", "operator=", "operator>>", "operator<<",
    "operator!", "operator==", "operator!=",
};
static const char *const kOpsAZ[] = {
    "operator[]", nullptr /* conversion */, "operator->", "operator*", "operator++", "operator--",
    "operator-", "operator+", "operator&", "operator->*", "operator/", "operator%", "operator<",
    "operator<=", "operator>", "operator>=", "operator,", "operator()", "operator~", "operator^",
    "operator|", "operator&&", "operator||", "operator*=", "operator+=", "operator-=",
};
static const char *const kOps_[] = {
    // ?_0 .. ?_9
    "operator/=", "operator%=", "operator>>=", "operator<<=", "operator&=", "operator|=", "operator^=",
    "`vftable'", "`vbtable'", "`vcall'",
};
static const char *const kOps_AZ[] = {
    "`typeof'", "`local static guard'", "`string'", "`vbase destructor'",
    "`vector deleting destructor'", "`default constructor closure'", "`scalar deleting destructor'",
    "`vector constructor iterator'", "`vector destructor iterator'",
    "`vector vbase constructor iterator'", "`virtual displacement map'",
    "`eh vector constructor iterator'", "`eh vector destructor iterator'",
    "`eh vector vbase constructor iterator'", "`copy constructor closure'", "`udt returning'",
    nullptr, nullptr /* RTTI */, "`local vftable'", "`local vftable constructor closure'",
    "operator new[]", "operator delete[]", nullptr, "`placement delete closure'",
    "`placement delete[] closure'", nullptr,
};
static const char *const kOps__AZ[] = {
    "`managed vector constructor iterator'", "`managed vector destructor iterator'",
    "`eh vector copy constructor iterator'", "`eh vector vbase copy constructor iterator'",
    nullptr /* dynamic initializer */, nullptr /* dynamic atexit destructor */,
    "`vector copy constructor iterator'", "`vector vbase copy constructor iterator'",
    "`managed vector copy constructor iterator'", "`local static thread guard'",
    nullptr /* literal operator */, "operator co_await", "operator<=>",
};

// After '?' in a name: an operator or special name. Constructors and
// destructors get their class name in finishSpecial.
std::string Undecorator::operatorName(Name &n, bool inTemplate)
{
    char c = get();
    if (c >= '0' && c <= '9') {
        if (c == '0') { n.special = 1; return ""; }
        if (c == '1') { n.special = 2; return ""; }
        return kOps[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        if (c == 'B') { n.special = 3; return "operator"; }
        return kOpsAZ[c - 'A'];
    }
    if (c != '_') return err = true, "";
    c = get();
    if (c >= '0' && c <= '9') return kOps_[c - '0'];
    if (c == 'R') {
        char r = get();
        switch (r) {
        case '0': {
            // A type, then the rest of the name.
            Node *t = type(false);
            if (err) return "";
            return render(t, "") + " `RTTI Type Descriptor'";
        }
        case '1': {
            std::string a = signedNumber(), b = signedNumber(), c2 = signedNumber(), d = signedNumber();
            return "`RTTI Base Class Descriptor at (" + a + "," + b + "," + c2 + "," + d + ")'";
        }
        case '2': return "`RTTI Base Class Array'";
        case '3': return "`RTTI Class Hierarchy Descriptor'";
        case '4': return "`RTTI Complete Object Locator'";
        }
        return err = true, "";
    }
    if (c >= 'A' && c <= 'Z') {
        const char *t = kOps_AZ[c - 'A'];
        if (!t) return err = true, "";
        return t;
    }
    if (c != '_') return err = true, "";
    c = get();
    if (c < 'A' || c > 'N') return err = true, "";
    if (c == 'E' || c == 'F') {
        std::string what = c == 'E' ? "`dynamic initializer for '" : "`dynamic atexit destructor for '";
        std::string inner;
        if (peek() == '?') {
            inner = subSymbol();
            if (err || !eat('@')) return err = true, "";
        } else {
            inner = identifier();
        }
        return what + inner + "''";
    }
    if (c == 'K') return "operator \"\" " + identifier();
    if (c == 'N') {
        if (!eat('2')) return err = true, "";
        return classValue();
    }
    const char *t = kOps__AZ[c - 'A'];
    if (!t) return err = true, "";
    (void)inTemplate;
    return t;
}

// "?$" already consumed: name@ then template arguments up to '@'.
std::string Undecorator::templateName(Name *special)
{
    std::vector<std::string> outerNames;
    std::vector<Node *> outerTypes;
    std::swap(outerNames, names);
    std::swap(outerTypes, types);
    std::string base;
    if (eat('?')) {
        Name n;
        base = operatorName(n, true);
        if (special) {
            special->special = n.special;
            if (n.special == 3) base = "operator";
        }
    } else {
        base = identifier();
        memorize(base);
    }
    std::string args = templateArgs();
    std::swap(outerNames, names);
    std::swap(outerTypes, types);
    if (special && special->special) {
        special->tmplArgs = args;
        return base;
    }
    return base + args;
}

std::string Undecorator::templateArgs()
{
    std::vector<std::string> out;
    while (!err && peek() != '@') {
        if (i >= s.size()) return err = true, "";
        if (eat("$$V") || eat("$$Z") || eat("$S")) continue;
        if (peek() == '$' && peek(1) == '$' && peek(2) == '$' && peek(3) == 'V') {
            i += 4;
            continue;
        }
        if (peek() == '$' && peek(1) != '$') {
            i++;
            char c = get();
            switch (c) {
            case '0': out.push_back(signedNumber()); break;
            case '1': {
                // &symbol
                std::string sym = subSymbol();
                if (err) return "";
                out.push_back("&" + sym);
                break;
            }
            case 'E': {
                std::string sym = subSymbol();
                if (err) return "";
                out.push_back(sym);
                break;
            }
            case 'F': {
                std::string a = signedNumber(), b = signedNumber();
                out.push_back("{" + a + "," + b + "}");
                break;
            }
            case 'G': {
                std::string a = signedNumber(), b = signedNumber(), d = signedNumber();
                out.push_back("{" + a + "," + b + "," + d + "}");
                break;
            }
            case 'H': case 'I': case 'J': {
                std::string sym = subSymbol();
                if (err) return "";
                std::string v = "{" + sym;
                for (int n = 0; n < c - 'G'; n++) v += "," + signedNumber();
                out.push_back(v + "}");
                break;
            }
            case 'D': out.push_back("`template-parameter" + signedNumber() + "'"); break;
            case '2': out.push_back(classValue()); break;
            case 'B': {
                uint64_t v;
                bool neg;
                if (!number(v, neg)) return "";
                double d;
                memcpy(&d, &v, sizeof d);
                char b[64];
                snprintf(b, sizeof b, "%f", neg ? -d : d);
                out.push_back(b);
                break;
            }
            case 'M': {
                // A non-type argument with its type: the value only.
                type(false);
                if (err) return "";
                char v = peek();
                if (v == '0') { i++; out.push_back(signedNumber()); }
                else if (v == '1' || v == 'E') {
                    i++;
                    std::string sym = subSymbol();
                if (err) return "";
                out.push_back(v == '1' ? "&" + sym : sym);
                } else if (v == 'F' || v == 'G') {
                    i++;
                    std::string a = signedNumber(), b = signedNumber();
                    if (v == 'G') b += "," + signedNumber();
                    out.push_back("{" + a + "," + b + "}");
                } else if (v == 'H' || v == 'I' || v == 'J') {
                    i++;
                    std::string sym = subSymbol();
                if (err) return "";
                std::string val = "{" + sym;
                    for (int n = 0; n < v - 'G'; n++) val += "," + signedNumber();
                    out.push_back(val + "}");
                } else {
                    return err = true, "";
                }
                break;
            }
            default: return err = true, "";
            }
            continue;
        }
        Node *t = type(false);
        if (err) return "";
        out.push_back(render(t, ""));
    }
    if (!eat('@')) return err = true, "";
    std::string r = "<";
    for (size_t k = 0; k < out.size(); k++) r += (k ? "," : "") + out[k];
    if (r.back() == '>') r += ' ';
    return r + ">";
}

// A class-type non-type template argument: the type, then its members'
// values up to '@': "struct Lit{int:7}".
std::string Undecorator::classValue()
{
    Node *t = type(false);
    if (err) return "";
    std::string r = render(t, "") + "{";
    bool first = true;
    while (!err && !eat('@')) {
        if (i >= s.size()) return err = true, "";
        Node *ft = type(false);
        if (err) return "";
        std::string v;
        if (eat('0')) v = signedNumber();
        else if (eat('2')) v = classValue();
        else return err = true, "";
        r += (first ? "" : ",") + render(ft, "") + ":" + v;
        first = false;
    }
    return r + "}";
}

// One scope component of a qualified name.
std::string Undecorator::scopePiece()
{
    char c = peek();
    if (c >= '0' && c <= '9') {
        i++;
        return backref(c);
    }
    if (c != '?') {
        std::string id = identifier();
        memorize(id);
        return id;
    }
    i++;
    c = peek();
    if (c == '$') {
        i++;
        std::string t = templateName(nullptr);
        memorize(t);
        return t;
    }
    if (c == '?') {
        std::string inner = subSymbol();
                if (err) return "";
                return "`" + inner + "'";
    }
    if (c == 'A') {
        identifier();
        std::string n = "`anonymous namespace'";
        memorize(n);
        return n;
    }
    if ((c >= '0' && c <= '9') || (c >= 'B' && c <= 'P') || c == '@') {
        uint64_t v;
        bool neg;
        if (!number(v, neg)) return "";
        return "`" + std::to_string(v) + "'";
    }
    return err = true, "";
}

// A qualified name up to and including its terminating '@'. A symbol's
// first component may be an operator or special name.
Name Undecorator::qualifiedName(bool symbol)
{
    Name n;
    char c = peek();
    if (symbol && c == '?' && peek(1) == '$') {
        i += 2;
        n.parts.push_back(templateName(&n));
    } else if (symbol && c == '?') {
        i++;
        n.isOperator = true;
        n.parts.push_back(operatorName(n, false));
    } else if (c == '?' && peek(1) == '$') {
        i += 2;
        std::string t = templateName(nullptr);
        memorize(t);
        n.parts.push_back(t);
    } else if (c >= '0' && c <= '9') {
        i++;
        n.parts.push_back(backref(c));
    } else {
        std::string id = identifier();
        memorize(id);
        n.parts.push_back(id);
    }
    while (!err && !eat('@')) {
        if (i >= s.size()) {
            err = true;
            break;
        }
        n.parts.push_back(scopePiece());
    }
    if (n.special == 1 || n.special == 2) {
        if (n.parts.size() < 2) {
            err = true;
            return n;
        }
        std::string cls = n.parts[1];
        size_t lt = cls.find('<');
        // A constructor of a class template is named after the class with
        // its arguments.
        n.parts[0] = (n.special == 2 ? "~" : "") + cls + n.tmplArgs;
        (void)lt;
    }
    return n;
}

// ======================================================================== types

unsigned Undecorator::cvLetter(char c, bool &member, bool &ok)
{
    ok = true;
    member = false;
    switch (c) {
    case 'A': return 0;
    case 'B': return CV_CONST;
    case 'C': return CV_VOLATILE;
    case 'D': return CV_CONST | CV_VOLATILE;
    case 'Q': member = true; return 0;
    case 'R': member = true; return CV_CONST;
    case 'S': member = true; return CV_VOLATILE;
    case 'T': member = true; return CV_CONST | CV_VOLATILE;
    }
    ok = false;
    return 0;
}

bool Undecorator::callingConvention(std::string &cc)
{
    char c = get();
    switch (c) {
    case 'A': case 'B': cc = "__cdecl"; return true;
    case 'C': cc = "__preserve_none"; return true;
    case 'D': cc = "__pascal"; return true;
    case 'E': case 'F': cc = "__thiscall"; return true;
    case 'G': case 'H': cc = "__stdcall"; return true;
    case 'I': case 'J': cc = "__fastcall"; return true;
    case 'M': case 'N': cc = "__clrcall"; return true;
    case 'O': case 'P': cc = "__eabi"; return true;
    case 'Q': cc = "__vectorcall"; return true;
    case 'S': cc = "__swift_1"; return true;
    case 'U': cc = "__swift_2"; return true;
    case 'W': cc = "__swift_3"; return true;
    }
    err = true;
    return false;
}

Node *Undecorator::basicOrUdt(char c)
{
    Node *t = node(K::Basic);
    switch (c) {
    case 'C': t->text = "signed char"; return t;
    case 'D': t->text = "char"; return t;
    case 'E': t->text = "unsigned char"; return t;
    case 'F': t->text = "short"; return t;
    case 'G': t->text = "unsigned short"; return t;
    case 'H': t->text = "int"; return t;
    case 'I': t->text = "unsigned int"; return t;
    case 'J': t->text = "long"; return t;
    case 'K': t->text = "unsigned long"; return t;
    case 'M': t->text = "float"; return t;
    case 'N': t->text = "double"; return t;
    case 'O': t->text = "long double"; return t;
    case 'X': t->text = "void"; return t;
    case 'T': case 'U': case 'V': {
        t->k = K::Udt;
        t->keyword = c == 'T' ? "union" : c == 'U' ? "struct" : "class";
        t->text = qualifiedName(false).str();
        return t;
    }
    case 'W': {
        char u = get();
        if (u < '0' || u > '7') return err = true, t;
        t->k = K::Udt;
        t->keyword = "enum";
        t->text = qualifiedName(false).str();
        return t;
    }
    case '_': {
        char x = get();
        switch (x) {
        case 'D': t->text = "__int8"; return t;
        case 'E': t->text = "unsigned __int8"; return t;
        case 'F': t->text = "__int16"; return t;
        case 'G': t->text = "unsigned __int16"; return t;
        case 'H': t->text = "__int32"; return t;
        case 'I': t->text = "unsigned __int32"; return t;
        case 'J': t->text = "__int64"; return t;
        case 'K': t->text = "unsigned __int64"; return t;
        case 'L': t->text = "__int128"; return t;
        case 'M': t->text = "unsigned __int128"; return t;
        case 'N': t->text = "bool"; return t;
        case 'P': t->text = "auto"; return t;
        case 'Q': t->text = "char8_t"; return t;
        case 'S': t->text = "char16_t"; return t;
        case 'T': t->text = "decltype(auto)"; return t;
        case 'U': t->text = "char32_t"; return t;
        case 'W': t->text = "wchar_t"; return t;
        case 'X': case 'Y':
            t->k = K::Udt;
            t->keyword = x == 'X' ? "coclass" : "cointerface";
            t->text = qualifiedName(false).str();
            return t;
        }
        return err = true, t;
    }
    }
    return err = true, t;
}

// The pointee of a pointer or reference whose kind letter and own cv were
// consumed: modifiers, the pointee cv (or member/function marker), type.
Node *Undecorator::pointerTo(Node *p, unsigned)
{
    bool unaligned = false;
    for (;;) {
        char c = peek();
        if (c == 'E') p->mods.push_back("__ptr64");
        else if (c == 'F') unaligned = true;
        else if (c == 'I') p->mods.push_back("__restrict");
        else break;
        i++;
    }
    char c = get();
    if (c == '6') {
        p->target = functionType(false, "");
        return p;
    }
    if (c == '8') {
        std::string cls = qualifiedName(false).str();
        p->target = functionType(true, cls);
        p->cls = cls;
        return p;
    }
    bool member, ok;
    unsigned cv = cvLetter(c, member, ok);
    if (!ok) return err = true, p;
    if (member) p->cls = qualifiedName(false).str();
    Node *t = type(false);
    if (err) return p;
    if (t->k == K::Ptr) t->ptrCv = cv;
    else t->cv |= cv;
    if (unaligned) t->unaligned = true;  // printed even where the pointer's cv is not
    p->target = t;
    return p;
}

void Undecorator::thisQualifiers(Node *fn)
{
    for (;;) {
        char c = peek();
        if (c == 'E') fn->thisMods.push_back("__ptr64");
        else if (c == 'F') fn->thisUnaligned = true;
        else if (c == 'I') fn->thisMods.push_back("__restrict");
        else if (c == 'G') fn->thisRef = 1;
        else if (c == 'H') fn->thisRef = 2;
        else break;
        i++;
    }
    bool member, ok;
    fn->thisCv = cvLetter(get(), member, ok);
    if (!ok || member) err = true;
}

void Undecorator::argList(Node *fn)
{
    if (eat('X')) {
        fn->argsVoid = true;
        return;
    }
    while (!err) {
        if (eat('@')) return;
        if (eat('Z')) {
            fn->variadic = true;
            return;
        }
        char c = peek();
        if (c >= '0' && c <= '9') {
            i++;
            size_t k = (size_t)(c - '0');
            if (k >= types.size()) {
                err = true;
                return;
            }
            fn->args.push_back(types[k]);
            continue;
        }
        size_t start = i;
        Node *t = type(true);
        if (err) return;
        if (i - start > 1 && types.size() < 10) types.push_back(t);
        fn->args.push_back(t);
    }
}

// After '6' or '8' (and the class of a member function): the function.
Node *Undecorator::functionType(bool member, const std::string &cls)
{
    Node *fn = node(K::Func);
    fn->cls = cls;
    if (member) thisQualifiers(fn);
    if (!callingConvention(fn->cc)) return fn;
    if (eat('@')) {
        fn->ret = nullptr;
    } else {
        fn->ret = type(false);
    }
    argList(fn);
    if (eat("_E")) fn->noexcept_ = true;
    else if (!eat('Z')) err = true;
    return fn;
}

Node *Undecorator::type(bool memo, bool)
{
    (void)memo;
    char c = get();
    if (err) return node(K::Empty);
    switch (c) {
    case 'A': case 'B': {
        Node *p = node(K::Ptr);
        p->sym = "&";
        p->ptrCv = c == 'B' ? CV_VOLATILE : 0;
        return pointerTo(p);
    }
    case 'P': case 'Q': case 'R': case 'S': {
        Node *p = node(K::Ptr);
        p->sym = "*";
        p->ptrCv = c == 'P' ? 0 : c == 'Q' ? CV_CONST : c == 'R' ? CV_VOLATILE : CV_CONST | CV_VOLATILE;
        return pointerTo(p);
    }
    case 'Y': {
        Node *a = node(K::Array);
        uint64_t n;
        bool neg;
        if (!number(n, neg)) return a;
        for (uint64_t k = 0; k < n && !err; k++) {
            uint64_t d;
            if (!number(d, neg)) return a;
            a->dims.push_back((int64_t)d);
        }
        a->target = type(false);
        return a;
    }
    case '?': {
        // A storage-qualified type: cv letter, then the type.
        bool member, ok;
        unsigned cv = cvLetter(get(), member, ok);
        if (!ok) return err = true, node(K::Empty);
        if (member) qualifiedName(false);
        if (peek() == '?') {
            // A placeholder such as ?<auto>@@ or a back-reference ?1@.
            Node *t = node(K::Basic);
            i++;
            if (peek() >= '0' && peek() <= '9') {
                t->text = backref(get());
                eat('@');
            } else {
                t->text = qualifiedName(false).str();
            }
            t->cv = cv;
            return t;
        }
        Node *t = type(false);
        if (t->k == K::Ptr) {
            t->ptrCv = cv;
            return t;
        }
        if (!cv) return t;
        // Rendered with a trailing space, like $$C.
        Node *w = node(K::CvType);
        w->target = t;
        w->cv = cv;
        return w;
    }
    case '$': {
        if (eat('$')) {
            char d = get();
            switch (d) {
            case 'Q': case 'R': {
                Node *p = node(K::Ptr);
                p->sym = "&&";
                p->ptrCv = d == 'R' ? CV_VOLATILE : 0;
                return pointerTo(p);
            }
            case 'A': {
                char k = get();
                if (k == '6') return functionType(false, "");
                if (k == '8') {
                    qualifiedName(false);
                    return functionType(true, "");
                }
                return err = true, node(K::Empty);
            }
            case 'B': {
                Node *t = type(false);
                return t;
            }
            case 'C': {
                bool member, ok;
                unsigned cv = cvLetter(get(), member, ok);
                if (!ok) return err = true, node(K::Empty);
                Node *w = node(K::CvType);
                w->target = type(false);
                w->cv = cv;
                return w;
            }
            case 'T': {
                Node *t = node(K::Basic);
                t->text = "std::nullptr_t";
                return t;
            }
            case 'Y': {
                Node *t = node(K::Basic);
                t->text = qualifiedName(false).str();
                return t;
            }
            case 'V': case 'Z': return node(K::Empty);
            }
            return err = true, node(K::Empty);
        }
        return err = true, node(K::Empty);
    }
    }
    if (c >= '0' && c <= '9') {
        size_t k = (size_t)(c - '0');
        if (k >= types.size()) return err = true, node(K::Empty);
        return types[k];
    }
    return basicOrUdt(c);
}

// ======================================================================== rendering

std::string Undecorator::thisText(Node *fn)
{
    if ((f & UND_NO_THISTYPE) == UND_NO_THISTYPE) return "";
    std::string t = cvText(fn->thisCv);
    if (fn->thisUnaligned) {
        std::string u = kw("__unaligned");
        if (!u.empty()) t += (t.empty() ? "" : " ") + u;
    }
    if (!t.empty()) t += " ";
    for (auto &m : fn->thisMods) {
        std::string k = mod(m);
        if (k.empty()) continue;
        if (t.empty() || t.back() != ' ') t += " ";
        t += k;
    }
    if (fn->thisRef) t += fn->thisRef == 1 ? "& " : "&& ";
    return t;
}

std::string Undecorator::renderArgs(Node *fn)
{
    std::string a = "(";
    if (fn->argsVoid) return "(void)";
    for (size_t k = 0; k < fn->args.size(); k++) a += (k ? "," : "") + render(fn->args[k], "");
    if (fn->variadic) a += std::string(fn->args.empty() ? "" : ",") + (has(UND_ELLIPSIS_TEXT) ? "<ellipsis>" : "...");
    return a + ")";
}

std::string Undecorator::render(Node *t, const std::string &decl, bool top)
{
    switch (t->k) {
    case K::Empty: return decl;
    case K::Basic:
    case K::Udt: {
        std::string b = t->text;
        if (t->k == K::Udt && (t->keyword == "enum" ? !has(UND_NO_ECSU) : ecsu())) b = t->keyword + " " + b;
        std::string cv = cvText(t->cv);
        if (!cv.empty()) b += " " + cv;
        if (t->unaligned) {
            std::string u = kw("__unaligned");
            if (!u.empty()) b += " " + u;
        }
        return join(b, decl);
    }
    case K::CvType: {
        Node tmp = *t->target;
        tmp.cv |= t->cv;
        std::string b = render(&tmp, "", top) + " ";
        return join(b, decl);
    }
    case K::Array: {
        std::string d = decl;
        for (int64_t n : t->dims) d += "[" + std::to_string(n) + "]";
        return render(t->target, d, true);
    }
    case K::Func: {
        std::string d = kw(t->cc.c_str()) + renderArgs(t) + thisText(t);
        if (t->noexcept_) d += " noexcept";
        d = join(decl, d);
        if (!t->ret) return d;
        return render(t->ret, d, true);
    }
    case K::Ptr: break;
    }
    // Pointers and references.
    Node *to = t->target;
    if (to && to->k == K::Func) {
        std::string in;
        std::string cc = kw(to->cc.c_str());
        if (!t->cls.empty()) in = "(" + cc + " " + t->cls + "::" + t->sym;
        else in = "(" + cc + t->sym;
        for (auto &m : t->mods) {
            std::string k = mod(m);
            if (!k.empty()) in += " " + k;
        }
        if (!top && t->ptrCv) {
            in += cvText(t->ptrCv);
            if (!decl.empty() && decl[0] != ' ') in += " ";
        }
        in += decl + ")" + renderArgs(to) + thisText(to);
        if (to->noexcept_) in += " noexcept";
        if (!to->ret) return in;
        return render(to->ret, in, true);
    }
    std::string in = t->cls.empty() ? t->sym : t->cls + "::" + t->sym;
    for (auto &m : t->mods) {
        std::string k = mod(m);
        if (!k.empty()) in += " " + k;
    }
    if (!top && t->ptrCv) in += " " + cvText(t->ptrCv);
    if (t->unaligned) {
        std::string u = kw("__unaligned");
        if (!u.empty()) in += " " + u;
    }
    if (!decl.empty()) in += (decl[0] == ' ' ? "" : " ") + decl;
    if (to && to->k == K::Array) in = "(" + in + ")";
    return render(to, in);
}

// ======================================================================== symbols

std::string Undecorator::encoding(Name &n)
{
    std::string name = nameOnly ? n.str() : n.str();
    char c = peek();
    if (c >= '0' && c <= '4') {
        i++;
        std::string pre;
        if (c <= '2' && !has(UND_NO_ACCESS_SPECIFIERS))
            pre = c == '0' ? "private: " : c == '1' ? "protected: " : "public: ";
        if (c <= '2' && !has(UND_NO_MEMBER_TYPE)) pre += "static ";
        Node *t = type(false);
        if (err) return "";
        // Storage class: modifiers and cv of the variable itself.
        if (t->k == K::Array) return err = true, "";
        std::vector<std::string> mods;
        bool stUnaligned = false;
        for (;;) {
            char m = peek();
            if (m == 'E') mods.push_back("__ptr64");
            else if (m == 'F') stUnaligned = true;
            else if (m == 'I') mods.push_back("__restrict");
            else break;
            i++;
        }
        bool member, ok;
        unsigned cv = cvLetter(get(), member, ok);
        if (!ok) return err = true, "";
        if (member) qualifiedName(false);
        if (has(UND_NAME_ONLY)) return name;
        std::string st = cvText(cv);
        if (stUnaligned) {
            std::string u = kw("__unaligned");
            if (!u.empty()) st += (st.empty() ? "" : " ") + u;
        }
        for (auto &m : mods) {
            std::string k = mod(m);
            if (!k.empty()) st += (st.empty() ? "" : " ") + k;
        }
        std::string decl = " " + (st.empty() ? "" : st + " ") + name;
        if (t->k == K::Ptr) return pre + render(t, decl, true);
        return pre + render(t, decl, true);
    }
    if (c == '6' || c == '7') {
        i++;
        bool member, ok;
        unsigned cv = cvLetter(get(), member, ok);
        if (!ok) return err = true, "";
        std::vector<std::string> fors;
        while (!err && !eat('@')) {
            if (i >= s.size()) return err = true, "";
            fors.push_back(qualifiedName(false).str());
        }
        if (has(UND_NAME_ONLY)) return name;
        std::string r = cvText(cv);
        if (!r.empty()) r += " ";
        r += name;
        if (!fors.empty()) {
            r += "{for ";
            for (size_t k = 0; k < fors.size(); k++) r += (k ? "s `" : "`") + fors[k] + "'";
            r += "}";
        }
        return r;
    }
    if (c == '8' || c == '9') {
        i++;
        return name;
    }
    if (c == '5') {
        i++;
        std::string num = unsignedNumber();
        if (has(UND_NAME_ONLY)) return name;
        return name + "{" + num + "}'";
    }
    // Functions.
    std::string pre, thunk, after;
    bool member = false, isStatic = false, isVirtual = false, isThunk = false;
    std::string externC;
    if (c == '$' && peek(1) == '$' && peek(2) == 'J' && peek(3) >= '0' && peek(3) <= '9') {
        i += 4;
        externC = "extern \"C\" ";
        c = peek();
    }
    if (c == '$') {
        i++;
        char d = get();
        if (d == 'B') {
            // vcall thunk: offset, call model, calling convention.
            std::string off = unsignedNumber();
            if (!eat('A')) return err = true, "";
            std::string cc;
            if (!callingConvention(cc)) return "";
            if (has(UND_NAME_ONLY)) return name + "{" + off + "}'";
            std::string k = has(UND_NO_ALLOCATION_LANGUAGE) ? "" : kw(cc.c_str());
            return "[thunk]:" + (k.empty() ? "" : " " + k + " ") + name + "{" + off + ",{flat}}' }'";
        }
        bool ex = false;
        if (d == 'R') {
            ex = true;
            d = get();
        }
        if (d < '0' || d > '5') return err = true, "";
        int acc = (d - '0') / 2;
        pre = acc == 0 ? "private: " : acc == 1 ? "protected: " : "public: ";
        isVirtual = true;
        isThunk = true;
        member = true;
        std::string a = unsignedNumber(), b = unsignedNumber();
        if (ex) {
            std::string c2 = unsignedNumber(), d2 = unsignedNumber();
            thunk = "`vtordispex{" + a + "," + b + "," + c2 + "," + d2 + "}' ";
        } else {
            thunk = "`vtordisp{" + a + "," + b + "}' ";
        }
    } else if (c >= 'A' && c <= 'Z') {
        i++;
        if (c <= 'X') {
            int grp = (c - 'A') / 8, kind = ((c - 'A') % 8) / 2;
            pre = grp == 0 ? "private: " : grp == 1 ? "protected: " : "public: ";
            member = true;
            if (kind == 1) isStatic = true, member = false;
            if (kind == 2) isVirtual = true;
            if (kind == 3) {
                isVirtual = true;
                isThunk = true;
                thunk = "`adjustor{" + signedNumber() + "}' ";
            }
        }
    } else {
        return err = true, "";
    }
    Node fn;
    fn.k = K::Func;
    if (member) thisQualifiers(&fn);
    if (!callingConvention(fn.cc)) return "";
    if (eat('@')) {
        fn.ret = nullptr;
    } else {
        fn.ret = type(false);
    }
    argList(&fn);
    if (err) return "";
    if (eat("_E")) fn.noexcept_ = true;
    else if (!eat('Z')) return err = true, "";
    if (n.special == 3) {
        // A conversion operator is named after its type.
        std::string conv = fn.ret ? render(fn.ret, "", true) : "";
        name = n.str() + n.tmplArgs + " " + conv;
        fn.ret = nullptr;
    }
    if (has(UND_NAME_ONLY)) return name;
    std::string decl;
    if (!has(UND_NO_ALLOCATION_LANGUAGE)) decl = kw(fn.cc.c_str());
    decl = join(decl, name + thunk + renderArgs(&fn) + thisText(&fn));
    if (fn.noexcept_) decl += " noexcept";
    std::string r = decl;
    if (fn.ret && !has(UND_NO_FUNCTION_RETURNS)) r = render(fn.ret, decl, true);
    std::string head = externC;
    if (isThunk) head += "[thunk]:";
    if (!has(UND_NO_ACCESS_SPECIFIERS)) head += pre;
    if (!has(UND_NO_MEMBER_TYPE)) {
        if (isStatic) head += "static ";
        if (isVirtual) head += "virtual ";
    }
    return head + r;
}

std::string Undecorator::symbol()
{
    if (!eat('?')) return err = true, "";
    if (peek() == '?' && peek(1) == '_' && peek(2) == 'C' && peek(3) == '@') {
        // A string literal: ??_C@_<char size><length>@<hash>@<chars>@
        i += 4;
        if (has(UND_NAME_ONLY) && outermost) {
            i = s.size();
            return "`string'";
        }
        if (!eat('_')) return err = true, "";
        if (peek() != '0' && peek() != '1') return err = true, "";
        i++;
        uint64_t len;
        bool neg;
        if (!number(len, neg)) return "";
        if (identifier().empty() && err) return "";
        size_t at = s.find('@', i);
        if (at == std::string::npos) return err = true, "";
        i = at + 1;
        return "`string'";
    }
    Name n = qualifiedName(true);
    if (err) return "";
    if (has(UND_NAME_ONLY) && n.special != 3 && outermost && n.parts[0] != "`vcall'") {
        // The name is all; whatever follows is not decoded.
        i = s.size();
        return n.str();
    }
    if (i >= s.size()) {
        // A special name alone, such as an RTTI type descriptor; an
        // ordinary name without a type is printed after " ?? ".
        if (n.special || n.isOperator) return n.str();
        return " ?? " + n.str();
    }
    return encoding(n);
}

std::string Undecorator::run()
{
    if (s.size() < 2 || s[0] != '?') return s;
    if (s.compare(0, 3, "??@") == 0) return s;
    if (has(UND_TYPE_ONLY)) {
        // The name is decoded as a type: ?<cv><type>, as in RTTI names.
        i = 1;
        bool member, ok;
        unsigned cv = cvLetter(get(), member, ok);
        if (!ok || member) return s;
        Node *t = type(false);
        if (err || i != s.size()) return s;
        if (t->k == K::Ptr) t->ptrCv = cv;
        else t->cv |= cv;
        return render(t, "");
    }
    std::string r = symbol();
    if (err || i != s.size()) return s;
    return r;
}

}  // namespace

std::string undname(const std::string &decorated, unsigned flags)
{
    Undecorator u(decorated, flags);
    return u.run();
}

std::string diaUndecorate(const std::string &decorated, unsigned flags)
{
    std::string n = decorated;
    if (!n.empty() && n[0] == '.') n = n.substr(1);
    return undname(n, flags);
}

}  // namespace om
