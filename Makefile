all:
	[ ! -d .deps ] && mkdir .deps || echo;
	if libtool --tag=CC --mode=compile gcc -DHAVE_CONFIG_H -I. -I. -I..    -Wall -g -I/usr/include/alsa -g -O2 -MT pcm_freesurround.lo -MD -MP -MF ".deps/pcm_freesurround.Tpo" -c -o pcm_freesurround.lo pcm_freesurround.c; \
		then mv -f ".deps/pcm_freesurround.Tpo" ".deps/pcm_freesurround.Plo"; else rm -f ".deps/pcm_freesurround.Tpo"; exit 1; fi 
	mkdir .libs 2>/dev/null ; \
	gcc -DHAVE_CONFIG_H -I. -I. -I.. -Wall -g -I/usr/include/alsa -g -O2 -MT pcm_freesurround.lo -MD -MP -MF .deps/pcm_freesurround.Tpo -c pcm_freesurround.c  -fPIC -DPIC -o .libs/pcm_freesurround.o
	libtool --tag=CC --mode=link gcc -Wall -g -I/usr/include/alsa -g -O2 -module -avoid-version -export-dynamic   -o libasound_module_pcm_freesurround.la -rpath /usr/lib/alsa-lib  pcm_freesurround.lo -lasound   -lfftw3 -lfftw3f 
	gcc -shared  .libs/pcm_freesurround.o -lfftw3 -lfftw3f /usr/lib/libasound.so  -Wl,-soname -Wl,libasound_module_pcm_freesurround.so -o .libs/libasound_module_pcm_freesurround.so

install:
	cp .libs/libasound_module_pcm_freesurround.so /usr/lib/alsa-lib/
	cp .libs/libasound_module_pcm_freesurround.a /usr/lib/alsa-lib/
	cp libasound_module_pcm_freesurround.la /usr/lib/alsa-lib/

uninstall:
	rm /usr/lib/alsa-lib/libasound_module_pcm_freesurround.so 
	rm /usr/lib/alsa-lib/libasound_module_pcm_freesurround.a 
	rm /usr/lib/alsa-lib/libasound_module_pcm_freesurround.la 


clean:
	rm -rf .deps/
	rm -rf .libs/
	rm -f *.o *.lo *.la
