// OpenMSDIA: the symbol model behind the COM objects.
//
// One PDB is opened through PdbFile (pdb.h), which uses LLVM's low-level
// PDB stream and CodeView record readers. Everything DIA-shaped is ours:
// every symbol is a Sym subclass held in the Model's symbol table, and its
// symIndexId is its index there. Symbols are created on first use: types
// per type index, module symbols per record offset, class members per
// field-list position, globals and publics per symbol-stream record.
//
// Property semantics follow observations of the real msdia140.dll: each
// symbol class answers the properties the real DLL reports for its tag
// and returns false (S_FALSE) for all others. See docs/plan.md.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"

#include "dia2_symprops_gen.h"
#include "pdb.h"

namespace om {

using SymIndexId = uint32_t;

// DIA's SymTagEnum (the values are in the type library).
enum class SymTag : uint32_t {
    None, Exe, Compiland, CompilandDetails, CompilandEnv, Function, Block, Data, Annotation, Label,
    PublicSymbol, UDT, Enum, FunctionSig, PointerType, ArrayType, BuiltinType, Typedef, BaseClass,
    Friend, FunctionArg, FuncDebugStart, FuncDebugEnd, UsingNamespace, VTableShape, VTable, Custom,
    Thunk, CustomType, ManagedType, Dimension, CallSite, InlineSite, BaseInterface, VectorType,
    MatrixType, HLSLType, Caller, Callee, Export, HeapAllocationSite, CoffGroup, Inlinee,
};

// DIA's DataKind, LocationType, UdtKind and BasicType values (public DIA
// documentation).
enum class DataKind : uint32_t {
    Unknown, Local, StaticLocal, Param, ObjectPtr, FileStatic, Global, Member, StaticMember, Constant,
};
enum class LocType : uint32_t {
    Null, Static, TLS, RegRel, ThisRel, Enregistered, BitField, Slot, IlRel, MetaData, Constant,
};
enum class UdtKind : uint32_t { Struct, Class, Union, Interface };
enum BasicType : uint32_t {
    btNoType = 0, btVoid = 1, btChar = 2, btWChar = 3, btInt = 6, btUInt = 7, btFloat = 8, btBool = 10,
    btLong = 13, btULong = 14, btHresult = 31, btChar16 = 32, btChar32 = 33, btChar8 = 34,
};

enum Prop {
#define X(n, k) P_##n,
    OPENMSDIA_SYMBOL_GETTERS(X)
#undef X
    P_COUNT,
    P_undecoratedNameEx0 = P_COUNT + 1,  // get_undecoratedNameEx(0), not a getter
};
const char *propName(Prop p);

// A VARIANT value.
struct Variant {
    enum Type { Empty, I1, I2, I4, I8, UI1, UI2, UI4, UI8, R4, R8, Bool, Str } type = Empty;
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0;
};

// A property value.
struct Val {
    enum Kind { None, U, I, Str, Sym, Guid, Var, Err } kind = None;  // Err: the getter fails with HRESULT u
    uint64_t u = 0;
    int64_t i = 0;
    std::string s;
    uint8_t g[16] = {};
    Variant var;
    bool strVar = false;  // a VT_BSTR variant holding `s`

