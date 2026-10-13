// OpenMSDIA symbol model: see model.h.
#include "model.h"

#include <algorithm>
#include <bitset>
#include <cstring>
#include <set>

#include "llvm/ADT/StringExtras.h"
#include "llvm/DebugInfo/CodeView/CVTypeVisitor.h"
#include "llvm/DebugInfo/CodeView/DebugChecksumsSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/LazyRandomTypeCollection.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/TypeDeserializer.h"
#include "llvm/DebugInfo/CodeView/TypeRecordHelpers.h"
#include "llvm/DebugInfo/CodeView/TypeVisitorCallbacks.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptor.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleList.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/GlobalsStream.h"
#include "llvm/DebugInfo/PDB/Native/ISectionContribVisitor.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/ModuleDebugStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeExeSymbol.h"
#include "llvm/DebugInfo/PDB/Native/NativeTypeFunctionSig.h"
#include "llvm/DebugInfo/PDB/Native/NativeTypeUDT.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PDBStringTable.h"
#include "llvm/DebugInfo/PDB/Native/PublicsStream.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/RawTypes.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/DebugInfo/PDB/Native/SymbolCache.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/DebugInfo/PDB/PDBSymbol.h"
#include "llvm/DebugInfo/PDB/PDBSymbolCompiland.h"
#include "llvm/DebugInfo/PDB/IPDBEnumChildren.h"
#include "llvm/Support/ConvertUTF.h"

using namespace llvm;
using namespace llvm::codeview;
using namespace llvm::pdb;

namespace om {

const char *propName(Prop p)
{
    static const char *names[] = {
#define X(n, k) #n,
        OPENMSDIA_SYMBOL_GETTERS(X)
#undef X
    };
    return p < P_COUNT ? names[p] : "?";
}

static LazyRandomTypeCollection *tpiTypes(NativeSession &ns);
static bool resolveTag(Model &m, NativeSession &ns, TypeIndex ti, CVType &out, TypeIndex &full);

template <class E> static bool has(E v, E f) { return ((uint64_t)v & (uint64_t)f) != 0; }

namespace {

enum PKind { K_LONG, K_ULONG, K_ULONGLONG, K_BSTR, K_GUID, K_VARIANT, K_SYM };
const PKind propKinds[] = {
#define X(n, k) K_##k,
    OPENMSDIA_SYMBOL_GETTERS(X)
#undef X
};

Prop propByName(const char *n)
{
    for (int i = 0; i < P_COUNT; i++)
        if (!strcmp(propName((Prop)i), n)) return (Prop)i;
    return P_COUNT;
}

// IDiaSymbol property -> IPDBRawSymbol method (LLVM's DIA backend, read
// backwards).
bool rawGet(const IPDBRawSymbol &r, Prop p, Val &v)
{
    switch (p) {
#define R_BOOL(m) v = Val::b_(r.m())
#define R_U32(m) v = Val::u_(r.m())
#define R_I32(m) v = Val::i_(r.m())
#define R_U64(m) v = Val::u_(r.m())
#define R_ID(m) v = Val::sym(r.m())
#define R_STR(m) v = Val::str(r.m())
#define R_ENUM(m) v = Val::u_((uint32_t)r.m())
#define R_GUID(m) { auto g = r.m(); memcpy(&v.g, &g, 16); v.kind = Val::Guid; }
#define R_VER_MAJOR(m) { VersionInfo vi{}; r.m(vi); v = Val::u_(vi.Major); }
#define R_VER_MINOR(m) { VersionInfo vi{}; r.m(vi); v = Val::u_(vi.Minor); }
#define R_VER_BUILD(m) { VersionInfo vi{}; r.m(vi); v = Val::u_(vi.Build); }
#define R_VER_QFE(m) { VersionInfo vi{}; r.m(vi); v = Val::u_(vi.QFE); }
#define R(prop, meth, conv) case P_##prop: R_##conv(meth); return true;
#include "rawsym_map.inc"
#undef R
    case P_value:
        v.kind = Val::Var;
        v.var = r.getValue();
        return true;
    default:
        return false;
    }
}

// The *Id property that backs a symbol-valued property.
Prop idPropOf(Prop p)
{
    static std::vector<Prop> map;
    if (map.empty()) {
        map.assign(P_COUNT, P_COUNT);
        for (int i = 0; i < P_COUNT; i++) {
            if (propKinds[i] != K_SYM) continue;
            std::string n = std::string(propName((Prop)i)) + "Id";
            map[i] = propByName(n.c_str());
        }
    }
    return map[p];
}

// Properties the real DLL reports for symbols of a tag that LLVM models.
struct TagProps {
    std::bitset<P_COUNT> always;
};

std::bitset<P_COUNT> bits(std::initializer_list<const char *> names)
{
    std::bitset<P_COUNT> b;
    for (auto *n : names) {
        Prop p = propByName(n);
        if (p != P_COUNT) b.set(p);
    }
    return b;
}

const std::bitset<P_COUNT> &tagProps(PDB_SymType t)
{
    static std::map<PDB_SymType, std::bitset<P_COUNT>> m;
    if (m.empty()) {
        auto common = bits({"symIndexId", "symTag", "lexicalParent", "lexicalParentId"});
        auto cv = bits({"constType", "volatileType", "unalignedType", "constantExport"});
        auto udtish = bits({"constructor", "exportIsForwarder", "hasAssignmentOperator",
                            "hasCastOperator", "hasNestedTypes", "hfaDouble", "hfaFloat",
                            "intrinsic", "isInterfaceUdt", "isRefUdt", "isValueUdt", "length",
                            "name", "nested", "overloadedOperator", "packed", "scoped", "sealed"});
        m[PDB_SymType::Exe] = bits({"symIndexId", "symTag", "age", "guid", "isCTypes", "isStripped",
                                    "machineType", "name", "signature", "symbolsFileName"});
        m[PDB_SymType::Compiland] = common | bits({"editAndContinueEnabled", "libraryName", "name"});
        m[PDB_SymType::ArrayType] = common | cv | bits({"arrayIndexType", "arrayIndexTypeId", "count",
                                                        "length", "name", "type", "typeId"});
        m[PDB_SymType::BuiltinType] = common | cv | bits({"baseType", "length", "unmodifiedType",
                                                          "unmodifiedTypeId"});
        m[PDB_SymType::Enum] = common | cv | udtish | bits({"baseType", "type", "typeId"});
        m[PDB_SymType::FunctionSig] = common | cv | bits({"callingConvention", "constructor", "count",
                                                          "isConstructorVirtualBase", "isCxxReturnUdt",
                                                          "type", "typeId"});
        m[PDB_SymType::FunctionArg] = common | bits({"classParent", "classParentId", "type", "typeId"});
        m[PDB_SymType::PointerType] = common | cv | bits({"RValueReference", "isPointerToDataMember",
                                                          "isPointerToMemberFunction", "isWinRTPointer",
                                                          "length", "reference", "restrictedType",
                                                          "type", "typeId"});
        m[PDB_SymType::Typedef] = common | bits({"name", "type", "typeId"});
        m[PDB_SymType::UDT] = common | cv | udtish | bits({"udtKind"});
        m[PDB_SymType::VTableShape] = common | cv | bits({"count"});
    }
    static std::bitset<P_COUNT> none;
    auto it = m.find(t);
    return it == m.end() ? none : it->second;
}

template <class T> bool deser(const CVSymbol &s, T &out)
{
    auto e = SymbolDeserializer::deserializeAs<T>(s);
    if (!e) {
        consumeError(e.takeError());
        return false;
    }
    out = std::move(*e);
    return true;
}

template <class T> bool deserType(CVType t, T &out)
{
    if (auto e = TypeDeserializer::deserializeAs<T>(t, out)) {
        consumeError(std::move(e));
        return false;
    }
    return true;
}

bool isProc(SymbolKind k)
{
    return k == S_GPROC32 || k == S_LPROC32 || k == S_GPROC32_ID || k == S_LPROC32_ID ||
           k == S_LPROC32_DPC || k == S_LPROC32_DPC_ID;
}

bool opensScope(SymbolKind k)
{
    return isProc(k) || k == S_BLOCK32 || k == S_THUNK32 || k == S_INLINESITE || k == S_INLINESITE2 ||
           k == S_WITH32 || k == S_SEPCODE;
}

// VARIANT type that the real DLL reports for a CodeView numeric leaf, as
// LLVM decodes it into an APSInt: 16-bit unsigned literals become VT_I2.
Variant variantOf(const APSInt &v)
{
    Variant r;
    unsigned w = v.getBitWidth();
    if (w <= 8) {
        r.Type = v.isSigned() ? PDB_VariantType::Int8 : PDB_VariantType::UInt8;
        if (v.isSigned()) r.Value.Int8 = (int8_t)v.getSExtValue(); else r.Value.UInt8 = (uint8_t)v.getZExtValue();
    } else if (w <= 16) {
        r.Type = PDB_VariantType::Int16;
        r.Value.Int16 = (int16_t)v.getZExtValue();
    } else if (w <= 32) {
        r.Type = v.isSigned() ? PDB_VariantType::Int32 : PDB_VariantType::UInt32;
        if (v.isSigned()) r.Value.Int32 = (int32_t)v.getSExtValue(); else r.Value.UInt32 = (uint32_t)v.getZExtValue();
    } else {
        r.Type = v.isSigned() ? PDB_VariantType::Int64 : PDB_VariantType::UInt64;
        if (v.isSigned()) r.Value.Int64 = v.getSExtValue(); else r.Value.UInt64 = v.getZExtValue();
    }
    return r;
}

}  // namespace

// ------------------------------------------------------------------ symbols

// A function from a module symbol stream (S_GPROC32 and friends).
class OmFunction : public OmSym {
public:
    OmFunction(NativeSession &s, SymIndexId id, uint16_t modi, uint32_t off, ProcSym ps)
        : OmSym(s, PDB_SymType::Function, id), modi(modi), off(off), ps(std::move(ps)) {}
    uint16_t modi;
    uint32_t off;
    ProcSym ps;
    bool hasFrame = false;
    FrameProcSym fp{SymbolRecordKind::FrameProcSym};
    CPUType cpu = CPUType::X64;
    // Bound class method (LF_ONEMETHOD) attributes, if any.
    bool isMethod = false;
    MemberAccess access = MemberAccess::None;
    MethodKind mkind = MethodKind::Vanilla;
    MethodOptions mopts = MethodOptions::None;
    int32_t vftableOffset = 0;
    std::string shortName;

    uint32_t start() const { return M->rva(ps.Segment, ps.CodeOffset); }

    bool prop(Prop p, Val &v) const override
    {
        bool isStatic = kind == S_LPROC32 || kind == S_LPROC32_ID || kind == S_LPROC32_DPC || kind == S_LPROC32_DPC_ID;
        switch (p) {
        case P_name: v = Val::str(ps.Name.str()); return true;
        case P_undecoratedName: v = Val::str(M->functionSignature(getSymIndexId(), isStatic)); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_classParent: case P_classParentId:
            if (!classParent) return false;
            v = Val::sym(classParent);
            return true;
        case P_type: case P_typeId: v = Val::sym(M->typeSym(ps.FunctionType)); return true;
        case P_addressSection: v = Val::u_(ps.Segment); return true;
        case P_addressOffset: v = Val::u_(ps.CodeOffset); return true;
        case P_relativeVirtualAddress: v = Val::u_(start()); return true;
        case P_virtualAddress: v = Val::u_(start() + M->loadAddress); return true;
        case P_length: v = Val::u_(ps.CodeSize); return true;
        case P_locationType: v = Val::u_((uint32_t)PDB_LocType::Static); return true;
        case P_isStatic: if (!isStatic) return false; v = Val::b_(true); return true;
        case P_framePointerPresent: v = Val::b_(has(ps.Flags, ProcSymFlags::HasFP)); return true;
        case P_interruptReturn: v = Val::b_(has(ps.Flags, ProcSymFlags::HasIRET)); return true;
        case P_farReturn: v = Val::b_(has(ps.Flags, ProcSymFlags::HasFRET)); return true;
        case P_noReturn: v = Val::b_(has(ps.Flags, ProcSymFlags::IsNoReturn)); return true;
        case P_notReached: v = Val::b_(has(ps.Flags, ProcSymFlags::IsUnreachable)); return true;
        case P_customCallingConvention: v = Val::b_(has(ps.Flags, ProcSymFlags::HasCustomCallingConv)); return true;
        case P_noInline: v = Val::b_(has(ps.Flags, ProcSymFlags::IsNoInline)); return true;
        case P_optimizedCodeDebugInfo: v = Val::b_(has(ps.Flags, ProcSymFlags::HasOptimizedDebugInfo)); return true;
        case P_coroutineKind: case P_hasValidPGOCounts: case P_isPGO: case P_isOptimizedForSpeed:
        case P_hasControlFlowCheck: case P_isRTCs: case P_isSignRet: case P_noexcept: case P_wasInlined:
        case P_exceptionHandlerAddressOffset: case P_exceptionHandlerAddressSection:
        case P_exceptionHandlerRelativeVirtualAddress: case P_exceptionHandlerVirtualAddress:
        case P_framePadOffset: case P_framePadSize: case P_frameSize: case P_hasAlloca: case P_hasEH:
        case P_hasEHa: case P_hasInlAsm: case P_hasLongJump: case P_hasSEH: case P_hasSecurityChecks:
        case P_hasSetJump: case P_inlSpec: case P_isNaked: case P_isSafeBuffers: case P_noStackOrdering:
        case P_strictGSCheck: case P_localBasePointerRegisterId: case P_paramBasePointerRegisterId:
            return frameProp(p, v);
        case P_access: if (!isMethod) return false; v = Val::u_((uint32_t)access); return true;
        case P_virtual: if (!isMethod) return false;
            v = Val::b_(mkind == MethodKind::Virtual || mkind == MethodKind::IntroducingVirtual ||
                        mkind == MethodKind::PureVirtual || mkind == MethodKind::PureIntroducingVirtual);
            return true;
        case P_pure: if (!isMethod) return false;
            v = Val::b_(mkind == MethodKind::PureVirtual || mkind == MethodKind::PureIntroducingVirtual);
            return true;
        case P_intro: if (!isMethod) return false;
            v = Val::b_(mkind == MethodKind::IntroducingVirtual || mkind == MethodKind::PureIntroducingVirtual);
            return true;
        case P_isStaticMemberFunc: if (!isMethod) return false; v = Val::b_(mkind == MethodKind::Static); return true;
        case P_compilerGenerated: if (!isMethod) return false;
            v = Val::b_(has(mopts, MethodOptions::CompilerGenerated));
            return true;
        case P_sealed: if (!isMethod) return false; v = Val::b_(has(mopts, MethodOptions::Sealed)); return true;
        case P_virtualBaseOffset: if (!isMethod) return false; v = Val::u_(vftableOffset); return true;
        default: return false;
        }
    }

