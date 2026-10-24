# SPDX-License-Identifier: MIT
#
# Markdown Notes' scripted runner (markdown-notes-script, script_runner.cpp: the vault seeds, the changes on disk, the
# file checks, the fixed clock) on the shared headless harness: the same objects as apps/_common/headless/Makefile
# (LVGL, cp0_lvgl, _common, the harness's platform and layout checks, the app), with the script player instead of the
# scene player (headless_main.cpp). RSS Reader's runner.mk. Usually driven by ../../build.sh host, after build_app.sh
# host built the objects:
#
#   make -f apps/markdown-notes/tests/flows/runner.mk APP=apps/markdown-notes LAUNCHER=<launcher build tree> BUILD=<dir> GEN=<dir>
#
# Product: $(BUILD)/markdown-notes-script.

include $(dir $(lastword $(MAKEFILE_LIST)))../../../_common/headless/Makefile

RUNNER     := $(BUILD)/$(APP_NAME)-script
RUNNER_OBJ := $(call appobj,$(APP_DIR)/tests/flows/script_runner.cpp)

.DEFAULT_GOAL := runner
.PHONY: runner
runner: $(RUNNER)

$(RUNNER): $(LVGL_OBJS) $(CP0_OBJS) $(filter-out %/headless_main.cpp.o,$(COMMON_OBJS)) $(APP_OBJS) $(RUNNER_OBJ)
	$(CXX) -o $@ $^ -Wl,--gc-sections $(FT_LIBS) $(PNG_LIBS) -lpthread -lm

-include $(RUNNER_OBJ:.o=.d)
