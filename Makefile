.DEFAULT_GOAL := build
.NOTPARALLEL:
SHELL := bash
.SHELLFLAGS := -eu -o pipefail -c
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
TARGET ?= x86_64-linux-gnu
CONFIG ?= Debug
JOBS ?= 1
LIFECYCLE_SCRIPTS_DIR ?= $(HOME)/.codex/skills/pkt-systems-cmake-lifecycle/scripts
BUILD_DIR := $(ROOT)/build/$(TARGET)/$(CONFIG)

.PHONY: help prepare configure build test benchmark memcheck racecheck install format format-check clean
help:
	@printf '%s\n' 'make [build|test|benchmark|memcheck|racecheck|install|format-check|clean]' 'TARGET=x86_64-linux-gnu|x86_64-linux-musl CONFIG=Debug|Release JOBS=1' 'make prepare: prepare the pinned toolchain and pristine Lua 5.5.1 test reference' 'make benchmark CONFIG=Release: identical-code upstream/Luthrum CPU and wall-time comparisons' 'LIFECYCLE_SCRIPTS_DIR selects the activated lifecycle skill scripts.'
prepare:
	"$(LIFECYCLE_SCRIPTS_DIR)/cpkt-toolchains.sh" ensure "$(TARGET)"
	bash "$(ROOT)/tools/prepare_lua_reference.sh"
configure:
	cmake -S "$(ROOT)" -B "$(BUILD_DIR)" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(ROOT)/cmake/toolchains/bootlin.cmake" -DCPKT_LIFECYCLE_SCRIPTS_DIR="$(LIFECYCLE_SCRIPTS_DIR)" -DLUTHRUM_TARGET="$(TARGET)" -DCMAKE_BUILD_TYPE="$(CONFIG)" -DBUILD_TESTING=ON
build: configure
	cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"
test: build
	ctest --test-dir "$(BUILD_DIR)" --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
benchmark: configure
	cmake --build "$(BUILD_DIR)" --target benchmark_lua --parallel "$(JOBS)"
memcheck: build
	ctest --test-dir "$(BUILD_DIR)" -T MemCheck -L memcheck --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
racecheck: build
	ctest --test-dir "$(BUILD_DIR)" -T MemCheck -L parallel --overwrite 'MemoryCheckCommandOptions=--tool=helgrind --error-exitcode=1 --history-level=approx --fair-sched=yes' --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
install: build
	cmake --install "$(BUILD_DIR)" --prefix "$(ROOT)/build/sdk/$(TARGET)/$(CONFIG)"
format:
	cd "$(ROOT)" && rg --files src include tests -g '*.[ch]' -0 | xargs -0 clang-format -i
format-check:
	cd "$(ROOT)" && rg --files src include tests -g '*.[ch]' -0 | xargs -0 clang-format --dry-run --Werror
clean:
	rm -rf -- "$(ROOT)/build" "$(ROOT)/dist" "$(ROOT)/.cache"
