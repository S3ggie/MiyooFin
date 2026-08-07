# -------------------------------------------------------------------
# MiyooFin Makefile
# Targets:
#   all          — Host debug build
#   test         — Host unit tests (empty in Checkpoint A)
#   onionos      — Cross-compile via Docker (builds ARMarch binary)
#   verify-arm   — Verify ARM binary architecture
#   package      — Stage OnionOS folder under output/package/ (uses ARM binary)
#   clean        — Remove output/
# -------------------------------------------------------------------

CXX         := g++
CXXFLAGS    := -std=c++17 -Wall -Wextra -Wpedantic -g -O0
LDFLAGS     :=
INCLUDES    := -I. -Iinclude

# SDL2 flags from pkg-config
SDL_CFLAGS  := $(shell pkg-config --cflags sdl2 2>/dev/null || echo '-I/usr/include/SDL2')
SDL_LIBS    := $(shell pkg-config --libs sdl2 2>/dev/null || echo '-lSDL2')

# libcurl flags from pkg-config
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null || echo '')
CURL_LIBS   := $(shell pkg-config --libs libcurl 2>/dev/null || echo '-lcurl')

# Source files
SRC_DIR     := src
SRCS        := \
    $(SRC_DIR)/main.cpp \
    $(SRC_DIR)/app/App.cpp \
    $(SRC_DIR)/app/ScreenStack.cpp \
    $(SRC_DIR)/data/MockData.cpp \
    $(SRC_DIR)/input/InputManager.cpp \
    $(SRC_DIR)/net/HttpClient.cpp \
    $(SRC_DIR)/net/JellyfinApi.cpp \
    $(SRC_DIR)/ui/BitmapFont.cpp \
    $(SRC_DIR)/ui/screens/HomeScreen.cpp \
    $(SRC_DIR)/ui/screens/StartupScreen.cpp \
    $(SRC_DIR)/ui/screens/ServerEntryScreen.cpp \
    $(SRC_DIR)/ui/screens/ConnectScreen.cpp \
    $(SRC_DIR)/ui/screens/InputDiagnosticsScreen.cpp

OBJS        := $(SRCS:src/%.cpp=output/build/%.o)
OUT_DIRS    := output/build/app output/build/data output/build/input \
               output/build/net output/build/ui output/build/ui/screens

TARGET      := output/build/miyoofin

# -------------------------------------------------------------------
# Host build
# -------------------------------------------------------------------
.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS) | output/build
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(SDL_LIBS) $(CURL_LIBS)
	@echo "  [LINK] $@"

output/build/%.o: src/%.cpp | $(OUT_DIRS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SDL_CFLAGS) $(CURL_CFLAGS) -c -o $@ $<
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
TEST_SRCS   := tests/test_main.cpp \
               src/net/JellyfinApi.cpp \
               src/net/HttpClient.cpp

.PHONY: test
test: $(TEST_TARGET)
	@$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRCS) | output/test
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(CURL_LIBS)
	@echo "  [LINK] $@"

output/test:
	@mkdir -p $@

# -------------------------------------------------------------------
# OnionOS cross-compilation via Docker
# -------------------------------------------------------------------
DOCKER_TAG := miyoofin-toolchain
ARM_TARGET := output/build-arm/miyoofin

.PHONY: onionos
onionos: $(DOCKER_TAG)
	@mkdir -p output/build-arm
	docker run --rm -v $(PWD):/build $(DOCKER_TAG) \
	    make -f Makefile.cross
	@echo "  [ONIONOS] $(ARM_TARGET)"

# Build the Docker toolchain image
$(DOCKER_TAG): Dockerfile.onionos
	docker build -f Dockerfile.onionos -t $(DOCKER_TAG) .

