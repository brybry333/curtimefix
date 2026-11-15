# libcurtimefix.so — preloaded into cstrike_linux64 (LD_PRELOAD). Same build setup as CS:TV's client:
# static libstdc++/libgcc and every symbol hidden, so our C++ runtime never interposes on the game's.
# Make can't handle spaces: clone somewhere without any in the path.
#   make               build libcurtimefix.so
#   make check-glibc   the Steam runtime has glibc 2.31
SDK      := ../third_party/source-sdk-2013/src
SRC      := ../src
CXX      ?= g++
CXXFLAGS := -O2 -g -Wall -Wno-unused-parameter -Wno-unknown-pragmas -std=c++17 -fno-strict-aliasing -fPIC -m64 \
            -ffp-contract=off -fno-fast-math \
            -DPOSIX -DLINUX -D_LINUX -DGNUC -DCOMPILER_GCC -DPLATFORM_64BITS -DX64BITS -DNO_MALLOC_OVERRIDE -DCLIENT_DLL \
            -fvisibility=hidden -fvisibility-inlines-hidden -fno-stack-protector
SDKFLAGS := -Wno-dangling-else -I$(SDK)/public -I$(SDK)/public/tier0 -I$(SDK)/public/tier1 -I$(SDK)/common
LDFLAGS  := -shared -static-libstdc++ -static-libgcc -Wl,--no-undefined -Wl,--exclude-libs,ALL \
            -Wl,--version-script=curtimefix.ver -Wl,--no-as-needed -l:libdl.so.2 -l:libpthread.so.0 -lm
OBJS     := fix.o find.o plat_linux.o cmd.o glibc_compat.o

libcurtimefix.so: $(OBJS) curtimefix.ver
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

cmd.o: $(SRC)/cmd.cpp $(SRC)/ctf.h
	$(CXX) $(CXXFLAGS) $(SDKFLAGS) -c -o $@ $<

%.o: $(SRC)/%.cpp $(SRC)/ctf.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f libcurtimefix.so *.o

check-glibc: libcurtimefix.so
	@v=$$(objdump -T libcurtimefix.so | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1); echo "highest glibc symbol: $$v"; \
	case "$$v" in GLIBC_2.[0-9]|GLIBC_2.[0-9].*|GLIBC_2.[12][0-9]|GLIBC_2.[12][0-9].*|GLIBC_2.3[01]) ;; *) echo "too new"; exit 1 ;; esac
	@e=$$(nm -D --defined-only libcurtimefix.so | grep -v ' [AB] _\|_edata\|_end\|__bss_start' | wc -l); echo "exported symbols: $$e"; [ $$e -eq 0 ]

.PHONY: clean check-glibc
