CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -O2 -g -D_POSIX_C_SOURCE=200809L
LDFLAGS  ?=

BIN       = mymydb
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

.PHONY: all clean run test bench memcheck

all: $(BIN)

$(BIN): $(LIB_OBJS) $(CLI_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Generic object rule: mirror the source tree under build/.
$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TEST_BIN): $(LIB_OBJS) $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BENCH_BIN): $(LIB_OBJS) $(BENCH_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

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
	rm -rf $(OBJDIR) $(BIN)
