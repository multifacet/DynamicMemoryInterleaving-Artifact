# GNR is default, for haswell: make ARCH=haswell
ARCH=gnr
BWMODE=totalbw

cflags.common=-Wall -Werror -std=c++20
cflags.haswell=-DHASWELL
cflags.gnr=-DGNR
cflags.spr=-DSPR
cflags.zen5=-DZEN5
cflags.totalbw=-DTOTALBW
cflags.localbw=-DLOCALBW
CFLAGS := ${cflags.common} ${cflags.${ARCH}} ${cflags.${BWMODE}}

all: dmi bwmon memlat
	@echo "DONE"

dmi: dmi.o perf.o
	g++ $^ -lfftw3 -lm -o $@

bwmon: bwmon.o perf.o
	g++ $^ -o $@

memlat: memlat.o perf.o
	g++ $^ -o $@ -lnuma

dmi.o: dmi.cpp perf.h
	g++ $(CFLAGS) dmi.cpp -c -o $@

bwmon.o: bwmon.cpp perf.h
	g++ $(CFLAGS) bwmon.cpp -c -o $@

memlat.o: memlat.cpp perf.h
	g++ $(CFLAGS) memlat.cpp -c -o $@

perf.o: perf.cpp perf.h
	g++ $(CFLAGS) perf.cpp -c -o $@

clean:
	rm -rf dmi bwmon memlat *.o
