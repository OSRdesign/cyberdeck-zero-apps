# Toolkit switches of Deck Atlas (Kconfig lines; apps/_common/build_app.sh cross appends them to this app's scratch
# configuration and apps/_common/headless/Makefile includes them): the launcher toolkit's map engine, off by default
# for every other app (cp0_lvgl Kconfig CONFIG_CP0_LVGL_MAP; SQLite and libcurl are loaded at run time).
CONFIG_CP0_LVGL_MAP=y
