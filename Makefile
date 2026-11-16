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
RACE_BUILD_DIR := $(ROOT)/build/$(TARGET)/ThreadSanitizer
RACE_FALLBACK_DIR := $(ROOT)/build/$(TARGET)/Debug

.PHONY: help prepare configure build unit test benchmark benchmark-parallel benchmark-scheduler memcheck racecheck install format format-check clean
help:
	@printf '%s\n' 'make [build|unit|test|benchmark|benchmark-parallel|benchmark-scheduler|memcheck|racecheck|install|format-check|clean]' 'TARGET=x86_64-linux-gnu|x86_64-linux-musl CONFIG=Debug|Release JOBS=1' 'make unit: short, non-exhaustive implementation checks and affected product builds' 'make test, benchmark, memcheck, racecheck: final stabilization after runtime integration' 'make prepare: prepare the pinned toolchain, pristine Lua 5.5.1 reference and official tests' 'make benchmark CONFIG=Release: identical-code upstream/Luthr CPU and wall-time comparisons' 'make benchmark-parallel CONFIG=Release: shared-VM host-pool scaling, latency and memory' 'make benchmark-scheduler CONFIG=Release: public task scheduling and native compensation' 'LIFECYCLE_SCRIPTS_DIR selects the activated lifecycle skill scripts.'
prepare:
	"$(LIFECYCLE_SCRIPTS_DIR)/cpkt-toolchains.sh" ensure "$(TARGET)"
	bash "$(ROOT)/tools/prepare_lua_reference.sh"
configure:
	cmake -S "$(ROOT)" -B "$(BUILD_DIR)" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(ROOT)/cmake/toolchains/bootlin.cmake" -DCPKT_LIFECYCLE_SCRIPTS_DIR="$(LIFECYCLE_SCRIPTS_DIR)" -DLUTHR_TARGET="$(TARGET)" -DCMAKE_BUILD_TYPE="$(CONFIG)" -DBUILD_TESTING=ON -DLUTHR_THREAD_SANITIZER=OFF
build: configure
	cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"
unit: configure
	cmake --build "$(BUILD_DIR)" --target unit_build --parallel "$(JOBS)"
	ctest --test-dir "$(BUILD_DIR)" -L unit --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
test: build
	ctest --test-dir "$(BUILD_DIR)" --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
benchmark: configure
	cmake --build "$(BUILD_DIR)" --target benchmark_lua --parallel "$(JOBS)"
benchmark-parallel: configure
	cmake --build "$(BUILD_DIR)" --target benchmark_parallel --parallel "$(JOBS)"
benchmark-scheduler: configure
	cmake --build "$(BUILD_DIR)" --target benchmark_scheduler --parallel "$(JOBS)"
memcheck: build
	ctest --test-dir "$(BUILD_DIR)" -T MemCheck -L memcheck --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
racecheck:
	cmake -S "$(ROOT)" -B "$(RACE_BUILD_DIR)" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(ROOT)/cmake/toolchains/bootlin.cmake" -DCPKT_LIFECYCLE_SCRIPTS_DIR="$(LIFECYCLE_SCRIPTS_DIR)" -DLUTHR_TARGET="$(TARGET)" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DLUTHR_THREAD_SANITIZER=ON
	cmake --build "$(RACE_BUILD_DIR)" --parallel "$(JOBS)"
	$(MAKE) configure CONFIG=Debug
	cmake --build "$(RACE_FALLBACK_DIR)" --target cross_api_test --parallel "$(JOBS)"
# The pinned libtsan cannot recover libc jump buffers (also fails without Lua).
# Keep this panic/longjmp case under Helgrind; do not suppress its race checks.
	ctest --test-dir "$(RACE_FALLBACK_DIR)" -T MemCheck -R '^cross_api_foreign_error$$' --overwrite 'MemoryCheckCommandOptions=--tool=helgrind --error-exitcode=1 --history-level=approx --fair-sched=yes' --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
	ctest --test-dir "$(RACE_BUILD_DIR)" -L parallel -E '^cross_api_foreign_error$$' --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
install: build
	cmake --install "$(BUILD_DIR)" --prefix "$(ROOT)/build/sdk/$(TARGET)/$(CONFIG)"
# Runtime sources retain Lua's formatting; format the SDK and tests here.
format:
	cd "$(ROOT)" && rg --files src include tests -g '*.[ch]' -g '!src/runtime/**' -0 | xargs -0 clang-format -i
format-check:
	cd "$(ROOT)" && rg --files src include tests -g '*.[ch]' -g '!src/runtime/**' -0 | xargs -0 clang-format --dry-run --Werror
clean:
	rm -rf -- "$(ROOT)/build" "$(ROOT)/dist" "$(ROOT)/.cache"