    bool frameProp(Prop p, Val &v) const
    {
        auto F = [&](FrameProcedureOptions o) { return hasFrame && has(fp.Flags, o); };
        switch (p) {
        case P_coroutineKind: case P_hasControlFlowCheck: case P_isRTCs: case P_isSignRet:
        case P_noexcept: case P_wasInlined:
        case P_exceptionHandlerAddressOffset: case P_exceptionHandlerAddressSection:
        case P_exceptionHandlerRelativeVirtualAddress: case P_exceptionHandlerVirtualAddress:
            if (p == P_exceptionHandlerAddressOffset && hasFrame) { v = Val::u_(fp.OffsetOfExceptionHandler); return true; }
            if (p == P_exceptionHandlerAddressSection && hasFrame) { v = Val::u_(fp.SectionIdOfExceptionHandler); return true; }
            if (p == P_exceptionHandlerRelativeVirtualAddress && hasFrame && fp.SectionIdOfExceptionHandler) {
                v = Val::u_(M->rva(fp.SectionIdOfExceptionHandler, fp.OffsetOfExceptionHandler));
                return true;
            }
            if (p == P_wasInlined) { v = Val::b_(F(FrameProcedureOptions::Inlined)); return true; }
            v = Val::u_(0);
            return true;
        case P_hasValidPGOCounts: v = Val::b_(F(FrameProcedureOptions::ValidProfileCounts)); return true;
        case P_isPGO: v = Val::b_(F(FrameProcedureOptions::ProfileGuidedOptimization)); return true;
        case P_isOptimizedForSpeed: v = Val::b_(F(FrameProcedureOptions::OptimizedForSpeed)); return true;
        case P_framePadOffset: v = Val::u_(hasFrame ? fp.OffsetToPadding : 0); return true;
        case P_framePadSize: v = Val::u_(hasFrame ? fp.PaddingFrameBytes : 0); return true;
        case P_frameSize: v = Val::u_(hasFrame ? fp.TotalFrameBytes : 0); return true;
        case P_hasAlloca: v = Val::b_(F(FrameProcedureOptions::HasAlloca)); return true;
        case P_hasEH: v = Val::b_(F(FrameProcedureOptions::HasExceptionHandling)); return true;
        case P_hasEHa: v = Val::b_(F(FrameProcedureOptions::AsynchronousExceptionHandling)); return true;
        case P_hasInlAsm: v = Val::b_(F(FrameProcedureOptions::HasInlineAssembly)); return true;
        case P_hasLongJump: v = Val::b_(F(FrameProcedureOptions::HasLongJmp)); return true;
        case P_hasSEH: v = Val::b_(F(FrameProcedureOptions::HasStructuredExceptionHandling)); return true;
        case P_hasSecurityChecks: v = Val::b_(F(FrameProcedureOptions::SecurityChecks)); return true;
        case P_hasSetJump: v = Val::b_(F(FrameProcedureOptions::HasSetJmp)); return true;
        case P_inlSpec: v = Val::b_(F(FrameProcedureOptions::MarkedInline)); return true;
        case P_isNaked: v = Val::b_(F(FrameProcedureOptions::Naked)); return true;
        case P_isSafeBuffers: v = Val::b_(F(FrameProcedureOptions::SafeBuffers)); return true;
        case P_noStackOrdering: v = Val::b_(F(FrameProcedureOptions::NoStackOrderingForSecurityChecks)); return true;
        case P_strictGSCheck: v = Val::b_(F(FrameProcedureOptions::StrictSecurityChecks)); return true;
        case P_localBasePointerRegisterId:
            if (!hasFrame || !(uint32_t)fp.getLocalFramePtrReg(cpu)) return false;
            v = Val::u_((uint32_t)fp.getLocalFramePtrReg(cpu));
            return true;
        case P_paramBasePointerRegisterId:
            if (!hasFrame || !(uint32_t)fp.getParamFramePtrReg(cpu)) return false;
            v = Val::u_((uint32_t)fp.getParamFramePtrReg(cpu));
            return true;
        default: return false;
        }
    }

    SymbolKind kind = S_GPROC32;
    mutable SymIndexId dbgStart = 0, dbgEnd = 0;
    void children(std::vector<SymIndexId> &out) const override;
};

// FuncDebugStart / FuncDebugEnd of a function.
class OmFuncDebug : public OmSym {
public:
    OmFuncDebug(NativeSession &s, SymIndexId id, PDB_SymType tag, const OmFunction *fn)
        : OmSym(s, tag, id), fn(fn) {}
    const OmFunction *fn;
    bool prop(Prop p, Val &v) const override
    {
        uint32_t delta = getSymTag() == PDB_SymType::FuncDebugStart ? fn->ps.DbgStart : fn->ps.DbgEnd;
        switch (p) {
        case P_addressSection: v = Val::u_(fn->ps.Segment); return true;
        case P_addressOffset: v = Val::u_(fn->ps.CodeOffset + delta); return true;
        case P_relativeVirtualAddress: v = Val::u_(fn->start() + delta); return true;
        case P_virtualAddress: v = Val::u_(fn->start() + delta + M->loadAddress); return true;
        case P_locationType: v = Val::u_((uint32_t)PDB_LocType::Static); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_isStatic: case P_framePointerPresent: case P_interruptReturn: case P_farReturn:
        case P_noReturn: case P_notReached: case P_customCallingConvention: case P_noInline:
        case P_optimizedCodeDebugInfo:
            return fn->prop(p, v);
        default: return false;
        }
    }
};

// S_BLOCK32.
class OmBlock : public OmSym {
public:
    OmBlock(NativeSession &s, SymIndexId id, uint16_t modi, uint32_t off, BlockSym bs)
        : OmSym(s, PDB_SymType::Block, id), modi(modi), off(off), bs(std::move(bs)) {}
    uint16_t modi;
    uint32_t off;
    BlockSym bs;
    SymIndexId func = 0;
    bool prop(Prop p, Val &v) const override
    {
        uint32_t rva = M->rva(bs.Segment, bs.CodeOffset);
        switch (p) {
        case P_name: v = Val::str(bs.Name.str()); return true;
        case P_addressSection: v = Val::u_(bs.Segment); return true;
        case P_addressOffset: v = Val::u_(bs.CodeOffset); return true;
        case P_relativeVirtualAddress: v = Val::u_(rva); return true;
        case P_virtualAddress: v = Val::u_(rva + M->loadAddress); return true;
        case P_length: v = Val::u_(bs.CodeSize); return true;
        case P_locationType: v = Val::u_((uint32_t)PDB_LocType::Static); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        default: return false;
        }
    }
    void children(std::vector<SymIndexId> &out) const override;
};

// Data: globals, statics, members, static members, constants, enumerators.
class OmData : public OmSym {
public:
    enum Kind { Global, Member, StaticMember, Constant, Enumerator };
    OmData(NativeSession &s, SymIndexId id, Kind k) : OmSym(s, PDB_SymType::Data, id), k(k) {}
    Kind k;
    PDB_DataKind dataKind = PDB_DataKind::Unknown;
    std::string nm;
    TypeIndex type;
    SymIndexId typeId = 0;  // when set, overrides `type`
    uint16_t seg = 0;
    uint32_t off = 0;
    bool hasAddr = false;
    int32_t memberOffset = 0;
    MemberAccess access = MemberAccess::None;
    bool bitfield = false;
    uint32_t bitPos = 0, bitLen = 0;
    Variant value;
    bool staticConst = false;  // a static member defined by an S_CONSTANT

    bool fromGlobals = false;
    bool localConst = false;  // S_CONSTANT inside a function
    bool noChildren() const override { return (k == Constant && !localConst) || fromGlobals; }

