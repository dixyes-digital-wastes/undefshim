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
     test_thunk test_ldapr test_acpi test_ldr

test_ldr: test_ldr.c $(ROOT)/core/ldr.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_acpi: test_acpi.c $(ROOT)/core/acpi.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_ldapr: test_ldapr.c $(ROOT)/core/ldapr.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_thunk: test_thunk.c $(ROOT)/core/thunk.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_cfg: test_cfg.c $(ROOT)/core/cfg.c $(TOML)/toml.c
	$(CC) $(CFG_CFLAGS) $^ -o $@

test_layout: test_layout.c $(ROOT)/core/pool.c
	$(CC) -std=gnu23 -O2 -Wall -Wextra -I$(ROOT) $^ -o $@

test_scan_images: test_scan_images.c $(ROOT)/core/pe.c
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
	./test_cfg
	./test_layout
	./test_scan_images
	./test_rva_patch
	./test_plan
	./test_pgtable
	./test_par
	./test_thunk
	./test_ldapr
	./test_acpi
	./test_ldr
	python3 check_entry_frame.py $(ROOT)/payload/entry.S