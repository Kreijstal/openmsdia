// OpenMSDIA: access to the streams of one PDB file.
//
// This is the only layer that talks to LLVM's PDB reader, and it uses only
// LLVM's low-level pieces: the MSF container (PDBFile), the stream readers
// (info, DBI, TPI/IPI, module, globals, publics, symbol records) and the
// CodeView record collections. The DIA object model on top of it (symbols,
// ids, parents, children, address maps) is OpenMSDIA's own: see model.h.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/Support/Allocator.h"

namespace llvm {
class MemoryBuffer;
namespace codeview {
class LazyRandomTypeCollection;
}
namespace pdb {
class PDBFile;
class DbiStream;
class TpiStream;
class InfoStream;
class GlobalsStream;
class PublicsStream;
class SymbolStream;
class PDBStringTable;
class ModuleDebugStreamRef;
}  // namespace pdb
}  // namespace llvm

namespace om {

class PdbFile {
public:
    // Parses the MSF container of `buf`; `path` is the file name reported
    // for it. Returns null (and sets err) when the container is invalid.
    static std::unique_ptr<PdbFile> open(std::unique_ptr<llvm::MemoryBuffer> buf, std::string &err);
    ~PdbFile();

    llvm::pdb::PDBFile &file() { return *pdb; }

    // Streams; null when absent or invalid.
    llvm::pdb::InfoStream *info();
    llvm::pdb::DbiStream *dbi();
    llvm::pdb::TpiStream *tpi();
    llvm::pdb::TpiStream *ipi();
    llvm::pdb::GlobalsStream *globals();
    llvm::pdb::PublicsStream *publics();
    llvm::pdb::SymbolStream *symbols();
    const llvm::pdb::PDBStringTable *strings();
    llvm::codeview::LazyRandomTypeCollection *types();  // TPI records
    llvm::codeview::LazyRandomTypeCollection *ids();    // IPI records
    // The CodeView record at `ti` in the TPI stream (an empty record for
    // simple or out-of-range indices).
    llvm::codeview::CVType type(llvm::codeview::TypeIndex ti);
    llvm::codeview::CVType id(llvm::codeview::TypeIndex ti);

    // Module (compiland) streams, cached; null without a stream.
    uint32_t moduleCount();
    llvm::pdb::ModuleDebugStreamRef *module(uint32_t modi);
    // The record at `off` of a module's symbol stream.
    llvm::codeview::CVSymbol moduleRecord(uint32_t modi, uint32_t off);

    // Addresses, from the section headers of the DBI stream.
    uint32_t rva(uint32_t sect, uint32_t off);
    bool sectOff(uint32_t rva, uint32_t &sect, uint32_t &off);

private:
    PdbFile() = default;
    std::unique_ptr<llvm::BumpPtrAllocator> alloc;
    std::unique_ptr<llvm::pdb::PDBFile> pdb;
    std::map<uint32_t, std::unique_ptr<llvm::pdb::ModuleDebugStreamRef>> modules;
};

}  // namespace om
