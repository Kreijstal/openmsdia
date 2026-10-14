// OpenMSDIA: finding the PDB of an executable, as the real DLL's
// loadDataForExe and loadDataFromCodeViewInfo do (observed with
// walker/exe_probe.cpp, oracle in oracle/exeload):
//
//  1. the executable's CodeView record (RSDS) is read from the file, or
//     through IDiaReadExeAtOffsetCallback / IDiaReadExeAtRVACallback when
//     the callback offers them (then the file is not opened at all);
//  2. IDiaLoadCallback::NotifyDebugDir gets the debug directory;
//  3. candidates, each reported through NotifyOpenPDB with its outcome:
//     the executable's directory (not for CodeView info), the path in the
//     record, then for every directory D of the ';'-separated search path
//     D\symbols\<ext>\<pdb>, D\<ext>\<pdb> and D\<pdb> (<ext> is the
//     executable's extension), then the same under %SystemRoot%;
//     IDiaLoadCallback2's Restrict* methods are asked before each group
//     (S_OK allows it), RestrictRegistryAccess before the (unused) registry
//     paths, RestrictSymbolServerAccess for every srv* entry, which is not
//     searched otherwise (no symbol server). Environment variables such as
//     _NT_SYMBOL_PATH are not used.
#include "exeload.h"

#include <string.h>
#include <wchar.h>

