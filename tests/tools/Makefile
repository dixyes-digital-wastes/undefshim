# Host tools. These inspect images; they are not part of the driver.
#
# The corpus is not in the repository, so it is passed in:
#   make check CORPUS=/path/to/windowsthings

ifeq ($(origin CC),default)
CC := clang
endif

ROOT := ../..
CORPUS ?=

CFLAGS := -std=gnu23 -O2 -Wall -Wextra -Wno-unused-function -I$(ROOT)

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
		echo "peprobe: no CORPUS given, skipping"; \
	else \
		./peprobe "$(CORPUS)"; \
	fi
	@if [ -z "$(PLAN_CORPUS)" ]; then \
		echo "planprobe: no PLAN_CORPUS given, skipping"; \
	else \
		./planprobe "$(PLAN_CORPUS)/winload.efi" "$(PLAN_CORPUS)/ntoskrnl.exe" || \
			echo "planprobe: the plan is incomplete"; \
	fi

clean:
	rm -f peprobe planprobe holes
