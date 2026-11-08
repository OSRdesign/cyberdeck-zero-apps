# SPDX-License-Identifier: MIT
#
# Host build of the Cassette core tests (no LVGL, no ALSA): make -C tests BUILD=<fresh dir> -j3 test
# Run it on a WSL native-filesystem copy (tests/run.sh does that).
APP   ?= ..
BUILD ?= build
CORE  := $(wildcard $(APP)/src/core/*.cpp)
TESTS := $(wildcard $(APP)/tests/*.cpp)
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(CORE) $(TESTS))
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
CXXFLAGS += -I$(APP)/src/core -I$(APP)/tests
LIBS := -lpthread

$(BUILD)/test-cassette: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

test: $(BUILD)/test-cassette
	$(BUILD)/test-cassette

-include $(OBJS:.o=.d)
.PHONY: test