namespace om {

namespace {

struct Reader {
    HANDLE file = INVALID_HANDLE_VALUE;
    IDiaReadExeAtOffsetCallback *atOffset = nullptr;
    IDiaReadExeAtRVACallback *atRva = nullptr;
    bool read(uint64_t off, uint32_t rva, void *buf, DWORD n, bool byRva)
    {
        DWORD got = 0;
        if (atOffset && !byRva) return atOffset->ReadExecutableAt(off, n, &got, (BYTE *)buf) == S_OK && got == n;
        if (atRva && byRva) return atRva->ReadExecutableAtRVA(rva, n, &got, (BYTE *)buf) == S_OK && got == n;
        if (atRva) return atRva->ReadExecutableAtRVA((DWORD)off, n, &got, (BYTE *)buf) == S_OK && got == n;
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)off;
        if (!SetFilePointerEx(file, li, nullptr, FILE_BEGIN)) return false;
        return ReadFile(file, buf, n, &got, nullptr) && got == n;
    }
};

template <class T> T rd(const uint8_t *p) { T v; memcpy(&v, p, sizeof v); return v; }

// The debug directory and the CodeView record of a PE image.
HRESULT readDebugInfo(Reader &r, std::vector<uint8_t> &dir, std::vector<uint8_t> &cv)
{
    uint8_t dos[64];
    if (!r.read(0, 0, dos, sizeof dos, false) || dos[0] != 'M' || dos[1] != 'Z') return E_PDB_INVALID_EXECUTABLE;
    uint32_t lfanew = rd<uint32_t>(dos + 0x3c);
    // Signature, file header and the larger (PE32+) optional header.
    uint8_t nt[264];
    if (!r.read(lfanew, lfanew, nt, sizeof nt, false) || memcmp(nt, "PE\0\0", 4)) return E_PDB_INVALID_EXECUTABLE;
    uint16_t nsec = rd<uint16_t>(nt + 6), optSize = rd<uint16_t>(nt + 20), magic = rd<uint16_t>(nt + 24);
    size_t dirs = 24 + (magic == 0x20b ? 112 : 96);
    if (magic != 0x10b && magic != 0x20b) return E_PDB_INVALID_EXECUTABLE;
    uint32_t numDirs = rd<uint32_t>(nt + dirs - 4);
    if (numDirs <= 6 || dirs + 7 * 8 > sizeof nt) return E_PDB_NO_DEBUG_INFO;
    uint32_t dbgRva = rd<uint32_t>(nt + dirs + 48), dbgSize = rd<uint32_t>(nt + dirs + 52);
    if (!dbgRva || dbgSize < 28) return E_PDB_NO_DEBUG_INFO;
    uint64_t dbgOff = dbgRva;
    if (!r.atRva) {
        // Section headers one at a time, up to the one holding the directory.
        uint64_t sec = lfanew + 24 + optSize;
        bool found = false;
        for (uint16_t i = 0; i < nsec; i++) {
            uint8_t sh[40];
            if (!r.read(sec + i * 40, 0, sh, sizeof sh, false)) return E_PDB_INVALID_EXECUTABLE;
            uint32_t va = rd<uint32_t>(sh + 12), vs = rd<uint32_t>(sh + 8), raw = rd<uint32_t>(sh + 20),
                     rs = rd<uint32_t>(sh + 16);
            if (dbgRva >= va && dbgRva < va + (vs ? vs : rs)) {
                dbgOff = raw + (dbgRva - va);
                found = true;
                break;
            }
        }
        if (!found) return E_PDB_NO_DEBUG_INFO;
    }
    dir.resize(dbgSize - dbgSize % 28);
    if (!r.read(dbgOff, dbgRva, dir.data(), (DWORD)dir.size(), true)) return E_PDB_INVALID_EXECUTABLE;
    for (size_t k = 0; k + 28 <= dir.size(); k += 28) {
        const uint8_t *e = &dir[k];
        if (rd<uint32_t>(e + 12) != 2) continue;  // IMAGE_DEBUG_TYPE_CODEVIEW
        uint32_t size = rd<uint32_t>(e + 16), rva = rd<uint32_t>(e + 20), off = rd<uint32_t>(e + 24);
        if (size < 24 || size > 65536) continue;
        cv.resize(size);
        if (!r.read(off, rva, cv.data(), size, true)) return E_PDB_INVALID_EXECUTABLE;
        return S_OK;
    }
    return E_PDB_NO_DEBUG_INFO;
}

bool parseCv(const uint8_t *p, size_t n, CvInfo &out)
{
    if (n >= 24 && !memcmp(p, "RSDS", 4)) {
        memcpy(&out.guid, p + 4, 16);
        out.age = rd<uint32_t>(p + 20);
        out.path.assign((const char *)p + 24, strnlen((const char *)p + 24, n - 24));
        out.rsds = true;
        return true;
    }
    if (n >= 16 && !memcmp(p, "NB10", 4)) {
        out.sig = rd<uint32_t>(p + 8);
        out.age = rd<uint32_t>(p + 12);
        out.path.assign((const char *)p + 16, strnlen((const char *)p + 16, n - 16));
        out.rsds = false;
        return true;
    }
    return false;
}

std::wstring widen(const std::string &s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

}  // namespace

HRESULT findPdb(const wchar_t *exe, const wchar_t *searchPath, IUnknown *callback, const uint8_t *cvInfo,
                size_t cvSize, const TryPdb &tryPdb, std::wstring &lastError)
{
    IDiaLoadCallback *cb1 = nullptr;
    IDiaLoadCallback2 *cb2 = nullptr;
    Reader r;
    if (callback) {
        callback->QueryInterface(IID_IDiaLoadCallback, (void **)&cb1);
        callback->QueryInterface(IID_IDiaLoadCallback2, (void **)&cb2);
        callback->QueryInterface(IID_IDiaReadExeAtOffsetCallback, (void **)&r.atOffset);
        callback->QueryInterface(IID_IDiaReadExeAtRVACallback, (void **)&r.atRva);
    }
    IDiaLoadCallback *notify = cb2 ? static_cast<IDiaLoadCallback *>(cb2) : cb1;
    struct Release {
        IUnknown *p[4];
        ~Release()
        {
            for (IUnknown *u : p)
                if (u) u->Release();
        }
    } release{{cb1, cb2, r.atOffset, r.atRva}};

    std::wstring exePath = exe ? exe : L"";
    CvInfo info;
    if (cvInfo) {
        if (!parseCv(cvInfo, cvSize, info)) {
            lastError = exePath;
            return E_PDB_NO_DEBUG_INFO;
        }
    } else {
        if (!r.atOffset && !r.atRva) {
            r.file = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
            if (r.file == INVALID_HANDLE_VALUE) {
                lastError = exePath;
                return E_PDB_NOT_FOUND;
            }
        }
        std::vector<uint8_t> dir, cv;
        HRESULT hr = readDebugInfo(r, dir, cv);
        if (r.file != INVALID_HANDLE_VALUE) CloseHandle(r.file);
        if (hr != S_OK) {
            lastError = exePath;
            return hr;
        }
        if (!parseCv(cv.data(), cv.size(), info)) {
            lastError = exePath;
            return E_PDB_NO_DEBUG_INFO;
        }
        if (cb1) cb1->NotifyDebugDir(TRUE, (DWORD)dir.size(), dir.data());
    }
    std::wstring recorded = widen(info.path);
    size_t slash = recorded.find_last_of(L"\\/");
    std::wstring pdbName = slash == std::wstring::npos ? recorded : recorded.substr(slash + 1);
    // The executable's extension names a symbols subdirectory.
    std::wstring ext;
    if (!cvInfo) {
        size_t base = exePath.find_last_of(L"\\/");
        size_t dot = exePath.find_last_of(L'.');
        if (dot != std::wstring::npos && (base == std::wstring::npos || dot > base)) ext = exePath.substr(dot + 1);
        for (auto &c : ext) c = (wchar_t)towlower(c);
    }

    auto attempt = [&](const std::wstring &path) {
        HRESULT hr = tryPdb(path, info);
        if (notify) notify->NotifyOpenPDB(path.c_str(), hr);
        return hr == S_OK;
    };
    auto inDir = [&](std::wstring d) {
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        std::vector<std::wstring> tries;
        tries.push_back(d + L"\\symbols\\" + (ext.empty() ? L"" : ext + L"\\") + pdbName);
        if (!ext.empty()) tries.push_back(d + L"\\" + ext + L"\\" + pdbName);
        tries.push_back(d + L"\\" + pdbName);
        for (auto &t : tries)
            if (attempt(t)) return true;
        return false;
    };
    auto allowed = [](HRESULT hr) { return hr == S_OK; };

    if (cb2) cb2->RestrictDBGAccess();
    if (!cvInfo && (!cb2 || allowed(cb2->RestrictReferencePathAccess()))) {
        size_t base = exePath.find_last_of(L"\\/");
        std::wstring d = base == std::wstring::npos ? L"." : exePath.substr(0, base);
        if (attempt(d + L"\\" + pdbName)) return S_OK;
    }
    if (!cb2 || allowed(cb2->RestrictOriginalPathAccess()))
        if (!recorded.empty() && attempt(recorded)) return S_OK;
    if (searchPath) {
        std::wstring sp = searchPath;
        size_t at = 0;
        while (at <= sp.size()) {
            size_t semi = sp.find(L';', at);
            if (semi == std::wstring::npos) semi = sp.size();
            std::wstring e = sp.substr(at, semi - at);
            at = semi + 1;
            if (e.empty()) continue;
            if (e.find(L'*') != std::wstring::npos) {
                // srv*... and symsrv*... need a symbol server, which is not
                // used; cache*... entries are skipped as well.
                if (!_wcsnicmp(e.c_str(), L"srv*", 4) || !_wcsnicmp(e.c_str(), L"symsrv*", 7))
                    if (notify) notify->RestrictSymbolServerAccess();
                continue;
            }
            if (inDir(e)) return S_OK;
        }
    }
    if (notify) notify->RestrictRegistryAccess();
    if (!cb2 || allowed(cb2->RestrictSystemRootAccess())) {
        wchar_t root[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"SystemRoot", root, MAX_PATH);
        if (n && n < MAX_PATH && inDir(root)) return S_OK;
    }
    lastError = recorded;
    return E_PDB_NOT_FOUND;
}

}  // namespace om
