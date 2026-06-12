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
VIZ_LIBS := -lEGL -lGL -lX11 -lpng -ljpeg -lm
GUI_SRC  := $(wildcard gui/*.c)
GUI_BIN  := $(patsubst gui/%.c,$(BUILD)/%,$(GUI_SRC))

.PHONY: all test viz-test oracles apps gui clean
.SECONDARY: $(CORE_OBJ) $(VIZ_OBJ)        # keep object files; don't auto-delete
all: $(CORE_OBJ) $(APP_BIN)
apps: $(APP_BIN)
gui: $(GUI_BIN)

# Visual-correctness regression (needs a GL-capable environment: EGL/Mesa).
# Kept out of `make test` so the core suite stays GPU-free / portable.
viz-test: $(BUILD)/viz_golden
	$(BUILD)/viz_golden

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

# Build & run every C test against the committed (frozen) oracle vectors in
# tools/*_vectors.txt. These vectors are the verified Python<->C parity snapshot
# captured before the Python reference was removed; they are the regression
# reference now. No Python required.
test: $(TEST_BIN)
	@fail=0; for t in $(TEST_BIN); do \
	  echo "=== $$t ==="; $$t || fail=1; done; \
	  exit $$fail

# Regenerate the oracle vectors from a Python reference (only meaningful when a
# Python implementation is present; kept for provenance / future re-derivation).
oracles:
	@python3 tools/oracle_orbit.py
	@python3 tools/oracle_topology.py
	@python3 tools/oracle_routing.py
	@python3 tools/oracle_ground.py

clean:
	rm -rf $(BUILD)