    static Val u_(uint64_t v) { Val r; r.kind = U; r.u = v; return r; }
    static Val i_(int64_t v) { Val r; r.kind = I; r.i = v; return r; }
    static Val b_(bool v) { return u_(v ? 1 : 0); }
    static Val str(std::string v) { Val r; r.kind = Str; r.s = std::move(v); return r; }
    static Val sym(SymIndexId id) { Val r; r.kind = Sym; r.u = id; return r; }
};

class Model;

// A symbol. Owned by the Model; `id` is its index in the symbol table.
class Sym {
public:
    explicit Sym(SymTag tag) : tag(tag) {}
    virtual ~Sym() = default;
    SymTag tag;
    SymIndexId id = 0;
    Model *M = nullptr;
    SymIndexId lexParent = 0;
    SymIndexId classParent = 0;
    SymIndexId getSymIndexId() const { return id; }
    SymTag getSymTag() const { return tag; }
    // Returns false when the property is not available (S_FALSE).
    virtual bool prop(Prop p, Val &v) const = 0;
    virtual void children(std::vector<SymIndexId> &out) const { (void)out; }
    // findChildren on this symbol fails with S_FALSE (constants, publics).
    virtual bool noChildren() const { return false; }
};

struct LineEntry {
    uint32_t sect = 0, off = 0, len = 0;
    uint32_t line = 0, lineEnd = 0, col = 0, colEnd = 0;
    bool statement = false;
    uint16_t modi = 0;
    uint32_t file = 0;  // source file id (index into files)
};

struct SourceFile {
    std::string name;
    uint32_t checksumKind = 0;
    std::vector<uint8_t> checksum;
    std::vector<uint16_t> modules;
};

struct SectionContrib {
    uint16_t sect = 0;
    uint32_t off = 0, size = 0, characteristics = 0, dataCrc = 0, relocCrc = 0;
    uint16_t modi = 0;
};

// A row of the x86 frame data (NEWFPO) stream.
struct FrameRow {
    uint32_t rva, codeSize, localSize, paramsSize, maxStack, frameFunc;
    uint16_t prologSize, savedRegsSize;
    uint32_t flags;  // 1 SEH, 2 C++ EH, 4 function start
};

struct DebugStream {
    std::string name;
    uint32_t recordSize = 1;
    std::vector<uint8_t> data;
};

class Model {
public:
    // A whole PDB in memory (loadDataFromIStream).
    static std::unique_ptr<Model> openBuffer(std::vector<uint8_t> data, std::string &err);
    struct GUID70 { uint8_t b[16]; };
    // GUID, signature and age from the PDB info stream.
    bool identity(GUID70 &guid, uint32_t &sig, uint32_t &age);
    // `path` is an absolute UTF-8 path of an existing file.
    static std::unique_ptr<Model> open(const std::string &path, std::string &err);
    ~Model();

    std::unique_ptr<PdbFile> pdb;
    std::string pdbPath;
    SymIndexId exe = 0;
    uint64_t loadAddress = 0;

    bool valid(SymIndexId id) const { return id && id < syms.size() && syms[id]; }
    SymTag tag(SymIndexId id) const { return valid(id) ? syms[id]->tag : SymTag::None; }
    Sym *ours(SymIndexId id) const { return valid(id) ? syms[id].get() : nullptr; }

    // Property access: false means S_FALSE.
    bool get(SymIndexId id, Prop p, Val &v);
    std::string name(SymIndexId id);
    // Children in DIA order; noChildren: findChildren returns S_FALSE.
    const std::vector<SymIndexId> &children(SymIndexId id, bool *noChildren = nullptr);

    // Addresses.
    uint32_t rva(uint32_t sect, uint32_t off) const { return pdb->rva(sect, off); }
    bool sectOff(uint32_t rva, uint32_t &sect, uint32_t &off) const { return pdb->sectOff(rva, sect, off); }
    SymIndexId findByRVA(uint32_t rva, SymTag tag, int32_t *disp = nullptr);
    bool inLastSection(uint32_t rva) const { return pdb->inLastSection(rva); }
    bool absoluteSection(uint32_t sect) const { return pdb->absoluteSection(sect); }
    std::vector<SymIndexId> symbolsByAddr();
    // The Symbols table: the exe, the compilands and everything below them,
    // then the rest of the exe's children.
    std::vector<SymIndexId> allSymbols();
    std::vector<FrameRow> frameRowCache;
    bool frameRowsRead = false;
    uint32_t hiddenLocalCount();

    // Lines and files.
    const std::vector<LineEntry> &lines(uint16_t modi);
    std::vector<LineEntry> linesByRVA(uint32_t rva, uint32_t len);
    const std::vector<SourceFile> &files();
    std::vector<uint32_t> filesOfModule(uint16_t modi);
    uint32_t moduleCount() const { return pdb->moduleCount(); }
    SymIndexId compiland(uint16_t modi);
    int moduleOf(SymIndexId compilandId) const;
    const std::vector<SectionContrib> &sectionContribs();
    // The section contribution holding sect:off has IMAGE_SCN_CNT_CODE.
    bool codeContribution(uint32_t sect, uint32_t off);
    std::vector<const SectionContrib *> sortedContribs;
    std::vector<DebugStream> debugStreams();
    uint32_t frameDataCount();
    const std::vector<FrameRow> &frameRows();
    std::string frameProgram(uint32_t frameFunc);  // from the PDB string table
    bool is64();  // a 64-bit image (pointers print as __ptr64 in complete undecorated names)  // records of the x86 frame data (NEWFPO) stream
    uint32_t segmentCount();

