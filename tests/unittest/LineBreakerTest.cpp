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

#include <gtest/gtest.h>

#include <algorithm>
#include <cutils/log.h>
#include "ICUTestBase.h"
#include "WordBreaker.h"
#include <minikin/LineBreaker.h>
#include <unicode/locid.h>
#include <unicode/uchriter.h>
#include "../util/FontTestUtils.h"

namespace minikin {

constexpr float CHAR_WIDTH = 10.0;  // Mock implementation alwasy returns 10.0 for advance.

struct LocaleComparator {
    bool operator() (const icu::Locale& l, const icu::Locale& r) const {
        return strcmp(l.getName(), r.getName()) > 0;
    }
};

typedef std::map<icu::Locale, std::string, LocaleComparator> LocaleIteratorMap;

class MockBreakIterator : public icu::BreakIterator {
public:
    MockBreakIterator(const std::vector<int32_t>& breakPoints) : mBreakPoints(breakPoints) {
    }

    virtual ~MockBreakIterator() {
    }

    UClassID getDynamicClassID() const override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return 0;
    }

    UBool operator==(const BreakIterator&) const override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return false;
    }

    BreakIterator* clone() const override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return nullptr;
    }

    CharacterIterator& getText() const override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return *static_cast<CharacterIterator*>(new UCharCharacterIterator(nullptr, 0));
    }

    UText* getUText(UText*, UErrorCode&) const override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return nullptr;
    }

    void setText(const UnicodeString&) override {
        // Don't care
    }

    void setText(UText*, UErrorCode&) override {
        // Don't care
    }

    void adoptText(CharacterIterator*) override {
        // Don't care
    }

    int32_t first() override {
        mCurrent = 0;
        return 0;
    }

    int32_t last() override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return 0;
    }

    int32_t previous() override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return 0;
    }

    int32_t next() override {
        return following(mCurrent);
    }

    int32_t current() const override {
        return mCurrent;
    }

    int32_t following(int32_t offset) override {
        auto i = std::upper_bound(mBreakPoints.begin(), mBreakPoints.end(), offset);
        if (i == mBreakPoints.end()) {
            mCurrent = DONE;
        } else {
            mCurrent = *i;
        }
        return mCurrent;
    }

    int32_t preceding(int32_t) override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return 0;
    }

    UBool isBoundary(int32_t) override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return false;
    }

    int32_t next(int32_t) override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return 0;
    }

    BreakIterator* createBufferClone(void*, int32_t&, UErrorCode&) override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return nullptr;
    }

    BreakIterator &refreshInputText(UText*, UErrorCode&) override {
        LOG_ALWAYS_FATAL("Not yet implemented.");
        return *this;
    }

private:
    int32_t mCurrent;
    std::vector<int32_t> mBreakPoints;
};

class TestableWordBreaker : public WordBreaker {
public:
    TestableWordBreaker(LocaleIteratorMap&& map) : mLocaleIteratorMap(std::move(map)) {
    }

    std::unique_ptr<icu::BreakIterator> createBreakIterator(const icu::Locale& locale) override {
        auto i = mLocaleIteratorMap.find(locale);
        LOG_ALWAYS_FATAL_IF(i == mLocaleIteratorMap.end(),
                "Iterator not found for %s", locale.getName());
        return std::make_unique<MockBreakIterator>(buildBreakPointList(i->second));
    }

private:
    // Converts break point string representation to break point index list.
    // For example, "a|bc|d" will be {0, 1, 3, 4}.
    // This means '|' could not be used as an input character.
    static std::vector<int> buildBreakPointList(const std::string& input) {
        std::vector<int> out = { 0 };
        int breakPos = 0;
        for (const char c : input) {
            if (c == '|') {
                out.push_back(breakPos);
            } else {
                breakPos++;
            }
        }
        out.push_back(breakPos);
        return out;
    }
    LocaleIteratorMap mLocaleIteratorMap;
};

class TestableLineBreaker : public LineBreaker {
public:
    TestableLineBreaker(LocaleIteratorMap&& breaker)
            : LineBreaker(std::make_unique<TestableWordBreaker>(std::move(breaker))) {
    }
};

class RectangleLineWidthDelegate : public LineBreaker::LineWidthDelegate {
public:
    RectangleLineWidthDelegate(float width) : mWidth(width) {}
    virtual ~RectangleLineWidthDelegate() {}

    float getLineWidth(size_t) override {
        return mWidth;
    }

private:
    float mWidth;
};

