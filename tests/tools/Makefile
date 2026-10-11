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

# What each tool read, asked of the preprocessor: a tool that links two core
# sources by hand is a tool that stops covering what it names the moment one of
# them includes something new
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
CHECK := ../deploy/check.sh
SKIP := 77

.PHONY: all check clean
all: peprobe planprobe holes

peprobe: peprobe.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c \
         $(ROOT)/core/locate.c $(ROOT)/core/site.c
	$(CC) $(CFLAGS) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

planprobe: planprobe.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c \
           $(ROOT)/core/locate.c $(ROOT)/core/site.c $(ROOT)/core/plan.c
	$(CC) $(CFLAGS) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

holes: holes.c $(ROOT)/core/pe.c $(ROOT)/core/scan.c \
       $(ROOT)/core/locate.c $(ROOT)/core/site.c
	$(CC) $(CFLAGS) $(US_SRCS) $(US_DEPFLAGS) -MT $@ -MF $@.d -o $@

# Named rather than worked out with wildcard: on a first build there are no .d
# files at all, and make has to be told they are missing rather than find none
-include peprobe.d planprobe.d holes.d

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
