shell = /bin/sh
objects = build/.libs/kiss_fft.o build/.libs/kiss_fftr.o build/.libs/channelmaps.o build/.libs/freesurround_decoder.o
CXX = g++
CXXFLAGS = -pthread -std=c++1z -I. -Wall -I/usr/include/alsa -g3 -o $@

all: build/fsdecode
build/fsdecode: $(objects) fsdecode.cpp
	$(CXX) $(CXXFLAGS) $(objects) fsdecode.cpp
build/.libs/%.o: FreeSurround/%.cpp
	$(CXX) $< $(CXXFLAGS) -c
fsdecode.cpp: threaded_circ_buffer.hpp FreeSurround/freesurround_decoder.h FreeSurround/stream_chunker.h AudioFile/AudioFile.h ArgumentParser/argparse.hpp
FreeSurround/kiss_fft.cpp: FreeSurround/kiss_fft.h FreeSurround/_kiss_fft_guts.h
FreeSurround/kiss_fftr.cpp: FreeSurround/kiss_fftr.h FreeSurround/kiss_fft.h FreeSurround/_kiss_fft_guts.h
FreeSurround/channelmaps.cpp: FreeSurround/channelmaps.h
FreeSurround/freesurround_decoder.cpp: FreeSurround/kiss_fftr.h FreeSurround/channelmaps.h FreeSurround/freesurround_decoder.h

clean:
	-@rm -rf build
.PHONY: clean install
