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

#include "minikin/LineBreaker.h"

#include <algorithm>

#include <cutils/log.h>
#include <unicode/locid.h>
#include <unicode/uchriter.h>
#include <gtest/gtest.h>

#include "FontTestUtils.h"
#include "ICUTestBase.h"
#include "LocaleListCache.h"
#include "MinikinInternal.h"
#include "WordBreaker.h"

namespace minikin {

constexpr float CHAR_WIDTH = 10.0;  // Mock implementation always returns 10.0 for advance.

static uint32_t getLocaleListId(const std::string& langTags) {
    android::AutoMutex _l(gMinikinLock);
    return LocaleListCache::getId(langTags);
}

static const LocaleList& getLocaleList(uint32_t localeListId) {
    android::AutoMutex _l(gMinikinLock);
    return LocaleListCache::getById(localeListId);
}

struct LocaleComparator {
    bool operator() (const Locale& l, const Locale& r) const {
        return l.getIdentifier() > r.getIdentifier();
    }
};

typedef std::map<Locale, std::string, LocaleComparator> LocaleIteratorMap;

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

    UBool isBoundary(int32_t offset) override {
        auto i = std::find(mBreakPoints.begin(), mBreakPoints.end(), offset);
        return i != mBreakPoints.end();
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

// A mock implementation of ICULineBreakerPool for injecting mock break iterators.
// This class does not pool and delete immediately when the iterator is released.
class MockICULineBreakerPoolImpl : public ICULineBreakerPool {
public:
    MockICULineBreakerPoolImpl(LocaleIteratorMap&& map) : mLocaleIteratorMap(std::move(map)) {}

    Slot acquire(const Locale& locale) override {
        auto i = mLocaleIteratorMap.find(locale);
        LOG_ALWAYS_FATAL_IF(i == mLocaleIteratorMap.end(),
                "Iterator not found for %s", locale.getString().c_str());
        return {locale.getIdentifier(),
              std::make_unique<MockBreakIterator>(buildBreakPointList(i->second)) };

    }

    void release(Slot&& slot) override {
        // Move to local variable, so that the slot will be deleted when returning this method.
        Slot localSlot = std::move(slot);
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

class TestableWordBreaker : public WordBreaker {
public:
    TestableWordBreaker(ICULineBreakerPool* pool) : WordBreaker(pool) {}
};

class TestableLineBreaker : public LineBreaker {
public:
    TestableLineBreaker(ICULineBreakerPool* pool)
            : LineBreaker(std::make_unique<TestableWordBreaker>(pool)) {}
};

class RectangleLineWidthDelegate : public LineBreaker::LineWidthDelegate {
public:
    RectangleLineWidthDelegate(float width) : mWidth(width) {}
    virtual ~RectangleLineWidthDelegate() {}

    float getLineWidth(size_t) override {
        return mWidth;
    }
    float getLeftPadding(size_t) override {
        return 0;
    }
    float getRightPadding(size_t) override {
        return 0;
    }

private:
    float mWidth;
};

static void setupLineBreaker(LineBreaker* b, const std::string& text) {
    b->resize(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        b->buffer()[i] = text[i];
    }
    b->setText();
}

class LineBreakerTest : public ICUTestBase {
public:
    LineBreakerTest() : ICUTestBase() {}

    virtual ~LineBreakerTest() {}

protected:
    virtual void SetUp() override {
        ICUTestBase::SetUp();
        mCollection = getFontCollection(SYSTEM_FONT_PATH, SYSTEM_FONT_XML);
    }

    virtual void TearDown() override {
        mCollection.reset();
        ICUTestBase::TearDown();
    }

    std::shared_ptr<FontCollection> mCollection;

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
    const uint32_t enUSLocaleListId = getLocaleListId("en-US");
    const Locale& enUSLocale = getLocaleList(enUSLocaleListId)[0];
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
        map[enUSLocale] = "ab|cde|fgh|ijk|lmn|o|pqr|st|u|vwxyz";

        MockICULineBreakerPoolImpl impl(std::move(map));
        TestableLineBreaker b(&impl);
        setupLineBreaker(&b, text);

        b.setLineWidthDelegate(std::make_unique<RectangleLineWidthDelegate>(5 * CHAR_WIDTH));

        MinikinPaint paint;
        paint.localeListId = enUSLocaleListId;
        b.addStyleRun(&paint, mCollection, 0, text.size(), false);

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
    const uint32_t enUSLocaleListId = getLocaleListId("en-US");
    const Locale& enUSLocale = getLocaleList(enUSLocaleListId)[0];
    const uint32_t frFRLocaleListId = getLocaleListId("fr-FR");
    const Locale& frFRLocale = getLocaleList(frFRLocaleListId)[0];
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
        map[enUSLocale] = "ab|cde|fgh";
        map[frFRLocale] = "ab|cdef|gh";

        MockICULineBreakerPoolImpl impl(std::move(map));
        TestableLineBreaker b(&impl);
        setupLineBreaker(&b, text);
        b.setLineWidthDelegate(std::make_unique<RectangleLineWidthDelegate>(5 * CHAR_WIDTH));

        MinikinPaint paint;
        paint.localeListId = enUSLocaleListId;
        b.addStyleRun(&paint, mCollection, 0, 2, false);
        paint.localeListId = frFRLocaleListId;
        b.addStyleRun(&paint, mCollection, 2, text.size(), false);
        const size_t breakNum = b.computeBreaks();
        ASSERT_EQ(3U, breakNum);
        expectLineBreaks("ab|cdef|gh",
                 std::vector<int>(b.getBreaks(), b.getBreaks() + breakNum));

        EXPECT_EQ(std::vector<float>({
                2 * CHAR_WIDTH,
                4 * CHAR_WIDTH,
                2 * CHAR_WIDTH,
            }), std::vector<float>(b.getWidths(), b.getWidths() + breakNum));
    }
    // TODO: Add more test cases, hyphenataion etc.
}

TEST_F(LineBreakerTest, greedyLocaleSwich_KeepSameLocaleTest) {
    const uint32_t enUSLocaleListId = getLocaleListId("en-US");
    const Locale& enUSLocale = getLocaleList(enUSLocaleListId)[0];
    {
        // The line breaking is expected to work like this:
        // Input:
        //    Text:           a b c d e f g h
        //    US WordBreaker:    ^     ^
        // Output: | a b c d e |
        //         | f g h _ _ |
        // Here, "|" is canvas space. "^" means word break point and "_" means empty
        const std::string text = "abcdefgh";
        LocaleIteratorMap map;
        map[enUSLocale] = "ab|cde|fgh";

        MockICULineBreakerPoolImpl impl(std::move(map));
        TestableLineBreaker b(&impl);
        setupLineBreaker(&b, text);
        b.setLineWidthDelegate(std::make_unique<RectangleLineWidthDelegate>(5 * CHAR_WIDTH));

        MinikinPaint paint;
        paint.localeListId = enUSLocaleListId;
        b.addStyleRun(&paint, mCollection, 0, 2, false);
        b.addStyleRun(&paint, mCollection, 2, text.size(), false);

        const size_t breakNum = b.computeBreaks();
        ASSERT_EQ(2U, breakNum);
        expectLineBreaks("abcde|fgh",
                 std::vector<int>(b.getBreaks(), b.getBreaks() + breakNum));
        EXPECT_EQ(std::vector<float>({
                5 * CHAR_WIDTH,
                3 * CHAR_WIDTH,
            }), std::vector<float>(b.getWidths(), b.getWidths() + breakNum));
    }
}

TEST_F(LineBreakerTest, greedyLocaleSwich_insideEmail) {
    const uint32_t enUSLocaleListId = getLocaleListId("en-US");
    const Locale& enUSLocale = getLocaleList(enUSLocaleListId)[0];
    const uint32_t frFRLocaleListId = getLocaleListId("fr-FR");
    const Locale& frFRLocale = getLocaleList(frFRLocaleListId)[0];
    {
        // The line breaking is expected to work like this:
        // Input:
        //    Text:           a a a U+20 b @ c - d U+20 e e e
        //    US WordBreaker:           ^              ^
        //    FR WordBreaker:           ^              ^
        //    Locale Region : [    US          ][     FR     ]
        // Output: | a a a _ |
        //         | b @ c _ |
        //         | - d _ _ |
        //         | e e e _ |
        //
        // Here, "|" is canvas space. "^" means word break point and "_" means empty
        const std::string text = "aaa b@c-d eee";
        LocaleIteratorMap map;
        map[enUSLocale] = "aaa |b@c-d |eee";
        map[frFRLocale] = "aaa |b@c-d |eee";

        MockICULineBreakerPoolImpl impl(std::move(map));
        TestableLineBreaker b(&impl);
        setupLineBreaker(&b, text);
        b.setLineWidthDelegate(std::make_unique<RectangleLineWidthDelegate>(4 * CHAR_WIDTH));

        MinikinPaint paint;
        paint.localeListId = enUSLocaleListId;
        b.addStyleRun(&paint, mCollection, 0, 7, false);
        paint.localeListId = frFRLocaleListId;
        b.addStyleRun(&paint, mCollection, 7, text.size(), false);

        const size_t breakNum = b.computeBreaks();
        expectLineBreaks("aaa |b@c|-d |eee",
                 std::vector<int>(b.getBreaks(), b.getBreaks() + breakNum));
        EXPECT_EQ(std::vector<float>({
                3 * CHAR_WIDTH,  // Doesn't count the trailing spaces.
                3 * CHAR_WIDTH,  // Doesn't count the trailing spaces.
                2 * CHAR_WIDTH,
                3 * CHAR_WIDTH,
            }), std::vector<float>(b.getWidths(), b.getWidths() + breakNum));
    }
}

// b/68669534
TEST_F(LineBreakerTest, CrashFix_Space_Tab) {
    const uint32_t enUSLocaleListId = getLocaleListId("en-US");
    const Locale& enUSLocale = getLocaleList(enUSLocaleListId)[0];

    const std::string text = "a \tb";
    LocaleIteratorMap map;
    map[enUSLocale] = "a |\t|b";

    MockICULineBreakerPoolImpl impl(std::move(map));
    TestableLineBreaker b(&impl);
    setupLineBreaker(&b, text);
    b.setLineWidthDelegate(std::make_unique<RectangleLineWidthDelegate>(5 * CHAR_WIDTH));

    MinikinPaint paint;
    paint.localeListId = enUSLocaleListId;
    b.addStyleRun(&paint, mCollection, 0, text.size(), false);

    b.computeBreaks();  // Make sure no crash happens.
}

class LocaleTraceICULineBreakerPoolImpl : public ICULineBreakerPool {
public:
    LocaleTraceICULineBreakerPoolImpl() : mAcquireCallCount(0) {}

    Slot acquire(const Locale& locale) override {
        mAcquireCallCount++;
        mLastRequestedLanguage = locale;
        UErrorCode status = U_ZERO_ERROR;
        return {locale.getIdentifier(), std::unique_ptr<icu::BreakIterator>(
                icu::BreakIterator::createLineInstance(icu::Locale::getRoot(), status))};
    }

    void release(Slot&& slot) override {
        // Move to local variable, so that the slot will be deleted when returning this method.
        Slot localSlot = std::move(slot);
    }

    int getLocaleChangeCount() const {
        return mAcquireCallCount;
    }

    const Locale& getLastPassedLocale() const {
        return mLastRequestedLanguage;
    }

private:
    int mAcquireCallCount;
    Locale mLastRequestedLanguage;

};

TEST_F(LineBreakerTest, setLocaleList) {
    constexpr size_t CHAR_COUNT = 14;
    MinikinPaint paint;
    std::string text('a', CHAR_COUNT);
    LocaleTraceICULineBreakerPoolImpl localeTracer;
    TestableLineBreaker lineBreaker(&localeTracer);
    setupLineBreaker(&lineBreaker, text);
    lineBreaker.setLineWidthDelegate(
            std::make_unique<RectangleLineWidthDelegate>(CHAR_COUNT * CHAR_WIDTH));
    int expectedCallCount = 0;

    const Locale& enUS = getLocaleList(getLocaleListId("en-US"))[0];
    const Locale& frFR = getLocaleList(getLocaleListId("fr-FR"))[0];
    {
        paint.localeListId = getLocaleListId("en-US");
        lineBreaker.addStyleRun(&paint, mCollection, 0, 1, false);
        expectedCallCount++;  // The locale changes from initial state to en-US.
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_EQ(enUS, localeTracer.getLastPassedLocale());

        // Calling the same locale must not update locale.
        lineBreaker.addStyleRun(&paint, mCollection, 1, 2, false);
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_EQ(enUS, localeTracer.getLastPassedLocale());
    }
    {
        paint.localeListId = getLocaleListId("fr-FR,en-US");
        lineBreaker.addStyleRun(&paint, mCollection, 2, 3, false);
        expectedCallCount++;  // The locale changes from en-US to fr-FR.
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_EQ(frFR, localeTracer.getLastPassedLocale());

        // Calling the same locale must not update locale.
        lineBreaker.addStyleRun(&paint, mCollection, 3, 4, false);
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_EQ(frFR, localeTracer.getLastPassedLocale());
    }
    {
        paint.localeListId = getLocaleListId("fr-FR");
        lineBreaker.addStyleRun(&paint, mCollection, 4, 5, false);
        // Expected not to be called since the first locale is not changed.
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_EQ(frFR, localeTracer.getLastPassedLocale());

        // Calling the same locale must not update locale.
        lineBreaker.addStyleRun(&paint, mCollection, 5, 6, false);
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_EQ(frFR, localeTracer.getLastPassedLocale());
    }
    {
        paint.localeListId = getLocaleListId("");
        lineBreaker.addStyleRun(&paint, mCollection, 6, 7, false);
        expectedCallCount++;  // The locale changes from fr-FR to icu::Locale::Root.
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_FALSE(localeTracer.getLastPassedLocale().isSupported());

        // Calling the same locale must not update locale.
        lineBreaker.addStyleRun(&paint, mCollection, 7, 8, false);
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_FALSE(localeTracer.getLastPassedLocale().isSupported());
    }
    {
        paint.localeListId = getLocaleListId("THISISABOGUSLANGUAGE");
        lineBreaker.addStyleRun(&paint, mCollection, 8, 9, false);
        // Expected not to be called. The bogus locale ends up with the empty locale.
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_FALSE(localeTracer.getLastPassedLocale().isSupported());

        // Calling the same locale must not update locale.
        lineBreaker.addStyleRun(&paint, mCollection, 9, 10, false);
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_FALSE(localeTracer.getLastPassedLocale().isSupported());
    }
    {
        paint.localeListId = getLocaleListId("THISISABOGUSLANGUAGE,en-US");
        lineBreaker.addStyleRun(&paint, mCollection, 10, 11, false);
        expectedCallCount++;  // The locale changes from icu::Locale::Root to en-US.
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_EQ(enUS, localeTracer.getLastPassedLocale());

        // Calling the same locale must not update locale.
        lineBreaker.addStyleRun(&paint, mCollection, 11, 12, false);
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_EQ(enUS, localeTracer.getLastPassedLocale());
    }
    {
        paint.localeListId = getLocaleListId("THISISABOGUSLANGUAGE,ANOTHERBOGUSLANGUAGE");
        lineBreaker.addStyleRun(&paint, mCollection, 12, 13, false);
        expectedCallCount++;  // The locale changes from en-US to icu::Locale::Root
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_FALSE(localeTracer.getLastPassedLocale().isSupported());

        // Calling the same locale must not update locale.
        lineBreaker.addStyleRun(&paint, mCollection, 13, 14, false);
        EXPECT_EQ(expectedCallCount, localeTracer.getLocaleChangeCount());
        EXPECT_FALSE(localeTracer.getLastPassedLocale().isSupported());
    }
}

}  // namespace minikin
