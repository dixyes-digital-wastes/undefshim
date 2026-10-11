# Unit tests for this project. Everything here runs on the host.
#
# Tests for the forks live with the forks: posix-uefi's under
# third_party/posix-uefi/tests, toml-c's under third_party/toml-c/tests. What
# is checked here is our own logic, and how it is wired to the parser.

# CC is predefined by make, so a plain ?= would never take effect.
ifeq ($(origin CC),default)
CC := clang
endif

# What each test read, asked of the preprocessor
#
# A test includes the header of the thing it tests, and a rule that lists its
# sources by hand is a rule that stops being true the first time one of them
# includes something new - which is a test that passes while running
# yesterday's object. The target in each file is the test binary, because
# there is no object to name
#
# Written at each compile and not kept in a variable: $@ is expanded where the
# variable is written, and at that point make has not decided which target the
# recipe is for
US_DEPFLAGS := -MMD -MP

# The sources and not the headers: $^ is every prerequisite, and the .d files
# have just made the headers prerequisites as well, so passing $^ to the
# compiler hands it a header as an input - which clang refuses with a message
# about multiple output files rather than about the header
US_SRCS = $(filter-out %.h,$(^))

ROOT := ../..
TOML := $(ROOT)/third_party/toml-c

# core/cfg is built the way the driver builds it, against the same optional
# subsets of the parser, so the tests exercise that configuration.
TOML_DEFS := -DTOML_NO_FLOAT -DTOML_NO_TIMESTAMP -DTOML_NO_FILE
CFG_CFLAGS := -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) -I$(TOML) $(TOML_DEFS)

.PHONY: all check clean
all: test_cfg test_layout test_scan_images test_rva_patch test_plan test_pgtable test_par \
     test_thunk test_stackgen test_payload_place test_early test_ldapr test_acpi test_ldr \
     test_patchlist test_screen test_driver_hook test_drvhook test_panic test_panic_face test_fb \
     test_stage

# The face of a panic report, drawn on the host over the primitives the
# payload draws with. The picture is the one the payload carries, so what is
# looked at is what a machine would show, and what is checked is where the
# paper, the picture and the text are
test_panic_face: test_panic_face.c $(ROOT)/payload/panelface.c \
                 $(ROOT)/payload/panelface.h $(ROOT)/payload/panicimage.h \
                 $(ROOT)/core/pixel.h $(ROOT)/common/font.h
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I. -I$(ROOT) \
	    test_panic_face.c $(ROOT)/payload/panelface.c $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

# How many two megabyte entries the frame buffer's mapping takes. The
# mapping itself needs page tables and a machine; this is the arithmetic it
# is worked out from, which is the part that was wrong once and left the last
# rows of a screen unmapped
test_fb: test_fb.c $(ROOT)/payload/fb.h $(ROOT)/core/pgtable.h \
         $(ROOT)/core/pixel.h
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) test_fb.c $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

# The name a stop is asked for by, compared the way the configuration hands it
# over: a pointer and a length rather than a terminated string. The comparison
# is in the header, so this links nothing
test_panic: test_panic.c $(ROOT)/uefi/panic.h
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) test_panic.c $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_early: test_early.c $(ROOT)/payload/early.c $(ROOT)/payload/vamap.c \
            $(ROOT)/core/thunk.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

# The stage names, with the console they are said through stood in for: what
# is checked is the table, and linking the firmware library for a real console
# would need a screen and a port to go with it. The firmware headers are on the
# path because stage.c reaches the console's declaration through them
test_stage: test_stage.c $(ROOT)/uefi/stage.c $(ROOT)/uefi/stage.h
	$(CC) -std=gnu23 -O2 -Wall -Wextra -Wno-incompatible-library-redeclaration \
	    -Wno-builtin-requires-header -I. -I$(ROOT) \
	    -I$(ROOT)/third_party/posix-uefi/uefi \
	    -I$(ROOT)/third_party/posix-uefi/uefi/freestanding \
	    test_stage.c $(ROOT)/uefi/stage.c $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

