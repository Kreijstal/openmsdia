// OpenMSDIA COM objects: DiaSource, the session, symbols and enumerators.
#include "model.h"  // LLVM before windows.h

#include <windows.h>
#include <oleauto.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "dia2_stubs_gen.h"

using namespace om;

extern std::atomic<long> g_objects;

namespace {

BSTR toBSTR(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    BSTR b = SysAllocStringLen(nullptr, n);
    if (b && n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), b, n);
    return b;
}

std::string fromW(const wchar_t *w)
{
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// IUnknown for single-inheritance-chain COM classes. `Iids` lists the
// interfaces answered with `this`.
#define OM_IUNKNOWN(...)                                                                  \
    std::atomic<long> refs_{1};                                                           \
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++refs_; }                  \
    ULONG STDMETHODCALLTYPE Release() override                                            \
    {                                                                                     \
        long r = --refs_;                                                                 \
        if (!r) delete this;                                                              \
        return (ULONG)r;                                                                  \
    }                                                                                     \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override            \
    {                                                                                     \
        if (!ppv) return E_POINTER;                                                       \
        static const IID *const iids[] = {&IID_IUnknown, __VA_ARGS__};                    \
        for (const IID *i : iids)                                                         \
            if (IsEqualIID(riid, *i)) {                                                   \
                *ppv = this;                                                              \
                AddRef();                                                                 \
                return S_OK;                                                              \
            }                                                                             \
        *ppv = nullptr;                                                                   \
        return E_NOINTERFACE;                                                             \
    }

struct Counted {
    Counted() { ++g_objects; }
    ~Counted() { --g_objects; }
};

class DiaSession;
HRESULT newSymbol(DiaSession *s, SymIndexId id, IDiaSymbol **out);
HRESULT newMemberSymbol(DiaSession *s, SymIndexId id, IDiaSymbol **out);
HRESULT newViewSymbol(DiaSession *s, SymIndexId id, SymIndexId classParent, IDiaSymbol **out);

// ------------------------------------------------------------ enumerators

