/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "PathUtils.h"

#include <cutils/log.h>
#include <libgen.h>
#include <unistd.h>

namespace minikin {

const char* SELF_EXE_PATH = "/proc/self/exe";

std::string getTestFontPath(const std::string& fontFilePath) {
    char buf[PATH_MAX] = {};
    LOG_ALWAYS_FATAL_IF(readlink(SELF_EXE_PATH, buf, PATH_MAX) == -1, "readlink failed.");
    const char* dir = dirname(buf);
    LOG_ALWAYS_FATAL_IF(dir == nullptr, "dirname failed.");
    return std::string(dir) + "/data/" + fontFilePath;
}

}  // namespace minikin
