# Host tools. These inspect images; they are not part of the driver.
#
# The corpus is not in the repository, so it is passed in:
#   make check CORPUS=/path/to/windowsthings

ifeq ($(origin CC),default)
CC := clang
endif

ROOT := ../..
CORPUS ?=

# -I../unit is where the header every check reports through lives: these are
# host programs that check things too, and one way of saying what they found is
# the point of it
CFLAGS := -std=gnu23 -O2 -Wall -Wextra -Wno-unused-function -I$(ROOT) -I../unit

# The reporter, and the code a check that skipped exits with. A skip is not a
# failure: the point of the run is to find out what this machine can be asked,
# and "nothing, no corpus" is an answer
CHECK := ../deploy/check.sh
SKIP := 77

.PHONY: all check clean
all: peprobe planprobe holes

peprobe: peprobe.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c
	$(CC) $(CFLAGS) $^ -o $@

planprobe: planprobe.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c $(ROOT)/core/plan.c
	$(CC) $(CFLAGS) $^ -o $@

holes: holes.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c
	$(CC) $(CFLAGS) $^ -o $@

# Without a corpus there is nothing to probe, which is a skip, not a failure.
check: all
	@if [ -z "$(CORPUS)" ]; then \
		$(CHECK) skip peprobe "no CORPUS given" || test $$? -eq $(SKIP); \
	else \
		./peprobe "$(CORPUS)"; \
	fi
	@if [ -z "$(PLAN_CORPUS)" ]; then \
		$(CHECK) skip planprobe "no PLAN_CORPUS given" || test $$? -eq $(SKIP); \
	else \
		./planprobe "$(PLAN_CORPUS)/winload.efi" "$(PLAN_CORPUS)/ntoskrnl.exe"; \
	fi

clean:
	rm -f peprobe planprobe holes