    bool prop(Prop p, Val &v) const override
    {
        bool member = k == Member || k == StaticMember || k == Enumerator;
        switch (p) {
        case P_name: v = Val::str(nm); return true;
        case P_dataKind: v = Val::u_((uint32_t)dataKind); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_type: case P_typeId: v = Val::sym(typeId ? typeId : M->typeSym(type)); return true;
        case P_classParent: case P_classParentId:
            if (!member) return false;
            v = Val::sym(classParent);
            return true;
        case P_constType: case P_volatileType: case P_unalignedType: case P_constantExport:
            if (!member) return false;
            v = Val::b_(false);
            return true;
        case P_access:
            if (k != Member && k != StaticMember) return false;
            v = Val::u_((uint32_t)access);
            return true;
        case P_locationType:
            if (k == Constant || k == Enumerator || staticConst) v = Val::u_((uint32_t)PDB_LocType::Constant);
            else if (k == Member) v = Val::u_((uint32_t)(bitfield ? PDB_LocType::BitField : PDB_LocType::ThisRel));
            else v = Val::u_((uint32_t)PDB_LocType::Static);
            return true;
        case P_offset:
            if (k != Member) return false;
            v = Val::i_(memberOffset);
            return true;
        case P_bitPosition:
            if (!bitfield) return false;
            v = Val::u_(bitPos);
            return true;
        case P_length:
            if (!bitfield) return false;
            v = Val::u_(bitLen);
            return true;
        case P_addressSection: if (!hasAddr) return false; v = Val::u_(seg); return true;
        case P_addressOffset: if (!hasAddr) return false; v = Val::u_(off); return true;
        case P_relativeVirtualAddress: if (!hasAddr) return false; v = Val::u_(M->rva(seg, off)); return true;
        case P_virtualAddress: if (!hasAddr) return false; v = Val::u_(M->rva(seg, off) + M->loadAddress); return true;
        case P_value:
            if (k != Constant && k != Enumerator && !staticConst) return false;
            v.kind = Val::Var;
            v.var = value;
            return true;
        default: return false;
        }
    }
};

// S_PUB32.
class OmPublic : public OmSym {
public:
    OmPublic(NativeSession &s, SymIndexId id, PublicSym32 ps) : OmSym(s, PDB_SymType::PublicSymbol, id), ps(std::move(ps)) {}
    PublicSym32 ps;
    uint32_t len = 0;
    bool noChildren() const override { return true; }
    bool prop(Prop p, Val &v) const override
    {
        uint32_t rva = M->rva(ps.Segment, ps.Offset);
        bool code = has(ps.Flags, PublicSymFlags::Code) || has(ps.Flags, PublicSymFlags::Function);
        switch (p) {
        case P_name: v = Val::str(ps.Name.str()); return true;
        case P_undecoratedName: {
            std::string n = ps.Name.str();
            if (!n.empty() && n[0] == '.') n = n.substr(1);
            v = Val::str(n);
            return true;
        }
        case P_addressSection: v = Val::u_(ps.Segment); return true;
        case P_addressOffset: v = Val::u_(ps.Offset); return true;
        case P_relativeVirtualAddress: v = Val::u_(rva); return true;
        case P_virtualAddress: v = Val::u_(rva + M->loadAddress); return true;
        case P_length: v = Val::u_(len); return true;
        case P_locationType: v = Val::u_((uint32_t)PDB_LocType::Static); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_code: v = Val::b_(code); return true;
        case P_function: v = Val::b_(has(ps.Flags, PublicSymFlags::Function)); return true;
        case P_managed: v = Val::b_(has(ps.Flags, PublicSymFlags::Managed)); return true;
        case P_msil: v = Val::b_(has(ps.Flags, PublicSymFlags::MSIL)); return true;
        case P_dataExport: v = Val::b_(!code); return true;
        default: return false;
        }
    }
};

// S_COMPILE3 (or S_COMPILE2) of a module.
class OmCompilandDetails : public OmSym {
public:
    OmCompilandDetails(NativeSession &s, SymIndexId id, Compile3Sym cs)
        : OmSym(s, PDB_SymType::CompilandDetails, id), cs(std::move(cs)) {}
    Compile3Sym cs;
    bool prop(Prop p, Val &v) const override
    {
        auto F = [&](CompileSym3Flags f) { return has(cs.getFlags(), f); };
        switch (p) {
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_compilerName: v = Val::str(cs.Version.str()); return true;
        case P_language: v = Val::u_((uint32_t)cs.getLanguage()); return true;
        case P_platform: v = Val::u_((uint32_t)cs.Machine); return true;
        case P_frontEndMajor: v = Val::u_(cs.VersionFrontendMajor); return true;
        case P_frontEndMinor: v = Val::u_(cs.VersionFrontendMinor); return true;
        case P_frontEndBuild: v = Val::u_(cs.VersionFrontendBuild); return true;
        case P_frontEndQFE: v = Val::u_(cs.VersionFrontendQFE); return true;
        case P_backEndMajor: v = Val::u_(cs.VersionBackendMajor); return true;
        case P_backEndMinor: v = Val::u_(cs.VersionBackendMinor); return true;
        case P_backEndBuild: v = Val::u_(cs.VersionBackendBuild); return true;
        case P_backEndQFE: v = Val::u_(cs.VersionBackendQFE); return true;
        case P_editAndContinueEnabled: v = Val::b_(F(CompileSym3Flags::EC)); return true;
        case P_hasDebugInfo: v = Val::b_(!F(CompileSym3Flags::NoDbgInfo)); return true;
        case P_isLTCG: v = Val::b_(F(CompileSym3Flags::LTCG)); return true;
        case P_isDataAligned: v = Val::b_(!F(CompileSym3Flags::NoDataAlign)); return true;
        case P_hasManagedCode: v = Val::b_(F(CompileSym3Flags::ManagedPresent)); return true;
        case P_hasSecurityChecks: v = Val::b_(F(CompileSym3Flags::SecurityChecks)); return true;
        case P_isHotpatchable: v = Val::b_(F(CompileSym3Flags::HotPatch)); return true;
        case P_isCVTCIL: v = Val::b_(F(CompileSym3Flags::CVTCIL)); return true;
        case P_isMSILNetmodule: v = Val::b_(F(CompileSym3Flags::MSILModule)); return true;
        case P_isSdl: v = Val::b_(F(CompileSym3Flags::Sdl)); return true;
        case P_isPGO: v = Val::b_(F(CompileSym3Flags::PGO)); return true;
        default: return false;
        }
    }
};

// One name/value pair of a module's build environment.
class OmCompilandEnv : public OmSym {
public:
    OmCompilandEnv(NativeSession &s, SymIndexId id, std::string n, std::string val)
        : OmSym(s, PDB_SymType::CompilandEnv, id), n(std::move(n)), val(std::move(val)) {}
    std::string n, val;
    bool prop(Prop p, Val &v) const override
    {
        switch (p) {
        case P_name: v = Val::str(n); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_value:
            v.kind = Val::Var;
            v.strVar = true;
            v.s = val;
            return true;
        default: return false;
        }
    }
};

// LF_BCLASS / LF_VBCLASS / LF_IVBCLASS.
class OmBaseClass : public OmSym {
public:
    OmBaseClass(NativeSession &s, SymIndexId id) : OmSym(s, PDB_SymType::BaseClass, id) {}
    SymIndexId base = 0;  // the base UDT
    MemberAccess access = MemberAccess::None;
    int32_t offset = 0;
    bool isVirtual = false, indirect = false;
    SymIndexId vbptrType = 0;
    int32_t vbptrOffset = 0;
    uint32_t vbIndex = 0;
    bool prop(Prop p, Val &v) const override
    {
        switch (p) {
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_classParent: case P_classParentId: v = Val::sym(classParent); return true;
        case P_type: case P_typeId: v = Val::sym(base); return true;
        case P_access: v = Val::u_((uint32_t)access); return true;
        case P_offset: if (isVirtual) return false; v = Val::i_(offset); return true;
        case P_virtualBaseClass: v = Val::b_(isVirtual); return true;
        case P_indirectVirtualBaseClass: v = Val::b_(indirect); return true;
        case P_virtualBaseDispIndex: if (!isVirtual) return false; v = Val::u_(vbIndex); return true;
        case P_virtualBasePointerOffset: if (!isVirtual) return false; v = Val::i_(vbptrOffset); return true;
        case P_virtualBaseTableType: if (!isVirtual) return false; v = Val::sym(vbptrType); return true;
        case P_name: case P_length: case P_udtKind: case P_constructor: case P_constType:
        case P_volatileType: case P_unalignedType: case P_constantExport: case P_hasAssignmentOperator:
        case P_hasCastOperator: case P_hasNestedTypes: case P_nested: case P_overloadedOperator:
        case P_packed: case P_scoped: case P_sealed: case P_exportIsForwarder: case P_hfaDouble:
        case P_hfaFloat: case P_intrinsic: case P_isInterfaceUdt: case P_isRefUdt: case P_isValueUdt:
        case P_virtualTableShape: case P_virtualTableShapeId:
            return M->get(base, p, v);
        default: return false;
        }
    }
    void children(std::vector<SymIndexId> &out) const override { out = M->children(base); }
};

// LF_VFUNCTAB.
class OmVTable : public OmSym {
public:
    OmVTable(NativeSession &s, SymIndexId id, TypeIndex t) : OmSym(s, PDB_SymType::VTable, id), t(t) {}
    TypeIndex t;
    bool prop(Prop p, Val &v) const override
    {
        switch (p) {
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_classParent: case P_classParentId: v = Val::sym(classParent); return true;
        case P_type: case P_typeId: v = Val::sym(M->typeSym(t)); return true;
        case P_offset: v = Val::i_(0); return true;
        case P_constType: case P_volatileType: case P_unalignedType: case P_constantExport:
            v = Val::b_(false);
            return true;
        default: return false;
        }
    }
};

// An inlined call site (S_INLINESITE).
class OmInlineSite : public OmSym {
public:
    OmInlineSite(NativeSession &s, SymIndexId id, uint16_t modi, uint32_t off, uint32_t end)
        : OmSym(s, PDB_SymType::InlineSite, id), modi(modi), off(off), end(end) {}
    uint16_t modi;
    uint32_t off, end;
    uint16_t seg = 0;
    uint32_t start = 0;  // section offset of the first code range
    std::string nm;
    TypeIndex type;
    SymIndexId inlinee = 0, func = 0;
    bool prop(Prop p, Val &v) const override
    {
        uint32_t rva = M->rva(seg, start);
        switch (p) {
        case P_name: v = Val::str(nm); return true;
        case P_undecoratedName: v = Val::str(""); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_type: case P_typeId: v = Val::sym(M->typeSym(type)); return true;
        case P_inlinee: case P_inlineeId: v = Val::sym(inlinee); return true;
        case P_addressSection: v = Val::u_(seg); return true;
        case P_addressOffset: v = Val::u_(start); return true;
        case P_relativeVirtualAddress: v = Val::u_(rva); return true;
        case P_virtualAddress: v = Val::u_(rva + M->loadAddress); return true;
        case P_locationType: v = Val::u_((uint32_t)PDB_LocType::Static); return true;
        default: return false;
        }
    }
    void children(std::vector<SymIndexId> &out) const override;
};

// The inlinee of an inline site: the real 14.30 DLL returns a symbol with
// SymTagNull here.
class OmInlinee : public OmSym {
public:
    OmInlinee(NativeSession &s, SymIndexId id) : OmSym(s, PDB_SymType::None, id) {}
    bool prop(Prop, Val &) const override { return false; }
};

// A nested type entry (LF_NESTTYPE) of a class: the type under its nested
// name, with the class as class parent.
class OmNested : public OmSym {
public:
    OmNested(NativeSession &s, SymIndexId id, PDB_SymType tag, SymIndexId target, std::string nm)
        : OmSym(s, tag, id), target(target), nm(std::move(nm)) {}
    SymIndexId target;
    std::string nm;
    bool prop(Prop p, Val &v) const override
    {
        switch (p) {
        case P_name: v = Val::str(nm); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_classParent: case P_classParentId: v = Val::sym(classParent); return true;
        default: return M->get(target, p, v);
        }
    }
    void children(std::vector<SymIndexId> &out) const override { out = M->children(target); }
};

// A class method without an implementation in the PDB (declaration only).
class OmMethod : public OmSym {
public:
    OmMethod(NativeSession &s, SymIndexId id, OneMethodRecord m)
        : OmSym(s, PDB_SymType::Function, id), m(std::move(m)) {}
    OneMethodRecord m;
    bool prop(Prop p, Val &v) const override
    {
        MethodKind mk = m.getMethodKind();
        switch (p) {
        case P_name: v = Val::str(m.getName().str()); return true;
        case P_undecoratedName: v = Val::str(M->functionSignature(getSymIndexId(), false)); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_classParent: case P_classParentId: v = Val::sym(classParent); return true;
        case P_type: case P_typeId: v = Val::sym(M->typeSym(m.getType())); return true;
        case P_access: v = Val::u_((uint32_t)m.getAccess()); return true;
        case P_virtual:
            v = Val::b_(mk == MethodKind::Virtual || mk == MethodKind::IntroducingVirtual ||
                        mk == MethodKind::PureVirtual || mk == MethodKind::PureIntroducingVirtual);
            return true;
        case P_pure: v = Val::b_(mk == MethodKind::PureVirtual || mk == MethodKind::PureIntroducingVirtual); return true;
        case P_intro: v = Val::b_(mk == MethodKind::IntroducingVirtual || mk == MethodKind::PureIntroducingVirtual); return true;
        case P_isStaticMemberFunc: v = Val::b_(mk == MethodKind::Static); return true;
        case P_compilerGenerated: v = Val::b_(has(m.getOptions(), MethodOptions::CompilerGenerated)); return true;
        case P_sealed: v = Val::b_(has(m.getOptions(), MethodOptions::Sealed)); return true;
        case P_virtualBaseOffset: v = Val::u_(m.isIntroducingVirtual() ? m.getVFTableOffset() : 0); return true;
        default: return false;
        }
    }
};

template <class T, class... A> SymIndexId Model::make(A &&...args)
{
    SymIndexId id = ns.getSymbolCache().createSymbol<T>(std::forward<A>(args)...);
    auto *s = static_cast<OmSym *>(&ns.getSymbolCache().getNativeSymbolById(id));
    s->M = this;
    omIndex[id] = s;
    return id;
}

// ------------------------------------------------------------------ model

Model::Model(std::unique_ptr<IPDBSession> s)
    : ns(static_cast<NativeSession &>(*s)), owner(std::move(s)) {}

Model::~Model() = default;

std::unique_ptr<Model> Model::open(const std::string &path, std::string &err)
{
    std::unique_ptr<IPDBSession> s;
    if (auto e = NativeSession::createFromPdbPath(path, s)) {
        err = toString(std::move(e));
        return nullptr;
    }
    std::unique_ptr<Model> m(new Model(std::move(s)));
    m->pdbPath = path;
    m->exe = m->ns.getNativeGlobalScope().getSymIndexId();
    return m;
}

bool Model::valid(SymIndexId id) const
{
    return id != 0 && ns.getSymbolCache().getSymbolById(id) != nullptr;
}

NativeRawSymbol &Model::raw(SymIndexId id) const { return ns.getSymbolCache().getNativeSymbolById(id); }

PDB_SymType Model::tag(SymIndexId id) const { return valid(id) ? raw(id).getSymTag() : PDB_SymType::None; }

OmSym *Model::ours(SymIndexId id) const
{
    auto it = omIndex.find(id);
    return it == omIndex.end() ? nullptr : it->second;
}

uint32_t Model::rva(uint32_t sect, uint32_t off) const { return ns.getRVAFromSectOffset(sect, off); }

bool Model::sectOff(uint32_t r, uint32_t &sect, uint32_t &off) const
{
    return ns.addressForRVA(r, sect, off);
}

SymIndexId Model::typeSym(TypeIndex ti)
{
    return ns.getSymbolCache().findSymbolByTypeIndex(fullDecl(ti));
}

// Forward references resolved to their definition. LLVM's
// TpiStream::findFullDeclForForwardRef relies on the TPI hash buckets and
// misses some definitions in lld-linked optimized PDBs; this falls back to
// matching the unique name (or name) over the whole stream.
TypeIndex Model::fullDecl(TypeIndex ti)
{
    if (ti.isSimple() || ti.isNoneType()) return ti;
    auto *types = tpiTypes(ns);
    if (!types) return ti;
    CVType t = types->getType(ti);
    if (!isUdtForwardRef(t)) return ti;
    auto it = fwdCache.find(ti);
    if (it != fwdCache.end()) return it->second;
    if (fullByName.empty()) {
        fullByName.emplace("", TypeIndex::None());
        for (auto i = types->getFirst(); i; i = types->getNext(*i)) {
            CVType c = types->getType(*i);
            if (isUdtForwardRef(c)) continue;
            std::string key;
            switch (c.kind()) {
            case LF_CLASS: case LF_STRUCTURE: case LF_INTERFACE: {
                ClassRecord cr(TypeRecordKind::Class);
                if (!deserType(c, cr)) continue;
                key = std::to_string((int)c.kind()) + ":" + (cr.hasUniqueName() ? cr.getUniqueName() : cr.getName()).str();
                fullByName.emplace(std::to_string((int)c.kind()) + "=" + cr.getName().str(), *i);
                break;
            }
            case LF_UNION: {
                UnionRecord ur(TypeRecordKind::Union);
                if (!deserType(c, ur)) continue;
                key = std::to_string((int)c.kind()) + ":" + (ur.hasUniqueName() ? ur.getUniqueName() : ur.getName()).str();
                fullByName.emplace(std::to_string((int)c.kind()) + "=" + ur.getName().str(), *i);
                break;
            }
            case LF_ENUM: {
                EnumRecord er(TypeRecordKind::Enum);
                if (!deserType(c, er)) continue;
                key = std::to_string((int)c.kind()) + ":" + (er.hasUniqueName() ? er.getUniqueName() : er.getName()).str();
                fullByName.emplace(std::to_string((int)c.kind()) + "=" + er.getName().str(), *i);
                break;
            }
            default:
                continue;
            }
            fullByName.emplace(key, *i);
        }
    }
    std::string key, plain;
    switch (t.kind()) {
    case LF_CLASS: case LF_STRUCTURE: case LF_INTERFACE: {
        ClassRecord cr(TypeRecordKind::Class);
        if (deserType(t, cr))
            key = std::to_string((int)t.kind()) + ":" + (cr.hasUniqueName() ? cr.getUniqueName() : cr.getName()).str(),
            plain = std::to_string((int)t.kind()) + "=" + cr.getName().str();
        break;
    }
    case LF_UNION: {
        UnionRecord ur(TypeRecordKind::Union);
        if (deserType(t, ur))
            key = std::to_string((int)t.kind()) + ":" + (ur.hasUniqueName() ? ur.getUniqueName() : ur.getName()).str(),
            plain = std::to_string((int)t.kind()) + "=" + ur.getName().str();
        break;
    }
    case LF_ENUM: {
        EnumRecord er(TypeRecordKind::Enum);
        if (deserType(t, er))
            key = std::to_string((int)t.kind()) + ":" + (er.hasUniqueName() ? er.getUniqueName() : er.getName()).str(),
            plain = std::to_string((int)t.kind()) + "=" + er.getName().str();
        break;
    }
    default:
        break;
    }
    auto f = fullByName.find(key);
    if (f == fullByName.end() || f->second.isNoneType()) f = fullByName.find(plain);
    TypeIndex r = (f != fullByName.end() && !f->second.isNoneType()) ? f->second : ti;
    fwdCache[ti] = r;
    return r;
}

SymIndexId Model::noType()
{
    if (!noTypeId) noTypeId = typeSym(TypeIndex::None());
    return noTypeId;
}

std::string Model::name(SymIndexId id)
{
    Val v;
    if (get(id, P_name, v) && v.kind == Val::Str) return v.s;
    return "";
}

bool Model::get(SymIndexId id, Prop p, Val &v)
{
    if (!valid(id)) return false;
    if (p == P_symIndexId) { v = Val::u_(id); return true; }
    if (p == P_symTag) { v = Val::u_((uint32_t)tag(id)); return true; }
    if (propKinds[p] == K_SYM) {
        Prop ip = idPropOf(p);
        if (ip != P_COUNT) {
            if (!get(id, ip, v)) return false;
            if (v.kind == Val::U) v = Val::sym((SymIndexId)v.u);
            return v.kind == Val::Sym && valid((SymIndexId)v.u);
        }
    }
    if (OmSym *o = ours(id)) {
        if (!o->prop(p, v)) return false;
        if (v.kind == Val::Sym && !valid((SymIndexId)v.u) && propKinds[p] != K_SYM) v = Val::u_(0);
        return true;
    }
    NativeRawSymbol &r = raw(id);
    PDB_SymType t = r.getSymTag();
    // Fix-ups where LLVM's native reader differs from the real DLL.
    switch (p) {
    case P_lexicalParentId:
        if (t == PDB_SymType::Exe) return false;
        if (t == PDB_SymType::Compiland || !(tagProps(t)[p])) {
            if (!tagProps(t)[p]) return false;
        }
        v = Val::sym(exe);
        return true;
    case P_name:
        if (t == PDB_SymType::Exe) {
            std::string n = pdbPath;
            size_t s = n.find_last_of("\\/");
            if (s != std::string::npos) n = n.substr(s + 1);
            size_t d = n.find_last_of('.');
            if (d != std::string::npos) n = n.substr(0, d);
            v = Val::str(n);
            return true;
        }
        if (t == PDB_SymType::ArrayType) { v = Val::str(""); return true; }
        break;
    case P_symbolsFileName:
        if (t == PDB_SymType::Exe) {
            std::string n = pdbPath;
            std::replace(n.begin(), n.end(), '/', '\\');
            v = Val::str(n);
            return true;
        }
        break;
    case P_isStripped:
        if (t == PDB_SymType::Exe) {
            // LLVM's hasPrivateSymbols() is the negation (see docs/plan.md).
            v = Val::b_(!r.hasPrivateSymbols());
            return true;
        }
        break;
    case P_constantExport:
        if (tagProps(t)[p]) { v = Val::b_(r.isConstType()); return true; }
        return false;
    case P_exportIsForwarder: case P_hfaDouble: case P_hfaFloat: case P_isWinRTPointer:
        if (tagProps(t)[p]) { v = Val::b_(false); return true; }
        return false;
    case P_classParentId:
        if (t == PDB_SymType::FunctionArg) break;
        if (t == PDB_SymType::FunctionSig || t == PDB_SymType::PointerType || t == PDB_SymType::UDT ||
            t == PDB_SymType::Enum) {
            SymIndexId c = r.getClassParentId();
            if (!c) return false;
            v = Val::sym(c);
            return true;
        }
        break;
    case P_unmodifiedTypeId:
        if (t == PDB_SymType::UDT || t == PDB_SymType::Enum) {
            SymIndexId u = r.getUnmodifiedTypeId();
            if (!u) return false;
            v = Val::sym(u);
            return true;
        }
        if (t == PDB_SymType::BuiltinType) {
            // The plain base type behind a const/volatile one.
            TypeIndex ti = typeIndexOf(id);
            if (auto *types = tpiTypes(ns))
                if (!ti.isNoneType() && !ti.isSimple() && types->getType(ti).kind() == LF_MODIFIER) {
                    ModifierRecord mr(TypeRecordKind::Modifier);
                    if (deserType(types->getType(ti), mr)) {
                        v = Val::sym(typeSym(mr.getModifiedType()));
                        return true;
                    }
                }
            v = Val::sym(id);
            return true;
        }
        break;
    case P_virtualTableShapeId:
        if (t == PDB_SymType::UDT) {
            if (r.getUdtKind() == PDB_UdtType::Union) return false;
            v = Val::sym(r.getVirtualTableShapeId());
            return true;
        }
        break;
    case P_noexcept:
        if (t == PDB_SymType::FunctionSig) {
            if (r.getClassParentId()) return false;
            v = Val::b_(false);
            return true;
        }
        break;
    case P_thisAdjust:
        if (t == PDB_SymType::FunctionSig) {
            if (!r.getClassParentId()) return false;
            v = Val::i_(r.getThisAdjust());
            return true;
        }
        break;
    case P_objectPointerType:
        if (t == PDB_SymType::FunctionSig) {
            if (!r.getClassParentId()) return false;
            auto *types = tpiTypes(ns);
            TypeIndex ti = typeIndexOf(id);
            if (!types || ti.isNoneType()) return false;
            MemberFunctionRecord mf(TypeRecordKind::MemberFunction);
            if (!deserType(types->getType(ti), mf) || mf.getThisType().isNoneType()) return false;
            v = Val::sym(typeSym(mf.getThisType()));
            return true;
        }
        break;
    case P_baseType:
        if (t == PDB_SymType::BuiltinType) {
            uint32_t bt, len;
            if (basicType(id, bt, len)) {
                v = Val::u_(bt);
                return true;
            }
        } else if (t == PDB_SymType::Enum) {
            Val ty;
            uint32_t bt, len;
            if (get(id, P_typeId, ty) && basicType((SymIndexId)ty.u, bt, len)) {
                v = Val::u_(bt);
                return true;
            }
        }
        break;
    case P_machineType:
        if (t == PDB_SymType::Exe) {
            auto dbi = ns.getPDBFile().getPDBDbiStream();
            if (!dbi) {
                consumeError(dbi.takeError());
                return false;
            }
            v = Val::u_((uint32_t)dbi->getMachineType());
            return true;
        }
        break;
    case P_signature:
        if (t == PDB_SymType::Exe) {
            auto info = ns.getPDBFile().getPDBInfoStream();
            if (!info) {
                consumeError(info.takeError());
                return false;
            }
            v = Val::u_(info->getSignature());
            return true;
        }
        break;
    case P_sealed:
        if (t == PDB_SymType::UDT || t == PDB_SymType::Enum) {
            v = Val::b_((udtOptions(id) & (uint16_t)ClassOptions::Sealed) != 0);
            return true;
        }
        break;
    default:
        break;
    }
    if (!tagProps(t)[p]) return false;
    if (!rawGet(r, p, v)) return false;
    if (p == P_unmodifiedTypeId && t == PDB_SymType::BuiltinType) {
        // The plain base type of a const/volatile one.
        SymIndexId self = id;
        (void)self;
    }
    return true;
}

// ------------------------------------------------------------------ records

CVSymbol Model::moduleRecord(uint16_t modi, uint32_t off)
{
    auto ms = ns.getModuleDebugStream(modi);
    if (!ms) {
        consumeError(ms.takeError());
        return CVSymbol();
    }
    return *ms->getSymbolArray().at(off);
}

uint32_t Model::moduleCount() const
{
    auto dbi = ns.getPDBFile().getPDBDbiStream();
    if (!dbi) {
        consumeError(dbi.takeError());
        return 0;
    }
    return dbi->modules().getModuleCount();
}

SymIndexId Model::compiland(uint16_t modi)
{
    auto c = ns.getSymbolCache().getOrCreateCompiland(modi);
    return c ? c->getSymIndexId() : 0;
}

int Model::moduleOf(SymIndexId compilandId) const
{
    for (uint32_t i = 0, n = moduleCount(); i < n; i++) {
        auto c = ns.getSymbolCache().getOrCreateCompiland(i);
        if (c && c->getSymIndexId() == compilandId) return (int)i;
    }
    return -1;
}

SymIndexId Model::procSym(uint16_t modi, uint32_t off)
{
    auto key = std::make_pair(modi, off);
    auto it = procByRecord.find(key);
    if (it != procByRecord.end()) return it->second;
    auto ms = ns.getModuleDebugStream(modi);
    if (!ms) {
        consumeError(ms.takeError());
        return 0;
    }
    CVSymbolArray syms = ms->getSymbolArray();
    auto I = syms.at(off);
    if (I == syms.end() || !isProc(I->kind())) return 0;
    ProcSym ps(SymbolRecordKind::ProcSym);
    if (!deser(*I, ps)) return 0;
    SymbolKind kind = I->kind();
    SymIndexId id = make<OmFunction>(modi, off, ps);
    auto *f = static_cast<OmFunction *>(ours(id));
    f->kind = kind;
    f->lexParent = compiland(modi);
    // Frame data: the first S_FRAMEPROC directly inside the procedure.
    for (auto J = std::next(I); J != syms.end() && J.offset() < ps.End; ++J) {
        if (J->kind() == S_FRAMEPROC) {
            f->hasFrame = deser(*J, f->fp);
            break;
        }
        if (opensScope(J->kind())) break;
    }
    // CPU type from the module's compile record.
    for (auto J = syms.begin(); J != syms.end(); ++J) {
        if (J->kind() == S_COMPILE3) {
            Compile3Sym cs(SymbolRecordKind::Compile3Sym);
            if (deser(*J, cs)) f->cpu = cs.Machine;
            break;
        }
        if (isProc(J->kind())) break;
    }
    // Member function: bind to the class's method record.
    if (!ps.FunctionType.isSimple()) {
        auto tpi = ns.getPDBFile().getPDBTpiStream();
        if (tpi) {
            CVType ft = tpi->typeCollection().getType(ps.FunctionType);
            if (ft.kind() == LF_MFUNCTION) {
                MemberFunctionRecord mf(TypeRecordKind::MemberFunction);
                if (deserType(ft, mf)) {
                    f->classParent = typeSym(mf.getClassType());
                    // Look for a method of that name and type in the class.
                    std::string qual = name(f->classParent);
                    std::string nm = ps.Name.str();
                    std::string shortName = nm;
                    if (!qual.empty() && nm.size() > qual.size() + 2 && nm.compare(0, qual.size(), qual) == 0 &&
                        nm.compare(qual.size(), 2, "::") == 0)
                        shortName = nm.substr(qual.size() + 2);
                    auto &u = raw(f->classParent);
                    (void)u;
                    // Walk the class field list.
                    CVType ct = tpi->typeCollection().getType(mf.getClassType());
                    TypeIndex fl;
                    if (ct.kind() == LF_CLASS || ct.kind() == LF_STRUCTURE || ct.kind() == LF_INTERFACE) {
                        ClassRecord cr(TypeRecordKind::Class);
                        if (deserType(ct, cr)) {
                            if (cr.isForwardRef()) {
                                auto full = tpi->findFullDeclForForwardRef(mf.getClassType());
                                if (full) {
                                    CVType c2 = tpi->typeCollection().getType(*full);
                                    ClassRecord cr2(TypeRecordKind::Class);
                                    if (deserType(c2, cr2)) fl = cr2.getFieldList();
                                } else {
                                    consumeError(full.takeError());
                                }
                            } else {
                                fl = cr.getFieldList();
                            }
                        }
                    }
                    if (!fl.isNoneType()) {
                        CVType flt = tpi->typeCollection().getType(fl);
                        FieldListRecord flr(TypeRecordKind::FieldList);
                        struct V : TypeVisitorCallbacks {
                            std::string want;
                            TypeIndex type;
                            LazyRandomTypeCollection *types;
                            OmFunction *f;
                            Error visitKnownMember(CVMemberRecord &, OneMethodRecord &m) override
                            {
                                if (m.getName() == want && m.getType() == type) set(m);
                                return Error::success();
                            }
                            Error visitKnownMember(CVMemberRecord &, OverloadedMethodRecord &m) override
                            {
                                if (m.getName() != want) return Error::success();
                                CVType ml = types->getType(m.getMethodList());
                                MethodOverloadListRecord list(TypeRecordKind::MethodOverloadList);
                                if (deserType(ml, list))
                                    for (auto &o : list.getMethods())
                                        if (o.getType() == type) set(o);
                                return Error::success();
                            }
                            void set(const OneMethodRecord &m)
                            {
                                f->isMethod = true;
                                f->shortName = want;
                                f->access = m.getAccess();
                                f->mkind = m.getMethodKind();
                                f->mopts = m.getOptions();
                                f->vftableOffset = m.isIntroducingVirtual() ? m.getVFTableOffset() : 0;
                            }
                        } vis;
                        vis.want = shortName;
                        vis.type = ps.FunctionType;
                        vis.types = &tpi->typeCollection();
                        vis.f = f;
                        if (deserType(flt, flr)) consumeError(visitMemberRecordStream(flr.Data, vis));
                    }
                }
            }
        } else {
            consumeError(tpi.takeError());
        }
    }
    procByRecord[key] = id;
    return id;
}

// Children of a scope in a module symbol stream: records in [begin, end)
// at the top level of that range.
void Model::moduleChildren(uint16_t modi, SymIndexId parent, uint32_t begin, uint32_t end,
                           std::vector<SymIndexId> &out, SymIndexId func)
{
    auto ms = ns.getModuleDebugStream(modi);
    if (!ms) {
        consumeError(ms.takeError());
        return;
    }
    CVSymbolArray syms = ms->getSymbolArray();
    for (auto I = begin ? syms.at(begin) : syms.begin(); I != syms.end() && (end == 0 || I.offset() < end); ++I) {
        SymbolKind k = I->kind();
        uint32_t off = I.offset();
        auto key = std::make_pair(modi, off);
        if (isProc(k)) {
            SymIndexId f = procSym(modi, off);
            if (f) out.push_back(f);
            ProcSym ps(SymbolRecordKind::ProcSym);
            if (deser(*I, ps) && ps.End > off) {
                I = syms.at(ps.End);
                if (I == syms.end()) break;
            }
            continue;
        }
        if (k == S_BLOCK32) {
            BlockSym bs(SymbolRecordKind::BlockSym);
            if (!deser(*I, bs)) continue;
            SymIndexId id;
            auto it = recordSyms.find(key);
            if (it != recordSyms.end()) {
                id = it->second;
            } else {
                id = make<OmBlock>(modi, off, bs);
                ours(id)->lexParent = parent;
                static_cast<OmBlock *>(ours(id))->func = func;
                recordSyms[key] = id;
            }
            out.push_back(id);
            if (bs.End > off) {
                I = syms.at(bs.End);
                if (I == syms.end()) break;
            }
            continue;
        }
        if (k == S_INLINESITE && func) {
            InlineSiteSym is(SymbolRecordKind::InlineSiteSym);
            if (!deser(*I, is)) continue;
            auto it = recordSyms.find(key);
            SymIndexId id;
            if (it != recordSyms.end()) {
                id = it->second;
            } else {
                id = make<OmInlineSite>(modi, off, is.End);
                auto *site = static_cast<OmInlineSite *>(ours(id));
                site->lexParent = parent;
                site->func = func;
                auto *fn = static_cast<OmFunction *>(ours(func));
                site->seg = fn->ps.Segment;
                site->start = fn->ps.CodeOffset;
                for (auto &a : is.annotations()) {
                    if (a.OpCode == BinaryAnnotationsOpCode::CodeOffset ||
                        a.OpCode == BinaryAnnotationsOpCode::ChangeCodeOffset ||
                        a.OpCode == BinaryAnnotationsOpCode::ChangeCodeOffsetAndLineOffset) {
                        site->start += a.U1;
                        break;
                    }
                    if (a.OpCode == BinaryAnnotationsOpCode::ChangeCodeLengthAndCodeOffset) {
                        site->start += a.U2;
                        break;
                    }
                }
                auto ipi = ns.getPDBFile().getPDBIpiStream();
                if (ipi) {
                    CVType ft = ipi->typeCollection().getType(is.Inlinee);
                    if (ft.kind() == LF_FUNC_ID) {
                        FuncIdRecord fr(TypeRecordKind::FuncId);
                        if (deserType(ft, fr)) {
                            site->nm = fr.getName().str();
                            site->type = fr.getFunctionType();
                            if (!fr.getParentScope().isNoneType()) {
                                StringIdRecord sr(TypeRecordKind::StringId);
                                if (deserType(ipi->typeCollection().getType(fr.getParentScope()), sr))
                                    site->nm = sr.getString().str() + "::" + site->nm;
                            }
                        }
                    } else if (ft.kind() == LF_MFUNC_ID) {
                        MemberFuncIdRecord fr(TypeRecordKind::MemberFuncId);
                        if (deserType(ft, fr)) {
                            site->nm = name(typeSym(fr.getClassType())) + "::" + fr.getName().str();
                            site->type = fr.getFunctionType();
                        }
                    }
                } else {
                    consumeError(ipi.takeError());
                }
                site->inlinee = make<OmInlinee>();
                recordSyms[key] = id;
            }
            out.push_back(id);
            if (is.End > off) {
                I = syms.at(is.End);
                if (I == syms.end()) break;
            }
            continue;
        }
        if (k == S_CONSTANT && func) {
            ConstantSym cs(SymbolRecordKind::ConstantSym);
            if (!deser(*I, cs)) continue;
            auto it = recordSyms.find(key);
            SymIndexId id;
            if (it != recordSyms.end()) {
                id = it->second;
            } else {
                id = make<OmData>(OmData::Constant);
                auto *d = static_cast<OmData *>(ours(id));
                d->nm = cs.Name.str();
                d->dataKind = PDB_DataKind::Constant;
                d->type = cs.Type;
                d->value = variantOf(cs.Value);
                d->lexParent = parent;
                d->localConst = true;
                recordSyms[key] = id;
            }
            out.push_back(id);
            continue;
        }
        if (opensScope(k)) {
            // Skip other scopes (thunks, separated code) for now.
            uint32_t e = 0;
            if (k == S_THUNK32) {
                Thunk32Sym t(SymbolRecordKind::Thunk32Sym);
                if (deser(*I, t)) e = t.End;
            } else if (k == S_INLINESITE) {
                InlineSiteSym t(SymbolRecordKind::InlineSiteSym);
                if (deser(*I, t)) e = t.End;
            }
            if (e > off) {
                I = syms.at(e);
                if (I == syms.end()) break;
            }
            continue;
        }
        if (k == S_LDATA32 || k == S_GDATA32 || k == S_LTHREAD32 || k == S_GTHREAD32) {
            if (!func && k == S_GDATA32) continue;  // module globals are listed under the exe
            DataSym ds(SymbolRecordKind::DataSym);
            if (!deser(*I, ds)) continue;
            auto gk = std::make_pair((uint16_t)0xffff, (uint32_t)0);
            (void)gk;
            SymIndexId id;
            auto it = recordSyms.find(key);
            if (it != recordSyms.end()) {
                id = it->second;
            } else {
                id = make<OmData>(OmData::Global);
                auto *d = static_cast<OmData *>(ours(id));
                d->nm = ds.Name.str();
                d->type = ds.Type;
                d->seg = ds.Segment;
                d->off = ds.DataOffset;
                d->hasAddr = true;
                d->dataKind = func ? PDB_DataKind::StaticLocal
                                   : (k == S_GDATA32 ? PDB_DataKind::Global : PDB_DataKind::FileStatic);
                d->lexParent = parent;
                recordSyms[key] = id;
            }
            out.push_back(id);
            continue;
        }
        if (func) continue;
        if (k == S_COMPILE3) {
            Compile3Sym cs(SymbolRecordKind::Compile3Sym);
            if (!deser(*I, cs)) continue;
            auto it = recordSyms.find(key);
            SymIndexId id = it != recordSyms.end() ? it->second : make<OmCompilandDetails>(cs);
            ours(id)->lexParent = parent;
            recordSyms[key] = id;
            out.push_back(id);
        }
    }
}

void OmFunction::children(std::vector<SymIndexId> &out) const
{
    Model &m = *M;
    auto key = std::make_pair(modi, off);
    (void)key;
    // FuncDebugStart / FuncDebugEnd.
    if (!dbgStart) {
        dbgStart = m.make<OmFuncDebug>(PDB_SymType::FuncDebugStart, this);
        dbgEnd = m.make<OmFuncDebug>(PDB_SymType::FuncDebugEnd, this);
        m.ours(dbgStart)->lexParent = getSymIndexId();
        m.ours(dbgEnd)->lexParent = getSymIndexId();
    }
    out.push_back(dbgStart);
    out.push_back(dbgEnd);
    auto ms = m.ns.getModuleDebugStream(modi);
    if (!ms) {
        consumeError(ms.takeError());
        return;
    }
    CVSymbolArray syms = ms->getSymbolArray();
    auto I = syms.at(off);
    uint32_t begin = std::next(I).offset();
    m.moduleChildren(modi, getSymIndexId(), begin, ps.End, out, getSymIndexId());
}

void OmInlineSite::children(std::vector<SymIndexId> &out) const
{
    auto ms = M->ns.getModuleDebugStream(modi);
    if (!ms) {
        consumeError(ms.takeError());
        return;
    }
    CVSymbolArray syms = ms->getSymbolArray();
    auto I = syms.at(off);
    M->moduleChildren(modi, getSymIndexId(), std::next(I).offset(), end, out, func);
}

void OmBlock::children(std::vector<SymIndexId> &out) const
{
    auto ms = M->ns.getModuleDebugStream(modi);
    if (!ms) {
        consumeError(ms.takeError());
        return;
    }
    CVSymbolArray syms = ms->getSymbolArray();
    auto I = syms.at(off);
    M->moduleChildren(modi, getSymIndexId(), std::next(I).offset(), bs.End, out, func);
}

}  // namespace om

