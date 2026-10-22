/*
 * SPDX-License-Identifier: MIT
 *
 * Version stamp of an app build. build_app.sh writes cdk_build_info.h (the app's version from app.json, the launcher
 * commit the build used and whether it matches apps/_common/LAUNCHER_REF, the date); without it (a hand build) the
 * defaults below say so.
 */
#pragma once

#if __has_include("cdk_build_info.h")
#include "cdk_build_info.h"
#endif

#ifndef CDK_APP_VERSION
#define CDK_APP_VERSION "0.0.0-dev"
#endif
#ifndef CDK_LAUNCHER_REF
#define CDK_LAUNCHER_REF "unknown"
#endif
#ifndef CDK_LAUNCHER_PIN
#define CDK_LAUNCHER_PIN "unchecked" /* pinned | same-tree | differs | unchecked */
#endif
#ifndef CDK_BUILD_DATE
#define CDK_BUILD_DATE "unknown"
#endif
#ifndef CDK_BUILD_TARGET
#define CDK_BUILD_TARGET "unknown" /* aarch64 | x86_64 | host */
#endif
