// exe_probe: record how a msdia140.dll finds the PDB of an executable
// (IDiaDataSource::loadDataForExe, loadDataFromCodeViewInfo) and what it
// asks of the load callback, as deterministic JSON.
//
// Usage: exe_probe <msdia140.dll> <app.exe> <app.pdb> <other.pdb> <work-dir> > out.json
//
// app.exe/app.pdb are a matching pair, other.pdb any other PDB. Each
// scenario copies them into its own directory under work-dir (patching the
// CodeView record's PDB path so that it does not exist, except where noted)
// and loads. The callback is a COM object whose QueryInterface and every
// vtable slot are logged; the IIDs and slot numbers it is asked for are how
// the layout of IDiaLoadCallback(2) and the read-exe callbacks (not in the
// type library) is established.
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>
#include <stdint.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "dia2_gen.h"

typedef HRESULT(WINAPI *PFN_DllGetClassObject)(REFCLSID, REFIID, void **);

static std::wstring g_work;
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
// Paths under the work directory print as <W>, case-insensitively.
static std::string mapped(std::string s)
{
    std::string w = utf8(g_work.c_str());
    for (size_t p = 0; p + w.size() <= s.size(); p++) {
        if (_strnicmp(s.c_str() + p, w.c_str(), w.size()) == 0) {
            s.replace(p, w.size(), "<W>");
            p += 2;
        }
    }
    for (auto &c : s)
        if (c == '/') c = '\\';
    return s;
}
static std::string hrstr(HRESULT hr)
{
    if (hr == S_OK) return "S_OK";
    if (hr == S_FALSE) return "S_FALSE";
    if (hr == E_NOTIMPL) return "E_NOTIMPL";
    if (hr == E_INVALIDARG) return "E_INVALIDARG";
    if (hr == E_UNEXPECTED) return "E_UNEXPECTED";
    if (hr == E_FAIL) return "E_FAIL";
    char b[16];
    snprintf(b, sizeof b, "0x%08lX", (unsigned long)hr);
    return b;
}
static std::string guidstr(const GUID &g)
{
    char b[64];
    snprintf(b, sizeof b, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", (unsigned long)g.Data1, g.Data2,
             g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

// A readable wide string at p (or "")?
static bool readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || (uintptr_t)p < 0x10000 || !VirtualQuery(p, &mi, sizeof mi)) return false;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (const char *)p + n <= (const char *)mi.BaseAddress + mi.RegionSize;
}
static std::string argText(uintptr_t a)
{
    if (a < 0x10000) return std::to_string((unsigned long long)a);
    if (readable((const void *)a, 2)) {
        const wchar_t *w = (const wchar_t *)a;
        std::wstring s;
        for (int i = 0; i < 260 && readable(w + i, 2) && w[i]; i++) {
            if (w[i] < 0x20 || w[i] > 0x7e) return "<ptr>";
            s += w[i];
        }
        if (s.size() >= 2) return "L" + jstr(mapped(utf8(s.c_str())));
    }
    return "<ptr>";
}

