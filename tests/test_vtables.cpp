// Checks that the generated C++ interface classes have COM vtables laid out
// exactly as the type library says: for every interface, each method's
// vtable slot (from the snapshot) holds that method. Every stub method
// returns 0x1000 + its slot; the test calls each slot through the raw
// vtable and compares. x64 only (on x86 the stdcall callee pops its
// arguments, so calling with the wrong argument count is not safe).
#include <windows.h>
#include <stdio.h>

#define OPENMSDIA_STUB_RESULT(slot) ((HRESULT)(0x1000 + (slot)))
#include "dia2_stubs_gen.h"

static int failures = 0;

template <class T> static void check(const char *name, T *obj, int first, int count)
{
    void **vtbl = *(void ***)obj;
    for (int slot = first; slot < count; slot++) {
        auto fn = (HRESULT(STDMETHODCALLTYPE *)(void *, void *, void *, void *, void *, void *, void *, void *))vtbl[slot];
        HRESULT hr = fn(obj, 0, 0, 0, 0, 0, 0, 0);
        if (hr != 0x1000 + slot) {
            printf("FAIL %s slot %d returned 0x%lx\n", name, slot, (unsigned long)hr);
            failures++;
        }
    }
}

#define X(I, first, count)                                                                    \
    {                                                                                         \
        struct P : Stub_##I<I> {                                                              \
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override { return 1; } \
            ULONG STDMETHODCALLTYPE AddRef() override { return 1; }                           \
            ULONG STDMETHODCALLTYPE Release() override { return 1; }                          \
        } p;                                                                                  \
        check(#I, &p, 3, count);                                                              \
        n++;                                                                                  \
    }

int main()
{
#if !defined(_WIN64)
    printf("test_vtables: x64 only, skipped\n");
    return 0;
#else
    int n = 0;
    OPENMSDIA_FOR_EACH_INTERFACE(X)
    printf("test_vtables: %d interfaces, %d failures\n", n, failures);
    return failures ? 1 : 0;
#endif
}
