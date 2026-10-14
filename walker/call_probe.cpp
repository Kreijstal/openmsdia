// call_probe: call patterns of real DIA consumers that dia_walk does not
// cover (dia_walk sorts children and calls every getter once, in a fixed
// order). Learned from running Ghidra's PDB exporter and Dia2Dump against
// both DLLs (tools/consumers.sh).
//
//   call_probe <msdia140.dll> <file.pdb> > out.json
//
// Output: {"<key>": <value>}, keys sorted:
//   order|<path>              findChildren(SymTagNull) in enumeration order
//                             ("tag:name" items, at most 400)
//   tag<N>|<path>             findChildren(N): count and names in order
//   names|<udt path>|<how>    get_name called twice on each method reached
//                             through the class, the class itself reached
//                             through findChildren(SymTagNull|SymTagUDT) of
//                             the global scope and its children through
//                             findChildren(SymTagNull|SymTagFunction)
//   tables                    getEnumTables: names in order, and which
//                             enumerator interfaces each table answers
//   checksum|<file>           IDiaSourceFile checksum type and bytes
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>
#include <map>
#include <string>
#include <vector>

#include "dia2_gen.h"

typedef HRESULT(WINAPI *PFN_DllGetClassObject)(REFCLSID, REFIID, void **);

static std::map<std::string, std::string> out;

