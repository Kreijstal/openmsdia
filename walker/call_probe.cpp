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
//   segments                  the SegmentMap table as IDiaEnumSegments
//                             (E8368CA9-..., from Ghidra's QueryInterface
//                             order): every slot of each IDiaSegment
//   framedata                 the FrameData table as IDiaEnumFrameData
//                             (9FC77A4B-...): count and rows' RVAs
//   symtable                  the Symbols table's rows in enumeration
//                             order ("tag:name", at most 5000)
//   qi|<iid>                  a symbol queried for an interface Dia2Dump
//                             asks for
//   namesq|<class>|<getter>   get_name, <getter>, get_name on methods
//                             reached through their class
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

static void symbolTable(IDiaSession *session)
{
    IDiaEnumTables *et = nullptr;
    if (session->getEnumTables(&et) != S_OK || !et) return;
    for (;;) {
        IDiaTable *t = nullptr;
        ULONG got = 0;
        if (et->Next(1, &t, &got) != S_OK || got != 1 || !t) break;
        IDiaEnumSymbols *es = nullptr;
        if (t->QueryInterface(IID_IDiaEnumSymbols, (void **)&es) == S_OK && es) {
            std::vector<std::string> v;
            for (;;) {
                IDiaSymbol *x = nullptr;
                ULONG g = 0;
                if (v.size() >= 5000 || es->Next(1, &x, &g) != S_OK || g != 1 || !x) break;
                v.push_back(std::to_string(tag(x)) + ":" + name(x));
                x->Release();
            }
            out["symtable"] = list(v);
            es->Release();
        }
        t->Release();
    }
    et->Release();
}

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

static std::string checksumOf(IDiaSourceFile *f)
{
    DWORD type = 0xA5A5A5A5;
    HRESULT ht = f->get_checksumType(&type);
    BYTE data[256];
    DWORD cb = 0xA5A5A5A5;
    HRESULT hd = f->get_checksum(sizeof data, &cb, data);
    std::string bytes;
    for (DWORD i = 0; hd == S_OK && i < cb && i < sizeof data; i++) {
        char x[4];
        snprintf(x, sizeof x, "%02X", data[i]);
        bytes += x;
    }
    return jstr(hex(ht) + " " + hex(type) + " / " + hex(hd) + " " + hex(cb) + " " + bytes);
}

