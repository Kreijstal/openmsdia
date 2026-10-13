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
SNAPSHOT := phase0/snapshots/msdia140-14.51.36257.0-amd64/typelib.json
GEN      := include/openmsdia/dia2_gen.h include/openmsdia/dia2_stubs_gen.h include/openmsdia/dia2_meta_gen.h
WALK_CXXFLAGS := -std=c++17 -O1 -g -municode -static -Wall -Iinclude/openmsdia

all: $(B)/dia_walk.exe

$(GEN): tools/gen_dia.py $(SNAPSHOT)
	$(PYTHON) tools/gen_dia.py $(SNAPSHOT) --out include/openmsdia

$(B)/dia_walk.exe: walker/dia_walk.cpp $(GEN)
	@mkdir -p $(B)
	$(CXX) $(WALK_CXXFLAGS) $< -loleaut32 -lole32 -luuid -o $@

corpus:
	corpus/build.sh $(B)/corpus

oracle: $(B)/dia_walk.exe
	walker/run_corpus.sh "$(MSDIA)" $(B)/corpus $(B)/oracle

.PHONY: all corpus oracle
