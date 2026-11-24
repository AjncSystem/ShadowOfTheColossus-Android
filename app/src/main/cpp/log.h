// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <android/log.h>

#define SOTC_LOG_TAG "SotC"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, SOTC_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, SOTC_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SOTC_LOG_TAG, __VA_ARGS__)