static std::string jstr(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

static std::string utf8(const wchar_t *w)
{
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    return s;
}

static std::string hex(unsigned long v)
{
    char b[16];
    snprintf(b, sizeof b, "0x%lX", v);
    return b;
}

static std::string name(IDiaSymbol *s)
{
    BSTR b = nullptr;
    HRESULT hr = s->get_name(&b);
    std::string r = hr == S_OK ? utf8(b) : "!" + hex((unsigned long)hr);
    if (hr == S_OK && b) SysFreeString(b);
    return r;
}

static DWORD tag(IDiaSymbol *s)
{
    DWORD t = 0;
    s->get_symTag(&t);
    return t;
}

static std::vector<IDiaSymbol *> children(IDiaSymbol *s, DWORD t, LONG *count = nullptr)
{
    std::vector<IDiaSymbol *> v;
    IDiaEnumSymbols *e = nullptr;
    if (count) *count = -1;
    if (s->findChildren((enum SymTagEnum)t, nullptr, 0, &e) != S_OK || !e) return v;
    if (count) e->get_count(count);
    for (;;) {
        IDiaSymbol *c = nullptr;
        ULONG got = 0;
        if (e->Next(1, &c, &got) != S_OK || got != 1 || !c) break;
        v.push_back(c);
    }
    e->Release();
    return v;
}

static void release(std::vector<IDiaSymbol *> &v)
{
    for (auto *s : v) s->Release();
    v.clear();
}

static std::string list(const std::vector<std::string> &v)
{
    std::string r = "[";
    for (size_t i = 0; i < v.size(); i++) r += (i ? ", " : "") + jstr(v[i]);
    return r + "]";
}

// Children in enumeration order, as "tag:name".
static void order(IDiaSymbol *s, const std::string &path)
{
    auto kids = children(s, 0);
    std::vector<std::string> v;
    for (auto *k : kids) {
        if (v.size() >= 400) break;
        v.push_back(std::to_string(tag(k)) + ":" + name(k));
    }
    out["order|" + path] = list(v);
    release(kids);
}

// findChildren with a tag filter: count and names in order.
static void byTag(IDiaSymbol *s, const std::string &path)
{
    for (DWORD t = 1; t <= 43; t++) {
        LONG count = -1;
        auto kids = children(s, t, &count);
        if (kids.empty() && count <= 0) continue;
        std::vector<std::string> v;
        for (auto *k : kids) {
            if (v.size() >= 200) break;
            v.push_back(name(k));
        }
        out["tag" + std::to_string(t) + "|" + path] =
            "{\"count\": " + std::to_string(count) + ", \"fetched\": " + std::to_string(kids.size()) +
            ", \"names\": " + list(v) + "}";
        release(kids);
    }
}

// get_name twice on the methods of each class, by how the class and its
// children were reached.
static void methodNames(IDiaSymbol *global)
{
    for (DWORD how : {0u, 11u}) {
        auto udts = children(global, how);
        int n = 0;
        for (auto *u : udts) {
            if (tag(u) != 11 || n >= 200) continue;
            n++;
            std::string un = name(u);
            for (DWORD kidTag : {0u, 5u}) {
                auto kids = children(u, kidTag);
                std::vector<std::string> v;
                for (auto *k : kids) {
                    if (tag(k) != 5) continue;
                    std::string a = name(k), b = name(k);
                    v.push_back(a + " | " + b);
                }
                char key[64];
                snprintf(key, sizeof key, "global:%lu,class:%lu", (unsigned long)how, (unsigned long)kidTag);
                std::string k = "names|" + un + "|" + key;
                if (out.count(k)) k += "#" + std::to_string(n);
                if (!v.empty()) out[k] = list(v);
                release(kids);
            }
        }
        release(udts);
    }
}

static const struct { const char *name; IID iid; } kEnums[] = {
    {"IDiaEnumSymbols", IID_IDiaEnumSymbols},
    {"IDiaEnumSourceFiles", IID_IDiaEnumSourceFiles},
    {"IDiaEnumLineNumbers", IID_IDiaEnumLineNumbers},
    {"IDiaEnumSectionContribs", IID_IDiaEnumSectionContribs},
    {"IDiaEnumInjectedSources", IID_IDiaEnumInjectedSources},
    {"IDiaEnumInputAssemblyFiles", IID_IDiaEnumInputAssemblyFiles},
    {"IEnumUnknown", IID_IEnumUnknown},
};

static void tables(IDiaSession *session)
{
    IDiaEnumTables *et = nullptr;
    if (session->getEnumTables(&et) != S_OK || !et) return;
    std::vector<std::string> v;
    for (;;) {
        IDiaTable *t = nullptr;
        ULONG got = 0;
        if (et->Next(1, &t, &got) != S_OK || got != 1 || !t) break;
        BSTR b = nullptr;
        std::string n = t->get_name(&b) == S_OK ? utf8(b) : "?";
        if (b) SysFreeString(b);
        std::string ifs;
        for (auto &e : kEnums) {
            IUnknown *p = nullptr;
            if (t->QueryInterface(e.iid, (void **)&p) == S_OK && p) {
                ifs += std::string(ifs.empty() ? "" : ",") + e.name;
                p->Release();
            }
        }
        v.push_back(n + " " + ifs);
        t->Release();
    }
    et->Release();
    out["tables"] = list(v);
}

static void checksums(IDiaSession *session)
{
    IDiaEnumSourceFiles *ef = nullptr;
    if (session->findFile(nullptr, nullptr, 0, &ef) != S_OK || !ef) return;
    for (;;) {
        IDiaSourceFile *f = nullptr;
        ULONG got = 0;
        if (ef->Next(1, &f, &got) != S_OK || got != 1 || !f) break;
        BSTR b = nullptr;
        std::string fn = f->get_fileName(&b) == S_OK ? utf8(b) : "?";
        if (b) SysFreeString(b);
        DWORD type = 0xA5A5A5A5;
        HRESULT ht = f->get_checksumType(&type);
        DWORD cb = 0xA5A5A5A5;
        HRESULT hs = f->get_checksum(0, &cb, nullptr);
        BYTE data[256];
        DWORD cb2 = 0;
        HRESULT hd = cb != 0xA5A5A5A5 && cb <= sizeof data ? f->get_checksum(cb, &cb2, data) : E_FAIL;
        std::string bytes;
        for (DWORD i = 0; hd == S_OK && i < cb2; i++) {
            char x[4];
            snprintf(x, sizeof x, "%02X", data[i]);
            bytes += x;
        }
        out["checksum|" + fn] = "{\"type\": " + jstr(hex(ht) + " " + hex(type)) + ", \"size\": " +
                                jstr(hex(hs) + " " + hex(cb)) + ", \"bytes\": " + jstr(hex(hd) + " " + bytes) + "}";
        f->Release();
    }
    ef->Release();
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: call_probe <msdia140.dll> <file.pdb>\n");
        return 2;
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) return 1;
    auto gco = (PFN_DllGetClassObject)(void *)GetProcAddress(dll, "DllGetClassObject");
    IClassFactory *cf = nullptr;
    if (!gco || gco(CLSID_DiaSource, IID_IClassFactory, (void **)&cf) != S_OK) return 1;
    IDiaDataSource *src = nullptr;
    if (cf->CreateInstance(nullptr, IID_IDiaDataSource, (void **)&src) != S_OK) return 1;
    if (src->loadDataFromPdb(argv[2]) != S_OK) return 1;
    IDiaSession *session = nullptr;
    if (src->openSession(&session) != S_OK) return 1;
    IDiaSymbol *global = nullptr;
    session->get_globalScope(&global);

    // Method names first: they depend on what was asked before.
    methodNames(global);
    order(global, "Exe");
    byTag(global, "Exe");
    auto comps = children(global, 2);
    int n = 0;
    for (auto *c : comps) {
        std::string p = "Exe/" + name(c) + "#" + std::to_string(n++);
        order(c, p);
        byTag(c, p);
        auto fns = children(c, 5);
        for (auto *f : fns) order(f, p + "/" + name(f));
        release(fns);
    }
    release(comps);
    tables(session);
    checksums(session);

    printf("{\n");
    size_t i = 0;
    for (auto &kv : out) printf(" %s: %s%s\n", jstr(kv.first).c_str(), kv.second.c_str(), ++i < out.size() ? "," : "");
    printf("}\n");
    global->Release();
    session->Release();
    src->Release();
    cf->Release();
    return 0;
}
