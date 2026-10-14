/* DIA interfaces that are not in msdia140's type library, written by
 * OpenMSDIA from public facts: the method names and signatures from
 * Microsoft's public DIA SDK documentation, the IIDs and the vtable order
 * from black-box observation of the real DLL (walker/exe_probe.cpp: the
 * IIDs it queries a load callback for, and which slot it calls with which
 * arguments). See docs/plan.md, "Load callbacks". */

#ifndef OPENMSDIA_DIA2_EXTRA_H
#define OPENMSDIA_DIA2_EXTRA_H

#include <windows.h>
#include <unknwn.h>

/* QueryInterface'd by loadDataForExe/loadDataFromCodeViewInfo, in this
 * order. */
inline constexpr IID IID_IDiaLoadCallback = {0xC32ADB82, 0x73F4, 0x421B, {0x95, 0xD5, 0xA4, 0x70, 0x6E, 0xDF, 0x5D, 0xBE}};
inline constexpr IID IID_IDiaLoadCallback2 = {0x4688A074, 0x5A4D, 0x4486, {0xAE, 0xA8, 0x7B, 0x90, 0x71, 0x1D, 0x9F, 0x7C}};
inline constexpr IID IID_IDiaReadExeAtOffsetCallback = {0x587A461C, 0xB80B, 0x4F54, {0x91, 0x94, 0x50, 0x32, 0x58, 0x9A, 0x63, 0x19}};
inline constexpr IID IID_IDiaReadExeAtRVACallback = {0x8E3F80CA, 0x7517, 0x432A, {0xBA, 0x07, 0x28, 0x51, 0x34, 0xAA, 0xEA, 0x8E}};

struct IDiaLoadCallback : public IUnknown {
    /* slot 3: called with the executable's debug directory */
    virtual HRESULT STDMETHODCALLTYPE NotifyDebugDir(BOOL fExecutable, DWORD cbData, BYTE *pbData) = 0;
    /* slot 4 */
    virtual HRESULT STDMETHODCALLTYPE NotifyOpenDBG(LPCOLESTR dbgPath, HRESULT resultCode) = 0;
    /* slot 5: called for every PDB path tried, with the outcome */
    virtual HRESULT STDMETHODCALLTYPE NotifyOpenPDB(LPCOLESTR pdbPath, HRESULT resultCode) = 0;
    /* slots 6, 7: S_OK allows the access */
    virtual HRESULT STDMETHODCALLTYPE RestrictRegistryAccess() = 0;
    virtual HRESULT STDMETHODCALLTYPE RestrictSymbolServerAccess() = 0;
};

struct IDiaLoadCallback2 : public IDiaLoadCallback {
    /* slots 8 to 11: S_OK allows the access */
    virtual HRESULT STDMETHODCALLTYPE RestrictOriginalPathAccess() = 0;
    virtual HRESULT STDMETHODCALLTYPE RestrictReferencePathAccess() = 0;
    virtual HRESULT STDMETHODCALLTYPE RestrictDBGAccess() = 0;
    virtual HRESULT STDMETHODCALLTYPE RestrictSystemRootAccess() = 0;
};

struct IDiaReadExeAtOffsetCallback : public IUnknown {
    /* slot 3 */
    virtual HRESULT STDMETHODCALLTYPE ReadExecutableAt(DWORDLONG fileOffset, DWORD cbData, DWORD *pcbData,
                                                       BYTE *pbData) = 0;
};

struct IDiaReadExeAtRVACallback : public IUnknown {
    /* slot 3 */
    virtual HRESULT STDMETHODCALLTYPE ReadExecutableAtRVA(DWORD relativeVirtualAddress, DWORD cbData, DWORD *pcbData,
                                                          BYTE *pbData) = 0;
};

/* Tables' enumerators that are not in the type library. The IIDs are those
 * Ghidra's PDB exporter queries a table for (traced with
 * OPENMSDIA_TRACE_QI; its source names the interfaces), the method names
 * and signatures are from the public documentation, the vtable order is
 * observed by calling each slot of the real DLL's objects
 * (walker/call_probe.cpp). */
inline constexpr IID IID_IDiaEnumSegments = {0xE8368CA9, 0x01D1, 0x419D, {0xAC, 0x0C, 0xE3, 0x12, 0x35, 0xDB, 0xDA, 0x9F}};
inline constexpr IID IID_IDiaEnumFrameData = {0x9FC77A4B, 0x3C1C, 0x44ED, {0xA7, 0x98, 0x6C, 0x1D, 0xEE, 0xA5, 0x3E, 0x1F}};

/* A section map entry (slots 3 to 11 in this order). */
struct IDiaSegment : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_frame(DWORD *pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_offset(DWORD *pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_length(DWORD *pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_read(BOOL *pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_write(BOOL *pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_execute(BOOL *pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_addressSection(DWORD *pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_relativeVirtualAddress(DWORD *pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_virtualAddress(ULONGLONG *pRetVal) = 0;
};

/* Laid out like the type library's enumerators: get__NewEnum, get_Count
 * (slot 4), Item, Next (slot 6), Skip, Reset, Clone. */
struct IDiaEnumSegments : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get__NewEnum(IUnknown **pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Count(LONG *pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE Item(DWORD index, IDiaSegment **segment) = 0;
    virtual HRESULT STDMETHODCALLTYPE Next(ULONG celt, IDiaSegment **rgelt, ULONG *pceltFetched) = 0;
    virtual HRESULT STDMETHODCALLTYPE Skip(ULONG celt) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset() = 0;
    virtual HRESULT STDMETHODCALLTYPE Clone(IDiaEnumSegments **ppenum) = 0;
};

struct IDiaFrameData;

/* The same, then frameByRVA (slot 10) and frameByVA (slot 11). */
struct IDiaEnumFrameData : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get__NewEnum(IUnknown **pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Count(LONG *pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE Item(DWORD index, IDiaFrameData **frame) = 0;
    virtual HRESULT STDMETHODCALLTYPE Next(ULONG celt, IDiaFrameData **rgelt, ULONG *pceltFetched) = 0;
    virtual HRESULT STDMETHODCALLTYPE Skip(ULONG celt) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset() = 0;
    virtual HRESULT STDMETHODCALLTYPE Clone(IDiaEnumFrameData **ppenum) = 0;
    virtual HRESULT STDMETHODCALLTYPE frameByRVA(DWORD relativeVirtualAddress, IDiaFrameData **frame) = 0;
    virtual HRESULT STDMETHODCALLTYPE frameByVA(ULONGLONG virtualAddress, IDiaFrameData **frame) = 0;
};

#endif