// ---------------------------------------------------------------- callback
struct Callback;
static std::vector<std::string> *g_log;
struct Callback {
    void **vtbl;
    LONG refs = 1;
    // Which IIDs QueryInterface accepts ("*" for all) and what every slot
    // returns.
    std::vector<std::string> accept;
    HRESULT ret = S_OK;
    // Slot 3 reads the executable: 1 at file offsets (ReadExecutableAt),
    // 2 at RVAs (ReadExecutableAtRVA), from this image.
    int read = 0;
    const std::vector<uint8_t> *image = nullptr;
};
static size_t rvaToOffset(const std::vector<uint8_t> &d, uint32_t rva);
static HRESULT STDMETHODCALLTYPE cbQI(Callback *self, REFIID riid, void **ppv)
{
    std::string g = guidstr(riid);
    bool ok = IsEqualIID(riid, IID_IUnknown);
    for (auto &a : self->accept)
        if (a == "*" || a == g) ok = true;
    if (g_log) g_log->push_back(jstr("QI " + g + (ok ? " ok" : " no")));
    if (!ok) {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    *ppv = self;
    self->refs++;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE cbAddRef(Callback *self) { return ++self->refs; }
static ULONG STDMETHODCALLTYPE cbRelease(Callback *self) { return --self->refs; }
template <int N>
static HRESULT STDMETHODCALLTYPE cbSlot(Callback *self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    if (N == 3 && self->read && self->image) {
        // ReadExecutableAt(offset, cb, pcb, pb) / ReadExecutableAtRVA(rva, cb, pcb, pb)
        size_t off = self->read == 1 ? (size_t)a : rvaToOffset(*self->image, (uint32_t)a);
        DWORD n = 0;
        if (off != (size_t)-1 && off < self->image->size()) {
            n = (DWORD)std::min<size_t>((size_t)b, self->image->size() - off);
            memcpy((void *)d, self->image->data() + off, n);
        }
        *(DWORD *)c = n;
        if (g_log) g_log->push_back(jstr("read " + std::to_string((unsigned long long)a) + " " +
                                         std::to_string((unsigned long long)b) + " -> " + std::to_string(n)));
        return n ? S_OK : E_FAIL;
    }
    if (g_log) {
        // NotifyOpenDBG/NotifyOpenPDB (slots 4, 5) pass an HRESULT second.
        // NotifyDebugDir(fExecutable, cbData, pbData), NotifyOpenDBG/PDB(path,
        // HRESULT); the other slots (Restrict*) take no arguments.
        std::string s = "slot " + std::to_string(N) + "(";
        if (N == 3) s += argText(a) + "," + argText(b) + "," + argText(c);
        else if (N == 4 || N == 5) s += argText(a) + "," + hrstr((HRESULT)b);
        g_log->push_back(jstr(s + ")"));
    }
    return self->ret;
}
static void *g_vtbl[] = {
    (void *)cbQI,      (void *)cbAddRef,   (void *)cbRelease,  (void *)cbSlot<3>,  (void *)cbSlot<4>,
    (void *)cbSlot<5>, (void *)cbSlot<6>,  (void *)cbSlot<7>,  (void *)cbSlot<8>,  (void *)cbSlot<9>,
    (void *)cbSlot<10>, (void *)cbSlot<11>, (void *)cbSlot<12>, (void *)cbSlot<13>, (void *)cbSlot<14>,
};

// ---------------------------------------------------------------- files
static std::vector<uint8_t> readFile(const std::wstring &p)
{
    std::vector<uint8_t> d;
    FILE *f = _wfopen(p.c_str(), L"rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    d.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (!d.empty() && fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    fclose(f);
    return d;
}
static void writeFile(const std::wstring &p, const std::vector<uint8_t> &d)
{
    FILE *f = _wfopen(p.c_str(), L"wb");
    if (!f) return;
    fwrite(d.data(), 1, d.size(), f);
    fclose(f);
}
static void mkdirs(const std::wstring &p)
{
    for (size_t i = 3; i <= p.size(); i++)
        if (i == p.size() || p[i] == L'\\') CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
}

// The CodeView RSDS record of a PE image: file offset, or 0.
struct Rsds {
    size_t off = 0, size = 0;  // of the record
    size_t dirEntry = 0;       // file offset of the IMAGE_DEBUG_DIRECTORY entry
    size_t dataDir = 0;        // file offset of the debug data directory
};
static Rsds findRsds(const std::vector<uint8_t> &d)
{
    Rsds r;
    if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return r;
    uint32_t pe = *(const uint32_t *)&d[0x3c];
    if (pe + 0x18 > d.size()) return r;
    uint16_t nsec = *(const uint16_t *)&d[pe + 6];
    uint16_t optsz = *(const uint16_t *)&d[pe + 20];
    size_t opt = pe + 24;
    uint16_t magic = *(const uint16_t *)&d[opt];
    size_t dirs = opt + (magic == 0x20b ? 112 : 96);
    r.dataDir = dirs + 6 * 8;
    uint32_t dbgRva = *(const uint32_t *)&d[r.dataDir], dbgSize = *(const uint32_t *)&d[r.dataDir + 4];
    size_t sec = opt + optsz;
    auto rva2off = [&](uint32_t rva) -> size_t {
        for (int i = 0; i < nsec; i++) {
            const uint8_t *s = &d[sec + i * 40];
            uint32_t va = *(const uint32_t *)(s + 12), vs = *(const uint32_t *)(s + 8), raw = *(const uint32_t *)(s + 20);
            if (rva >= va && rva < va + (vs ? vs : *(const uint32_t *)(s + 16))) return raw + (rva - va);
        }
        return 0;
    };
    size_t dd = rva2off(dbgRva);
    for (uint32_t k = 0; dd && k + 28 <= dbgSize; k += 28) {
        const uint8_t *e = &d[dd + k];
        if (*(const uint32_t *)(e + 12) != 2) continue;  // IMAGE_DEBUG_TYPE_CODEVIEW
        r.dirEntry = dd + k;
        r.size = *(const uint32_t *)(e + 16);
        r.off = *(const uint32_t *)(e + 24);
        break;
    }
    return r;
}
// Replace the PDB path in the RSDS record by `path` (same length or
// shorter, NUL-padded).
static bool patchPath(std::vector<uint8_t> &d, const std::string &path)
{
    Rsds r = findRsds(d);
    if (!r.off || r.size < 25) return false;
    size_t room = r.size - 24;
    if (path.size() + 1 > room) return false;
    memset(&d[r.off + 24], 0, room);
    memcpy(&d[r.off + 24], path.data(), path.size());
    return true;
}
static size_t rvaToOffset(const std::vector<uint8_t> &d, uint32_t rva)
{
    if (d.size() < 0x40) return (size_t)-1;
    uint32_t pe = *(const uint32_t *)&d[0x3c];
    uint16_t nsec = *(const uint16_t *)&d[pe + 6];
    uint16_t optsz = *(const uint16_t *)&d[pe + 20];
    size_t sec = pe + 24 + optsz;
    uint32_t hdrs = *(const uint32_t *)&d[pe + 24 + 60];  // SizeOfHeaders
    if (rva < hdrs) return rva;
    for (int i = 0; i < nsec; i++) {
        const uint8_t *s = &d[sec + i * 40];
        uint32_t va = *(const uint32_t *)(s + 12), vs = *(const uint32_t *)(s + 8), raw = *(const uint32_t *)(s + 20);
        if (rva >= va && rva < va + vs) return raw + (rva - va);
    }
    return (size_t)-1;
}
static std::string rsdsPath(const std::vector<uint8_t> &d)
{
    Rsds r = findRsds(d);
    if (!r.off) return "";
    return std::string((const char *)&d[r.off + 24]);
}

// ---------------------------------------------------------------- scenarios
static const char *const kLoadCb = "C32ADB82-73F4-421B-95D5-A4706EDF5DBE";   // IDiaLoadCallback
static const char *const kLoadCb2 = "4688A074-5A4D-4486-AEA8-7B90711D9F7C";  // IDiaLoadCallback2
static const char *const kReadOff = "587A461C-B80B-4F54-9194-5032589A6319";  // IDiaReadExeAtOffsetCallback
static const char *const kReadRva = "8E3F80CA-7517-432A-BA07-285134AAEA8E";  // IDiaReadExeAtRVACallback
static IClassFactory *g_cf;
static std::vector<uint8_t> g_exe, g_pdb, g_other;
static std::string g_pdbName;  // file name in the RSDS record

static std::string loaded(IDiaDataSource *src)
{
    IDiaSession *s = nullptr;
    if (src->openSession(&s) != S_OK || !s) return jstr("-");
    IDiaSymbol *g = nullptr;
    std::string r = jstr("?");
    if (s->get_globalScope(&g) == S_OK && g) {
        GUID guid;
        DWORD age = 0;
        if (g->get_guid(&guid) == S_OK && g->get_age(&age) == S_OK) r = jstr(guidstr(guid) + "/" + std::to_string(age));
        g->Release();
    }
    s->Release();
    return r;
}

struct Result {
    std::map<std::string, std::string> o;
};

static std::string run(const std::wstring &dir, const wchar_t *exe, const wchar_t *search, int cbMode, HRESULT cbRet,
                       const char *how = "exe", const char *onlyIid = nullptr, int read = 0,
                       const std::vector<uint8_t> *image = nullptr)
{
    std::vector<std::string> log;
    Callback cb;
    cb.vtbl = g_vtbl;
    cb.ret = cbRet;
    if (cbMode == 1) cb.accept = {};       // only IUnknown
    else if (cbMode == 2) cb.accept = {"*"};
    else if (cbMode == 3) cb.accept = {onlyIid};
    cb.read = read;
    cb.image = image;
    IDiaDataSource *src = nullptr;
    g_cf->CreateInstance(nullptr, IID_IDiaDataSource, (void **)&src);
    g_log = &log;
    HRESULT hr;
    std::wstring exePath = exe ? dir + L"\\" + exe : L"";
    if (!strcmp(how, "cv")) {
        const std::vector<uint8_t> &img = image ? *image : g_exe;
        Rsds r = findRsds(img);
        std::vector<uint8_t> cv(img.begin() + r.off, img.begin() + r.off + r.size);
        hr = src->loadDataFromCodeViewInfo((LPWSTR)exePath.c_str(), (LPWSTR)search, (DWORD)cv.size(), cv.data(),
                                           cbMode ? (IUnknown *)&cb : nullptr);
    } else {
        hr = src->loadDataForExe((LPWSTR)exePath.c_str(), (LPWSTR)search, cbMode ? (IUnknown *)&cb : nullptr);
    }
    g_log = nullptr;
    std::string r = "{\"hr\": " + jstr(hrstr(hr));
    BSTR b = nullptr;
    if (src->get_lastError(&b) == S_OK) r += ", \"lastError\": " + jstr(mapped(utf8(b)));
    if (b) SysFreeString(b);
    if (hr == S_OK) r += ", \"loaded\": " + loaded(src);
    if (cbMode) {
        r += ", \"callback\": [";
        for (size_t i = 0; i < log.size(); i++) r += (i ? ", " : "") + log[i];
        r += "]";
    }
    src->Release();
    return r + "}";
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: exe_probe <msdia140.dll> <app.exe> <app.pdb> <other.pdb> <work-dir>\n");
        return 2;
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) return 1;
    auto gco = (PFN_DllGetClassObject)(void *)GetProcAddress(dll, "DllGetClassObject");
    if (!gco || gco(CLSID_DiaSource, IID_IClassFactory, (void **)&g_cf) != S_OK) return 1;
    g_exe = readFile(argv[2]);
    g_pdb = readFile(argv[3]);
    g_other = readFile(argv[4]);
    wchar_t full[MAX_PATH];
    GetFullPathNameW(argv[5], MAX_PATH, full, nullptr);
    g_work = full;
    if (g_exe.empty() || g_pdb.empty() || g_other.empty()) return 1;
    std::string orig = rsdsPath(g_exe);
    size_t slash = orig.find_last_of("\\/");
    g_pdbName = slash == std::string::npos ? orig : orig.substr(slash + 1);
    std::wstring pdbW(g_pdbName.begin(), g_pdbName.end());
    // A path of the same length that does not exist: Z:\x\x...\name.pdb
    std::string missing = "Z:\\";
    while (missing.size() + 1 + g_pdbName.size() < orig.size()) missing += "x";
    missing += "\\" + g_pdbName;
    std::vector<uint8_t> exeMissing = g_exe;
    patchPath(exeMissing, missing);

    std::map<std::string, std::string> out;
    auto dirOf = [&](const char *name) {
        std::wstring d = g_work + L"\\" + std::wstring(name, name + strlen(name));
        mkdirs(d);
        return d;
    };
    // Matching PDB beside the executable.
    {
        std::wstring d = dirOf("beside");
        writeFile(d + L"\\app.exe", exeMissing);
        writeFile(d + L"\\" + pdbW, g_pdb);
        out["beside"] = run(d, L"app.exe", nullptr, 0, S_OK);
        out["beside, callback (IUnknown only)"] = run(d, L"app.exe", nullptr, 1, S_OK);
        out["codeview info, beside"] = run(d, L"app.exe", nullptr, 0, S_OK, "cv", nullptr, 0, &exeMissing);
        out["codeview info, beside, load callback 2"] =
            run(d, L"app.exe", nullptr, 3, S_OK, "cv", kLoadCb2, 0, &exeMissing);
        out["codeview info, no executable path"] = run(d, nullptr, nullptr, 0, S_OK, "cv", nullptr, 0, &exeMissing);
        // The executable read through the callback only.
        out["read at offset callback"] = run(d, L"app.exe", nullptr, 3, S_OK, "exe", kReadOff, 1, &exeMissing);
        out["read at RVA callback"] = run(d, L"app.exe", nullptr, 3, S_OK, "exe", kReadRva, 2, &exeMissing);
        out["read at offset callback, no file"] = run(d, L"nofile.exe", nullptr, 3, S_OK, "exe", kReadOff, 1, &exeMissing);
    }
    // The callback interfaces one at a time (IIDs as queried by the real DLL).
    // (The read callbacks only with real data, see below: without it the
    // real DLL parses whatever its buffers held before.)
    for (const char *iid : {"C32ADB82-73F4-421B-95D5-A4706EDF5DBE", "4688A074-5A4D-4486-AEA8-7B90711D9F7C"}) {
        std::wstring d = g_work + L"\\beside";
        out[std::string("beside, callback ") + iid] = run(d, L"app.exe", nullptr, 3, S_OK, "exe", iid);
        out[std::string("beside, callback ") + iid + " (S_FALSE)"] = run(d, L"app.exe", nullptr, 3, S_FALSE, "exe", iid);
        out[std::string("codeview info, callback ") + iid] = run(d, L"app.exe", nullptr, 3, S_OK, "cv", iid);
    }
    // No PDB anywhere.
    {
        std::wstring d = dirOf("none");
        writeFile(d + L"\\app.exe", exeMissing);
        out["none"] = run(d, L"app.exe", nullptr, 0, S_OK);
        out["none, load callback"] = run(d, L"app.exe", nullptr, 3, S_OK, "exe", kLoadCb);
        out["none, load callback 2"] = run(d, L"app.exe", nullptr, 3, S_OK, "exe", kLoadCb2);
        out["none, load callback 2 (S_FALSE)"] = run(d, L"app.exe", nullptr, 3, S_FALSE, "exe", kLoadCb2);
        std::wstring sp = g_work + L"\\none\\empty1;" + g_work + L"\\none\\empty2";
        mkdirs(g_work + L"\\none\\empty1");
        out["none, search path"] = run(d, L"app.exe", sp.c_str(), 3, S_OK, "exe", kLoadCb);
        out["none, search path, load callback 2"] = run(d, L"app.exe", sp.c_str(), 3, S_OK, "exe", kLoadCb2);
    }
    // The PDB at the path recorded in the executable.
    {
        std::wstring d = dirOf("recorded");
        std::wstring sub = d + L"\\sym";
        mkdirs(sub);
        std::string p = utf8(sub.c_str()) + "\\" + g_pdbName;
        std::vector<uint8_t> e = g_exe;
        if (patchPath(e, p)) {
            std::wstring bin = d + L"\\bin";
            mkdirs(bin);
            writeFile(bin + L"\\app.exe", e);
            writeFile(sub + L"\\" + pdbW, g_pdb);
            out["recorded path"] = run(bin, L"app.exe", nullptr, 3, S_OK, "exe", kLoadCb);
            out["recorded path, load callback 2"] = run(bin, L"app.exe", nullptr, 3, S_OK, "exe", kLoadCb2);
            out["recorded path, load callback 2 (S_FALSE)"] = run(bin, L"app.exe", nullptr, 3, S_FALSE, "exe", kLoadCb2);
            // Both beside the executable and at the recorded path.
            writeFile(bin + L"\\" + pdbW, g_pdb);
            out["recorded path and beside"] = run(bin, L"app.exe", nullptr, 3, S_OK, "exe", kLoadCb);
            std::wstring sp = d + L"\\sym";
            out["recorded path and beside, search path"] = run(bin, L"app.exe", sp.c_str(), 3, S_OK, "exe", kLoadCb);
        } else {
            out["recorded path"] = jstr("(path too long to patch)");
        }
    }
    // Search path: plain directories, ';'-separated.
    {
        std::wstring d = dirOf("search");
        std::wstring bin = d + L"\\bin", sym = d + L"\\sym", sym2 = d + L"\\sym2";
        mkdirs(bin);
        mkdirs(sym);
        mkdirs(sym2);
        writeFile(bin + L"\\app.exe", exeMissing);
        writeFile(sym2 + L"\\" + pdbW, g_pdb);
        std::wstring sp = sym + L";" + sym2;
        out["search path, second dir"] = run(bin, L"app.exe", sp.c_str(), 0, S_OK);
        out["search path, second dir, load callback"] = run(bin, L"app.exe", sp.c_str(), 3, S_OK, "exe", kLoadCb);
        out["search path, second dir, load callback 2"] = run(bin, L"app.exe", sp.c_str(), 3, S_OK, "exe", kLoadCb2);
        std::wstring sp2 = L" " + sym2 + L" ";
        out["search path, spaces"] = run(bin, L"app.exe", sp2.c_str(), 0, S_OK);
        std::wstring sp3 = sym2 + L"\\";
        out["search path, trailing backslash"] = run(bin, L"app.exe", sp3.c_str(), 0, S_OK);
        // A symbol store layout: <dir>\<pdb>\<GUID><age>\<pdb>.
        Rsds r = findRsds(g_exe);
        GUID g;
        memcpy(&g, &g_exe[r.off + 4], 16);
        uint32_t age = *(const uint32_t *)&g_exe[r.off + 20];
        char key[64];
        snprintf(key, sizeof key, "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X", (unsigned long)g.Data1,
                 g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
                 g.Data4[6], g.Data4[7], age);
        std::wstring store = d + L"\\store";
        std::wstring keyW(key, key + strlen(key));
        mkdirs(store + L"\\" + pdbW + L"\\" + keyW);
        writeFile(store + L"\\" + pdbW + L"\\" + keyW + L"\\" + pdbW, g_pdb);
        std::wstring srv = L"srv*" + store;
        out["search path, srv* local store"] = run(bin, L"app.exe", srv.c_str(), 0, S_OK);
        out["search path, srv* local store, load callback 2"] = run(bin, L"app.exe", srv.c_str(), 3, S_OK, "exe", kLoadCb2);
        std::wstring srv2 = L"SRV*" + store + L"*" + sym;
        out["search path, SRV*store*dir"] = run(bin, L"app.exe", srv2.c_str(), 0, S_OK);
        std::wstring cache = L"cache*" + sym + L";" + sym2;
        out["search path, cache* then dir"] = run(bin, L"app.exe", cache.c_str(), 0, S_OK);
        out["search path, store dir as plain dir"] = run(bin, L"app.exe", store.c_str(), 0, S_OK);
    }
    // A PDB with the right name but another signature.
    {
        std::wstring d = dirOf("mismatch");
        writeFile(d + L"\\app.exe", exeMissing);
        writeFile(d + L"\\" + pdbW, g_other);
        out["mismatched pdb"] = run(d, L"app.exe", nullptr, 0, S_OK);
        out["mismatched pdb, load callback"] = run(d, L"app.exe", nullptr, 3, S_OK, "exe", kLoadCb);
    }
    // No debug directory.
    {
        std::wstring d = dirOf("nodebug");
        std::vector<uint8_t> e = g_exe;
        Rsds r = findRsds(e);
        if (r.dataDir) memset(&e[r.dataDir], 0, 8);
        writeFile(d + L"\\app.exe", e);
        writeFile(d + L"\\" + pdbW, g_pdb);
        out["no debug directory"] = run(d, L"app.exe", nullptr, 3, S_OK, "exe", kLoadCb);
    }
    // Not an executable, no file.
    {
        std::wstring d = dirOf("bad");
        std::vector<uint8_t> t(4096, 'x');
        writeFile(d + L"\\app.exe", t);
        out["not an executable"] = run(d, L"app.exe", nullptr, 0, S_OK);
        out["missing file"] = run(d, L"nothere.exe", nullptr, 0, S_OK);
        out["pdb passed as exe"] = run(dirOf("beside"), pdbW.c_str(), nullptr, 0, S_OK);
    }
    // _NT_SYMBOL_PATH and _NT_ALT_SYMBOL_PATH.
    {
        std::wstring d = g_work + L"\\search\\bin", sym2 = g_work + L"\\search\\sym2";
        SetEnvironmentVariableW(L"_NT_SYMBOL_PATH", sym2.c_str());
        out["_NT_SYMBOL_PATH"] = run(d, L"app.exe", nullptr, 3, S_OK, "exe", kLoadCb);
        SetEnvironmentVariableW(L"_NT_SYMBOL_PATH", nullptr);
        SetEnvironmentVariableW(L"_NT_ALT_SYMBOL_PATH", sym2.c_str());
        out["_NT_ALT_SYMBOL_PATH"] = run(d, L"app.exe", nullptr, 3, S_OK, "exe", kLoadCb);
        SetEnvironmentVariableW(L"_NT_ALT_SYMBOL_PATH", nullptr);
    }
    // Twice on one data source.
    {
        std::wstring d = g_work + L"\\beside\\app.exe";
        IDiaDataSource *src = nullptr;
        g_cf->CreateInstance(nullptr, IID_IDiaDataSource, (void **)&src);
        HRESULT a = src->loadDataForExe((LPWSTR)d.c_str(), nullptr, nullptr);
        HRESULT b = src->loadDataForExe((LPWSTR)d.c_str(), nullptr, nullptr);
        out["twice"] = "[" + jstr(hrstr(a)) + ", " + jstr(hrstr(b)) + "]";
        src->Release();
    }
    printf("{\n");
    size_t i = 0;
    for (auto &kv : out) printf("  %s: %s%s\n", jstr(kv.first).c_str(), kv.second.c_str(), ++i < out.size() ? "," : "");
    printf("}\n");
    return 0;
}
