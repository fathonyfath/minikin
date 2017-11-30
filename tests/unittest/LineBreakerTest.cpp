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
#include <array>
#include <memory>
#include <utility>

#include <cutils/log.h>
#include <gtest/gtest.h>
#include <unicode/locid.h>
#include <unicode/uchriter.h>

#include "FontTestUtils.h"
#include "ICUTestBase.h"
#include "LocaleListCache.h"
#include "MinikinInternal.h"
#include "UnicodeUtils.h"
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
    bool operator()(const Locale& l, const Locale& r) const {
        return l.getIdentifier() > r.getIdentifier();
    }
};

// Helper function for creating vector of move-only elements.
template <class Base, class T, class... Tail>
constexpr std::vector<Base> makeVector(T&& head, Tail&&... tail) {
    std::array<T, 1 + sizeof...(Tail)> ar = {{std::forward<T>(head), std::forward<Tail>(tail)...}};
    return std::vector<Base>(
            {std::make_move_iterator(std::begin(ar)), std::make_move_iterator(std::end(ar))});
}

typedef std::map<Locale, std::string, LocaleComparator> LocaleIteratorMap;

class MockBreakIterator : public icu::BreakIterator {
public:
    MockBreakIterator(const std::vector<int32_t>& breakPoints) : mBreakPoints(breakPoints) {}

    virtual ~MockBreakIterator() {}

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

    int32_t next() override { return following(mCurrent); }

    int32_t current() const override { return mCurrent; }

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

    BreakIterator& refreshInputText(UText*, UErrorCode&) override {
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
        LOG_ALWAYS_FATAL_IF(i == mLocaleIteratorMap.end(), "Iterator not found for %s",
                            locale.getString().c_str());
        return {locale.getIdentifier(),
                std::make_unique<MockBreakIterator>(buildBreakPointList(i->second))};
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
        std::vector<int> out = {0};
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
    TestableLineBreaker(ICULineBreakerPool* pool, const U16StringPiece& string,
                        const MeasuredText& measuredText)
            : LineBreaker(std::make_unique<TestableWordBreaker>(pool), string, measuredText,
                          BreakStrategy::Greedy, HyphenationFrequency::None,
                          false /* justified */) {}
};

class RectangleLineWidthDelegate : public LineWidth {
public:
    RectangleLineWidthDelegate(float width) : mWidth(width) {}
    virtual ~RectangleLineWidthDelegate() {}

    float getAt(size_t) const override { return mWidth; }
    float getMin() const override { return mWidth; }
    float getLeftPaddingAt(size_t) const override { return 0; }
    float getRightPaddingAt(size_t) const override { return 0; }

private:
    float mWidth;
};

class LineBreakerTest : public ICUTestBase {
public:
    LineBreakerTest() : ICUTestBase(), mTabStops(nullptr, 0, 0) {}

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

    LineBreakResult doLineBreak(ICULineBreakerPool* pool, const U16StringPiece& text,
                                const std::vector<std::unique_ptr<minikin::Run>>& runs,
                                float lineWidth) {
        RectangleLineWidthDelegate rectangleLineWidth(lineWidth);
        MeasuredText measuredText = MeasuredText::generate(text, runs);
        TestableLineBreaker b(pool, text, measuredText);
        return b.computeBreaks(runs, rectangleLineWidth, mTabStops);
    }

    std::shared_ptr<FontCollection> mCollection;
    TabStops mTabStops;

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

class ConstantRun : public Run {
public:
    ConstantRun(const Range& range, uint32_t localeListId, float width)
            : Run(range), mLocaleListId(localeListId), mWidth(width) {}

    virtual bool isRtl() const override { return false; }
    virtual bool canHyphenate() const override { return true; }
    virtual uint32_t getLocaleListId() const { return mLocaleListId; }

    virtual void getMetrics(const U16StringPiece&, float* advances, MinikinExtent*,
                            LayoutOverhang*) const {
        std::fill(advances, advances + mRange.getLength(), mWidth);
    }

