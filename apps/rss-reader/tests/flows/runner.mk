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

.DEFAULT_GOAL := runner
.PHONY: runner
runner: $(RUNNER)

$(RUNNER): $(LVGL_OBJS) $(CP0_OBJS) $(filter-out %/headless_main.cpp.o,$(COMMON_OBJS)) $(APP_OBJS) $(RUNNER_OBJ)
	$(CXX) -o $@ $^ -Wl,--gc-sections $(FT_LIBS) $(PNG_LIBS) -lpthread -lm

-include $(RUNNER_OBJ:.o=.d)
