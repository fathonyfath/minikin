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

#include "minikin/MeasuredText.h"

#include <gtest/gtest.h>

#include "minikin/LineBreaker.h"

#include "FontTestUtils.h"
#include "UnicodeUtils.h"

namespace minikin {

constexpr float CHAR_WIDTH = 10.0;  // Mock implementation always returns 10.0 for advance.

TEST(MeasuredTextTest, RunTests) {
    constexpr uint32_t CHAR_COUNT = 6;
    constexpr float REPLACEMENT_WIDTH = 20.0f;
    auto font = buildFontCollection(kTestFontDir "Ascii.ttf");

    MeasuredTextBuilder builder;

    MinikinPaint paint1(font);
    paint1.size = 10.0f;  // make 1em = 10px
    builder.addStyleRun(0, 2, std::move(paint1), false /* is RTL */);
    builder.addReplacementRun(2, 4, REPLACEMENT_WIDTH, 0 /* locale list id */);
    MinikinPaint paint2(font);
    paint2.size = 10.0f;  // make 1em = 10px
    builder.addStyleRun(4, 6, std::move(paint2), false /* is RTL */);

    std::vector<uint16_t> text(CHAR_COUNT, 'a');

    std::unique_ptr<MeasuredText> measuredText =
            builder.build(text, true /* compute hyphenation */, false /* compute full layout */);

    ASSERT_TRUE(measuredText);

    // ReplacementRun assigns all width to the first character and leave zeros others.
    std::vector<float> expectedWidths = {CHAR_WIDTH, CHAR_WIDTH, REPLACEMENT_WIDTH,
                                         0,          CHAR_WIDTH, CHAR_WIDTH};

    EXPECT_EQ(expectedWidths, measuredText->widths);
}

TEST(MeasuredTextTest, buildLayoutTest) {
    std::vector<uint16_t> text = utf8ToUtf16("Hello, world.");
    Range range(0, text.size());
    auto font = buildFontCollection(kTestFontDir "Ascii.ttf");
    MinikinPaint paint(font);
    Bidi bidi = Bidi::FORCE_LTR;

    MeasuredTextBuilder builder;
    builder.addStyleRun(0, text.size(), MinikinPaint(font), false /* is RTL */);
    std::unique_ptr<MeasuredText> mt = builder.build(
            text, true /* compute hyphenation */,
            false /* compute full layout. Fill manually later for testing purposes */);

    auto& offsetMap = mt->layoutPieces.offsetMap;
    {
        // If there is no pre-computed layouts, do not touch layout and return false.
        Layout layout;
        offsetMap.clear();
        EXPECT_FALSE(mt->buildLayout(text, range, paint, bidi, 0, StartHyphenEdit::NO_EDIT,
                                     EndHyphenEdit::NO_EDIT, &layout));
        EXPECT_EQ(0U, layout.nGlyphs());
    }
    {
        // If layout result size is different, do not touch layout and return false.
        Layout outLayout;
        Layout inLayout;
        inLayout.mAdvances.resize(text.size() + 1);
        offsetMap.clear();
        offsetMap[0] = inLayout;
        EXPECT_FALSE(mt->buildLayout(text, range, paint, bidi, 0, StartHyphenEdit::NO_EDIT,
                                     EndHyphenEdit::NO_EDIT, &outLayout));
        EXPECT_EQ(0U, outLayout.nGlyphs());
    }
    {
        // If requested layout starts from unknown position, do not touch layout and return false.
        Layout outLayout;
        Layout inLayout;
        inLayout.mAdvances.resize(text.size());
        offsetMap.clear();
        offsetMap[0] = inLayout;
        EXPECT_FALSE(mt->buildLayout(text, Range(range.getStart() + 1, range.getEnd()), paint, bidi,
                                     0, StartHyphenEdit::NO_EDIT, EndHyphenEdit::NO_EDIT,
                                     &outLayout));
        EXPECT_EQ(0U, outLayout.nGlyphs());
    }
    {
        // If requested layout starts from unknown position, do not touch layout and return false.
        // (MeasuredText offset moves forward.)
        Layout outLayout;
        Layout inLayout;
        inLayout.mAdvances.resize(text.size());
        offsetMap.clear();
        offsetMap[0] = inLayout;
        EXPECT_FALSE(mt->buildLayout(text, range, paint, bidi, 1, StartHyphenEdit::NO_EDIT,
                                     EndHyphenEdit::NO_EDIT, &outLayout));
        EXPECT_EQ(0U, outLayout.nGlyphs());
    }
    {
        // Currently justification is not supported.
        Layout outLayout;
        Layout inLayout;
        inLayout.mAdvances.resize(text.size());
        offsetMap.clear();
        offsetMap[0] = inLayout;
        MinikinPaint justifiedPaint(font);
        justifiedPaint.wordSpacing = 1.0;
        EXPECT_FALSE(mt->buildLayout(text, range, justifiedPaint, bidi, 0, StartHyphenEdit::NO_EDIT,
                                     EndHyphenEdit::NO_EDIT, &outLayout));
        EXPECT_EQ(0U, outLayout.nGlyphs());
    }
    {
        // Currently hyphenation is not supported.
        Layout outLayout;
        Layout inLayout;
        inLayout.mAdvances.resize(text.size());
        offsetMap.clear();
        offsetMap[0] = inLayout;
        MinikinPaint hyphenatedPaint(font);
        EXPECT_FALSE(mt->buildLayout(text, range, hyphenatedPaint, bidi, 0,
                                     StartHyphenEdit::NO_EDIT, EndHyphenEdit::INSERT_HYPHEN,
                                     &outLayout));
        EXPECT_EQ(0U, outLayout.nGlyphs());
    }

    // TODO: Add positive test case. This requires Layout refactoring or real text layout in test.
}

}  // namespace minikin
