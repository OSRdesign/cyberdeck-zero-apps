# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD, OSRdesign
# SPDX-License-Identifier: MIT
#
# The component of a cdk app's scratch project (written by apps/_common/build_app.sh as projects/<Name>/main/SConstruct
# in the launcher build tree):
#   main/app/        the app's src/ (main.cpp, ui/, core/)
#   main/common/     apps/_common/*.{hpp,cpp} + stub/ + cdk_build_info.h (no headless/, no tests/)
#   main/lvgl_ovr/   the launcher's LVGL FreeType files (projects/APPLaunch/main/lvgl), so text renders as in the launcher
# Links cp0_lvgl (display backend, keyboard thread, widgets) and LVGL statically. -O2 for LVGL, cp0_lvgl and the app:
# the SDK compiles without optimisation by default and LVGL drawing needs it on a Pi Zero 2W (as Mesh Hop does).

import os

Import('env')
with open(env['PROJECT_TOOL_S']) as f:
    exec(f.read())

SRCS = append_srcs_dir(ADir('app')) + append_srcs_dir(ADir('common'))
INCLUDE = [ADir('app'), ADir('app/ui'), ADir('app/core'), ADir('common')]
PRIVATE_INCLUDE = []
REQUIREMENTS = ['cp0_lvgl', 'lvgl_component', 'eventpp', 'pthread']
STATIC_LIB = []
DYNAMIC_LIB = []
DEFINITIONS = []
DEFINITIONS_PRIVATE = ['-O2']
LDFLAGS = ['-Wl,--as-needed']   # no NEEDED entry for a library nothing in the app calls
LINK_SEARCH_PATH = []
STATIC_FILES = []

toolchain_sysroot = os.environ.get('CONFIG_TOOLCHAIN_SYSROOT', '') or '/'
lvgl_component = list(filter(lambda x: x['target'] == 'lvgl_component', env['COMPONENTS']))[0]

if 'CONFIG_V9_5_LV_USE_FREETYPE' in os.environ:
    lvgl_component['REQUIREMENTS'] += ['freetype']
    lvgl_component['INCLUDE'] += [
        os.path.join(toolchain_sysroot, 'usr', 'include', 'freetype2'),
        os.path.join(toolchain_sysroot, 'usr', 'include', 'libpng16'),
    ]

# the launcher's FreeType glue instead of LVGL's (same as projects/APPLaunch/main/SConstruct)
override_srcs = AGlob('lvgl_ovr/lv_freetype*.c')
override_names = set(os.path.basename(str(s)) for s in override_srcs)
lvgl_component['SRCS'] = [s for s in lvgl_component['SRCS'] if os.path.basename(str(s)) not in override_names]
lvgl_component['SRCS'] += override_srcs
lvgl_component['PRIVATE_INCLUDE'] = [
    ADir('lvgl_ovr'),
    ADir('../../../SDK/github_source/lvgl/lvgl_9_5/lvgl/src/libs/freetype'),
] + lvgl_component['PRIVATE_INCLUDE']
lvgl_component['SRCS'] = [s for s in lvgl_component['SRCS'] if 'lv_sdl_keyboard.c' not in str(s)]
lvgl_component['DEFINITIONS_PRIVATE'] += ['-O2']

for component in env['COMPONENTS']:
    if component['target'] == 'cp0_lvgl':
        component['DEFINITIONS_PRIVATE'] = component.get('DEFINITIONS_PRIVATE', []) + ['-O2']

# the status strip renderer (cp0_statusbar.c, through _common/stub until task L1) includes the FreeType headers
PRIVATE_INCLUDE += [os.path.join(toolchain_sysroot, 'usr', 'include', 'freetype2')]

env['COMPONENTS'].append({'target': env['PROJECT_NAME'],
                          'SRCS': SRCS,
                          'INCLUDE': INCLUDE,
                          'PRIVATE_INCLUDE': PRIVATE_INCLUDE,
                          'REQUIREMENTS': REQUIREMENTS,
                          'STATIC_LIB': STATIC_LIB,
                          'DYNAMIC_LIB': DYNAMIC_LIB,
                          'DEFINITIONS': DEFINITIONS,
                          'DEFINITIONS_PRIVATE': DEFINITIONS_PRIVATE,
                          'LDFLAGS': LDFLAGS,
                          'LINK_SEARCH_PATH': LINK_SEARCH_PATH,
                          'STATIC_FILES': STATIC_FILES,
                          'REGISTER': 'project'
                          })
