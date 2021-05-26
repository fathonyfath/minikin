cmake_minimum_required(VERSION 3.12)
project(minikin)

set(target_sources 
        BidiUtils.cpp
        CmapCoverage.cpp
        Emoji.cpp
        FontCollection.cpp
        FontFamily.cpp
        FontUtils.cpp
        GraphemeBreak.cpp
        GreedyLineBreaker.cpp
        Hyphenator.cpp
        HyphenatorMap.cpp
        Layout.cpp
        LayoutCore.cpp
        LayoutUtils.cpp
        LineBreaker.cpp
        LineBreakerUtil.cpp
        Locale.cpp
        LocaleListCache.cpp
        MeasuredText.cpp
        Measurement.cpp
        MinikinInternal.cpp
        OptimalLineBreaker.cpp
        SparseBitSet.cpp
        SystemFonts.cpp
        WordBreaker.cpp)
        
list(TRANSFORM target_sources PREPEND "libs/minikin/")

add_library(${PROJECT_NAME} ${target_sources})
add_library(minikin::minikin ALIAS ${PROJECT_NAME})

set_target_properties(${PROJECT_NAME} PROPERTIES
                        CXX_STANDARD 17)

target_include_directories(${PROJECT_NAME} PUBLIC ${PROJECT_SOURCE_DIR}/include)

set(BUILD_ICU ON CACHE BOOL "Enable compilation of ICU" FORCE)
set(ICU_BUILD_VERSION "69.1" CACHE STRING "ICU version to build" FORCE)
set(ICU_STATIC ON CACHE BOOL "" FORCE)

add_subdirectory(vendor/icu-cmake)
target_link_libraries(${PROJECT_NAME} icu)

add_subdirectory(vendor/googletest)
target_link_libraries(${PROJECT_NAME} gtest_main)

add_subdirectory(vendor/harfbuzz)
target_link_libraries(${PROJECT_NAME} harfbuzz)

add_subdirectory(vendor/utils)
target_link_libraries(${PROJECT_NAME} utils)

add_subdirectory(vendor/log)
target_link_libraries(${PROJECT_NAME} log)

add_executable(minikin-app main.cc)
target_link_libraries(minikin-app minikin::minikin)

option(WITH_ZLIB "Freetype: Enable zlib " OFF)
option(WITH_BZip2 "Freetype: Enable bzip2 " OFF)
option(WITH_HarfBuzz "Freetype: Enable harfbuzz" OFF)
option(WITH_PNG "Freetype: Build with PNG" OFF)
add_subdirectory(vendor/freetype)