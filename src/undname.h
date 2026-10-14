// OpenMSDIA's undecorator for Microsoft C++ decorated names, written from
// the published name-mangling scheme and from the real msdia140.dll's
// observed output (oracle/undname). No dependencies.
#ifndef OPENMSDIA_UNDNAME_H
#define OPENMSDIA_UNDNAME_H

#include <string>

namespace om {

// The UNDNAME_* option bits get_undecoratedNameEx accepts.
enum : unsigned {
    UND_NO_LEADING_UNDERSCORES = 0x0001,
    UND_NO_MS_KEYWORDS = 0x0002,
    UND_NO_FUNCTION_RETURNS = 0x0004,
    UND_NO_ALLOCATION_MODEL = 0x0008,
    UND_NO_ALLOCATION_LANGUAGE = 0x0010,
    UND_NO_MS_THISTYPE = 0x0020,
    UND_NO_CV_THISTYPE = 0x0040,
    UND_NO_THISTYPE = 0x0060,
    UND_NO_ACCESS_SPECIFIERS = 0x0080,
    UND_NO_THROW_SIGNATURES = 0x0100,
    UND_NO_MEMBER_TYPE = 0x0200,
    UND_NO_RETURN_UDT_MODEL = 0x0400,
    UND_32_BIT_DECODE = 0x0800,
    UND_NAME_ONLY = 0x1000,
    UND_TYPE_ONLY = 0x2000,  // documented as UNDNAME_NO_ARGUMENTS
    UND_NO_SPECIAL_SYMS = 0x4000,
    UND_NO_ECSU = 0x8000,
    UND_NO_PTR64 = 0x20000,
    UND_ELLIPSIS_TEXT = 0x40000,  // "<ellipsis>" for "..."
};

// Undecorate a name the way msdia140's undecorator does with the given
// options; a name it cannot decode comes back unchanged.
std::string undname(const std::string &decorated, unsigned flags);

// get_undecoratedNameEx of a public symbol: DIA drops a leading '.'
// first. get_undecoratedName is the same with UND_NO_PTR64.
std::string diaUndecorate(const std::string &decorated, unsigned flags);

}  // namespace om

#endif