static void setupLineBreaker(LineBreaker* b, const std::string& text) {
    b->resize(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        b->buffer()[i] = text[i];
    }
    // Need setLocales otherwise null pointer dereference happens in setText.
    // TODO: Remove this setLocales.
    b->setLocales("en_US", std::vector<Hyphenator*>());
    b->setText();
}

class LineBreakerTest : public ICUTestBase {
public:
    LineBreakerTest()
            : ICUTestBase(), mCollection(getFontCollection(SYSTEM_FONT_PATH, SYSTEM_FONT_XML)) {
    }

    virtual ~LineBreakerTest() {}

protected:
    const std::shared_ptr<FontCollection> mCollection;

private:
    static constexpr const char* SYSTEM_FONT_PATH = "/system/fonts/";
    static constexpr const char* SYSTEM_FONT_XML = "/system/etc/fonts.xml";
};

static std::string buildDisplayText(const std::string& text, const std::vector<int>& breaks) {
    std::string out = text;
    for (auto ri = breaks.rbegin(); ri != breaks.rend(); ri++) {
        out.insert(*ri, "|");
    }
    return out;
}

static void expectLineBreaks(const std::string& expected, const std::vector<int>& actualBreaks) {
    std::vector<int> expectedBreaks;
    std::string text;
    int breakPos = 0;
    for (char c : expected) {
        if (c == '|') {
            expectedBreaks.push_back(breakPos);
        } else {
            text.push_back(c);
            breakPos++;
        }
    }
    expectedBreaks.push_back(breakPos);

    EXPECT_EQ(expectedBreaks, actualBreaks)
        << "Expected: " << buildDisplayText(text, expectedBreaks) << std::endl
        << "Actual  : " << buildDisplayText(text, actualBreaks);
}

TEST_F(LineBreakerTest, greedyLineBreakAtWordBreakingPoint) {
    {
        // The line breaking is expected to work like this:
        // Input:
        //    Text:        a b c d e f g h i j k l m n o p q r s t u v w x y z
        //    WordBreaker:    ^     ^     ^     ^     ^ ^     ^   ^ ^
        // Output: | a b c d e |
        //         | f g h     |
        //         | i j k     |
        //         | l m n o   |
        //         | p q r s t |
        //         | u         |
        //         | v w x y z |
        // Here, "|" is canvas boundary. "^" is word break point.
        const std::string text = "abcdefghijklmnopqrstuvwxyz";
        LocaleIteratorMap map;
        map[icu::Locale::getUS()] = "ab|cde|fgh|ijk|lmn|o|pqr|st|u|vwxyz";

        TestableLineBreaker b(std::move(map));
        setupLineBreaker(&b, text);

        b.setLineWidthDelegate(std::make_unique<RectangleLineWidthDelegate>(5 * CHAR_WIDTH));

        MinikinPaint paint;
        b.addStyleRun(&paint, mCollection, FontStyle(), 0, text.size(), false /* isRtl */);

        const size_t breakNum = b.computeBreaks();
        ASSERT_EQ(7U, breakNum);
        expectLineBreaks("abcde|fgh|ijk|lmno|pqrst|u|vwxyz",
                  std::vector<int>(b.getBreaks(), b.getBreaks() + breakNum));

        EXPECT_EQ(std::vector<float>({
                5 * CHAR_WIDTH,
                3 * CHAR_WIDTH,
                3 * CHAR_WIDTH,
                4 * CHAR_WIDTH,
                5 * CHAR_WIDTH,
                1 * CHAR_WIDTH,
                5 * CHAR_WIDTH,
            }), std::vector<float>(b.getWidths(), b.getWidths() + breakNum));
    }
    // TODO: Add more test cases, non rectangle, hyphenation, addReplacementSpan etc.
}

