/*
 * Copyright 2017 Google, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdint.h>

#include <cassert>
#include <iostream>

#if defined(_WIN32)
// Conflicts with macro on Windows.
#undef ERROR
#endif

#ifndef INFO
#define INFO "Info"
#endif

#ifndef WARNING
#define WARNING "Warning"
#endif

#ifndef ERROR
#define ERROR "Error"
#endif

#ifndef FATAL
#define FATAL "Fatal"
#endif

#ifndef INTERNAL_LOG
#define INTERNAL_LOG(level) std::cout << level << ": "
#endif

#ifndef INTERNAL_CHECK
#define INTERNAL_CHECK(cond) assert(cond)
#endif

#ifndef INTERNAL_DLOG
#define INTERNAL_DLOG(level) std::cout << level << ": "
#endif

#ifndef LOG_TERMINATE
#define LOG_TERMINATE std::endl
#endif

#ifndef LOG_ALWAYS_FATAL_IF
#define LOG_ALWAYS_FATAL_IF(cond, ...) \
    if (cond) INTERNAL_LOG(FATAL) << #cond << ": " << __VA_ARGS__ << LOG_TERMINATE
#endif

#ifndef LOG_ALWAYS_FATAL
#define LOG_ALWAYS_FATAL(...) INTERNAL_LOG(FATAL) << __VA_ARGS__ << LOG_TERMINATE
#endif

#ifndef LOG_ASSERT
#define LOG_ASSERT(cond, ...) INTERNAL_CHECK(cond)
#define ALOG_ASSERT LOG_ASSERT
#endif

#ifndef ALOGD
#define ALOGD(message, ...) INTERNAL_DLOG(INFO) << (message) << LOG_TERMINATE
#endif

#ifndef ALOGW
#define ALOGW(message, ...) INTERNAL_LOG(WARNING) << (message) << LOG_TERMINATE
#endif

#ifndef ALOGE
#define ALOGE(message, ...) INTERNAL_LOG(ERROR) << (message) << LOG_TERMINATE
#endif

#define android_errorWriteLog(tag, subTag) __android_log_error_write(tag, subTag, -1, NULL, 0)
#define android_errorWriteWithInfoLog(tag, subTag, uid, data, dataLen) \
    __android_log_error_write(tag, subTag, uid, data, dataLen)

int __android_log_error_write(int tag, const char* subTag, int32_t uid, const char* data,
                              uint32_t dataLen);