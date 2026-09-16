# -------------------------------------------------------------------
# MiyooFin Makefile
# Targets:
#   all          — Host debug build
#   test         — Host unit tests
#   onionos      — Cross-compile via Docker (builds ARM binary)
#   verify-arm   — Verify ARM binary architecture
#   package      — Stage OnionOS folder under output/package/ (uses ARM binary)
#   clean        — Remove output/
# -------------------------------------------------------------------

CXX         := g++
CC          := gcc
PERF_TELEMETRY ?= 1
RELEASE     ?= 0
# Opt-in sanitizer switch for host builds only (never forwarded to
# Makefile.cross, so the ARM build is unaffected).
#   SANITIZE=1 make test   — or the `test-sanitize` convenience target below.
# Default (SANITIZE=0) leaves CXXFLAGS/LDFLAGS exactly as before.
SANITIZE    ?= 0
CXXFLAGS    := -std=c++17 -Wall -Wextra -Wpedantic -g -O0 -DMIYOOFIN_ENABLE_PERF_TELEMETRY=$(PERF_TELEMETRY)
LDFLAGS     :=
ifeq ($(SANITIZE),1)
SANITIZE_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g
CXXFLAGS    += $(SANITIZE_FLAGS)
LDFLAGS     += -fsanitize=address,undefined
SQLITE_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g
endif
INCLUDES    := -I. -Iinclude
SQLITE_DIR  := vendor/sqlite
SQLITE_SRC  := $(SQLITE_DIR)/sqlite3.c
SQLITE_DEFINES := -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 \
                 -DSQLITE_DQS=0 -DSQLITE_TRUSTED_SCHEMA=0 \
                 -DSQLITE_OMIT_LOAD_EXTENSION
SQLITE_CFLAGS := -Os $(SQLITE_DEFINES)
SQLITE_HOST_OBJ := output/build/sqlite/sqlite3.o

# SDL2 flags from pkg-config
SDL_CFLAGS  := $(shell pkg-config --cflags sdl2 2>/dev/null || echo '-I/usr/include/SDL2')
SDL_LIBS    := $(shell pkg-config --libs sdl2 2>/dev/null || echo '-lSDL2')

# libcurl flags from pkg-config
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null || echo '')
CURL_LIBS   := $(shell pkg-config --libs libcurl 2>/dev/null || echo '-lcurl')

# Source files
SRC_DIR     := src
include sources.mk

SRCS        := $(MIYOOFIN_PROD_SRCS) $(TELEMETRY_SRCS)

OBJS        := $(SRCS:src/%.cpp=output/build/%.o)
OBJS        += $(SQLITE_HOST_OBJ)
DEPS        := $(OBJS:.o=.d)
OUT_DIRS    := output/build/app output/build/data output/build/input \
               output/build/image output/build/net output/build/cache \
               output/build/download output/build/catalog \
               output/build/library \
               output/build/diagnostics \
               output/build/ui output/build/ui/screens \
               output/build/playback \
               output/build/update

TARGET      := output/build/miyoofin

.DEFAULT_GOAL := all
-include $(DEPS)

# -------------------------------------------------------------------
# Host build
# -------------------------------------------------------------------
.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS) | output/build
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(SDL_LIBS) $(CURL_LIBS)
	@echo "  [LINK] $@"

output/build/%.o: src/%.cpp | $(OUT_DIRS)
	$(CXX) $(CXXFLAGS) -MMD -MP $(INCLUDES) $(SDL_CFLAGS) $(CURL_CFLAGS) -c -o $@ $<
	@echo "  [CC]   $@"

# Vendored third-party stb_image triggers -Wunused-parameter under -Wall
# -Wextra. Suppress only that warning for only this translation unit so our
# own unused parameters are still diagnosed. Third-party source is not edited.
output/build/image/stb_image_impl.o: CXXFLAGS += -Wno-unused-parameter
output/test/objects/image/stb_image_impl.o: TEST_CXXFLAGS += -Wno-unused-parameter