# -------------------------------------------------------------------
# Verify ARM binary architecture
# -------------------------------------------------------------------
.PHONY: verify-arm
verify-arm:
	@echo "=== Verifying ARM binary ==="
	@if [ ! -f $(ARM_TARGET) ]; then \
	    echo "ERROR: $(ARM_TARGET) not found. Run 'make onionos' first."; \
	    exit 1; \
	fi
	@echo "--- file output ---"
	file $(ARM_TARGET)
	@echo "--- readelf header ---"
	readelf -h $(ARM_TARGET) 2>/dev/null || arm-linux-gnueabihf-readelf -h $(ARM_TARGET)
	@echo "--- dynamic section (shared libs) ---"
	readelf -d $(ARM_TARGET) 2>/dev/null | grep NEEDED || arm-linux-gnueabihf-readelf -d $(ARM_TARGET) 2>/dev/null | grep NEEDED
	@echo "--- checking for x86-64 contamination ---"
	@file $(ARM_TARGET) | grep -qi 'x86-64' && { echo "FAIL: Binary is x86-64!"; exit 1; } || echo "OK: Not x86-64."
	@file $(ARM_TARGET) | grep -qi 'ARM' && echo "OK: Binary is ARM." || { echo "FAIL: Not ARM!"; exit 1; }
	@echo "--- checking GLIBC version requirements ---"
	@docker run --rm -v $(PWD):/build miyoofin-toolchain \
	    sh -c 'for f in $(ARM_TARGET) /usr/arm-linux-gnueabihf/lib/libSDL2-2.0.so.0.18.2 /usr/arm-linux-gnueabihf/lib/libstdc++.so.6 /usr/arm-linux-gnueabihf/lib/libgcc_s.so.1; do \
	        maxver=$$(arm-linux-gnueabihf-objdump -T "$$f" 2>/dev/null | grep -o "GLIBC_[0-9.]*" | sort -u -V | tail -1); \
	        echo "  $$(basename $$f): max $$maxver"; \
	        if [ "$$(echo "$$maxver" | sed "s/GLIBC_//")" != "2.28" ] && [ "$$(echo "$$maxver" | sed "s/GLIBC_//")" != "2.4" ] && [ "$$(echo "$$maxver" | sed "s/GLIBC_//")" != "2.18" ] && [ "$$(echo "$$maxver" | sed "s/GLIBC_//")" != "2.0" ]; then \
	            if [ "$$(echo "$$maxver" | sed "s/GLIBC_//" | cut -d. -f1)" -gt 2 ] || [ "$$(echo "$$maxver" | sed "s/GLIBC_//" | cut -d. -f2)" -gt 28 ]; then \
	                echo "FAIL: $$f requires $$maxver > 2.28!"; exit 1; \
	            fi; \
	        fi; \
	    done && echo "OK: All GLIBC requirements <= 2.28."'
	@echo "=== ARM verification passed ==="
# -------------------------------------------------------------------
# Package
# -------------------------------------------------------------------
PACKAGE_DIR := output/package/MiyooFin

.PHONY: package
package: $(ARM_TARGET)
	@rm -rf $(PACKAGE_DIR)
	@mkdir -p $(PACKAGE_DIR)/lib
	@mkdir -p $(PACKAGE_DIR)/assets
	@cp $(ARM_TARGET) $(PACKAGE_DIR)/miyoofin
	@cp distributions/onionos/launch.sh $(PACKAGE_DIR)/
	@cp distributions/onionos/config.json $(PACKAGE_DIR)/
	@cp assets/icon.png $(PACKAGE_DIR)/icon.png 2>/dev/null || true
	@cp assets/placeholder.png $(PACKAGE_DIR)/assets/placeholder.png 2>/dev/null || true
	@echo "  Bundling ARM shared libraries from toolchain..."
	@docker run --rm -v $(PWD)/$(PACKAGE_DIR)/lib:/out miyoofin-toolchain \
	    bash -c '\
	    cp -aP /usr/arm-linux-gnueabihf/lib/libSDL2-2.0.so.0 /out/ && \
	    cp -aP /usr/arm-linux-gnueabihf/lib/libSDL2-2.0.so.0.18.2 /out/ && \
	    cp -aP /usr/arm-linux-gnueabihf/lib/libSDL2.so /out/ && \
	    cp -aP /usr/arm-linux-gnueabihf/lib/libstdc++.so.6 /out/ && \
	    cp -aP /usr/arm-linux-gnueabihf/lib/libstdc++.so.6.0.25 /out/ && \
	    cp -aP /usr/arm-linux-gnueabihf/lib/libgcc_s.so.1 /out/ && \
	    echo "  Libraries bundled successfully"'
	@echo "  Verifying package binary architecture..."
	@file $(PACKAGE_DIR)/miyoofin | grep -qi 'ARM' || \
	    { echo "ERROR: $(PACKAGE_DIR)/miyoofin is NOT ARM!"; exit 1; }
	@echo "  Package uses ARM binary: OK"
	@echo "  Package staged at: $(PACKAGE_DIR)/"
	@echo "  Bundle as: cd output/package && zip -r MiyooFin.zip MiyooFin/"

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
	@echo "  make onionos    — Cross-compile for Miyoo via Docker"
	@echo "  make verify-arm — Verify ARM binary architecture"
	@echo "  make package    — Stage OnionOS package (uses ARMarch binary)"
	@echo "  make clean   — Remove output/"
	@echo "  make help    — This message"