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

#include "LineBreakerImpl.h"

#include <algorithm>
#include <limits>

#include "minikin/Characters.h"
#include "minikin/Layout.h"
#include "minikin/Range.h"
#include "minikin/U16StringPiece.h"

#include "GreedyLineBreaker.h"
#include "HyphenatorMap.h"
#include "LayoutUtils.h"
#include "LineBreakerUtil.h"
#include "Locale.h"
#include "LocaleListCache.h"
#include "MinikinInternal.h"
#include "WordBreaker.h"

namespace minikin {

// Large scores in a hierarchy; we prefer desperate breaks to an overfull line. All these
// constants are larger than any reasonable actual width score.
const float SCORE_INFTY = std::numeric_limits<float>::max();
const float SCORE_OVERFULL = 1e12f;
const float SCORE_DESPERATE = 1e10f;

// Multiplier for hyphen penalty on last line.
const float LAST_LINE_PENALTY_MULTIPLIER = 4.0f;
// Penalty assigned to each line break (to try to minimize number of lines)
// TODO: when we implement full justification (so spaces can shrink and stretch), this is
// probably not the most appropriate method.
const float LINE_PENALTY_MULTIPLIER = 2.0f;

// Penalty assigned to shrinking the whitepsace.
const float SHRINK_PENALTY_MULTIPLIER = 4.0f;

// Maximum amount that spaces can shrink, in justified text.
const float SHRINKABILITY = 1.0 / 3.0;

inline static const LocaleList& getLocaleList(uint32_t localeListId) {
    android::AutoMutex _l(gMinikinLock);
    return LocaleListCache::getById(localeListId);
}

LineBreakerImpl::LineBreakerImpl(const U16StringPiece& textBuffer, BreakStrategy strategy,
                                 HyphenationFrequency frequency, bool justified)
        : LineBreakerImpl(std::make_unique<WordBreaker>(), textBuffer, strategy, frequency,
                          justified) {}

LineBreakerImpl::LineBreakerImpl(std::unique_ptr<WordBreaker>&& breaker,
                                 const U16StringPiece& textBuffer, BreakStrategy strategy,
                                 HyphenationFrequency frequency, bool justified)
        : mCurrentLocaleListId(LocaleListCache::kInvalidListId),
          mWordBreaker(std::move(breaker)),
          mTextBuf(textBuffer),
          mStrategy(strategy),
          mHyphenationFrequency(frequency),
          mJustified(justified),
          mSpaceCount(0) {
    mWordBreaker->setText(mTextBuf.data(), mTextBuf.size());

    // handle initial break here because addStyleRun may never be called
    mCandidates.push_back({0 /* offset */, 0.0 /* preBreak */, 0.0 /* postBreak */,
                           0.0 /* penalty */, 0 /* preSpaceCount */, 0 /* postSpaceCount */,
                           HyphenationType::DONT_BREAK /* hyphenType */,
                           false /* isRtl. TODO: may need to be based on input. */});
}

LineBreakerImpl::~LineBreakerImpl() {}

void LineBreakerImpl::setLocaleList(uint32_t localeListId, size_t restartFrom) {
    if (mCurrentLocaleListId == localeListId) {
        return;
    }

    const LocaleList& newLocales = getLocaleList(localeListId);
    const Locale newLocale = newLocales.empty() ? Locale() : newLocales[0];
    const uint64_t newLocaleId = newLocale.getIdentifier();

    const bool needUpdate =
            // The first time setLocale is called.
            mCurrentLocaleListId == LocaleListCache::kInvalidListId ||
            // The effective locale is changed.
            newLocaleId != mCurrentLocaleId;

    // For now, we ignore all locales except the first valid one.
    // TODO: Support selecting the locale based on the script of the text.
    mCurrentLocaleListId = localeListId;
    mCurrentLocaleId = newLocaleId;
    if (needUpdate) {
        mWordBreaker->followingWithLocale(newLocale, restartFrom);
        mHyphenator = HyphenatorMap::lookup(newLocale);
    }
}

// This adds all the hyphenation candidates for a given word by first finding all the hyphenation
// points and then calling addWordBreak for each.
//
// paint will be used for measuring the text and paint must not be null.
// wordRange is the range for the word.
// contextRange is the range from the last word breakpoint to the first code unit after the word.
// For example, if the word starts with the punctuations or ends with spaces, the contextRange
// contains both punctuations and trailing spaces but wordRange excludes them.
// lastBreakWidth is the width seen until the begining of context range.
// bidiFlags keep the bidi flags to determine the direction of text for layout and other
//     calculations. It may only be Bidi::FORCE_RTL or Bidi::FORCE_LTR.
//
// The following parameters are needed to be passed to addWordBreak:
// postBreak is the width that would be seen if we decide to break at the end of the word (so it
//     doesn't count any line-ending space after the word).
// postSpaceCount is the number of spaces that would be seen if we decide to break at the end of the
//     word (and like postBreak it doesn't count any line-ending space after the word).
// hyphenPenalty is the amount of penalty for hyphenation.
void LineBreakerImpl::addHyphenationCandidates(const Run& run, const Range& contextRange,
                                               const Range& wordRange, ParaWidth lastBreakWidth,
                                               ParaWidth postBreak, size_t postSpaceCount,
                                               float hyphenPenalty) {
    MINIKIN_ASSERT(contextRange.contains(wordRange), "Context must contain word range");

    const bool isRtlWord = run.isRtl();
    const std::vector<HyphenationType> hyphenResult =
            hyphenate(mTextBuf.substr(wordRange), *mHyphenator);

    // measure hyphenated substrings
    for (size_t j : wordRange) {
        HyphenationType hyph = hyphenResult[wordRange.toRangeOffset(j)];
        if (hyph == HyphenationType::DONT_BREAK) {
            continue;
        }

        auto hyphenPart = contextRange.split(j);

        const float firstPartWidth = run.measureHyphenPiece(
                mTextBuf, hyphenPart.first, StartHyphenEdit::NO_EDIT, editForThisLine(hyph),
                nullptr /* advances */, nullptr /* overhang */);
        const ParaWidth hyphPostBreak = lastBreakWidth + firstPartWidth;

        const float secondPartWidth = run.measureHyphenPiece(
                mTextBuf, hyphenPart.second, editForNextLine(hyph), EndHyphenEdit::NO_EDIT,
                nullptr /* advances */, nullptr /* overhangs */);
        // hyphPreBreak is calculated like this so that when the line width for a future line break
        // is being calculated, the width of the whole word would be subtracted and the width of the
        // second part would be added.
        const ParaWidth hyphPreBreak = postBreak - secondPartWidth;

        addWordBreak(j, hyphPreBreak, hyphPostBreak, postSpaceCount, postSpaceCount, hyphenPenalty,
                     hyph, isRtlWord);
    }
}

// This method finds the candidate word breaks (using the ICU break iterator) and sends them
// to addWordBreak.
void LineBreakerImpl::addRuns(const MeasuredText& measured, const LineWidth& lineWidth) {
    for (const auto& run : measured.runs) {
        const bool isRtl = run->isRtl();
        const Range& range = run->getRange();

        const bool canHyphenate = run->canHyphenate();
        float hyphenPenalty = 0.0;
        if (canHyphenate) {
            const MinikinPaint* paint = run->getPaint();
            // a heuristic that seems to perform well
            hyphenPenalty = 0.5 * paint->size * paint->scaleX * lineWidth.getAt(0);
            if (mHyphenationFrequency == HyphenationFrequency::Normal) {
                hyphenPenalty *= 4.0;  // TODO: Replace with a better value after some testing
            }

            if (mJustified) {
                // Make hyphenation more aggressive for fully justified text (so that "normal" in
                // justified mode is the same as "full" in ragged-right).
                hyphenPenalty *= 0.25;
            } else {
                // Line penalty is zero for justified text.
                mLinePenalty = std::max(mLinePenalty, hyphenPenalty * LINE_PENALTY_MULTIPLIER);
            }
        }

        setLocaleList(run->getLocaleListId(), range.getStart());
        size_t current = (size_t)mWordBreaker->current();
        // This will keep the index of last code unit seen that's not a line-ending space, plus one.
        // In other words, the index of the first code unit after a word.

        Range hyphenationContextRange(range.getStart(), range.getStart());
        ParaWidth lastBreakWidth = mWidth;  // The width of the text as of the previous break point.
        ParaWidth postBreak = mWidth;       // The width of text seen if we decide to break here
        // The maximum amount of backward overhang seen since last word.
        size_t postSpaceCount = mSpaceCount;
        const bool hyphenate = canHyphenate && mHyphenationFrequency != HyphenationFrequency::None;
        for (size_t i : range) {
            const uint16_t c = mTextBuf[i];
            MINIKIN_ASSERT(c != CHAR_TAB, "TAB is not supported in optimal line breaking.");
            if (isWordSpace(c)) {
                mSpaceCount += 1;
            }
            mWidth += measured.widths[i];
            if (isLineEndSpace(c)) {
                // If we break a line on a line-ending space, that space goes away. So postBreak
                // and postSpaceCount, which keep the width and number of spaces if we decide to
                // break at this point, don't need to get adjusted.
                //
                // TODO: handle the rare case of line ending spaces having overhang (it can
                // happen for U+1680 OGHAM SPACE MARK).
            } else {
                postBreak = mWidth;
                postSpaceCount = mSpaceCount;
                hyphenationContextRange.setEnd(i + 1);
            }
            if (i + 1 == current) {  // We are at the end of a word.
                // We skip breaks for zero-width characters inside replacement spans.
                const bool addBreak =
                        canHyphenate || current == range.getEnd() || measured.widths[current] > 0;

                if (hyphenate) {
                    const Range wordRange = mWordBreaker->wordRange();
                    if (!wordRange.isEmpty() && range.contains(wordRange)) {
                        addHyphenationCandidates(*run, hyphenationContextRange, wordRange,
                                                 lastBreakWidth, postBreak, postSpaceCount,
                                                 hyphenPenalty);
                    }
                }
                if (addBreak) {
                    const float penalty = hyphenPenalty * mWordBreaker->breakBadness();
                    // TODO: overhangs may need adjustment at bidi boundaries.
                    addWordBreak(current, mWidth /* preBreak */, postBreak, mSpaceCount,
                                 postSpaceCount, penalty, HyphenationType::DONT_BREAK, isRtl);
                }
                hyphenationContextRange = Range(current, current);
                lastBreakWidth = mWidth;
                current = (size_t)mWordBreaker->next();
            }
        }
    }
}

// Add a word break (possibly for a hyphenated fragment).
inline void LineBreakerImpl::addWordBreak(size_t offset, ParaWidth preBreak, ParaWidth postBreak,
                                          size_t preSpaceCount, size_t postSpaceCount,
                                          float penalty, HyphenationType hyph, bool isRtl) {
    mCandidates.push_back(
            {offset, preBreak, postBreak, penalty, preSpaceCount, postSpaceCount, hyph, isRtl});
}

// Find the needed extent between the start and end ranges. start is inclusive and end is exclusive.
// Both are indices of the source string.
MinikinExtent LineBreakerImpl::computeMaxExtent(const MeasuredText& measured, size_t start,
                                                size_t end) const {
    MinikinExtent res = {0.0, 0.0, 0.0};
    for (size_t j = start; j < end; j++) {
        res.extendBy(measured.extents[j]);
    }
    return res;
}

// Get the width of a space. May return 0 if there are no spaces.
// Note: if there are multiple different widths for spaces (for example, because of mixing of
// fonts), it's only guaranteed to pick one.
float LineBreakerImpl::getSpaceWidth(const MeasuredText& measured) const {
    for (size_t i = 0; i < mTextBuf.size(); i++) {
        if (isWordSpace(mTextBuf[i])) {
            return measured.widths[i];
        }
    }
    return 0.0f;
}

// Add desperate breaks for the optimal algorithm.
// Note: these breaks are based on the shaping of the (non-broken) original text; they
// are imprecise especially in the presence of kerning, ligatures, overhangs, and Arabic shaping.
void LineBreakerImpl::addDesperateBreaksOptimal(const MeasuredText& measured,
                                                std::vector<Candidate>* out,
                                                ParaWidth existingPreBreak, size_t postSpaceCount,
                                                bool isRtl, size_t start, size_t end) {
    ParaWidth width = existingPreBreak + measured.widths[start];
    for (size_t i = start + 1; i < end; i++) {
        const float w = measured.widths[i];
        if (w > 0) {  // Add desperate breaks only before grapheme clusters.
            out->push_back({i /* offset */, width /* preBreak */, width /* postBreak */,
                            SCORE_DESPERATE /* penalty */,
                            // postSpaceCount doesn't include trailing spaces.
                            postSpaceCount /* preSpaceCount */, postSpaceCount /* postSpaceCount */,
                            HyphenationType::BREAK_AND_DONT_INSERT_HYPHEN /* hyphenType */,
                            isRtl /* isRtl */});
            width += w;
        }
    }
}

void LineBreakerImpl::addAllDesperateBreaksOptimal(const MeasuredText& measured,
                                                   const LineWidth& lineWidth) {
    const ParaWidth minLineWidth = lineWidth.getMin();
    size_t firstDesperateIndex = 0;
    const size_t nCand = mCandidates.size();
    for (size_t i = 1; i < nCand; i++) {
        const ParaWidth requiredWidth = mCandidates[i].postBreak - mCandidates[i - 1].preBreak;
        if (requiredWidth > minLineWidth) {
            // A desperate break is needed.
            firstDesperateIndex = i;
            break;
        }
    }
    if (firstDesperateIndex == 0) {  // No desperate breaks needed.
        return;
    }

    // This temporary holds an expanded list of candidates, which will be later copied back into
    // mCandidates. The beginning of mCandidates, where there are no desparate breaks, is skipped.
    std::vector<Candidate> expandedCandidates;
    const size_t nRemainingCandidates = nCand - firstDesperateIndex;
    expandedCandidates.reserve(nRemainingCandidates + 1);  // At least one more is needed.
    for (size_t i = firstDesperateIndex; i < nCand; i++) {
        const Candidate& previousCand = mCandidates[i - 1];
        const Candidate& thisCand = mCandidates[i];
        const ParaWidth requiredWidth = thisCand.postBreak - previousCand.preBreak;
        if (requiredWidth > minLineWidth) {
            addDesperateBreaksOptimal(measured, &expandedCandidates, previousCand.preBreak,
                                      thisCand.postSpaceCount, thisCand.isRtl,
                                      previousCand.offset /* start */, thisCand.offset /* end */);
        }
        expandedCandidates.push_back(thisCand);
    }

    mCandidates.reserve(firstDesperateIndex + expandedCandidates.size());
    // Iterator to the first candidate to insert from expandedCandidates. The candidates before this
    // would simply be copied.
    auto firstToInsert = expandedCandidates.begin() + nRemainingCandidates;
    // Copy until the end of mCandidates.
    std::copy(expandedCandidates.begin(), firstToInsert, mCandidates.begin() + firstDesperateIndex);
    // Insert the rest.
    mCandidates.insert(mCandidates.end(), firstToInsert, expandedCandidates.end());
}

// Follow "prev" links in mCandidates array, and copy to result arrays.
void LineBreakerImpl::finishBreaksOptimal(const MeasuredText& measured,
                                          const std::vector<OptimalBreaksData>& breaksData) {
    // clear output vectors.
    clearResults();

    const size_t nCand = mCandidates.size();
    size_t prev;
    for (size_t i = nCand - 1; i > 0; i = prev) {
        prev = breaksData[i].prev;
        mBreaks.push_back(mCandidates[i].offset);
        mWidths.push_back(mCandidates[i].postBreak - mCandidates[prev].preBreak);
        MinikinExtent extent =
                computeMaxExtent(measured, mCandidates[prev].offset, mCandidates[i].offset);
        mAscents.push_back(extent.ascent);
        mDescents.push_back(extent.descent);

        const HyphenEdit edit =
                packHyphenEdit(prev == 0 ? StartHyphenEdit::NO_EDIT
                                         : editForNextLine(mCandidates[prev].hyphenType),
                               editForThisLine(mCandidates[i].hyphenType));
        mFlags.push_back(static_cast<int>(edit));
    }
    std::reverse(mBreaks.begin(), mBreaks.end());
    std::reverse(mWidths.begin(), mWidths.end());
    std::reverse(mFlags.begin(), mFlags.end());
}

void LineBreakerImpl::computeBreaksOptimal(const MeasuredText& measured,
                                           const LineWidth& lineWidth) {
    size_t active = 0;
    const size_t nCand = mCandidates.size();
    const float maxShrink = mJustified ? SHRINKABILITY * getSpaceWidth(measured) : 0.0f;

    std::vector<OptimalBreaksData> breaksData;
    breaksData.reserve(nCand);
    breaksData.push_back({0.0, 0, 0});  // The first candidate is always at the first line.

    // "i" iterates through candidates for the end of the line.
    for (size_t i = 1; i < nCand; i++) {
        const bool atEnd = i == nCand - 1;
        float best = SCORE_INFTY;
        size_t bestPrev = 0;

        size_t lineNumberLast = breaksData[active].lineNumber;
        float width = lineWidth.getAt(lineNumberLast);

        ParaWidth leftEdge = mCandidates[i].postBreak - width;
        float bestHope = 0;

        // "j" iterates through candidates for the beginning of the line.
        for (size_t j = active; j < i; j++) {
            const size_t lineNumber = breaksData[j].lineNumber;
            if (lineNumber != lineNumberLast) {
                const float widthNew = lineWidth.getAt(lineNumber);
                if (widthNew != width) {
                    leftEdge = mCandidates[i].postBreak - width;
                    bestHope = 0;
                    width = widthNew;
                }
                lineNumberLast = lineNumber;
            }
            const float jScore = breaksData[j].score;
            if (jScore + bestHope >= best) continue;
            const float delta = mCandidates[j].preBreak - leftEdge;

            // compute width score for line

            // Note: the "bestHope" optimization makes the assumption that, when delta is
            // non-negative, widthScore will increase monotonically as successive candidate
            // breaks are considered.
            float widthScore = 0.0f;
            float additionalPenalty = 0.0f;
            if ((atEnd || !mJustified) && delta < 0) {
                widthScore = SCORE_OVERFULL;
            } else if (atEnd && mStrategy != BreakStrategy::Balanced) {
                // increase penalty for hyphen on last line
                additionalPenalty = LAST_LINE_PENALTY_MULTIPLIER * mCandidates[j].penalty;
            } else {
                widthScore = delta * delta;
                if (delta < 0) {
                    if (-delta < maxShrink * (mCandidates[i].postSpaceCount -
                                              mCandidates[j].preSpaceCount)) {
                        widthScore *= SHRINK_PENALTY_MULTIPLIER;
                    } else {
                        widthScore = SCORE_OVERFULL;
                    }
                }
            }

            if (delta < 0) {
                active = j + 1;
            } else {
                bestHope = widthScore;
            }

            const float score = jScore + widthScore + additionalPenalty;
            if (score <= best) {
                best = score;
                bestPrev = j;
            }
        }
        breaksData.push_back({best + mCandidates[i].penalty + mLinePenalty,  // score
                              bestPrev,                                      // prev
                              breaksData[bestPrev].lineNumber + 1});         // lineNumber
    }
    finishBreaksOptimal(measured, breaksData);
}

LineBreakResult LineBreakerImpl::computeBreaks(const MeasuredText& measured,
                                               const LineWidth& lineWidth) {
    if (mTextBuf.size() == 0) {
        return LineBreakResult();
    }
    addRuns(measured, lineWidth);
    addAllDesperateBreaksOptimal(measured, lineWidth);
    computeBreaksOptimal(measured, lineWidth);
    LineBreakResult result;
    result.breakPoints = std::move(mBreaks);
    result.widths = std::move(mWidths);
    result.ascents = std::move(mAscents);
    result.descents = std::move(mDescents);
    result.flags = std::move(mFlags);
    return result;
}

LineBreakResult breakIntoLines(const U16StringPiece& textBuffer, BreakStrategy strategy,
                               HyphenationFrequency frequency, bool justified,
                               const MeasuredText& measuredText, const LineWidth& lineWidth,
                               const TabStops& tabStops) {
    if (strategy == BreakStrategy::Greedy || textBuffer.hasChar(CHAR_TAB)) {
        return breakLineGreedy(textBuffer, measuredText, lineWidth, tabStops,
                               frequency != HyphenationFrequency::None);
    } else {
        LineBreakerImpl impl(textBuffer, strategy, frequency, justified);
        return impl.computeBreaks(measuredText, lineWidth);
    }
}

}  // namespace minikin