// The files of each compiland (findFile with the compiland) and of the
// first line of each, through IDiaLineNumber::get_sourceFile.
static void moduleChecksums(IDiaSession *session, IDiaSymbol *global)
{
    auto comps = children(global, 2);
    for (auto *c : comps) {
        IDiaEnumSourceFiles *ef = nullptr;
        if (session->findFile(c, nullptr, 0, &ef) != S_OK || !ef) continue;
        for (;;) {
            IDiaSourceFile *f = nullptr;
            ULONG got = 0;
            if (ef->Next(1, &f, &got) != S_OK || got != 1 || !f) break;
            BSTR b = nullptr;
            std::string fn = f->get_fileName(&b) == S_OK ? utf8(b) : "?";
            if (b) SysFreeString(b);
            out["checksum.compiland|" + fn] = checksumOf(f);
            IDiaEnumLineNumbers *el = nullptr;
            if (session->findLines(c, f, &el) == S_OK && el) {
                IDiaLineNumber *ln = nullptr;
                ULONG g2 = 0;
                if (el->Next(1, &ln, &g2) == S_OK && g2 == 1 && ln) {
                    IDiaSourceFile *lf = nullptr;
                    if (ln->get_sourceFile(&lf) == S_OK && lf) {
                        out["checksum.line|" + fn] = checksumOf(lf);
                        lf->Release();
                    }
                    ln->Release();
                }
                el->Release();
            }
            f->Release();
        }
        ef->Release();
    }
    release(comps);
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
        cb2 = 0xA5A5A5A5;
        HRESULT hd = f->get_checksum(sizeof data, &cb2, data);
        if (cb2 > sizeof data) cb2 = 0;
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

// IIDs learned from clients' QueryInterface calls (OPENMSDIA_TRACE_QI).
static const IID kIID_EnumSegments = {0xE8368CA9, 0x01D1, 0x419D, {0xAC, 0x0C, 0xE3, 0x12, 0x35, 0xDB, 0xDA, 0x9F}};
static const IID kIID_EnumFrameData = {0x9FC77A4B, 0x3C1C, 0x44ED, {0xA7, 0x98, 0x6C, 0x1D, 0xEE, 0xA5, 0x3E, 0x1F}};
static const IID kIID_SymbolNext = {0xD4D55C6B, 0x2C67, 0x4C1A, {0xB8, 0xF2, 0x7E, 0x8F, 0x3D, 0x5E, 0x2A, 0x1D}};

typedef HRESULT(STDMETHODCALLTYPE *Getter)(void *, ULONGLONG *);
typedef HRESULT(STDMETHODCALLTYPE *CountFn)(void *, LONG *);
typedef HRESULT(STDMETHODCALLTYPE *NextFn)(void *, ULONG, IUnknown **, ULONG *);

// An enumerator laid out like DIA's others: IUnknown, get__NewEnum,
// get_Count (4), Item, Next (6), Skip, Reset, Clone.
static void probeEnum(IDiaSession *session, const char *table, const IID &iid, const char *key, int slots)
{
    IDiaEnumTables *et = nullptr;
    if (session->getEnumTables(&et) != S_OK || !et) return;
    for (;;) {
        IDiaTable *t = nullptr;
        ULONG got = 0;
        if (et->Next(1, &t, &got) != S_OK || got != 1 || !t) break;
        BSTR b = nullptr;
        std::string n = t->get_name(&b) == S_OK ? utf8(b) : "?";
        if (b) SysFreeString(b);
        if (n == table) {
            IUnknown *e = nullptr;
            HRESULT hq = t->QueryInterface(iid, (void **)&e);
            std::vector<std::string> v;
            v.push_back("QI " + hex((unsigned long)hq));
            if (hq == S_OK && e) {
                void **vt = *(void ***)e;
                LONG cnt = -1;
                HRESULT hc = ((CountFn)vt[4])(e, &cnt);
                v.push_back("count " + hex((unsigned long)hc) + " " + std::to_string(cnt));
                for (int row = 0; row < 64; row++) {
                    IUnknown *item = nullptr;
                    ULONG f = 0;
                    if (((NextFn)vt[6])(e, 1, &item, &f) != S_OK || f != 1 || !item) break;
                    std::string r = "row";
                    IDiaFrameData *fd = nullptr;
                    if (slots == 0 && row < 4 && item->QueryInterface(IID_IDiaFrameData, (void **)&fd) == S_OK && fd) {
                        // frameByRVA and frameByVA, assumed to follow Clone
                        // (slots 10 and 11), as the documentation lists them.
                        DWORD rva = 0;
                        fd->get_relativeVirtualAddress(&rva);
                        typedef HRESULT(STDMETHODCALLTYPE * ByRva)(void *, DWORD, IDiaFrameData **);
                        typedef HRESULT(STDMETHODCALLTYPE * ByVa)(void *, ULONGLONG, IDiaFrameData **);
                        IDiaFrameData *a = nullptr, *b2 = nullptr;
                        HRESULT h1 = ((ByRva)vt[10])(e, rva + 1, &a);
                        HRESULT h2 = ((ByVa)vt[11])(e, (ULONGLONG)rva + 1, &b2);
                        DWORD ra = 0, rb = 0;
                        if (a) { a->get_relativeVirtualAddress(&ra); a->Release(); }
                        if (b2) { b2->get_relativeVirtualAddress(&rb); b2->Release(); }
                        char x[128];
                        snprintf(x, sizeof x, " rva=%lX byRVA(+1)=%lX:%lX byVA(+1)=%lX:%lX", (unsigned long)rva,
                                 (unsigned long)h1, (unsigned long)ra, (unsigned long)h2, (unsigned long)rb);
                        r += x;
                        fd->Release();
                    }
                    void **iv = *(void ***)item;
                    for (int sl = 3; sl < 3 + slots; sl++) {
                        ULONGLONG val = 0xA5A5A5A5A5A5A5A5ull;
                        HRESULT h = ((Getter)iv[sl])(item, &val);
                        char x[64];
                        snprintf(x, sizeof x, " %d:%lX:%llX", sl, (unsigned long)h, val);
                        r += x;
                    }
                    v.push_back(r);
                    item->Release();
                }
                e->Release();
            }
            out[key] = list(v);
        }
        t->Release();
    }
    et->Release();
}

// get_name, then another getter, then get_name, on methods reached through
// their class (global findChildren(SymTagUDT), class findChildren(Null)).
static void nameSequences(IDiaSymbol *global)
{
    static const char *getters[] = {"none", "type", "length", "classParent", "lexicalParent", "symIndexId",
                                    "undecoratedName", "locationType", "access", "virtual", "relativeVirtualAddress"};
    auto udts = children(global, 11);
    int n = 0;
    for (auto *u : udts) {
        if (n++ >= 40) break;
        std::string un = name(u);
        for (const char *g : getters) {
            auto kids = children(u, 0);
            std::vector<std::string> v;
            for (auto *k : kids) {
                if (tag(k) != 5) continue;
                std::string a = name(k);
                std::string sg = g;
                IDiaSymbol *x = nullptr;
                DWORD d = 0;
                ULONGLONG q = 0;
                BSTR bs = nullptr;
                if (sg == "type") { if (k->get_type(&x) == S_OK && x) x->Release(); }
                else if (sg == "length") k->get_length(&q);
                else if (sg == "classParent") { if (k->get_classParent(&x) == S_OK && x) x->Release(); }
                else if (sg == "lexicalParent") { if (k->get_lexicalParent(&x) == S_OK && x) x->Release(); }
                else if (sg == "symIndexId") k->get_symIndexId(&d);
                else if (sg == "undecoratedName") { if (k->get_undecoratedName(&bs) == S_OK && bs) SysFreeString(bs); }
                else if (sg == "locationType") k->get_locationType(&d);
                else if (sg == "access") k->get_access(&d);
                else if (sg == "virtual") { LONG bb; k->get_virtual(&bb); }
                else if (sg == "relativeVirtualAddress") k->get_relativeVirtualAddress(&d);
                v.push_back(a + " | " + name(k));
            }
            release(kids);
            if (!v.empty()) out["namesq|" + un + "#" + std::to_string(n) + "|" + g] = list(v);
        }
    }
    release(udts);
}

static void probeQI(IDiaSymbol *global)
{
    auto kids = children(global, 0);
    std::map<DWORD, std::string> byTag;
    for (auto *k : kids) {
        DWORD t = tag(k);
        if (byTag.count(t)) continue;
        IUnknown *p = nullptr;
        HRESULT h = k->QueryInterface(kIID_SymbolNext, (void **)&p);
        byTag[t] = hex((unsigned long)h);
        if (p) p->Release();
    }
    release(kids);
    std::vector<std::string> v;
    for (auto &kv : byTag) v.push_back(std::to_string(kv.first) + " " + kv.second);
    out["qi|D4D55C6B"] = list(v);
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
    nameSequences(global);
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
    symbolTable(session);
    checksums(session);
    moduleChecksums(session, global);
    probeEnum(session, "SegmentMap", kIID_EnumSegments, "segments", 9);
    probeEnum(session, "FrameData", kIID_EnumFrameData, "framedata", 0);
    probeQI(global);

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
