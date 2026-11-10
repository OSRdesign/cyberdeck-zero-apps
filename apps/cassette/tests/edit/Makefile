# SPDX-License-Identifier: MIT
#
# Host build of the Cassette edit tests (trim, split, peaks; no LVGL, no ALSA):
#   make -C tests/edit BUILD=<fresh dir> -j3 test         (SAN=1 adds ASan and UBSan)
# Run it on a WSL native-filesystem copy (tests/edit/run.sh does that).
APP   ?= ../..
BUILD ?= build
CODEC := codec_libs stream_scan ogg_opus decoder flac_decoder opus_decoder decoded_source encoder byte_sink flac_encoder
CORE  := $(APP)/src/core/fileops.cpp $(APP)/src/core/wav.cpp $(wildcard $(APP)/src/core/edit/*.cpp) \
         $(patsubst %,$(APP)/src/core/codec/%.cpp,$(CODEC))
TESTS := $(wildcard $(APP)/tests/edit/*.cpp)
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(CORE) $(TESTS))
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
ifdef SAN
CXXFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
LDFLAGS  += -fsanitize=address,undefined
endif
CXXFLAGS += -I$(APP)/src/core -I$(APP)/src/core/edit -I$(APP)/tests/edit -I$(APP)/tests
LIBS := -ldl -lpthread

$(BUILD)/test-edit: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

test: $(BUILD)/test-edit
	$(BUILD)/test-edit

-include $(OBJS:.o=.d)
.PHONY: test
