#include "log/log.h"

#include <cassert>
#include <iostream>

#define PRINT_LEVEL(level) std::cout << "Level: " << level << std::endl;

int main(int, char**) {
    PRINT_LEVEL(INFO);
    PRINT_LEVEL(WARNING);
    PRINT_LEVEL(ERROR);
    PRINT_LEVEL(FATAL);

    INTERNAL_LOG(INFO) << LOG_TERMINATE;
    INTERNAL_DLOG(INFO) << LOG_TERMINATE;

    LOG_ALWAYS_FATAL_IF(true, "LOG_ALWAYS_FATAL_IF(true)");
    LOG_ALWAYS_FATAL_IF(false, "LOG_ALWAYS_FATAL_IF(false)");
    LOG_ALWAYS_FATAL("LOG_ALWAYS_FATAL");

    ALOGD("ALOGD");
    ALOGW("ALOGW");
    ALOGE("ALOGE");

    return 0;
}