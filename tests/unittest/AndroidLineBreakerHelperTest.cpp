/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "minikin/AndroidLineBreakerHelper.h"

#include <gtest/gtest.h>

#include "minikin/LineBreaker.h"
#include "FontTestUtils.h"
#include "ICUTestBase.h"
#include "UnicodeUtils.h"

namespace minikin {
namespace android {

constexpr float CHAR_WIDTH = 10.0;  // Mock implementation always returns 10.0 for advance.
constexpr const char* SYSTEM_FONT_PATH = "/system/fonts/";
constexpr const char* SYSTEM_FONT_XML = "/system/etc/fonts.xml";

typedef ICUTestBase AndroidLineBreakerHelperTest;

TEST_F(AndroidLineBreakerHelperTest, RunTests) {
    constexpr uint32_t CHAR_COUNT = 6;
    constexpr float REPLACEMENT_WIDTH = 20.0f;
    constexpr float LINE_WIDTH = 1024 * 1024;  // enough space to be one line.
    std::shared_ptr<FontCollection> collection =
            getFontCollection(SYSTEM_FONT_PATH, SYSTEM_FONT_XML);

    StaticLayoutNative layoutNative(
            kBreakStrategy_Greedy,  // break strategy
            kHyphenationFrequency_None,  // hyphenation frequency
            false,  // not justified
            std::vector<float>(),  // indents
            std::vector<float>(),  // left padding
            std::vector<float>());  // right padding

    layoutNative.addStyleRun(0, 2, MinikinPaint(), collection, false /* is RTL */);
    layoutNative.addReplacementRun(2, 4, REPLACEMENT_WIDTH, 0 /* locale list id */);
    layoutNative.addStyleRun(4, 6, MinikinPaint(), collection, false /* is RTL */);

    std::vector<uint16_t> text(CHAR_COUNT, 'a');

    LineBreaker lineBreaker(text);
    lineBreaker.setStrategy(layoutNative.getStrategy());
    lineBreaker.setHyphenationFrequency(layoutNative.getFrequency());
    lineBreaker.setJustified(layoutNative.isJustified());
    lineBreaker.setLineWidthDelegate(
        layoutNative.buildLineWidthDelegate(LINE_WIDTH, 1, LINE_WIDTH, 0 /* starting line no */));

    layoutNative.addRuns(&lineBreaker);

    lineBreaker.computeBreaks();

    // ReplacementRun assigns all width to the first character and leave zeros others.
    std::vector<float> expectedWidths = {
        CHAR_WIDTH, CHAR_WIDTH, REPLACEMENT_WIDTH, 0, CHAR_WIDTH, CHAR_WIDTH
    };

    std::vector<float> actualWidths(lineBreaker.charWidths(),
                                    lineBreaker.charWidths() + lineBreaker.size());

    EXPECT_EQ(expectedWidths, actualWidths);
}

}  // namespace android
}  // namespace minikin
