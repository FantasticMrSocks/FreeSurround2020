shell = /bin/sh
objects = build/.libs/pcm_freesurround2020.o build/.libs/kiss_fft.o build/.libs/kiss_fftr.o build/.libs/channelmaps.o build/.libs/freesurround_decoder.o
libs = build/pcm_freesurround2020.lo build/kiss_fft.lo build/kiss_fftr.lo build/channelmaps.lo build/freesurround_decoder.lo
CXX = g++
CXXFLAGS = -std=c++1z -I. -Wall -I/usr/include/alsa -g3 -MT build/$(*F).lo -MD -MP -MF "build/.deps/$(*F).Tpo" -c

all: build/libasound_module_pcm_freesurround2020.so
build/libasound_module_pcm_freesurround2020.so: build/libasound_module_pcm_freesurround2020.la
	g++ -shared $(objects) /usr/lib/libasound.so  -Wl,-soname -Wl,build/libasound_module_pcm_freesurround2020.so -o build/libasound_module_pcm_freesurround2020.so
build/libasound_module_pcm_freesurround2020.la: $(libs)
	libtool --tag=CXX --mode=link g++ -Wall -I/usr/include/alsa  -g3 -module -avoid-version -export-dynamic -o build/libasound_module_pcm_freesurround2020.la -rpath /usr/lib/alsa-lib $(libs) -lasound
build/%.lo: %.cpp
	-@mkdir build
	-@mkdir build/.deps
	libtool --tag=CXX --mode=compile $(CXX) $(CXXFLAGS) -o build/$(*F).lo $(*F).cpp
pcm_freesurround2020.cpp: FreeSurround/freesurround_decoder.h FreeSurround/stream_chunker.h circ_buffer.hpp
FreeSurround/kiss_fft.cpp: FreeSurround/kiss_fft.h FreeSurround/_kiss_fft_guts.h
FreeSurround/kiss_fftr.cpp: FreeSurround/kiss_fftr.h FreeSurround/kiss_fft.h FreeSurround/_kiss_fft_guts.h
FreeSurround/channelmaps.cpp: FreeSurround/channelmaps.h
FreeSurround/freesurround_decoder.cpp: FreeSurround/kiss_fftr.h FreeSurround/channelmaps.h FreeSurround/freesurround_decoder.h

clean:
	-@rm -rf build
.PHONY: clean install