$(SQLITE_HOST_OBJ): $(SQLITE_SRC) $(SQLITE_DIR)/sqlite3.h | output/build/sqlite
	$(CC) $(SQLITE_CFLAGS) -MMD -MP -I$(SQLITE_DIR) -c -o $@ $<
	@echo "  [CC]   $@"

# Create output directories
$(OUT_DIRS):
	@mkdir -p $@

output/build:
	@mkdir -p $@

output/build/sqlite:
	@mkdir -p $@

# -------------------------------------------------------------------
# Test
# -------------------------------------------------------------------
TEST_TARGET := output/test/test_runner
SQLITE_TEST_TARGET := output/test/test_sqlite_build
SQLITE_TEST_SRC := tests/test_sqlite_build.cpp
CATALOG_BENCHMARK_TARGET := output/test/test_catalog_journal_benchmark
CATALOG_BENCHMARK_TEST_SRC := tests/test_catalog_journal_benchmark.cpp
CATALOG_BENCHMARK_SRC := tools/catalog_journal_benchmark.cpp
CATALOG_BENCHMARK_MAIN := tools/catalog_journal_benchmark_main.cpp
CATALOG_BENCHMARK_RUNNER := output/build/catalog-journal-benchmark
TEST_CXXFLAGS := $(CXXFLAGS) -DMIYOOFIN_TELEMETRY_HOST_TEST=1 -DMIYOOFIN_TEST_BUILD=1
RUNNER_TEST := tests/test_playback_runner.sh
CA_BUNDLE_TEST := tests/test_ca_bundle.sh
TELEMETRY_DECODER_TEST := tests/test_telemetry_decoder.py
ONION_REMOTE_LAUNCH_TEST := tests/test_onion_remote_launcher.sh
TEST_GROUPS := catalog api_session ui_foundation ui_models cache_offline \
               artwork_episode downloads misc playback telemetry telemetry_format telemetry_service telemetry_schema \
               catalog_parity_query catalog_parity_hierarchy catalog_parity_sync api_core api_events session \
               imagecache update
TEST_GROUP_TARGETS := $(addprefix output/test/test_,$(TEST_GROUPS))
TEST_PROD_SRCS := $(MIYOOFIN_TEST_SRCS)
TEST_PROD_OBJS := $(TEST_PROD_SRCS:src/%.cpp=output/test/objects/%.o)
TEST_PROD_DEPS := $(TEST_PROD_OBJS:.o=.d)
TEST_PROD_LIB := output/test/libmiyoofin-test.a
-include $(TEST_PROD_DEPS)

.PHONY: test test-sanitize
test: $(TEST_TARGET) $(SQLITE_TEST_TARGET) $(CATALOG_BENCHMARK_TARGET)
	@$(TEST_TARGET)
	@$(SQLITE_TEST_TARGET)
	@$(CATALOG_BENCHMARK_TARGET)
	@sh $(RUNNER_TEST)
	@sh $(CA_BUNDLE_TEST)
	@sh $(ONION_REMOTE_LAUNCH_TEST)
	@python3 $(TELEMETRY_DECODER_TEST)

# Convenience entry point for the sanitizer run: rebuilds and runs the host
# test suite with AddressSanitizer + UndefinedBehaviorSanitizer.
test-sanitize:
	@$(MAKE) SANITIZE=1 test

.PHONY: refactor-check
refactor-check:
	@sh tools/refactor-check.sh

$(TEST_GROUP_TARGETS): output/test/test_%: tests/test_%.cpp $(TEST_PROD_LIB) $(SQLITE_HOST_OBJ) | output/test
	$(CXX) $(TEST_CXXFLAGS) $(INCLUDES) $(SDL_CFLAGS) -o $@ $< -Wl,--start-group $(TEST_PROD_LIB) $(SQLITE_HOST_OBJ) -Wl,--end-group $(LDFLAGS) $(CURL_LIBS) $(SDL_LIBS)
	@echo "  [LINK] $@"

