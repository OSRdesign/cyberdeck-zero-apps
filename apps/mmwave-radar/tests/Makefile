# SPDX-License-Identifier: MIT
#
# Host build of the mmWave Radar core tests (no LVGL needed): make -C tests BUILD=<fresh dir> -j3 test
# Run it on a WSL native-filesystem copy (tests/run.sh does that).
APP   ?= ..
BUILD ?= build
CORE  := $(wildcard $(APP)/src/core/*.cpp)
TESTS := $(wildcard $(APP)/tests/*.cpp)
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(CORE) $(TESTS))
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
CXXFLAGS += -I$(APP)/src/core -I$(APP)/tests -DMMW_DATA_DIR='"$(abspath $(APP))/tests/data/"'
LIBS := -lutil

$(BUILD)/test-mmwave-radar: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

test: $(BUILD)/test-mmwave-radar
	$(BUILD)/test-mmwave-radar

-include $(OBJS:.o=.d)
.PHONY: test