    virtual const MinikinPaint* getPaint() const { return &mPaint; }

    virtual float measureHyphenPiece(const U16StringPiece&, const Range& range, StartHyphenEdit,
                                     EndHyphenEdit, float* advances, LayoutOverhang*) const {
        std::fill(advances, advances + range.getLength(), mWidth);
        return mWidth * range.getLength();
    }

private:
    MinikinPaint mPaint;
    uint32_t mLocaleListId;
    float mWidth;
};

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
        const std::vector<uint16_t> text = utf8ToUtf16("abcdefghijklmnopqrstuvwxyz");
        LocaleIteratorMap map;
        map[enUSLocale] = "ab|cde|fgh|ijk|lmn|o|pqr|st|u|vwxyz";

        MockICULineBreakerPoolImpl impl(std::move(map));
        LineBreakResult result =
                doLineBreak(&impl, text,
                            makeVector<std::unique_ptr<minikin::Run>>(std::make_unique<ConstantRun>(
                                    Range(0, text.size()), enUSLocaleListId, CHAR_WIDTH)),
                            5 * CHAR_WIDTH);

        ASSERT_EQ(7U, result.breakPoints.size());
        expectLineBreaks("abcde|fgh|ijk|lmno|pqrst|u|vwxyz", result.breakPoints);

        EXPECT_EQ(std::vector<float>({
                          5 * CHAR_WIDTH, 3 * CHAR_WIDTH, 3 * CHAR_WIDTH, 4 * CHAR_WIDTH,
                          5 * CHAR_WIDTH, 1 * CHAR_WIDTH, 5 * CHAR_WIDTH,
                  }),
                  result.widths);
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
        const std::vector<uint16_t> text = utf8ToUtf16("abcdefgh");
        LocaleIteratorMap map;
        map[enUSLocale] = "ab|cde|fgh";
        map[frFRLocale] = "ab|cdef|gh";

        MockICULineBreakerPoolImpl impl(std::move(map));
        LineBreakResult result = doLineBreak(
                &impl, text,
                makeVector<std::unique_ptr<minikin::Run>>(
                        std::make_unique<ConstantRun>(Range(0, 2), enUSLocaleListId, CHAR_WIDTH),
                        std::make_unique<ConstantRun>(Range(2, text.size()), frFRLocaleListId,
                                                      CHAR_WIDTH)),
                5 * CHAR_WIDTH);

        ASSERT_EQ(3U, result.breakPoints.size());
        expectLineBreaks("ab|cdef|gh", result.breakPoints);

        EXPECT_EQ(std::vector<float>({
                          2 * CHAR_WIDTH, 4 * CHAR_WIDTH, 2 * CHAR_WIDTH,
                  }),
                  result.widths);
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
        const std::vector<uint16_t> text = utf8ToUtf16("abcdefgh");
        LocaleIteratorMap map;
        map[enUSLocale] = "ab|cde|fgh";

        MockICULineBreakerPoolImpl impl(std::move(map));
        LineBreakResult result = doLineBreak(
                &impl, text,
                makeVector<std::unique_ptr<minikin::Run>>(
                        std::make_unique<ConstantRun>(Range(0, 2), enUSLocaleListId, CHAR_WIDTH),
                        std::make_unique<ConstantRun>(Range(2, text.size()), enUSLocaleListId,
                                                      CHAR_WIDTH)),
                5 * CHAR_WIDTH);

        ASSERT_EQ(2U, result.breakPoints.size());
        expectLineBreaks("abcde|fgh", result.breakPoints);
        EXPECT_EQ(std::vector<float>({
                          5 * CHAR_WIDTH, 3 * CHAR_WIDTH,
                  }),
                  result.widths);
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
        const std::vector<uint16_t> text = utf8ToUtf16("aaa b@c-d eee");
        LocaleIteratorMap map;
        map[enUSLocale] = "aaa |b@c-d |eee";
        map[frFRLocale] = "aaa |b@c-d |eee";

