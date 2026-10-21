# SPDX-License-Identifier: MIT
#
# Host build of one cdk app for the PC (WSL): its headless harness binary and the unit tests of the app and of _common.
# Usually driven by ../build_app.sh host <app dir>. Needs gcc/g++, make, pkg-config, FreeType and libpng headers, and a
# launcher build tree whose SDK/ was downloaded by one scons build (LVGL 9.5 is compiled from it, as the launcher's
# render harness does: projects/APPLaunch/tests/render/Makefile).
#
#   make -C apps/_common/headless APP=<app dir> LAUNCHER=<launcher build tree> [BUILD=<dir>] [GEN=<dir>]
#
# Products: $(BUILD)/<app>-headless, $(BUILD)/test-<app>, $(BUILD)/test-common.

COMMON   := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
HEADLESS := $(COMMON)/headless
APP      ?= $(error APP=<app dir> is needed)
APP_DIR  := $(abspath $(APP))
APP_NAME := $(notdir $(APP_DIR))
LAUNCHER ?= $(HOME)/zero7/launcher
CP0      := $(LAUNCHER)/ext_components/cp0_lvgl
LVGL_DIR := $(LAUNCHER)/SDK/github_source/lvgl/lvgl_9_5
LVGL     := $(LVGL_DIR)/lvgl
LVCONF   := $(LAUNCHER)/projects/APPLaunch/tests/render/config/lvgl_config.h
FT_OVR   := $(LAUNCHER)/projects/APPLaunch/main/lvgl
BUILD    ?= $(HOME)/cdk-build/$(APP_NAME)/host
LVBUILD  ?= $(HOME)/cdk-build/_lvgl-host
GEN      ?= $(BUILD)/gen
OBJ      := $(BUILD)/obj

CC  ?= gcc
CXX ?= g++

ifeq ($(wildcard $(LVGL)/lvgl.h),)
$(error LVGL 9.5 not found at $(LVGL): run one scons build of the launcher so its SDK downloads it)
endif

FT_CFLAGS  := $(shell pkg-config --cflags freetype2)
FT_LIBS    := $(shell pkg-config --libs freetype2)
PNG_CFLAGS := $(shell pkg-config --cflags libpng)
PNG_LIBS   := $(shell pkg-config --libs libpng)

OPT     := -O2 -g0 -ffunction-sections -fdata-sections -fno-strict-aliasing -U_FORTIFY_SOURCE
LV_DEFS := '-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE="$(LVCONF)"'
LV_INC  := -I$(LVGL_DIR) -I$(LVGL) $(FT_CFLAGS)
INC     := -I$(GEN) -I$(COMMON) -I$(HEADLESS) -I$(APP_DIR)/src -I$(APP_DIR)/src/ui -I$(APP_DIR)/src/core \
           -I$(CP0)/include -I$(CP0)/src -I$(LAUNCHER)/SDK/github_source/eventpp/include \
           -I$(LAUNCHER)/SDK/components/utilities/include $(LV_INC) $(PNG_CFLAGS)
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

# LVGL: everything under src/, with the launcher's FreeType files instead of LVGL's (as the launcher builds it).
LV_OVERRIDE := lv_freetype.c lv_freetype_glyph.c lv_freetype_image.c
LVGL_SRCS := $(filter-out $(addprefix $(LVGL)/src/libs/freetype/,$(LV_OVERRIDE)),\
               $(shell find $(LVGL)/src -name '*.c' -o -name '*.cpp'))
LVGL_PRIV := -I$(FT_OVR) -I$(LVGL)/src/libs/freetype

# cp0_lvgl: the widgets, the layout service, the keyboard queue and bridge, the status strip and its renderer, the app side
# of the multitasking lifecycle (real code).
CP0_SRCS := \
    $(CP0)/src/cp0_app_contract.c \
    $(CP0)/src/cp0_app_lifecycle.c \
    $(CP0)/src/cp0_app_lifecycle_lvgl.c \
    $(CP0)/src/cp0_ui_metrics.c \
    $(CP0)/src/cp0_ui_metrics_lvgl.c \
    $(CP0)/src/cp0_ui_overlay.c \
    $(CP0)/src/cp0_ui_roller.c \
    $(CP0)/src/cp0_ui_roller_widget.cpp \
    $(CP0)/src/cp0_ui_list.c \
    $(CP0)/src/cp0_ui_list_widget.cpp \
    $(CP0)/src/cp0_ui_page.cpp \
    $(CP0)/src/cp0_ui_osk.c \
    $(CP0)/src/cp0_ui_osk_widget.cpp \
    $(CP0)/src/cp0_ui_text_field.cpp \
    $(CP0)/src/cp0_ui_info.c \
    $(CP0)/src/cp0_ui_info_widget.cpp \
    $(CP0)/src/cp0_ui_dialog.c \
    $(CP0)/src/cp0_ui_dialog_widget.cpp \
    $(CP0)/src/cp0_ui_status_strip.c \
    $(CP0)/src/cp0_ui_status_strip_widget.cpp \
    $(CP0)/src/cp0_keyboard_inject_key.c \
    $(CP0)/src/cp0_keyboard_queue.c \
    $(CP0)/src/cp0_keyboard_text.c \
    $(CP0)/src/cp0_esc_state.c \
    $(CP0)/src/cp0/cp0_keyboard_lvgl_input.c \
    $(CP0)/src/cp0/cp0_keyboard_presence.c \
    $(CP0)/src/cp0/cp0_statusbar.c \
    $(CP0)/src/cp0_font_service.cpp \
    $(CP0)/src/commount.cpp \
    $(CP0)/src/cp0_network_api_contract.cpp

