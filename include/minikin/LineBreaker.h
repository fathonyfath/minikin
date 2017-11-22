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

#include <deque>
#include <vector>

#include "minikin/FontCollection.h"
#include "minikin/Layout.h"
#include "minikin/Macros.h"
#include "minikin/MinikinFont.h"
#include "minikin/Range.h"
#include "minikin/U16StringPiece.h"

namespace minikin {

enum class BreakStrategy : uint8_t {
    Greedy = 0,
    HighQuality = 1,
    Balanced = 2,
};

enum class HyphenationFrequency : uint8_t {
    None = 0,
    Normal = 1,
    Full = 2,
};

class Hyphenator;
class WordBreaker;

class Run {
    public:
        Run(const Range& range) : mRange(range) {}
        virtual ~Run() {}

        // Returns true if this run is RTL. Otherwise returns false.
        virtual bool isRtl() const = 0;

        // Returns true if this run is a target of hyphenation. Otherwise return false.
        virtual bool canHyphenate() const = 0;

        // Returns the locale list ID for this run.
        virtual uint32_t getLocaleListId() const = 0;

        // Fills the each character's advances, extents and overhangs.
        virtual void getMetrics(const U16StringPiece& text, float* advances, MinikinExtent* extents,
                                LayoutOverhang* overhangs) const = 0;

        // Following two methods are only called when the implementation returns true for
        // canHyphenate method.

        // Returns the paint pointer used for this run. Do not return null if you returns true for
        // canHyphenate method.
        virtual const MinikinPaint* getPaint() const { return nullptr; }

        // Measure the hyphenation piece and fills the each character's advances and overhangs.
        virtual float measureHyphenPiece(const U16StringPiece&, const Range&, StartHyphenEdit,
                                         EndHyphenEdit, float*, LayoutOverhang*) const {
            return 0.0;
        }

        inline const Range& getRange() const { return mRange; }

    protected:
        const Range mRange;
};

class TabStops {
    public:
        // Caller must free stops. stops can be nullprt.
        TabStops(const int32_t* stops, size_t nStops, int32_t tabWidth)
            : mStops(stops), mStopsSize(nStops), mTabWidth(tabWidth) { }

        float nextTab(float widthSoFar) const {
            for (size_t i = 0; i < mStopsSize; i++) {
                if (mStops[i] > widthSoFar) {
                    return mStops[i];
                }
            }
            return floor(widthSoFar / mTabWidth + 1) * mTabWidth;
        }
    private:
        const int32_t* mStops;
        size_t mStopsSize;
        int32_t mTabWidth;
};

// Implement this for the additional information during line breaking.
// The functions in this class's interface may be called several times. The implementation
// must return the same value for the same input.
class LineWidth {
    public:
        virtual ~LineWidth() {}

        // Called to find out the width for the line.
        virtual float getAt(size_t lineNo) const = 0;

        // Called to find out the minimum line width.
        virtual float getMin() const = 0;

        // Called to find out the available left-side padding for the line.
        virtual float getLeftPaddingAt(size_t lineNo) const = 0;

        // Called to find out the available right-side padding for the line.
        virtual float getRightPaddingAt(size_t lineNo) const = 0;
};

class MeasuredText {
public:
    static MeasuredText generate(const U16StringPiece& text,
                                 const std::vector<std::unique_ptr<Run>>& runs);

    // Following three vectors have the same length.
    // TODO: Introduce individual character info struct if copy cost in JNI is negligible.
    std::vector<float> widths;
    std::vector<MinikinExtent> extents;
    std::vector<LayoutOverhang> overhangs;

    MeasuredText(MeasuredText&&) = default;
    MeasuredText& operator=(MeasuredText&&) = default;

    PREVENT_COPY_AND_ASSIGN(MeasuredText);
private:
    // Use generate method instead.
    MeasuredText(uint32_t size) : widths(size), extents(size), overhangs(size) {}
};

struct LineBreakResult {
public:
    LineBreakResult() = default;

