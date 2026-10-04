# Top level build. Toolchain and inputs live in config.mk.

include config.mk

# A fresh id every build, so the running image can be matched to the artifact.
BUILD_ID ?= $(shell head -c 6 /dev/urandom | base64 | tr -d '+/=' | tr 'A-Z' 'a-z')

TOML := third_party/toml-c
SHIMS := $(POSIX_UEFI)/freestanding
POSIX_UEFI_TESTS := third_party/posix-uefi/tests
TOML_TESTS := third_party/toml-c/tests
TOOLS_TESTS := tests/tools

# --- the payload ---------------------------------------------------------
#
# A position independent blob, linked at zero and copied to wherever the boot
# decides. It gets none of the driver's flags: no firmware, no libc, and no
# floating point or SIMD, because on the target those may be unavailable and
# an exception path cannot afford to care.

PAYLOAD_DIR := payload
PAYLOAD_BUILD := $(BUILD_DIR)/payload

#
# x18 is reserved rather than allocated. The entry below the exception uses it
# as its one scratch register - it is the register the kernel rebuilds for
# itself at every kernel-mode entry, and the only one that can be spent there -
# and it has already destroyed the value the payload's own code had before a
# fault inside the payload is answered. Compiled code that kept anything in x18
# would be resumed with the entry's leftovers; reserving it means nothing does.
# Every header the sources include. Without this a change to a structure in
# one of them rebuilds only the files make happens to know depend on it, and a
# machine can end up running two different ideas of the same layout - which is
# how a pool that grew a field on one side and not the other reads as garbage.
US_HEADERS := $(wildcard core/*.h common/*.h payload/*.h uefi/src/*.h)

PAYLOAD_CFLAGS := --target=aarch64-none-elf -std=gnu23 -ffreestanding \
                  -ffixed-x18 \
                  -fshort-wchar -mno-red-zone -fno-stack-protector \
                  -fomit-frame-pointer -mgeneral-regs-only \
                  -fno-pic -fno-pie -fno-jump-tables -fno-builtin \
                  -fno-unwind-tables -fno-asynchronous-unwind-tables \
                  -mno-outline-atomics -fno-zero-initialized-in-bss \
                  -O2 -Wall -Wextra -I. -I$(PAYLOAD_BUILD)

PAYLOAD_SRCS := $(PAYLOAD_DIR)/payload.c $(PAYLOAD_DIR)/uart.c $(PAYLOAD_DIR)/us_mem.c \
                $(PAYLOAD_DIR)/selfmap.c $(PAYLOAD_DIR)/transfer.c $(PAYLOAD_DIR)/vamap.c \
                $(PAYLOAD_DIR)/early.c $(PAYLOAD_DIR)/rewrite.c \
                core/cache.c core/thunk.c \
                core/pgtable.c core/par.c core/ldapr.c core/ldr.c core/stackgen.c core/translate.c
PAYLOAD_ASM := $(PAYLOAD_DIR)/entry.S $(PAYLOAD_DIR)/end.S
PAYLOAD_OBJS := $(patsubst %.c,$(PAYLOAD_BUILD)/%.o,$(notdir $(PAYLOAD_SRCS))) \
                $(patsubst $(PAYLOAD_DIR)/%.S,$(PAYLOAD_BUILD)/%.o,$(PAYLOAD_ASM))

PAYLOAD_ELF := $(PAYLOAD_BUILD)/payload.elf
PAYLOAD_BIN := $(PAYLOAD_BUILD)/payload.bin
PAYLOAD_HDR := $(PAYLOAD_BUILD)/payload_blob.h

# --- the handover stub ---------------------------------------------------
#
# Copied into a spare slot in the loader, so it is assembled at zero and
# carries its one absolute address as a literal the boot fills in. It has to
# fit in the slot, which is what the size check is for: a stub that overran it
# would run off into whatever follows and there would be nothing to say so.

TRANSFER_SRC := $(PAYLOAD_DIR)/transfer.S
TRANSFER_OBJ := $(PAYLOAD_BUILD)/transfer_stub.o
TRANSFER_ELF := $(PAYLOAD_BUILD)/transfer.elf
TRANSFER_ELF := $(PAYLOAD_BUILD)/transfer.elf
TRANSFER_BIN := $(PAYLOAD_BUILD)/transfer.bin
TRANSFER_HDR := $(PAYLOAD_BUILD)/transfer_blob.h

# The slot the loader leaves for exactly this kind of thing.
US_TRANSFER_SLOT_BYTES := 128

# The shims come first on purpose: they supply the standard headers the
# compiler does not have for a freestanding target, and map them onto uefi.h.
US_DRIVER_CFLAGS := --target=$(TARGET_TRIPLE) -std=gnu23 -ffreestanding \
                    -fshort-wchar -mno-red-zone -fno-stack-protector \
                    -fomit-frame-pointer -O2 -Wall -Wextra \
                    -I. -I$(POSIX_UEFI) -I$(SHIMS) -I$(TOML) -I$(PAYLOAD_BUILD) \
                    -DTOML_NO_FLOAT -DTOML_NO_TIMESTAMP -DTOML_NO_FILE \
                    -DUS_BUILD_ID=\"$(BUILD_ID)\" $(EXTRA_CFLAGS)

US_DRIVER_LDFLAGS := --target=$(TARGET_TRIPLE) -nostdlib -fuse-ld=lld-link \
                     -Wl,-entry:uefi_init \
                     -Wl,-subsystem:efi_boot_service_driver \
                     -Wl,-base:0x50000000 -Wl,-stack:262144 -Wl,/Brepro $(EXTRA_LDFLAGS)

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

DRIVER_SRCS := uefi/src/config.c uefi/src/patch_apply.c uefi/src/registry.c \
               uefi/src/loadimage_hook.c core/cache.c uefi/src/console.c uefi/src/screen.c \
               uefi/src/pool.c uefi/src/service_hook.c uefi/src/gmm_hook.c \
               uefi/src/patch.c uefi/src/work.c uefi/src/payload_place.c uefi/src/arm.c \
               uefi/src/rewrite.c \
               uefi/src/acpi.c \
               uefi/src/vamap.c \
               uefi/src/session.c \
               core/cfg.c core/pe.c core/scan.c core/plan.c core/rva_patch.c core/pool.c \
               core/patchlist.c core/patchapply.c core/sha256.c \
               core/thunk.c core/ldapr.c core/acpi.c core/stackgen.c core/translate.c \
               core/par.c \
               $(TOML)/toml.c
DRIVER_ASM := uefi/src/stack.S
DRIVER_OBJS := $(DRIVER_MAIN_OBJ) \
               $(patsubst %.c,$(BUILD_DIR)/%.o,$(DRIVER_SRCS)) \
               $(patsubst %.S,$(BUILD_DIR)/%.o,$(DRIVER_ASM))
DRIVER := $(BUILD_DIR)/undefshim_driver.efi

all: $(DRIVER)

$(BUILD_DIR)/uefi/src $(BUILD_DIR)/core $(BUILD_DIR)/$(TOML) $(PAYLOAD_BUILD):
	@mkdir -p $@

# --- payload rules -------------------------------------------------------

# The frame offsets, from the structure that defines them. The entry is
# assembled against this, so the two halves of the payload cannot disagree.
$(PAYLOAD_BUILD)/layout_defs.inc: tests/tools/layoutgen.c $(PAYLOAD_DIR)/payload.h | $(PAYLOAD_BUILD)
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I. $< -o $(PAYLOAD_BUILD)/layoutgen
	$(PAYLOAD_BUILD)/layoutgen $@

$(PAYLOAD_BUILD)/%.o: $(PAYLOAD_DIR)/%.c | $(PAYLOAD_BUILD)
	$(CC) $(PAYLOAD_CFLAGS) -c $< -o $@

# A core source in the payload: the same file the driver builds, compiled for
# the blob's world instead. It is freestanding either way, which is the point
# of keeping it in core.
$(PAYLOAD_BUILD)/%.o: core/%.c | $(PAYLOAD_BUILD)
	$(CC) $(PAYLOAD_CFLAGS) -c $< -o $@

$(PAYLOAD_BUILD)/entry.o: $(PAYLOAD_DIR)/entry.S $(PAYLOAD_BUILD)/layout_defs.inc | $(PAYLOAD_BUILD)
	$(CC) $(PAYLOAD_CFLAGS) -c $< -o $@

$(PAYLOAD_BUILD)/%.o: $(PAYLOAD_DIR)/%.S | $(PAYLOAD_BUILD)
	$(CC) $(PAYLOAD_CFLAGS) -c $< -o $@

# Linked at zero with no libraries. --no-relax is not optional: a relaxation
# that turns a branch into a different one changes the size of the code the
# entry's PC relative addressing is measured against.
# Nothing inside the payload refers to these: they are entry points the code
# outside calls, and records the code outside writes. The link keeps them on
# purpose, and a missing one would show up as an offset of zero, which is a
# plausible looking answer rather than an obvious failure, so the header rule
# refuses zero as well.
PAYLOAD_KEEP := -Wl,--undefined=usTransferEntry -Wl,--undefined=usVAMapRecord \
                -Wl,--undefined=usVAMapHook -Wl,--undefined=usVAMapNotify

$(PAYLOAD_ELF): $(PAYLOAD_OBJS) $(PAYLOAD_DIR)/payload.lds
	$(CC) --target=aarch64-none-elf -nostdlib -fuse-ld=lld \
	    -Wl,-T,$(PAYLOAD_DIR)/payload.lds -Wl,--no-relax -Wl,--build-id=none \
	    -Wl,--gc-sections -o $@ $(PAYLOAD_OBJS) $(PAYLOAD_KEEP)

# The contract: nothing in the blob may name an absolute address, because it
# is copied somewhere the linker never knew about.
#
# Two checks, because they catch different things. A relocation means the
# linker had to leave something for a loader; an address left in the bytes
# means the linker resolved it at link time, which leaves no relocation behind
# at all. The second is the one that actually bit this project: a GOT entry
# filled with a link time address, reached through an indirect call.
#
# readelf reports "no relocations" in prose when the table is empty, so the
# check counts relocation entries rather than looking at whether it printed
# anything.
$(PAYLOAD_BIN): $(PAYLOAD_ELF) $(TOOLS_TESTS)/blobcheck.c | $(PAYLOAD_BUILD)
	@rel=$$(llvm-readelf -r $< | grep -c 'R_AARCH64' || true); \
	if [ "$$rel" != "0" ]; then \
	    echo "ERROR: the payload needs $$rel relocations to be loaded"; \
	    llvm-readelf -r $< | grep 'R_AARCH64'; \
	    exit 1; \
	fi
	llvm-objcopy -O binary $< $@
	@$(CC) -std=gnu23 -O2 -Wall -Wextra $(TOOLS_TESTS)/blobcheck.c -o $(PAYLOAD_BUILD)/blobcheck
	@$(PAYLOAD_BUILD)/blobcheck $@ $<

# The blob, as the driver sees it: bytes to copy, and the two offsets the
# boot needs. They are read out of the linked ELF rather than fixed by the
# link script, so the layout stays free to change.
$(PAYLOAD_HDR): $(PAYLOAD_BIN)
	@printf '/* Generated from %s. */\n' "$<" > $@
	@printf '#define US_PAYLOAD_BYTES %s\n' "$$(stat -c %s $<)" >> $@
	@for sym in usSyncEntry:ENTRY usSyncEntrySP0:ENTRYSP0 usPayloadConfigBlock:CONFIG usPayloadSelfTest:SELFTEST \
	           usPayloadHandle:HANDLE usTransferEntry:TRANSFER usVAMapRecord:VAMAP \
	           usVAMapHook:VAMAPHOOK usVAMapNotify:VAMAPNOTIFY \
	           usStackLookup:STACKLOOKUP usSyncStackReady:READY usSyncNoStack:NOSTACK; do \
	    name=$${sym%%:*}; tag=$${sym##*:}; \
	    off=$$(llvm-nm $(PAYLOAD_ELF) | awk -v w="$$name" \
	        '$$3==w {v=strtonum("0x"$$1)} $$3=="usPayloadStart" {s=strtonum("0x"$$1)} END{print v-s}'); \
	    if [ -z "$$off" ]; then echo "ERROR: $${name} is not in the payload"; exit 1; fi; \
	    if [ "$$off" = "0" ] && [ "$$name" != "usPayloadStart" ]; then \
	        echo "ERROR: $${name} is at offset zero, which means it was dropped"; \
	        exit 1; \
	    fi; \
	    printf '#define US_PAYLOAD_%s_OFFSET %s\n' "$$tag" "$$off" >> $@; \
	done
	@printf 'static const unsigned char kPayloadBlob[US_PAYLOAD_BYTES] = {\n#embed "payload.bin"\n};\n' >> $@
	@echo "payload header: $$(grep -c '^#define' $@) constants"

# --- the handover stub ---------------------------------------------------

$(TRANSFER_OBJ): $(TRANSFER_SRC) | $(PAYLOAD_BUILD)
	$(CC) $(PAYLOAD_CFLAGS) -c $< -o $@

# Linked before it is turned into bytes. The literal load is left as a
# relocation by the assembler, and objcopy on its own would put a zero where
# the offset belongs, which loads the instruction itself. Linking resolves it.
$(TRANSFER_ELF): $(TRANSFER_OBJ)
	$(CC) --target=aarch64-none-elf -nostdlib -fuse-ld=lld -Wl,-Ttext=0 \
	    -Wl,--image-base=0 -Wl,--no-relax -Wl,--build-id=none -o $@ $<
	@rel=$$(llvm-readelf -r $@ | grep -c 'R_AARCH64' || true); \
	if [ "$$rel" != "0" ]; then \
	    echo "ERROR: the stub still needs $$rel relocations to be placed"; \
	    exit 1; \
	fi

$(TRANSFER_BIN): $(TRANSFER_ELF)
	llvm-objcopy -O binary --only-section=.text $< $@
	@size=$$(stat -c %s $@); \
	if [ "$$size" -gt $(US_TRANSFER_SLOT_BYTES) ]; then \
	    echo "ERROR: the stub is $$size bytes, the slot is $(US_TRANSFER_SLOT_BYTES)"; \
	    exit 1; \
	fi
	@echo "stub: $$(stat -c %s $@) bytes of $(US_TRANSFER_SLOT_BYTES)"

# The offsets the boot needs: where the payload address goes, and where the
# stub's own code is. Both come from the linked image rather than being
# assumed, so the assembly stays free to change.
$(TRANSFER_HDR): $(TRANSFER_BIN) $(TRANSFER_ELF)
	@printf '/* Generated from %s. */\n' "$<" > $@
	@printf '#define US_TRANSFER_BYTES %s\n' "$$(stat -c %s $<)" >> $@
	@printf '#define US_TRANSFER_TARGET_OFFSET %s\n' \
	    "$$(llvm-nm $(TRANSFER_ELF) | awk '/ usTransferTarget$$/{print strtonum("0x"$$1)}')" >> $@
	@printf 'static const unsigned char kTransferStub[US_TRANSFER_BYTES] = {\n#embed "transfer.bin"\n};\n' >> $@

# Named explicitly because the source can come from outside the tree, and
# because the generated payload header has to exist before it is compiled.
$(DRIVER_MAIN_OBJ): $(DRIVER_MAIN) $(PAYLOAD_HDR) $(TRANSFER_HDR) $(US_HEADERS) | $(BUILD_DIR)/uefi/src
	$(CC) $(US_DRIVER_CFLAGS) -c $< -o $@

# Every other translation unit.
#
# The generated headers are real prerequisites here, not order-only ones. A
# generated header whose bytes are compiled into an object has to rebuild that
# object, or the old bytes are embedded again and the change looks like it did
# nothing at all. Keeping a list of the units that embed one was tried and
# forgot a unit three times over, so every unit is rebuilt instead: the
# objects are small, and the failure mode of the list is silent.
$(BUILD_DIR)/%.o: %.c $(PAYLOAD_HDR) $(TRANSFER_HDR) $(US_HEADERS) \
        | $(BUILD_DIR)/uefi/src $(BUILD_DIR)/core $(BUILD_DIR)/$(TOML)
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
# The fake kernel: an ordinary UEFI application, so it can use the firmware's
# console, built for a processor that has the instructions at issue.
FAKE_BUILD := $(BUILD_DIR)/fake
FAKE_CFLAGS := --target=$(TARGET_TRIPLE) -march=armv8.3-a -std=gnu23 -ffreestanding \
               -fshort-wchar -mno-red-zone -fno-stack-protector -fomit-frame-pointer \
               -O2 -Wall -Wextra -I. -I$(POSIX_UEFI) -I$(SHIMS)
# Its text is marked writable on purpose: the firmware maps a loaded image's
# code read-only, and this image exists to have its code written to - which is
# also how a real kernel's text is mapped when its own patching runs.
FAKE_LDFLAGS := --target=$(TARGET_TRIPLE) -march=armv8.3-a -nostdlib -fuse-ld=lld-link \
                -Wl,-entry:uefi_init -Wl,-subsystem:efi_application -Wl,-stack:262144 \
                -Wl,/Brepro -Xlinker /SECTION:.text,ERW

fake: $(FAKE_BUILD)/ntoskrnl.efi

$(FAKE_BUILD)/ntoskrnl.efi: tests/fake/ntoskrnl.c $(POSIX_CRT) $(POSIX_LIB) | $(FAKE_BUILD)
	$(CC) $(FAKE_CFLAGS) $(FAKE_LDFLAGS) -o $@ $< $(POSIX_CRT) $(POSIX_LIB)

$(FAKE_BUILD):
	mkdir -p $@

.PHONY: fake

esp: $(DRIVER)
	@tests/deploy/build_esp.sh

run: esp
	@tests/deploy/run.sh

# Host side checks: pure logic, no firmware involved, seconds to run.
# The forks carry their own tests, so they are run from their own trees.
check:
	@$(MAKE) --no-print-directory -C $(POSIX_UEFI_TESTS) check
	@$(MAKE) --no-print-directory -C $(TOML_TESTS) check
	@$(MAKE) --no-print-directory -C tests/unit check BUILD_DIR=$(BUILD_DIR)
	@$(MAKE) --no-print-directory -C $(TOOLS_TESTS) check CORPUS=$(CORPUS) PLAN_CORPUS=$(PLAN_CORPUS)

# Checks that boot the image. Slower, but this is the only evidence that
# anything actually works. The Windows disk is external, so this one is
# skipped unless WIN_DISK points at it.
#
# esp is a prerequisite on purpose: a variant run leaves its own image behind,
# and without this the checks happily boot whatever was built last.
check-qemu: esp
	@tests/deploy/driver_boot.sh
	@tests/deploy/config_cases.sh
	@tests/deploy/payload_case.sh
	@WIN_DISK=$(WIN_DISK) tests/deploy/boot_hook.sh
	@WIN_DISK=$(WIN_DISK) tests/deploy/patch_case.sh
	@WIN_DISK=$(WIN_DISK) PLAN_CORPUS=$(PLAN_CORPUS) tests/deploy/plan_case.sh
	@WIN_DISK=$(WIN_DISK) tests/deploy/breakpoint_case.sh
	@WIN_DISK=$(WIN_DISK) tests/deploy/vector_case.sh

clean:
	rm -rf $(BUILD_DIR)
	@$(MAKE) --no-print-directory -C tests/unit clean >/dev/null 2>&1 || true
	@$(MAKE) --no-print-directory -C $(TOOLS_TESTS) clean >/dev/null 2>&1 || true
	@$(MAKE) --no-print-directory -C $(POSIX_UEFI_TESTS) clean >/dev/null 2>&1 || true
	@$(MAKE) --no-print-directory -C $(TOML_TESTS) clean >/dev/null 2>&1 || true
	@$(MAKE) --no-print-directory -C $(POSIX_UEFI) clean >/dev/null 2>&1 || true
