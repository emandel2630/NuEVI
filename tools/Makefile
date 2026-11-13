# Host-side build of the chord mode voicer harness. No Arduino, no Teensy.
#   make -C tools && ./tools/chordtest

CXX ?= g++
CXXFLAGS ?= -std=c++14 -O2 -Wall -Wextra -Wpedantic

chordtest: chordtest.cpp ../NuEVI/chordvoicer.cpp ../NuEVI/chordvoicer.h
	$(CXX) $(CXXFLAGS) -o $@ chordtest.cpp ../NuEVI/chordvoicer.cpp

.PHONY: clean run
run: chordtest
	./chordtest

clean:
	rm -f chordtest
