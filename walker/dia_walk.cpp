// dia_walk: load a PDB through a given msdia140.dll (by path, without COM
// registration) and write everything observable through the DIA interfaces
// as deterministic JSON. Used as the behaviour oracle (real DLL) and as the
// test driver (OpenMSDIA).
//
// Usage: dia_walk <msdia140.dll> <file.pdb> [--map FROM=TO]... > out.json
//
// Symbol identity: symIndexIds differ between implementations, so symbols
// are keyed by a lexical path: parent path / Tag:name, with #n appended to
// tell apart siblings with the same tag and name (ordered by address,
// offset and type). Property getters come from the generated table
// (tools/gen_dia.py, from the type library) and are called by vtable slot.
#include <windows.h>
#include <oleauto.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdint.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dia2_gen.h"
#include "dia2_meta_gen.h"

typedef HRESULT(WINAPI *PFN_DllGetClassObject)(REFCLSID, REFIID, void **);

static std::vector<std::pair<std::string, std::string>> g_maps;

// ---------------------------------------------------------------- JSON
static std::string jstr(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

static std::string mapped(std::string s)
{
    for (auto &m : g_maps) {
        size_t p;
        while ((p = s.find(m.first)) != std::string::npos)
            s.replace(p, m.first.size(), m.second);
    }
    return s;
}

static std::string utf8(const wchar_t *w)
{
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    return mapped(s);
}

static std::string bstr_take(BSTR b)
{
    std::string s = utf8(b);
    if (b) SysFreeString(b);
    return s;
}

static std::string num(unsigned long long v) { return std::to_string(v); }
static std::string snum(long long v) { return std::to_string(v); }

static std::string hrstr(HRESULT hr)
{
    switch (hr) {
    case S_OK: return "S_OK";
    case S_FALSE: return "S_FALSE";
    case E_NOTIMPL: return "E_NOTIMPL";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_FAIL: return "E_FAIL";
    case E_POINTER: return "E_POINTER";
    case E_NOINTERFACE: return "E_NOINTERFACE";
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    case E_UNEXPECTED: return "E_UNEXPECTED";
    }
    char b[16];
    snprintf(b, sizeof b, "0x%08lX", (unsigned long)hr);
    return b;
}

// An object of sorted key -> raw JSON text.
typedef std::map<std::string, std::string> JObj;

static std::string jobj_inline(const JObj &o)
{
    std::string s = "{";
    bool first = true;
    for (auto &kv : o) {
        if (!first) s += ", ";
        first = false;
        s += jstr(kv.first) + ": " + kv.second;
    }
    return s + "}";
}

static std::string jarr_inline(const std::vector<std::string> &v)
{
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + v[i];
    return s + "]";
}

static std::string guidstr(const GUID &g)
{
    char b[64];
    snprintf(b, sizeof b, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
             g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

static std::string variantstr(VARIANT &v)
{
    char b[64];
    std::string val;
    switch (V_VT(&v)) {
    case VT_EMPTY: val = "null"; break;
    case VT_I1: val = snum(V_I1(&v)); break;
    case VT_I2: val = snum(V_I2(&v)); break;
    case VT_I4: val = snum(V_I4(&v)); break;
    case VT_I8: val = snum(V_I8(&v)); break;
    case VT_INT: val = snum(V_INT(&v)); break;
    case VT_UI1: val = num(V_UI1(&v)); break;
    case VT_UI2: val = num(V_UI2(&v)); break;
    case VT_UI4: val = num(V_UI4(&v)); break;
    case VT_UI8: val = num(V_UI8(&v)); break;
    case VT_UINT: val = num(V_UINT(&v)); break;
    case VT_BOOL: val = V_BOOL(&v) ? "true" : "false"; break;
    case VT_R4: snprintf(b, sizeof b, "%.9g", (double)V_R4(&v)); val = jstr(b); break;
    case VT_R8: snprintf(b, sizeof b, "%.17g", V_R8(&v)); val = jstr(b); break;
    case VT_BSTR: val = jstr(utf8(V_BSTR(&v))); break;
    default: val = jstr("?"); break;
    }
    std::string s = "{\"vt\": " + num(V_VT(&v)) + ", \"v\": " + val + "}";
    VariantClear(&v);
    return s;
}

// ---------------------------------------------------------------- helpers
static const char *tagname(DWORD tag)
{
    static const char *names[] = {
        "Null", "Exe", "Compiland", "CompilandDetails", "CompilandEnv", "Function", "Block",
        "Data", "Annotation", "Label", "PublicSymbol", "UDT", "Enum", "FunctionType",
        "PointerType", "ArrayType", "BaseType", "Typedef", "BaseClass", "Friend",
        "FunctionArgType", "FuncDebugStart", "FuncDebugEnd", "UsingNamespace", "VTableShape",
        "VTable", "Custom", "Thunk", "CustomType", "ManagedType", "Dimension", "CallSite",
        "InlineSite", "BaseInterface", "VectorType", "MatrixType", "HLSLType", "Caller",
        "Callee", "Export", "HeapAllocationSite", "CoffGroup", "Inlinee", "TaggedUnionCase"};
    static char buf[32];
    if (tag < sizeof names / sizeof names[0]) return names[tag];
    snprintf(buf, sizeof buf, "Tag%lu", (unsigned long)tag);
    return buf;
}

template <class T> struct Ptr {
    T *p = nullptr;
    Ptr() {}
    ~Ptr() { if (p) p->Release(); }
    Ptr(const Ptr &) = delete;
    Ptr &operator=(const Ptr &) = delete;
    T **operator&() { if (p) { p->Release(); p = nullptr; } return &p; }
    T *operator->() const { return p; }
    operator T *() const { return p; }
};

static std::vector<IDiaSymbol *> all_symbols(IDiaEnumSymbols *e)
{
    std::vector<IDiaSymbol *> v;
    if (!e) return v;
    for (;;) {
        IDiaSymbol *s = nullptr;
        ULONG got = 0;
        if (e->Next(1, &s, &got) != S_OK || got != 1 || !s) break;
        v.push_back(s);
        if (v.size() > 1000000) break;
    }
    return v;
}

static DWORD sym_tag(IDiaSymbol *s)
{
    DWORD t = 0;
    if (s->get_symTag(&t) != S_OK) return 0;
    return t;
}

static std::string sym_name(IDiaSymbol *s)
{
    BSTR b = nullptr;
    if (s->get_name(&b) != S_OK) { if (b) SysFreeString(b); return ""; }
    return bstr_take(b);
}

// A short description of a referenced symbol that does not depend on
// symIndexIds, e.g. "PointerType(BaseType(int:4))".
static std::string describe(IDiaSymbol *s, int depth = 0)
{
    if (!s) return "null";
    if (depth > 5) return "...";
    DWORD tag = sym_tag(s);
    std::string d = tagname(tag);
    std::string n = sym_name(s);
    ULONGLONG len = 0;
    DWORD dw = 0;
    LONG f = 0;
    auto sub = [&](const char *what) {
        Ptr<IDiaSymbol> t;
        if (s->get_type(&t) == S_OK && t) return describe(t, depth + 1);
        (void)what;
        return std::string("?");
    };
    switch (tag) {
    case 16: { // BaseType
        if (s->get_baseType(&dw) != S_OK) dw = 0;
        if (s->get_length(&len) != S_OK) len = 0;
        d += "(" + num(dw) + ":" + num(len);
        if (s->get_constType(&f) == S_OK && f) d += " const";
        if (s->get_volatileType(&f) == S_OK && f) d += " volatile";
        d += ")";
        break;
    }
    case 14: // PointerType
        d += "(";
        if (s->get_reference(&f) == S_OK && f) d += "& ";
        if (s->get_constType(&f) == S_OK && f) d += "const ";
        if (s->get_volatileType(&f) == S_OK && f) d += "volatile ";
        d += sub("type") + ")";
        break;
    case 15: // ArrayType
        if (s->get_count(&dw) != S_OK) dw = 0;
        d += "[" + num(dw) + "](" + sub("type") + ")";
        break;
    case 13: // FunctionType
        if (s->get_count(&dw) != S_OK) dw = 0;
        d += "(" + num(dw) + " args -> " + sub("type") + ")";
        break;
    case 20: // FunctionArgType
        d += "(" + sub("type") + ")";
        break;
    case 5: case 10: case 7: { // Function, PublicSymbol, Data
        d += ":" + n;
        DWORD rva = 0;
        if (s->get_relativeVirtualAddress(&rva) == S_OK) d += "@" + num(rva);
        LONG off = 0;
        if (tag == 7 && s->get_offset(&off) == S_OK) d += "+" + snum(off);
        break;
    }
    default:
        if (!n.empty()) d += ":" + n;
        if (tag == 11 || tag == 12 || tag == 24) {
            if (s->get_constType(&f) == S_OK && f) d += " const";
            if (s->get_volatileType(&f) == S_OK && f) d += " volatile";
        }
        break;
    }
    return d;
}

// ---------------------------------------------------------------- getters
// Call every single-value property getter of `iface` (and of the interfaces
// it derives from) on `obj`, by vtable slot. Successful values go to `ok`;
// failures are grouped by HRESULT.
static void collect_props(IUnknown *obj, const char *iface, JObj &out)
{
    std::map<std::string, std::vector<std::string>> failed;
    // Interfaces in the chain of `iface`, each QI'd once.
    std::map<std::string, IUnknown *> qi;
    std::vector<std::string> chain;
    for (const char *n = iface; n && *n;) {
        chain.push_back(n);
        const char *next = nullptr;
        for (auto &i : dia_ifaces)
            if (!strcmp(i.name, n)) next = i.base;
        n = (next && strcmp(next, "IUnknown") && strcmp(next, "IEnumUnknown")) ? next : nullptr;
    }
    for (auto &c : chain) {
        IUnknown *p = nullptr;
        for (auto &i : dia_ifaces)
            if (c == i.name) {
                if (obj->QueryInterface(i.iid, (void **)&p) != S_OK) p = nullptr;
            }
        qi[c] = p;
        if (!p && c != chain.back()) failed["E_NOINTERFACE"].push_back("<" + c + ">");
    }
    for (auto &g : dia_getters) {
        if (!qi.count(g.iface)) continue;
        IUnknown *p = qi[g.iface];
        std::string name = g.name;
        if (!p) { failed["E_NOINTERFACE"].push_back(name); continue; }
        void **vtbl = *(void ***)p;
        void *fn = vtbl[g.slot];
        std::string kind = g.kind;
        HRESULT hr;
        std::string val;
        if (kind == "LONG" || kind == "ULONG") {
            ULONG v = 0;
            hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, ULONG *))fn)(p, &v);
            val = kind == "LONG" ? snum((LONG)v) : num(v);
        } else if (kind == "USHORT" || kind == "BYTE") {
            ULONG v = 0;
            hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, ULONG *))fn)(p, &v);
            val = num(kind == "BYTE" ? (v & 0xff) : (v & 0xffff));
        } else if (kind == "ULONGLONG") {
            ULONGLONG v = 0;
            hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, ULONGLONG *))fn)(p, &v);
            val = num(v);
        } else if (kind == "BSTR") {
            BSTR v = nullptr;
            hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, BSTR *))fn)(p, &v);
            std::string s = bstr_take(v);
            val = jstr(s);
        } else if (kind == "GUID") {
            GUID v = {};
            hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, GUID *))fn)(p, &v);
            val = jstr(guidstr(v));
        } else if (kind == "VARIANT") {
            VARIANT v;
            VariantInit(&v);
            hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, VARIANT *))fn)(p, &v);
            val = variantstr(v);
        } else if (kind == "IFACE:IDiaSymbol") {
            IDiaSymbol *v = nullptr;
            hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, IDiaSymbol **))fn)(p, &v);
            val = jstr(describe(v));
            if (v) v->Release();
        } else if (kind == "IFACE:IDiaSourceFile") {
            IDiaSourceFile *v = nullptr;
            hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, IDiaSourceFile **))fn)(p, &v);
            std::string s = "null";
            if (v) {
                BSTR b = nullptr;
                if (v->get_fileName(&b) == S_OK) s = bstr_take(b);
                v->Release();
            }
            val = jstr(s);
        } else if (kind.rfind("IFACE:", 0) == 0 || kind == "UNKNOWN") {
            IUnknown *v = nullptr;
            hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, IUnknown **))fn)(p, &v);
            val = jstr(v ? "<object>" : "null");
            if (v) v->Release();
        } else {
            continue;
        }
        if (hr == S_OK)
            out[name] = val;
        else
            failed[hrstr(hr)].push_back(name);
    }
    for (auto &kv : qi)
        if (kv.second) kv.second->Release();
    for (auto &kv : failed) {
        // S_FALSE ("no such property for this symbol") is the common case;
        // getters that are absent from the output returned S_FALSE.
        if (kv.first == "S_FALSE") continue;
        std::sort(kv.second.begin(), kv.second.end());
        std::vector<std::string> q;
        for (auto &n : kv.second) q.push_back(jstr(n));
        out["!" + kv.first] = jarr_inline(q);
    }
}

