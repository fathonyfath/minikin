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

#include "minikin/LineBreaker.h"

#include <algorithm>
#include <limits>

#include "minikin/Characters.h"
#include "minikin/Layout.h"
#include "minikin/Range.h"
#include "minikin/U16StringPiece.h"
#include "HyphenatorMap.h"
#include "LayoutUtils.h"
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

// Very long words trigger O(n^2) behavior in hyphenation, so we disable hyphenation for
// unreasonably long words. This is somewhat of a heuristic because extremely long words
// are possible in some languages. This does mean that very long real words can get
// broken by desperate breaks, with no hyphens.
const size_t LONGEST_HYPHENATED_WORD = 45;

// Maximum amount that spaces can shrink, in justified text.
const float SHRINKABILITY = 1.0 / 3.0;

constexpr size_t LAST_BREAK_OFFSET_NOWHERE = SIZE_MAX;
constexpr size_t LAST_BREAK_OFFSET_DESPERATE = LAST_BREAK_OFFSET_NOWHERE - 1;

inline static const LocaleList& getLocaleList(uint32_t localeListId) {
    android::AutoMutex _l(gMinikinLock);
    return LocaleListCache::getById(localeListId);
}

MeasuredText MeasuredText::generate(const U16StringPiece& text,
                                    const std::vector<std::unique_ptr<Run>>& runs) {
    MeasuredText result(text.size());
    for (const auto& run : runs) {
        const uint32_t runOffset = run->getRange().getStart();
        run->getMetrics(text, result.widths.data() + runOffset,
                        result.extents.data() + runOffset, result.overhangs.data() + runOffset);
    }
    return result;
}

LineBreaker::LineBreaker(const U16StringPiece& textBuffer, const MeasuredText& measuredText,
                         BreakStrategy strategy, HyphenationFrequency frequency, bool justified)
        : LineBreaker(std::make_unique<WordBreaker>(), textBuffer, measuredText, strategy,
                      frequency, justified) {}

LineBreaker::LineBreaker(std::unique_ptr<WordBreaker>&& breaker, const U16StringPiece& textBuffer,
                         const MeasuredText& measuredText, BreakStrategy strategy,
                         HyphenationFrequency frequency, bool justified)
        : mCurrentLocaleListId(LocaleListCache::kInvalidListId),
        mWordBreaker(std::move(breaker)),
        mTextBuf(textBuffer),
        mMeasuredText(measuredText),
        mStrategy(strategy),
        mHyphenationFrequency(frequency),
        mJustified(justified),
        mLastConsideredGreedyCandidate(LAST_BREAK_OFFSET_NOWHERE),
        mSpaceCount(0) {

    mWordBreaker->setText(mTextBuf.data(), mTextBuf.size());

    // handle initial break here because addStyleRun may never be called
    mCandidates.push_back({
            0 /* offset */, 0.0 /* preBreak */, 0.0 /* postBreak */,
            0.0 /* firstOverhang */, 0.0 /* secondOverhang */,
            0.0 /* penalty */,
            0 /* preSpaceCount */, 0 /* postSpaceCount */,
            HyphenationType::DONT_BREAK /* hyphenType */,
            false /* isRtl. TODO: may need to be based on input. */});
}

LineBreaker::~LineBreaker() {}

const LineBreaker::Candidate& LineBreaker::getLastBreakCandidate() const {
    MINIKIN_ASSERT(mLastGreedyBreakIndex != LAST_BREAK_OFFSET_NOWHERE,
                   "Line break hasn't started.");
    return mLastGreedyBreakIndex == LAST_BREAK_OFFSET_DESPERATE
            ?  mFakeDesperateCandidate : mCandidates[mLastGreedyBreakIndex];
}

