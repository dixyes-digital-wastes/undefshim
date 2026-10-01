# Top level build. Toolchain and inputs live in config.mk.

include config.mk

# A fresh id every build, so the running image can be matched to the artifact.
BUILD_ID ?= $(shell head -c 6 /dev/urandom | base64 | tr -d '+/=' | tr 'A-Z' 'a-z')

TOML := third_party/toml-c
SHIMS := $(POSIX_UEFI)/freestanding
POSIX_UEFI_TESTS := third_party/posix-uefi/tests
TOML_TESTS := third_party/toml-c/tests
TOOLS_TESTS := tests/tools

# The shims come first on purpose: they supply the standard headers the
# compiler does not have for a freestanding target, and map them onto uefi.h.
US_DRIVER_CFLAGS := --target=$(TARGET_TRIPLE) -std=gnu23 -ffreestanding \
                    -fshort-wchar -mno-red-zone -fno-stack-protector \
                    -fomit-frame-pointer -O2 -Wall -Wextra \
                    -I. -I$(POSIX_UEFI) -I$(SHIMS) -I$(TOML) \
                    -DTOML_NO_FLOAT -DTOML_NO_TIMESTAMP -DTOML_NO_FILE \
                    -DUS_UART_BASE=$(US_UART_BASE) \
                    -DUS_BUILD_ID=\"$(BUILD_ID)\"

US_DRIVER_LDFLAGS := --target=$(TARGET_TRIPLE) -nostdlib -fuse-ld=lld-link \
                     -Wl,-entry:uefi_init \
                     -Wl,-subsystem:efi_boot_service_driver \
                     -Wl,-base:0x50000000 -Wl,-stack:262144 -Wl,/Brepro

# posix-uefi is a third party tree, so it keeps its own warnings rather than ours.
POSIX_CFLAGS := --target=$(TARGET_TRIPLE) -ffreestanding -fshort-wchar \
                -fno-strict-aliasing -fno-stack-protector -fno-stack-check \
                -Wno-builtin-requires-header -Wno-incompatible-library-redeclaration \
                -Wno-long-long -I.

POSIX_CRT := $(POSIX_UEFI)/crt_aarch64.o
POSIX_LIB := $(POSIX_UEFI)/libuefi.a

# The driver entry point is replaceable so that a variant can be built
# without touching the source tree: overwriting uefi/src/driver.c from a test
# script has already cost this project a working file.
DRIVER_MAIN ?= uefi/src/driver.c
DRIVER_MAIN_OBJ := $(BUILD_DIR)/driver_main.o

DRIVER_SRCS := uefi/src/config.c uefi/src/registry.c \
               uefi/src/loadimage_hook.c uefi/src/cache.c uefi/src/console.c \
               uefi/src/pool.c uefi/src/service_hook.c uefi/src/gmm_hook.c \
               uefi/src/session.c \
               core/cfg.c core/pe.c core/scan.c core/rva_patch.c core/pool.c \
               $(TOML)/toml.c
DRIVER_ASM := uefi/src/stack.S
DRIVER_OBJS := $(DRIVER_MAIN_OBJ) \
               $(patsubst %.c,$(BUILD_DIR)/%.o,$(DRIVER_SRCS)) \
               $(patsubst %.S,$(BUILD_DIR)/%.o,$(DRIVER_ASM))
DRIVER := $(BUILD_DIR)/undefshim_driver.efi

all: $(DRIVER)

$(BUILD_DIR)/uefi/src $(BUILD_DIR)/core $(BUILD_DIR)/$(TOML):
	@mkdir -p $@

# Named explicitly because the source can come from outside the tree.
$(DRIVER_MAIN_OBJ): $(DRIVER_MAIN) | $(BUILD_DIR)/uefi/src
	$(CC) $(US_DRIVER_CFLAGS) -c $< -o $@

# One rule for every translation unit; the include paths are the same for all
# of them, and third party sources are compiled with our flags on purpose so
# they see the shims.
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)/uefi/src $(BUILD_DIR)/core $(BUILD_DIR)/$(TOML)
	$(CC) $(US_DRIVER_CFLAGS) -c $< -o $@

# Assembly needs no C dialect flags, but does need the target.
$(BUILD_DIR)/%.o: %.S | $(BUILD_DIR)/uefi/src
	$(CC) --target=$(TARGET_TRIPLE) -ffreestanding -c $< -o $@

# Rebuild the library whenever the fork changes.
# llvm-ar is required: lld-link cannot resolve symbols through an index that
# GNU ar wrote for these COFF objects.
.PHONY: posix-uefi
posix-uefi:
	@$(MAKE) --no-print-directory -C $(POSIX_UEFI) \
	    CFLAGS="$(POSIX_CFLAGS)" AR=llvm-ar libuefi.a crt_aarch64.o

# A normal prerequisite, not order-only: when the library changes the driver
# has to be relinked against it.
$(DRIVER): $(DRIVER_OBJS) posix-uefi
	$(CC) $(US_DRIVER_LDFLAGS) -o $@ $(POSIX_CRT) $(DRIVER_OBJS) $(POSIX_LIB)
	@echo "built $@"

# Deployment. See tests/deploy for the details.
.PHONY: esp run check check-qemu clean
esp: $(DRIVER)
	@tests/deploy/build_esp.sh

run: esp
	@tests/deploy/run.sh

# Host side checks: pure logic, no firmware involved, seconds to run.
# The forks carry their own tests, so they are run from their own trees.
check:
	@$(MAKE) --no-print-directory -C $(POSIX_UEFI_TESTS) check
	@$(MAKE) --no-print-directory -C $(TOML_TESTS) check
	@$(MAKE) --no-print-directory -C tests/unit check
	@$(MAKE) --no-print-directory -C $(TOOLS_TESTS) check CORPUS=$(CORPUS)

# Checks that boot the image. Slower, but this is the only evidence that
# anything actually works. The Windows disk is external, so this one is
# skipped unless WIN_DISK points at it.
#
# esp is a prerequisite on purpose: a variant run leaves its own image behind,
# and without this the checks happily boot whatever was built last.
check-qemu: esp
	@tests/deploy/driver_boot.sh
	@tests/deploy/config_cases.sh
	@WIN_DISK=$(WIN_DISK) tests/deploy/boot_hook.sh

clean:
	rm -rf $(BUILD_DIR)
	@$(MAKE) --no-print-directory -C tests/unit clean >/dev/null 2>&1 || true
	@$(MAKE) --no-print-directory -C $(TOOLS_TESTS) clean >/dev/null 2>&1 || true
	@$(MAKE) --no-print-directory -C $(POSIX_UEFI_TESTS) clean >/dev/null 2>&1 || true
	@$(MAKE) --no-print-directory -C $(TOML_TESTS) clean >/dev/null 2>&1 || true
	@$(MAKE) --no-print-directory -C $(POSIX_UEFI) clean >/dev/null 2>&1 || true