// ---------------------------------------------------------------- walk
struct Walker {
    IDiaSession *session = nullptr;
    std::map<std::string, JObj> symbols;              // path -> props
    std::map<DWORD, std::string> seen;                // symIndexId -> path
    std::set<std::pair<DWORD, DWORD>> addrs;          // (rva, length) samples
    std::vector<std::pair<std::string, IDiaSymbol *>> compilands;
    size_t limit = 200000;

    std::string sortkey(IDiaSymbol *s)
    {
        DWORD rva = 0;
        LONG off = 0;
        ULONGLONG len = 0;
        char b[64];
        if (s->get_relativeVirtualAddress(&rva) != S_OK) rva = 0;
        if (s->get_offset(&off) != S_OK) off = 0;
        if (s->get_length(&len) != S_OK) len = 0;
        snprintf(b, sizeof b, "%08lx|%08lx|%016llx|", (unsigned long)rva, (unsigned long)off,
                 (unsigned long long)len);
        Ptr<IDiaSymbol> t, cp;
        std::string ty = (s->get_type(&t) == S_OK && t) ? describe(t) : "";
        std::string cls = (s->get_classParent(&cp) == S_OK && cp) ? describe(cp) : "";
        return b + ty + "|" + cls;
    }

    void walk(IDiaSymbol *s, const std::string &path, int depth)
    {
        DWORD id = 0;
        s->get_symIndexId(&id);
        JObj props;
        collect_props(s, "IDiaSymbol11", props);
        DWORD tag = sym_tag(s);
        if (tag == 2) {
            s->AddRef();
            compilands.push_back({path, s});
        }
        DWORD rva = 0;
        ULONGLONG len = 0;
        if (s->get_relativeVirtualAddress(&rva) == S_OK && rva) {
            if (s->get_length(&len) != S_OK) len = 0;
            addrs.insert({rva, (DWORD)len});
        }
        // Function-like extras.
        {
            BSTR b = nullptr;
            HRESULT hr = s->get_undecoratedNameEx(0, &b);
            props["undecoratedNameEx(0)"] = hr == S_OK ? jstr(bstr_take(b)) : jstr("!" + hrstr(hr));
            if (hr != S_OK && b) SysFreeString(b);
            if (hr == S_OK) {
                // Two more option sets: UNDNAME_NAME_ONLY and the set
                // Breakpad's dump_syms uses.
                for (ULONG o : {0x1000ul, 0x87FEul}) {
                    b = nullptr;
                    HRESULT h2 = s->get_undecoratedNameEx(o, &b);
                    char k[40];
                    snprintf(k, sizeof k, "undecoratedNameEx(0x%lX)", o);
                    props[k] = h2 == S_OK ? jstr(bstr_take(b)) : jstr("!" + hrstr(h2));
                    if (h2 != S_OK && b) SysFreeString(b);
                }
            }
            ULONG n = 0;
            hr = s->get_types(0, &n, nullptr);
            if (hr == S_OK && n > 0 && n < 4096) {
                std::vector<IDiaSymbol *> v(n);
                ULONG got = 0;
                if (s->get_types(n, &got, v.data()) == S_OK) {
                    std::vector<std::string> d;
                    for (ULONG i = 0; i < got; i++) { d.push_back(jstr(describe(v[i]))); if (v[i]) v[i]->Release(); }
                    props["types()"] = jarr_inline(d);
                }
            } else {
                props["types()"] = jstr("!" + hrstr(hr) + (hr == S_OK ? " n=" + num(n) : ""));
            }
        }
        seen[id] = path;
        if (depth > 12 || symbols.size() > limit) { symbols[path] = props; return; }

        Ptr<IDiaEnumSymbols> en;
        HRESULT hr = s->findChildren((enum SymTagEnum)0, nullptr, 0, &en);
        if (hr != S_OK || !en) {
            props["findChildren()"] = jstr("!" + hrstr(hr));
            symbols[path] = props;
            return;
        }
        LONG count = -1;
        HRESULT hc = en->get_count(&count);
        std::vector<IDiaSymbol *> kids = all_symbols(en);
        props["findChildren().count"] = hc == S_OK ? snum(count) : jstr("!" + hrstr(hc));
        props["findChildren().fetched"] = num(kids.size());
        // Sort children by tag, name, then address/offset/type for stable ordinals.
        struct K { std::string tag, name, key; IDiaSymbol *s; };
        std::vector<K> ks;
        for (auto *k : kids) ks.push_back({tagname(sym_tag(k)), sym_name(k), sortkey(k), k});
        std::stable_sort(ks.begin(), ks.end(), [](const K &a, const K &b) {
            if (a.tag != b.tag) return a.tag < b.tag;
            if (a.name != b.name) return a.name < b.name;
            return a.key < b.key;
        });
        std::map<std::string, int> ord;
        std::map<std::string, int> total;
        for (auto &k : ks) total[k.tag + ":" + k.name]++;
        std::map<std::string, int> tagcount;
        for (auto &k : ks) {
            std::string base = k.tag + ":" + k.name;
            std::string child = path + "/" + base;
            if (total[base] > 1) child += "#" + num(ord[base]++);
            tagcount[k.tag]++;
            DWORD cid = 0;
            k.s->get_symIndexId(&cid);
            if (seen.count(cid)) {
                symbols[child]["@seen"] = jstr(seen[cid]);
            } else {
                walk(k.s, child, depth + 1);
            }
            k.s->Release();
        }
        JObj tc;
        for (auto &kv : tagcount) tc[kv.first] = num(kv.second);
        props["children"] = jobj_inline(tc);
        symbols[path] = props;
    }
};

