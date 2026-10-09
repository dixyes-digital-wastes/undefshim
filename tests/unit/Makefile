# Unit tests for this project. Everything here runs on the host.
#
# Tests for the forks live with the forks: posix-uefi's under
# third_party/posix-uefi/tests, toml-c's under third_party/toml-c/tests. What
# is checked here is our own logic, and how it is wired to the parser.

# CC is predefined by make, so a plain ?= would never take effect.
ifeq ($(origin CC),default)
CC := clang
endif

ROOT := ../..
TOML := $(ROOT)/third_party/toml-c

# core/cfg is built the way the driver builds it, against the same optional
# subsets of the parser, so the tests exercise that configuration.
TOML_DEFS := -DTOML_NO_FLOAT -DTOML_NO_TIMESTAMP -DTOML_NO_FILE
CFG_CFLAGS := -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) -I$(TOML) $(TOML_DEFS)

.PHONY: all check clean
all: test_cfg test_layout test_scan_images test_rva_patch test_plan test_pgtable test_par \
     test_thunk test_stackgen test_payload_place test_early test_ldapr test_acpi test_ldr \
     test_patchlist test_screen test_driver_hook test_drvhook test_panic test_panic_face test_fb

# The face of a panic report, drawn on the host over the primitives the
# payload draws with. The picture is the one the payload carries, so what is
# looked at is what a machine would show, and what is checked is where the
# paper, the picture and the text are
test_panic_face: test_panic_face.c $(ROOT)/payload/panelface.c \
                 $(ROOT)/payload/panelface.h $(ROOT)/payload/panicimage.h \
                 $(ROOT)/core/pixel.h $(ROOT)/common/font.h
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I. -I$(ROOT) \
	    test_panic_face.c $(ROOT)/payload/panelface.c -o $@

# How many two megabyte entries the frame buffer's mapping takes. The
# mapping itself needs page tables and a machine; this is the arithmetic it
# is worked out from, which is the part that was wrong once and left the last
# rows of a screen unmapped
test_fb: test_fb.c $(ROOT)/payload/fb.h $(ROOT)/core/pgtable.h \
         $(ROOT)/core/pixel.h
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) test_fb.c -o $@

# The name a stop is asked for by, compared the way the configuration hands it
# over: a pointer and a length rather than a terminated string. The comparison
# is in the header, so this links nothing
test_panic: test_panic.c $(ROOT)/uefi/panic.h
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) test_panic.c -o $@

test_early: test_early.c $(ROOT)/payload/early.c $(ROOT)/payload/vamap.c \
            $(ROOT)/core/thunk.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

# The screen drawer is built for the host with the firmware headers, and the
# frame buffer it draws into is an array in the test.
# The console is linked in as well: the preview drives the real one into the
# real drawer, so what is looked at is what a machine would draw
test_screen: test_screen.c $(ROOT)/uefi/screen.c $(ROOT)/uefi/screen.h \
             $(ROOT)/uefi/console.c $(ROOT)/uefi/console.h $(ROOT)/common/font.h \
             $(ROOT)/uefi/panic.c $(ROOT)/uefi/panic.h $(ROOT)/common/log.h
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I. -I$(ROOT) \
	    -I$(ROOT)/third_party/posix-uefi/uefi \
	    test_screen.c $(ROOT)/uefi/screen.c $(ROOT)/uefi/console.c \
	    $(ROOT)/uefi/panic.c -o $@

test_patchlist: test_patchlist.c $(ROOT)/core/patchlist.c $(ROOT)/core/patchapply.c \
                $(ROOT)/core/sha256.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_ldr: test_ldr.c $(ROOT)/core/ldr.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_acpi: test_acpi.c $(ROOT)/core/acpi.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_ldapr: test_ldapr.c $(ROOT)/core/ldapr.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_thunk: test_thunk.c $(ROOT)/core/thunk.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_stackgen: test_stackgen.c $(ROOT)/core/stackgen.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

# BUILD_DIR is relative to the repository root, as in the top-level Makefile.
# Build only the blob; no firmware or AArch64 code runs in this host test.
BUILD_DIR ?= build
PLACEMENT_PAYLOAD_DIR := $(ROOT)/$(BUILD_DIR)/payload
PLACEMENT_SRCS := test_payload_place.c $(ROOT)/uefi/payload_place.c \
                  $(ROOT)/core/stackgen.c
PLACEMENT_HEADERS := payload_place_host/stdio.h \
                     $(ROOT)/common/layout.h $(ROOT)/core/stackgen.h \
                     $(ROOT)/payload/payload.h $(ROOT)/uefi/session.h \
                     $(ROOT)/uefi/payload_place.h

.PHONY: placement-payload
placement-payload:
	$(MAKE) --no-print-directory -C $(ROOT) BUILD_DIR=$(BUILD_DIR) \
	    $(BUILD_DIR)/payload/payload_blob.h

test_payload_place: placement-payload $(PLACEMENT_SRCS) $(PLACEMENT_HEADERS)
	$(CC) -std=gnu23 -O2 -Wall -Wextra -fshort-wchar -fno-builtin \
	    -Ipayload_place_host -I$(ROOT) -I$(TOML) \
	    -I$(ROOT)/third_party/posix-uefi/uefi \
	    -I$(PLACEMENT_PAYLOAD_DIR) $(TOML_DEFS) \
	    $(PLACEMENT_SRCS) -o $@

test_cfg: test_cfg.c $(ROOT)/core/cfg.c $(TOML)/toml.c
	$(CC) $(CFG_CFLAGS) $^ -o $@

test_layout: test_layout.c $(ROOT)/core/pool.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

# The region scan, and the descriptor base under it. Both are shape and
# content only, so the images are built here and no Windows binary is needed
test_scan_images: test_scan_images.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

# The locator and the two PE facts under it: the export table and the
# exception table. The image is built in memory, so no Windows binary is
# needed and the test states exactly which property makes the shape
# recognizable
test_driver_hook: test_driver_hook.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c \
                 $(ROOT)/core/thunk.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

# The entry a diverted kernel call arrives at. It reads no register state, so
# it runs on the host with a configuration of the test's own making and a stand
# -in for the function it calls through to
test_drvhook: test_drvhook.c $(ROOT)/payload/drvhook.c $(ROOT)/core/ldapr.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_rva_patch: test_rva_patch.c $(ROOT)/core/pe.c $(ROOT)/core/rva_patch.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_plan: test_plan.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c $(ROOT)/core/plan.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_pgtable: test_pgtable.c $(ROOT)/core/pgtable.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_par: test_par.c $(ROOT)/core/par.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

check: all
	@./test_cfg
	@./test_layout
	@./test_scan_images
	@./test_rva_patch
	@./test_plan
	@./test_pgtable
	@./test_par
	@./test_thunk
	@./test_stackgen
	@./test_payload_place
	@./test_early
	@./test_ldapr
	@./test_patchlist
	@./test_screen
	@./test_panic
	@./test_panic_face
	@./test_fb
	@./test_acpi
	@./test_ldr
	@./test_driver_hook
	@./test_drvhook
	@python3 check_entry_frame.py $(ROOT)/payload/entry.S
	@python3 check_rewrite.py
	@python3 check_pte.py
	@python3 check_watch.py
	@python3 check_layout.py $(ROOT)
	@python3 check_encodings.py $(ROOT)