    // Following five vectors have the same length.
    // TODO: Introduce individual line info struct if copy cost in JNI is negligible.
    std::vector<int> breakPoints;
    std::vector<float> widths;
    std::vector<float> ascents;
    std::vector<float> descents;
    std::vector<int> flags;

    LineBreakResult(LineBreakResult&&) = default;
    LineBreakResult& operator=(LineBreakResult&&) = default;

    PREVENT_COPY_AND_ASSIGN(LineBreakResult);
};

class LineBreaker {
    public:
        LineBreaker(const U16StringPiece& textBuffer, const MeasuredText& measuredText,
                    BreakStrategy strategy, HyphenationFrequency frequency, bool justified);

        virtual ~LineBreaker();

        const static int kTab_Shift = 29;  // keep synchronized with TAB_MASK in StaticLayout.java

        LineBreakResult computeBreaks(const std::vector<std::unique_ptr<Run>>& runs,
                                      const LineWidth& lineWidth, const TabStops& tabStops);

    protected:
        // For testing purposes.
        LineBreaker(std::unique_ptr<WordBreaker>&& breaker, const U16StringPiece& textBuffer,
                    const MeasuredText& measuredText, BreakStrategy strategy,
                    HyphenationFrequency frequency, bool justified);

    private:
        // ParaWidth is used to hold cumulative width from beginning of paragraph. Note that for
        // very large paragraphs, accuracy could degrade using only 32-bit float. Note however
        // that float is used extensively on the Java side for this. This is a typedef so that
        // we can easily change it based on performance/accuracy tradeoff.
        typedef double ParaWidth;

        // A single candidate break
        struct Candidate {
            size_t offset;  // offset to text buffer, in code units

            ParaWidth preBreak;  // width of text until this point, if we decide to not break here:
                                 // preBreak is used as an optimized way to calculate the width
                                 // between two candidates. The line width between two line break
                                 // candidates i and j is calculated as postBreak(j) - preBreak(i).
            ParaWidth postBreak;  // width of text until this point, if we decide to break here

            float firstOverhang;  // amount of overhang needed on the end of the line if we decide
                                  // to break here
            float secondOverhang;  // amount of overhang needed on the beginning of the next line
                                   // if we decide to break here

            float penalty;  // penalty of this break (for example, hyphen penalty)
            size_t preSpaceCount;  // preceding space count before breaking
            size_t postSpaceCount;  // preceding space count after breaking
            HyphenationType hyphenType;
            bool isRtl; // The direction of the bidi run containing or ending in this candidate
        };

        void addRun(const Run& run, const LineWidth& lineWidth, const TabStops& tabStops);

        void setLocaleList(uint32_t localeListId, size_t restartFrom);
        // A locale list ID and locale ID currently used for word iterator and hyphenator.
        uint32_t mCurrentLocaleListId;
        uint64_t mCurrentLocaleId = 0;

        // Hyphenates a string potentially containing non-breaking spaces.
        std::vector<HyphenationType> hyphenate(const U16StringPiece& string);

        void addHyphenationCandidates(const Run& run,
                                      const Range& contextRange,
                                      const Range& wordRange, ParaWidth lastBreakWidth,
                                      ParaWidth PostBreak, size_t postSpaceCount,
                                      float hyphenPenalty);

        void addWordBreak(size_t offset, ParaWidth preBreak, ParaWidth postBreak,
                float firstOverhang, float secondOverhang,
                size_t preSpaceCount, size_t postSpaceCount,
                float penalty, HyphenationType hyph, bool isRtl);

        void adjustSecondOverhang(float secondOverhang);

        std::unique_ptr<WordBreaker> mWordBreaker;
        U16StringPiece mTextBuf;
        const MeasuredText& mMeasuredText;

        const Hyphenator* mHyphenator;

        // layout parameters
        BreakStrategy mStrategy;
        HyphenationFrequency mHyphenationFrequency;
        bool mJustified;