$(TEST_GROUP_TARGETS): tests/test_support.hpp
output/test/test_api_session: tests/cases/test_session.inc tests/cases/test_api_core.inc tests/cases/test_api_events.inc
output/test/test_api_core: tests/cases/test_api_core.inc
output/test/test_api_events: tests/cases/test_api_events.inc
output/test/test_session: tests/cases/test_session.inc
output/test/test_ui_foundation: tests/cases/test_ui_foundation.inc
output/test/test_ui_models: tests/cases/test_ui_models.inc
output/test/test_cache_offline: tests/cases/test_cache_offline.inc
output/test/test_artwork_episode: tests/cases/test_artwork_episode.inc
output/test/test_downloads: tests/cases/test_downloads.inc
output/test/test_misc: tests/cases/test_misc_regressions.inc
output/test/test_playback: tests/cases/test_playback_ui.inc
output/test/test_telemetry: tests/cases/test_telemetry_core.inc
output/test/test_telemetry_format: tests/cases/test_telemetry_format.inc
output/test/test_telemetry_service: tests/cases/test_telemetry_service.inc
output/test/test_telemetry_schema: tests/cases/test_telemetry_schema.inc tests/cases/test_telemetry_schema_tail.inc
output/test/test_catalog: tests/cases/test_catalog_core.inc tests/cases/test_catalog_migration.inc tests/cases/test_catalog_parity_support.hpp tests/cases/test_catalog_parity_query.inc tests/cases/test_catalog_parity_hierarchy.inc tests/cases/test_catalog_parity_sync.inc
output/test/test_catalog_parity_query: tests/cases/test_catalog_migration_support.hpp tests/cases/test_catalog_parity_support.hpp tests/cases/test_catalog_parity_query.inc tests/cases/test_catalog_parity_sync.inc
output/test/test_catalog_parity_hierarchy: tests/cases/test_catalog_migration_support.hpp tests/cases/test_catalog_parity_support.hpp tests/cases/test_catalog_parity_hierarchy.inc
output/test/test_catalog_parity_sync: tests/cases/test_catalog_migration_support.hpp tests/cases/test_catalog_parity_support.hpp tests/cases/test_catalog_parity_sync.inc
output/test/test_update: tests/cases/test_update.inc tests/cases/test_update_installer.inc tests/cases/test_update_manager.inc src/update/UpdateInstaller.hpp src/update/UpdateManager.hpp src/net/HttpClient.hpp

output/test/objects/%.o: src/%.cpp | output/test
	@mkdir -p $(@D)
	$(CXX) $(TEST_CXXFLAGS) -MMD -MP $(INCLUDES) $(SDL_CFLAGS) $(CURL_CFLAGS) -c -o $@ $<
	@echo "  [CC]   $@"

$(TEST_PROD_LIB): $(TEST_PROD_OBJS) | output/test
	rm -f $@
	$(AR) rcs $@ $^
	@echo "  [AR]   $@"

$(TEST_TARGET): tests/test_runner.sh $(TEST_GROUP_TARGETS) | output/test
	cp tests/test_runner.sh $@
	chmod +x $@

$(SQLITE_TEST_TARGET): $(SQLITE_TEST_SRC) $(SQLITE_HOST_OBJ) | output/test
	$(CXX) $(TEST_CXXFLAGS) $(INCLUDES) -I$(SQLITE_DIR) -o $@ $^ $(LDFLAGS)
	@echo "  [LINK] $@"

$(CATALOG_BENCHMARK_TARGET): $(CATALOG_BENCHMARK_TEST_SRC) $(CATALOG_BENCHMARK_SRC) src/catalog/MediaItemSql.cpp $(SQLITE_HOST_OBJ) | output/test
	$(CXX) $(TEST_CXXFLAGS) $(INCLUDES) $(SDL_CFLAGS) -I$(SQLITE_DIR) -o $@ $^ $(LDFLAGS) -lpthread
	@echo "  [LINK] $@"

