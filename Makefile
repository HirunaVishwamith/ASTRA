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
CFLAGS  := $(CSTD) $(WARN) $(OPT) -Iinclude -pthread
LDLIBS  := -lm -pthread
BUILD   := build

# Core simulation sources (headless, no graphics dependency).
CORE_SRC := $(wildcard src/*.c)
CORE_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRC))

# Test programs (each tests/*.c is its own binary).
TEST_SRC := $(wildcard tests/*.c)
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))

# Headless apps (each apps/*.c is its own binary).
APP_SRC := $(wildcard apps/*.c)
APP_BIN := $(patsubst apps/%.c,$(BUILD)/%,$(APP_SRC))

# Visualization: renderer library (viz/*.c) + GUI/headless-render binaries
# (gui/*.c). Uses GLX/X11 + EGL directly (no GLFW). PNG via libpng.
VIZ_SRC  := $(wildcard viz/*.c)
VIZ_OBJ  := $(patsubst viz/%.c,$(BUILD)/viz_%.o,$(VIZ_SRC))
VIZ_LIBS := -lEGL -lGL -lX11 -lpng -lm
GUI_SRC  := $(wildcard gui/*.c)
GUI_BIN  := $(patsubst gui/%.c,$(BUILD)/%,$(GUI_SRC))

.PHONY: all test apps gui clean
.SECONDARY: $(CORE_OBJ) $(VIZ_OBJ)        # keep object files; don't auto-delete
all: $(CORE_OBJ) $(APP_BIN)
apps: $(APP_BIN)
gui: $(GUI_BIN)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/viz_%.o: viz/%.c | $(BUILD)
	$(CC) $(CFLAGS) -Iviz -c $< -o $@

$(BUILD)/%: tests/%.c $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< $(CORE_OBJ) -o $@ $(LDLIBS)

$(BUILD)/%: apps/%.c $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< $(CORE_OBJ) -o $@ $(LDLIBS)

$(BUILD)/%: gui/%.c $(CORE_OBJ) $(VIZ_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) -Iviz $< $(CORE_OBJ) $(VIZ_OBJ) -o $@ $(LDLIBS) $(VIZ_LIBS)

$(BUILD):
	@mkdir -p $(BUILD)

# Regenerate Python oracle vectors, then build & run every test binary.
test: $(TEST_BIN)
	@python3 tools/oracle_orbit.py
	@python3 tools/oracle_topology.py
	@python3 tools/oracle_routing.py
	@python3 tools/oracle_ground.py
	@fail=0; for t in $(TEST_BIN); do \
	  echo "=== $$t ==="; $$t || fail=1; done; \
	  exit $$fail

clean:
	rm -rf $(BUILD)
