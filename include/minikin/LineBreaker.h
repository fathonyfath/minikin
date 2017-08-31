/*
 * Copyright (C) 2015 The Android Open Source Project
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

/**
 * A module for breaking paragraphs into lines, supporting high quality
 * hyphenation and justification.
 */

#ifndef MINIKIN_LINE_BREAKER_H
#define MINIKIN_LINE_BREAKER_H

#include <gtest/gtest_prod.h>
#include "unicode/brkiter.h"
#include "unicode/locid.h"
#include <cmath>
#include <vector>
#include "minikin/FontCollection.h"
#include "minikin/Hyphenator.h"
#include "minikin/Layout.h"
#include "minikin/MinikinFont.h"

namespace minikin {

enum BreakStrategy {
    kBreakStrategy_Greedy = 0,
    kBreakStrategy_HighQuality = 1,
    kBreakStrategy_Balanced = 2
};

enum HyphenationFrequency {
    kHyphenationFrequency_None = 0,
    kHyphenationFrequency_Normal = 1,
    kHyphenationFrequency_Full = 2
};

class WordBreaker;

class TabStops {
    public:
        void set(const int* stops, size_t nStops, int tabWidth) {
            if (stops != nullptr) {
                mStops.assign(stops, stops + nStops);
            } else {
                mStops.clear();
            }
            mTabWidth = tabWidth;
        }
        float nextTab(float widthSoFar) const {
            for (size_t i = 0; i < mStops.size(); i++) {
                if (mStops[i] > widthSoFar) {
                    return mStops[i];
                }
            }
            return floor(widthSoFar / mTabWidth + 1) * mTabWidth;
        }
    private:
        std::vector<int> mStops;
        int mTabWidth;
};

class LineBreaker {
    public:
        // Implement this for the additional information during line breaking.
        class LineWidthDelegate {
            public:
                virtual ~LineWidthDelegate() {}

                // Called to find out the width for the line.
                // This function may be called several times. The implementation must return the
                // same value for the same input.
                virtual float getLineWidth(size_t lineNo) = 0;
        };

        LineBreaker();

        virtual ~LineBreaker();

        const static int kTab_Shift = 29;  // keep synchronized with TAB_MASK in StaticLayout.java

        void resize(size_t size) {
            mTextBuf.resize(size);
            mCharWidths.resize(size);
            mCharExtents.resize(size);
            mCharOverhangs.resize(size);
        }

        size_t size() const {
            return mTextBuf.size();
        }

        uint16_t* buffer() {
            return mTextBuf.data();
        }

        float* charWidths() {
            return mCharWidths.data();
        }

        MinikinExtent* charExtents() {
            return mCharExtents.data();
        }

        LayoutOverhang* charOverhangs() {
            return mCharOverhangs.data();
        }

        void setLineWidthDelegate(std::unique_ptr<LineWidthDelegate>&& lineWidths) {
            mLineWidthDelegate = std::move(lineWidths);
        }

        // set text to current contents of buffer
        void setText();

        void setTabStops(const int* stops, size_t nStops, int tabWidth) {
            mTabStops.set(stops, nStops, tabWidth);
        }

        BreakStrategy getStrategy() const { return mStrategy; }

        void setStrategy(BreakStrategy strategy) { mStrategy = strategy; }

        void setJustified(bool justified) { mJustified = justified; }

        HyphenationFrequency getHyphenationFrequency() const { return mHyphenationFrequency; }

        void setHyphenationFrequency(HyphenationFrequency frequency) {
            mHyphenationFrequency = frequency;
        }

        float addStyleRun(MinikinPaint* paint, const std::shared_ptr<FontCollection>& typeface,
                FontStyle style, size_t start, size_t end, bool isRtl, const char* langTags,
                const std::vector<Hyphenator*>& hyphenators);

        void addReplacement(size_t start, size_t end, float width);

        size_t computeBreaks();

        const int* getBreaks() const {
            return mBreaks.data();
        }

        const float* getWidths() const {
            return mWidths.data();
        }

        const float* getAscents() const {
            return mAscents.data();
        }

        const float* getDescents() const {
            return mDescents.data();
        }

        const int* getFlags() const {
            return mFlags.data();
        }

        void finish();
    protected:
        // For testing purpose.
        LineBreaker(std::unique_ptr<WordBreaker>&& breaker);

