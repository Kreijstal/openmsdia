// OpenMSDIA: PDB stream access, see pdb.h.
#include "pdb.h"

#include "llvm/DebugInfo/CodeView/LazyRandomTypeCollection.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptor.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleList.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/GlobalsStream.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/ModuleDebugStream.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PDBStringTable.h"
#include "llvm/DebugInfo/PDB/Native/PublicsStream.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/Object/COFF.h"  // section headers
#include "llvm/Support/Allocator.h"
#include "llvm/Support/BinaryByteStream.h"
#include "llvm/Support/MemoryBuffer.h"

#include <string.h>

using namespace llvm;
using namespace llvm::codeview;
using namespace llvm::pdb;

namespace om {

namespace {
template <class T> T *ptr(Expected<T &> e)
{
    if (!e) {
        consumeError(e.takeError());
        return nullptr;
    }
    return &*e;
}
}  // namespace

std::unique_ptr<PdbFile> PdbFile::open(std::unique_ptr<MemoryBuffer> buf, std::string &err)
{
    std::unique_ptr<PdbFile> f(new PdbFile);
    f->alloc = std::make_unique<BumpPtrAllocator>();
    // MSVC's linker can leave the file one partial block short; the real
    // DLL reads such files, LLVM's MSF reader rejects them. Pad with zeros.
    if (buf->getBufferSize() >= 36) {
        uint32_t bs;
        memcpy(&bs, buf->getBufferStart() + 32, 4);
        size_t n = buf->getBufferSize();
        if (bs >= 512 && bs <= 65536 && (bs & (bs - 1)) == 0 && n % bs) {
            auto padded = WritableMemoryBuffer::getNewMemBuffer(n + (bs - n % bs), buf->getBufferIdentifier());
            memcpy(padded->getBufferStart(), buf->getBufferStart(), n);
            memset(padded->getBufferStart() + n, 0, bs - n % bs);
            buf = std::move(padded);
        }
    }
    StringRef path = buf->getBufferIdentifier();
    auto stream = std::make_unique<MemoryBufferByteStream>(std::move(buf), llvm::endianness::little);
    f->pdb = std::make_unique<PDBFile>(path, std::move(stream), *f->alloc);
    if (auto e = f->pdb->parseFileHeaders()) {
        err = toString(std::move(e));
        return nullptr;
    }
    if (auto e = f->pdb->parseStreamData()) {
        err = toString(std::move(e));
        return nullptr;
    }
    return f;
}

PdbFile::~PdbFile()
{
    modules.clear();
    pdb.reset();
}

InfoStream *PdbFile::info() { return ptr(pdb->getPDBInfoStream()); }
DbiStream *PdbFile::dbi() { return ptr(pdb->getPDBDbiStream()); }
TpiStream *PdbFile::tpi() { return ptr(pdb->getPDBTpiStream()); }
TpiStream *PdbFile::ipi() { return pdb->hasPDBIpiStream() ? ptr(pdb->getPDBIpiStream()) : nullptr; }
GlobalsStream *PdbFile::globals() { return ptr(pdb->getPDBGlobalsStream()); }
PublicsStream *PdbFile::publics() { return ptr(pdb->getPDBPublicsStream()); }
SymbolStream *PdbFile::symbols() { return ptr(pdb->getPDBSymbolStream()); }
const PDBStringTable *PdbFile::strings() { return ptr(pdb->getStringTable()); }

LazyRandomTypeCollection *PdbFile::types()
{
    TpiStream *t = tpi();
    return t ? &t->typeCollection() : nullptr;
}

LazyRandomTypeCollection *PdbFile::ids()
{
    TpiStream *t = ipi();
    return t ? &t->typeCollection() : nullptr;
}

// A record of kind 0 (no CodeView leaf or symbol kind) stands in for a
// missing one, so callers can test kind() without a null check.
static const uint8_t noRecord[4] = {2, 0, 0, 0};

static CVType recordAt(LazyRandomTypeCollection *c, TypeIndex ti)
{
    if (!c || ti.isSimple() || ti.isNoneType()) return CVType(ArrayRef<uint8_t>(noRecord, 4));
    auto t = c->tryGetType(ti);
    return t ? *t : CVType(ArrayRef<uint8_t>(noRecord, 4));
}

CVType PdbFile::type(TypeIndex ti) { return recordAt(types(), ti); }
CVType PdbFile::id(TypeIndex ti) { return recordAt(ids(), ti); }

uint32_t PdbFile::moduleCount()
{
    DbiStream *d = dbi();
    return d ? d->modules().getModuleCount() : 0;
}

ModuleDebugStreamRef *PdbFile::module(uint32_t modi)
{
    auto it = modules.find(modi);
    if (it != modules.end()) return it->second.get();
    auto &slot = modules[modi];
    DbiStream *d = dbi();
    if (!d || modi >= d->modules().getModuleCount()) return nullptr;
    DbiModuleDescriptor desc = d->modules().getModuleDescriptor(modi);
    uint16_t sn = desc.getModuleStreamIndex();
    if (sn == kInvalidStreamIndex || sn >= pdb->getNumStreams()) return nullptr;
    auto ms = std::make_unique<ModuleDebugStreamRef>(desc, pdb->createIndexedStream(sn));
    if (auto e = ms->reload()) {
        consumeError(std::move(e));
        return nullptr;
    }
    slot = std::move(ms);
    return slot.get();
}

CVSymbol PdbFile::moduleRecord(uint32_t modi, uint32_t off)
{
    ModuleDebugStreamRef *ms = module(modi);
    if (!ms) return CVSymbol(ArrayRef<uint8_t>(noRecord, 4));
    auto I = ms->getSymbolArray().at(off);
    return I == ms->getSymbolArray().end() ? CVSymbol(ArrayRef<uint8_t>(noRecord, 4)) : *I;
}

// RVA of a section:offset; section numbers are 1-based, 0 gives 0.
uint32_t PdbFile::rva(uint32_t sect, uint32_t off)
{
    DbiStream *d = dbi();
    if (!sect || !d) return 0;
    auto hdrs = d->getSectionHeaders();
    if (hdrs.empty()) return 0;
    uint32_t i = std::min<uint32_t>(sect, hdrs.size()) - 1;
    return hdrs[i].VirtualAddress + off;
}

// Section:offset of an RVA: the last section starting at or before it
// (section 0 before the first section, offset relative to it).
bool PdbFile::sectOff(uint32_t r, uint32_t &sect, uint32_t &off)
{
    sect = 0;
    off = 0;
    DbiStream *d = dbi();
    if (!d) return false;
    if ((int32_t)r < 0) return true;
    off = r;
    auto hdrs = d->getSectionHeaders();
    for (; sect < hdrs.size(); ++sect) {
        if (r < hdrs[sect].VirtualAddress) return true;
        off = r - hdrs[sect].VirtualAddress;
    }
    return true;
}

// lld ends the section map with an absolute entry whose frame is one past
// the last section (MSVC's link uses frame 0); with such a map the real DLL
// fails RVA lookups in the last section with E_UNEXPECTED.
bool PdbFile::inLastSection(uint32_t r)
{
    DbiStream *d = dbi();
    if (!d) return false;
    auto hdrs = d->getSectionHeaders();
    auto map = d->getSectionMap();
    if (hdrs.empty() || map.size() <= hdrs.size()) return false;
    if (map[hdrs.size()].Frame <= hdrs.size()) return false;
    return r >= hdrs[hdrs.size() - 1].VirtualAddress;
}

}  // namespace om
