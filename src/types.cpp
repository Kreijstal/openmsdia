// OpenMSDIA: the exe, compiland and type symbols (UDT, enum, pointer,
// array, base type, function type, typedef, vtable shape), built directly
// from the DBI, info and TPI streams.
#include <algorithm>
#include <cstring>

#include "cvutil.h"
#include "model.h"

#include "llvm/DebugInfo/CodeView/LazyRandomTypeCollection.h"
#include "llvm/DebugInfo/CodeView/TypeRecordHelpers.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptor.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleList.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/GlobalsStream.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"

using namespace llvm;
using namespace llvm::codeview;
using namespace llvm::pdb;

namespace om {

namespace {

bool cvProp(Prop p, Val &v, ModifierOptions mods)
{
    switch (p) {
    case P_constType: case P_constantExport: v = Val::b_(has(mods, ModifierOptions::Const)); return true;
    case P_volatileType: v = Val::b_(has(mods, ModifierOptions::Volatile)); return true;
    case P_unalignedType: v = Val::b_(has(mods, ModifierOptions::Unaligned)); return true;
    default: return false;
    }
}

// The global scope.
class SymExe : public Sym {
public:
    SymExe() : Sym(SymTag::Exe) {}
    bool prop(Prop p, Val &v) const override
    {
        PdbFile &f = *M->pdb;
        switch (p) {
        case P_name: {
            std::string n = M->pdbPath;
            size_t s = n.find_last_of("\\/");
            if (s != std::string::npos) n = n.substr(s + 1);
            size_t d = n.find_last_of('.');
            if (d != std::string::npos) n = n.substr(0, d);
            v = Val::str(n);
            return true;
        }
        case P_symbolsFileName: {
            std::string n = M->pdbPath;
            std::replace(n.begin(), n.end(), '/', '\\');
            v = Val::str(n);
            return true;
        }
        case P_age: v = Val::u_(f.info() ? f.info()->getAge() : 0); return true;
        case P_signature: v = Val::u_(f.info() ? f.info()->getSignature() : 0); return true;
        case P_guid: {
            codeview::GUID g{};
            if (f.info()) g = f.info()->getGuid();
            memcpy(v.g, &g, 16);
            v.kind = Val::Guid;
            return true;
        }
        case P_isCTypes: v = Val::b_(f.dbi() && f.dbi()->hasCTypes()); return true;
        case P_isStripped: v = Val::b_(f.dbi() && f.dbi()->isStripped()); return true;
        case P_machineType: v = Val::u_(f.dbi() ? (uint32_t)f.dbi()->getMachineType() : 0); return true;
        default: return false;
        }
    }
};

// One module of the DBI stream.
class SymCompiland : public Sym {
public:
    explicit SymCompiland(uint16_t modi) : Sym(SymTag::Compiland), modi(modi) {}
    uint16_t modi;
    bool prop(Prop p, Val &v) const override
    {
        DbiStream *dbi = M->pdb->dbi();
        if (!dbi) return false;
        DbiModuleDescriptor d = dbi->modules().getModuleDescriptor(modi);
        switch (p) {
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(M->exe); return true;
        case P_name: v = Val::str(d.getModuleName().str()); return true;
        case P_libraryName: v = Val::str(d.getObjFileName().str()); return true;
        case P_editAndContinueEnabled: v = Val::b_(d.hasECInfo()); return true;
        default: return false;
        }
    }
    void children(std::vector<SymIndexId> &out) const override { M->compilandChildren(modi, id, out); }
};

// Base class of the type symbols: lexical parent is the exe.
class TypeSym : public Sym {
public:
    TypeSym(SymTag t, TypeIndex ti) : Sym(t), ti(ti) {}
    TypeIndex ti;  // the record behind the symbol (a modifier for modified types)
    bool common(Prop p, Val &v) const
    {
        if (p == P_lexicalParent || p == P_lexicalParentId) {
            v = Val::sym(M->exe);
            return true;
        }
        return false;
    }
};

// A CodeView simple type (or a const/volatile modifier of one).
class SymBuiltin : public TypeSym {
public:
    SymBuiltin(TypeIndex ti, uint32_t kind, ModifierOptions mods, TypeIndex modified)
        : TypeSym(SymTag::BuiltinType, ti), kind(kind), mods(mods), modified(modified) {}
    uint32_t kind;
    ModifierOptions mods;
    TypeIndex modified;  // the unmodified simple type, for a modifier record
    bool prop(Prop p, Val &v) const override
    {
        if (common(p, v) || cvProp(p, v, mods)) return true;
        uint32_t bt = 0, len = 0;
        Model::simpleBasicType(kind, bt, len);
        switch (p) {
        case P_baseType: v = Val::u_(bt); return true;
        case P_length: v = Val::u_(len); return true;
        case P_unmodifiedType: case P_unmodifiedTypeId:
            // The plain base type behind a const/volatile one.
            v = Val::sym(ti.isSimple() ? id : M->typeSym(modified));
            return true;
        default: return false;
        }
    }
};

// LF_CLASS, LF_STRUCTURE, LF_INTERFACE, LF_UNION, or a modifier of one.
class SymUDT : public TypeSym {
public:
    SymUDT(TypeIndex ti, ClassRecord cr) : TypeSym(SymTag::UDT, ti), isClass(true), cr(std::move(cr)) {}
    SymUDT(TypeIndex ti, UnionRecord ur) : TypeSym(SymTag::UDT, ti), isClass(false), ur(std::move(ur)) {}
    SymUDT(TypeIndex ti, const SymUDT *base, ModifierOptions mods)
        : TypeSym(SymTag::UDT, ti), isClass(base->isClass), cr(base->cr), ur(base->ur), base(base), mods(mods) {}
    bool isClass;
    ClassRecord cr{TypeRecordKind::Class};
    UnionRecord ur{TypeRecordKind::Union};
    const SymUDT *base = nullptr;  // the unmodified type
    ModifierOptions mods = ModifierOptions::None;
    const TagRecord &tagRec() const { return isClass ? (const TagRecord &)cr : (const TagRecord &)ur; }
    bool opt(ClassOptions o) const { return has(tagRec().getOptions(), o); }
    bool prop(Prop p, Val &v) const override
    {
        if (common(p, v) || cvProp(p, v, mods)) return true;
        switch (p) {
        case P_name: v = Val::str(tagRec().getName().str()); return true;
        case P_udtKind: {
            UdtKind k = UdtKind::Struct;
            if (!isClass) k = UdtKind::Union;
            else if (cr.getKind() == TypeRecordKind::Class) k = UdtKind::Class;
            else if (cr.getKind() == TypeRecordKind::Interface) k = UdtKind::Interface;
            v = Val::u_((uint32_t)k);
            return true;
        }
        case P_length: v = Val::u_(isClass ? cr.getSize() : ur.getSize()); return true;
        case P_constructor: v = Val::b_(opt(ClassOptions::HasConstructorOrDestructor)); return true;
        case P_hasAssignmentOperator: v = Val::b_(opt(ClassOptions::HasOverloadedAssignmentOperator)); return true;
        case P_hasCastOperator: v = Val::b_(opt(ClassOptions::HasConversionOperator)); return true;
        case P_hasNestedTypes: v = Val::b_(opt(ClassOptions::ContainsNestedClass)); return true;
        case P_overloadedOperator: v = Val::b_(opt(ClassOptions::HasOverloadedOperator)); return true;
        case P_intrinsic: v = Val::b_(opt(ClassOptions::Intrinsic)); return true;
        case P_nested: v = Val::b_(opt(ClassOptions::Nested)); return true;
        case P_packed: v = Val::b_(opt(ClassOptions::Packed)); return true;
        case P_scoped: v = Val::b_(opt(ClassOptions::Scoped)); return true;
        case P_sealed: v = Val::b_(opt(ClassOptions::Sealed)); return true;
        case P_hfaFloat: case P_hfaDouble: {
            // CV_prop_t.hfa (bits 11-12): 1 float, 2 double.
            uint32_t hfa = ((uint16_t)tagRec().getOptions() >> 11) & 3;
            v = Val::b_(hfa == (p == P_hfaFloat ? 1u : 2u));
            return true;
        }
        case P_isInterfaceUdt: case P_isRefUdt: case P_isValueUdt: case P_exportIsForwarder:
            v = Val::b_(false);
            return true;
        case P_unmodifiedType: case P_unmodifiedTypeId:
            if (!base) return false;
            v = Val::sym(base->id);
            return true;
        case P_virtualTableShape: case P_virtualTableShapeId:
            // A class without a vtable reports the no-type base type.
            if (!isClass) return false;
            v = Val::sym(M->typeSym(cr.getVTableShape()));
            return true;
        default: return false;
        }
    }
    void children(std::vector<SymIndexId> &out) const override;
};

// LF_ENUM, or a modifier of one.
class SymEnum : public TypeSym {
public:
    SymEnum(TypeIndex ti, EnumRecord er) : TypeSym(SymTag::Enum, ti), er(std::move(er)) {}
    SymEnum(TypeIndex ti, const SymEnum *base, ModifierOptions mods)
        : TypeSym(SymTag::Enum, ti), er(base->er), base(base), mods(mods) {}
    EnumRecord er{TypeRecordKind::Enum};
    const SymEnum *base = nullptr;
    ModifierOptions mods = ModifierOptions::None;
    bool opt(ClassOptions o) const { return has(er.getOptions(), o); }
    bool prop(Prop p, Val &v) const override
    {
        if (common(p, v) || cvProp(p, v, mods)) return true;
        TypeIndex u = er.getUnderlyingType();
        switch (p) {
        case P_name: v = Val::str(er.getName().str()); return true;
        case P_type: case P_typeId: v = Val::sym(M->typeSym(u)); return true;
        case P_baseType: case P_length: {
            uint32_t bt = 0, len = 0;
            if (!u.isSimple() || u.getSimpleMode() != SimpleTypeMode::Direct ||
                !Model::simpleBasicType((uint32_t)u.getSimpleKind(), bt, len))
                bt = len = 0;
            v = Val::u_(p == P_baseType ? bt : len);
            return true;
        }
        case P_constructor: v = Val::b_(opt(ClassOptions::HasConstructorOrDestructor)); return true;
        case P_hasAssignmentOperator: v = Val::b_(opt(ClassOptions::HasOverloadedAssignmentOperator)); return true;
        case P_hasCastOperator: v = Val::b_(opt(ClassOptions::HasConversionOperator)); return true;
        case P_hasNestedTypes: v = Val::b_(opt(ClassOptions::ContainsNestedClass)); return true;
        case P_overloadedOperator: v = Val::b_(opt(ClassOptions::HasOverloadedOperator)); return true;
        case P_intrinsic: v = Val::b_(opt(ClassOptions::Intrinsic)); return true;
        case P_nested: v = Val::b_(opt(ClassOptions::Nested)); return true;
        case P_packed: v = Val::b_(opt(ClassOptions::Packed)); return true;
        case P_scoped: v = Val::b_(opt(ClassOptions::Scoped)); return true;
        case P_sealed: v = Val::b_(opt(ClassOptions::Sealed)); return true;
        case P_hfaFloat: case P_hfaDouble: case P_isInterfaceUdt: case P_isRefUdt: case P_isValueUdt:
        case P_exportIsForwarder:
            v = Val::b_(false);
            return true;
        case P_unmodifiedType: case P_unmodifiedTypeId:
            if (!base) return false;
            v = Val::sym(base->id);
            return true;
        default: return false;
        }
    }
    void children(std::vector<SymIndexId> &out) const override;
};

// LF_POINTER, or a CodeView simple pointer type (T_64PINT4 and friends).
class SymPointer : public TypeSym {
public:
    SymPointer(TypeIndex ti, PointerRecord pr) : TypeSym(SymTag::PointerType, ti), rec(true), pr(std::move(pr)) {}
    explicit SymPointer(TypeIndex simple) : TypeSym(SymTag::PointerType, simple) {}
    bool rec = false;
    PointerRecord pr{TypeRecordKind::Pointer};
    bool memberPtr() const
    {
        return rec && (pr.getMode() == PointerMode::PointerToDataMember ||
                       pr.getMode() == PointerMode::PointerToMemberFunction);
    }
    uint32_t length() const
    {
        if (rec) return pr.getSize();
        switch (ti.getSimpleMode()) {
        case SimpleTypeMode::NearPointer: case SimpleTypeMode::FarPointer: case SimpleTypeMode::HugePointer: return 2;
        case SimpleTypeMode::NearPointer32: case SimpleTypeMode::FarPointer32: return 4;
        case SimpleTypeMode::NearPointer64: return 8;
        case SimpleTypeMode::NearPointer128: return 16;
        default: return 0;
        }
    }
    bool prop(Prop p, Val &v) const override
    {
        if (common(p, v)) return true;
        PointerOptions o = rec ? pr.getOptions() : PointerOptions::None;
        PointerMode m = rec ? pr.getMode() : PointerMode::Pointer;
        switch (p) {
        case P_type: case P_typeId: v = Val::sym(M->typeSym(rec ? pr.getReferentType() : ti.makeDirect())); return true;
        case P_length: v = Val::u_(length()); return true;
        case P_reference: v = Val::b_(rec && m == PointerMode::LValueReference); return true;
        case P_RValueReference: v = Val::b_(rec && m == PointerMode::RValueReference); return true;
        case P_isPointerToDataMember: v = Val::b_(rec && m == PointerMode::PointerToDataMember); return true;
        case P_isPointerToMemberFunction: v = Val::b_(rec && m == PointerMode::PointerToMemberFunction); return true;
        case P_isWinRTPointer: v = Val::b_(false); return true;
        case P_constType: case P_constantExport: v = Val::b_(has(o, PointerOptions::Const)); return true;
        case P_volatileType: v = Val::b_(has(o, PointerOptions::Volatile)); return true;
        case P_unalignedType: v = Val::b_(has(o, PointerOptions::Unaligned)); return true;
        case P_restrictedType: v = Val::b_(has(o, PointerOptions::Restrict)); return true;
        case P_isSingleInheritance: case P_isMultipleInheritance: case P_isVirtualInheritance: {
            // Pointers to members: the property naming the class's
            // inheritance model is reported (as 1), the others are not.
            if (!memberPtr()) return false;
            using R = PointerToMemberRepresentation;
            R r = pr.getMemberInfo().getRepresentation();
            bool hit = p == P_isSingleInheritance ? (r == R::SingleInheritanceData || r == R::SingleInheritanceFunction)
                     : p == P_isMultipleInheritance ? (r == R::MultipleInheritanceData || r == R::MultipleInheritanceFunction)
                     : (r == R::VirtualInheritanceData || r == R::VirtualInheritanceFunction);
            if (!hit) return false;
            v = Val::b_(true);
            return true;
        }
        case P_classParent: case P_classParentId: {
            if (!memberPtr()) return false;
            SymIndexId c = M->typeSym(pr.getMemberInfo().getContainingType());
            if (!c) return false;
            v = Val::sym(c);
            return true;
        }
        default: return false;
        }
    }
};

// LF_ARRAY.
class SymArray : public TypeSym {
public:
    SymArray(TypeIndex ti, ArrayRecord ar) : TypeSym(SymTag::ArrayType, ti), ar(std::move(ar)) {}
    ArrayRecord ar{TypeRecordKind::Array};
    bool prop(Prop p, Val &v) const override
    {
        if (common(p, v) || cvProp(p, v, ModifierOptions::None)) return true;
        switch (p) {
        case P_name: v = Val::str(""); return true;
        case P_type: case P_typeId: v = Val::sym(M->typeSym(ar.getElementType())); return true;
        case P_arrayIndexType: case P_arrayIndexTypeId: v = Val::sym(M->typeSym(ar.getIndexType())); return true;
        case P_length: v = Val::u_(ar.getSize()); return true;
        case P_count: {
            Val el;
            uint64_t n = 0;
            if (M->get(M->typeSym(ar.getElementType()), P_length, el) && el.u) n = ar.getSize() / el.u;
            v = Val::u_(n);
            return true;
        }
        default: return false;
        }
    }
};

// LF_PROCEDURE or LF_MFUNCTION.
class SymFuncSig : public TypeSym {
public:
    SymFuncSig(TypeIndex ti, ProcedureRecord pr) : TypeSym(SymTag::FunctionSig, ti), member(false), pr(std::move(pr)) {}
    SymFuncSig(TypeIndex ti, MemberFunctionRecord mf)
        : TypeSym(SymTag::FunctionSig, ti), member(true), mf(std::move(mf)) {}
    bool member;
    ProcedureRecord pr{TypeRecordKind::Procedure};
    MemberFunctionRecord mf{TypeRecordKind::MemberFunction};
    FunctionOptions opts() const { return member ? mf.getOptions() : pr.getOptions(); }
    bool prop(Prop p, Val &v) const override
    {
        if (common(p, v) || cvProp(p, v, ModifierOptions::None)) return true;
        switch (p) {
        case P_type: case P_typeId: v = Val::sym(M->typeSym(member ? mf.getReturnType() : pr.getReturnType())); return true;
        case P_callingConvention: v = Val::u_((uint32_t)(member ? mf.getCallConv() : pr.getCallConv())); return true;
        // Member functions count `this` (static ones have none).
        case P_count:
            v = Val::u_(member ? mf.getParameterCount() + (mf.getThisType().isNoneType() ? 0u : 1u)
                               : pr.getParameterCount());
            return true;
        case P_constructor: v = Val::b_(member && has(mf.getOptions(), FunctionOptions::Constructor)); return true;
        case P_isConstructorVirtualBase:
            v = Val::b_(member && has(mf.getOptions(), FunctionOptions::ConstructorWithVirtualBases));
            return true;
        case P_isCxxReturnUdt: v = Val::b_(has(opts(), FunctionOptions::CxxReturnUdt)); return true;
        // 14.30 reported noexcept only for non-member function types. The
        // flag is bit 3 of the CodeView function options (MSVC sets it).
        case P_noexcept: v = Val::b_(((uint8_t)opts() & 0x08) != 0); return true;
        case P_classParent: case P_classParentId: {
            if (!member) return false;
            SymIndexId c = M->typeSym(mf.getClassType());
            if (!c) return false;
            v = Val::sym(c);
            return true;
        }
        case P_thisAdjust:
            if (!member || !M->typeSym(mf.getClassType())) return false;
            v = Val::i_(mf.getThisPointerAdjustment());
            return true;
        case P_objectPointerType:
            if (!member || !M->typeSym(mf.getClassType()) || mf.getThisType().isNoneType()) return false;
            v = Val::sym(M->typeSym(mf.getThisType()));
            return true;
        default: return false;
        }
    }
    void children(std::vector<SymIndexId> &out) const override { M->sigArgs(id, out); }
};

// S_UDT of the globals stream.
class SymTypedef : public Sym {
public:
    explicit SymTypedef(UDTSym u) : Sym(SymTag::Typedef), u(std::move(u)) {}
    UDTSym u;
    bool noChildren() const override { return true; }
    bool prop(Prop p, Val &v) const override
    {
        switch (p) {
        case P_lexicalParent: case P_lexicalParentId: v = Val::sym(M->exe); return true;
        case P_name: v = Val::str(u.Name.str()); return true;
        case P_type: case P_typeId: v = Val::sym(M->typeSym(u.Type)); return true;
        default: return false;
        }
    }
};

// LF_VTSHAPE.
class SymVTShape : public TypeSym {
public:
    SymVTShape(TypeIndex ti, VFTableShapeRecord r) : TypeSym(SymTag::VTableShape, ti), r(std::move(r)) {}
    VFTableShapeRecord r{TypeRecordKind::VFTableShape};
    bool prop(Prop p, Val &v) const override
    {
        if (common(p, v) || cvProp(p, v, ModifierOptions::None)) return true;
        if (p == P_count) {
            v = Val::u_(r.getEntryCount());
            return true;
        }
        return false;
    }
};

void SymUDT::children(std::vector<SymIndexId> &out) const { M->udtChildren(id, out); }
void SymEnum::children(std::vector<SymIndexId> &out) const { M->udtChildren(id, out); }

}  // namespace

void Model::init()
{
    syms.emplace_back();  // id 0 is no symbol
    exe = make<SymExe>();
    for (uint32_t i = 0, n = moduleCount(); i < n; i++) compilands.push_back(make<SymCompiland>((uint16_t)i));
}

SymIndexId Model::compiland(uint16_t modi) { return modi < compilands.size() ? compilands[modi] : 0; }

int Model::moduleOf(SymIndexId id) const
{
    auto *c = dynamic_cast<SymCompiland *>(ours(id));
    return c ? (int)c->modi : -1;
}

SymIndexId Model::typeSym(TypeIndex ti)
{
    TypeIndex full = fullDecl(ti);
    auto it = typeSyms.find(full);
    if (it != typeSyms.end()) return it->second;
    SymIndexId id = createTypeSym(full);
    typeSyms[full] = id;
    return id;
}

SymIndexId Model::createTypeSym(TypeIndex ti)
{
    if (ti.isSimple()) {
        if (ti.getSimpleMode() != SimpleTypeMode::Direct) return make<SymPointer>(ti);
        uint32_t bt, len;
        if (!simpleBasicType((uint32_t)ti.getSimpleKind(), bt, len)) return 0;
        return make<SymBuiltin>(ti, (uint32_t)ti.getSimpleKind(), ModifierOptions::None, TypeIndex::None());
    }
    CVType t = pdb->type(ti);
    switch (t.kind()) {
    case LF_CLASS: case LF_STRUCTURE: case LF_INTERFACE: {
        ClassRecord cr(TypeRecordKind::Class);
        return deserType(t, cr) ? make<SymUDT>(ti, std::move(cr)) : 0;
    }
    case LF_UNION: {
        UnionRecord ur(TypeRecordKind::Union);
        return deserType(t, ur) ? make<SymUDT>(ti, std::move(ur)) : 0;
    }
    case LF_ENUM: {
        EnumRecord er(TypeRecordKind::Enum);
        return deserType(t, er) ? make<SymEnum>(ti, std::move(er)) : 0;
    }
    case LF_POINTER: {
        PointerRecord pr(TypeRecordKind::Pointer);
        return deserType(t, pr) ? make<SymPointer>(ti, std::move(pr)) : 0;
    }
    case LF_ARRAY: {
        ArrayRecord ar(TypeRecordKind::Array);
        return deserType(t, ar) ? make<SymArray>(ti, std::move(ar)) : 0;
    }
    case LF_PROCEDURE: {
        ProcedureRecord pr(TypeRecordKind::Procedure);
        return deserType(t, pr) ? make<SymFuncSig>(ti, std::move(pr)) : 0;
    }
    case LF_MFUNCTION: {
        MemberFunctionRecord mf(TypeRecordKind::MemberFunction);
        return deserType(t, mf) ? make<SymFuncSig>(ti, std::move(mf)) : 0;
    }
    case LF_VTSHAPE: {
        VFTableShapeRecord r(TypeRecordKind::VFTableShape);
        return deserType(t, r) ? make<SymVTShape>(ti, std::move(r)) : 0;
    }
    case LF_MODIFIER: {
        ModifierRecord mr(TypeRecordKind::Modifier);
        if (!deserType(t, mr)) return 0;
        TypeIndex m = mr.getModifiedType();
        if (m.isSimple()) {
            uint32_t bt, len;
            if (m.getSimpleMode() != SimpleTypeMode::Direct ||
                !simpleBasicType((uint32_t)m.getSimpleKind(), bt, len))
                return 0;
            return make<SymBuiltin>(ti, (uint32_t)m.getSimpleKind(), mr.getModifiers(), m);
        }
        // Only class, union and enum types are modified this way; pointers
        // carry their own qualifiers.
        SymIndexId base = typeSym(m);
        if (auto *u = dynamic_cast<SymUDT *>(ours(base))) return make<SymUDT>(ti, u, mr.getModifiers());
        if (auto *e = dynamic_cast<SymEnum *>(ours(base))) return make<SymEnum>(ti, e, mr.getModifiers());
        return 0;
    }
    default:
        return 0;
    }
}

TypeIndex Model::typeIndexOf(SymIndexId id)
{
    auto *t = dynamic_cast<TypeSym *>(ours(id));
    return t && !t->ti.isSimple() ? t->ti : TypeIndex::None();
}

std::vector<SymIndexId> Model::typesOfKind(std::initializer_list<TypeLeafKind> kinds)
{
    std::vector<SymIndexId> out;
    auto *types = pdb->types();
    if (!types) return out;
    auto match = [&](TypeLeafKind k) { return std::find(kinds.begin(), kinds.end(), k) != kinds.end(); };
    for (auto ti = types->getFirst(); ti; ti = types->getNext(*ti)) {
        CVType t = types->getType(*ti);
        bool want = false;
        if (match(t.kind())) {
            want = !isUdtForwardRef(t);
        } else if (t.kind() == LF_MODIFIER) {
            ModifierRecord mr(TypeRecordKind::Modifier);
            if (deserType(t, mr) && !mr.getModifiedType().isSimple())
                want = match(pdb->type(mr.getModifiedType()).kind());
        }
        if (!want) continue;
        if (SymIndexId s = typeSym(*ti)) out.push_back(s);
    }
    return out;
}

SymIndexId Model::typedefSym(uint32_t off)
{
    auto it = typedefSyms.find(off);
    if (it != typedefSyms.end()) return it->second;
    SymIndexId id = 0;
    if (auto *ss = pdb->symbols()) {
        UDTSym u(SymbolRecordKind::UDTSym);
        if (deser(ss->readRecord(off), u)) id = make<SymTypedef>(std::move(u));
    }
    return typedefSyms[off] = id;
}

}  // namespace om
