# probe2.so: per-frame camera recorder (see probe2.cpp). Load with LD_PRELOAD and PROBE2_DIR=<dir>.
CXX      ?= g++
CXXFLAGS := -O2 -g -Wall -Wno-unused-parameter -std=c++17 -fno-strict-aliasing -fPIC -m64 \
            -fvisibility=hidden -fvisibility-inlines-hidden -fno-stack-protector
LDFLAGS  := -shared -static-libstdc++ -static-libgcc -Wl,--no-undefined -Wl,--exclude-libs,ALL \
            -Wl,--version-script=probe2.ver -Wl,--no-as-needed -l:libdl.so.2 -l:libpthread.so.0 -lm

probe2.so: probe2.o glibc_compat.o probe2.ver
	$(CXX) $(CXXFLAGS) -o $@ probe2.o glibc_compat.o $(LDFLAGS)

probe2.o: probe2.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

glibc_compat.o: ../../src/glibc_compat.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f probe2.so *.o

.PHONY: clean
