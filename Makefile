# ASTRA-C build system.
#   make            -> build the headless simulation core library + tests
#   make test       -> build & run all verification tests against Python oracle
#   make astra      -> build the GUI binary (requires raylib; see make raylib)
#   make raylib     -> vendor & build raylib into third_party/ (one-time)
#   make clean

CC      ?= gcc
CSTD    ?= -std=c11
WARN    := -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
OPT     ?= -O2 -march=native
CFLAGS  := $(CSTD) $(WARN) $(OPT) -Iinclude
LDLIBS  := -lm
BUILD   := build

# Core simulation sources (headless, no graphics dependency).
CORE_SRC := $(wildcard src/*.c)
CORE_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRC))

# Test programs (each tests/*.c is its own binary).
TEST_SRC := $(wildcard tests/*.c)
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))

.PHONY: all test clean
all: $(CORE_OBJ)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%: tests/%.c $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< $(CORE_OBJ) -o $@ $(LDLIBS)

$(BUILD):
	@mkdir -p $(BUILD)

# Regenerate Python oracle vectors, then build & run every test binary.
test: $(TEST_BIN)
	@python3 tools/oracle_orbit.py
	@python3 tools/oracle_topology.py
	@python3 tools/oracle_routing.py
	@fail=0; for t in $(TEST_BIN); do \
	  echo "=== $$t ==="; $$t || fail=1; done; \
	  exit $$fail

clean:
	rm -rf $(BUILD)