.PHONY: catalog-journal-benchmark-test catalog-journal-benchmark
catalog-journal-benchmark-test: $(CATALOG_BENCHMARK_TARGET)
	@$(CATALOG_BENCHMARK_TARGET)

catalog-journal-benchmark: $(CATALOG_BENCHMARK_RUNNER)
	@$(CATALOG_BENCHMARK_RUNNER) --help

$(CATALOG_BENCHMARK_RUNNER): $(CATALOG_BENCHMARK_MAIN) $(CATALOG_BENCHMARK_SRC) src/catalog/MediaItemSql.cpp $(SQLITE_HOST_OBJ) | output/build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SDL_CFLAGS) -I$(SQLITE_DIR) -o $@ $^ -lpthread
	@echo "  [LINK] $@"

output/test:
	@mkdir -p $@

# -------------------------------------------------------------------
# Miyoo build-time shared libraries (build inputs, not shipped)
# -------------------------------------------------------------------
MIYOO_LIB_DIR := vendor/miyoo/lib
MIYOO_LIBS    := $(MIYOO_LIB_DIR)/libEGL.so.1 \
                 $(MIYOO_LIB_DIR)/libGLESv2.so \
                 $(MIYOO_LIB_DIR)/libmi_ao.so \
                 $(MIYOO_LIB_DIR)/libmi_common.so \
                 $(MIYOO_LIB_DIR)/libmi_gfx.so \
                 $(MIYOO_LIB_DIR)/libmi_sys.so

.PHONY: import-miyoo-libs
import-miyoo-libs:
	@sh tools/import-miyoo-build-libs.sh

.PHONY: check-miyoo-libs
check-miyoo-libs:
	@sh tools/import-miyoo-build-libs.sh --verify

# -------------------------------------------------------------------
# OnionOS cross-compilation via Docker
# -------------------------------------------------------------------
DOCKER_TAG := miyoofin-toolchain
DOCKER_USER := $(shell id -u):$(shell id -g)
ARM_TARGET := output/build-arm/miyoofin

.PHONY: onionos
onionos: check-miyoo-libs $(DOCKER_TAG)
	@mkdir -p output/build-arm
	docker run --rm --user $(DOCKER_USER) -v $(PWD):/build $(DOCKER_TAG) \
	    make -f Makefile.cross PERF_TELEMETRY=$(PERF_TELEMETRY) RELEASE=$(RELEASE) all bridge reporter benchmark
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
CA_BUNDLE := cacert.pem
ARM_BRIDGE := output/build-arm/miyoofin-https-bridge
ARM_REPORTER := output/build-arm/miyoofin-playback-reporter

.PHONY: package check-ca-bundle update-ca-bundle
check-ca-bundle: $(CA_BUNDLE)
	@test -s $(CA_BUNDLE) || { echo "ERROR: $(CA_BUNDLE) is missing or empty. Run 'make update-ca-bundle'."; exit 1; }
	@grep -q -- 'BEGIN CERTIFICATE' $(CA_BUNDLE) || { echo "ERROR: $(CA_BUNDLE) is not a PEM CA bundle. Run 'make update-ca-bundle'."; exit 1; }

# Deliberately refresh the vendored Mozilla CA bundle from curl's official URL.
update-ca-bundle:
	@sh tools/update-ca-bundle.sh $(CA_BUNDLE)