    // Types: the symbol of a type index (forward references resolved to
    // their definition), created on first use; 0 for records DIA has no
    // symbol for.
    SymIndexId typeSym(llvm::codeview::TypeIndex ti);
    llvm::codeview::TypeIndex fullDecl(llvm::codeview::TypeIndex ti);
    std::map<llvm::codeview::TypeIndex, llvm::codeview::TypeIndex> fwdCache;
    std::map<std::string, llvm::codeview::TypeIndex> fullByName;
    SymIndexId noType();
    std::string typeName(SymIndexId id);  // C-like spelling, for undecorated names
    // options: get_undecoratedNameEx's (UND_NO_PTR64 for get_undecoratedName)
    std::string functionSignature(SymIndexId fn, bool isStatic, unsigned options);
    // The undecorated name of the public symbol at a function's address
    // (its own name for a C public); false when there is none.
    bool publicSignature(SymIndexId fn, unsigned options, std::string &out);
    // get_undecoratedNameEx(options); false when the symbol has no
    // options-dependent form (get_undecoratedName then applies).
    bool undecoratedNameEx(SymIndexId id, unsigned options, std::string &out);
    // Callees: the position in their S_CALLEES record (-1 for others).
    int calleeIndex(SymIndexId id);
    // The function of an IPI function id (LF_FUNC_ID / LF_MFUNC_ID): its
    // qualified name, function type and the function symbol, if any.
    bool funcIdName(llvm::codeview::TypeIndex funcId, std::string &name, llvm::codeview::TypeIndex &type);
    SymIndexId funcIdTarget(llvm::codeview::TypeIndex funcId, std::string &name, llvm::codeview::TypeIndex &type);


    llvm::codeview::TypeIndex typeIndexOf(SymIndexId id);
    // The record of a (possibly modified) UDT or enum, resolved to its full
    // definition.
    bool resolveTag(llvm::codeview::TypeIndex ti, llvm::codeview::CVType &out, llvm::codeview::TypeIndex &full);
    const std::vector<SymIndexId> &publics();
    // Per child: 1 when it was reached through a class's member list.
    std::vector<uint8_t> flagsOf(SymIndexId id);
    // Per child: the class parent this enumeration reports for it (0: its
    // own). FunctionArgType symbols are shared by the function types using
    // their argument list, and each enumeration names its function type.
    std::vector<SymIndexId> viewsOf(SymIndexId id);
    uint16_t udtOptions(SymIndexId id);
    // DIA BasicType and length of a base type symbol.
    bool basicType(SymIndexId id, uint32_t &bt, uint32_t &len);
    static bool simpleBasicType(uint32_t kind, uint32_t &bt, uint32_t &len);
    std::unordered_map<SymIndexId, std::vector<uint8_t>> childFlags;
    uint32_t paramCount(SymIndexId func);
    bool constThis(llvm::codeview::TypeIndex memberFunctionType);
    std::map<SymIndexId, uint32_t> paramsSeen;
    SymIndexId coffGroupAt(uint32_t sect, uint32_t off);
    std::vector<SymIndexId> coffGroups;
    bool coffGroupsLoaded = false;
    // For a function bound to a class method: the method's own name.
    std::string methodShortName(SymIndexId id);
    bool methodOverloaded(SymIndexId id);
    void bindStaticMember(class OmData *d);
    SymIndexId findMethodImpl(SymIndexId udt, llvm::StringRef name, llvm::codeview::TypeIndex type);
    SymIndexId dataSym(uint16_t seg, uint32_t off, llvm::StringRef nm, uint32_t type, std::function<SymIndexId()> mk);
    SymIndexId globalData(const llvm::codeview::DataSym &ds, llvm::codeview::SymbolKind k);
    void compilandChildren(uint16_t modi, SymIndexId c, std::vector<SymIndexId> &out);
    void udtChildren(SymIndexId udt, std::vector<SymIndexId> &out);
    void sigArgs(SymIndexId sig, std::vector<SymIndexId> &out);
    std::map<std::pair<SymIndexId, uint32_t>, SymIndexId> memberSyms;

