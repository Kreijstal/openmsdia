// name_probe: record how a msdia140.dll undecorates names, for the
// undecoration test (tools/undname_test.py).
//
//   name_probe dia <msdia140.dll> <file.pdb>...   > out.json
//       every public symbol of the PDBs: get_name, get_undecoratedName and
//       get_undecoratedNameEx for each option set in kFlags
//   name_probe dbghelp <names.txt>                > out.json
//       dbghelp's UnDecorateSymbolName on each line, for comparison only
//   name_probe flags
//       the option sets, one per line
//
// Output: {"<decorated name>": {"name": ..., "undecoratedName": ...,
// "ex": {"0x0": ..., "0x1000": ...}}}, keys sorted. Under "ex" an option
// set is listed only when its result differs from option set 0; a failing
// call is "!<HRESULT>".
#include <windows.h>
#include <dbghelp.h>
#include <oleauto.h>
#include <stdio.h>
#include <map>
#include <string>
#include <vector>

#include "dia2_gen.h"

typedef HRESULT(WINAPI *PFN_DllGetClassObject)(REFCLSID, REFIID, void **);

static const unsigned kFlags[] = {
    0x0,    0x1,    0x2,    0x4,     0x8,     0x10,    0x20,    0x40,    0x60,   0x80,   0x100,  0x200,  0x400,
    0x800,  0x1000, 0x2000, 0x4000,  0x8000,  0x10000, 0x20000, 0x40000, 0x80000, 0x87FE, 0x3000, 0x2002, 0xFFFF,
};

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

static std::string bstr_result(HRESULT hr, BSTR b)
{
    std::string s = hr == S_OK ? jstr(utf8(b)) : jstr("!" + hex((unsigned long)hr));
    if (b) SysFreeString(b);
    return s;
}

struct Entry {
    std::string name, undecorated;
    std::map<unsigned, std::string> ex;  // raw JSON values
};

static void print(const std::map<std::string, Entry> &all)
{
    printf("{\n");
    size_t i = 0;
    for (auto &kv : all) {
        const Entry &e = kv.second;
        std::string s = "  " + jstr(kv.first) + ": {";
        bool first = true;
        if (!e.name.empty()) { s += "\"name\": " + e.name; first = false; }
        if (!e.undecorated.empty()) { s += std::string(first ? "" : ", ") + "\"undecoratedName\": " + e.undecorated; first = false; }
        s += std::string(first ? "" : ", ") + "\"ex\": {";
        const std::string &base = e.ex.at(0);
        bool f2 = true;
        for (auto &x : e.ex) {
            if (x.first != 0 && x.second == base) continue;
            s += std::string(f2 ? "" : ", ") + jstr(hex(x.first)) + ": " + x.second;
            f2 = false;
        }
        s += "}}";
        printf("%s%s\n", s.c_str(), ++i < all.size() ? "," : "");
    }
    printf("}\n");
}

static int probe_dia(int argc, wchar_t **argv)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(argv[2]);
    if (!dll) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    auto gco = (PFN_DllGetClassObject)(void *)GetProcAddress(dll, "DllGetClassObject");
    if (!gco) { fprintf(stderr, "no DllGetClassObject\n"); return 1; }
    IClassFactory *cf = nullptr;
    if (gco(CLSID_DiaSource, IID_IClassFactory, (void **)&cf) != S_OK) return 1;
    std::map<std::string, Entry> all;
    for (int a = 3; a < argc; a++) {
        IDiaDataSource *src = nullptr;
        IDiaSession *session = nullptr;
        IDiaSymbol *global = nullptr;
        IDiaEnumSymbols *e = nullptr;
        if (cf->CreateInstance(nullptr, IID_IDiaDataSource, (void **)&src) != S_OK) return 1;
        HRESULT hr = src->loadDataFromPdb(argv[a]);
        if (hr != S_OK) { fprintf(stderr, "load %ls: 0x%08lX\n", argv[a], (unsigned long)hr); return 1; }
        src->openSession(&session);
        session->get_globalScope(&global);
        global->findChildren(SymTagPublicSymbol, nullptr, 0, &e);
        for (;;) {
            IDiaSymbol *s = nullptr;
            ULONG got = 0;
            if (e->Next(1, &s, &got) != S_OK || got != 1 || !s) break;
            BSTR b = nullptr;
            HRESULT h = s->get_name(&b);
            std::string key = h == S_OK ? utf8(b) : "";
            Entry en;
            en.name = bstr_result(h, b);
            b = nullptr;
            h = s->get_undecoratedName(&b);
            en.undecorated = bstr_result(h, b);
            for (unsigned f : kFlags) {
                b = nullptr;
                h = s->get_undecoratedNameEx(f, &b);
                en.ex[f] = bstr_result(h, b);
            }
            all.emplace(key, en);
            s->Release();
        }
        e->Release();
        global->Release();
        session->Release();
        src->Release();
    }
    print(all);
    return 0;
}

static int probe_dbghelp(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 1;
    std::map<std::string, Entry> all;
    char line[16384];
    while (fgets(line, sizeof line, f)) {
        std::string n = line;
        while (!n.empty() && (n.back() == '\n' || n.back() == '\r' || n.back() == ' ')) n.pop_back();
        if (n.empty() || n[0] == '#') continue;
        Entry en;
        for (unsigned fl : kFlags) {
            static char out[65536];
            DWORD r = UnDecorateSymbolName(n.c_str(), out, sizeof out, fl);
            en.ex[fl] = r ? jstr(out) : jstr("!" + hex(GetLastError()));
        }
        all.emplace(n, en);
    }
    fclose(f);
    print(all);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc >= 2 && !wcscmp(argv[1], L"flags")) {
        for (unsigned fl : kFlags) printf("%s\n", hex(fl).c_str());
        return 0;
    }
    if (argc >= 4 && !wcscmp(argv[1], L"dia")) return probe_dia(argc, argv);
    if (argc == 3 && !wcscmp(argv[1], L"dbghelp")) return probe_dbghelp(argv[2]);
    fprintf(stderr, "usage: name_probe dia <msdia140.dll> <file.pdb>... | dbghelp <names.txt> | flags\n");
    return 2;
}
