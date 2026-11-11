# SPDX-License-Identifier: MIT
#
# Host build of the Cassette takes tests (no LVGL, no ALSA): make -C tests/takes BUILD=<fresh dir> -j3 test
# SAN=1 builds with AddressSanitizer and UndefinedBehaviorSanitizer. Run it on a WSL native-filesystem copy
# (tests/takes/run.sh does that).
APP   ?= ../..
BUILD ?= build
CORE  := $(APP)/src/core/fileops.cpp $(APP)/src/core/wav.cpp $(APP)/src/core/journal.cpp \
         $(APP)/src/core/codec/stream_scan.cpp $(APP)/src/core/codec/byte_sink.cpp $(wildcard $(APP)/src/core/takes/*.cpp)
TESTS := $(wildcard $(APP)/tests/takes/*.cpp)
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(CORE) $(TESTS))
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
CXXFLAGS += -I$(APP)/src/core -I$(APP)/src/core/takes -I$(APP)/tests/takes
LIBS := -lpthread
ifeq ($(SAN),1)
CXXFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
LDFLAGS  += -fsanitize=address,undefined
endif

$(BUILD)/test-takes: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

test: $(BUILD)/test-takes
	$(BUILD)/test-takes

-include $(OBJS:.o=.d)
.PHONY: test