static std::string line_entry(IDiaLineNumber *ln)
{
    JObj o;
    collect_props(ln, "IDiaLineNumber", o);
    return jobj_inline(o);
}

static std::vector<std::string> lines_of(IDiaEnumLineNumbers *e)
{
    std::vector<std::string> v;
    if (!e) return v;
    for (;;) {
        IDiaLineNumber *ln = nullptr;
        ULONG got = 0;
        if (e->Next(1, &ln, &got) != S_OK || got != 1 || !ln) break;
        v.push_back(line_entry(ln));
        ln->Release();
    }
    return v;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: dia_walk <msdia140.dll> <file.pdb> [--map FROM=TO]...\n");
        return 2;
    }
    for (int i = 3; i + 1 < argc; i++) {
        if (wcscmp(argv[i], L"--map")) continue;
        std::string m = utf8(argv[++i]);
        size_t eq = m.find('=');
        if (eq == std::string::npos) continue;
        std::string from = m.substr(0, eq), to = m.substr(eq + 1);
        std::string alt = from;
        std::replace(alt.begin(), alt.end(), '/', '\\');
        g_maps.push_back({from, to});
        if (alt != from) g_maps.push_back({alt, to});
        alt = from;
        std::replace(alt.begin(), alt.end(), '\\', '/');
        if (alt != from) g_maps.push_back({alt, to});
    }
    std::sort(g_maps.begin(), g_maps.end(), [](auto &a, auto &b) { return a.first.size() > b.first.size(); });

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    auto gco = (PFN_DllGetClassObject)(void *)GetProcAddress(dll, "DllGetClassObject");
    if (!gco) { fprintf(stderr, "no DllGetClassObject\n"); return 1; }
    Ptr<IClassFactory> cf;
    HRESULT hr = gco(CLSID_DiaSource, IID_IClassFactory, (void **)&cf);
    if (hr != S_OK) { fprintf(stderr, "DllGetClassObject: %s\n", hrstr(hr).c_str()); return 1; }
    Ptr<IDiaDataSource> src;
    hr = cf->CreateInstance(nullptr, IID_IDiaDataSource, (void **)&src);
    if (hr != S_OK) { fprintf(stderr, "CreateInstance: %s\n", hrstr(hr).c_str()); return 1; }

    std::map<std::string, std::string> top;
    JObj load;
    hr = src->loadDataFromPdb(argv[2]);
    load["loadDataFromPdb"] = jstr(hrstr(hr));
    {
        BSTR b = nullptr;
        HRESULT h2 = src->get_lastError(&b);
        load["lastError"] = h2 == S_OK ? jstr(bstr_take(b)) : jstr("!" + hrstr(h2));
    }
    Ptr<IDiaSession> session;
    HRESULT hs = src->openSession(&session);
    load["openSession"] = jstr(hrstr(hs));
    {
        // A second load on a used data source.
        Ptr<IDiaDataSource> src2;
        cf->CreateInstance(nullptr, IID_IDiaDataSource, (void **)&src2);
        load["loadDataFromPdb(missing)"] = jstr(hrstr(src2->loadDataFromPdb((wchar_t *)L"Z:\\does\\not\\exist.pdb")));
        Ptr<IDiaSession> s2;
        load["openSession(unloaded)"] = jstr(hrstr(src2->openSession(&s2)));
    }
    if (hs != S_OK) {
        printf("{\n \"load\": %s\n}\n", jobj_inline(load).c_str());
        return 0;
    }
    {
        // Validation against the PDB's own GUID, signature and age, and
        // against wrong ones; loading from an IStream; a file that is not
        // a PDB.
        Ptr<IDiaSymbol> g;
        GUID guid = {};
        ULONG sig = 0, age = 0;
        if (session->get_globalScope(&g) == S_OK && g) {
            g->get_guid(&guid);
            g->get_signature(&sig);
            g->get_age(&age);
        }
        auto validate = [&](GUID *gp, ULONG s, ULONG a) {
            Ptr<IDiaDataSource> v;
            cf->CreateInstance(nullptr, IID_IDiaDataSource, (void **)&v);
            return jstr(hrstr(v->loadAndValidateDataFromPdb(argv[2], gp, s, a)));
        };
        load["loadAndValidateDataFromPdb(own)"] = validate(&guid, sig, age);
        load["loadAndValidateDataFromPdb(age+1)"] = validate(&guid, sig, age + 1);
        GUID other = guid;
        other.Data1 ^= 1;
        load["loadAndValidateDataFromPdb(other guid)"] = validate(&other, sig, age);
        load["loadAndValidateDataFromPdb(no guid, sig)"] = validate(nullptr, sig, age);
        {
            Ptr<IDiaDataSource> v;
            cf->CreateInstance(nullptr, IID_IDiaDataSource, (void **)&v);
            Ptr<IStream> st;
            HRESULT h = SHCreateStreamOnFileEx(argv[2], STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &st);
            if (h == S_OK) {
                h = v->loadDataFromIStream(st);
                load["loadDataFromIStream"] = jstr(hrstr(h));
                Ptr<IDiaSession> s2;
                Ptr<IDiaSymbol> g2;
                if (h == S_OK && v->openSession(&s2) == S_OK && s2->get_globalScope(&g2) == S_OK && g2) {
                    LONG cnt = -1;
                    Ptr<IDiaEnumSymbols> e2;
                    if (g2->findChildren((enum SymTagEnum)0, nullptr, 0, &e2) == S_OK) e2->get_count(&cnt);
                    load["loadDataFromIStream.globalScope.children"] = snum(cnt);
                }
            }
        }
        {
            Ptr<IDiaDataSource> v;
            cf->CreateInstance(nullptr, IID_IDiaDataSource, (void **)&v);
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            load["loadDataFromPdb(not a pdb)"] = jstr(hrstr(v->loadDataFromPdb(self)));
            load["loadDataFromPdb(twice)"] = jstr(hrstr(src->loadDataFromPdb(argv[2])));
        }
    }
    ULONGLONG la = 0;
    HRESULT hl = session->get_loadAddress(&la);
    load["session.loadAddress"] = hl == S_OK ? num(la) : jstr("!" + hrstr(hl));

    Walker w;
    w.session = session;
    Ptr<IDiaSymbol> global;
    hr = session->get_globalScope(&global);
    load["session.globalScope"] = jstr(hrstr(hr));
    if (global) w.walk(global, "Exe", 0);

    // Lines per compiland and source file.
    std::map<std::string, std::string> lines, files;
    for (auto &c : w.compilands) {
        Ptr<IDiaEnumSourceFiles> ef;
        HRESULT h = session->findFile(c.second, nullptr, 0, &ef);
        if (h != S_OK || !ef) { lines[c.first] = jstr("!findFile " + hrstr(h)); c.second->Release(); continue; }
        for (;;) {
            IDiaSourceFile *f = nullptr;
            ULONG got = 0;
            if (ef->Next(1, &f, &got) != S_OK || got != 1 || !f) break;
            BSTR b = nullptr;
            std::string fname = f->get_fileName(&b) == S_OK ? bstr_take(b) : "?";
            Ptr<IDiaEnumLineNumbers> el;
            h = session->findLines(c.second, f, &el);
            std::string key = c.first + "|" + fname;
            lines[key] = h == S_OK ? jarr_inline(lines_of(el)) : jstr("!" + hrstr(h));
            f->Release();
        }
        c.second->Release();
    }
    // All source files.
    {
        Ptr<IDiaEnumSourceFiles> ef;
        HRESULT h = session->findFile(nullptr, nullptr, 0, &ef);
        if (h == S_OK && ef) {
            for (;;) {
                IDiaSourceFile *f = nullptr;
                ULONG got = 0;
                if (ef->Next(1, &f, &got) != S_OK || got != 1 || !f) break;
                JObj o;
                collect_props(f, "IDiaSourceFile", o);
                ULONG cb = 0;
                HRESULT hc = f->get_checksum(0, &cb, nullptr);
                if (hc == S_OK && cb && cb <= 64) {
                    BYTE buf[64];
                    if (f->get_checksum(cb, &cb, buf) == S_OK) {
                        std::string hex;
                        char t[4];
                        for (ULONG i = 0; i < cb; i++) { snprintf(t, sizeof t, "%02x", buf[i]); hex += t; }
                        o["checksum()"] = jstr(hex);
                    }
                } else {
                    o["checksum()"] = jstr("!" + hrstr(hc) + " cb=" + num(cb));
                }
                Ptr<IDiaEnumSymbols> ec;
                std::vector<std::string> cn;
                if (f->get_compilands(&ec) == S_OK)
                    for (auto *s : all_symbols(ec)) { cn.push_back(jstr(describe(s))); s->Release(); }
                std::sort(cn.begin(), cn.end());
                o["compilands()"] = jarr_inline(cn);
                BSTR fb = nullptr;
                std::string fname = f->get_fileName(&fb) == S_OK ? bstr_take(fb) : "?";
                files[fname] = jobj_inline(o);
                f->Release();
            }
        } else {
            files["!"] = jstr(hrstr(h));
        }
    }
    // Address lookups.
    std::map<std::string, std::string> byaddr;
    static const DWORD tags[] = {0, 5, 7, 10, 6};
    for (auto &a : w.addrs) {
        for (DWORD delta : {0u, 1u}) {
            DWORD rva = a.first + delta;
            JObj o;
            for (DWORD t : tags) {
                Ptr<IDiaSymbol> s;
                HRESULT h = session->findSymbolByRVA(rva, (enum SymTagEnum)t, &s);
                o[std::string("rva.") + tagname(t)] = h == S_OK ? jstr(describe(s)) : jstr("!" + hrstr(h));
            }
            {
                Ptr<IDiaSymbol> s;
                LONG disp = 0;
                HRESULT h = session->findSymbolByRVAEx(rva, (enum SymTagEnum)0, &s, &disp);
                o["rvaEx.Null"] = h == S_OK ? jstr(describe(s) + " disp=" + snum(disp)) : jstr("!" + hrstr(h));
            }
            {
                Ptr<IDiaSymbol> s;
                HRESULT h = session->findSymbolByVA(rva, (enum SymTagEnum)0, &s);
                o["va.Null"] = h == S_OK ? jstr(describe(s)) : jstr("!" + hrstr(h));
            }
            {
                DWORD sect = 0, off = 0;
                HRESULT h = session->addressForRVA(rva, &sect, &off);
                o["addressForRVA"] = h == S_OK ? jstr(num(sect) + ":" + num(off)) : jstr("!" + hrstr(h));
                if (h == S_OK) {
                    Ptr<IDiaSymbol> s;
                    h = session->findSymbolByAddr(sect, off, (enum SymTagEnum)0, &s);
                    o["addr.Null"] = h == S_OK ? jstr(describe(s)) : jstr("!" + hrstr(h));
                }
            }
            if (delta == 0) {
                Ptr<IDiaEnumLineNumbers> el;
                HRESULT h = session->findLinesByRVA(rva, a.second ? a.second : 1, &el);
                o["findLinesByRVA"] = h == S_OK ? jarr_inline(lines_of(el)) : jstr("!" + hrstr(h));
            }
            char k[32];
            snprintf(k, sizeof k, "%08lx", (unsigned long)rva);
            byaddr[k] = jobj_inline(o);
        }
    }
    // Symbols by address.
    std::vector<std::string> byaddr_order;
    {
        Ptr<IDiaEnumSymbolsByAddr> ea;
        HRESULT h = session->getSymbolsByAddr(&ea);
        if (h == S_OK && ea) {
            Ptr<IDiaSymbol> first;
            h = ea->symbolByAddr(1, 0, &first);
            if (h == S_OK && first) {
                byaddr_order.push_back(jstr(describe(first)));
                for (int i = 0; i < 100000; i++) {
                    IDiaSymbol *s = nullptr;
                    ULONG got = 0;
                    if (ea->Next(1, &s, &got) != S_OK || got != 1 || !s) break;
                    byaddr_order.push_back(jstr(describe(s)));
                    s->Release();
                }
            } else {
                byaddr_order.push_back(jstr("!symbolByAddr " + hrstr(h)));
            }
        } else {
            byaddr_order.push_back(jstr("!" + hrstr(h)));
        }
    }
    // Tables.
    std::map<std::string, std::string> tables;
    {
        Ptr<IDiaEnumTables> et;
        HRESULT h = session->getEnumTables(&et);
        if (h == S_OK && et) {
            for (;;) {
                IDiaTable *t = nullptr;
                ULONG got = 0;
                if (et->Next(1, &t, &got) != S_OK || got != 1 || !t) break;
                BSTR b = nullptr;
                std::string name = t->get_name(&b) == S_OK ? bstr_take(b) : "?";
                LONG cnt = -1;
                HRESULT hc = t->get_count(&cnt);
                std::vector<std::string> items;
                IDiaEnumSectionContribs *sc = nullptr;
                if (t->QueryInterface(IID_IDiaEnumSectionContribs, (void **)&sc) == S_OK) {
                    for (;;) {
                        IDiaSectionContrib *c = nullptr;
                        ULONG g2 = 0;
                        if (sc->Next(1, &c, &g2) != S_OK || g2 != 1 || !c) break;
                        JObj o;
                        collect_props(c, "IDiaSectionContrib", o);
                        items.push_back(jobj_inline(o));
                        c->Release();
                    }
                    sc->Release();
                }
                // Frame data rows: IDiaTable::Item, as IDiaFrameData.
                if (items.empty() && hc == S_OK && cnt > 0) {
                    for (LONG k = 0; k < cnt && k < 4096; k++) {
                        IUnknown *u = nullptr;
                        if (t->Item(k, &u) != S_OK || !u) break;
                        IDiaFrameData *fd = nullptr;
                        if (u->QueryInterface(IID_IDiaFrameData, (void **)&fd) == S_OK && fd) {
                            JObj o;
                            collect_props(fd, "IDiaFrameData", o);
                            items.push_back(jobj_inline(o));
                            fd->Release();
                        }
                        u->Release();
                    }
                }
                static const IID *known[] = {&IID_IDiaEnumSymbols, &IID_IDiaEnumSourceFiles,
                                             &IID_IDiaEnumLineNumbers, &IID_IDiaEnumSectionContribs,
                                             &IID_IDiaEnumInjectedSources, &IID_IDiaEnumInputAssemblyFiles};
                static const char *kn[] = {"IDiaEnumSymbols", "IDiaEnumSourceFiles", "IDiaEnumLineNumbers",
                                           "IDiaEnumSectionContribs", "IDiaEnumInjectedSources",
                                           "IDiaEnumInputAssemblyFiles"};
                std::vector<std::string> ifs;
                for (int i = 0; i < 6; i++) {
                    IUnknown *u = nullptr;
                    if (t->QueryInterface(*known[i], (void **)&u) == S_OK) { ifs.push_back(jstr(kn[i])); u->Release(); }
                }
                JObj o;
                o["count"] = hc == S_OK ? snum(cnt) : jstr("!" + hrstr(hc));
                o["interfaces"] = jarr_inline(ifs);
                if (!items.empty()) o["items"] = jarr_inline(items);
                tables[name] = jobj_inline(o);
                t->Release();
            }
        } else {
            tables["!"] = jstr(hrstr(h));
        }
    }
    // Debug streams.
    std::map<std::string, std::string> streams;
    {
        Ptr<IDiaEnumDebugStreams> es;
        HRESULT h = session->getEnumDebugStreams(&es);
        if (h == S_OK && es) {
            for (;;) {
                IDiaEnumDebugStreamData *d = nullptr;
                ULONG got = 0;
                if (es->Next(1, &d, &got) != S_OK || got != 1 || !d) break;
                BSTR b = nullptr;
                std::string name = d->get_name(&b) == S_OK ? bstr_take(b) : "?";
                LONG cnt = -1;
                HRESULT hc = d->get_count(&cnt);
                streams[name] = hc == S_OK ? snum(cnt) : jstr("!" + hrstr(hc));
                d->Release();
            }
        } else {
            streams["!"] = jstr(hrstr(h));
        }
    }

    // Output: one symbol per block, one property per line.
    printf("{\n \"conventions\": \"getters missing from an object returned S_FALSE; !HRESULT keys list getters that failed otherwise\",\n");
    printf(" \"load\": %s,\n", jobj_inline(load).c_str());
    printf(" \"symbols\": {\n");
    size_t i = 0;
    for (auto &kv : w.symbols) {
        printf("  %s: {\n", jstr(kv.first).c_str());
        size_t j = 0;
        for (auto &p : kv.second)
            printf("   %s: %s%s\n", jstr(p.first).c_str(), p.second.c_str(), ++j < kv.second.size() ? "," : "");
        printf("  }%s\n", ++i < w.symbols.size() ? "," : "");
    }
    printf(" },\n");
    auto section = [](const char *name, std::map<std::string, std::string> &m, bool last) {
        printf(" \"%s\": {\n", name);
        size_t k = 0;
        for (auto &kv : m) printf("  %s: %s%s\n", jstr(kv.first).c_str(), kv.second.c_str(), ++k < m.size() ? "," : "");
        printf(" }%s\n", last ? "" : ",");
    };
    section("lines", lines, false);
    section("files", files, false);
    section("byaddr", byaddr, false);
    printf(" \"symbolsByAddr\": [\n");
    for (size_t k = 0; k < byaddr_order.size(); k++)
        printf("  %s%s\n", byaddr_order[k].c_str(), k + 1 < byaddr_order.size() ? "," : "");
    printf(" ],\n");
    section("tables", tables, false);
    section("debugStreams", streams, true);
    printf("}\n");
    fflush(stdout);
    // Release everything before unloading.
    global.~Ptr();
    global.p = nullptr;
    session.~Ptr();
    session.p = nullptr;
    _exit(0);
}
