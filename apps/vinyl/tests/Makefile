# SPDX-License-Identifier: MIT
#
# Host build of the Vinyl unit tests without LVGL (core/ and tests/*.cpp; the same binary the headless harness builds as
# test-vinyl), with -Werror. Run it through tests/run.sh (a WSL native-filesystem copy and the real-library fixtures).
#   make -C tests APP=<app copy> BUILD=<fresh dir> -j2 test     SAN=address,undefined or SAN=thread adds sanitizers
APP   ?= ..
BUILD ?= build
SRCS  := $(shell find $(APP)/src/core -name '*.cpp') $(wildcard $(APP)/tests/*.cpp)
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS))
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
ifneq ($(SAN),)
CXXFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
LDFLAGS  += -fsanitize=$(SAN)
endif
INCS := -I$(APP)/src/core -I$(APP)/src -I$(APP)/tests

all: $(BUILD)/test-vinyl

$(BUILD)/test-vinyl: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ -ldl -lpthread

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCS) -MMD -c -o $@ $<

test: all
	$(BUILD)/test-vinyl

-include $(OBJS:.o=.d)
.PHONY: all test