# The screen drawer is built for the host with the firmware headers, and the
# frame buffer it draws into is an array in the test.
# The console is linked in as well: the preview drives the real one into the
# real drawer, so what is looked at is what a machine would draw
# uart_hw.c is linked in as well: the console is one file's worth of
# formatting over a port, and the port the driver uses is the one under test.
# Leaving it out left the test linking the stand-in definitions console.c
# carries for a build with no port at all, which is a different program
test_screen: test_screen.c $(ROOT)/uefi/screen.c $(ROOT)/uefi/screen.h \
             $(ROOT)/uefi/gop.c $(ROOT)/uefi/gop.h \
             $(ROOT)/uefi/console.c $(ROOT)/uefi/console.h $(ROOT)/common/font.h \
             $(ROOT)/uefi/ansi.c $(ROOT)/uefi/ansi.h \
             $(ROOT)/uefi/log.c $(ROOT)/uefi/log.h \
             $(ROOT)/uefi/uart_hw.c $(ROOT)/uefi/uart_hw.h \
             $(ROOT)/uefi/panic.c $(ROOT)/uefi/panic.h $(ROOT)/common/log.h
	$(CC) -std=gnu23 -O2 -Wall -Wextra -Wno-incompatible-library-redeclaration \
	    -I. -I$(ROOT) \
	    -I$(ROOT)/third_party/posix-uefi/uefi \
	    test_screen.c $(ROOT)/uefi/screen.c $(ROOT)/uefi/gop.c \
	    $(ROOT)/uefi/console.c $(ROOT)/uefi/ansi.c $(ROOT)/uefi/log.c \
	    $(ROOT)/uefi/uart_hw.c \
	    $(ROOT)/uefi/panic.c $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_patchlist: test_patchlist.c $(ROOT)/core/patchlist.c $(ROOT)/core/patchapply.c \
                $(ROOT)/core/sha256.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) -DUS_ROOT='"$(ROOT)"' $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_ldr: test_ldr.c $(ROOT)/core/ldr.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_acpi: test_acpi.c $(ROOT)/core/acpi.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_ldapr: test_ldapr.c $(ROOT)/core/ldapr.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_thunk: test_thunk.c $(ROOT)/core/thunk.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_stackgen: test_stackgen.c $(ROOT)/core/stackgen.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

# BUILD_DIR is relative to the repository root, as in the top-level Makefile.
# Build only the blob; no firmware or AArch64 code runs in this host test.
BUILD_DIR ?= build
PLACEMENT_PAYLOAD_DIR := $(ROOT)/$(BUILD_DIR)/payload
PLACEMENT_SRCS := test_payload_place.c $(ROOT)/uefi/payload_place.c \
                  $(ROOT)/uefi/stage.c $(ROOT)/core/stackgen.c
PLACEMENT_HEADERS := payload_place_host/stdio.h \
                     $(ROOT)/common/layout.h $(ROOT)/core/stackgen.h \
                     $(ROOT)/uefi/stage.h \
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
	    $(PLACEMENT_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_cfg: test_cfg.c $(ROOT)/core/cfg.c $(TOML)/toml.c
	$(CC) $(CFG_CFLAGS) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_layout: test_layout.c $(ROOT)/core/pool.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

# The region scan, and the descriptor base under it. Both are shape and
# content only, so the images are built here and no Windows binary is needed
test_scan_images: test_scan_images.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c \
                  $(ROOT)/core/locate.c $(ROOT)/core/site.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

# The locator and the two PE facts under it: the export table and the
# exception table. The image is built in memory, so no Windows binary is
# needed and the test states exactly which property makes the shape
# recognizable
test_driver_hook: test_driver_hook.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c \
                 $(ROOT)/core/locate.c $(ROOT)/core/site.c $(ROOT)/core/thunk.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

# The entry a diverted kernel call arrives at. It reads no register state, so
# it runs on the host with a configuration of the test's own making and a stand
# -in for the function it calls through to
test_drvhook: test_drvhook.c $(ROOT)/payload/drvhook.c $(ROOT)/core/rcpc_scan.c \
              $(ROOT)/core/ldapr.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_rva_patch: test_rva_patch.c $(ROOT)/core/pe.c $(ROOT)/core/rva_patch.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_plan: test_plan.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c \
           $(ROOT)/core/locate.c $(ROOT)/core/site.c $(ROOT)/core/plan.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_pgtable: test_pgtable.c $(ROOT)/core/pgtable.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

test_par: test_par.c $(ROOT)/core/par.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

# Named from the same list the tests are named in, and included rather than
# worked out with wildcard: on a first build there are no .d files at all, and
# make has to be told they are missing rather than find none of them
US_TEST_BINS := test_cfg test_layout test_scan_images test_rva_patch test_plan \
                test_pgtable test_par test_thunk test_stackgen test_payload_place \
                test_early test_ldapr test_acpi test_ldr test_patchlist test_screen \
                test_driver_hook test_drvhook test_panic test_panic_face test_fb \
                test_stage
-include $(addsuffix .d,$(US_TEST_BINS))

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
	@./test_stage
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