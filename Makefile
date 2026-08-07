# -------------------------------------------------------------------
# MiyooFin Makefile
# Targets:
#   all        — Host debug build
#   test       — Host unit tests (empty in Checkpoint A)
#   onionos    — Cross-compile via Docker (stub until toolchain is set)
#   package    — Stage OnionOS folder under output/package/
#   clean      — Remove output/
# -------------------------------------------------------------------

CXX         := g++
CXXFLAGS    := -std=c++17 -Wall -Wextra -Wpedantic -g -O0
LDFLAGS     :=
INCLUDES    := -I. -Iinclude

# SDL2 flags from pkg-config
SDL_CFLAGS  := $(shell pkg-config --cflags sdl2 2>/dev/null || echo '-I/usr/include/SDL2')
SDL_LIBS    := $(shell pkg-config --libs sdl2 2>/dev/null || echo '-lSDL2')

# Source files for Checkpoint A
SRC_DIR     := src
SRCS        := \
    $(SRC_DIR)/main.cpp \
    $(SRC_DIR)/app/App.cpp \
    $(SRC_DIR)/app/ScreenStack.cpp \
    $(SRC_DIR)/input/InputManager.cpp \
    $(SRC_DIR)/ui/BitmapFont.cpp \
    $(SRC_DIR)/ui/screens/StartupScreen.cpp \
    $(SRC_DIR)/ui/screens/InputDiagnosticsScreen.cpp

OBJS        := $(SRCS:src/%.cpp=output/build/%.o)
OUT_DIRS    := output/build/app output/build/input output/build/ui output/build/ui/screens

TARGET      := output/build/miyoofin

# -------------------------------------------------------------------
# Host build
# -------------------------------------------------------------------
.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS) | output/build
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(SDL_LIBS)
	@echo "  [LINK] $@"

output/build/%.o: src/%.cpp | $(OUT_DIRS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SDL_CFLAGS) -c -o $@ $<
	@echo "  [CC]   $@"

# Create output directories
$(OUT_DIRS):
	@mkdir -p $@

output/build:
	@mkdir -p $@

# -------------------------------------------------------------------
# Test
# -------------------------------------------------------------------
TEST_TARGET := output/test/test_runner
TEST_SRCS   := tests/test_main.cpp

.PHONY: test
test: $(TEST_TARGET)
	@$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRCS) | output/test
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^
	@echo "  [LINK] $@"

output/test:
	@mkdir -p $@

# -------------------------------------------------------------------
# OnionOS cross-compilation (stub — requires toolchain)
# -------------------------------------------------------------------
.PHONY: onionos
onionos:
	@echo "Toolchain not yet configured. See docs/toolchain.md."
	@echo "Run: docker build -f Dockerfile.onionos -t miyoofin-toolchain ."
	@echo "Then: docker run ... make -C /build cross"

# -------------------------------------------------------------------
# Package
# -------------------------------------------------------------------
PACKAGE_DIR := output/package/MiyooFin

.PHONY: package
package: $(TARGET)
	@rm -rf $(PACKAGE_DIR)
	@mkdir -p $(PACKAGE_DIR)/lib
	@mkdir -p $(PACKAGE_DIR)/assets
	@cp $(TARGET) $(PACKAGE_DIR)/MiyooFin
	@cp distributions/onionos/launch.sh $(PACKAGE_DIR)/
	@cp distributions/onionos/config.json $(PACKAGE_DIR)/
	@cp assets/icon.png $(PACKAGE_DIR)/ 2>/dev/null || true
	@cp assets/placeholder.png $(PACKAGE_DIR)/assets/ 2>/dev/null || true
	@echo "  [PACKAGE] $(PACKAGE_DIR)/"
	@echo "  Package staged. Bundle as: cd output/package && zip -r MiyooFin.zip MiyooFin/"

# -------------------------------------------------------------------
# Clean
# -------------------------------------------------------------------
.PHONY: clean
clean:
	@rm -rf output
	@echo "  [CLEAN]"

# -------------------------------------------------------------------
# Help
# -------------------------------------------------------------------
.PHONY: help
help:
	@echo "MiyooFin Makefile"
	@echo "  make         — Host build"
	@echo "  make test    — Run unit tests"
	@echo "  make onionos — Cross-compile for Miyoo (stub)"
	@echo "  make package — Stage OnionOS package"
	@echo "  make clean   — Remove output/"
	@echo "  make help    — This message"