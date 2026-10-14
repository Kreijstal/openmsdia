// OpenMSDIA symbol model: see model.h.
#include "model.h"
#include "undname.h"

#include <functional>

#include <algorithm>
#include <bitset>
#include <cstring>
#include <set>

#include "cvutil.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/DebugInfo/CodeView/CVTypeVisitor.h"
#include "llvm/DebugInfo/CodeView/DebugChecksumsSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/LazyRandomTypeCollection.h"
#include "llvm/DebugInfo/CodeView/TypeRecordHelpers.h"
#include "llvm/DebugInfo/CodeView/TypeVisitorCallbacks.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptor.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleList.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/GlobalsStream.h"
#include "llvm/DebugInfo/PDB/Native/ISectionContribVisitor.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/ModuleDebugStream.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PDBStringTable.h"
#include "llvm/DebugInfo/PDB/Native/PublicsStream.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/RawTypes.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/MemoryBuffer.h"

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

// get_undecoratedNameEx's options while P_undecoratedNameEx0 is computed
// (Model::undecoratedNameEx).
static thread_local unsigned g_exOptions = 0;

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

bool isProc(SymbolKind k)
{
    return k == S_GPROC32 || k == S_LPROC32 || k == S_GPROC32_ID || k == S_LPROC32_ID ||
           k == S_LPROC32_DPC || k == S_LPROC32_DPC_ID;
}

// The end offset of a scope-opening record (0 for others).
uint32_t scopeEnd(const CVSymbol &r)
{
    switch (r.kind()) {
    case S_GPROC32: case S_LPROC32: case S_GPROC32_ID: case S_LPROC32_ID: case S_LPROC32_DPC: case S_LPROC32_DPC_ID: {
        ProcSym ps(SymbolRecordKind::ProcSym);
        return deser(r, ps) ? ps.End : 0;
    }
    case S_BLOCK32: {
        BlockSym bs(SymbolRecordKind::BlockSym);
        return deser(r, bs) ? bs.End : 0;
    }
    case S_THUNK32: {
        Thunk32Sym t(SymbolRecordKind::Thunk32Sym);
        return deser(r, t) ? t.End : 0;
    }
    case S_INLINESITE: case S_INLINESITE2: {
        InlineSiteSym is(SymbolRecordKind::InlineSiteSym);
        return deser(r, is) ? is.End : 0;
    }
    case S_SEPCODE: {
        // No LLVM record class: pParent, pEnd, ... (CodeView layout).
        auto c = r.content();
        return c.size() >= 8 ? llvm::support::endian::read32le(c.data() + 4) : 0;
    }
    default:
        return 0;
    }
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
        r.type = v.isSigned() ? Variant::I1 : Variant::UI1;
    } else if (w <= 16) {
        r.type = Variant::I2;
        r.i = (int16_t)v.getZExtValue();
        return r;
    } else if (w <= 32) {
        r.type = v.isSigned() ? Variant::I4 : Variant::UI4;
    } else {
        r.type = v.isSigned() ? Variant::I8 : Variant::UI8;
    }
    if (v.isSigned()) r.i = v.getSExtValue(); else r.u = v.getZExtValue();
    return r;
}

}  // namespace

// ------------------------------------------------------------------ symbols

// A function from a module symbol stream (S_GPROC32 and friends).
class OmFunction : public Sym {
public:
    OmFunction(uint16_t modi, uint32_t off, ProcSym ps)
        : Sym(SymTag::Function), modi(modi), off(off), ps(std::move(ps)) {}
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
    bool overloaded = false;  // bound to a member of an overload set

    uint32_t start() const { return M->rva(ps.Segment, ps.CodeOffset); }