        MockICULineBreakerPoolImpl impl(std::move(map));
        LineBreakResult result = doLineBreak(
                &impl, text,
                makeVector<std::unique_ptr<minikin::Run>>(
                        std::make_unique<ConstantRun>(Range(0, 7), enUSLocaleListId, CHAR_WIDTH),
                        std::make_unique<ConstantRun>(Range(7, text.size()), frFRLocaleListId,
                                                      CHAR_WIDTH)),
                4 * CHAR_WIDTH);

        ASSERT_EQ(4U, result.breakPoints.size());
        expectLineBreaks("aaa |b@c|-d |eee", result.breakPoints);
        EXPECT_EQ(std::vector<float>({
                          3 * CHAR_WIDTH,  // Doesn't count the trailing spaces.
                          3 * CHAR_WIDTH,  // Doesn't count the trailing spaces.
                          2 * CHAR_WIDTH, 3 * CHAR_WIDTH,
                  }),
                  result.widths);
    }
}

// b/68669534
TEST_F(LineBreakerTest, CrashFix_Space_Tab) {
    const uint32_t enUSLocaleListId = getLocaleListId("en-US");
    const Locale& enUSLocale = getLocaleList(enUSLocaleListId)[0];

    const std::vector<uint16_t> text = utf8ToUtf16("a \tb");
    LocaleIteratorMap map;
    map[enUSLocale] = "a |\t|b";

    MockICULineBreakerPoolImpl impl(std::move(map));
    std::vector<std::unique_ptr<minikin::Run>> runs = makeVector<std::unique_ptr<minikin::Run>>(
            std::make_unique<ConstantRun>(Range(0, text.size()), enUSLocaleListId, CHAR_WIDTH));
    doLineBreak(&impl, text, runs, 5 * CHAR_WIDTH);  // Make sure no crash happens.
}

class LocaleTraceICULineBreakerPoolImpl : public ICULineBreakerPool {
public:
    LocaleTraceICULineBreakerPoolImpl() {}

    Slot acquire(const Locale& locale) override {
        mRequestedLocaleLog.push_back(locale);
        UErrorCode status = U_ZERO_ERROR;
        return {locale.getIdentifier(),
                std::unique_ptr<icu::BreakIterator>(
                        icu::BreakIterator::createLineInstance(icu::Locale::getRoot(), status))};
    }

    void release(Slot&& slot) override {
        // Move to local variable, so that the slot will be deleted when returning this method.
        Slot localSlot = std::move(slot);
    }

    const std::vector<Locale>& getPassedLocaleLog() const { return mRequestedLocaleLog; }

    void reset() { mRequestedLocaleLog.clear(); }

private:
    std::vector<Locale> mRequestedLocaleLog;
};

