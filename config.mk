# Single source of truth for toolchain, targets and external inputs.
# Override any of these on the command line, e.g. make QEMU=... .

# Toolchain
# CC and LD are predefined by make, so a plain ?= would never take effect.
ifeq ($(origin CC),default)
CC := clang
endif
ifeq ($(origin LD),default)
LD := lld-link
endif
OBJDUMP      ?= llvm-objdump
READOBJ      ?= llvm-readobj
TARGET_TRIPLE ?= arm64-unknown-windows

# Host side tools
MAKE         ?= make
PYTHON       ?= python3

# Layout
BUILD_DIR    ?= build
POSIX_UEFI   ?= third_party/posix-uefi/uefi

# Deployment. These point outside the project and must stay overridable.
QEMU         ?= ../qemu/build/qemu-system-aarch64
QEMU_FW      ?= ../winemu/linaro_ovmf.fd
WIN_IMAGE    ?= ../winemu/files/winpe_26100.qcow2
QEMU_CPU     ?= cortex-a76-nolrcpc
QEMU_MEM     ?= 4096
QEMU_SMP     ?= 8,sockets=1,clusters=2,cores=4,threads=1

# Console UART. QEMU virt maps PL011 at 0x09000000.