    bool prop(Prop p, Val &v) const override
    {
        bool isStatic = kind == S_LPROC32 || kind == S_LPROC32_ID || kind == S_LPROC32_DPC || kind == S_LPROC32_DPC_ID;
        switch (p) {
        case P_name: v = Val::str(ps.Name.str()); return true;
        case P_undecoratedName: v = Val::str(M->functionSignature(getSymIndexId(), isStatic, UND_NO_PTR64)); return true;
        case P_undecoratedNameEx0: v = Val::str(M->functionSignature(getSymIndexId(), isStatic, g_exOptions)); return true;
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
        case P_locationType: v = Val::u_((uint32_t)LocType::Static); return true;
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
        // The real DLL also calls static methods pure (observed with 14.30).
        case P_pure: if (!isMethod) return false;
            v = Val::b_(mkind == MethodKind::PureVirtual || mkind == MethodKind::PureIntroducingVirtual ||
                        mkind == MethodKind::Static);
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
        // constType & co. of overloaded methods: only through the class
        // (DiaSymbol::memberView), see Model::methodOverloaded.
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
            // S_FRAMEPROC flag bits as observed: 21 (GuardCfg) is
            // hasControlFlowCheck, 23 noexcept.
            if (p == P_hasControlFlowCheck) { v = Val::b_(F(FrameProcedureOptions::GuardCfg)); return true; }
            if (p == P_noexcept) { v = Val::b_(hasFrame && ((uint32_t)fp.Flags & (1u << 23))); return true; }
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
class OmFuncDebug : public Sym {
public:
    OmFuncDebug(SymTag tag, const OmFunction *fn)
        : Sym(tag), fn(fn) {}
    const OmFunction *fn;
    bool prop(Prop p, Val &v) const override
    {
        uint32_t delta = getSymTag() == SymTag::FuncDebugStart ? fn->ps.DbgStart : fn->ps.DbgEnd;
        switch (p) {
        case P_addressSection: v = Val::u_(fn->ps.Segment); return true;
        case P_addressOffset: v = Val::u_(fn->ps.CodeOffset + delta); return true;
        case P_relativeVirtualAddress: v = Val::u_(fn->start() + delta); return true;
        case P_virtualAddress: v = Val::u_(fn->start() + delta + M->loadAddress); return true;
        case P_locationType: v = Val::u_((uint32_t)LocType::Static); return true;
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
class OmBlock : public Sym {
public:
    OmBlock(uint16_t modi, uint32_t off, BlockSym bs)
        : Sym(SymTag::Block), modi(modi), off(off), bs(std::move(bs)) {}
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
        case P_locationType: v = Val::u_((uint32_t)LocType::Static); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        default: return false;
        }
    }
    void children(std::vector<SymIndexId> &out) const override;
};

// Data: globals, statics, members, static members, constants, enumerators.
class OmData : public Sym {
public:
    enum Kind { Global, Member, StaticMember, Constant, Enumerator, RegRel, Register };
    OmData(Kind k) : Sym(SymTag::Data), k(k) {}
    Kind k;
    DataKind dataKind = DataKind::Unknown;
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
    bool tls = false;         // S_GTHREAD32 / S_LTHREAD32
    uint32_t reg = 0;         // RegRel / Register
    int32_t regOffset = 0;
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
            if (k == Constant || k == Enumerator || staticConst) v = Val::u_((uint32_t)LocType::Constant);
            else if (k == Member) v = Val::u_((uint32_t)(bitfield ? LocType::BitField : LocType::ThisRel));
            else if (k == RegRel) v = Val::u_((uint32_t)LocType::RegRel);
            else if (k == Register) v = Val::u_((uint32_t)LocType::Enregistered);
            else v = Val::u_((uint32_t)(tls ? LocType::TLS : LocType::Static));
            return true;
        case P_offset:
            if (k == RegRel) { v = Val::i_(regOffset); return true; }
            if (k != Member) return false;
            v = Val::i_(memberOffset);
            return true;
        case P_registerId:
            if (k != RegRel && k != Register) return false;
            v = Val::u_(reg);
            return true;
        case P_bitPosition:
            if (!bitfield) return false;
            v = Val::u_(bitPos);
            return true;
        case P_addressSection: if (!hasAddr) return false; v = Val::u_(seg); return true;
        case P_addressOffset: if (!hasAddr) return false; v = Val::u_(off); return true;
        case P_relativeVirtualAddress: if (!hasAddr || tls) return false; v = Val::u_(M->rva(seg, off)); return true;
        case P_virtualAddress: if (!hasAddr || tls) return false; v = Val::u_(M->rva(seg, off) + M->loadAddress); return true;
        case P_baseType: case P_length:
            if (p == P_length && bitfield) { v = Val::u_(bitLen); return true; }
            if (k == Constant || k == Enumerator) {
                // Constants of enum type report the enum's base type and size (14.51).
                SymIndexId t = typeId ? typeId : M->typeSym(type);
                if (M->tag(t) != SymTag::Enum) return false;
                return M->get(t, p, v);
            }
            return false;
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
class OmPublic : public Sym {
public:
    OmPublic(PublicSym32 ps) : Sym(SymTag::PublicSymbol), ps(std::move(ps)) {}
    PublicSym32 ps;
    uint32_t len = 0;
    bool noChildren() const override { return true; }
    bool prop(Prop p, Val &v) const override
    {
        uint32_t rva = M->rva(ps.Segment, ps.Offset);
        bool code = has(ps.Flags, PublicSymFlags::Code) || has(ps.Flags, PublicSymFlags::Function);
        switch (p) {
        case P_name: v = Val::str(ps.Name.str()); return true;
        case P_undecoratedName: v = Val::str(diaUndecorate(ps.Name.str(), UND_NO_PTR64)); return true;
        case P_undecoratedNameEx0: v = Val::str(diaUndecorate(ps.Name.str(), g_exOptions)); return true;
        case P_addressSection: v = Val::u_(ps.Segment); return true;
        case P_addressOffset: v = Val::u_(ps.Offset); return true;
        case P_relativeVirtualAddress: v = Val::u_(rva); return true;
        case P_virtualAddress: v = Val::u_(rva + M->loadAddress); return true;
        case P_length: v = Val::u_(len); return true;
        case P_locationType: v = Val::u_((uint32_t)LocType::Static); return true;
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
class OmCompilandDetails : public Sym {
public:
    OmCompilandDetails(Compile3Sym cs)
        : Sym(SymTag::CompilandDetails), cs(std::move(cs)) {}
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
class OmCompilandEnv : public Sym {
public:
    OmCompilandEnv(std::string n, std::string val)
        : Sym(SymTag::CompilandEnv), n(std::move(n)), val(std::move(val)) {}
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
class OmBaseClass : public Sym {
public:
    OmBaseClass() : Sym(SymTag::BaseClass) {}
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
class OmVTable : public Sym {
public:
    OmVTable(TypeIndex t) : Sym(SymTag::VTable), t(t) {}
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
class OmInlineSite : public Sym {
public:
    OmInlineSite(uint16_t modi, uint32_t off, uint32_t end)
        : Sym(SymTag::InlineSite), modi(modi), off(off), end(end) {}
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
        // 14.51 fails get_inlinee with this error (14.30 returned a
        // SymTagNull symbol); inlineeId still names that symbol.
        case P_inlinee: v.kind = Val::Err; v.u = 0x806D000E; return true;
        case P_inlineeId: v = Val::sym(inlinee); return true;
        case P_addressSection: v = Val::u_(seg); return true;
        case P_addressOffset: v = Val::u_(start); return true;
        case P_relativeVirtualAddress: v = Val::u_(rva); return true;
        case P_virtualAddress: v = Val::u_(rva + M->loadAddress); return true;
        case P_locationType: v = Val::u_((uint32_t)LocType::Static); return true;
        default: return false;
        }
    }
    void children(std::vector<SymIndexId> &out) const override;
};

// The inlinee of an inline site: the real 14.30 DLL returns a symbol with
// SymTagNull here.
class OmInlinee : public Sym {
public:
    OmInlinee() : Sym(SymTag::None) {}
    bool prop(Prop, Val &) const override { return false; }
};

// One entry of an S_CALLEES record: a view of the called function, under
// the calling function. Without a definition in the PDB, only its name and
// type are known.
class OmCallee : public Sym {
public:
    // S_CALLEES entries are Callee symbols, S_CALLERS entries Caller
    // symbols (the latter not seen in any corpus PDB yet).
    OmCallee(std::string nm, TypeIndex type, SymIndexId fn, int index, SymTag tag = SymTag::Callee)
        : Sym(tag), nm(std::move(nm)), type(type), fn(fn), index(index) {}
    std::string nm;
    TypeIndex type;
    SymIndexId fn;
    int index;
    bool prop(Prop p, Val &v) const override
    {
        switch (p) {
        case P_name: v = Val::str(nm); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_PGOEdgeCount: v = Val::u_(0); return true;
        case P_undecoratedName: case P_undecoratedNameEx0: {
            std::string u;
            if (!fn || !M->publicSignature(fn, p == P_undecoratedNameEx0 ? g_exOptions : UND_NO_PTR64, u)) u.clear();
            v = Val::str(u);
            return true;
        }
        case P_type: case P_typeId:
            if (fn) return M->get(fn, P_typeId, v);
            v = Val::sym(M->typeSym(type));
            return true;
        case P_isStatic: case P_classParent: case P_classParentId: case P_coffGroup:
            return false;
        default:
            return fn && M->get(fn, p, v);
        }
    }
};

// S_COFFGROUP of the linker module.
class OmCoffGroup : public Sym {
public:
    OmCoffGroup(CoffGroupSym cg) : Sym(SymTag::CoffGroup), cg(std::move(cg)) {}
    CoffGroupSym cg;
    bool prop(Prop p, Val &v) const override
    {
        uint32_t rva = M->rva(cg.Segment, cg.Offset);
        switch (p) {
        case P_name: v = Val::str(cg.Name.str()); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_addressSection: v = Val::u_(cg.Segment); return true;
        case P_addressOffset: v = Val::u_(cg.Offset); return true;
        case P_relativeVirtualAddress: v = Val::u_(rva); return true;
        case P_virtualAddress: v = Val::u_(rva + M->loadAddress); return true;
        case P_length: v = Val::u_(cg.Size); return true;
        case P_characteristics: v = Val::u_(cg.Characteristics); return true;
        case P_locationType: v = Val::u_((uint32_t)LocType::Static); return true;
        default: return false;
        }
    }
};

// S_THUNK32.
class OmThunk : public Sym {
public:
    explicit OmThunk(Thunk32Sym t) : Sym(SymTag::Thunk), t(std::move(t)) {}
    Thunk32Sym t;
    bool prop(Prop p, Val &v) const override
    {
        uint32_t rva = M->rva(t.Segment, t.Offset);
        switch (p) {
        case P_name: v = Val::str(t.Name.str()); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_addressSection: v = Val::u_(t.Segment); return true;
        case P_addressOffset: v = Val::u_(t.Offset); return true;
        case P_relativeVirtualAddress: v = Val::u_(rva); return true;
        case P_virtualAddress: v = Val::u_(rva + M->loadAddress); return true;
        case P_length: v = Val::u_(t.Length); return true;
        case P_locationType: v = Val::u_((uint32_t)LocType::Static); return true;
        case P_thunkOrdinal: v = Val::u_((uint32_t)t.Thunk); return true;
        default: return false;
        }
    }
};

// S_LABEL32.
class OmLabel : public Sym {
public:
    explicit OmLabel(LabelSym l) : Sym(SymTag::Label), l(std::move(l)) {}
    LabelSym l;
    bool prop(Prop p, Val &v) const override
    {
        uint32_t rva = M->rva(l.Segment, l.CodeOffset);
        switch (p) {
        case P_name: v = Val::str(l.Name.str()); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_addressSection: v = Val::u_(l.Segment); return true;
        case P_addressOffset: v = Val::u_(l.CodeOffset); return true;
        case P_relativeVirtualAddress: v = Val::u_(rva); return true;
        case P_virtualAddress: v = Val::u_(rva + M->loadAddress); return true;
        case P_locationType: v = Val::u_((uint32_t)LocType::Static); return true;
        case P_framePointerPresent: v = Val::b_(has(l.Flags, ProcSymFlags::HasFP)); return true;
        case P_interruptReturn: v = Val::b_(has(l.Flags, ProcSymFlags::HasIRET)); return true;
        case P_farReturn: v = Val::b_(has(l.Flags, ProcSymFlags::HasFRET)); return true;
        case P_noReturn: v = Val::b_(has(l.Flags, ProcSymFlags::IsNoReturn)); return true;
        case P_notReached: v = Val::b_(has(l.Flags, ProcSymFlags::IsUnreachable)); return true;
        case P_customCallingConvention: v = Val::b_(has(l.Flags, ProcSymFlags::HasCustomCallingConv)); return true;
        case P_noInline: v = Val::b_(has(l.Flags, ProcSymFlags::IsNoInline)); return true;
        case P_optimizedCodeDebugInfo: v = Val::b_(has(l.Flags, ProcSymFlags::HasOptimizedDebugInfo)); return true;
        default: return false;
        }
    }
};

// S_UDT inside a function: a local typedef (lambda closure types).
class OmLocalTypedef : public Sym {
public:
    explicit OmLocalTypedef(UDTSym u) : Sym(SymTag::Typedef), u(std::move(u)) {}
    UDTSym u;
    bool prop(Prop p, Val &v) const override
    {
        switch (p) {
        case P_name: v = Val::str(u.Name.str()); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_type: case P_typeId: v = Val::sym(M->typeSym(u.Type)); return true;
        default: return false;
        }
    }
};

// S_UNAMESPACE.
class OmUsingNamespace : public Sym {
public:
    OmUsingNamespace(std::string nm)
        : Sym(SymTag::UsingNamespace), nm(std::move(nm)) {}
    std::string nm;
    bool prop(Prop p, Val &v) const override
    {
        switch (p) {
        case P_name: v = Val::str(nm); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        default: return false;
        }
    }
};

// S_CALLSITEINFO.
class OmCallSite : public Sym {
public:
    OmCallSite(CallSiteInfoSym cs)
        : Sym(SymTag::CallSite), cs(std::move(cs)) {}
    CallSiteInfoSym cs;
    bool prop(Prop p, Val &v) const override
    {
        switch (p) {
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_addressSection: v = Val::u_(cs.Segment); return true;
        case P_addressOffset: v = Val::u_(cs.CodeOffset); return true;
        case P_relativeVirtualAddress: v = Val::u_(M->rva(cs.Segment, cs.CodeOffset)); return true;
        case P_type: case P_typeId: v = Val::sym(M->typeSym(cs.Type)); return true;
        default: return false;
        }
    }
};

// A nested type entry (LF_NESTTYPE) of a class: the type under its nested
// name, with the class as class parent. A nested class or enum (one with
// the nested flag) is reported with its own tag; any other entry (a member
// typedef of a namespace-scope class, a base type, a pointer...) is a
// Typedef whose type is the target and which otherwise answers as the
// target, with nested set (as observed).
class OmNested : public Sym {
public:
    OmNested(SymTag tag, SymIndexId target, std::string nm)
        : Sym(tag), target(target), nm(std::move(nm)) {}
    SymIndexId target;
    std::string nm;
    bool prop(Prop p, Val &v) const override
    {
        switch (p) {
        case P_name: v = Val::str(nm); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_classParent: case P_classParentId: v = Val::sym(classParent); return true;
        case P_type: case P_typeId:
            if (getSymTag() != SymTag::Typedef) return M->get(target, p, v);
            v = Val::sym(target);
            return true;
        case P_nested:
            if (getSymTag() != SymTag::Typedef) return M->get(target, p, v);
            v = Val::b_(true);
            return true;
        default: return M->get(target, p, v);
        }
    }
    void children(std::vector<SymIndexId> &out) const override { out = M->children(target); }
};

// A class method without an implementation in the PDB (declaration only).
class OmMethod : public Sym {
public:
    OmMethod(OneMethodRecord m)
        : Sym(SymTag::Function), m(std::move(m)) {}
    OneMethodRecord m;
    bool overloaded = false;
    bool prop(Prop p, Val &v) const override
    {
        MethodKind mk = m.getMethodKind();
        switch (p) {
        case P_name: v = Val::str(m.getName().str()); return true;
        case P_undecoratedName: v = Val::str(M->functionSignature(getSymIndexId(), false, UND_NO_PTR64)); return true;
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(lexParent); return true;
        case P_classParent: case P_classParentId: v = Val::sym(classParent); return true;
        case P_type: case P_typeId: v = Val::sym(M->typeSym(m.getType())); return true;
        case P_access: v = Val::u_((uint32_t)m.getAccess()); return true;
        case P_virtual:
            v = Val::b_(mk == MethodKind::Virtual || mk == MethodKind::IntroducingVirtual ||
                        mk == MethodKind::PureVirtual || mk == MethodKind::PureIntroducingVirtual);
            return true;
        case P_pure:
            v = Val::b_(mk == MethodKind::PureVirtual || mk == MethodKind::PureIntroducingVirtual || mk == MethodKind::Static);
            return true;
        case P_intro: v = Val::b_(mk == MethodKind::IntroducingVirtual || mk == MethodKind::PureIntroducingVirtual); return true;
        case P_isStaticMemberFunc: v = Val::b_(mk == MethodKind::Static); return true;
        case P_compilerGenerated: v = Val::b_(has(m.getOptions(), MethodOptions::CompilerGenerated)); return true;
        case P_sealed: v = Val::b_(has(m.getOptions(), MethodOptions::Sealed)); return true;
        case P_virtualBaseOffset: v = Val::u_(m.isIntroducingVirtual() ? m.getVFTableOffset() : 0); return true;
        case P_constType: case P_volatileType: case P_unalignedType: case P_constantExport:
            // 14.51 reports these (as 0) for the members of an overload set
            // (LF_METHOD), not for single methods (LF_ONEMETHOD); 14.30
            // reported none.
            if (!overloaded) return false;
            v = Val::b_(false);
            return true;
        default: return false;
        }
    }
};

// ------------------------------------------------------------------ model

Model::Model(std::unique_ptr<PdbFile> f) : pdb(std::move(f)) {}

Model::~Model() = default;

std::unique_ptr<Model> Model::openBuffer(std::vector<uint8_t> data, std::string &err)
{
    auto mb = MemoryBuffer::getMemBufferCopy(
        StringRef(reinterpret_cast<const char *>(data.data()), data.size()), "stream");
    auto f = PdbFile::open(std::move(mb), err);
    if (!f) return nullptr;
    std::unique_ptr<Model> m(new Model(std::move(f)));
    m->init();
    return m;
}

std::unique_ptr<Model> Model::open(const std::string &path, std::string &err)
{
    auto mb = MemoryBuffer::getFile(path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
    if (!mb) {
        err = mb.getError().message();
        return nullptr;
    }
    auto f = PdbFile::open(std::move(*mb), err);
    if (!f) return nullptr;
    std::unique_ptr<Model> m(new Model(std::move(f)));
    m->pdbPath = path;
    m->init();
    return m;
}

bool Model::identity(GUID70 &guid, uint32_t &sig, uint32_t &age)
{
    InfoStream *info = pdb->info();
    if (!info) return false;
    auto g = info->getGuid();
    memcpy(guid.b, &g, 16);
    sig = info->getSignature();
    age = info->getAge();
    return true;
}

TypeIndex Model::fullDecl(TypeIndex ti)
{
    if (ti.isSimple() || ti.isNoneType()) return ti;
    auto *types = pdb->types();
    if (!types) return ti;
    CVType t = pdb->type(ti);
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
    Sym *o = ours(id);
    if (!o) return false;
    if (p >= P_COUNT) return o->prop(p, v);
    if (p == P_symIndexId) { v = Val::u_(id); return true; }
    if (p == P_symTag) { v = Val::u_((uint32_t)o->tag); return true; }
    if (propKinds[p] == K_SYM) {
        Val e;
        if (o->prop(p, e) && e.kind == Val::Err) {
            v = e;
            return true;
        }
        Prop ip = idPropOf(p);
        if (ip != P_COUNT) {
            if (!get(id, ip, v)) return false;
            if (v.kind == Val::U) v = Val::sym((SymIndexId)v.u);
            return v.kind == Val::Sym && valid((SymIndexId)v.u);
        }
    }
    if (p == P_coffGroup) {
        // The COFF group holding the symbol's address (DIA 14.51).
        SymTag t = o->tag;
        if (t == SymTag::Function || t == SymTag::FuncDebugStart || t == SymTag::FuncDebugEnd ||
            t == SymTag::Data || t == SymTag::PublicSymbol || t == SymTag::Block ||
            t == SymTag::CallSite || t == SymTag::InlineSite || t == SymTag::CoffGroup ||
            t == SymTag::Label || t == SymTag::Thunk || t == SymTag::Callee || t == SymTag::Caller) {
            Val sec, off;
            if (!get(id, P_addressSection, sec) || !get(id, P_addressOffset, off)) return false;
            SymIndexId g = coffGroupAt((uint32_t)sec.u, (uint32_t)off.u);
            if (!g) return false;
            v = Val::sym(g);
            return true;
        }
        return false;
    }
    if (!o->prop(p, v)) return false;
    if (v.kind == Val::Sym && !valid((SymIndexId)v.u) && propKinds[p] != K_SYM) v = Val::u_(0);
    return true;
}

// ------------------------------------------------------------------ records

// Qualified name and function type of an LF_FUNC_ID / LF_MFUNC_ID record.
bool Model::funcIdName(TypeIndex id, std::string &nm, TypeIndex &type)
{
    CVType ft = pdb->id(id);
    if (ft.kind() == LF_FUNC_ID) {
        FuncIdRecord fr(TypeRecordKind::FuncId);
        if (!deserType(ft, fr)) return false;
        nm = fr.getName().str();
        type = fr.getFunctionType();
        if (!fr.getParentScope().isNoneType()) {
            StringIdRecord sr(TypeRecordKind::StringId);
            if (deserType(pdb->id(fr.getParentScope()), sr)) nm = sr.getString().str() + "::" + nm;
        }
        return true;
    }
    if (ft.kind() == LF_MFUNC_ID) {
        MemberFuncIdRecord fr(TypeRecordKind::MemberFuncId);
        if (!deserType(ft, fr)) return false;
        nm = name(typeSym(fr.getClassType())) + "::" + fr.getName().str();
        type = fr.getFunctionType();
        return true;
    }
    return false;
}

SymIndexId Model::funcIdTarget(TypeIndex id, std::string &nm, TypeIndex &type)
{
    if (!funcIdName(id, nm, type)) return 0;
    auto *gs = pdb->globals();
    auto *ss = pdb->symbols();
    if (!gs || !ss) return 0;
    SymIndexId any = 0;
    for (auto &r : gs->findRecordsByName(nm, *ss)) {
        if (r.second.kind() != S_PROCREF && r.second.kind() != S_LPROCREF) continue;
        ProcRefSym pr(SymbolRecordKind::ProcRefSym);
        if (!deser(r.second, pr) || !pr.Module) continue;
        SymIndexId f = procSym(pr.modi(), pr.SymOffset);
        auto *fn = dynamic_cast<OmFunction *>(ours(f));
        if (!fn) continue;
        if (fn->ps.FunctionType == type) return f;
        if (!any) any = f;
    }
    return any;
}

int Model::calleeIndex(SymIndexId id)
{
    auto *c = dynamic_cast<OmCallee *>(ours(id));
    return c ? c->index : -1;
}

SymIndexId Model::procSym(uint16_t modi, uint32_t off)
{
    auto key = std::make_pair(modi, off);
    auto it = procByRecord.find(key);
    if (it != procByRecord.end()) return it->second;
    auto *ms = pdb->module(modi);
    if (!ms) return 0;
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
    // Frame data: the S_FRAMEPROC directly inside the procedure (after
    // nested scopes in optimized MSVC code).
    for (auto J = std::next(I); J != syms.end() && J.offset() < ps.End; ++J) {
        if (J->kind() == S_FRAMEPROC) {
            f->hasFrame = deser(*J, f->fp);
            break;
        }
        uint32_t e = scopeEnd(*J);
        if (e > J.offset()) {
            J = syms.at(e);
            if (J == syms.end()) break;
        }
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
    CVType ft = pdb->type(ps.FunctionType);
    MemberFunctionRecord mf(TypeRecordKind::MemberFunction);
    if (ft.kind() == LF_MFUNCTION && deserType(ft, mf)) {
        f->classParent = typeSym(mf.getClassType());
        // Look for a method of that name and type in the class.
        std::string qual = name(f->classParent);
        std::string nm = ps.Name.str();
        std::string shortName = nm;
        if (!qual.empty() && nm.size() > qual.size() + 2 && nm.compare(0, qual.size(), qual) == 0 &&
            nm.compare(qual.size(), 2, "::") == 0) {
            shortName = nm.substr(qual.size() + 2);
        } else {
            // The function may be named after a different spelling of the
            // class (MSVC lambdas: `<lambda_...>::operator()` of the class
            // `start::__l2::<lambda_...>`): the last component, outside
            // template arguments.
            int depth = 0;
            for (size_t i = nm.size(); i-- > 1;) {
                if (nm[i] == '>') depth++;
                else if (nm[i] == '<') depth--;
                else if (!depth && nm[i] == ':' && nm[i - 1] == ':') {
                    shortName = nm.substr(i + 1);
                    break;
                }
            }
        }
        // Walk the class field list.
        CVType ct = pdb->type(fullDecl(mf.getClassType()));
        TypeIndex fl;
        ClassRecord cr(TypeRecordKind::Class);
        if ((ct.kind() == LF_CLASS || ct.kind() == LF_STRUCTURE || ct.kind() == LF_INTERFACE) && deserType(ct, cr) &&
            !cr.isForwardRef())
            fl = cr.getFieldList();
        FieldListRecord flr(TypeRecordKind::FieldList);
        if (!fl.isNoneType() && deserType(pdb->type(fl), flr)) {
            struct V : TypeVisitorCallbacks {
                std::string want;
                TypeIndex type;
                PdbFile *pdb;
                OmFunction *f;
                Error visitKnownMember(CVMemberRecord &, OneMethodRecord &m) override
                {
                    if (m.getName() == want && m.getType() == type) set(m);
                    return Error::success();
                }
                Error visitKnownMember(CVMemberRecord &, OverloadedMethodRecord &m) override
                {
                    if (m.getName() != want) return Error::success();
                    MethodOverloadListRecord list(TypeRecordKind::MethodOverloadList);
                    if (deserType(pdb->type(m.getMethodList()), list))
                        for (auto &o : list.getMethods())
                            if (o.getType() == type) {
                                set(o);
                                f->overloaded = true;
                            }
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
            vis.pdb = pdb.get();
            vis.f = f;
            consumeError(visitMemberRecordStream(flr.Data, vis));
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
    auto *ms = pdb->module(modi);
    if (!ms) return;
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
                funcIdName(is.Inlinee, site->nm, site->type);
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
                d->dataKind = DataKind::Constant;
                d->type = cs.Type;
                d->value = variantOf(cs.Value);
                d->lexParent = parent;
                d->localConst = true;
                recordSyms[key] = id;
            }
            out.push_back(id);
            continue;
        }
        if (k == S_THUNK32) {
            Thunk32Sym t(SymbolRecordKind::Thunk32Sym);
            if (!deser(*I, t)) continue;
            auto it = recordSyms.find(key);
            SymIndexId id = it != recordSyms.end() ? it->second : make<OmThunk>(t);
            ours(id)->lexParent = parent;
            recordSyms[key] = id;
            out.push_back(id);
            if (t.End > off) {
                I = syms.at(t.End);
                if (I == syms.end()) break;
            }
            continue;
        }
        if (func && k == S_LABEL32) {
            LabelSym l(SymbolRecordKind::LabelSym);
            if (!deser(*I, l)) continue;
            auto it = recordSyms.find(key);
            SymIndexId id = it != recordSyms.end() ? it->second : make<OmLabel>(l);
            ours(id)->lexParent = parent;
            recordSyms[key] = id;
            out.push_back(id);
            continue;
        }
        if (func && k == S_UDT) {
            UDTSym u(SymbolRecordKind::UDTSym);
            if (!deser(*I, u)) continue;
            auto it = recordSyms.find(key);
            SymIndexId id = it != recordSyms.end() ? it->second : make<OmLocalTypedef>(u);
            ours(id)->lexParent = parent;
            recordSyms[key] = id;
            out.push_back(id);
            continue;
        }
        if (opensScope(k)) {
            // Skip other scopes (separated code) for now.
            uint32_t e = scopeEnd(*I);
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
                d->dataKind = func ? DataKind::StaticLocal
                                   : ((k == S_GDATA32 || k == S_GTHREAD32) ? DataKind::Global : DataKind::FileStatic);
                d->tls = k == S_GTHREAD32 || k == S_LTHREAD32;
                d->lexParent = parent;
                recordSyms[key] = id;
            }
            out.push_back(id);
            continue;
        }
        if (func && (k == S_REGREL32 || k == S_REGISTER || k == S_BPREL32)) {
            auto it = recordSyms.find(key);
            SymIndexId id;
            if (it != recordSyms.end()) {
                id = it->second;
            } else {
                id = make<OmData>(k == S_REGISTER ? OmData::Register : OmData::RegRel);
                auto *d = static_cast<OmData *>(ours(id));
                if (k == S_BPREL32) {
                    // x86 frame-relative: register CV_ALLREG_VFRAME.
                    BPRelativeSym bp(SymbolRecordKind::BPRelativeSym);
                    if (!deser(*I, bp)) continue;
                    d->nm = bp.Name.str();
                    d->type = bp.Type;
                    d->reg = 30006;
                    d->regOffset = bp.Offset;
                } else if (k == S_REGREL32) {
                    RegRelativeSym rr(SymbolRecordKind::RegRelativeSym);
                    if (!deser(*I, rr)) continue;
                    d->nm = rr.Name.str();
                    d->type = rr.Type;
                    d->reg = (uint32_t)rr.Register;
                    d->regOffset = (int32_t)rr.Offset;
                } else {
                    RegisterSym rs(SymbolRecordKind::RegisterSym);
                    if (!deser(*I, rs)) continue;
                    d->nm = rs.Name.str();
                    d->type = rs.Index;
                    d->reg = (uint32_t)rs.Register;
                }
                // Parameters come first in the function's own scope, as many
                // as the function type has (plus `this`).
                d->dataKind = DataKind::Local;
                if (parent == func) {
                    auto &n = paramsSeen[func];
                    if (n < paramCount(func)) {
                        d->dataKind = d->nm == "this" ? DataKind::ObjectPtr : DataKind::Param;
                        n++;
                    }
                }
                d->lexParent = parent;
                recordSyms[key] = id;
            }
            out.push_back(id);
            continue;
        }
        if (func && (k == S_CALLEES || k == S_CALLERS)) {
            CallerSym cs(k == S_CALLEES ? SymbolRecordKind::CalleeSym : SymbolRecordKind::CallerSym);
            if (!deser(*I, cs)) continue;
            for (size_t i = 0; i < cs.Indices.size(); i++) {
                auto ckey = std::make_tuple(modi, off, (uint32_t)i);
                auto it = calleeSyms.find(ckey);
                SymIndexId id;
                if (it != calleeSyms.end()) {
                    id = it->second;
                } else {
                    std::string nm;
                    TypeIndex type;
                    SymIndexId fn = funcIdTarget(cs.Indices[i], nm, type);
                    id = make<OmCallee>(nm, type, fn, (int)i, k == S_CALLEES ? SymTag::Callee : SymTag::Caller);
                    ours(id)->lexParent = parent;
                    calleeSyms[ckey] = id;
                }
                out.push_back(id);
            }
            continue;
        }
        if (func && k == S_CALLSITEINFO) {
            CallSiteInfoSym cs(SymbolRecordKind::CallSiteInfoSym);
            if (!deser(*I, cs)) continue;
            auto it = recordSyms.find(key);
            SymIndexId id = it != recordSyms.end() ? it->second : make<OmCallSite>(cs);
            ours(id)->lexParent = parent;
            recordSyms[key] = id;
            out.push_back(id);
            continue;
        }
        if (func) continue;
        if (k == S_UNAMESPACE) {
            UsingNamespaceSym un(SymbolRecordKind::UsingNamespaceSym);
            if (!deser(*I, un)) continue;
            auto it = recordSyms.find(key);
            SymIndexId id = it != recordSyms.end() ? it->second : make<OmUsingNamespace>(un.Name.str());
            ours(id)->lexParent = parent;
            recordSyms[key] = id;
            out.push_back(id);
            continue;
        }
        if (k == S_COFFGROUP) {
            CoffGroupSym cg(SymbolRecordKind::CoffGroupSym);
            if (!deser(*I, cg)) continue;
            auto it = recordSyms.find(key);
            SymIndexId id = it != recordSyms.end() ? it->second : make<OmCoffGroup>(cg);
            ours(id)->lexParent = parent;
            recordSyms[key] = id;
            out.push_back(id);
            continue;
        }
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
        dbgStart = m.make<OmFuncDebug>(SymTag::FuncDebugStart, this);
        dbgEnd = m.make<OmFuncDebug>(SymTag::FuncDebugEnd, this);
        m.ours(dbgStart)->lexParent = getSymIndexId();
        m.ours(dbgEnd)->lexParent = getSymIndexId();
    }
    out.push_back(dbgStart);
    out.push_back(dbgEnd);
    auto *ms = m.pdb->module(modi);
    if (!ms) return;
    CVSymbolArray syms = ms->getSymbolArray();
    auto I = syms.at(off);
    uint32_t begin = std::next(I).offset();
    m.moduleChildren(modi, getSymIndexId(), begin, ps.End, out, getSymIndexId());
}

void OmInlineSite::children(std::vector<SymIndexId> &out) const
{
    auto *ms = M->pdb->module(modi);
    if (!ms) return;
    CVSymbolArray syms = ms->getSymbolArray();
    auto I = syms.at(off);
    M->moduleChildren(modi, getSymIndexId(), std::next(I).offset(), end, out, func);
}

void OmBlock::children(std::vector<SymIndexId> &out) const
{
    auto *ms = M->pdb->module(modi);
    if (!ms) return;
    CVSymbolArray syms = ms->getSymbolArray();
    auto I = syms.at(off);
    M->moduleChildren(modi, getSymIndexId(), std::next(I).offset(), bs.End, out, func);
}

}  // namespace om

// ------------------------------------------------------------------ part 2
namespace om {

namespace {

// A FunctionArgType: one argument of a FunctionType.
class OmFuncArg : public Sym {
public:
    OmFuncArg(TypeIndex t) : Sym(SymTag::FunctionArg), t(t) {}
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

uint16_t Model::udtOptions(SymIndexId id)
{
    CVType t;
    TypeIndex full;
    if (!resolveTag(typeIndexOf(id), t, full)) return 0;
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

bool Model::resolveTag(TypeIndex ti, CVType &out, TypeIndex &full)
{
    if (ti.isSimple() || ti.isNoneType()) return false;
    CVType t = pdb->type(ti);
    if (t.kind() == LF_MODIFIER) {
        ModifierRecord mr(TypeRecordKind::Modifier);
        if (!deserType(t, mr)) return false;
        return resolveTag(mr.getModifiedType(), out, full);
    }
    ti = fullDecl(ti);
    out = pdb->type(ti);
    full = ti;
    return out.kind() != 0;
}

void Model::typeMembers(SymIndexId udt, TypeIndex fieldList, std::vector<SymIndexId> &out)
{
    auto *types = pdb->types();
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
                d->dataKind = DataKind::Member;
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
                d->dataKind = DataKind::StaticMember;
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
                d->dataKind = DataKind::Constant;
                d->classParent = udt;
                d->lexParent = m->exe;
                d->typeId = udt;  // the enum itself (DIA 14.51; 14.30 reported int)
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
        void method(const OneMethodRecord &r, StringRef name, bool overloaded = false)
        {
            out->push_back(get([&] {
                SymIndexId impl = m->findMethodImpl(udt, name, r.getType());
                if (impl) return impl;
                OneMethodRecord copy = r;
                SymIndexId id = m->make<OmMethod>(copy);
                static_cast<OmMethod *>(m->ours(id))->overloaded = overloaded;
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
                    method(named, r.getName(), true);
                }
            return Error::success();
        }
        Error visitKnownMember(CVMemberRecord &, NestedTypeRecord &r) override
        {
            SymIndexId target = m->typeSym(r.getNestedType());
            if (!target) {
                index++;
                return Error::success();
            }
            SymTag tt = m->tag(target);
            Val nested;
            if ((tt != SymTag::UDT && tt != SymTag::Enum) || !m->get(target, P_nested, nested) || !nested.u)
                tt = SymTag::Typedef;
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
    if (tag(udt) == SymTag::Enum) {
        vis.isEnum = true;
        CVType et;
        TypeIndex full;
        if (resolveTag(typeIndexOf(udt), et, full)) {
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
    if (!resolveTag(ti, t, full)) return;
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
    auto *gs = pdb->globals();
    auto *ss = pdb->symbols();
    if (!gs || !ss) {
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
    auto *gs = pdb->globals();
    auto *ss = pdb->symbols();
    if (!gs || !ss) {
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
        d->dataKind = (k == S_GDATA32 || k == S_GTHREAD32) ? DataKind::Global : DataKind::FileStatic;
        d->tls = k == S_GTHREAD32 || k == S_LTHREAD32;
        d->lexParent = exe;
        d->fromGlobals = true;
        return id;
    });
}

void Model::compilandChildren(uint16_t modi, SymIndexId c, std::vector<SymIndexId> &out)
{
    auto *ms = pdb->module(modi);
    if (!ms) return;
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
                if (!pdb->ids()) continue;
                auto &ids = *pdb->ids();
                BuildInfoRecord br(TypeRecordKind::BuildInfo);
                if (!deserType(ids.getType(bi.BuildId), br)) continue;
                static const char *keys[] = {"cwd", "cl", "src", "pdb", "cmd"};
                auto args = br.getArgs();
                for (size_t i = 0; i < args.size() && i < 5; i++) {
                    std::string val;
                    if (!args[i].isNoneType()) {
                        StringIdRecord sr(TypeRecordKind::StringId);
                        if (deserType(ids.getType(args[i]), sr)) {
                            // Long strings start with an LF_SUBSTR_LIST of LF_STRING_IDs.
                            if (!sr.getId().isNoneType()) {
                                StringListRecord sl(TypeRecordKind::StringList);
                                if (deserType(ids.getType(sr.getId()), sl))
                                    for (TypeIndex part : sl.getIndices()) {
                                        StringIdRecord ps(TypeRecordKind::StringId);
                                        if (deserType(ids.getType(part), ps)) val += ps.getString().str();
                                    }
                            }
                            val += sr.getString().str();
                        }
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
    for (uint32_t i = 0, n = moduleCount(); i < n; i++)
        if (SymIndexId c = compiland(i)) out.push_back(c);
    auto addAll = [&](const std::vector<SymIndexId> &ids, std::vector<SymIndexId> *keep) {
        out.insert(out.end(), ids.begin(), ids.end());
        if (keep) keep->insert(keep->end(), ids.begin(), ids.end());
    };
    std::vector<SymIndexId> udts, enums, sigs;
    addAll(typesOfKind({LF_STRUCTURE, LF_CLASS, LF_UNION, LF_INTERFACE}), &udts);
    addAll(typesOfKind({LF_ENUM}), &enums);
    addAll(typesOfKind({LF_POINTER}), nullptr);
    addAll(typesOfKind({LF_ARRAY}), nullptr);
    addAll(typesOfKind({LF_PROCEDURE, LF_MFUNCTION}), &sigs);
    // Typedefs: S_UDT records of the globals stream.
    auto *gs = pdb->globals();
    auto *ss = pdb->symbols();
    if (gs && ss)
        for (uint32_t off : gs->getGlobalsTable())
            if (ss->readRecord(off).kind() == S_UDT)
                if (SymIndexId t = typedefSym(off)) out.push_back(t);
    // const/volatile base types.
    if (auto *types = pdb->types()) {
        for (auto ti = types->getFirst(); ti; ti = types->getNext(*ti)) {
            CVType t = types->getType(*ti);
            if (t.kind() != LF_MODIFIER) continue;
            ModifierRecord mr(TypeRecordKind::Modifier);
            if (deserType(t, mr) && mr.getModifiedType().isSimple())
                if (SymIndexId s = typeSym(*ti)) out.push_back(s);
        }
    }
    // Globals stream.
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
                    d->dataKind = DataKind::Constant;
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
    }
    std::vector<std::pair<size_t, SymIndexId>> argViews;
    for (SymIndexId s : sigs)
        for (SymIndexId a : children(s)) {
            argViews.push_back({out.size(), s});
            out.push_back(a);
        }
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
    auto &vw = childViews[exe];
    vw.assign(out.size(), 0);
    for (auto &av : argViews) vw[av.first] = av.second;
}

std::vector<uint8_t> Model::flagsOf(SymIndexId id)
{
    const auto &c = children(id);
    SymTag t = tag(id);
    if (t == SymTag::UDT || t == SymTag::BaseClass) return std::vector<uint8_t>(c.size(), 1);
    auto it = childFlags.find(id);
    if (it != childFlags.end() && it->second.size() == c.size()) return it->second;
    return std::vector<uint8_t>(c.size(), 0);
}

std::vector<SymIndexId> Model::viewsOf(SymIndexId id)
{
    const auto &c = children(id);
    if (tag(id) == SymTag::FunctionSig) return std::vector<SymIndexId>(c.size(), id);
    auto it = childViews.find(id);
    if (it != childViews.end() && it->second.size() == c.size()) return it->second;
    return std::vector<SymIndexId>(c.size(), 0);
}

const std::vector<SymIndexId> &Model::publics()
{
    if (publicsLoaded) return publicList;
    publicsLoaded = true;
    auto *ps = pdb->publics();
    auto *ss = pdb->symbols();
    if (!ps || !ss) return publicList;
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
    // Length (as observed): up to the next public at a higher RVA, in any
    // section, but not past the end of the section contribution holding
    // the symbol; 0 for the last one.
    std::vector<OmPublic *> sorted = pubs;
    auto rvaOf = [&](OmPublic *p) { return rva(p->ps.Segment, p->ps.Offset); };
    std::stable_sort(sorted.begin(), sorted.end(), [&](OmPublic *a, OmPublic *b) { return rvaOf(a) < rvaOf(b); });
    const auto &sc = sectionContribs();
    for (size_t i = 0; i < sorted.size(); i++) {
        OmPublic *p = sorted[i];
        uint32_t r = rvaOf(p);
        uint64_t end = 0;
        for (size_t j = i + 1; j < sorted.size(); j++)
            if (rvaOf(sorted[j]) > r) {
                end = rvaOf(sorted[j]);
                break;
            }
        if (end)
            for (auto &c : sc)
                if (c.sect == p->ps.Segment && p->ps.Offset >= c.off && p->ps.Offset < c.off + c.size) {
                    end = std::min<uint64_t>(end, rva(c.sect, c.off) + c.size);
                    break;
                }
        p->len = end ? (uint32_t)(end - r) : 0;
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
    if (id == exe) {
        buildGlobalChildren(out);
    } else {
        Sym *o = ours(id);
        none = o->noChildren();
        o->children(out);
    }
    noChildCache[id] = none;
    if (noChildren) *noChildren = none;
    return childCache[id] = std::move(out);
}

void Model::sigArgs(SymIndexId sig, std::vector<SymIndexId> &out)
{
    CVType ft = pdb->type(typeIndexOf(sig));
    TypeIndex argList;
    if (ft.kind() == LF_PROCEDURE) {
        ProcedureRecord pr(TypeRecordKind::Procedure);
        if (deserType(ft, pr)) argList = pr.getArgumentList();
    } else if (ft.kind() == LF_MFUNCTION) {
        MemberFunctionRecord mr(TypeRecordKind::MemberFunction);
        if (deserType(ft, mr)) argList = mr.getArgumentList();
    }
    ArgListRecord al(TypeRecordKind::ArgList);
    if (argList.isNoneType() || !deserType(pdb->type(argList), al)) return;
    // Argument symbols belong to the argument list, which function types
    // share; each enumeration reports the function type it went through as
    // class parent (see viewsOf), this one is for lookups by id.
    for (size_t i = 0; i < al.getIndices().size(); i++) {
        auto key = std::make_pair((SymIndexId)(0x80000000u | argList.getIndex()), (uint32_t)i);
        auto m = memberSyms.find(key);
        SymIndexId a;
        if (m != memberSyms.end()) {
            a = m->second;
        } else {
            a = make<OmFuncArg>(al.getIndices()[i]);
            ours(a)->classParent = sig;
            ours(a)->lexParent = exe;
            memberSyms[key] = a;
        }
        out.push_back(a);
    }
}

bool Model::constThis(TypeIndex mfType)
{
    MemberFunctionRecord mr(TypeRecordKind::MemberFunction);
    if (!deserType(pdb->type(mfType), mr) || mr.getThisType().isNoneType() || mr.getThisType().isSimple())
        return false;
    PointerRecord pr(TypeRecordKind::Pointer);
    if (!deserType(pdb->type(mr.getThisType()), pr) || pr.getReferentType().isSimple()) return false;
    return pdb->type(pr.getReferentType()).kind() == LF_MODIFIER;
}

uint32_t Model::paramCount(SymIndexId func)
{
    auto *f = dynamic_cast<OmFunction *>(ours(func));
    if (!f) return 0;
    CVType ft = pdb->type(f->ps.FunctionType);
    // The "..." of a variadic function (a T_NOTYPE argument) has no record.
    auto declared = [&](TypeIndex argList, uint32_t n) {
        ArgListRecord al(TypeRecordKind::ArgList);
        if (n && deserType(pdb->type(argList), al) && !al.getIndices().empty() &&
            al.getIndices().back() == TypeIndex::None())
            n--;
        return n;
    };
    if (ft.kind() == LF_PROCEDURE) {
        ProcedureRecord pr(TypeRecordKind::Procedure);
        return deserType(ft, pr) ? declared(pr.getArgumentList(), pr.getParameterCount()) : 0;
    }
    if (ft.kind() == LF_MFUNCTION) {
        MemberFunctionRecord mr(TypeRecordKind::MemberFunction);
        if (!deserType(ft, mr)) return 0;
        return declared(mr.getArgumentList(), mr.getParameterCount()) + (mr.getThisType().isNoneType() ? 0 : 1);
    }
    return 0;
}

SymIndexId Model::coffGroupAt(uint32_t sect, uint32_t off)
{
    if (!coffGroupsLoaded) {
        coffGroupsLoaded = true;
        for (uint32_t i = 0, n = moduleCount(); i < n; i++) {
            SymIndexId c = compiland((uint16_t)i);
            if (name(c) != "* Linker *") continue;
            for (SymIndexId k : children(c))
                if (tag(k) == SymTag::CoffGroup) coffGroups.push_back(k);
        }
    }
    for (SymIndexId g : coffGroups) {
        auto *cg = static_cast<OmCoffGroup *>(ours(g));
        if (cg->cg.Segment == sect && off >= cg->cg.Offset && off < cg->cg.Offset + cg->cg.Size) return g;
    }
    return 0;
}

bool Model::methodOverloaded(SymIndexId id)
{
    auto *f = dynamic_cast<OmFunction *>(ours(id));
    return f && f->isMethod && f->overloaded;
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
    auto *ms = pdb->module(modi);
    if (!ms) return out;
    auto checksums = ms->findChecksumsSubsection();
    if (!checksums) {
        consumeError(checksums.takeError());
        return out;
    }
    files();
    const PDBStringTable *strings = pdb->strings();
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
    // In address order (MSVC writes one line block per COMDAT function, in
    // no particular order).
    std::stable_sort(out.begin(), out.end(), [&](const LineEntry &a, const LineEntry &b) {
        return rva(a.sect, a.off) < rva(b.sect, b.off);
    });
    return out;
}

std::vector<LineEntry> Model::linesByRVA(uint32_t r, uint32_t len)
{
    std::vector<LineEntry> out;
    for (uint32_t i = 0, n = moduleCount(); i < n; i++)
        for (auto &e : lines((uint16_t)i)) {
            uint32_t a = rva(e.sect, e.off);
            if ((a + e.len > r && a < r + len) || (e.len == 0 && a == r)) out.push_back(e);
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
    const PDBStringTable *strings = pdb->strings();
    if (!strings) return fileList;
    for (uint32_t i = 0, n = moduleCount(); i < n; i++) {
        auto *ms = pdb->module(i);
        if (!ms) continue;
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
    DbiStream *dbi = pdb->dbi();
    if (!dbi) return contribs;
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
    DbiStream *dbi = pdb->dbi();
    return dbi ? (uint32_t)dbi->getSectionMap().size() : 0;
}

bool Model::simpleBasicType(uint32_t kind, uint32_t &bt, uint32_t &len)
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
    for (auto &e : table)
        if ((uint32_t)e.k == kind) {
            bt = e.bt;
            len = e.len;
            return true;
        }
    return false;
}

bool Model::basicType(SymIndexId id, uint32_t &bt, uint32_t &len)
{
    if (tag(id) != SymTag::BuiltinType) return false;
    Val b, l;
    if (!get(id, P_baseType, b) || !get(id, P_length, l)) return false;
    bt = (uint32_t)b.u;
    len = (uint32_t)l.u;
    return true;
}

uint32_t Model::frameDataCount()
{
    for (auto &d : debugStreams())
        if (d.name == "NEWFPO") return (uint32_t)(d.data.size() / d.recordSize);
    return 0;
}

const std::vector<FrameRow> &Model::frameRows()
{
    if (frameRowsRead) return frameRowCache;
    frameRowsRead = true;
    for (auto &d : debugStreams()) {
        if (d.name != "NEWFPO") continue;
        for (size_t at = 0; at + 32 <= d.data.size(); at += 32) {
            const uint8_t *p = &d.data[at];
            auto u32 = [&](int o) { uint32_t v; memcpy(&v, p + o, 4); return v; };
            auto u16 = [&](int o) { uint16_t v; memcpy(&v, p + o, 2); return v; };
            frameRowCache.push_back({u32(0), u32(4), u32(8), u32(12), u32(16), u32(20), u16(24), u16(26), u32(28)});
        }
    }
    return frameRowCache;
}

std::string Model::frameProgram(uint32_t frameFunc)
{
    const PDBStringTable *st = pdb->strings();
    if (!st) return "";
    auto e = st->getStringForID(frameFunc);
    if (!e) {
        consumeError(e.takeError());
        return "";
    }
    return e->str();
}

std::vector<DebugStream> Model::debugStreams()
{
    std::vector<DebugStream> out;
    DbiStream *dbi = pdb->dbi();
    if (!dbi) return out;
    PDBFile &file = pdb->file();
    struct K { DbgHeaderType t; const char *name; uint32_t size; };
    static const K kinds[] = {
        {DbgHeaderType::FPO, "FPO", 16},          {DbgHeaderType::Exception, "EXCEPTION", 1},
        {DbgHeaderType::Fixup, "FIXUP", 10},      {DbgHeaderType::OmapToSrc, "OMAPTO", 8},
        {DbgHeaderType::OmapFromSrc, "OMAPFROM", 8}, {DbgHeaderType::SectionHdr, "SECTIONHEADERS", 40},
        {DbgHeaderType::TokenRidMap, "TOKENRIDMAP", 4}, {DbgHeaderType::Xdata, "XDATA", 1},
        {DbgHeaderType::Pdata, "PDATA", 12},     {DbgHeaderType::NewFPO, "NEWFPO", 32},
        {DbgHeaderType::SectionHdrOrig, "SECTIONHEADERS_ORIG", 40},
    };
    for (auto &k : kinds) {
        uint32_t idx = dbi->getDebugStreamIndex(k.t);
        if (idx == kInvalidStreamIndex || idx >= file.getNumStreams()) continue;
        DebugStream d;
        d.name = k.name;
        d.recordSize = k.size;
        uint32_t size = file.getStreamByteSize(idx);
        auto stream = file.createIndexedStream(idx);
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
    // Module data are found by address only when named and when the section
    // contribution at their address belongs to their own module (as
    // observed: LTCG can place a module's static elsewhere).
    std::vector<const SectionContrib *> sc;
    for (auto &c : sectionContribs()) sc.push_back(&c);
    std::sort(sc.begin(), sc.end(), [](const SectionContrib *a, const SectionContrib *b) {
        return a->sect != b->sect ? a->sect < b->sect : a->off < b->off;
    });
    auto contribModule = [&](uint32_t sect, uint32_t off) -> int {
        auto it = std::upper_bound(sc.begin(), sc.end(), std::make_pair(sect, off),
                                   [](const std::pair<uint32_t, uint32_t> &k, const SectionContrib *c) {
                                       return k.first != c->sect ? k.first < c->sect : k.second < c->off;
                                   });
        if (it == sc.begin()) return -1;
        const SectionContrib *c = *--it;
        if (c->sect != sect || off >= c->off + c->size) return -1;
        return c->modi;
    };
    int curModule = -1;
    // Functions, their blocks and static locals; module-level data.
    std::function<void(SymIndexId, SymIndexId)> scope = [&](SymIndexId parent, SymIndexId func) {
        for (SymIndexId k : children(parent)) {
            SymTag kt = tag(k);
            AddrEntry e;
            if (kt == SymTag::Function && ours(k) && !func) {
                if (!entry(k, e)) continue;
                funcMap.push_back(e);
                scope(k, k);
            } else if (kt == SymTag::Block) {
                if (!entry(k, e)) continue;
                e.parent = func;
                blockMap.push_back(e);
                scope(k, func);
            } else if (kt == SymTag::Data) {
                Val off;
                if (entry(k, e) && !e.name.empty() && get(k, P_addressOffset, off) &&
                    contribModule(e.sect, (uint32_t)off.u) == curModule)
                    dataMap.push_back(e);
            } else if (kt == SymTag::Thunk && !func) {
                if (entry(k, e)) thunkMap.push_back(e);
            }
        }
    };
    for (SymIndexId c : children(exe)) {
        SymTag t = tag(c);
        AddrEntry e;
        if (t == SymTag::Compiland) {
            curModule = moduleOf(c);
            scope(c, 0);
        } else if (t == SymTag::PublicSymbol) {
            if (entry(c, e)) publicMap.push_back(e);
        } else if (t == SymTag::Data) {
            auto *d = static_cast<OmData *>(ours(c));
            if (d && d->fromGlobals && d->dataKind == DataKind::Global && entry(c, e)) dataMap.push_back(e);
        }
    }
    auto byAddr = [](const AddrEntry &a, const AddrEntry &b) {
        if (a.rva != b.rva) return a.rva < b.rva;
        return a.name < b.name;
    };
    for (auto *m : {&funcMap, &blockMap, &dataMap, &publicMap, &thunkMap})
        std::stable_sort(m->begin(), m->end(), byAddr);
    // Symbols by address: functions, thunks and data, and publics where
    // none of them is.
    std::set<uint32_t> covered;
    for (auto *m : {&funcMap, &thunkMap, &dataMap})
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

SymIndexId Model::findByRVA(uint32_t r, SymTag want, int32_t *disp)
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
    // Nearest symbol at or before the address, in any section (as observed).
    auto preceding = [&](const std::vector<AddrEntry> &m) -> const AddrEntry * {
        const AddrEntry *best = nullptr;
        for (auto &e : m)
            if (e.rva <= r && (!best || e.rva > best->rva)) best = &e;
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
    case SymTag::Function: hit = containing(funcMap); break;
    case SymTag::Block:
        if (auto *f = containing(funcMap)) hit = innermostBlock(f->id);
        break;
    case SymTag::Data: hit = preceding(dataMap); break;
    case SymTag::PublicSymbol: hit = preceding(publicMap); break;
    case SymTag::None:
        if (auto *f = containing(funcMap)) {
            hit = innermostBlock(f->id);
            if (!hit) hit = f;
        } else if (auto *t = containing(thunkMap)) {
            hit = t;
        } else if (auto *d = containing(dataMap)) {
            hit = d;
        } else {
            // The nearest preceding function, data or public symbol, in that
            // order of preference at equal addresses.
            for (auto *m : {&funcMap, &thunkMap, &dataMap, &publicMap}) {
                const AddrEntry *e = preceding(*m);
                if (e && (!hit || e->rva > hit->rva)) hit = e;
            }
        }
        break;
    default:
        break;
    }
    if (!hit) return 0;
    // Displacement into the symbol; -1 when it lies in another section.
    if (disp) *disp = hit->sect == sect ? (int32_t)(r - hit->rva) : -1;
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
    SymTag t = tag(id);
    auto cv = [&](std::string s) {
        Val v;
        std::string pre;
        if (get(id, P_constType, v) && v.u) pre += "const ";
        if (get(id, P_volatileType, v) && v.u) pre += "volatile ";
        return pre + s;
    };
    switch (t) {
    case SymTag::BuiltinType: {
        Val bt, len;
        get(id, P_baseType, bt);
        get(id, P_length, len);
        std::string n;
        switch ((uint32_t)bt.u) {
        case btVoid: n = "void"; break;
        case btChar: n = "char"; break;
        case btWChar: n = "wchar_t"; break;
        case btChar8: n = "char8_t"; break;
        case btChar16: n = "char16_t"; break;
        case btChar32: n = "char32_t"; break;
        case btBool: n = "bool"; break;
        case btFloat: n = len.u == 4 ? "float" : len.u == 8 ? "double" : "long double"; break;
        case btInt: case btLong:
            n = len.u == 1 ? "char" : len.u == 2 ? "short" : len.u == 4 ? (bt.u == btLong ? "long" : "int") : "__int64";
            if (len.u == 1) n = "signed char";
            break;
        case btUInt: case btULong:
            n = len.u == 1 ? "unsigned char" : len.u == 2 ? "unsigned short" : len.u == 4 ? (bt.u == btULong ? "unsigned long" : "unsigned int") : "unsigned __int64";
            break;
        case btNoType: n = "<NoType>"; break;  // the "..." of a variadic function
        default: n = "?"; break;
        }
        return cv(n);
    }
    case SymTag::PointerType: {
        Val ty, ref, rref, c;
        get(id, P_type, ty);
        bool isRef = get(id, P_reference, ref) && ref.u;
        bool isRRef = get(id, P_RValueReference, rref) && rref.u;
        // As observed: references print as "& const", pointer cv is dropped.
        return typeName((SymIndexId)ty.u) + (isRRef ? " *" : isRef ? " & const" : " *");
    }
    case SymTag::UDT: {
        Val k;
        get(id, P_udtKind, k);
        const char *kw = k.u == (uint32_t)UdtKind::Class ? "class " : k.u == (uint32_t)UdtKind::Union ? "union " : "struct ";
        return cv(kw + name(id));
    }
    case SymTag::Enum:
        return cv(name(id));
    case SymTag::Typedef:
        return name(id);
    case SymTag::ArrayType: {
        Val ty, n;
        get(id, P_type, ty);
        get(id, P_count, n);
        return typeName((SymIndexId)ty.u) + "[" + std::to_string(n.u) + "]";
    }
    case SymTag::FunctionSig: {
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

std::vector<SymIndexId> Model::allSymbols()
{
    // As the real DLL lists them: the exe, the compilands, each compiland's
    // symbols (depth first), then the exe's other children (globals,
    // publics, types in TPI order), each followed by its own children. A
    // type's children are listed again for every type that has them (a
    // modified class repeats its class's members, function types sharing
    // an argument list repeat its arguments).
    std::vector<SymIndexId> out{exe};
    std::set<SymIndexId> seen{exe};
    std::vector<SymIndexId> top = children(exe);
    auto add = [&](SymIndexId id) { return seen.insert(id).second ? (out.push_back(id), true) : false; };
    std::function<void(SymIndexId)> below = [&](SymIndexId id) {
        for (SymIndexId c : std::vector<SymIndexId>(children(id)))
            if (add(c)) below(c);
    };
    for (SymIndexId c : top)
        if (tag(c) == SymTag::Compiland) add(c);
    for (SymIndexId c : top)
        if (tag(c) == SymTag::Compiland) below(c);
    for (SymIndexId c : top) {
        if (!add(c)) continue;
        SymTag t = tag(c);
        if (t == SymTag::UDT || t == SymTag::Enum || t == SymTag::FunctionSig) {
            for (SymIndexId k : std::vector<SymIndexId>(children(c))) {
                out.push_back(k);
                seen.insert(k);
            }
        } else {
            below(c);
        }
    }
    return out;
}

uint32_t Model::hiddenLocalCount()
{
    // S_LOCAL and S_FILESTATIC records: symbols of the Symbols table that
    // no findChildren lists.
    uint32_t n = 0;
    for (uint32_t m = 0; m < moduleCount(); m++) {
        auto *ms = pdb->module((uint16_t)m);
        if (!ms) continue;
        for (auto &rec : ms->getSymbolArray())
            if (rec.kind() == S_LOCAL || rec.kind() == S_FILESTATIC) n++;
    }
    return n;
}

bool Model::undecoratedNameEx(SymIndexId id, unsigned options, std::string &out)
{
    unsigned saved = g_exOptions;
    g_exOptions = options;
    Val v;
    bool ok = get(id, P_undecoratedNameEx0, v) && v.kind == Val::Str;
    g_exOptions = saved;
    if (ok) out = v.s;
    return ok;
}

// A function with a public symbol at its address: an undecorated
// Microsoft name (the one naming this function when several publics share
// the address), or its own name for an undecorated (C) public.
bool Model::publicSignature(SymIndexId fn, unsigned options, std::string &out)
{
    auto *f = dynamic_cast<OmFunction *>(ours(fn));
    if (!f) return false;
    std::string nm = name(fn);
    std::string first;
    for (SymIndexId p : publics()) {
        auto *pub = static_cast<OmPublic *>(ours(p));
        if (pub->ps.Segment != f->ps.Segment || pub->ps.Offset != f->ps.CodeOffset) continue;
        if (pub->ps.Name == f->ps.Name) {
            out = nm;
            return true;
        }
        // x86 C decorations (_name, _name@n, @name@n): the public's name.
        StringRef pn = pub->ps.Name, fnm = f->ps.Name;
        if ((pn.size() > fnm.size() && (pn[0] == '_' || pn[0] == '@') && pn.substr(1).starts_with(fnm) &&
             (pn.size() == fnm.size() + 1 || pn[fnm.size() + 1] == '@'))) {
            out = pn.str();
            return true;
        }
        if (!pub->ps.Name.starts_with("?")) continue;
        std::string u = undname(pub->ps.Name.str(), UND_NO_PTR64);
        if (u.find(nm) != std::string::npos) {
            out = undname(pub->ps.Name.str(), options);
            return true;
        }
        if (first.empty()) first = undname(pub->ps.Name.str(), options);
    }
    out = first;
    return !first.empty();
}

std::string Model::functionSignature(SymIndexId fn, bool isStatic, unsigned options)
{
    std::string nm = name(fn);
    std::string pub;
    if (publicSignature(fn, options, pub)) return pub;
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