TEST_F(LineBreakerTest, setLocaleList) {
    constexpr size_t CHAR_COUNT = 14;
    constexpr float LINE_WIDTH = CHAR_COUNT * CHAR_WIDTH;
    const std::vector<uint16_t> text(CHAR_COUNT, 'a');
    LocaleTraceICULineBreakerPoolImpl localeTracer;

    const Locale& enUS = getLocaleList(getLocaleListId("en-US"))[0];
    const Locale& frFR = getLocaleList(getLocaleListId("fr-FR"))[0];
    const uint32_t enUSId = getLocaleListId("en-US");
    {
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH)),
                    LINE_WIDTH);
        EXPECT_EQ(std::vector<Locale>({enUS}), localeTracer.getPassedLocaleLog());

        // Changing to the same locale must not update locale.
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), enUSId, CHAR_WIDTH)),
                    LINE_WIDTH);
        EXPECT_EQ(std::vector<Locale>({enUS}), localeTracer.getPassedLocaleLog());
    }
    {
        const uint32_t localeListId = getLocaleListId("fr-FR,en-US");
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        EXPECT_EQ(std::vector<Locale>({enUS, frFR}), localeTracer.getPassedLocaleLog());

        // Changing to the same locale must not update locale.
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(2, 3), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        EXPECT_EQ(std::vector<Locale>({enUS, frFR}), localeTracer.getPassedLocaleLog());
    }
    {
        const uint32_t localeListId = getLocaleListId("fr-FR");
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        EXPECT_EQ(std::vector<Locale>({enUS, frFR}), localeTracer.getPassedLocaleLog());

        // Changing to the same locale must not update locale.
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(2, 3), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        EXPECT_EQ(std::vector<Locale>({enUS, frFR}), localeTracer.getPassedLocaleLog());
    }
    {
        const uint32_t localeListId = getLocaleListId("");
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        ASSERT_EQ(2u, localeTracer.getPassedLocaleLog().size());
        EXPECT_EQ(enUS, localeTracer.getPassedLocaleLog()[0]);
        EXPECT_FALSE(localeTracer.getPassedLocaleLog()[1].isSupported());

        // Changing to the same locale must not update locale.
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(2, 3), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        ASSERT_EQ(2u, localeTracer.getPassedLocaleLog().size());
        EXPECT_EQ(enUS, localeTracer.getPassedLocaleLog()[0]);
        EXPECT_FALSE(localeTracer.getPassedLocaleLog()[1].isSupported());
    }
    {
        const uint32_t localeListId = getLocaleListId("THISISABOGUSLANGUAGE");
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        ASSERT_EQ(2u, localeTracer.getPassedLocaleLog().size());
        EXPECT_EQ(enUS, localeTracer.getPassedLocaleLog()[0]);
        EXPECT_FALSE(localeTracer.getPassedLocaleLog()[1].isSupported());

        // Changing to the same locale must not update locale.
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(2, 3), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        ASSERT_EQ(2u, localeTracer.getPassedLocaleLog().size());
        EXPECT_EQ(enUS, localeTracer.getPassedLocaleLog()[0]);
        EXPECT_FALSE(localeTracer.getPassedLocaleLog()[1].isSupported());
    }
    {
        const uint32_t localeListId = getLocaleListId("THISISABOGUSLANGUAGE,fr-FR");
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        EXPECT_EQ(std::vector<Locale>({enUS, frFR}), localeTracer.getPassedLocaleLog());

        // Changing to the same locale must not update locale.
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(2, 3), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        EXPECT_EQ(std::vector<Locale>({enUS, frFR}), localeTracer.getPassedLocaleLog());
    }
    {
        const uint32_t localeListId = getLocaleListId("THISISABOGUSLANGUAGE,en-US");
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        EXPECT_EQ(std::vector<Locale>({enUS}), localeTracer.getPassedLocaleLog());

        // Changing to the same locale must not update locale.
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(2, 3), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        EXPECT_EQ(std::vector<Locale>({enUS}), localeTracer.getPassedLocaleLog());
    }
    {
        const uint32_t localeListId = getLocaleListId("THISISABOGUSLANGUAGE,ANOTHERBOGUSLANGUAGE");
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        ASSERT_EQ(2u, localeTracer.getPassedLocaleLog().size());
        EXPECT_EQ(enUS, localeTracer.getPassedLocaleLog()[0]);
        EXPECT_FALSE(localeTracer.getPassedLocaleLog()[1].isSupported());

        // Changing to the same locale must not update locale.
        localeTracer.reset();
        doLineBreak(&localeTracer, text,
                    makeVector<std::unique_ptr<minikin::Run>>(
                            std::make_unique<ConstantRun>(Range(0, 1), enUSId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(1, 2), localeListId, CHAR_WIDTH),
                            std::make_unique<ConstantRun>(Range(2, 3), localeListId, CHAR_WIDTH)),
                    LINE_WIDTH);
        ASSERT_EQ(2u, localeTracer.getPassedLocaleLog().size());
        EXPECT_EQ(enUS, localeTracer.getPassedLocaleLog()[0]);
        EXPECT_FALSE(localeTracer.getPassedLocaleLog()[1].isSupported());
    }
}

}  // namespace minikin
