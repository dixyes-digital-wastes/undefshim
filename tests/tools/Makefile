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
all: peprobe

peprobe: peprobe.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c
	$(CC) $(CFLAGS) $^ -o $@

# Without a corpus there is nothing to probe, which is a skip, not a failure.
check: peprobe
	@if [ -z "$(CORPUS)" ]; then \
		echo "peprobe: no CORPUS given, skipping"; \
	else \
		./peprobe "$(CORPUS)"; \
	fi

clean:
	rm -f peprobe
