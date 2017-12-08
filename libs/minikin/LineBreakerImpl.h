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

#ifndef MINIKIN_LINE_BREAKER_IMPL_H
#define MINIKIN_LINE_BREAKER_IMPL_H

#include <deque>
#include <vector>

#include "minikin/FontCollection.h"
#include "minikin/Layout.h"
#include "minikin/LineBreaker.h"
#include "minikin/Macros.h"
#include "minikin/MeasuredText.h"
#include "minikin/MinikinFont.h"
#include "minikin/Range.h"
#include "minikin/U16StringPiece.h"

namespace minikin {

class LineBreakerImpl {
public:
    LineBreakerImpl(const U16StringPiece& textBuffer, BreakStrategy strategy,
                    HyphenationFrequency frequency, bool justified);

    virtual ~LineBreakerImpl();

    const static int kTab_Shift = 29;  // keep synchronized with TAB_MASK in StaticLayout.java

    LineBreakResult computeBreaks(const MeasuredText& measuredText, const LineWidth& lineWidth,
                                  const TabStops& tabStops);

protected:
    // For testing purposes.
    LineBreakerImpl(std::unique_ptr<WordBreaker>&& breaker, const U16StringPiece& textBuffer,
                    BreakStrategy strategy, HyphenationFrequency frequency, bool justified);

private:
    // ParaWidth is used to hold cumulative width from beginning of paragraph. Note that for
    // very large paragraphs, accuracy could degrade using only 32-bit float. Note however
    // that float is used extensively on the Java side for this. This is a typedef so that
    // we can easily change it based on performance/accuracy tradeoff.
    typedef double ParaWidth;

    // A single candidate break
    struct Candidate {
        size_t offset;  // offset to text buffer, in code units

        ParaWidth preBreak;   // width of text until this point, if we decide to not break here:
                              // preBreak is used as an optimized way to calculate the width
                              // between two candidates. The line width between two line break
                              // candidates i and j is calculated as postBreak(j) - preBreak(i).
        ParaWidth postBreak;  // width of text until this point, if we decide to break here

        float firstOverhang;   // amount of overhang needed on the end of the line if we decide
                               // to break here
        float secondOverhang;  // amount of overhang needed on the beginning of the next line
                               // if we decide to break here

        float penalty;          // penalty of this break (for example, hyphen penalty)
        size_t preSpaceCount;   // preceding space count before breaking
        size_t postSpaceCount;  // preceding space count after breaking
        HyphenationType hyphenType;
        bool isRtl;  // The direction of the bidi run containing or ending in this candidate
    };

    void addRuns(const MeasuredText& measured, const LineWidth& lineWidth,
                 const TabStops& tabStops);

    void setLocaleList(uint32_t localeListId, size_t restartFrom);
    // A locale list ID and locale ID currently used for word iterator and hyphenator.
    uint32_t mCurrentLocaleListId;
    uint64_t mCurrentLocaleId = 0;

    void addHyphenationCandidates(const Run& run, const Range& contextRange, const Range& wordRange,
                                  ParaWidth lastBreakWidth, ParaWidth PostBreak,
                                  size_t postSpaceCount, float hyphenPenalty);

    void addWordBreak(size_t offset, ParaWidth preBreak, ParaWidth postBreak, float firstOverhang,
                      float secondOverhang, size_t preSpaceCount, size_t postSpaceCount,
                      float penalty, HyphenationType hyph, bool isRtl);

    void adjustSecondOverhang(float secondOverhang);

    std::unique_ptr<WordBreaker> mWordBreaker;
    U16StringPiece mTextBuf;

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

    ParaWidth mWidth = 0;                // Total width of text seen, assuming no line breaks
    std::vector<Candidate> mCandidates;  // All line breaking candidates

    static LayoutOverhang computeOverhang(float totalAdvance, const std::vector<float>& advances,
                                          const std::vector<LayoutOverhang>& overhangs, bool isRtl);

    MinikinExtent computeMaxExtent(const MeasuredText& measured, size_t start, size_t end) const;

    //
    // Types, methods, and fields related to the greedy algorithm
    //

    void computeBreaksGreedy(const MeasuredText& measured, const LineWidth& lineWidth);

    // This method is called as a helper to computeBreaksGreedy(), but also when we encounter a
    // tab character, which forces the line breaking algorithm to greedy mode. It computes all
    // the greedy line breaks based on available candidates and returns the preBreak of the last
    // break which would then be used to calculate the width of the tab.
    ParaWidth computeBreaksGreedyPartial(const MeasuredText& measured, const LineWidth& lineWidth);

    // Called by computerBreaksGreedyPartial() on all candidates to determine if the line should
    // be broken at the candidate
    void considerGreedyBreakCandidate(const MeasuredText& measured, size_t candIndex,
                                      const LineWidth& lineWidth);

    // Adds a greedy break to list of line breaks.
    void addGreedyBreak(const MeasuredText& measured, size_t breakIndex);

    // Push an actual break to the output. Takes care of setting flags for tab, etc.
    void pushBreak(int offset, float width, MinikinExtent extent, HyphenEdit hyphenEdit);

    void addDesperateBreaksGreedy(const MeasuredText& measured, ParaWidth existingPreBreak,
                                  size_t start, size_t end, const LineWidth& lineWidth);

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

    void computeBreaksOptimal(const MeasuredText& measured, const LineWidth& lineWidth);

    void addDesperateBreaksOptimal(const MeasuredText& measured, std::vector<Candidate>* out,
                                   ParaWidth existingPreBreak, size_t postSpaceCount, bool isRtl,
                                   size_t start, size_t end);

    void addAllDesperateBreaksOptimal(const MeasuredText& measuredText, const LineWidth& lineWidth);

    // Data used to compute optimal line breaks
    struct OptimalBreaksData {
        float score;        // best score found for this break
        size_t prev;        // index to previous break
        size_t lineNumber;  // the computed line number of the candidate
    };
    void finishBreaksOptimal(const MeasuredText& measured,
                             const std::vector<OptimalBreaksData>& breaksData);

    float getSpaceWidth(const MeasuredText& measured) const;

    float mLinePenalty = 0.0f;
    size_t mSpaceCount;  // Number of word spaces seen in the input text
};

}  // namespace minikin

#endif  // MINIKIN_LINE_BREAKER_H