    // Symbol factories, memoized by record position.
    SymIndexId procSym(uint16_t modi, uint32_t off);
    llvm::codeview::CVSymbol moduleRecord(uint16_t modi, uint32_t off) { return pdb->moduleRecord(modi, off); }

    void moduleChildren(uint16_t modi, SymIndexId parent, uint32_t begin, uint32_t end,
                        std::vector<SymIndexId> &out, SymIndexId func);
    // S_SEPCODE blocks of a module, per function they were separated from.
    void sepCodes(uint16_t modi);
    std::set<uint16_t> sepScanned;
    std::unordered_map<SymIndexId, std::vector<SymIndexId>> sepByFunc;
    bool isSepCode(SymIndexId id);
    // S_ANNOTATION (module, record offset); the compiland of an annotation
    // string (0 for other symbols).
    SymIndexId annotationSym(uint16_t modi, uint32_t off);
    SymIndexId annotationCompiland(SymIndexId id);

    template <class T, class... A> SymIndexId make(A &&...args)
    {
        auto s = std::make_unique<T>(std::forward<A>(args)...);
        s->id = (SymIndexId)syms.size();
        s->M = this;
        syms.push_back(std::move(s));
        return (SymIndexId)syms.size() - 1;
    }

private:
    explicit Model(std::unique_ptr<PdbFile> f);
    void init();
    SymIndexId createTypeSym(llvm::codeview::TypeIndex ti);
    void buildGlobalChildren(std::vector<SymIndexId> &out);
    void typeMembers(SymIndexId udt, llvm::codeview::TypeIndex fieldList, std::vector<SymIndexId> &out);
    void buildAddressMap();
    // Symbols of the given TPI leaf kinds, in stream order (forward
    // references left out, modifiers of matching records included).
    std::vector<SymIndexId> typesOfKind(std::initializer_list<llvm::codeview::TypeLeafKind> kinds);
    SymIndexId typedefSym(uint32_t symOffset);

    std::vector<std::unique_ptr<Sym>> syms;
    std::map<llvm::codeview::TypeIndex, SymIndexId> typeSyms;
    std::map<uint32_t, SymIndexId> typedefSyms;
    std::unordered_map<SymIndexId, std::vector<SymIndexId>> childViews;
    std::vector<SymIndexId> compilands;
    std::unordered_map<SymIndexId, std::vector<SymIndexId>> childCache;
    std::unordered_map<SymIndexId, bool> noChildCache;
    std::map<std::pair<uint16_t, uint32_t>, SymIndexId> procByRecord;
    std::map<std::pair<uint16_t, uint32_t>, SymIndexId> recordSyms;
    std::map<std::tuple<uint16_t, uint32_t, uint32_t>, SymIndexId> calleeSyms;
    std::map<uint32_t, SymIndexId> globalSyms;
    std::map<std::tuple<uint16_t, uint32_t, std::string, uint32_t>, SymIndexId> dataByAddr;
    std::map<uint16_t, std::vector<SymIndexId>> envSyms;
    std::vector<SymIndexId> publicList;
    bool publicsLoaded = false;
    std::map<uint16_t, std::vector<LineEntry>> lineCache;
    std::vector<SourceFile> fileList;
    bool filesLoaded = false;
    std::map<std::string, uint32_t> fileIndex;
    std::vector<SectionContrib> contribs;
    bool contribsLoaded = false;
    struct AddrEntry {
        uint32_t rva = 0, len = 0, sect = 0;
        SymIndexId id = 0, parent = 0;
        SymTag tag = SymTag::None;
        std::string name;
    };
    std::vector<AddrEntry> addrMap, funcMap, blockMap, dataMap, publicMap, thunkMap, sepMap;
    bool addrBuilt = false;
    std::unordered_map<SymIndexId, std::vector<const AddrEntry *>> blocksOf;
    std::map<const std::vector<AddrEntry> *, uint32_t> maxLens;
    uint32_t maxLenOf(const std::vector<AddrEntry> &m);
    SymIndexId noTypeId = 0;
};

}  // namespace om
