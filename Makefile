CXX ?= c++
CXXFLAGS ?= -std=c++20 -O3 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=off
LDLIBS += -pthread

LIB_SOURCES := \
	src/aggregation/aggregate.cpp \
	src/codec.cpp \
	src/failure_injection.cpp \
	src/hash.cpp \
	src/identity.cpp \
	src/metrics.cpp \
	src/models/gbm.cpp \
	src/parse.cpp \
	src/persistence_codec.cpp \
	src/rng/philox_rng.cpp \
	src/run_store.cpp \
	src/runtime/coordinator.cpp \
	src/runtime/engine.cpp \
	src/run_spec.cpp
HEADERS := $(wildcard include/mc/*.hpp)
INTERNAL_HEADERS := $(wildcard src/*.hpp src/*/*.hpp)
TOOL_HEADERS := $(wildcard tools/*.hpp)
SOURCE_REVISION := $(shell sh tools/source_fingerprint.sh $(LIB_SOURCES) $(HEADERS) $(INTERNAL_HEADERS))
BUILD_FLAGS_ID := $(shell MC_BUILD_FLAGS='CXXFLAGS=$(CXXFLAGS);CPPFLAGS=$(CPPFLAGS);LDFLAGS=$(LDFLAGS);LDLIBS=$(LDLIBS)' sh tools/source_fingerprint.sh --flags)
MC_CPPFLAGS := $(CPPFLAGS) -Iinclude \
	-DMC_FP_CONTRACT_MODE=\"off\" \
	-DMC_SOURCE_REVISION=\"$(SOURCE_REVISION)\" \
	-DMC_BUILD_FLAGS_ID=\"$(BUILD_FLAGS_ID)\" \
	-DMC_BUILD_CONFIG=\"make\" \
	-DMC_CPU_POLICY=\"compiler-default\"

BUILD_DIR := build
RUNNER := $(BUILD_DIR)/run_simulation
TESTS := $(BUILD_DIR)/mc_tests
BENCHMARK := $(BUILD_DIR)/benchmark_scaling
REPLAY_FAILURE := $(BUILD_DIR)/replay_failure
CRASH_MATRIX := $(BUILD_DIR)/run_crash_matrix
BUILD_CONFIG_STAMP := $(BUILD_DIR)/.build-config
BUILD_CONFIG_ID := $(SOURCE_REVISION)-$(BUILD_FLAGS_ID)

.PHONY: all test benchmark r3-tools clean FORCE

all: $(RUNNER)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

FORCE:

$(BUILD_CONFIG_STAMP): FORCE | $(BUILD_DIR)
	@current_id=''; \
	if test -f $@; then IFS= read -r current_id < $@; fi; \
	if test "$$current_id" != "$(BUILD_CONFIG_ID)"; then \
		printf '%s\n' '$(BUILD_CONFIG_ID)' > $@.tmp; \
		mv $@.tmp $@; \
	fi

$(RUNNER): $(LIB_SOURCES) tools/run_simulation.cpp $(HEADERS) $(INTERNAL_HEADERS) $(BUILD_CONFIG_STAMP) | $(BUILD_DIR)
	$(CXX) $(MC_CPPFLAGS) $(CXXFLAGS) $(filter %.cpp,$^) $(LDFLAGS) $(LDLIBS) -o $@

$(TESTS): $(LIB_SOURCES) tests/test_main.cpp $(HEADERS) $(INTERNAL_HEADERS) $(TOOL_HEADERS) $(BUILD_CONFIG_STAMP) | $(BUILD_DIR)
	$(CXX) $(MC_CPPFLAGS) $(CXXFLAGS) $(filter %.cpp,$^) $(LDFLAGS) $(LDLIBS) -o $@

test: $(TESTS)
	$(TESTS)

$(BENCHMARK): $(LIB_SOURCES) tools/benchmark_scaling.cpp $(HEADERS) $(INTERNAL_HEADERS) $(BUILD_CONFIG_STAMP) | $(BUILD_DIR)
	$(CXX) $(MC_CPPFLAGS) $(CXXFLAGS) $(filter %.cpp,$^) $(LDFLAGS) $(LDLIBS) -o $@

benchmark: $(BENCHMARK)

$(REPLAY_FAILURE): $(LIB_SOURCES) tools/replay_failure.cpp $(HEADERS) $(INTERNAL_HEADERS) $(TOOL_HEADERS) $(BUILD_CONFIG_STAMP) | $(BUILD_DIR)
	$(CXX) $(MC_CPPFLAGS) $(CXXFLAGS) $(filter %.cpp,$^) $(LDFLAGS) $(LDLIBS) -o $@

$(CRASH_MATRIX): $(LIB_SOURCES) tools/run_crash_matrix.cpp $(HEADERS) $(INTERNAL_HEADERS) $(TOOL_HEADERS) $(BUILD_CONFIG_STAMP) | $(BUILD_DIR)
	$(CXX) $(MC_CPPFLAGS) $(CXXFLAGS) $(filter %.cpp,$^) $(LDFLAGS) $(LDLIBS) -o $@

r3-tools: $(REPLAY_FAILURE) $(CRASH_MATRIX)

clean:
	rm -f $(RUNNER) $(TESTS) $(BENCHMARK) $(REPLAY_FAILURE) $(CRASH_MATRIX) $(BUILD_CONFIG_STAMP) $(BUILD_CONFIG_STAMP).tmp