TEST_F(LineBreakerTest, greedyLocaleSwitchTest) {
    {
        // The line breaking is expected to work like this:
        // Input:
        //    Text:           a b c d e f g h
        //    US WordBreaker:    ^     ^
        //    FR WordBreaker:    ^       ^
        //    Locale Region : [US][    FR    ]
        // Output: | a b       |
        //         | c d e f   |
        //         | g h       |
        // Here, "|" is canvas boundary. "^" is word break point.
        const std::string text = "abcdefgh";
        LocaleIteratorMap map;
        map[icu::Locale::getUS()] = "ab|cde|fgh";
        map[icu::Locale::getFrance()] = "ab|cdef|gh";

        TestableLineBreaker b(std::move(map));
        setupLineBreaker(&b, text);
        b.setLineWidthDelegate(std::make_unique<RectangleLineWidthDelegate>(5 * CHAR_WIDTH));

        MinikinPaint paint;
        b.setLocales("en_US", std::vector<Hyphenator*>());
        b.addStyleRun(&paint, mCollection, FontStyle(), 0, 2, false /* rtl */);
        b.setLocales("fr_FR", std::vector<Hyphenator*>());
        b.addStyleRun(&paint, mCollection, FontStyle(), 2, text.size(), false /* isRtl */);

        // Currently following expectation doesn't work
        // The expected output is like
        // Output: | a b       |
        //         | c d e f   |
        //         | g h       |
        // However, currently it works as
        // Output: | a b c d e |
        //         | f g h     |
        // http://b/65175766
        // TODO(nona): Enable following test cases.
        // const size_t breakNum = b.computeBreaks();
        // ASSERT_EQ(3U, breakNum);
        // expectLineBreaks("ab|cdef|gh",
        //          std::vector<int>(b.getBreaks(), b.getBreaks() + breakNum));
        //
        // EXPECT_EQ(std::vector<float>({
        //         2 * CHAR_WIDTH,
        //         4 * CHAR_WIDTH,
        //         2 * CHAR_WIDTH,
        //     }), std::vector<float>(b.getWidths(), b.getWidths() + breakNum));
    }
    // TODO: Add more test cases, hyphenataion etc.
}

TEST_F(LineBreakerTest, setLocales) {
    {
        LineBreaker lineBreaker;
        Hyphenator hyphenator;
        std::vector<Hyphenator*> hyphenators;
        hyphenators.push_back(&hyphenator);
        lineBreaker.setLocales("en-US", hyphenators);
        EXPECT_EQ(icu::Locale::getUS(), lineBreaker.mLocale);
        EXPECT_EQ(&hyphenator, lineBreaker.mHyphenator);
    }
    {
        LineBreaker lineBreaker;
        Hyphenator hyphenator1, hyphenator2;
        std::vector<Hyphenator*> hyphenators;
        hyphenators.push_back(&hyphenator1);
        hyphenators.push_back(&hyphenator2);
        lineBreaker.setLocales("fr-FR,en-US", hyphenators);
        EXPECT_EQ(icu::Locale::getFrance(), lineBreaker.mLocale);
        EXPECT_EQ(&hyphenator1, lineBreaker.mHyphenator);
    }
    {
        LineBreaker lineBreaker;
        std::vector<Hyphenator*> hyphenators;
        lineBreaker.setLocales("", hyphenators);
        EXPECT_EQ(icu::Locale::getRoot(), lineBreaker.mLocale);
        EXPECT_EQ(nullptr, lineBreaker.mHyphenator);
    }
    {
        LineBreaker lineBreaker;
        std::vector<Hyphenator*> hyphenators;
        Hyphenator hyphenator;
        hyphenators.push_back(&hyphenator);
        lineBreaker.setLocales("THISISABOGUSLANGUAGE", hyphenators);
        EXPECT_EQ(icu::Locale::getRoot(), lineBreaker.mLocale);
        EXPECT_EQ(nullptr, lineBreaker.mHyphenator);
    }
    {
        LineBreaker lineBreaker;
        Hyphenator hyphenator1, hyphenator2;
        std::vector<Hyphenator*> hyphenators;
        hyphenators.push_back(&hyphenator1);
        hyphenators.push_back(&hyphenator2);
        lineBreaker.setLocales("THISISABOGUSLANGUAGE,en-US", hyphenators);
        EXPECT_EQ(icu::Locale::getUS(), lineBreaker.mLocale);
        EXPECT_EQ(&hyphenator2, lineBreaker.mHyphenator);
    }
    {
        LineBreaker lineBreaker;
        Hyphenator hyphenator1, hyphenator2;
        std::vector<Hyphenator*> hyphenators;
        hyphenators.push_back(&hyphenator1);
        hyphenators.push_back(&hyphenator2);
        lineBreaker.setLocales("THISISABOGUSLANGUAGE,ANOTHERBOGUSLANGUAGE", hyphenators);
        EXPECT_EQ(icu::Locale::getRoot(), lineBreaker.mLocale);
        EXPECT_EQ(nullptr, lineBreaker.mHyphenator);
    }
}

}  // namespace minikin