    private:
        // ParaWidth is used to hold cumulative width from beginning of paragraph. Note that for
        // very large paragraphs, accuracy could degrade using only 32-bit float. Note however
        // that float is used extensively on the Java side for this. This is a typedef so that
        // we can easily change it based on performance/accuracy tradeoff.
        typedef double ParaWidth;

        // A single candidate break
        struct Candidate {
            size_t offset;  // offset to text buffer, in code units
            size_t prev;  // index to previous break
            ParaWidth preBreak;  // width of text until this point, if we decide to not break here
            ParaWidth postBreak;  // width of text until this point, if we decide to break here
            float penalty;  // penalty of this break (for example, hyphen penalty)
            float score;  // best score found for this break
            size_t preSpaceCount;  // preceding space count before breaking
            size_t postSpaceCount;  // preceding space count after breaking
            MinikinExtent extent; // the largest extent between last candidate and this candidate
            HyphenationType hyphenType;
        };

        // Note: Locale persists across multiple invocations (it is not cleaned up by finish()),
        // explicitly to avoid the cost of creating ICU BreakIterator objects. It should always
        // be set on the first invocation, but callers are encouraged not to call again unless
        // locale has actually changed.
        // That logic could be here but it's better for performance that it's upstream because of
        // the cost of constructing and comparing the ICU Locale object.
        // Note: caller is responsible for managing lifetime of hyphenator
        void setLocales(const char* locales, const std::vector<Hyphenator*>& hyphenators,
                        size_t restartFrom);

        float currentLineWidth() const;

        void addHyphenationCandidates(MinikinPaint* paint,
                const std::shared_ptr<FontCollection>& typeface, FontStyle style, size_t runStart,
                size_t afterWord, size_t lastBreak, ParaWidth lastBreakWidth, ParaWidth PostBreak,
                size_t postSpaceCount, MinikinExtent* extent, float hyphenPenalty, int bidiFlags);

        void addWordBreak(size_t offset, ParaWidth preBreak, ParaWidth postBreak,
                size_t preSpaceCount, size_t postSpaceCount, MinikinExtent extent,
                float penalty, HyphenationType hyph);

        void addCandidate(Candidate cand);
        void pushGreedyBreak();

        MinikinExtent computeMaxExtent(size_t start, size_t end) const;

        static LayoutOverhang computeOverhang(float totalAdvance,
                const std::vector<float>& advances, const std::vector<LayoutOverhang>& overhangs,
                bool isRtl);

        // push an actual break to the output. Takes care of setting flags for tab
        void pushBreak(int offset, float width, MinikinExtent extent, uint8_t hyphenEdit);

        // Hyphenates a string potentially containing non-breaking spaces.
        std::vector<HyphenationType> hyphenate(const uint16_t* str, size_t len);

        float getSpaceWidth() const;

        void computeBreaksGreedy();

        void computeBreaksOptimal();

        void finishBreaksOptimal();

        std::unique_ptr<WordBreaker> mWordBreaker;
        icu::Locale mLocale;
        std::vector<uint16_t> mTextBuf;
        std::vector<float> mCharWidths;
        std::vector<MinikinExtent> mCharExtents;
        std::vector<LayoutOverhang> mCharOverhangs;

        Hyphenator* mHyphenator;

        // layout parameters
        BreakStrategy mStrategy = kBreakStrategy_Greedy;
        HyphenationFrequency mHyphenationFrequency = kHyphenationFrequency_Normal;
        bool mJustified;
        TabStops mTabStops;

        // result of line breaking
        std::vector<int> mBreaks;
        std::vector<float> mWidths;
        std::vector<float> mAscents;
        std::vector<float> mDescents;
        std::vector<int> mFlags;

        ParaWidth mWidth = 0; // Total width of text seen, assuming no line breaks
        std::vector<Candidate> mCandidates;
        float mLinePenalty = 0.0f;

        // the following are state for greedy breaker (updated while adding style runs)
        size_t mLastBreak;
        size_t mBestBreak;
        float mBestScore;
        ParaWidth mPreBreak;  // prebreak of last break
        uint32_t mLastHyphenation;  // hyphen edit of last break kept for next line
        int mFirstTabIndex; // The index of the first tab character seen in input text
        size_t mSpaceCount; // Number of word spaces seen in the input text

        std::unique_ptr<LineWidthDelegate> mLineWidthDelegate;

        FRIEND_TEST(LineBreakerTest, setLocales);
};

}  // namespace minikin

#endif  // MINIKIN_LINE_BREAKER_H
