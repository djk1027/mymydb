CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -O2 -g -D_POSIX_C_SOURCE=200809L
LDFLAGS  ?=

NAME      = mymydb
BIN       = bin/$(NAME)   # the binary lives under bin/ (v2.2 layout)
TEST_BIN  = build/run_tests
BENCH_BIN = build/run_bench
OBJDIR    = build

# Include every source subdirectory so flat "#include" names resolve.
INCLUDES = $(addprefix -I,$(shell find src -type d))

# Library sources = everything under src/ except the CLI entry point.
LIB_SRCS  = $(filter-out src/cli/main.c,$(shell find src -name '*.c'))
LIB_OBJS  = $(patsubst src/%.c,$(OBJDIR)/src/%.o,$(LIB_SRCS))

CLI_OBJ   = $(OBJDIR)/src/cli/main.o

TEST_SRCS = $(shell find tests -name '*.c' 2>/dev/null)
TEST_OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(TEST_SRCS))

BENCH_SRCS = $(shell find bench -name '*.c' 2>/dev/null)
BENCH_OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(BENCH_SRCS))

.PHONY: all clean run test bench memcheck install

# Instance base path (v2.2 layout: <base>/{bin,data}). Mirrors the runtime's
# default resolution: $MYMY/mymydb, or $HOME/mymydb when $MYMY is unset.
# Override explicitly with `make install BASE=/path`.
BASE ?= $(if $(MYMY),$(MYMY)/mymydb,$(HOME)/mymydb)

all: $(BIN)

$(BIN): $(LIB_OBJS) $(CLI_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Generic object rule: mirror the source tree under build/. -MMD -MP emits a
# .d file listing each object's header prerequisites so edits to a header
# (e.g. a struct layout change) rebuild every dependent .c — avoiding stale
# objects with mismatched struct layouts.
$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(shell find $(OBJDIR) -name '*.d' 2>/dev/null)

$(TEST_BIN): $(LIB_OBJS) $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BENCH_BIN): $(LIB_OBJS) $(BENCH_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Install the built binary into the instance's bin/ directory. When the base
# already is the working tree (source == base), the binary is built in place.
install: $(BIN)
	@mkdir -p "$(BASE)/bin"
	@if [ "$(abspath $(BIN))" != "$(abspath $(BASE)/bin/$(NAME))" ]; then \
		cp -f $(BIN) "$(BASE)/bin/$(NAME)"; \
		echo "installed $(NAME) -> $(BASE)/bin/$(NAME)"; \
	else \
		echo "$(NAME) already in place: $(BASE)/bin/$(NAME)"; \
	fi

run: $(BIN)
	./$(BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

# Stability / leak check. Requires valgrind; runs the full test suite under it.
memcheck: $(TEST_BIN)
	valgrind --leak-check=full --errors-for-leak-kinds=all \
	         --error-exitcode=1 ./$(TEST_BIN)

clean:
	rm -rf $(OBJDIR) bin
