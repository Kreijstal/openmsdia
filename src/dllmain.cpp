// OpenMSDIA: DLL entry points and the class factory.
#include <windows.h>

#include <atomic>

#include "dia2_gen.h"

std::atomic<long> g_objects{0};
static std::atomic<long> g_locks{0};

HRESULT om_create_datasource(REFIID riid, void **ppv);

namespace {

class Factory : public IClassFactory {
public:
    explicit Factory(const CLSID &c) : clsid(c) {}
    CLSID clsid;
    std::atomic<long> refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        long r = --refs;
        if (!r) {
            delete this;
            --g_objects;
        }
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return om_create_datasource(riid, ppv);
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if (lock) ++g_locks; else --g_locks;
        return S_OK;
    }
};

}  // namespace

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    // DiaSourceAlt differs from DiaSource only in COM-wide locking, which
    // this implementation does not use.
    if (!IsEqualCLSID(clsid, CLSID_DiaSource) && !IsEqualCLSID(clsid, CLSID_DiaSourceAlt))
        return CLASS_E_CLASSNOTAVAILABLE;
    ++g_objects;
    auto *f = new Factory(clsid);
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow(void)
{
    return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE;
}

// Registration is not implemented: OpenMSDIA is meant to be loaded by path
// (or registered by the installer of the tool that ships it).
extern "C" HRESULT STDAPICALLTYPE DllRegisterServer(void) { return E_NOTIMPL; }
extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer(void) { return E_NOTIMPL; }
extern "C" HRESULT STDAPICALLTYPE VSDllRegisterServer(void *) { return E_NOTIMPL; }
extern "C" HRESULT STDAPICALLTYPE VSDllUnregisterServer(void *) { return E_NOTIMPL; }