// ------------------------------------------------------------------ part 2
namespace om {

namespace {

// A FunctionArgType: one argument of a FunctionType. LLVM creates these
// without a SymIndexId, so we keep our own.
class OmFuncArg : public OmSym {
public:
    OmFuncArg(NativeSession &s, SymIndexId id, TypeIndex t) : OmSym(s, PDB_SymType::FunctionArg, id), t(t) {}
    TypeIndex t;
    bool prop(Prop p, Val &v) const override
    {
        switch (p) {
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_classParent: case P_classParentId: v = Val::sym(classParent); return true;
        case P_type: case P_typeId: v = Val::sym(M->typeSym(t)); return true;
        default: return false;
        }
    }
};

}  // namespace

static LazyRandomTypeCollection *tpiTypes(NativeSession &ns)
{
    auto tpi = ns.getPDBFile().getPDBTpiStream();
    if (!tpi) {
        consumeError(tpi.takeError());
        return nullptr;
    }
    return &tpi->typeCollection();
}

static TpiStream *tpiStream(NativeSession &ns)
{
    auto tpi = ns.getPDBFile().getPDBTpiStream();
    if (!tpi) {
        consumeError(tpi.takeError());
        return nullptr;
    }
    return &*tpi;
}

uint16_t Model::udtOptions(SymIndexId id)
{
    CVType t;
    TypeIndex full;
    if (!resolveTag(*this, ns, typeIndexOf(id), t, full)) return 0;
    switch (t.kind()) {
    case LF_CLASS: case LF_STRUCTURE: case LF_INTERFACE: {
        ClassRecord cr(TypeRecordKind::Class);
        return deserType(t, cr) ? (uint16_t)cr.getOptions() : 0;
    }
    case LF_UNION: {
        UnionRecord ur(TypeRecordKind::Union);
        return deserType(t, ur) ? (uint16_t)ur.getOptions() : 0;
    }
    case LF_ENUM: {
        EnumRecord er(TypeRecordKind::Enum);
        return deserType(t, er) ? (uint16_t)er.getOptions() : 0;
    }
    default:
        return 0;
    }
}

TypeIndex Model::typeIndexOf(SymIndexId id)
{
    if (!typeIndexBuilt) {
        typeIndexBuilt = true;
        if (auto *types = tpiTypes(ns)) {
            for (auto ti = types->getFirst(); ti; ti = types->getNext(*ti)) {
                CVType t = types->getType(*ti);
                if (isUdtForwardRef(t)) continue;
                switch (t.kind()) {
                case LF_CLASS: case LF_STRUCTURE: case LF_UNION: case LF_INTERFACE: case LF_ENUM:
                case LF_POINTER: case LF_ARRAY: case LF_PROCEDURE: case LF_MFUNCTION: case LF_MODIFIER:
                case LF_VTSHAPE: {
                    SymIndexId s = typeSym(*ti);
                    if (s) typeIndexMap.emplace(s, *ti);
                    break;
                }
                default:
                    break;
                }
            }
        }
    }
    auto it = typeIndexMap.find(id);
    return it == typeIndexMap.end() ? TypeIndex::None() : it->second;
}

// The record of a (possibly modified) UDT or enum, resolved to its full
// definition.
static bool resolveTag(Model &m, NativeSession &ns, TypeIndex ti, CVType &out, TypeIndex &full)
{
    auto *types = tpiTypes(ns);
    if (!types || ti.isSimple() || ti.isNoneType()) return false;
    CVType t = types->getType(ti);
    if (t.kind() == LF_MODIFIER) {
        ModifierRecord mr(TypeRecordKind::Modifier);
        if (!deserType(t, mr)) return false;
        return resolveTag(m, ns, mr.getModifiedType(), out, full);
    }
    if (isUdtForwardRef(t)) {
        auto f = tpiStream(ns)->findFullDeclForForwardRef(ti);
        if (!f) {
            consumeError(f.takeError());
            return false;
        }
        ti = *f;
        t = types->getType(ti);
    }
    out = t;
    full = ti;
    return true;
}

void Model::typeMembers(SymIndexId udt, TypeIndex fieldList, std::vector<SymIndexId> &out)
{
    auto *types = tpiTypes(ns);
    if (!types || fieldList.isNoneType()) return;
    struct V : TypeVisitorCallbacks {
        Model *m;
        SymIndexId udt;
        std::vector<SymIndexId> *out;
        LazyRandomTypeCollection *types;
        uint32_t index = 0;
        bool isEnum = false;
        TypeIndex enumType;
        TypeIndex next;  // LF_INDEX continuation
        SymIndexId get(std::function<SymIndexId()> mk)
        {
            auto key = std::make_pair(udt, index++);
            auto it = m->memberSyms.find(key);
            if (it != m->memberSyms.end()) return it->second;
            SymIndexId id = mk();
            m->memberSyms[key] = id;
            return id;
        }
        Error visitKnownMember(CVMemberRecord &, DataMemberRecord &r) override
        {
            out->push_back(get([&] {
                SymIndexId id = m->make<OmData>(OmData::Member);
                auto *d = static_cast<OmData *>(m->ours(id));
                d->nm = r.getName().str();
                d->dataKind = PDB_DataKind::Member;
                d->memberOffset = (int32_t)r.getFieldOffset();
                d->access = r.getAccess();
                d->classParent = udt;
                d->lexParent = m->exe;
                d->type = r.getType();
                if (!r.getType().isSimple()) {
                    CVType bt = types->getType(r.getType());
                    if (bt.kind() == LF_BITFIELD) {
                        BitFieldRecord bf(TypeRecordKind::BitField);
                        if (deserType(bt, bf)) {
                            d->bitfield = true;
                            d->bitPos = bf.getBitOffset();
                            d->bitLen = bf.getBitSize();
                            d->type = bf.getType();
                        }
                    }
                }
                return id;
            }));
            return Error::success();
        }
        Error visitKnownMember(CVMemberRecord &, StaticDataMemberRecord &r) override
        {
            out->push_back(get([&] {
                SymIndexId id = m->make<OmData>(OmData::StaticMember);
                auto *d = static_cast<OmData *>(m->ours(id));
                d->nm = r.getName().str();
                d->dataKind = PDB_DataKind::StaticMember;
                d->access = r.getAccess();
                d->classParent = udt;
                d->lexParent = m->exe;
                d->type = r.getType();
                m->bindStaticMember(d);
                return id;
            }));
            return Error::success();
        }
        Error visitKnownMember(CVMemberRecord &, EnumeratorRecord &r) override
        {
            out->push_back(get([&] {
                SymIndexId id = m->make<OmData>(OmData::Enumerator);
                auto *d = static_cast<OmData *>(m->ours(id));
                d->nm = r.getName().str();
                d->dataKind = PDB_DataKind::Constant;
                d->classParent = udt;
                d->lexParent = m->exe;
                d->type = TypeIndex(SimpleTypeKind::Int32);  // as observed, whatever the underlying type
                d->value = variantOf(r.getValue());
                return id;
            }));
            return Error::success();
        }
        Error visitKnownMember(CVMemberRecord &, BaseClassRecord &r) override
        {
            out->push_back(get([&] {
                SymIndexId id = m->make<OmBaseClass>();
                auto *b = static_cast<OmBaseClass *>(m->ours(id));
                b->base = m->typeSym(r.getBaseType());
                b->access = r.getAccess();
                b->offset = (int32_t)r.getBaseOffset();
                b->classParent = udt;
                b->lexParent = m->exe;
                return id;
            }));
            return Error::success();
        }
        Error visitKnownMember(CVMemberRecord &cvr, VirtualBaseClassRecord &r) override
        {
            out->push_back(get([&] {
                SymIndexId id = m->make<OmBaseClass>();
                auto *b = static_cast<OmBaseClass *>(m->ours(id));
                b->base = m->typeSym(r.getBaseType());
                b->access = r.getAccess();
                b->isVirtual = true;
                b->indirect = cvr.Kind == LF_IVBCLASS;
                b->vbptrType = m->typeSym(r.getVBPtrType());
                b->vbptrOffset = (int32_t)r.getVBPtrOffset();
                b->vbIndex = (uint32_t)r.getVTableIndex();
                b->classParent = udt;
                b->lexParent = m->exe;
                return id;
            }));
            return Error::success();
        }
        Error visitKnownMember(CVMemberRecord &, VFPtrRecord &r) override
        {
            out->push_back(get([&] {
                SymIndexId id = m->make<OmVTable>(r.getType());
                m->ours(id)->classParent = udt;
                m->ours(id)->lexParent = m->exe;
                return id;
            }));
            return Error::success();
        }
        void method(const OneMethodRecord &r, StringRef name)
        {
            out->push_back(get([&] {
                SymIndexId impl = m->findMethodImpl(udt, name, r.getType());
                if (impl) return impl;
                OneMethodRecord copy = r;
                SymIndexId id = m->make<OmMethod>(copy);
                m->ours(id)->classParent = udt;
                m->ours(id)->lexParent = m->exe;
                return id;
            }));
        }
        Error visitKnownMember(CVMemberRecord &, OneMethodRecord &r) override
        {
            method(r, r.getName());
            return Error::success();
        }
        Error visitKnownMember(CVMemberRecord &, OverloadedMethodRecord &r) override
        {
            CVType ml = types->getType(r.getMethodList());
            MethodOverloadListRecord list(TypeRecordKind::MethodOverloadList);
            if (deserType(ml, list))
                for (auto &o : list.getMethods()) {
                    OneMethodRecord named(o.getType(), o.Attrs, o.getVFTableOffset(), r.getName());
                    method(named, r.getName());
                }
            return Error::success();
        }
        Error visitKnownMember(CVMemberRecord &, NestedTypeRecord &r) override
        {
            SymIndexId target = m->typeSym(r.getNestedType());
            PDB_SymType tt = m->tag(target);
            if (tt != PDB_SymType::UDT && tt != PDB_SymType::Enum) {
                index++;
                return Error::success();
            }
            out->push_back(get([&] {
                SymIndexId id = m->make<OmNested>(tt, target, r.getName().str());
                m->ours(id)->classParent = udt;
                m->ours(id)->lexParent = m->exe;
                return id;
            }));
            return Error::success();
        }
        Error visitKnownMember(CVMemberRecord &, ListContinuationRecord &r) override
        {
            next = r.getContinuationIndex();
            return Error::success();
        }
    } vis;
    vis.m = this;
    vis.udt = udt;
    vis.out = &out;
    vis.types = types;
    TypeIndex fl = fieldList;
    if (tag(udt) == PDB_SymType::Enum) {
        vis.isEnum = true;
        CVType et;
        TypeIndex full;
        if (resolveTag(*this, ns, typeIndexOf(udt), et, full)) {
            EnumRecord er(TypeRecordKind::Enum);
            if (deserType(et, er)) vis.enumType = er.getUnderlyingType();
        }
    }
    for (int guard = 0; !fl.isNoneType() && guard < 64; guard++) {
        CVType t = types->getType(fl);
        FieldListRecord flr(TypeRecordKind::FieldList);
        if (!deserType(t, flr)) break;
        vis.next = TypeIndex::None();
        consumeError(visitMemberRecordStream(flr.Data, vis));
        fl = vis.next;
    }
}

void Model::udtChildren(SymIndexId udt, std::vector<SymIndexId> &out)
{
    TypeIndex ti = typeIndexOf(udt);
    CVType t;
    TypeIndex full;
    if (!resolveTag(*this, ns, ti, t, full)) return;
    // Members belong to the unmodified type.
    SymIndexId owner = typeSym(full);
    TypeIndex fl;
    switch (t.kind()) {
    case LF_CLASS: case LF_STRUCTURE: case LF_INTERFACE: {
        ClassRecord cr(TypeRecordKind::Class);
        if (deserType(t, cr)) fl = cr.getFieldList();
        break;
    }
    case LF_UNION: {
        UnionRecord ur(TypeRecordKind::Union);
        if (deserType(t, ur)) fl = ur.getFieldList();
        break;
    }
    case LF_ENUM: {
        EnumRecord er(TypeRecordKind::Enum);
        if (deserType(t, er)) fl = er.getFieldList();
        break;
    }
    default:
        return;
    }
    typeMembers(owner, fl, out);
}

void Model::bindStaticMember(OmData *d)
{
    // A static data member is defined by a global with the qualified name.
    std::string q = name(d->classParent) + "::" + d->nm;
    auto gs = ns.getPDBFile().getPDBGlobalsStream();
    auto ss = ns.getPDBFile().getPDBSymbolStream();
    if (!gs || !ss) {
        if (!gs) consumeError(gs.takeError());
        if (!ss) consumeError(ss.takeError());
        return;
    }
    for (auto &r : gs->findRecordsByName(q, *ss)) {
        if (r.second.kind() == S_CONSTANT) {
            ConstantSym cs(SymbolRecordKind::ConstantSym);
            if (!deser(r.second, cs)) continue;
            d->staticConst = true;
            d->value = variantOf(cs.Value);
            return;
        }
        if (r.second.kind() != S_GDATA32 && r.second.kind() != S_LDATA32) continue;
        DataSym ds(SymbolRecordKind::DataSym);
        if (!deser(r.second, ds)) continue;
        d->hasAddr = true;
        d->seg = ds.Segment;
        d->off = ds.DataOffset;
        return;
    }
}

SymIndexId Model::findMethodImpl(SymIndexId udt, StringRef name, TypeIndex type)
{
    std::string q = this->name(udt) + "::" + name.str();
    auto gs = ns.getPDBFile().getPDBGlobalsStream();
    auto ss = ns.getPDBFile().getPDBSymbolStream();
    if (!gs || !ss) {
        if (!gs) consumeError(gs.takeError());
        if (!ss) consumeError(ss.takeError());
        return 0;
    }
    for (auto &r : gs->findRecordsByName(q, *ss)) {
        if (r.second.kind() != S_PROCREF && r.second.kind() != S_LPROCREF) continue;
        ProcRefSym pr(SymbolRecordKind::ProcRefSym);
        if (!deser(r.second, pr) || !pr.Module) continue;
        SymIndexId f = procSym(pr.modi(), pr.SymOffset);
        auto *fn = static_cast<OmFunction *>(ours(f));
        if (fn && fn->ps.FunctionType == type) return f;
    }
    return 0;
}

SymIndexId Model::dataSym(uint16_t seg, uint32_t off, StringRef nm, std::function<SymIndexId()> mk)
{
    auto key = std::make_tuple(seg, off, nm.str());
    auto it = dataByAddr.find(key);
    if (it != dataByAddr.end()) return it->second;
    SymIndexId id = mk();
    dataByAddr[key] = id;
    return id;
}

SymIndexId Model::globalData(const DataSym &ds, SymbolKind k)
{
    return dataSym(ds.Segment, ds.DataOffset, ds.Name, [&] {
        SymIndexId id = make<OmData>(OmData::Global);
        auto *d = static_cast<OmData *>(ours(id));
        d->nm = ds.Name.str();
        d->type = ds.Type;
        d->seg = ds.Segment;
        d->off = ds.DataOffset;
        d->hasAddr = true;
        // The globals stream's copy is a symbol of its own, under the exe,
        // whose findChildren fails with S_FALSE (as observed).
        d->dataKind = (k == S_GDATA32 || k == S_GTHREAD32) ? PDB_DataKind::Global : PDB_DataKind::FileStatic;
        d->lexParent = exe;
        d->fromGlobals = true;
        return id;
    });
}

void Model::compilandChildren(uint16_t modi, SymIndexId c, std::vector<SymIndexId> &out)
{
    auto ms = ns.getModuleDebugStream(modi);
    if (!ms) {
        consumeError(ms.takeError());
        return;
    }
    // Build environment: S_OBJNAME, S_ENVBLOCK, S_BUILDINFO.
    auto &env = envSyms[modi];
    if (env.empty()) {
        std::vector<std::pair<std::string, std::string>> kv;
        for (auto &rec : ms->getSymbolArray()) {
            if (rec.kind() == S_OBJNAME) {
                ObjNameSym on(SymbolRecordKind::ObjNameSym);
                if (deser(rec, on)) kv.push_back({"obj", on.Name.str()});
            } else if (rec.kind() == S_ENVBLOCK) {
                EnvBlockSym eb(SymbolRecordKind::EnvBlockSym);
                if (deser(rec, eb))
                    for (size_t i = 0; i + 1 < eb.Fields.size(); i += 2)
                        kv.push_back({eb.Fields[i].str(), eb.Fields[i + 1].str()});
            } else if (rec.kind() == S_BUILDINFO) {
                BuildInfoSym bi(SymbolRecordKind::BuildInfoSym);
                if (!deser(rec, bi)) continue;
                auto ipi = ns.getPDBFile().getPDBIpiStream();
                if (!ipi) {
                    consumeError(ipi.takeError());
                    continue;
                }
                auto &ids = ipi->typeCollection();
                BuildInfoRecord br(TypeRecordKind::BuildInfo);
                if (!deserType(ids.getType(bi.BuildId), br)) continue;
                static const char *keys[] = {"cwd", "cl", "src", "pdb", "cmd"};
                auto args = br.getArgs();
                for (size_t i = 0; i < args.size() && i < 5; i++) {
                    std::string val;
                    if (!args[i].isNoneType()) {
                        StringIdRecord sr(TypeRecordKind::StringId);
                        if (deserType(ids.getType(args[i]), sr)) val = sr.getString().str();
                    }
                    kv.push_back({keys[i], val});
                }
            }
        }
        for (auto &p : kv) {
            SymIndexId id = make<OmCompilandEnv>(p.first, p.second);
            ours(id)->lexParent = c;
            env.push_back(id);
        }
    }
    moduleChildren(modi, c, 0, 0, out, 0);
    out.insert(out.end(), env.begin(), env.end());
}

void Model::buildGlobalChildren(std::vector<SymIndexId> &out)
{
    SymbolCache &cache = ns.getSymbolCache();
    for (uint32_t i = 0, n = moduleCount(); i < n; i++)
        if (SymIndexId c = compiland(i)) out.push_back(c);
    auto addAll = [&](std::unique_ptr<IPDBEnumSymbols> e, std::vector<SymIndexId> *keep) {
        if (!e) return;
        while (auto s = e->getNext()) {
            out.push_back(s->getSymIndexId());
            if (keep) keep->push_back(s->getSymIndexId());
        }
    };
    std::vector<SymIndexId> udts, enums, sigs;
    addAll(cache.createTypeEnumerator({LF_STRUCTURE, LF_CLASS, LF_UNION, LF_INTERFACE}), &udts);
    addAll(cache.createTypeEnumerator(LF_ENUM), &enums);
    addAll(cache.createTypeEnumerator(LF_POINTER), nullptr);
    addAll(cache.createTypeEnumerator(LF_ARRAY), nullptr);
    addAll(cache.createTypeEnumerator({LF_PROCEDURE, LF_MFUNCTION}), &sigs);
    addAll(cache.createGlobalsEnumerator(S_UDT), nullptr);
    // const/volatile base types.
    if (auto *types = tpiTypes(ns)) {
        for (auto ti = types->getFirst(); ti; ti = types->getNext(*ti)) {
            CVType t = types->getType(*ti);
            if (t.kind() != LF_MODIFIER) continue;
            ModifierRecord mr(TypeRecordKind::Modifier);
            if (deserType(t, mr) && mr.getModifiedType().isSimple())
                if (SymIndexId s = typeSym(*ti)) out.push_back(s);
        }
    }
    // Globals stream.
    auto gs = ns.getPDBFile().getPDBGlobalsStream();
    auto ss = ns.getPDBFile().getPDBSymbolStream();
    if (gs && ss) {
        for (uint32_t off : gs->getGlobalsTable()) {
            CVSymbol s = ss->readRecord(off);
            switch (s.kind()) {
            case S_PROCREF: case S_LPROCREF: {
                ProcRefSym pr(SymbolRecordKind::ProcRefSym);
                if (deser(s, pr) && pr.Module)
                    if (SymIndexId f = procSym(pr.modi(), pr.SymOffset)) out.push_back(f);
                break;
            }
            case S_GDATA32: case S_LDATA32: case S_GTHREAD32: case S_LTHREAD32: {
                DataSym ds(SymbolRecordKind::DataSym);
                if (deser(s, ds)) out.push_back(globalData(ds, s.kind()));
                break;
            }
            case S_CONSTANT: {
                ConstantSym cs(SymbolRecordKind::ConstantSym);
                if (!deser(s, cs)) break;
                auto it = globalSyms.find(off);
                SymIndexId id;
                if (it != globalSyms.end()) {
                    id = it->second;
                } else {
                    id = make<OmData>(OmData::Constant);
                    auto *d = static_cast<OmData *>(ours(id));
                    d->nm = cs.Name.str();
                    d->dataKind = PDB_DataKind::Constant;
                    d->type = cs.Type;
                    d->value = variantOf(cs.Value);
                    d->lexParent = exe;
                    globalSyms[off] = id;
                }
                out.push_back(id);
                break;
            }
            default:
                break;
            }
        }
    } else {
        if (!gs) consumeError(gs.takeError());
        if (!ss) consumeError(ss.takeError());
    }
    for (SymIndexId s : sigs)
        for (SymIndexId a : children(s)) out.push_back(a);
    {
        size_t before = out.size();
        for (SymIndexId u : udts)
            for (SymIndexId m : children(u)) out.push_back(m);
        auto &fl = childFlags[exe];
        fl.assign(out.size(), 0);
        for (size_t i = before; i < out.size(); i++) fl[i] = 1;
    }
    for (SymIndexId e : enums)
        for (SymIndexId m : children(e)) out.push_back(m);
    for (SymIndexId p : publics()) out.push_back(p);
    childFlags[exe].resize(out.size(), 0);
}

std::vector<uint8_t> Model::flagsOf(SymIndexId id)
{
    const auto &c = children(id);
    PDB_SymType t = tag(id);
    if (t == PDB_SymType::UDT || t == PDB_SymType::BaseClass) return std::vector<uint8_t>(c.size(), 1);
    auto it = childFlags.find(id);
    if (it != childFlags.end() && it->second.size() == c.size()) return it->second;
    return std::vector<uint8_t>(c.size(), 0);
}

const std::vector<SymIndexId> &Model::publics()
{
    if (publicsLoaded) return publicList;
    publicsLoaded = true;
    auto ps = ns.getPDBFile().getPDBPublicsStream();
    auto ss = ns.getPDBFile().getPDBSymbolStream();
    if (!ps || !ss) {
        if (!ps) consumeError(ps.takeError());
        if (!ss) consumeError(ss.takeError());
        return publicList;
    }
    std::vector<OmPublic *> pubs;
    for (uint32_t off : ps->getPublicsTable()) {
        CVSymbol s = ss->readRecord(off);
        if (s.kind() != S_PUB32) continue;
        PublicSym32 p(SymbolRecordKind::PublicSym32);
        if (!deser(s, p)) continue;
        SymIndexId id = make<OmPublic>(p);
        ours(id)->lexParent = exe;
        publicList.push_back(id);
        pubs.push_back(static_cast<OmPublic *>(ours(id)));
    }
    // Length: up to the next public at a higher address in the same
    // section, but not past the end of the section contribution.
    std::vector<OmPublic *> sorted = pubs;
    std::sort(sorted.begin(), sorted.end(), [](OmPublic *a, OmPublic *b) {
        return std::make_pair(a->ps.Segment, a->ps.Offset) < std::make_pair(b->ps.Segment, b->ps.Offset);
    });
    const auto &sc = sectionContribs();
    for (size_t i = 0; i < sorted.size(); i++) {
        OmPublic *p = sorted[i];
        uint64_t end = UINT64_MAX;
        for (size_t j = i + 1; j < sorted.size(); j++) {
            if (sorted[j]->ps.Segment != p->ps.Segment) break;
            if (sorted[j]->ps.Offset > p->ps.Offset) {
                end = sorted[j]->ps.Offset;
                break;
            }
        }
        for (auto &c : sc) {
            if (c.sect == p->ps.Segment && p->ps.Offset >= c.off && p->ps.Offset < c.off + c.size) {
                end = std::min<uint64_t>(end, c.off + c.size);
                break;
            }
        }
        if (end == UINT64_MAX) end = p->ps.Offset;
        p->len = (uint32_t)(end - p->ps.Offset);
    }
    return publicList;
}

const std::vector<SymIndexId> &Model::children(SymIndexId id, bool *noChildren)
{
    static const std::vector<SymIndexId> empty;
    if (noChildren) *noChildren = false;
    if (!valid(id)) return empty;
    auto it = childCache.find(id);
    if (it != childCache.end()) {
        if (noChildren) *noChildren = noChildCache[id];
        return it->second;
    }
    std::vector<SymIndexId> out;
    bool none = false;
    PDB_SymType t = tag(id);
    if (id == exe) {
        buildGlobalChildren(out);
    } else if (OmSym *o = ours(id)) {
        none = o->noChildren();
        o->children(out);
    } else if (t == PDB_SymType::Compiland) {
        int modi = moduleOf(id);
        if (modi >= 0) compilandChildren((uint16_t)modi, id, out);
    } else if (t == PDB_SymType::UDT || t == PDB_SymType::Enum) {
        udtChildren(id, out);
    } else if (t == PDB_SymType::FunctionSig) {
        TypeIndex ti = typeIndexOf(id);
        auto *types = tpiTypes(ns);
        if (types && !ti.isNoneType()) {
            CVType ft = types->getType(ti);
            TypeIndex argList;
            if (ft.kind() == LF_PROCEDURE) {
                ProcedureRecord pr(TypeRecordKind::Procedure);
                if (deserType(ft, pr)) argList = pr.getArgumentList();
            } else if (ft.kind() == LF_MFUNCTION) {
                MemberFunctionRecord mr(TypeRecordKind::MemberFunction);
                if (deserType(ft, mr)) argList = mr.getArgumentList();
            }
            ArgListRecord al(TypeRecordKind::ArgList);
            if (!argList.isNoneType() && deserType(types->getType(argList), al)) {
                for (size_t i = 0; i < al.getIndices().size(); i++) {
                    auto key = std::make_pair(id, (uint32_t)i);
                    auto m = memberSyms.find(key);
                    SymIndexId a;
                    if (m != memberSyms.end()) {
                        a = m->second;
                    } else {
                        a = make<OmFuncArg>(al.getIndices()[i]);
                        ours(a)->classParent = id;
                        ours(a)->lexParent = exe;
                        memberSyms[key] = a;
                    }
                    out.push_back(a);
                }
            }
        }
    } else if (t == PDB_SymType::Typedef) {
        none = true;
    }
    noChildCache[id] = none;
    if (noChildren) *noChildren = none;
    return childCache[id] = std::move(out);
}

std::string Model::methodShortName(SymIndexId id)
{
    auto *f = dynamic_cast<OmFunction *>(ours(id));
    if (!f || !f->isMethod) return "";
    return f->shortName;
}

// ------------------------------------------------------------------ lines

const std::vector<LineEntry> &Model::lines(uint16_t modi)
{
    auto it = lineCache.find(modi);
    if (it != lineCache.end()) return it->second;
    auto &out = lineCache[modi];
    auto ms = ns.getModuleDebugStream(modi);
    if (!ms) {
        consumeError(ms.takeError());
        return out;
    }
    auto checksums = ms->findChecksumsSubsection();
    if (!checksums) {
        consumeError(checksums.takeError());
        return out;
    }
    files();
    auto strings = ns.getPDBFile().getStringTable();
    for (const auto &ss : ms->subsections()) {
        if (ss.kind() != DebugSubsectionKind::Lines) continue;
        DebugLinesSubsectionRef lines;
        BinaryStreamReader r(ss.getRecordData());
        if (auto e = lines.initialize(r)) {
            consumeError(std::move(e));
            continue;
        }
        const auto &hdr = *lines.header();
        for (const auto &block : lines) {
            uint32_t file = 0;
            auto ci = checksums->getArray().at(block.NameIndex);
            if (ci != checksums->getArray().end() && strings) {
                auto n = strings->getStringForID(ci->FileNameOffset);
                if (n) {
                    auto f = fileIndex.find(n->str());
                    if (f != fileIndex.end()) file = f->second;
                } else {
                    consumeError(n.takeError());
                }
            }
            const auto &ln = block.LineNumbers;
            for (size_t i = 0; i < ln.size(); i++) {
                LineInfo li(ln[i].Flags);
                LineEntry e;
                e.sect = hdr.RelocSegment;
                e.off = hdr.RelocOffset + ln[i].Offset;
                uint32_t next = i + 1 < ln.size() ? ln[i + 1].Offset : hdr.CodeSize;
                e.len = next - ln[i].Offset;
                e.line = li.getStartLine();
                e.lineEnd = li.getEndLine();
                e.statement = li.isStatement();
                if (i < block.Columns.size()) {
                    e.col = block.Columns[i].StartColumn;
                    e.colEnd = block.Columns[i].EndColumn;
                }
                e.modi = modi;
                e.file = file;
                out.push_back(e);
            }
        }
    }
    if (!strings) consumeError(strings.takeError());
    return out;
}

std::vector<LineEntry> Model::linesByRVA(uint32_t r, uint32_t len)
{
    std::vector<LineEntry> out;
    for (uint32_t i = 0, n = moduleCount(); i < n; i++)
        for (auto &e : lines((uint16_t)i)) {
            uint32_t a = rva(e.sect, e.off);
            if (a + e.len > r && a < r + len) out.push_back(e);
        }
    std::sort(out.begin(), out.end(), [&](const LineEntry &a, const LineEntry &b) {
        return rva(a.sect, a.off) < rva(b.sect, b.off);
    });
    return out;
}

const std::vector<SourceFile> &Model::files()
{
    if (filesLoaded) return fileList;
    filesLoaded = true;
    auto strings = ns.getPDBFile().getStringTable();
    if (!strings) {
        consumeError(strings.takeError());
        return fileList;
    }
    for (uint32_t i = 0, n = moduleCount(); i < n; i++) {
        auto ms = ns.getModuleDebugStream(i);
        if (!ms) {
            consumeError(ms.takeError());
            continue;
        }
        auto checksums = ms->findChecksumsSubsection();
        if (!checksums) {
            consumeError(checksums.takeError());
            continue;
        }
        for (const auto &c : checksums->getArray()) {
            auto nm = strings->getStringForID(c.FileNameOffset);
            if (!nm) {
                consumeError(nm.takeError());
                continue;
            }
            auto f = fileIndex.find(nm->str());
            uint32_t idx;
            if (f == fileIndex.end()) {
                idx = (uint32_t)fileList.size();
                SourceFile sf;
                sf.name = nm->str();
                sf.checksumKind = (uint32_t)c.Kind;
                sf.checksum.assign(c.Checksum.begin(), c.Checksum.end());
                fileList.push_back(sf);
                fileIndex[sf.name] = idx;
            } else {
                idx = f->second;
            }
            auto &mods = fileList[idx].modules;
            if (std::find(mods.begin(), mods.end(), (uint16_t)i) == mods.end()) mods.push_back((uint16_t)i);
        }
    }
    return fileList;
}

std::vector<uint32_t> Model::filesOfModule(uint16_t modi)
{
    std::vector<uint32_t> out;
    const auto &fl = files();
    for (uint32_t i = 0; i < fl.size(); i++)
        if (std::find(fl[i].modules.begin(), fl[i].modules.end(), modi) != fl[i].modules.end()) out.push_back(i);
    return out;
}

const std::vector<SectionContrib> &Model::sectionContribs()
{
    if (contribsLoaded) return contribs;
    contribsLoaded = true;
    auto dbi = ns.getPDBFile().getPDBDbiStream();
    if (!dbi) {
        consumeError(dbi.takeError());
        return contribs;
    }
    struct V : ISectionContribVisitor {
        std::vector<om::SectionContrib> *out;
        void visit(const pdb::SectionContrib &c) override
        {
            om::SectionContrib s;
            s.sect = c.ISect;
            s.off = c.Off;
            s.size = c.Size;
            s.characteristics = c.Characteristics;
            s.modi = c.Imod;
            s.dataCrc = c.DataCrc;
            s.relocCrc = c.RelocCrc;
            out->push_back(s);
        }
        void visit(const pdb::SectionContrib2 &c) override { visit(c.Base); }
    } v;
    v.out = &contribs;
    dbi->visitSectionContributions(v);
    return contribs;
}

uint32_t Model::segmentCount()
{
    auto dbi = ns.getPDBFile().getPDBDbiStream();
    if (!dbi) {
        consumeError(dbi.takeError());
        return 0;
    }
    return (uint32_t)dbi->getSectionMap().size();
}

bool Model::basicType(SymIndexId id, uint32_t &bt, uint32_t &len)
{
    // DIA's BasicType for each CodeView simple type kind.
    struct M { SimpleTypeKind k; uint32_t bt, len; };
    static const M table[] = {
        {SimpleTypeKind::None, 0, 0},           {SimpleTypeKind::Void, 1, 0},
        {SimpleTypeKind::NotTranslated, 0, 0},  {SimpleTypeKind::HResult, 31, 4},
        {SimpleTypeKind::SignedCharacter, 6, 1}, {SimpleTypeKind::UnsignedCharacter, 7, 1},
        {SimpleTypeKind::NarrowCharacter, 2, 1}, {SimpleTypeKind::WideCharacter, 3, 2},
        {SimpleTypeKind::Character16, 32, 2},   {SimpleTypeKind::Character32, 33, 4},
        {SimpleTypeKind::Character8, 34, 1},    {SimpleTypeKind::SByte, 6, 1},
        {SimpleTypeKind::Byte, 7, 1},           {SimpleTypeKind::Int16Short, 6, 2},
        {SimpleTypeKind::UInt16Short, 7, 2},    {SimpleTypeKind::Int16, 6, 2},
        {SimpleTypeKind::UInt16, 7, 2},         {SimpleTypeKind::Int32Long, 13, 4},
        {SimpleTypeKind::UInt32Long, 14, 4},    {SimpleTypeKind::Int32, 6, 4},
        {SimpleTypeKind::UInt32, 7, 4},         {SimpleTypeKind::Int64Quad, 6, 8},
        {SimpleTypeKind::UInt64Quad, 7, 8},     {SimpleTypeKind::Int64, 6, 8},
        {SimpleTypeKind::UInt64, 7, 8},         {SimpleTypeKind::Int128Oct, 6, 16},
        {SimpleTypeKind::UInt128Oct, 7, 16},    {SimpleTypeKind::Int128, 6, 16},
        {SimpleTypeKind::UInt128, 7, 16},       {SimpleTypeKind::Float16, 8, 2},
        {SimpleTypeKind::Float32, 8, 4},        {SimpleTypeKind::Float64, 8, 8},
        {SimpleTypeKind::Float80, 8, 10},       {SimpleTypeKind::Float128, 8, 16},
        {SimpleTypeKind::Boolean8, 10, 1},      {SimpleTypeKind::Boolean16, 10, 2},
        {SimpleTypeKind::Boolean32, 10, 4},     {SimpleTypeKind::Boolean64, 10, 8},
    };
    if (simpleKinds.empty()) {
        for (auto &e : table)
            if (SymIndexId s = typeSym(TypeIndex(e.k))) simpleKinds.emplace(s, (uint32_t)e.k);
        simpleKinds.emplace(0, 0);
    }
    uint32_t kind;
    auto it = simpleKinds.find(id);
    if (it != simpleKinds.end()) {
        kind = it->second;
    } else {
        TypeIndex ti = typeIndexOf(id);
        auto *types = tpiTypes(ns);
        if (!types || ti.isNoneType() || ti.isSimple() || types->getType(ti).kind() != LF_MODIFIER) return false;
        ModifierRecord mr(TypeRecordKind::Modifier);
        if (!deserType(types->getType(ti), mr) || !mr.getModifiedType().isSimple()) return false;
        kind = (uint32_t)mr.getModifiedType().getSimpleKind();
    }
    for (auto &e : table)
        if ((uint32_t)e.k == kind) {
            bt = e.bt;
            len = e.len;
            return true;
        }
    return false;
}

std::vector<DebugStream> Model::debugStreams()
{
    std::vector<DebugStream> out;
    auto dbi = ns.getPDBFile().getPDBDbiStream();
    if (!dbi) {
        consumeError(dbi.takeError());
        return out;
    }
    struct K { DbgHeaderType t; const char *name; uint32_t size; };
    static const K kinds[] = {
        {DbgHeaderType::FPO, "FPO", 16},          {DbgHeaderType::Exception, "EXCEPTION", 1},
        {DbgHeaderType::Fixup, "FIXUP", 10},      {DbgHeaderType::OmapToSrc, "OMAPTO", 8},
        {DbgHeaderType::OmapFromSrc, "OMAPFROM", 8}, {DbgHeaderType::SectionHdr, "SECTIONHEADERS", 40},
        {DbgHeaderType::TokenRidMap, "TOKENRIDMAP", 4}, {DbgHeaderType::Xdata, "XDATA", 1},
        {DbgHeaderType::Pdata, "PDATA", 12},     {DbgHeaderType::NewFPO, "FRAMEDATA", 32},
        {DbgHeaderType::SectionHdrOrig, "SECTIONHEADERS_ORIG", 40},
    };
    for (auto &k : kinds) {
        uint32_t idx = dbi->getDebugStreamIndex(k.t);
        if (idx == kInvalidStreamIndex || idx >= ns.getPDBFile().getNumStreams()) continue;
        DebugStream d;
        d.name = k.name;
        d.recordSize = k.size;
        uint32_t size = ns.getPDBFile().getStreamByteSize(idx);
        auto stream = ns.getPDBFile().createIndexedStream(idx);
        if (!stream) continue;
        d.data.resize(size);
        BinaryStreamReader r(*stream);
        ArrayRef<uint8_t> bytes;
        if (auto e = r.readBytes(bytes, size)) {
            consumeError(std::move(e));
            continue;
        }
        memcpy(d.data.data(), bytes.data(), size);
        out.push_back(std::move(d));
    }
    return out;
}

// ------------------------------------------------------------------ addresses

void Model::buildAddressMap()
{
    if (addrBuilt) return;
    addrBuilt = true;
    auto entry = [&](SymIndexId k, AddrEntry &e) {
        Val a, l, sec;
        if (!get(k, P_relativeVirtualAddress, a) || !get(k, P_addressSection, sec)) return false;
        e.rva = (uint32_t)a.u;
        e.sect = (uint32_t)sec.u;
        e.len = 0;
        if (get(k, P_length, l)) {
            e.len = (uint32_t)l.u;
        } else {
            Val ty;
            if (get(k, P_type, ty) && get((SymIndexId)ty.u, P_length, l)) e.len = (uint32_t)l.u;
        }
        e.id = k;
        e.tag = tag(k);
        e.name = name(k);
        return true;
    };
    // Functions, their blocks and static locals; module-level data.
    std::function<void(SymIndexId, SymIndexId)> scope = [&](SymIndexId parent, SymIndexId func) {
        for (SymIndexId k : children(parent)) {
            PDB_SymType kt = tag(k);
            AddrEntry e;
            if (kt == PDB_SymType::Function && ours(k) && !func) {
                if (!entry(k, e)) continue;
                funcMap.push_back(e);
                scope(k, k);
            } else if (kt == PDB_SymType::Block) {
                if (!entry(k, e)) continue;
                e.parent = func;
                blockMap.push_back(e);
                scope(k, func);
            } else if (kt == PDB_SymType::Data) {
                if (entry(k, e)) dataMap.push_back(e);
            }
        }
    };
    for (SymIndexId c : children(exe)) {
        PDB_SymType t = tag(c);
        AddrEntry e;
        if (t == PDB_SymType::Compiland) {
            scope(c, 0);
        } else if (t == PDB_SymType::PublicSymbol) {
            if (entry(c, e)) publicMap.push_back(e);
        } else if (t == PDB_SymType::Data) {
            auto *d = static_cast<OmData *>(ours(c));
            if (d && d->fromGlobals && d->dataKind == PDB_DataKind::Global && entry(c, e)) dataMap.push_back(e);
        }
    }
    auto byAddr = [](const AddrEntry &a, const AddrEntry &b) {
        if (a.rva != b.rva) return a.rva < b.rva;
        return a.name < b.name;
    };
    for (auto *m : {&funcMap, &blockMap, &dataMap, &publicMap}) std::stable_sort(m->begin(), m->end(), byAddr);
    // Symbols by address: functions and data, and publics where neither is.
    std::set<uint32_t> covered;
    for (auto *m : {&funcMap, &dataMap})
        for (auto &e : *m) {
            addrMap.push_back(e);
            covered.insert(e.rva);
        }
    for (auto &e : publicMap)
        if (!covered.count(e.rva)) {
            addrMap.push_back(e);
            covered.insert(e.rva);
        }
    std::stable_sort(addrMap.begin(), addrMap.end(), [](const AddrEntry &a, const AddrEntry &b) {
        return a.rva < b.rva;
    });
}

SymIndexId Model::findByRVA(uint32_t r, PDB_SymType want, int32_t *disp)
{
    buildAddressMap();
    uint32_t sect = 0, off = 0;
    if (!sectOff(r, sect, off)) return 0;
    auto containing = [&](const std::vector<AddrEntry> &m) -> const AddrEntry * {
        const AddrEntry *best = nullptr;
        for (auto &e : m)
            if (r >= e.rva && r < e.rva + e.len && (!best || e.rva >= best->rva)) best = &e;
        return best;
    };
    auto preceding = [&](const std::vector<AddrEntry> &m) -> const AddrEntry * {
        const AddrEntry *best = nullptr;
        for (auto &e : m)
            if (e.sect == sect && e.rva <= r && (!best || e.rva > best->rva)) best = &e;
        return best;
    };
    auto innermostBlock = [&](SymIndexId func) -> const AddrEntry * {
        const AddrEntry *best = nullptr;
        for (auto &e : blockMap)
            if (e.parent == func && r >= e.rva && r < e.rva + e.len && (!best || e.len <= best->len)) best = &e;
        return best;
    };
    const AddrEntry *hit = nullptr;
    switch (want) {
    case PDB_SymType::Function: hit = containing(funcMap); break;
    case PDB_SymType::Block:
        if (auto *f = containing(funcMap)) hit = innermostBlock(f->id);
        break;
    case PDB_SymType::Data: hit = preceding(dataMap); break;
    case PDB_SymType::PublicSymbol: hit = preceding(publicMap); break;
    case PDB_SymType::None:
        if (auto *f = containing(funcMap)) {
            hit = innermostBlock(f->id);
            if (!hit) hit = f;
        } else if (auto *d = containing(dataMap)) {
            hit = d;
        } else {
            hit = preceding(publicMap);
        }
        break;
    default:
        break;
    }
    if (!hit) return 0;
    if (disp) *disp = (int32_t)(r - hit->rva);
    return hit->id;
}

std::vector<SymIndexId> Model::symbolsByAddr()
{
    buildAddressMap();
    std::vector<SymIndexId> out;
    for (auto &e : addrMap) out.push_back(e.id);
    return out;
}

// ------------------------------------------------------------------ names

std::string Model::typeName(SymIndexId id)
{
    if (!valid(id)) return "";
    auto &r = raw(id);
    PDB_SymType t = r.getSymTag();
    auto cv = [&](std::string s) {
        Val v;
        std::string pre;
        if (get(id, P_constType, v) && v.u) pre += "const ";
        if (get(id, P_volatileType, v) && v.u) pre += "volatile ";
        return pre + s;
    };
    switch (t) {
    case PDB_SymType::BuiltinType: {
        Val bt, len;
        get(id, P_baseType, bt);
        get(id, P_length, len);
        std::string n;
        switch ((PDB_BuiltinType)bt.u) {
        case PDB_BuiltinType::Void: n = "void"; break;
        case PDB_BuiltinType::Char: n = "char"; break;
        case PDB_BuiltinType::WCharT: n = "wchar_t"; break;
        case PDB_BuiltinType::Char8: n = "char8_t"; break;
        case PDB_BuiltinType::Char16: n = "char16_t"; break;
        case PDB_BuiltinType::Char32: n = "char32_t"; break;
        case PDB_BuiltinType::Bool: n = "bool"; break;
        case PDB_BuiltinType::Float: n = len.u == 4 ? "float" : len.u == 8 ? "double" : "long double"; break;
        case PDB_BuiltinType::Int: case PDB_BuiltinType::Long:
            n = len.u == 1 ? "char" : len.u == 2 ? "short" : len.u == 4 ? (bt.u == (uint32_t)PDB_BuiltinType::Long ? "long" : "int") : "__int64";
            if (len.u == 1) n = "signed char";
            break;
        case PDB_BuiltinType::UInt: case PDB_BuiltinType::ULong:
            n = len.u == 1 ? "unsigned char" : len.u == 2 ? "unsigned short" : len.u == 4 ? (bt.u == (uint32_t)PDB_BuiltinType::ULong ? "unsigned long" : "unsigned int") : "unsigned __int64";
            break;
        default: n = "?"; break;
        }
        return cv(n);
    }
    case PDB_SymType::PointerType: {
        Val ty, ref, rref, c;
        get(id, P_type, ty);
        bool isRef = get(id, P_reference, ref) && ref.u;
        bool isRRef = get(id, P_RValueReference, rref) && rref.u;
        // As observed: references print as "& const", pointer cv is dropped.
        return typeName((SymIndexId)ty.u) + (isRRef ? " && const" : isRef ? " & const" : " *");
    }
    case PDB_SymType::UDT: {
        Val k;
        get(id, P_udtKind, k);
        const char *kw = k.u == (uint32_t)PDB_UdtType::Class ? "class " : k.u == (uint32_t)PDB_UdtType::Union ? "union " : "struct ";
        return cv(kw + name(id));
    }
    case PDB_SymType::Enum:
        return cv(name(id));
    case PDB_SymType::Typedef:
        return name(id);
    case PDB_SymType::ArrayType: {
        Val ty, n;
        get(id, P_type, ty);
        get(id, P_count, n);
        return typeName((SymIndexId)ty.u) + "[" + std::to_string(n.u) + "]";
    }
    case PDB_SymType::FunctionSig: {
        Val ty;
        get(id, P_type, ty);
        std::string s = typeName((SymIndexId)ty.u) + " (";
        bool first = true;
        for (SymIndexId a : children(id)) {
            Val at;
            get(a, P_type, at);
            s += (first ? "" : ", ") + typeName((SymIndexId)at.u);
            first = false;
        }
        return s + ")";
    }
    default:
        return name(id);
    }
}

std::string Model::functionSignature(SymIndexId fn, bool isStatic)
{
    std::string nm = name(fn);
    // A function with a public symbol of the same name keeps its name.
    if (auto *f = dynamic_cast<OmFunction *>(ours(fn))) {
        for (SymIndexId p : publics()) {
            auto *pub = static_cast<OmPublic *>(ours(p));
            if (pub->ps.Segment == f->ps.Segment && pub->ps.Offset == f->ps.CodeOffset && pub->ps.Name == f->ps.Name)
                return nm;
        }
    }
    Val ty;
    if (!get(fn, P_type, ty)) return nm;
    SymIndexId sig = (SymIndexId)ty.u;
    Val ret;
    get(sig, P_type, ret);
    std::string s = isStatic ? "static " : "";
    s += typeName((SymIndexId)ret.u) + " " + nm + "(";
    bool first = true;
    for (SymIndexId a : children(sig)) {
        Val at;
        get(a, P_type, at);
        s += (first ? "" : ", ") + typeName((SymIndexId)at.u);
        first = false;
    }
    return s + ")";
}

}  // namespace om