void LineBreaker::setLocaleList(uint32_t localeListId, size_t restartFrom) {
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

// This function determines whether a character is a space that disappears at end of line.
// It is the Unicode set: [[:General_Category=Space_Separator:]-[:Line_Break=Glue:]],
// plus '\n'.
// Note: all such characters are in the BMP, so it's ok to use code units for this.
static bool isLineEndSpace(uint16_t c) {
    return c == '\n'
            || c == ' '      // SPACE
            || c == 0x1680   // OGHAM SPACE MARK
            || (0x2000 <= c && c <= 0x200A && c != 0x2007) // EN QUAD, EM QUAD, EN SPACE, EM SPACE,
                                                           // THREE-PER-EM SPACE, FOUR-PER-EM SPACE,
                                                           // SIX-PER-EM SPACE, PUNCTUATION SPACE,
                                                           // THIN SPACE, HAIR SPACE
            || c == 0x205F   // MEDIUM MATHEMATICAL SPACE
            || c == 0x3000;  // IDEOGRAPHIC SPACE
}

// Hyphenates a string potentially containing non-breaking spaces.
std::vector<HyphenationType> LineBreaker::hyphenate(const U16StringPiece& str) {
    std::vector<HyphenationType> out;
    const size_t len = str.size();
    out.reserve(len);

    // A word here is any consecutive string of non-NBSP characters.
    bool inWord = false;
    size_t wordStart = 0; // The initial value will never be accessed, but just in case.
    for (size_t i = 0; i <= len; i++) {
        if (i == len || str[i] == CHAR_NBSP) {
            if (inWord) {
                // A word just ended. Hyphenate it.
                const size_t wordLen = i - wordStart;
                if (wordLen <= LONGEST_HYPHENATED_WORD) {
                    if (wordStart == 0) {
                        // The string starts with a word. Use out directly.
                        mHyphenator->hyphenate(&out, str.data(), wordLen);
                    } else {
                        std::vector<HyphenationType> wordVec;
                        mHyphenator->hyphenate(&wordVec, str.data() + wordStart, wordLen);
                        out.insert(out.end(), wordVec.cbegin(), wordVec.cend());
                    }
                } else { // Word is too long. Inefficient to hyphenate.
                    out.insert(out.end(), wordLen, HyphenationType::DONT_BREAK);
                }
                inWord = false;
            }
            if (i < len) {
                // Insert one DONT_BREAK for the NBSP.
                out.push_back(HyphenationType::DONT_BREAK);
            }
        } else if (!inWord) {
            inWord = true;
            wordStart = i;
        }
    }
    return out;
}

// Compute the total overhang of text based on per-cluster advances and overhangs.
// The two input vectors are expected to be of the same size.
/* static */ LayoutOverhang LineBreaker::computeOverhang(float totalAdvance,
        const std::vector<float>& advances, const std::vector<LayoutOverhang>& overhangs,
        bool isRtl) {
    ParaWidth left = 0.0;
    ParaWidth right = 0.0;
    ParaWidth seenAdvance = 0.0;
    const size_t len = advances.size();
    if (isRtl) {
        for (size_t i = 0; i < len; i++) {
            right = std::max(right, overhangs[i].right - seenAdvance);
            seenAdvance += advances[i];
            left = std::max(left, overhangs[i].left - (totalAdvance - seenAdvance));
        }
    } else {
        for (size_t i = 0; i < len; i++) {
            left = std::max(left, overhangs[i].left - seenAdvance);
            seenAdvance += advances[i];
            right = std::max(right, overhangs[i].right - (totalAdvance - seenAdvance));
        }
    }
    return {static_cast<float>(left), static_cast<float>(right)};
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
void LineBreaker::addHyphenationCandidates(
        const Run& run,
        const Range& contextRange,
        const Range& wordRange,
        ParaWidth lastBreakWidth,
        ParaWidth postBreak,
        size_t postSpaceCount,
        float hyphenPenalty) {
    MINIKIN_ASSERT(contextRange.contains(wordRange), "Context must contain word range");

    const bool isRtlWord = run.isRtl();
    const std::vector<HyphenationType> hyphenResult = hyphenate(mTextBuf.substr(wordRange));

    std::vector<float> advances;
    std::vector<LayoutOverhang> overhangs;
    advances.reserve(contextRange.getLength());
    overhangs.reserve(contextRange.getLength());
    // measure hyphenated substrings
    for (size_t j : wordRange) {
        HyphenationType hyph = hyphenResult[wordRange.toRangeOffset(j)];
        if (hyph == HyphenationType::DONT_BREAK) {
            continue;
        }

        auto hyphenPart = contextRange.split(j);
        const Range& firstPart = hyphenPart.first;
        const Range& secondPart = hyphenPart.second;

        const size_t firstPartLen = firstPart.getLength();
        advances.resize(firstPartLen);
        overhangs.resize(firstPartLen);
        const float firstPartWidth = run.measureHyphenPiece(
                mTextBuf, firstPart, StartHyphenEdit::NO_EDIT, editForThisLine(hyph),
                advances.data(), overhangs.data());
        const ParaWidth hyphPostBreak = lastBreakWidth + firstPartWidth;
        LayoutOverhang overhang = computeOverhang(firstPartWidth, advances, overhangs, isRtlWord);
        // TODO: This ignores potential overhang from a previous word, e.g. in "R table" if the
        // right overhang of the R is larger than the advance of " ta-". In such cases, we need to
        // take the existing overhang into account.
        const float firstOverhang = isRtlWord ? overhang.left : overhang.right;

        const size_t secondPartLen = secondPart.getLength();
        advances.resize(secondPartLen);
        overhangs.resize(secondPartLen);
        const float secondPartWidth = run.measureHyphenPiece(
                mTextBuf, secondPart, editForNextLine(hyph), EndHyphenEdit::NO_EDIT,
                advances.data(), overhangs.data());
        // hyphPreBreak is calculated like this so that when the line width for a future line break
        // is being calculated, the width of the whole word would be subtracted and the width of the
        // second part would be added.
        const ParaWidth hyphPreBreak = postBreak - secondPartWidth;
        overhang = computeOverhang(secondPartWidth, advances, overhangs, isRtlWord);
        const float secondOverhang = isRtlWord ? overhang.right : overhang.left;

        addWordBreak(j, hyphPreBreak, hyphPostBreak, firstOverhang, secondOverhang,
                postSpaceCount, postSpaceCount, hyphenPenalty, hyph, isRtlWord);
    }
}

// This method finds the candidate word breaks (using the ICU break iterator) and sends them
// to addWordBreak.
void LineBreaker::addRun(const Run& run, const LineWidth& lineWidth, const TabStops& tabStops) {
    const bool isRtl = run.isRtl();
    const Range& range = run.getRange();

    const bool canHyphenate = run.canHyphenate();
    float hyphenPenalty = 0.0;
    if (canHyphenate) {
        const MinikinPaint* paint = run.getPaint();
        // a heuristic that seems to perform well
        hyphenPenalty = 0.5 * paint->size * paint->scaleX * lineWidth.getAt(0);
        if (mHyphenationFrequency == HyphenationFrequency::Normal) {
            hyphenPenalty *= 4.0; // TODO: Replace with a better value after some testing
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

    setLocaleList(run.getLocaleListId(), range.getStart());
    size_t current = (size_t) mWordBreaker->current();
    // This will keep the index of last code unit seen that's not a line-ending space, plus one.
    // In other words, the index of the first code unit after a word.

    Range hyphenationContextRange(range.getStart(), range.getStart());
    ParaWidth lastBreakWidth = mWidth; // The width of the text as of the previous break point.
    ParaWidth postBreak = mWidth; // The width of text seen if we decide to break here
    // postBreak plus potential forward overhang. Guaranteed to be >= postBreak.
    ParaWidth postBreakWithOverhang = mWidth;
    // The maximum amount of backward overhang seen since last word.
    float maxBackwardOverhang = 0;
    size_t postSpaceCount = mSpaceCount;
    const bool hyphenate = canHyphenate && mHyphenationFrequency != HyphenationFrequency::None;
    for (size_t i : range) {
        const uint16_t c = mTextBuf[i];
        if (c == CHAR_TAB) {
            // Fall back to greedy; other modes don't know how to deal with tabs.
            mStrategy = BreakStrategy::Greedy;
            // In order to figure out the actual width of the tab, we need to run the greedy
            // algorithm on all previous text and determine the last line break's preBreak.
            const ParaWidth lastPreBreak = computeBreaksGreedyPartial(lineWidth);
            mWidth = lastPreBreak + tabStops.nextTab(mWidth - lastPreBreak);
            if (mFirstTabIndex == INT_MAX) {
                mFirstTabIndex = static_cast<int>(i);
            }
            // No need to update afterWord since tab characters can not be an end of word character
            // in WordBreaker. See the implementation of WordBreaker::wordEnd.
        } else {
            if (isWordSpace(c)) {
                mSpaceCount += 1;
            }
            mWidth += mMeasuredText.widths[i];
            if (isLineEndSpace(c)) {
                // If we break a line on a line-ending space, that space goes away. So postBreak
                // and postSpaceCount, which keep the width and number of spaces if we decide to
                // break at this point, don't need to get adjusted.
                //
                // TODO: handle the rare case of line ending spaces having overhang (it can happen
                // for U+1680 OGHAM SPACE MARK).
            } else {
                postBreak = mWidth;
                postSpaceCount = mSpaceCount;
                hyphenationContextRange.setEnd(i + 1);

                // TODO: This doesn't work for very tight lines and large overhangs, where the
                // overhang from a previous word that may end up on an earline line may be
                // considered still in effect for a later word. But that's expected to be very rare,
                // so we ignore it for now.
                const float forwardOverhang =
                        isRtl ? mMeasuredText.overhangs[i].left : mMeasuredText.overhangs[i].right;
                postBreakWithOverhang = std::max(postBreakWithOverhang,
                                                 postBreak + forwardOverhang);

                float backwardOverhang =
                        isRtl ? mMeasuredText.overhangs[i].right : mMeasuredText.overhangs[i].left;
                // Adjust backwardOverhang by the advance already seen from the last break.
                backwardOverhang -= (mWidth - mMeasuredText.widths[i]) - lastBreakWidth;
                maxBackwardOverhang = std::max(maxBackwardOverhang, backwardOverhang);
            }
        }
        if (i + 1 == current) { // We are at the end of a word.
            // We skip breaks for zero-width characters inside replacement spans.
            const bool addBreak = canHyphenate || current == range.getEnd() ||
                    mMeasuredText.widths[current] > 0;

            if (addBreak) {
                // adjust second overhang for previous breaks
                adjustSecondOverhang(maxBackwardOverhang);
            }
            if (hyphenate) {
                const Range wordRange = mWordBreaker->wordRange();
                if (!wordRange.isEmpty() && range.contains(wordRange)) {
                    addHyphenationCandidates(run, hyphenationContextRange, wordRange,
                            lastBreakWidth, postBreak, postSpaceCount, hyphenPenalty);
                }
            }
            if (addBreak) {
                const float penalty = hyphenPenalty * mWordBreaker->breakBadness();
                // TODO: overhangs may need adjustment at bidi boundaries.
                addWordBreak(current, mWidth /* preBreak */, postBreak,
                        postBreakWithOverhang - postBreak /* firstOverhang */,
                        0.0 /* secondOverhang, to be adjusted later */,
                        mSpaceCount, postSpaceCount,
                        penalty, HyphenationType::DONT_BREAK, isRtl);
            }
            hyphenationContextRange = Range(current, current);
            lastBreakWidth = mWidth;
            maxBackwardOverhang = 0;
            current = (size_t)mWordBreaker->next();
        }
    }
}

// Add desperate breaks for the greedy algorithm.
// Note: these breaks are based on the shaping of the (non-broken) original text; they
// are imprecise especially in the presence of kerning, ligatures, overhangs, and Arabic shaping.
void LineBreaker::addDesperateBreaksGreedy(ParaWidth existingPreBreak, size_t start, size_t end,
                                           const LineWidth& lineWidth) {
    ParaWidth width = mMeasuredText.widths[start];
    for (size_t i = start + 1; i < end; i++) {
        const float w = mMeasuredText.widths[i];
        if (w > 0) { // Add desperate breaks only before grapheme clusters.
            const ParaWidth newWidth = width + w;
            if (!fitsOnCurrentLine(newWidth, 0.0, 0.0, lineWidth)) {
                const Candidate& lastGreedyBreak = getLastBreakCandidate();
                constexpr HyphenationType hyphen = HyphenationType::BREAK_AND_DONT_INSERT_HYPHEN;
                pushBreak(i, width, computeMaxExtent(lastGreedyBreak.offset, i),
                          packHyphenEdit(editForNextLine(lastGreedyBreak.hyphenType),
                                         editForThisLine(hyphen)));

                existingPreBreak += width;
                // Only set the fields that will be later read.
                mFakeDesperateCandidate.offset = i;
                mFakeDesperateCandidate.preBreak = existingPreBreak;
                mFakeDesperateCandidate.secondOverhang = 0.0;
                mFakeDesperateCandidate.hyphenType = hyphen;
                mLastGreedyBreakIndex = LAST_BREAK_OFFSET_DESPERATE;

                width = w;
            } else {
                width = newWidth;
            }
        }
    }
}

// Add a word break (possibly for a hyphenated fragment).
inline void LineBreaker::addWordBreak(size_t offset, ParaWidth preBreak, ParaWidth postBreak,
        float firstOverhang, float secondOverhang, size_t preSpaceCount, size_t postSpaceCount,
        float penalty, HyphenationType hyph, bool isRtl) {
    mCandidates.push_back({offset, preBreak, postBreak, firstOverhang, secondOverhang, penalty,
            preSpaceCount, postSpaceCount, hyph, isRtl});
}

// Go back and adjust earlier line breaks if needed.
void LineBreaker::adjustSecondOverhang(float secondOverhang) {
    const size_t lastCand = mCandidates.size() - 1;
    const ParaWidth lastPreBreak = mCandidates[lastCand].preBreak;
    for (ssize_t i = lastCand; i >= 0; i--) {
        // "lastPreBreak - mCandidates[i].preBreak" is the amount of difference in mWidth when those
        // breaks where added. So by subtracting that difference, we are subtracting the difference
        // in advances in order to find out how much overhang still remains.
        const float remainingOverhang = secondOverhang - (lastPreBreak - mCandidates[i].preBreak);
        if (remainingOverhang <= 0.0) {
            // No more remaining overhang. We don't need to adjust anything anymore.
            return;
        }
        mCandidates[i].secondOverhang = std::max(mCandidates[i].secondOverhang,
                remainingOverhang);
    }
}

// Find the needed extent between the start and end ranges. start is inclusive and end is exclusive.
// Both are indices of the source string.
MinikinExtent LineBreaker::computeMaxExtent(size_t start, size_t end) const {
    MinikinExtent res = {0.0, 0.0, 0.0};
    for (size_t j = start; j < end; j++) {
        res.extendBy(mMeasuredText.extents[j]);
    }
    return res;
}

void LineBreaker::addGreedyBreak(size_t breakIndex) {
    const Candidate& candidate = mCandidates[breakIndex];
    const Candidate& lastGreedyBreak = getLastBreakCandidate();
    pushBreak(candidate.offset,
            candidate.postBreak - lastGreedyBreak.preBreak,
            computeMaxExtent(lastGreedyBreak.offset, candidate.offset),
            packHyphenEdit(editForNextLine(lastGreedyBreak.hyphenType),
                    editForThisLine(candidate.hyphenType)));
    mLastGreedyBreakIndex = breakIndex;
}

// Also add desperate breaks if needed (ie when word exceeds current line width).
void LineBreaker::considerGreedyBreakCandidate(size_t candIndex, const LineWidth& lineWidth) {
    const Candidate* cand = &mCandidates[candIndex];
    const Candidate* lastGreedyBreak = &getLastBreakCandidate();
    float leftOverhang, rightOverhang;
    // TODO: Only works correctly for unidirectional text. Needs changes for bidi text.
    if (cand->isRtl) {
        leftOverhang = cand->firstOverhang;
        rightOverhang = lastGreedyBreak->secondOverhang;
    } else {
        leftOverhang = lastGreedyBreak->secondOverhang;
        rightOverhang = cand->firstOverhang;
    }
    while (!fitsOnCurrentLine(cand->postBreak - lastGreedyBreak->preBreak,
            leftOverhang, rightOverhang, lineWidth)) {
        // This break would create an overfull line, pick the best break and break there (greedy).
        // We do this in a loop, since there's no guarantee that after a break the remaining text
        // would fit on the next line.

        if (mBestGreedyBreaks.empty()) {
            // If no break has been found since last break but we are inside this loop, the
            // section between the last line break and this candidate doesn't fit in the available
            // space. So we need to consider desperate breaks.

            // Add desperate breaks starting immediately after the last break.
            addDesperateBreaksGreedy(lastGreedyBreak->preBreak, lastGreedyBreak->offset,
                    cand->offset, lineWidth);
            break;
        } else {
            // Break at the best known break.
            addGreedyBreak(popBestGreedyBreak());

            // addGreedyBreak updates the last break candidate.
            lastGreedyBreak = &getLastBreakCandidate();
            if (cand->isRtl) {
                rightOverhang = lastGreedyBreak->secondOverhang;
            } else {
                leftOverhang = lastGreedyBreak->secondOverhang;
            }
        }
    }
    insertGreedyBreakCandidate(candIndex, cand->penalty);
}

void LineBreaker::pushBreak(int offset, float width, MinikinExtent extent, HyphenEdit hyphenEdit) {
    mBreaks.push_back(offset);
    mWidths.push_back(width);
    mAscents.push_back(extent.ascent);
    mDescents.push_back(extent.descent);
    const int flags = ((mFirstTabIndex < mBreaks.back()) << kTab_Shift) | hyphenEdit;
    mFlags.push_back(flags);
    mFirstTabIndex = INT_MAX;
}

LineBreaker::ParaWidth LineBreaker::computeBreaksGreedyPartial(const LineWidth& lineWidth) {
    size_t firstCandidate;
    if (mLastConsideredGreedyCandidate == SIZE_MAX) {
        // Clear results and reset greedy line breaker state if we are here for the first time.
        clearResults();
        mBestGreedyBreaks.clear();
        mLastGreedyBreakIndex = 0;
        mFirstTabIndex = INT_MAX;
        firstCandidate = 1;
    } else {
        firstCandidate = mLastConsideredGreedyCandidate + 1;
    }

    const size_t lastCandidate = mCandidates.size() - 1;
    for (size_t cand = firstCandidate; cand <= lastCandidate; cand++) {
        considerGreedyBreakCandidate(cand, lineWidth);
    }
    mLastConsideredGreedyCandidate = lastCandidate;
    return getLastBreakCandidate().preBreak;
}

// Get the width of a space. May return 0 if there are no spaces.
// Note: if there are multiple different widths for spaces (for example, because of mixing of
// fonts), it's only guaranteed to pick one.
float LineBreaker::getSpaceWidth() const {
    for (size_t i = 0; i < mTextBuf.size(); i++) {
        if (isWordSpace(mTextBuf[i])) {
            return mMeasuredText.widths[i];
        }
    }
    return 0.0f;
}

bool LineBreaker::fitsOnCurrentLine(float width, float leftOverhang, float rightOverhang,
                                    const LineWidth& lineWidth) const {
    const size_t lineNo = mBreaks.size();
    const float availableWidth = lineWidth.getAt(lineNo);
    const float availableLeftPadding = lineWidth.getLeftPaddingAt(lineNo);
    const float availableRightPadding = lineWidth.getRightPaddingAt(lineNo);
    const float remainingLeftOverhang = std::max(0.0f, leftOverhang - availableLeftPadding);
    const float remainingRightOverhang = std::max(0.0f, rightOverhang - availableRightPadding);
    return width + remainingLeftOverhang + remainingRightOverhang <= availableWidth;
}

void LineBreaker::computeBreaksGreedy(const LineWidth& lineWidth) {
    computeBreaksGreedyPartial(lineWidth);
    // All breaks but the last have been added by computeBreaksGreedyPartial() already.
    const Candidate* lastCandidate = &mCandidates.back();
    if (mCandidates.size() == 1 || mLastGreedyBreakIndex != (mCandidates.size() - 1)) {
        const Candidate& lastGreedyBreak = getLastBreakCandidate();
        pushBreak(lastCandidate->offset,
                lastCandidate->postBreak - lastGreedyBreak.preBreak,
                computeMaxExtent(lastGreedyBreak.offset, lastCandidate->offset),
                packHyphenEdit(editForNextLine(lastGreedyBreak.hyphenType),
                        EndHyphenEdit::NO_EDIT));
        // No need to update mLastGreedyBreakIndex because we're done.
    }
}

// Add desperate breaks for the optimal algorithm.
// Note: these breaks are based on the shaping of the (non-broken) original text; they
// are imprecise especially in the presence of kerning, ligatures, overhangs, and Arabic shaping.
void LineBreaker::addDesperateBreaksOptimal(std::vector<Candidate>* out, ParaWidth existingPreBreak,
        size_t postSpaceCount, bool isRtl, size_t start, size_t end) {
    ParaWidth width = existingPreBreak + mMeasuredText.widths[start];
    for (size_t i = start + 1; i < end; i++) {
        const float w = mMeasuredText.widths[i];
        if (w > 0) { // Add desperate breaks only before grapheme clusters.
            out->push_back({i /* offset */, width /* preBreak */, width /* postBreak */,
                    0.0 /* firstOverhang */, 0.0 /* secondOverhang */,
                    SCORE_DESPERATE /* penalty */,
                    // postSpaceCount doesn't include trailing spaces.
                    postSpaceCount /* preSpaceCount */, postSpaceCount /* postSpaceCount */,
                    HyphenationType::BREAK_AND_DONT_INSERT_HYPHEN /* hyphenType */,
                    isRtl /* isRtl */});
            width += w;
        }
    }
}

void LineBreaker::addAllDesperateBreaksOptimal(const LineWidth& lineWidth) {
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
    if (firstDesperateIndex == 0) { // No desperate breaks needed.
        return;
    }

    // This temporary holds an expanded list of candidates, which will be later copied back into
    // mCandidates. The beginning of mCandidates, where there are no desparate breaks, is skipped.
    std::vector<Candidate> expandedCandidates;
    const size_t nRemainingCandidates = nCand - firstDesperateIndex;
    expandedCandidates.reserve(nRemainingCandidates + 1); // At least one more is needed.
    for (size_t i = firstDesperateIndex; i < nCand; i++) {
        const Candidate& previousCand = mCandidates[i - 1];
        const Candidate& thisCand = mCandidates[i];
        const ParaWidth requiredWidth = thisCand.postBreak - previousCand.preBreak;
        if (requiredWidth > minLineWidth) {
            addDesperateBreaksOptimal(&expandedCandidates, previousCand.preBreak,
                    thisCand.postSpaceCount, thisCand.isRtl, previousCand.offset /* start */,
                    thisCand.offset /* end */);
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
void LineBreaker::finishBreaksOptimal(const std::vector<OptimalBreaksData>& breaksData) {
    // clear output vectors.
    clearResults();

    const size_t nCand = mCandidates.size();
    size_t prev;
    for (size_t i = nCand - 1; i > 0; i = prev) {
        prev = breaksData[i].prev;
        mBreaks.push_back(mCandidates[i].offset);
        mWidths.push_back(mCandidates[i].postBreak - mCandidates[prev].preBreak);
        MinikinExtent extent = computeMaxExtent(mCandidates[prev].offset, mCandidates[i].offset);
        mAscents.push_back(extent.ascent);
        mDescents.push_back(extent.descent);

        const HyphenEdit edit = packHyphenEdit(
                prev == 0 ? StartHyphenEdit::NO_EDIT : editForNextLine(mCandidates[prev].hyphenType),
                editForThisLine(mCandidates[i].hyphenType));
        mFlags.push_back(static_cast<int>(edit));
    }
    std::reverse(mBreaks.begin(), mBreaks.end());
    std::reverse(mWidths.begin(), mWidths.end());
    std::reverse(mFlags.begin(), mFlags.end());
}

void LineBreaker::computeBreaksOptimal(const LineWidth& lineWidth) {
    size_t active = 0;
    const size_t nCand = mCandidates.size();
    const float maxShrink = mJustified ? SHRINKABILITY * getSpaceWidth() : 0.0f;

    std::vector<OptimalBreaksData> breaksData;
    breaksData.reserve(nCand);
    breaksData.push_back({0.0, 0, 0}); // The first candidate is always at the first line.

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
                    if (-delta < maxShrink *
                            (mCandidates[i].postSpaceCount - mCandidates[j].preSpaceCount)) {
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
        breaksData.push_back({
            best + mCandidates[i].penalty + mLinePenalty,  // score
            bestPrev,  // prev
            breaksData[bestPrev].lineNumber + 1});  // lineNumber
    }
    finishBreaksOptimal(breaksData);
}

LineBreakResult LineBreaker::computeBreaks(const std::vector<std::unique_ptr<Run>>& runs,
                                           const LineWidth& lineWidth,
                                           const TabStops& tabStops) {
    for (const auto& run : runs) {
        addRun(*run, lineWidth, tabStops);
    }

    if (mStrategy == BreakStrategy::Greedy) {
        computeBreaksGreedy(lineWidth);
    } else {
        addAllDesperateBreaksOptimal(lineWidth);
        computeBreaksOptimal(lineWidth);
    }
    LineBreakResult result;
    result.breakPoints = std::move(mBreaks);
    result.widths = std::move(mWidths);
    result.ascents = std::move(mAscents);
    result.descents = std::move(mDescents);
    result.flags = std::move(mFlags);
    return result;
}

size_t LineBreaker::popBestGreedyBreak() {
    const size_t bestBreak = mBestGreedyBreaks.front().index;
    mBestGreedyBreaks.pop_front();
    return bestBreak;
}

void LineBreaker::insertGreedyBreakCandidate(size_t index, float penalty) {
    GreedyBreak gBreak = {index, penalty};
    if (!mBestGreedyBreaks.empty()) {
        // Find the location in the queue where the penalty is <= the current penalty, and drop the
        // elements from there to the end of the queue.
        auto where = std::lower_bound(
                mBestGreedyBreaks.begin(), mBestGreedyBreaks.end(), gBreak,
                [](GreedyBreak first, GreedyBreak second) -> bool
                        { return first.penalty < second.penalty; });
        if (where != mBestGreedyBreaks.end()) {
            mBestGreedyBreaks.erase(where, mBestGreedyBreaks.end());
        }
    }
    mBestGreedyBreaks.push_back(gBreak);
}

}  // namespace minikin
