# The two demo programs, for a person to run on a machine under test.
#
# They are Windows ARM64 binaries built here with clang: no Windows SDK and no
# C runtime are involved, the kernel32 imports are named in kernel32.def and
# the import library is generated from it with llvm-lib.
#
# 	make -C tests/demo
#
# The result, a pair of .exe files, goes into the machine's own disk image --
# startnet.cmd and the like are the reader's business, not this Makefile's.
#
# The output is on the screen, nowhere else: the kernel does not map the UART,
# so there is nothing to read from the serial port while these run.

ifeq ($(origin CC),default)
CC := clang
endif

LLVM_LIB ?= llvm-lib

CFLAGS := --target=aarch64-pc-windows-msvc -march=armv8.3-a -std=gnu23 \
          -ffreestanding -fno-stack-protector -O2 -Wall -Wextra

# /Brepro makes the PE timestamp a hash of the content, so the same sources
# rebuild to the same bytes
LDFLAGS := --target=aarch64-pc-windows-msvc -nostdlib -fuse-ld=lld-link \
           -Wl,-entry:mainCRTStartup -Wl,-subsystem:console -Wl,/Brepro

DEMOS := lrcpc_demo udf_demo
OBJS := win_console.o $(addsuffix .o,$(DEMOS))

.PHONY: all clean
all: $(addsuffix .exe,$(DEMOS))

kernel32.lib: kernel32.def
	$(LLVM_LIB) /machine:arm64 /def:kernel32.def /out:$@

%.o: %.c win_common.h
	$(CC) $(CFLAGS) -c $< -o $@

%.exe: %.o win_console.o kernel32.lib
	$(CC) $(LDFLAGS) $^ -o $@

clean:
	rm -f $(OBJS) $(DEMOS:=.exe) kernel32.lib