package: onionos check-ca-bundle check-miyoo-libs
	@rm -rf $(PACKAGE_DIR)
	@mkdir -p $(PACKAGE_DIR)/lib
	@mkdir -p $(PACKAGE_DIR)/assets
	@cp $(ARM_TARGET) $(PACKAGE_DIR)/miyoofin
	@cp $(ARM_BRIDGE) $(PACKAGE_DIR)/miyoofin-https-bridge
	@cp $(CA_BUNDLE) $(PACKAGE_DIR)/cacert.pem
	@test -s $(PACKAGE_DIR)/cacert.pem || { echo "ERROR: failed to stage required CA bundle"; exit 1; }
	@cp distributions/onionos/launch.sh $(PACKAGE_DIR)/
	@cp distributions/onionos/playback_runner.sh $(PACKAGE_DIR)/
	@cp $(ARM_REPORTER) $(PACKAGE_DIR)/
	@cp distributions/onionos/config.json $(PACKAGE_DIR)/
	@cp assets/icon.png $(PACKAGE_DIR)/icon.png 2>/dev/null || true
	@cp assets/placeholder.png $(PACKAGE_DIR)/assets/placeholder.png 2>/dev/null || true
	@echo "  Bundling ARM shared libraries from toolchain..."
	@docker run --rm --user $(DOCKER_USER) -v $(PWD)/$(PACKAGE_DIR)/lib:/out miyoofin-toolchain \
	    bash -c '\
	    cp -aP /usr/arm-linux-gnueabihf/lib/libSDL2-2.0.so.0 /out/ && \
	    cp -aP /usr/arm-linux-gnueabihf/lib/libSDL2-2.0.so.0.18.2 /out/ && \
	    cp -aP /usr/arm-linux-gnueabihf/lib/libSDL2.so /out/ && \
	    cp -aP /usr/arm-linux-gnueabihf/lib/libstdc++.so.6 /out/ && \
	    cp -aP /usr/arm-linux-gnueabihf/lib/libstdc++.so.6.0.25 /out/ && \
	    cp -aP /usr/arm-linux-gnueabihf/lib/libgcc_s.so.1 /out/ && \
	    echo "  Libraries bundled successfully"'
	@echo "  Stripping packaged binaries..."
	@docker run --rm --user $(DOCKER_USER) -v $(PWD)/$(PACKAGE_DIR):/pkg miyoofin-toolchain \
	    bash -c '\
	    arm-linux-gnueabihf-strip --strip-unneeded /pkg/miyoofin && \
	    arm-linux-gnueabihf-strip --strip-unneeded /pkg/miyoofin-https-bridge && \
	    arm-linux-gnueabihf-strip --strip-unneeded /pkg/miyoofin-playback-reporter && \
	    arm-linux-gnueabihf-strip --strip-unneeded /pkg/lib/libSDL2-2.0.so.0.18.2 && \
	    echo "  Packaged binaries stripped successfully"'
	@echo "  Verifying package binary architecture..."
	@file $(PACKAGE_DIR)/miyoofin | grep -qi 'ARM' || \
	    { echo "ERROR: $(PACKAGE_DIR)/miyoofin is NOT ARM!"; exit 1; }
	@echo "  Package uses ARM binary: OK"
	@echo "  Package CA bundle: OK ($(PACKAGE_DIR)/cacert.pem)"
	@echo "  Package staged at: $(PACKAGE_DIR)/"
	@echo "  Bundle as: cd output/package && zip -r MiyooFin.zip MiyooFin/"

# -------------------------------------------------------------------
# HTTPS bridge (standalone helper — NOT linked into MiyooFin)
# -------------------------------------------------------------------
BRIDGE_SRC     := tools/https_bridge.cpp
BRIDGE_HOST    := output/build/miyoofin-https-bridge
BRIDGE_TEST    := output/test/test_bridge_parse
BRIDGE_TEST_SRC := tests/test_bridge_parse.cpp
BRIDGE_LOCAL_TEST := output/test/test_bridge_local
BRIDGE_LOCAL_TEST_SRC := tests/test_bridge_local.cpp

.PHONY: bridge
bridge: $(BRIDGE_HOST)