# _common without the device runner (platform.cpp needs cp0_lvgl's display backend and keyboard thread).
COMMON_SRCS := $(filter-out $(COMMON)/platform.cpp,$(wildcard $(COMMON)/*.cpp))
HEADLESS_SRCS := $(wildcard $(HEADLESS)/*.c $(HEADLESS)/*.cpp)
# The app without its device main.
APP_SRCS := $(filter-out $(APP_DIR)/src/main.cpp,$(shell find $(APP_DIR)/src -name '*.cpp' -o -name '*.c'))
APP_CORE_SRCS := $(shell find $(APP_DIR)/src/core -name '*.cpp' 2>/dev/null)
APP_TEST_SRCS := $(wildcard $(APP_DIR)/tests/*.cpp)
COMMON_TEST_SRCS := $(wildcard $(COMMON)/tests/*.cpp)

lvobj     = $(patsubst $(LVGL)/%,$(LVBUILD)/lvgl/%.o,$(1))
cp0obj    = $(patsubst $(CP0)/%,$(LVBUILD)/cp0/%.o,$(1))
commonobj = $(patsubst $(COMMON)/%,$(OBJ)/common/%.o,$(1))
appobj    = $(patsubst $(APP_DIR)/%,$(OBJ)/app/%.o,$(1))

LVGL_OBJS   := $(call lvobj,$(LVGL_SRCS)) $(addprefix $(LVBUILD)/ftovr/,$(addsuffix .o,$(LV_OVERRIDE)))
CP0_OBJS    := $(call cp0obj,$(CP0_SRCS))
COMMON_OBJS := $(call commonobj,$(COMMON_SRCS) $(HEADLESS_SRCS))
APP_OBJS    := $(call appobj,$(APP_SRCS))

BIN        := $(BUILD)/$(APP_NAME)-headless
TEST_APP   := $(BUILD)/test-$(APP_NAME)
TEST_COMMON := $(BUILD)/test-common

.PHONY: all clean
all: $(BIN) $(if $(APP_TEST_SRCS),$(TEST_APP)) $(if $(COMMON_TEST_SRCS),$(TEST_COMMON))

$(BIN): $(LVGL_OBJS) $(CP0_OBJS) $(COMMON_OBJS) $(APP_OBJS)
	$(CXX) -o $@ $^ -Wl,--gc-sections $(FT_LIBS) $(PNG_LIBS) -lpthread -lm

# Unit tests: the app's core/ and its tests (no LVGL); _common's pure pieces with LVGL linked for lv_lock / lv_async.
$(TEST_APP): $(call appobj,$(APP_CORE_SRCS) $(APP_TEST_SRCS))
	$(CXX) -o $@ $^ -lpthread
$(TEST_COMMON): $(call commonobj,$(COMMON_TEST_SRCS) $(COMMON)/async_run.cpp $(COMMON)/osk_policy.cpp $(COMMON)/app.cpp) \
                $(LVGL_OBJS) $(CP0_OBJS) $(call commonobj,$(HEADLESS)/headless_keyboard.c $(HEADLESS)/headless_platform.cpp)
	$(CXX) -o $@ $^ -Wl,--gc-sections $(FT_LIBS) $(PNG_LIBS) -lpthread -lm

$(LVBUILD)/lvgl/%.c.o: $(LVGL)/%.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 $(OPT) -w $(LV_DEFS) $(LVGL_PRIV) $(LV_INC) -MMD -MP -c $< -o $@
$(LVBUILD)/lvgl/%.cpp.o: $(LVGL)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -std=gnu++17 $(OPT) -w $(LV_DEFS) $(LVGL_PRIV) $(LV_INC) -MMD -MP -c $< -o $@
$(LVBUILD)/ftovr/%.o: $(FT_OVR)/%
	@mkdir -p $(@D)
	$(CC) -std=gnu11 $(OPT) -w $(LV_DEFS) $(LVGL_PRIV) $(LV_INC) -MMD -MP -c $< -o $@
$(LVBUILD)/cp0/%.c.o: $(CP0)/%.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 $(OPT) -w $(LV_DEFS) $(INC) -MMD -MP -c $< -o $@
$(LVBUILD)/cp0/%.cpp.o: $(CP0)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -std=gnu++17 $(OPT) -w $(LV_DEFS) $(INC) -MMD -MP -c $< -o $@
$(OBJ)/common/%.c.o: $(COMMON)/%.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 $(OPT) $(WARN) $(LV_DEFS) $(INC) -MMD -MP -c $< -o $@
$(OBJ)/common/%.cpp.o: $(COMMON)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -std=gnu++17 $(OPT) $(WARN) $(LV_DEFS) $(INC) -MMD -MP -c $< -o $@
$(OBJ)/app/%.c.o: $(APP_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 $(OPT) $(WARN) $(LV_DEFS) $(INC) -MMD -MP -c $< -o $@
$(OBJ)/app/%.cpp.o: $(APP_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -std=gnu++17 $(OPT) $(WARN) $(LV_DEFS) $(INC) -DCDK_APP_DIR='"$(APP_DIR)"' -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(LVGL_OBJS:.o=.d) $(CP0_OBJS:.o=.d) $(COMMON_OBJS:.o=.d) $(APP_OBJS:.o=.d)
# the unit tests too: a header of core/ changed recompiles them (else a test links against a stale layout)
-include $(patsubst %.o,%.d,$(call appobj,$(APP_TEST_SRCS)) $(call commonobj,$(COMMON_TEST_SRCS)))
