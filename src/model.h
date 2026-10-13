// OpenMSDIA: the symbol model behind the COM objects.
//
// One PDB is opened with LLVM's NativeSession. Symbols live in LLVM's
// SymbolCache, so they share one SymIndexId space: LLVM's own native
// symbols (types, compilands, the exe) and ours (OmSym subclasses, for
// what LLVM's native reader does not model yet: functions with their
// frame data, blocks, data, publics, compiland details and environment,
// base classes, vtables, member functions).
//
// Property semantics follow observations of the real msdia140.dll: for
// each SymTag a fixed set of properties is available, all others return
// S_FALSE. See docs/plan.md.
#pragma once

#include <cstdint>
#include <functional>
#include <tuple>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/PDB/IPDBSession.h"
#include "llvm/DebugInfo/PDB/Native/NativeRawSymbol.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/PDBTypes.h"

#include "dia2_symprops_gen.h"

namespace om {

using llvm::pdb::PDB_SymType;
using llvm::pdb::SymIndexId;

enum Prop {
#define X(n, k) P_##n,
    OPENMSDIA_SYMBOL_GETTERS(X)
#undef X
    P_COUNT
};
const char *propName(Prop p);

// A property value.
struct Val {
    enum Kind { None, U, I, Str, Sym, Guid, Var } kind = None;
    uint64_t u = 0;
    int64_t i = 0;
    std::string s;
    uint8_t g[16] = {};
    llvm::pdb::Variant var;
    bool strVar = false;  // a VT_BSTR variant holding `s`

    static Val u_(uint64_t v) { Val r; r.kind = U; r.u = v; return r; }
    static Val i_(int64_t v) { Val r; r.kind = I; r.i = v; return r; }
    static Val b_(bool v) { return u_(v ? 1 : 0); }
    static Val str(std::string v) { Val r; r.kind = Str; r.s = std::move(v); return r; }
    static Val sym(SymIndexId id) { Val r; r.kind = Sym; r.u = id; return r; }
};

class Model;

// Our own symbols. They are created through SymbolCache::createSymbol and
// therefore owned by LLVM's cache.
class OmSym : public llvm::pdb::NativeRawSymbol {
public:
    OmSym(llvm::pdb::NativeSession &s, PDB_SymType tag, SymIndexId id)
        : NativeRawSymbol(s, tag, id) {}
    Model *M = nullptr;
    SymIndexId lexParent = 0;
    SymIndexId classParent = 0;
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

struct DebugStream {
    std::string name;
    uint32_t recordSize = 1;
    std::vector<uint8_t> data;
};

class Model {
public:
    // `path` is an absolute UTF-8 path of an existing file.
    static std::unique_ptr<Model> open(const std::string &path, std::string &err);
    ~Model();

    llvm::pdb::NativeSession &ns;
    std::unique_ptr<llvm::pdb::IPDBSession> owner;
    std::string pdbPath;
    SymIndexId exe = 0;
    uint64_t loadAddress = 0;

    bool valid(SymIndexId id) const;
    PDB_SymType tag(SymIndexId id) const;
    OmSym *ours(SymIndexId id) const;
    llvm::pdb::NativeRawSymbol &raw(SymIndexId id) const;

    // Property access: false means S_FALSE.
    bool get(SymIndexId id, Prop p, Val &v);
    std::string name(SymIndexId id);
    // Children in DIA order; noChildren: findChildren returns S_FALSE.
    const std::vector<SymIndexId> &children(SymIndexId id, bool *noChildren = nullptr);

    // Addresses.
    uint32_t rva(uint32_t sect, uint32_t off) const;
    bool sectOff(uint32_t rva, uint32_t &sect, uint32_t &off) const;
    SymIndexId findByRVA(uint32_t rva, PDB_SymType tag, int32_t *disp = nullptr);
    std::vector<SymIndexId> symbolsByAddr();

    // Lines and files.
    const std::vector<LineEntry> &lines(uint16_t modi);
    std::vector<LineEntry> linesByRVA(uint32_t rva, uint32_t len);
    const std::vector<SourceFile> &files();
    std::vector<uint32_t> filesOfModule(uint16_t modi);
    uint32_t moduleCount() const;
    SymIndexId compiland(uint16_t modi);
    int moduleOf(SymIndexId compilandId) const;
    const std::vector<SectionContrib> &sectionContribs();
    std::vector<DebugStream> debugStreams();
    uint32_t segmentCount();

