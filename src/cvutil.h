// OpenMSDIA: small helpers over LLVM's CodeView record deserializers.
#pragma once

#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/TypeDeserializer.h"

namespace om {

template <class E> inline bool has(E v, E f) { return ((uint64_t)v & (uint64_t)f) != 0; }

template <class T> bool deser(const llvm::codeview::CVSymbol &s, T &out)
{
    auto e = llvm::codeview::SymbolDeserializer::deserializeAs<T>(s);
    if (!e) {
        llvm::consumeError(e.takeError());
        return false;
    }
    out = std::move(*e);
    return true;
}

template <class T> bool deserType(llvm::codeview::CVType t, T &out)
{
    if (auto e = llvm::codeview::TypeDeserializer::deserializeAs<T>(t, out)) {
        llvm::consumeError(std::move(e));
        return false;
    }
    return true;
}

}  // namespace om