        // result of line breaking
        std::vector<int> mBreaks;
        std::vector<float> mWidths;
        std::vector<float> mAscents;
        std::vector<float> mDescents;
        std::vector<int> mFlags;

        void clearResults() {
            mBreaks.clear();
            mWidths.clear();
            mAscents.clear();
            mDescents.clear();
            mFlags.clear();
        }

        ParaWidth mWidth = 0;  // Total width of text seen, assuming no line breaks
        std::vector<Candidate> mCandidates;  // All line breaking candidates

        static LayoutOverhang computeOverhang(float totalAdvance,
                const std::vector<float>& advances, const std::vector<LayoutOverhang>& overhangs,
                bool isRtl);

        MinikinExtent computeMaxExtent(size_t start, size_t end) const;

        //
        // Types, methods, and fields related to the greedy algorithm
        //

        void computeBreaksGreedy(const LineWidth& lineWidth);

        // This method is called as a helper to computeBreaksGreedy(), but also when we encounter a
        // tab character, which forces the line breaking algorithm to greedy mode. It computes all
        // the greedy line breaks based on available candidates and returns the preBreak of the last
        // break which would then be used to calculate the width of the tab.
        ParaWidth computeBreaksGreedyPartial(const LineWidth& lineWidth);

        // Called by computerBreaksGreedyPartial() on all candidates to determine if the line should
        // be broken at the candidate
        void considerGreedyBreakCandidate(size_t candIndex, const LineWidth& lineWidth);

        // Adds a greedy break to list of line breaks.
        void addGreedyBreak(size_t breakIndex);

        // Push an actual break to the output. Takes care of setting flags for tab, etc.
        void pushBreak(int offset, float width, MinikinExtent extent, HyphenEdit hyphenEdit);

        void addDesperateBreaksGreedy(ParaWidth existingPreBreak, size_t start, size_t end,
                                      const LineWidth& lineWidth);

        bool fitsOnCurrentLine(float width, float leftOverhang, float rightOverhang,
                               const LineWidth& lineWidth) const;

        struct GreedyBreak {
            size_t index;
            float penalty;
        };
        // This will hold a list of greedy breaks, with strictly increasing indices and penalties.
        // The top of the list always holds the best break.
        std::deque<GreedyBreak> mBestGreedyBreaks;
        // Return the best greedy break from the top of the queue.
        size_t popBestGreedyBreak();
        // Insert a greedy break in mBestGreedyBreaks.
        void insertGreedyBreakCandidate(size_t index, float penalty);

        // The following are state for greedy breaker. They get updated during calculation of
        // greedy breaks (including when a partial greedy algorithm is run when adding style runs
        // containing tabs).
        size_t mLastGreedyBreakIndex;  // The last greedy break index of mCandidates.
        const Candidate& getLastBreakCandidate() const;
        size_t mLastConsideredGreedyCandidate;  // The index of the last candidate considered
        int mFirstTabIndex;  // The index of the first tab character seen in current line

        // Used to hold a desperate break as the last greedy break
        Candidate mFakeDesperateCandidate;

        //
        // Types, methods, and fields related to the optimal algorithm
        //

        void computeBreaksOptimal(const LineWidth& lineWidth);

        void addDesperateBreaksOptimal(std::vector<Candidate>* out, ParaWidth existingPreBreak,
                size_t postSpaceCount, bool isRtl, size_t start, size_t end);

        void addAllDesperateBreaksOptimal(const LineWidth& lineWidth);

        // Data used to compute optimal line breaks
        struct OptimalBreaksData {
            float score;  // best score found for this break
            size_t prev;  // index to previous break
            size_t lineNumber;  // the computed line number of the candidate
        };
        void finishBreaksOptimal(const std::vector<OptimalBreaksData>& breaksData);

        float getSpaceWidth() const;

        float mLinePenalty = 0.0f;
        size_t mSpaceCount;  // Number of word spaces seen in the input text
};

}  // namespace minikin

#endif  // MINIKIN_LINE_BREAKER_H