    // Helpers used by the symbol classes.
    SymIndexId typeSym(llvm::codeview::TypeIndex ti);
    llvm::codeview::TypeIndex fullDecl(llvm::codeview::TypeIndex ti);
    std::map<uint32_t, llvm::codeview::TypeIndex> fwdCacheRaw;
    std::map<llvm::codeview::TypeIndex, llvm::codeview::TypeIndex> fwdCache;
    std::map<std::string, llvm::codeview::TypeIndex> fullByName;
    SymIndexId noType();
    std::string typeName(SymIndexId id);  // C-like spelling, for undecorated names
    std::string functionSignature(SymIndexId fn, bool isStatic);

    llvm::codeview::TypeIndex typeIndexOf(SymIndexId id);
    const std::vector<SymIndexId> &publics();
    // Per child: 1 when it was reached through a class's member list.
    std::vector<uint8_t> flagsOf(SymIndexId id);
    uint16_t udtOptions(SymIndexId id);
    // DIA BasicType and length of a base type symbol (LLVM's own mapping
    // differs for some CodeView kinds).
    bool basicType(SymIndexId id, uint32_t &bt, uint32_t &len);
    std::unordered_map<SymIndexId, uint32_t> simpleKinds;
    std::unordered_map<SymIndexId, std::vector<uint8_t>> childFlags;
    // For a function bound to a class method: the method's own name.
    std::string methodShortName(SymIndexId id);
    void bindStaticMember(class OmData *d);
    SymIndexId findMethodImpl(SymIndexId udt, llvm::StringRef name, llvm::codeview::TypeIndex type);
    SymIndexId dataSym(uint16_t seg, uint32_t off, llvm::StringRef nm, std::function<SymIndexId()> mk);
    SymIndexId globalData(const llvm::codeview::DataSym &ds, llvm::codeview::SymbolKind k);
    void compilandChildren(uint16_t modi, SymIndexId c, std::vector<SymIndexId> &out);
    std::map<std::pair<SymIndexId, uint32_t>, SymIndexId> memberSyms;

    // Symbol factories, memoized by record position.
    SymIndexId procSym(uint16_t modi, uint32_t off);
    llvm::codeview::CVSymbol moduleRecord(uint16_t modi, uint32_t off);

    void moduleChildren(uint16_t modi, SymIndexId parent, uint32_t begin, uint32_t end,
                        std::vector<SymIndexId> &out, SymIndexId func);

private:
    explicit Model(std::unique_ptr<llvm::pdb::IPDBSession> s);
    void buildGlobalChildren(std::vector<SymIndexId> &out);
    void udtChildren(SymIndexId udt, std::vector<SymIndexId> &out);
    void typeMembers(SymIndexId udt, llvm::codeview::TypeIndex fieldList, std::vector<SymIndexId> &out);
    void buildAddressMap();

    std::unordered_map<SymIndexId, std::vector<SymIndexId>> childCache;
    std::unordered_map<SymIndexId, bool> noChildCache;
    std::map<std::pair<uint16_t, uint32_t>, SymIndexId> procByRecord;
    std::map<std::pair<uint16_t, uint32_t>, SymIndexId> recordSyms;
    std::map<uint32_t, SymIndexId> globalSyms;
    std::map<std::tuple<uint16_t, uint32_t, std::string>, SymIndexId> dataByAddr;
    std::map<uint16_t, std::vector<SymIndexId>> envSyms;
    std::unordered_map<SymIndexId, llvm::codeview::TypeIndex> typeIndexMap;
    bool typeIndexBuilt = false;
    std::vector<SymIndexId> publicList;
    bool publicsLoaded = false;  // symbol stream offset
    std::map<uint32_t, SymIndexId> publicSyms;
    std::map<uint16_t, std::vector<LineEntry>> lineCache;
    std::vector<SourceFile> fileList;
    bool filesLoaded = false;
    std::map<std::string, uint32_t> fileIndex;
    std::vector<SectionContrib> contribs;
    bool contribsLoaded = false;
    struct AddrEntry {
        uint32_t rva = 0, len = 0, sect = 0;
        SymIndexId id = 0, parent = 0;
        PDB_SymType tag = PDB_SymType::None;
        std::string name;
    };
    std::vector<AddrEntry> addrMap, funcMap, blockMap, dataMap, publicMap;
    bool addrBuilt = false;
    SymIndexId noTypeId = 0;
    std::unordered_map<SymIndexId, OmSym *> omIndex;

    friend class OmSymFactory;
public:
    template <class T, class... A> SymIndexId make(A &&...args);
};

}  // namespace om
