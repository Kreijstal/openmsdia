# OpenMSDIA build (MSYS2 CLANG64 on Windows).
#   make            our msdia140.dll, the walker and the tests
#   make corpus     the test PDBs
#   make oracle MSDIA=/c/path/msdia140.dll   walk the corpus with a real DLL
#   make score      walk the corpus with our DLL and compare with the oracle
ifeq ($(origin CXX),default)
CXX      := clang++
endif
PYTHON   ?= python3
B        := build
ORACLE   ?= oracle/msdia140-14.30.30626.0-amd64
SNAPSHOT := phase0/snapshots/msdia140-14.51.36257.0-amd64/typelib.json
GEN      := include/openmsdia/dia2_gen.h include/openmsdia/dia2_stubs_gen.h include/openmsdia/dia2_meta_gen.h
WALK_CXXFLAGS := -std=c++17 -O1 -g -municode -static -Wall -Iinclude/openmsdia

LLVM_LIBS := $(shell llvm-config --link-static --libs debuginfopdb 2>/dev/null)
DLL_CXXFLAGS := -std=c++17 -O2 -g -Wall -Wno-unused-function -Iinclude/openmsdia -Isrc \
	-D_FILE_OFFSET_BITS=64 -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS \
	-fno-exceptions -ffunction-sections -fdata-sections
DLL_LDFLAGS := -shared -static -Wl,--gc-sections -Wl,--no-insert-timestamp
DLL_SRCS := src/dllmain.cpp src/com.cpp src/model.cpp
DLL_OBJS := $(DLL_SRCS:src/%.cpp=$(B)/obj/%.o)

all: $(B)/msdia140.dll $(B)/dia_walk.exe $(B)/test_vtables.exe

$(B)/obj/%.o: src/%.cpp src/model.h src/rawsym_map.inc $(GEN)
	@mkdir -p $(B)/obj
	$(CXX) $(DLL_CXXFLAGS) -c $< -o $@

# LLVM's static libLLVMDemangle carries dllexport directives; drop them so
# the DLL exports only the names in msdia140.def.
LLVM_LIBDIR := $(shell llvm-config --libdir 2>/dev/null)
$(B)/lib/libLLVMDemangle.a:
	@mkdir -p $(B)/lib
	llvm-objcopy --remove-section=.drectve $(LLVM_LIBDIR)/libLLVMDemangle.a $@

$(B)/msdia140.dll: $(DLL_OBJS) src/msdia140.def $(B)/lib/libLLVMDemangle.a
	$(CXX) $(DLL_LDFLAGS) -o $@ $(DLL_OBJS) src/msdia140.def -L$(B)/lib $(LLVM_LIBS) \
		-lpsapi -lshell32 -lole32 -loleaut32 -luuid -ladvapi32 -lws2_32 -lntdll -lz -lzstd

$(B)/test_vtables.exe: tests/test_vtables.cpp $(GEN)
	@mkdir -p $(B)
	$(CXX) -std=c++17 -O1 -static -Iinclude/openmsdia $< -lole32 -loleaut32 -luuid -o $@

check: $(B)/msdia140.dll $(B)/test_vtables.exe
	$(B)/test_vtables.exe
	$(PYTHON) tests/check_exports.py $(B)/msdia140.dll phase0/snapshots/msdia140-14.30.30626.0-amd64/exports.txt

score: $(B)/msdia140.dll $(B)/dia_walk.exe
	walker/run_corpus.sh "$$(cygpath -m $(B)/msdia140.dll)" $(B)/corpus $(B)/ours
	$(PYTHON) tools/diff_walk.py --dirs $(ORACLE) $(B)/ours

$(GEN): tools/gen_dia.py $(SNAPSHOT)
	$(PYTHON) tools/gen_dia.py $(SNAPSHOT) --out include/openmsdia

$(B)/dia_walk.exe: walker/dia_walk.cpp $(GEN)
	@mkdir -p $(B)
	$(CXX) $(WALK_CXXFLAGS) $< -loleaut32 -lole32 -luuid -lshlwapi -o $@

corpus:
	corpus/build.sh $(B)/corpus

oracle: $(B)/dia_walk.exe
	walker/run_corpus.sh "$(MSDIA)" $(B)/corpus $(B)/oracle

.PHONY: all corpus oracle check score