$(BRIDGE_HOST): $(BRIDGE_SRC) tools/https_bridge_parse.hpp tools/playback_route.hpp | output/build
	$(CXX) $(CXXFLAGS) -Itools -o $@ $< $(CURL_LIBS)
	@echo "  [LINK] $@"

.PHONY: bridge-test
bridge-test: $(BRIDGE_TEST) $(BRIDGE_LOCAL_TEST) bridge
	@$(BRIDGE_TEST)
	@$(BRIDGE_LOCAL_TEST)

$(BRIDGE_TEST): $(BRIDGE_TEST_SRC) tools/https_bridge_parse.hpp | output/test
	$(CXX) $(CXXFLAGS) -I. -o $@ $< $(CURL_LIBS)
	@echo "  [LINK] $@"

$(BRIDGE_LOCAL_TEST): $(BRIDGE_LOCAL_TEST_SRC) src/download/DownloadStore.cpp src/download/DownloadStore.hpp | output/test
	$(CXX) $(CXXFLAGS) -I. -o $@ $(BRIDGE_LOCAL_TEST_SRC) src/download/DownloadStore.cpp
	@echo "  [LINK] $@"

# -------------------------------------------------------------------
# wait_menu_release (tiny evdev helper — detects MENU button release)
# -------------------------------------------------------------------
WAIT_RELEASE_SRC  := tools/wait_menu_release.c
WAIT_RELEASE_HOST := output/build/wait_menu_release

$(WAIT_RELEASE_HOST): $(WAIT_RELEASE_SRC) | output/build
	$(CC) -Wall -Wextra -pedantic -O2 -o $@ $<
	@echo "  [LINK] $@"

.PHONY: wait-release
wait-release: $(WAIT_RELEASE_HOST)

# -------------------------------------------------------------------
# playback reporter (standalone Jellyfin playback reporting helper)
# -------------------------------------------------------------------
REPORTER_SRC     := tools/playback_reporter.cpp
REPORTER_HOST    := output/build/miyoofin-playback-reporter
REPORTER_TEST    := output/test/test_playback_reporter
REPORTER_TEST_SRC := tests/test_playback_reporter.cpp

.PHONY: reporter
reporter: $(REPORTER_HOST)

$(REPORTER_HOST): $(REPORTER_SRC) tools/playback_clock_parser.hpp tools/playback_route.hpp | output/build
	$(CXX) $(CXXFLAGS) -Itools -Iinclude -o $@ $< $(CURL_LIBS)
	@echo "  [LINK] $@"

.PHONY: reporter-test
reporter-test: $(REPORTER_TEST)
	@$(REPORTER_TEST)

$(REPORTER_TEST): $(REPORTER_TEST_SRC) tools/playback_clock_parser.hpp tools/playback_route.hpp | output/test
	$(CXX) $(CXXFLAGS) -Itools -Iinclude -o $@ $< $(CURL_LIBS)
	@echo "  [LINK] $@"

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
	@echo "  make test-sanitize — Run unit tests under ASan+UBSan (SANITIZE=1)"
	@echo "  make bridge  — Build HTTPS bridge helper (host)"
	@echo "  make bridge-test — Run bridge parsing tests"
	@echo "  make desktop-run — Run the host desktop development runtime"
	@echo "  make desktop-test — Check desktop runtime wiring"
	@echo "  make onionos    — Cross-compile for Miyoo via Docker"
	@echo "  make onionos RELEASE=1 — Cross-compile slim release for Miyoo"
	@echo "  make verify-arm — Verify ARM binary architecture"
	@echo "  make package    — Stage OnionOS package (uses ARMarch binary)"
	@echo "  make import-miyoo-libs — Import Miyoo build libraries from device"
	@echo "  make update-ca-bundle — Refresh cacert.pem from curl's Mozilla CA bundle"
	@echo "  make clean   — Remove output/"
	@echo "  make help    — This message"

-include Makefile.desktop
