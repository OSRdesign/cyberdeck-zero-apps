# SPDX-License-Identifier: MIT
#
# RSS Reader's scripted runner (rss-reader-script, script_runner.cpp: the fake network, the fixed clock, the seeding) on the
# shared headless harness: the same objects as apps/_common/headless/Makefile (LVGL, cp0_lvgl, _common, the harness's
# platform and layout checks, the app), with the script player instead of the scene player (headless_main.cpp). Usually
# driven by ../../build.sh host, after build_app.sh host built the objects:
#
#   make -f apps/rss-reader/tests/flows/runner.mk APP=apps/rss-reader LAUNCHER=<launcher build tree> BUILD=<dir> GEN=<dir>
#
# Product: $(BUILD)/rss-reader-script.

include $(dir $(lastword $(MAKEFILE_LIST)))../../../_common/headless/Makefile

RUNNER     := $(BUILD)/$(APP_NAME)-script
RUNNER_OBJ := $(call appobj,$(APP_DIR)/tests/flows/script_runner.cpp)

# rss-reader-imgtest: the image decoding (src/ui/image_decode.cpp, image_codecs.c) on the same LVGL objects, with the core
IMGTEST     := $(BUILD)/$(APP_NAME)-imgtest
IMGTEST_OBJ := $(call appobj,$(APP_DIR)/tests/unit/img_tests.cpp)
IMG_OBJS    := $(call appobj,$(APP_DIR)/src/ui/image_decode.cpp $(APP_DIR)/src/ui/image_codecs.c $(APP_CORE_SRCS))
$(IMGTEST_OBJ): INC += -I$(APP_DIR)/tests/unit -DTEST_DATA_DIR='"$(APP_DIR)/tests/data"'

.DEFAULT_GOAL := runner
.PHONY: runner
runner: $(RUNNER) $(IMGTEST)

$(RUNNER): $(LVGL_OBJS) $(CP0_OBJS) $(filter-out %/headless_main.cpp.o,$(COMMON_OBJS)) $(APP_OBJS) $(RUNNER_OBJ)
	$(CXX) -o $@ $^ -Wl,--gc-sections $(FT_LIBS) $(PNG_LIBS) -lpthread -lm

$(IMGTEST): $(LVGL_OBJS) $(IMG_OBJS) $(IMGTEST_OBJ)
	$(CXX) -o $@ $^ -Wl,--gc-sections $(FT_LIBS) $(PNG_LIBS) -lpthread -lm

-include $(RUNNER_OBJ:.o=.d) $(IMGTEST_OBJ:.o=.d)