// IDiaEnumSymbols over a fixed list.
class DiaEnumSymbols : public Stub_IDiaEnumSymbols<IDiaEnumSymbols>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaEnumSymbols)
    DiaEnumSymbols(DiaSession *s, std::vector<SymIndexId> ids, LONG reportedCount = -1);
    std::vector<uint8_t> memberFlags;  // reached through a class: see DiaSymbol::firstName
    std::vector<SymIndexId> views;     // class parent reported per entry, see Model::viewsOf
    HRESULT make(size_t i, IDiaSymbol **out)
    {
        if (i < views.size() && views[i]) return newViewSymbol(s, ids[i], views[i], out);
        return i < memberFlags.size() && memberFlags[i] ? newMemberSymbol(s, ids[i], out) : newSymbol(s, ids[i], out);
    }
    ~DiaEnumSymbols();
    DiaSession *s;
    std::vector<SymIndexId> ids;
    LONG reported;
    size_t pos = 0;
    HRESULT STDMETHODCALLTYPE get_count(LONG *p) override
    {
        if (!p) return E_POINTER;
        *p = reported >= 0 ? reported : (LONG)ids.size();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Item(ULONG i, IDiaSymbol **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (i >= ids.size()) return E_INVALIDARG;
        return make(i, out);
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG celt, IDiaSymbol **rgelt, ULONG *fetched) override
    {
        if (!rgelt) return E_POINTER;
        ULONG n = 0;
        while (n < celt && pos < ids.size()) {
            if (make(pos++, &rgelt[n]) == S_OK) n++;
        }
        if (fetched) *fetched = n;
        return n == celt ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override
    {
        pos += celt;
        if (pos > ids.size()) {
            pos = ids.size();
            return S_FALSE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override
    {
        pos = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IDiaEnumSymbols **out) override;
};

class DiaLineNumber : public Stub_IDiaLineNumber<IDiaLineNumber>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaLineNumber)
    DiaLineNumber(DiaSession *s, const LineEntry &e);
    ~DiaLineNumber();
    DiaSession *s;
    LineEntry e;
    HRESULT STDMETHODCALLTYPE get_compiland(IDiaSymbol **p) override;
    HRESULT STDMETHODCALLTYPE get_sourceFile(IDiaSourceFile **p) override;
    HRESULT STDMETHODCALLTYPE get_lineNumber(ULONG *p) override { return put(p, e.line); }
    HRESULT STDMETHODCALLTYPE get_lineNumberEnd(ULONG *p) override { return put(p, e.lineEnd); }
    HRESULT STDMETHODCALLTYPE get_columnNumber(ULONG *p) override { return put(p, e.col); }
    HRESULT STDMETHODCALLTYPE get_columnNumberEnd(ULONG *p) override { return put(p, e.colEnd); }
    HRESULT STDMETHODCALLTYPE get_addressSection(ULONG *p) override { return put(p, e.sect); }
    HRESULT STDMETHODCALLTYPE get_addressOffset(ULONG *p) override { return put(p, e.off); }
    HRESULT STDMETHODCALLTYPE get_relativeVirtualAddress(ULONG *p) override;
    HRESULT STDMETHODCALLTYPE get_virtualAddress(ULONGLONG *p) override;
    HRESULT STDMETHODCALLTYPE get_length(ULONG *p) override { return put(p, e.len); }
    HRESULT STDMETHODCALLTYPE get_sourceFileId(ULONG *p) override { return put(p, e.file + 1); }
    HRESULT STDMETHODCALLTYPE get_statement(LONG *p) override { return put((ULONG *)p, e.statement ? 1 : 0); }
    HRESULT STDMETHODCALLTYPE get_compilandId(ULONG *p) override;
    static HRESULT put(ULONG *p, ULONG v)
    {
        if (!p) return E_POINTER;
        *p = v;
        return S_OK;
    }
};

class DiaEnumLineNumbers : public Stub_IDiaEnumLineNumbers<IDiaEnumLineNumbers>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaEnumLineNumbers)
    DiaEnumLineNumbers(DiaSession *s, std::vector<LineEntry> l);
    ~DiaEnumLineNumbers();
    DiaSession *s;
    std::vector<LineEntry> l;
    size_t pos = 0;
    HRESULT STDMETHODCALLTYPE get_count(LONG *p) override
    {
        if (!p) return E_POINTER;
        *p = (LONG)l.size();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Item(ULONG i, IDiaLineNumber **out) override
    {
        if (!out) return E_POINTER;
        if (i >= l.size()) return E_INVALIDARG;
        *out = new DiaLineNumber(s, l[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG celt, IDiaLineNumber **rgelt, ULONG *fetched) override
    {
        if (!rgelt) return E_POINTER;
        ULONG n = 0;
        while (n < celt && pos < l.size()) rgelt[n++] = new DiaLineNumber(s, l[pos++]);
        if (fetched) *fetched = n;
        return n == celt ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override
    {
        pos = std::min(l.size(), pos + celt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override
    {
        pos = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IDiaEnumLineNumbers **out) override
    {
        if (!out) return E_POINTER;
        auto *c = new DiaEnumLineNumbers(s, l);
        c->pos = pos;
        *out = c;
        return S_OK;
    }
};

class DiaSourceFile : public Stub_IDiaSourceFile<IDiaSourceFile>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaSourceFile)
    DiaSourceFile(DiaSession *s, uint32_t idx);
    ~DiaSourceFile();
    DiaSession *s;
    uint32_t idx;
    HRESULT STDMETHODCALLTYPE get_uniqueId(ULONG *p) override
    {
        if (!p) return E_POINTER;
        *p = idx + 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_fileName(BSTR *p) override;
    HRESULT STDMETHODCALLTYPE get_checksumType(ULONG *p) override;
    HRESULT STDMETHODCALLTYPE get_compilands(IDiaEnumSymbols **p) override;
    HRESULT STDMETHODCALLTYPE get_checksum(ULONG cb, ULONG *pcb, BYTE *data) override;
};

class DiaEnumSourceFiles : public Stub_IDiaEnumSourceFiles<IDiaEnumSourceFiles>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaEnumSourceFiles)
    DiaEnumSourceFiles(DiaSession *s, std::vector<uint32_t> f);
    ~DiaEnumSourceFiles();
    DiaSession *s;
    std::vector<uint32_t> f;
    size_t pos = 0;
    HRESULT STDMETHODCALLTYPE get_count(LONG *p) override
    {
        if (!p) return E_POINTER;
        *p = (LONG)f.size();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Item(ULONG i, IDiaSourceFile **out) override
    {
        if (!out) return E_POINTER;
        if (i >= f.size()) return E_INVALIDARG;
        *out = new DiaSourceFile(s, f[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG celt, IDiaSourceFile **rgelt, ULONG *fetched) override
    {
        if (!rgelt) return E_POINTER;
        ULONG n = 0;
        while (n < celt && pos < f.size()) rgelt[n++] = new DiaSourceFile(s, f[pos++]);
        if (fetched) *fetched = n;
        return n == celt ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override
    {
        pos = std::min(f.size(), pos + celt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override
    {
        pos = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IDiaEnumSourceFiles **out) override
    {
        if (!out) return E_POINTER;
        auto *c = new DiaEnumSourceFiles(s, f);
        c->pos = pos;
        *out = c;
        return S_OK;
    }
};

class DiaSectionContrib : public Stub_IDiaSectionContrib<IDiaSectionContrib>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaSectionContrib)
    DiaSectionContrib(DiaSession *s, const SectionContrib &c);
    ~DiaSectionContrib();
    DiaSession *s;
    SectionContrib c;
    HRESULT flag(LONG *p, uint32_t f)
    {
        if (!p) return E_POINTER;
        *p = (c.characteristics & f) ? 1 : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_compiland(IDiaSymbol **p) override;
    HRESULT STDMETHODCALLTYPE get_addressSection(ULONG *p) override { return DiaLineNumber::put(p, c.sect); }
    HRESULT STDMETHODCALLTYPE get_addressOffset(ULONG *p) override { return DiaLineNumber::put(p, c.off); }
    HRESULT STDMETHODCALLTYPE get_relativeVirtualAddress(ULONG *p) override;
    HRESULT STDMETHODCALLTYPE get_virtualAddress(ULONGLONG *p) override;
    HRESULT STDMETHODCALLTYPE get_length(ULONG *p) override { return DiaLineNumber::put(p, c.size); }
    HRESULT STDMETHODCALLTYPE get_notPaged(LONG *p) override { return flag(p, 0x08000000); }
    HRESULT STDMETHODCALLTYPE get_code(LONG *p) override { return flag(p, 0x00000020); }
    HRESULT STDMETHODCALLTYPE get_initializedData(LONG *p) override { return flag(p, 0x00000040); }
    HRESULT STDMETHODCALLTYPE get_uninitializedData(LONG *p) override { return flag(p, 0x00000080); }
    HRESULT STDMETHODCALLTYPE get_remove(LONG *p) override { return flag(p, 0x00000800); }
    HRESULT STDMETHODCALLTYPE get_comdat(LONG *p) override { return flag(p, 0x00001000); }
    HRESULT STDMETHODCALLTYPE get_discardable(LONG *p) override { return flag(p, 0x02000000); }
    HRESULT STDMETHODCALLTYPE get_notCached(LONG *p) override { return flag(p, 0x04000000); }
    HRESULT STDMETHODCALLTYPE get_share(LONG *p) override { return flag(p, 0x10000000); }
    HRESULT STDMETHODCALLTYPE get_execute(LONG *p) override { return flag(p, 0x20000000); }
    HRESULT STDMETHODCALLTYPE get_read(LONG *p) override { return flag(p, 0x40000000); }
    HRESULT STDMETHODCALLTYPE get_write(LONG *p) override { return flag(p, 0x80000000); }
    HRESULT STDMETHODCALLTYPE get_dataCrc(ULONG *p) override { return DiaLineNumber::put(p, c.dataCrc); }
    HRESULT STDMETHODCALLTYPE get_relocationsCrc(ULONG *p) override { return DiaLineNumber::put(p, c.relocCrc); }
    HRESULT STDMETHODCALLTYPE get_compilandId(ULONG *p) override;
    HRESULT STDMETHODCALLTYPE get_code16bit(LONG *p) override { return flag(p, 0x00020000); }
};

// A table: IDiaTable plus, optionally, the matching enumerator interface.
// Rows are produced by `item`.
class DiaTable : public Stub_IDiaTable<IDiaTable>, Counted {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        long r = --refs_;
        if (!r) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDiaTable) || IsEqualIID(riid, IID_IEnumUnknown)) {
            *ppv = static_cast<IDiaTable *>(this);
        } else if (inner && IsEqualIID(riid, innerIid)) {
            // The rows' enumerator; a separate object (DIA tables are
            // queried for it and used through it).
            return inner->QueryInterface(riid, ppv);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    std::atomic<long> refs_{1};
    DiaTable(DiaSession *s, std::string name, LONG count, IUnknown *inner, IID innerIid);
    ~DiaTable();
    DiaSession *s;
    std::string name;
    LONG count;
    IUnknown *inner;  // the enumerator, delegating its IUnknown to us
    IID innerIid;
    size_t pos = 0;
    HRESULT STDMETHODCALLTYPE get_name(BSTR *p) override
    {
        if (!p) return E_POINTER;
        *p = toBSTR(name);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_count(LONG *p) override
    {
        if (!p) return E_POINTER;
        *p = count;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override
    {
        pos = 0;
        return S_OK;
    }
};

class DiaEnumTables : public Stub_IDiaEnumTables<IDiaEnumTables>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaEnumTables)
    DiaEnumTables(DiaSession *s);
    ~DiaEnumTables();
    DiaSession *s;
    size_t pos = 0;
    static constexpr int N = 8;
    HRESULT make(size_t i, IDiaTable **out);
    HRESULT STDMETHODCALLTYPE get_count(LONG *p) override
    {
        if (!p) return E_POINTER;
        *p = N;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG celt, IDiaTable **rgelt, ULONG *fetched) override
    {
        if (!rgelt) return E_POINTER;
        ULONG n = 0;
        while (n < celt && pos < (size_t)N)
            if (make(pos++, &rgelt[n]) == S_OK) n++;
        if (fetched) *fetched = n;
        return n == celt ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override
    {
        pos = std::min<size_t>(N, pos + celt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override
    {
        pos = 0;
        return S_OK;
    }
};

class DiaEnumDebugStreamData : public Stub_IDiaEnumDebugStreamData<IDiaEnumDebugStreamData>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaEnumDebugStreamData)
    explicit DiaEnumDebugStreamData(DebugStream d) : d(std::move(d)) {}
    DebugStream d;
    size_t pos = 0;
    size_t count() const { return d.data.size() / d.recordSize; }
    HRESULT STDMETHODCALLTYPE get_count(LONG *p) override
    {
        if (!p) return E_POINTER;
        *p = (LONG)count();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_name(BSTR *p) override
    {
        if (!p) return E_POINTER;
        *p = toBSTR(d.name);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Item(ULONG i, ULONG cb, ULONG *pcb, BYTE *data) override
    {
        if (i >= count()) return E_INVALIDARG;
        if (pcb) *pcb = d.recordSize;
        if (data) {
            if (cb < d.recordSize) return E_INVALIDARG;
            memcpy(data, d.data.data() + (size_t)i * d.recordSize, d.recordSize);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG celt, ULONG cb, ULONG *pcb, BYTE *data, ULONG *fetched) override
    {
        ULONG n = 0;
        if (celt && pos < count()) {
            HRESULT hr = Item((ULONG)pos, cb, pcb, data);
            if (hr != S_OK) return hr;
            pos++;
            n = 1;
        }
        if (fetched) *fetched = n;
        return n == celt ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override
    {
        pos = std::min(count(), pos + celt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override
    {
        pos = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IDiaEnumDebugStreamData **out) override
    {
        if (!out) return E_POINTER;
        auto *c = new DiaEnumDebugStreamData(d);
        c->pos = pos;
        *out = c;
        return S_OK;
    }
};

class DiaEnumDebugStreams : public Stub_IDiaEnumDebugStreams<IDiaEnumDebugStreams>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaEnumDebugStreams)
    explicit DiaEnumDebugStreams(std::vector<DebugStream> s) : s(std::move(s)) {}
    std::vector<DebugStream> s;
    size_t pos = 0;
    HRESULT STDMETHODCALLTYPE get_count(LONG *p) override
    {
        if (!p) return E_POINTER;
        *p = (LONG)s.size();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Item(VARIANT index, IDiaEnumDebugStreamData **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (index.vt == VT_BSTR) {
            std::string n = fromW(index.bstrVal);
            for (auto &d : s)
                if (d.name == n) {
                    *out = new DiaEnumDebugStreamData(d);
                    return S_OK;
                }
            return E_INVALIDARG;
        }
        VARIANT v;
        VariantInit(&v);
        if (FAILED(VariantChangeType(&v, &index, 0, VT_UI4)) || v.ulVal >= s.size()) return E_INVALIDARG;
        *out = new DiaEnumDebugStreamData(s[v.ulVal]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG celt, IDiaEnumDebugStreamData **rgelt, ULONG *fetched) override
    {
        if (!rgelt) return E_POINTER;
        ULONG n = 0;
        while (n < celt && pos < s.size()) rgelt[n++] = new DiaEnumDebugStreamData(s[pos++]);
        if (fetched) *fetched = n;
        return n == celt ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override
    {
        pos = std::min(s.size(), pos + celt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override
    {
        pos = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IDiaEnumDebugStreams **out) override
    {
        if (!out) return E_POINTER;
        auto *c = new DiaEnumDebugStreams(s);
        c->pos = pos;
        *out = c;
        return S_OK;
    }
};

class DiaEnumSymbolsByAddr : public Stub_IDiaEnumSymbolsByAddr<IDiaEnumSymbolsByAddr>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaEnumSymbolsByAddr)
    DiaEnumSymbolsByAddr(DiaSession *s);
    ~DiaEnumSymbolsByAddr();
    DiaSession *s;
    std::vector<SymIndexId> ids;
    size_t pos = 0;
    HRESULT at(size_t i, IDiaSymbol **out);
    HRESULT STDMETHODCALLTYPE symbolByAddr(ULONG sect, ULONG off, IDiaSymbol **out) override;
    HRESULT STDMETHODCALLTYPE symbolByRVA(ULONG rva, IDiaSymbol **out) override;
    HRESULT STDMETHODCALLTYPE symbolByVA(ULONGLONG va, IDiaSymbol **out) override;
    HRESULT STDMETHODCALLTYPE Next(ULONG celt, IDiaSymbol **rgelt, ULONG *fetched) override
    {
        if (!rgelt) return E_POINTER;
        ULONG n = 0;
        while (n < celt && pos < ids.size())
            if (at(pos++, &rgelt[n]) == S_OK) n++;
        if (fetched) *fetched = n;
        return n == celt ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Prev(ULONG celt, IDiaSymbol **rgelt, ULONG *fetched) override
    {
        if (!rgelt) return E_POINTER;
        ULONG n = 0;
        while (n < celt && pos > 1)
            if (at(--pos - 1, &rgelt[n]) == S_OK) n++;
        if (fetched) *fetched = n;
        return n == celt ? S_OK : S_FALSE;
    }
};

// ------------------------------------------------------------ session

class DiaSession : public Stub_IDiaSession<IDiaSession>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaSession)
    explicit DiaSession(std::shared_ptr<Model> m) : m(std::move(m)) {}
    std::shared_ptr<Model> m;

    HRESULT STDMETHODCALLTYPE get_loadAddress(ULONGLONG *p) override
    {
        if (!p) return E_POINTER;
        *p = m->loadAddress;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_loadAddress(ULONGLONG v) override
    {
        m->loadAddress = v;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_globalScope(IDiaSymbol **p) override
    {
        if (!p) return E_POINTER;
        return newSymbol(this, m->exe, p);
    }
    HRESULT STDMETHODCALLTYPE getEnumTables(IDiaEnumTables **p) override
    {
        if (!p) return E_POINTER;
        *p = new DiaEnumTables(this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE getSymbolsByAddr(IDiaEnumSymbolsByAddr **p) override
    {
        if (!p) return E_POINTER;
        *p = new DiaEnumSymbolsByAddr(this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE findChildren(IDiaSymbol *parent, enum SymTagEnum tag, LPWSTR name, ULONG flags,
                                           IDiaEnumSymbols **out) override
    {
        if (!parent) return E_INVALIDARG;
        return parent->findChildren(tag, name, flags, out);
    }
    HRESULT STDMETHODCALLTYPE findChildrenEx(IDiaSymbol *parent, enum SymTagEnum tag, LPWSTR name, ULONG flags,
                                             IDiaEnumSymbols **out) override
    {
        return findChildren(parent, tag, name, flags, out);
    }
    HRESULT find(uint32_t rva, enum SymTagEnum tag, IDiaSymbol **out, LONG *disp)
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        int32_t d = 0;
        SymIndexId id = m->findByRVA(rva, (SymTag)tag, &d);
        if (disp) *disp = d;
        if (!id) return S_FALSE;
        return newSymbol(this, id, out);
    }
    HRESULT STDMETHODCALLTYPE findSymbolByAddr(ULONG sect, ULONG off, enum SymTagEnum tag, IDiaSymbol **out) override
    {
        return find(m->rva(sect, off), tag, out, nullptr);
    }
    // RVA lookups fail with E_UNEXPECTED in the image's last section of
    // lld-linked PDBs (as observed with 14.51; see PdbFile::inLastSection).
    HRESULT findRVA(uint32_t rva, enum SymTagEnum tag, IDiaSymbol **out, LONG *disp)
    {
        if (out) *out = nullptr;
        if (m->inLastSection(rva)) return E_UNEXPECTED;
        return find(rva, tag, out, disp);
    }
    HRESULT STDMETHODCALLTYPE findSymbolByRVA(ULONG rva, enum SymTagEnum tag, IDiaSymbol **out) override
    {
        return findRVA(rva, tag, out, nullptr);
    }
    HRESULT STDMETHODCALLTYPE findSymbolByVA(ULONGLONG va, enum SymTagEnum tag, IDiaSymbol **out) override
    {
        return findRVA((uint32_t)(va - m->loadAddress), tag, out, nullptr);
    }
    HRESULT STDMETHODCALLTYPE findSymbolByRVAEx(ULONG rva, enum SymTagEnum tag, IDiaSymbol **out, LONG *disp) override
    {
        return findRVA(rva, tag, out, disp);
    }
    HRESULT STDMETHODCALLTYPE findSymbolByVAEx(ULONGLONG va, enum SymTagEnum tag, IDiaSymbol **out, LONG *disp) override
    {
        return findRVA((uint32_t)(va - m->loadAddress), tag, out, disp);
    }
    HRESULT STDMETHODCALLTYPE symbolById(ULONG id, IDiaSymbol **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!m->valid(id)) return E_INVALIDARG;
        return newSymbol(this, id, out);
    }
    HRESULT STDMETHODCALLTYPE symsAreEquiv(IDiaSymbol *a, IDiaSymbol *b) override
    {
        if (!a || !b) return E_INVALIDARG;
        ULONG x = 0, y = 0;
        a->get_symIndexId(&x);
        b->get_symIndexId(&y);
        return x == y ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE addressForRVA(ULONG rva, ULONG *sect, ULONG *off) override
    {
        if (!sect || !off) return E_POINTER;
        if (m->inLastSection(rva)) return E_UNEXPECTED;
        uint32_t s = 0, o = 0;
        if (!m->sectOff(rva, s, o)) return S_FALSE;
        *sect = s;
        *off = o;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE addressForVA(ULONGLONG va, ULONG *sect, ULONG *off) override
    {
        return addressForRVA((ULONG)(va - m->loadAddress), sect, off);
    }
    HRESULT STDMETHODCALLTYPE findFile(IDiaSymbol *compiland, LPWSTR name, ULONG flags, IDiaEnumSourceFiles **out) override
    {
        if (!out) return E_POINTER;
        std::vector<uint32_t> f;
        if (compiland) {
            ULONG id = 0;
            compiland->get_symIndexId(&id);
            int modi = m->moduleOf(id);
            if (modi >= 0) f = m->filesOfModule((uint16_t)modi);
        } else {
            for (uint32_t i = 0; i < m->files().size(); i++) f.push_back(i);
        }
        if (name) {
            std::string want = fromW(name);
            std::vector<uint32_t> g;
            for (uint32_t i : f) {
                const std::string &n = m->files()[i].name;
                bool eq = (flags & 2) ? _stricmp(n.c_str(), want.c_str()) == 0 : n == want;
                if (eq) g.push_back(i);
            }
            f = g;
        }
        *out = new DiaEnumSourceFiles(this, f);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE findFileById(ULONG id, IDiaSourceFile **out) override
    {
        if (!out) return E_POINTER;
        if (id == 0 || id > m->files().size()) return E_INVALIDARG;
        *out = new DiaSourceFile(this, id - 1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE findLines(IDiaSymbol *compiland, IDiaSourceFile *file, IDiaEnumLineNumbers **out) override
    {
        if (!out || !compiland) return E_INVALIDARG;
        ULONG id = 0;
        compiland->get_symIndexId(&id);
        int modi = m->moduleOf(id);
        std::vector<LineEntry> l;
        ULONG fid = 0;
        if (file) file->get_uniqueId(&fid);
        if (modi >= 0)
            for (auto &e : m->lines((uint16_t)modi))
                if (!file || e.file + 1 == fid) l.push_back(e);
        *out = new DiaEnumLineNumbers(this, l);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE findLinesByRVA(ULONG rva, ULONG len, IDiaEnumLineNumbers **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (m->inLastSection(rva)) return E_UNEXPECTED;
        *out = new DiaEnumLineNumbers(this, m->linesByRVA(rva, len));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE findLinesByAddr(ULONG sect, ULONG off, ULONG len, IDiaEnumLineNumbers **out) override
    {
        return findLinesByRVA(m->rva(sect, off), len, out);
    }
    HRESULT STDMETHODCALLTYPE findLinesByVA(ULONGLONG va, ULONG len, IDiaEnumLineNumbers **out) override
    {
        return findLinesByRVA((ULONG)(va - m->loadAddress), len, out);
    }
    HRESULT STDMETHODCALLTYPE findLinesByLinenum(IDiaSymbol *compiland, IDiaSourceFile *file, ULONG line, ULONG,
                                                 IDiaEnumLineNumbers **out) override
    {
        if (!out || !compiland || !file) return E_INVALIDARG;
        ULONG id = 0, fid = 0;
        compiland->get_symIndexId(&id);
        file->get_uniqueId(&fid);
        int modi = m->moduleOf(id);
        std::vector<LineEntry> l;
        if (modi >= 0) {
            // The first line at or after `line` in that file.
            uint32_t best = UINT32_MAX;
            for (auto &e : m->lines((uint16_t)modi))
                if (e.file + 1 == fid && e.line >= line) best = std::min(best, e.line);
            for (auto &e : m->lines((uint16_t)modi))
                if (e.file + 1 == fid && e.line == best) l.push_back(e);
        }
        *out = new DiaEnumLineNumbers(this, l);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE getEnumDebugStreams(IDiaEnumDebugStreams **p) override
    {
        if (!p) return E_POINTER;
        *p = new DiaEnumDebugStreams(m->debugStreams());
        return S_OK;
    }
};

// ------------------------------------------------------------ symbol

class DiaSymbol : public Stub_IDiaSymbol11<IDiaSymbol11>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaSymbol, &IID_IDiaSymbol2, &IID_IDiaSymbol3, &IID_IDiaSymbol4, &IID_IDiaSymbol5,
                &IID_IDiaSymbol6, &IID_IDiaSymbol7, &IID_IDiaSymbol8, &IID_IDiaSymbol9, &IID_IDiaSymbol10,
                &IID_IDiaSymbol11)
    DiaSymbol(DiaSession *s, SymIndexId id) : s(s), id(id) { s->AddRef(); }
    ~DiaSymbol() { s->Release(); }
    DiaSession *s;
    SymIndexId id;
    // A class method reached through its class answers its first get_name
    // with the unqualified method name, later ones with the function's
    // qualified name (as observed).
    std::string firstName;
    bool firstNameUsed = true;
    bool memberView = false;  // a method reached through its class: lexical parent is the exe
    SymIndexId viewClassParent = 0;  // class parent of this enumerated instance, see Model::viewsOf

    HRESULT getLONG(Prop p, LONG *out)
    {
        if (!out) return E_POINTER;
        if (memberView && (p == P_constType || p == P_volatileType || p == P_unalignedType || p == P_constantExport) &&
            s->m->methodOverloaded(id)) {
            // 14.51: overloaded methods seen through their class.
            *out = 0;
            return S_OK;
        }
        Val v;
        if (!s->m->get(id, p, v)) return S_FALSE;
        if (v.kind == Val::Err) return (HRESULT)v.u;
        *out = v.kind == Val::I ? (LONG)v.i : (LONG)v.u;
        return S_OK;
    }
    HRESULT getULONG(Prop p, ULONG *out)
    {
        if (!out) return E_POINTER;
        if (memberView && p == P_lexicalParentId) {
            *out = s->m->exe;
            return S_OK;
        }
        if (viewClassParent && p == P_classParentId) {
            *out = viewClassParent;
            return S_OK;
        }
        Val v;
        if (!s->m->get(id, p, v)) return S_FALSE;
        if (v.kind == Val::Err) return (HRESULT)v.u;
        *out = v.kind == Val::I ? (ULONG)v.i : (ULONG)v.u;
        return S_OK;
    }
    HRESULT getULONGLONG(Prop p, ULONGLONG *out)
    {
        if (!out) return E_POINTER;
        Val v;
        if (!s->m->get(id, p, v)) return S_FALSE;
        *out = v.kind == Val::I ? (ULONGLONG)v.i : v.u;
        return S_OK;
    }
    HRESULT getBSTR(Prop p, BSTR *out)
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (p == P_unused) return E_NOTIMPL;  // as observed
        if (p == P_name && !firstNameUsed) {
            firstNameUsed = true;
            *out = toBSTR(firstName);
            return S_OK;
        }
        Val v;
        if (!s->m->get(id, p, v) || v.kind != Val::Str) return S_FALSE;
        *out = toBSTR(v.s);
        return S_OK;
    }
    HRESULT getGUID(Prop p, GUID *out)
    {
        if (!out) return E_POINTER;
        Val v;
        if (!s->m->get(id, p, v) || v.kind != Val::Guid) return S_FALSE;
        memcpy(out, v.g, 16);
        return S_OK;
    }
    HRESULT getVARIANT(Prop p, VARIANT *out)
    {
        if (!out) return E_POINTER;
        VariantInit(out);
        Val v;
        if (!s->m->get(id, p, v) || v.kind != Val::Var) return S_FALSE;
        if (v.strVar) {
            out->vt = VT_BSTR;
            out->bstrVal = toBSTR(v.s);
            return S_OK;
        }
        switch (v.var.type) {
        case Variant::I1: out->vt = VT_I1; out->cVal = (CHAR)v.var.i; break;
        case Variant::I2: out->vt = VT_I2; out->iVal = (SHORT)v.var.i; break;
        case Variant::I4: out->vt = VT_I4; out->lVal = (LONG)v.var.i; break;
        case Variant::I8: out->vt = VT_I8; out->llVal = v.var.i; break;
        case Variant::UI1: out->vt = VT_UI1; out->bVal = (BYTE)v.var.u; break;
        case Variant::UI2: out->vt = VT_UI2; out->uiVal = (USHORT)v.var.u; break;
        case Variant::UI4: out->vt = VT_UI4; out->ulVal = (ULONG)v.var.u; break;
        case Variant::UI8: out->vt = VT_UI8; out->ullVal = v.var.u; break;
        case Variant::R4: out->vt = VT_R4; out->fltVal = (float)v.var.d; break;
        case Variant::R8: out->vt = VT_R8; out->dblVal = v.var.d; break;
        case Variant::Bool: out->vt = VT_BOOL; out->boolVal = v.var.u ? VARIANT_TRUE : VARIANT_FALSE; break;
        case Variant::Str: out->vt = VT_BSTR; out->bstrVal = toBSTR(v.s); break;
        default: return S_FALSE;
        }
        return S_OK;
    }
    HRESULT getSYM(Prop p, IDiaSymbol **out)
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (memberView && p == P_lexicalParent) return newSymbol(s, s->m->exe, out);
        if (viewClassParent && p == P_classParent) return newSymbol(s, viewClassParent, out);
        Val v;
        if (!s->m->get(id, p, v)) return S_FALSE;
        if (v.kind == Val::Err) return (HRESULT)v.u;
        if (v.kind != Val::Sym || !v.u) return S_FALSE;
        return newSymbol(s, (SymIndexId)v.u, out);
    }

#define OM_GETTER(n, k) \
    HRESULT STDMETHODCALLTYPE get_##n(OM_T_##k *out) override { return get##k(P_##n, out); }
#define OM_T_LONG LONG
#define OM_T_ULONG ULONG
#define OM_T_ULONGLONG ULONGLONG
#define OM_T_BSTR BSTR
#define OM_T_GUID GUID
#define OM_T_VARIANT VARIANT
#define OM_T_SYM IDiaSymbol *
#define getSYM_T getSYM
    OPENMSDIA_SYMBOL_GETTERS(OM_GETTER)
#undef OM_GETTER

    HRESULT STDMETHODCALLTYPE findChildren(enum SymTagEnum tag, LPWSTR name, ULONG flags, IDiaEnumSymbols **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        bool none = false;
        const auto &all = s->m->children(id, &none);
        if (none) return S_FALSE;
        std::vector<SymIndexId> pick;
        std::vector<uint8_t> pickFlags;
        std::vector<SymIndexId> pickViews;
        std::vector<uint8_t> flags_ = s->m->flagsOf(id);
        std::vector<SymIndexId> views_ = s->m->viewsOf(id);
        std::string want = name ? fromW(name) : "";
        for (size_t ci = 0; ci < all.size(); ci++) {
            SymIndexId c = all[ci];
            if (tag != 0 && (uint32_t)s->m->tag(c) != (uint32_t)tag) continue;
            if (name) {
                std::string n = s->m->name(c);
                bool ci = (flags & 2) != 0;  // nsCaseInsensitive
                if (ci ? _stricmp(n.c_str(), want.c_str()) != 0 : n != want) continue;
            }
            pick.push_back(c);
            pickFlags.push_back(flags_[ci]);
            pickViews.push_back(views_[ci]);
        }
        LONG reported = -1;
        SymTag t = s->m->tag(id);
        if (!name && tag == 0 && t == SymTag::Callee) {
            // A callee reports its caller's compiland's child count, except
            // the first one of its S_CALLEES record (0), and enumerates
            // nothing (as observed).
            SymIndexId c = id;
            Val v;
            for (int i = 0; i < 8 && s->m->tag(c) != SymTag::Compiland && s->m->get(c, P_lexicalParent, v); i++)
                c = (SymIndexId)v.u;
            if (s->m->tag(c) == SymTag::Compiland)
                reported = s->m->calleeIndex(id) == 0 ? 0 : (LONG)s->m->children(c).size();
        }
        if (!name && tag == 0 &&
            (t == SymTag::FuncDebugStart || t == SymTag::FuncDebugEnd || t == SymTag::CompilandEnv)) {
            // The real DLL reports the enclosing compiland's child count for
            // these leaf symbols (except the "cwd" and "obj" environment
            // entries) but enumerates nothing.
            SymIndexId c = id;
            Val v;
            for (int i = 0; i < 8 && s->m->tag(c) != SymTag::Compiland && s->m->get(c, P_lexicalParent, v); i++)
                c = (SymIndexId)v.u;
            std::string n = s->m->name(id);
            if (s->m->tag(c) == SymTag::Compiland)
                reported = (t == SymTag::CompilandEnv && (n == "cwd" || n == "obj"))
                               ? 0 : (LONG)s->m->children(c).size();
        }
        auto *e = new DiaEnumSymbols(s, pick, reported);
        e->memberFlags = pickFlags;
        e->views = pickViews;
        *out = e;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE findChildrenEx(enum SymTagEnum tag, LPWSTR name, ULONG flags, IDiaEnumSymbols **out) override
    {
        return findChildren(tag, name, flags, out);
    }
    HRESULT STDMETHODCALLTYPE get_undecoratedNameEx(ULONG options, BSTR *out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (options == 0) {
            Val v;
            if (s->m->get(id, P_undecoratedNameEx0, v) && v.kind == Val::Str) {
                *out = toBSTR(v.s);
                return S_OK;
            }
        }
        return getBSTR(P_undecoratedName, out);
    }
    HRESULT STDMETHODCALLTYPE get_types(ULONG, ULONG *pc, IDiaSymbol **) override
    {
        if (pc) *pc = 0;
        return S_FALSE;
    }
};

HRESULT newSymbol(DiaSession *s, SymIndexId id, IDiaSymbol **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!s->m->valid(id)) return S_FALSE;
    *out = new DiaSymbol(s, id);
    return S_OK;
}

HRESULT newMemberSymbol(DiaSession *s, SymIndexId id, IDiaSymbol **out)
{
    HRESULT hr = newSymbol(s, id, out);
    if (hr == S_OK) {
        std::string shortName = s->m->methodShortName(id);
        if (!shortName.empty()) {
            auto *d = static_cast<DiaSymbol *>(*out);
            d->firstName = shortName;
            d->firstNameUsed = false;
            d->memberView = true;
        }
    }
    return hr;
}

HRESULT newViewSymbol(DiaSession *s, SymIndexId id, SymIndexId classParent, IDiaSymbol **out)
{
    HRESULT hr = newSymbol(s, id, out);
    if (hr == S_OK) static_cast<DiaSymbol *>(*out)->viewClassParent = classParent;
    return hr;
}

// ------------------------------------------------------------ out-of-line bits

DiaEnumSymbols::DiaEnumSymbols(DiaSession *s, std::vector<SymIndexId> ids, LONG reportedCount)
    : s(s), ids(std::move(ids)), reported(reportedCount)
{
    s->AddRef();
}
DiaEnumSymbols::~DiaEnumSymbols() { s->Release(); }
HRESULT DiaEnumSymbols::Clone(IDiaEnumSymbols **out)
{
    if (!out) return E_POINTER;
    auto *c = new DiaEnumSymbols(s, ids, reported);
    c->pos = pos;
    c->memberFlags = memberFlags;
    c->views = views;
    *out = c;
    return S_OK;
}

DiaLineNumber::DiaLineNumber(DiaSession *s, const LineEntry &e) : s(s), e(e) { s->AddRef(); }
DiaLineNumber::~DiaLineNumber() { s->Release(); }
HRESULT DiaLineNumber::get_compiland(IDiaSymbol **p) { return newSymbol(s, s->m->compiland(e.modi), p); }
HRESULT DiaLineNumber::get_compilandId(ULONG *p) { return put(p, s->m->compiland(e.modi)); }
HRESULT DiaLineNumber::get_sourceFile(IDiaSourceFile **p)
{
    if (!p) return E_POINTER;
    *p = new DiaSourceFile(s, e.file);
    return S_OK;
}
HRESULT DiaLineNumber::get_relativeVirtualAddress(ULONG *p) { return put(p, s->m->rva(e.sect, e.off)); }
HRESULT DiaLineNumber::get_virtualAddress(ULONGLONG *p)
{
    if (!p) return E_POINTER;
    *p = s->m->rva(e.sect, e.off) + s->m->loadAddress;
    return S_OK;
}

DiaEnumLineNumbers::DiaEnumLineNumbers(DiaSession *s, std::vector<LineEntry> l) : s(s), l(std::move(l)) { s->AddRef(); }
DiaEnumLineNumbers::~DiaEnumLineNumbers() { s->Release(); }

DiaSourceFile::DiaSourceFile(DiaSession *s, uint32_t idx) : s(s), idx(idx) { s->AddRef(); }
DiaSourceFile::~DiaSourceFile() { s->Release(); }
HRESULT DiaSourceFile::get_fileName(BSTR *p)
{
    if (!p) return E_POINTER;
    *p = toBSTR(s->m->files()[idx].name);
    return S_OK;
}
// The real 14.30 DLL reports no checksum for the MD5 checksums clang
// writes; we do the same for every kind until a PDB shows otherwise.
static bool reportChecksums = false;

HRESULT DiaSourceFile::get_checksumType(ULONG *p)
{
    if (!p) return E_POINTER;
    const auto &f = s->m->files()[idx];
    if (!f.checksumKind || !reportChecksums) return S_FALSE;
    *p = f.checksumKind;
    return S_OK;
}
HRESULT DiaSourceFile::get_checksum(ULONG cb, ULONG *pcb, BYTE *data)
{
    if (!pcb) return E_POINTER;
    const auto &f = s->m->files()[idx];
    if (!f.checksumKind || !reportChecksums) {
        *pcb = 0;
        return S_FALSE;
    }
    *pcb = (ULONG)f.checksum.size();
    if (data) {
        if (cb < f.checksum.size()) return E_INVALIDARG;
        memcpy(data, f.checksum.data(), f.checksum.size());
    }
    return S_OK;
}
HRESULT DiaSourceFile::get_compilands(IDiaEnumSymbols **p)
{
    if (!p) return E_POINTER;
    std::vector<SymIndexId> ids;
    for (uint16_t modi : s->m->files()[idx].modules) ids.push_back(s->m->compiland(modi));
    *p = new DiaEnumSymbols(s, ids);
    return S_OK;
}

DiaEnumSourceFiles::DiaEnumSourceFiles(DiaSession *s, std::vector<uint32_t> f) : s(s), f(std::move(f)) { s->AddRef(); }
DiaEnumSourceFiles::~DiaEnumSourceFiles() { s->Release(); }

DiaSectionContrib::DiaSectionContrib(DiaSession *s, const SectionContrib &c) : s(s), c(c) { s->AddRef(); }
DiaSectionContrib::~DiaSectionContrib() { s->Release(); }
HRESULT DiaSectionContrib::get_compiland(IDiaSymbol **p) { return newSymbol(s, s->m->compiland(c.modi), p); }
HRESULT DiaSectionContrib::get_compilandId(ULONG *p) { return DiaLineNumber::put(p, s->m->compiland(c.modi)); }
HRESULT DiaSectionContrib::get_relativeVirtualAddress(ULONG *p) { return DiaLineNumber::put(p, s->m->rva(c.sect, c.off)); }
HRESULT DiaSectionContrib::get_virtualAddress(ULONGLONG *p)
{
    if (!p) return E_POINTER;
    *p = s->m->rva(c.sect, c.off) + s->m->loadAddress;
    return S_OK;
}

// Section contributions as an enumerator, aggregated into a DiaTable.
class DiaEnumSectionContribs : public Stub_IDiaEnumSectionContribs<IDiaEnumSectionContribs>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaEnumSectionContribs)
    DiaSession *s;
    size_t pos = 0;
    explicit DiaEnumSectionContribs(DiaSession *s) : s(s) { s->AddRef(); }
    ~DiaEnumSectionContribs() { s->Release(); }
    const std::vector<SectionContrib> &all() { return s->m->sectionContribs(); }
    HRESULT STDMETHODCALLTYPE get_count(LONG *p) override
    {
        if (!p) return E_POINTER;
        *p = (LONG)all().size();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Item(ULONG i, IDiaSectionContrib **out) override
    {
        if (!out) return E_POINTER;
        if (i >= all().size()) return E_INVALIDARG;
        *out = new DiaSectionContrib(s, all()[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG celt, IDiaSectionContrib **rgelt, ULONG *fetched) override
    {
        if (!rgelt) return E_POINTER;
        ULONG n = 0;
        while (n < celt && pos < all().size()) rgelt[n++] = new DiaSectionContrib(s, all()[pos++]);
        if (fetched) *fetched = n;
        return n == celt ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override
    {
        pos = std::min(all().size(), pos + celt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override
    {
        pos = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IDiaEnumSectionContribs **out) override
    {
        if (!out) return E_POINTER;
        auto *c = new DiaEnumSectionContribs(s);
        c->pos = pos;
        *out = c;
        return S_OK;
    }
};

// Empty enumerators for tables this implementation has no rows for.
class DiaEnumInjectedSources : public Stub_IDiaEnumInjectedSources<IDiaEnumInjectedSources>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaEnumInjectedSources)
    HRESULT STDMETHODCALLTYPE get_count(LONG *p) override
    {
        if (!p) return E_POINTER;
        *p = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG, IDiaInjectedSource **, ULONG *fetched) override
    {
        if (fetched) *fetched = 0;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
};

class DiaEnumInputAssemblyFiles : public Stub_IDiaEnumInputAssemblyFiles<IDiaEnumInputAssemblyFiles>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaEnumInputAssemblyFiles)
    HRESULT STDMETHODCALLTYPE get_count(LONG *p) override
    {
        if (!p) return E_POINTER;
        *p = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG, IDiaInputAssemblyFile **, ULONG *fetched) override
    {
        if (fetched) *fetched = 0;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
};

DiaTable::DiaTable(DiaSession *s, std::string name, LONG count, IUnknown *inner, IID innerIid)
    : s(s), name(std::move(name)), count(count), inner(inner), innerIid(innerIid)
{
    s->AddRef();
}
DiaTable::~DiaTable()
{
    if (inner) inner->Release();
    s->Release();
}

DiaEnumTables::DiaEnumTables(DiaSession *s) : s(s) { s->AddRef(); }
DiaEnumTables::~DiaEnumTables() { s->Release(); }

HRESULT DiaEnumTables::make(size_t i, IDiaTable **out)
{
    Model &m = *s->m;
    std::vector<LineEntry> lines;
    switch (i) {
    case 0: {
        auto *e = new DiaEnumSymbols(s, m.children(m.exe));
        e->memberFlags = m.flagsOf(m.exe);
        e->views = m.viewsOf(m.exe);
        *out = new DiaTable(s, "Symbols", (LONG)m.children(m.exe).size(), e, IID_IDiaEnumSymbols);
        return S_OK;
    }
    case 1: {
        std::vector<uint32_t> f;
        for (uint32_t k = 0; k < m.files().size(); k++) f.push_back(k);
        *out = new DiaTable(s, "SourceFiles", (LONG)f.size(), new DiaEnumSourceFiles(s, f), IID_IDiaEnumSourceFiles);
        return S_OK;
    }
    case 2:
        for (uint32_t k = 0, n = m.moduleCount(); k < n; k++)
            for (auto &e : m.lines((uint16_t)k)) lines.push_back(e);
        *out = new DiaTable(s, "LineNumbers", (LONG)lines.size(), new DiaEnumLineNumbers(s, lines),
                            IID_IDiaEnumLineNumbers);
        return S_OK;
    case 3:
        *out = new DiaTable(s, "Sections", (LONG)m.sectionContribs().size(), new DiaEnumSectionContribs(s),
                            IID_IDiaEnumSectionContribs);
        return S_OK;
    case 4: *out = new DiaTable(s, "SegmentMap", (LONG)m.segmentCount(), nullptr, IID_IUnknown); return S_OK;
    case 5:
        *out = new DiaTable(s, "InjectedSource", 0, new DiaEnumInjectedSources(), IID_IDiaEnumInjectedSources);
        return S_OK;
    case 6: *out = new DiaTable(s, "FrameData", 0, nullptr, IID_IUnknown); return S_OK;
    case 7:
        *out = new DiaTable(s, "InputAssemblyFiles", 0, new DiaEnumInputAssemblyFiles(), IID_IDiaEnumInputAssemblyFiles);
        return S_OK;
    }
    return E_INVALIDARG;
}

DiaEnumSymbolsByAddr::DiaEnumSymbolsByAddr(DiaSession *s) : s(s)
{
    s->AddRef();
    ids = s->m->symbolsByAddr();
}
DiaEnumSymbolsByAddr::~DiaEnumSymbolsByAddr() { s->Release(); }
HRESULT DiaEnumSymbolsByAddr::at(size_t i, IDiaSymbol **out) { return newSymbol(s, ids[i], out); }
HRESULT DiaEnumSymbolsByAddr::symbolByRVA(ULONG rva, IDiaSymbol **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    // Position at the first symbol at or after rva.
    for (size_t i = 0; i < ids.size(); i++) {
        Val v;
        if (s->m->get(ids[i], P_relativeVirtualAddress, v) && v.u >= rva) {
            pos = i + 1;
            return at(i, out);
        }
    }
    return S_FALSE;
}
HRESULT DiaEnumSymbolsByAddr::symbolByAddr(ULONG sect, ULONG off, IDiaSymbol **out)
{
    return symbolByRVA(s->m->rva(sect, off), out);
}
HRESULT DiaEnumSymbolsByAddr::symbolByVA(ULONGLONG va, IDiaSymbol **out)
{
    return symbolByRVA((ULONG)(va - s->m->loadAddress), out);
}

// ------------------------------------------------------------ data source

class DiaDataSource : public Stub_IDiaDataSourceEx<IDiaDataSourceEx>, Counted {
public:
    OM_IUNKNOWN(&IID_IDiaDataSource, &IID_IDiaDataSourceEx)
    std::shared_ptr<Model> m;
    std::string err;

    HRESULT STDMETHODCALLTYPE get_lastError(BSTR *p) override
    {
        if (!p) return E_POINTER;
        *p = toBSTR(err);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE loadDataFromPdb(LPWSTR path) override
    {
        if (!path) return E_INVALIDARG;
        if (m) return E_UNEXPECTED;
        wchar_t full[MAX_PATH * 4];
        DWORD n = GetFullPathNameW(path, sizeof full / sizeof full[0], full, nullptr);
        std::wstring fp = n && n < sizeof full / sizeof full[0] ? std::wstring(full, n) : std::wstring(path);
        if (GetFileAttributesW(fp.c_str()) == INVALID_FILE_ATTRIBUTES) {
            err = "PDB file not found";
            return (HRESULT)0x806D0005;  // E_PDB_NOT_FOUND, as observed
        }
        {
            uint8_t head[32] = {};
            DWORD got = 0;
            HANDLE f = CreateFileW(fp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                ReadFile(f, head, sizeof head, &got, nullptr);
                CloseHandle(f);
            }
            if (!isPdb(head, got)) {
                err = "not a PDB file";
                return (HRESULT)0x806D000C;  // E_PDB_FORMAT, as observed
            }
        }
        std::string e;
        auto model = Model::open(fromW(fp.c_str()), e);
        if (!model) {
            err = e;
            return (HRESULT)0x806D000C;
        }
        m = std::move(model);
        err.clear();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE loadDataFromPdbEx(LPWSTR path, LONG) override { return loadDataFromPdb(path); }

    // Error codes as observed: E_PDB_INVALID_SIG for a GUID or signature
    // mismatch, E_PDB_INVALID_AGE for an age mismatch.
    HRESULT STDMETHODCALLTYPE loadAndValidateDataFromPdb(LPWSTR path, GUID *guid, ULONG sig, ULONG age) override
    {
        HRESULT hr = loadDataFromPdb(path);
        if (hr != S_OK) return hr;
        Model::GUID70 g;
        uint32_t s = 0, a = 0;
        if (m->identity(g, s, a)) {
            bool sigOk = guid ? memcmp(guid, g.b, 16) == 0 : s == sig;
            if (!sigOk || a != age) {
                m.reset();
                err = !sigOk ? "PDB signature mismatch" : "PDB age mismatch";
                return !sigOk ? (HRESULT)0x806D0006 : (HRESULT)0x806D0007;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE loadAndValidateDataFromPdbEx(LPWSTR path, GUID *guid, ULONG sig, ULONG age, LONG) override
    {
        return loadAndValidateDataFromPdb(path, guid, sig, age);
    }

    HRESULT STDMETHODCALLTYPE loadDataFromIStream(IStream *st) override
    {
        if (!st) return E_INVALIDARG;
        if (m) return E_UNEXPECTED;
        std::vector<uint8_t> data;
        LARGE_INTEGER zero = {};
        st->Seek(zero, STREAM_SEEK_SET, nullptr);
        BYTE buf[65536];
        for (;;) {
            ULONG got = 0;
            HRESULT h = st->Read(buf, sizeof buf, &got);
            if (FAILED(h)) return h;
            data.insert(data.end(), buf, buf + got);
            if (got < sizeof buf) break;
        }
        if (!isPdb(data.data(), data.size())) return (HRESULT)0x806D000C;  // E_PDB_FORMAT, as observed
        std::string e;
        auto model = Model::openBuffer(std::move(data), e);
        if (!model) {
            err = e;
            return (HRESULT)0x806D000C;
        }
        m = std::move(model);
        err.clear();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE loadDataFromIStreamEx(IStream *st, LONG) override { return loadDataFromIStream(st); }

    static bool isPdb(const uint8_t *p, size_t n)
    {
        static const char magic[] = "Microsoft C/C++ MSF 7.00\r\n\x1a" "DS\0\0\0";
        return n >= 32 && memcmp(p, magic, 32) == 0;
    }
    HRESULT STDMETHODCALLTYPE openSession(IDiaSession **p) override
    {
        if (!p) return E_POINTER;
        *p = nullptr;
        if (!m) return E_UNEXPECTED;
        *p = new DiaSession(m);
        return S_OK;
    }
};

}  // namespace

HRESULT om_create_datasource(REFIID riid, void **ppv)
{
    auto *d = new DiaDataSource();
    HRESULT hr = d->QueryInterface(riid, ppv);
    d->Release();
    return hr;
}
