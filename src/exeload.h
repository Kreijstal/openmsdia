// OpenMSDIA: finding the PDB of an executable (loadDataForExe,
// loadDataFromCodeViewInfo); see exeload.cpp.
#ifndef OPENMSDIA_EXELOAD_H
#define OPENMSDIA_EXELOAD_H

#include <windows.h>
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

#include "dia2_extra.h"

namespace om {

// Error codes, as the real DLL returns them.
constexpr HRESULT E_PDB_NOT_FOUND = (HRESULT)0x806D0005;
constexpr HRESULT E_PDB_INVALID_SIG = (HRESULT)0x806D0006;
constexpr HRESULT E_PDB_INVALID_AGE = (HRESULT)0x806D0007;
constexpr HRESULT E_PDB_FORMAT = (HRESULT)0x806D000C;
constexpr HRESULT E_PDB_INVALID_EXECUTABLE = (HRESULT)0x806D0012;
constexpr HRESULT E_PDB_NO_DEBUG_INFO = (HRESULT)0x806D0014;

// A CodeView record: RSDS (GUID, age, path) or NB10 (signature, age, path).
struct CvInfo {
    bool rsds = true;
    GUID guid = {};
    uint32_t sig = 0, age = 0;
    std::string path;
};

// Opens and validates a candidate; S_OK keeps it as the loaded PDB.
using TryPdb = std::function<HRESULT(const std::wstring &path, const CvInfo &cv)>;

// Searches as the real DLL does. cvInfo (loadDataFromCodeViewInfo) replaces
// reading the executable. On failure lastError is what the real DLL
// reports in get_lastError.
HRESULT findPdb(const wchar_t *exe, const wchar_t *searchPath, IUnknown *callback, const uint8_t *cvInfo,
                size_t cvSize, const TryPdb &tryPdb, std::wstring &lastError);

}  // namespace om

#endif
