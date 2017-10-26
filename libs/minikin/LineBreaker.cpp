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

#include "minikin/Layout.h"
#include "HyphenatorMap.h"
#include "LayoutUtils.h"
#include "Locale.h"
#include "LocaleListCache.h"
#include "MinikinInternal.h"
#include "WordBreaker.h"

namespace minikin {

constexpr uint16_t CHAR_TAB = 0x0009;
constexpr uint16_t CHAR_NBSP = 0x00A0;

// Large scores in a hierarchy; we prefer desperate breaks to an overfull line. All these
// constants are larger than any reasonable actual width score.
const float SCORE_INFTY = std::numeric_limits<float>::max();
const float SCORE_OVERFULL = 1e12f;
// TODO: re-enable when adding support for desperate line breaks in optimal line breaking.
// const float SCORE_DESPERATE = 1e10f;

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

// When the text buffer is within this limit, capacity of vectors is retained at finish(),
// to avoid allocation.
const size_t MAX_TEXT_BUF_RETAIN = 32678;

// Maximum amount that spaces can shrink, in justified text.
const float SHRINKABILITY = 1.0 / 3.0;

inline static const LocaleList& getLocaleList(uint32_t localeListId) {
    android::AutoMutex _l(gMinikinLock);
    return LocaleListCache::getById(localeListId);
}

LineBreaker::LineBreaker() : mWordBreaker(std::make_unique<WordBreaker>()) { }

LineBreaker::LineBreaker(std::unique_ptr<WordBreaker>&& breaker)
        : mWordBreaker(std::move(breaker)) { }

LineBreaker::~LineBreaker() {}

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

void LineBreaker::setText() {
    mWordBreaker->setText(mTextBuf.data(), mTextBuf.size());

    // handle initial break here because addStyleRun may never be called
    mCandidates.clear();
    mCandidates.push_back({
            0 /* offset */, 0.0 /* preBreak */, 0.0 /* postBreak */,
            0.0 /* firstOverhang */, 0.0 /* secondOverhang */,
            0.0 /* penalty */,
            0 /* preSpaceCount */, 0 /* postSpaceCount */,
            HyphenationType::DONT_BREAK /* hyphenType */,
            false /* isRtl. TODO: may need to be based on input. */});

    mLastConsideredGreedyCandidate = SIZE_MAX;

    mSpaceCount = 0;
    mCurrentLocaleListId = LocaleListCache::kInvalidListId;
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
std::vector<HyphenationType> LineBreaker::hyphenate(const uint16_t* str, size_t len) {
    std::vector<HyphenationType> out;
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
                        mHyphenator->hyphenate(&out, str, wordLen);
                    } else {
                        std::vector<HyphenationType> wordVec;
                        mHyphenator->hyphenate(&wordVec, str + wordStart, wordLen);
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
// paint and typeface will be used for measuring the text and paint must not be null.
// runStart is the beginning of the whole run, and is used for making sure we don't hyphenated words
//     that go outside the run.
// afterWord is the index of last code unit seen that's not a line-ending space, plus one. In other
//     words, the index of the first code unit after a word.
// lastBreak is the index of the previous break point and lastBreakWidth is the width seen until
//     that point.
// bidiFlags keep the bidi flags to determine the direction of text for layout and other
//     calculations. It may only be Bidi::FORCE_RTL or Bidi::FORCE_LTR.
//
// The following parameters are needed to be passed to addWordBreak:
// postBreak is the width that would be seen if we decide to break at the end of the word (so it
//     doesn't count any line-ending space after the word).
// postSpaceCount is the number of spaces that would be seen if we decide to break at the end of the
//     word (and like postBreak it doesn't count any line-ending space after the word).
// hyphenPenalty is the amount of penalty for hyphenation.
void LineBreaker::addHyphenationCandidates(MinikinPaint* paint,
        const std::shared_ptr<FontCollection>& typeface, size_t runStart,
        size_t afterWord, size_t lastBreak, ParaWidth lastBreakWidth, ParaWidth postBreak,
        size_t postSpaceCount, float hyphenPenalty, Bidi bidiFlags) {
    const bool isRtlWord = isRtl(bidiFlags);
    const size_t wordStart = mWordBreaker->wordStart();
    const size_t wordEnd = mWordBreaker->wordEnd();
    if (wordStart < runStart || wordEnd <= wordStart) {
        return;
    }
    std::vector<HyphenationType> hyphenResult =
            hyphenate(&mTextBuf[wordStart], wordEnd - wordStart);

    std::vector<float> advances;
    std::vector<LayoutOverhang> overhangs;
    const size_t bufferSize = std::max(wordEnd - 1 - lastBreak, afterWord - wordStart);
    advances.reserve(bufferSize);
    overhangs.reserve(bufferSize);
    // measure hyphenated substrings
    for (size_t j = wordStart; j < wordEnd; j++) {
        HyphenationType hyph = hyphenResult[j - wordStart];
        if (hyph == HyphenationType::DONT_BREAK) {
            continue;
        }

        paint->hyphenEdit = HyphenEdit::editForThisLine(hyph);
        const size_t firstPartLen = j - lastBreak;
        advances.resize(firstPartLen);
        overhangs.resize(firstPartLen);
        const float firstPartWidth = Layout::measureText(mTextBuf.data(), lastBreak, firstPartLen,
                mTextBuf.size(), bidiFlags, *paint, typeface, advances.data(),
                nullptr /* extent */, overhangs.data());
        const ParaWidth hyphPostBreak = lastBreakWidth + firstPartWidth;
        LayoutOverhang overhang = computeOverhang(firstPartWidth, advances, overhangs, isRtlWord);
        // TODO: This ignores potential overhang from a previous word, e.g. in "R table" if the
        // right overhang of the R is larger than the advance of " ta-". In such cases, we need to
        // take the existing overhang into account.
        const float firstOverhang = isRtlWord ? overhang.left : overhang.right;

        paint->hyphenEdit = HyphenEdit::editForNextLine(hyph);
        const size_t secondPartLen = afterWord - j;
        advances.resize(secondPartLen);
        overhangs.resize(secondPartLen);
        const float secondPartWidth = Layout::measureText(mTextBuf.data(), j, secondPartLen,
                mTextBuf.size(), bidiFlags, *paint, typeface, advances.data(),
                nullptr /* extent */, overhangs.data());
        // hyphPreBreak is calculated like this so that when the line width for a future line break
        // is being calculated, the width of the whole word would be subtracted and the width of the
        // second part would be added.
        const ParaWidth hyphPreBreak = postBreak - secondPartWidth;
        overhang = computeOverhang(secondPartWidth, advances, overhangs, isRtlWord);
        const float secondOverhang = isRtlWord ? overhang.right : overhang.left;

        addWordBreak(j, hyphPreBreak, hyphPostBreak, firstOverhang, secondOverhang,
                postSpaceCount, postSpaceCount, hyphenPenalty, hyph, isRtlWord);

        paint->hyphenEdit = HyphenEdit::NO_EDIT;
    }
}

// Ordinarily, this method measures the text in the range given. However, when paint is nullptr, it
// assumes the character widths, extents, and overhangs have already been calculated and stored in
// the mCharWidths, mCharExtents, and mCharOverhangs buffers.
//
// This method finds the candidate word breaks (using the ICU break iterator) and sends them
// to addWordBreak.
void LineBreaker::addStyleRunInternal(MinikinPaint* paint,
        const std::shared_ptr<FontCollection>& typeface, size_t start, size_t end, bool isRtl,
        uint32_t localeListId) {
    const Bidi bidiFlags = isRtl ? Bidi::FORCE_RTL : Bidi::FORCE_LTR;

    float hyphenPenalty = 0.0;
    if (paint != nullptr) {
        Layout::measureText(mTextBuf.data(), start, end - start, mTextBuf.size(), bidiFlags,
                *paint, typeface, mCharWidths.data() + start, mCharExtents.data() + start,
                mCharOverhangs.data() + start);

        // a heuristic that seems to perform well
        hyphenPenalty = 0.5 * paint->size * paint->scaleX * mLineWidthDelegate->getLineWidth(0);
        if (mHyphenationFrequency == kHyphenationFrequency_Normal) {
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

    setLocaleList(localeListId, start);
    size_t current = (size_t) mWordBreaker->current();
    // This will keep the index of last code unit seen that's not a line-ending space, plus one.
    // In other words, the index of the first code unit after a word.
    size_t afterWord = start;

    size_t lastBreak = start; // The index of the previous break point.
    ParaWidth lastBreakWidth = mWidth; // The width of the text as of the previous break point.
    ParaWidth postBreak = mWidth; // The width of text seen if we decide to break here
    // postBreak plus potential forward overhang. Guaranteed to be >= postBreak.
    ParaWidth postBreakWithOverhang = mWidth;
    // The maximum amount of backward overhang seen since last word.
    float maxBackwardOverhang = 0;
    size_t postSpaceCount = mSpaceCount;
    const bool hyphenate = (paint != nullptr &&
            mHyphenationFrequency != kHyphenationFrequency_None);
    for (size_t i = start; i < end; i++) {
        const uint16_t c = mTextBuf[i];
        if (c == CHAR_TAB) {
            // Fall back to greedy; other modes don't know how to deal with tabs.
            mStrategy = kBreakStrategy_Greedy;
            // In order to figure out the actual width of the tab, we need to run the greedy
            // algorithm on all previous text and determine the last line break's preBreak.
            const ParaWidth lastPreBreak = computeBreaksGreedyPartial();
            mWidth = lastPreBreak + mTabStops.nextTab(mWidth - lastPreBreak);
            if (mFirstTabIndex == INT_MAX) {
                mFirstTabIndex = static_cast<int>(i);
            }
            // No need to update afterWord since tab characters can not be an end of word character
            // in WordBreaker. See the implementation of WordBreaker::wordEnd.
        } else {
            if (isWordSpace(c)) {
                mSpaceCount += 1;
            }
            mWidth += mCharWidths[i];
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
                afterWord = i + 1;

                // TODO: This doesn't work for very tight lines and large overhangs, where the
                // overhang from a previous word that may end up on an earline line may be
                // considered still in effect for a later word. But that's expected to be very rare,
                // so we ignore it for now.
                postBreakWithOverhang = std::max(postBreakWithOverhang,
                        postBreak + (isRtl ? mCharOverhangs[i].left : mCharOverhangs[i].right));

                float backwardOverhang = isRtl ? mCharOverhangs[i].right : mCharOverhangs[i].left;
                // Adjust backwardOverhang by the advance already seen from the last break.
                backwardOverhang -= (mWidth - mCharWidths[i]) - lastBreakWidth;
                maxBackwardOverhang = std::max(maxBackwardOverhang, backwardOverhang);
            }
        }
        if (i + 1 == current) { // We are at the end of a word.
            // We skip breaks for zero-width characters inside replacement spans.
            const bool addBreak = paint != nullptr || current == end || mCharWidths[current] > 0;

            if (addBreak) {
                // adjust second overhang for previous breaks
                adjustSecondOverhang(maxBackwardOverhang);
            }
            if (hyphenate) {
                addHyphenationCandidates(paint, typeface, start, afterWord, lastBreak,
                        lastBreakWidth, postBreak, postSpaceCount, hyphenPenalty,
                        bidiFlags);
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
            lastBreak = current;
            lastBreakWidth = mWidth;
            maxBackwardOverhang = 0;
            current = (size_t)mWordBreaker->next();
        }
    }
}

// Add desperate breaks.
// Note: these breaks are based on the shaping of the (non-broken) original text; they
// are imprecise especially in the presence of kerning, ligatures, overhangs, and Arabic shaping.
void LineBreaker::addDesperateBreaks(ParaWidth existingPreBreak, size_t start, size_t end) {
    ParaWidth width = mCharWidths[start];
    for (size_t i = start + 1; i < end; i++) {
        const float w = mCharWidths[i];
        if (w > 0) { // Add desperate breaks only before grapheme clusters.
            const ParaWidth newWidth = width + w;
            if (!fitsOnCurrentLine(newWidth, 0.0, 0.0)) {
                constexpr HyphenationType hyphen = HyphenationType::BREAK_AND_DONT_INSERT_HYPHEN;
                pushBreak(i, width, computeMaxExtent(mLastGreedyBreak->offset, i),
                        HyphenEdit::editForNextLine(mLastGreedyBreak->hyphenType)
                                | HyphenEdit::editForThisLine(hyphen));

                existingPreBreak += width;
                // Only set the fields that will be later read.
                mFakeDesperateCandidate.offset = i;
                mFakeDesperateCandidate.preBreak = existingPreBreak;
                mFakeDesperateCandidate.secondOverhang = 0.0;
                mFakeDesperateCandidate.hyphenType = hyphen;
                mLastGreedyBreak = &mFakeDesperateCandidate;

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
        res.extendBy(mCharExtents[j]);
    }
    return res;
}

void LineBreaker::addGreedyBreak(size_t breakIndex) {
    const Candidate& candidate = mCandidates[breakIndex];
    pushBreak(candidate.offset,
            candidate.postBreak - mLastGreedyBreak->preBreak,
            computeMaxExtent(mLastGreedyBreak->offset, candidate.offset),
            HyphenEdit::editForNextLine(mLastGreedyBreak->hyphenType)
                    | HyphenEdit::editForThisLine(candidate.hyphenType));
    mLastGreedyBreak = &candidate;
}

// Also add desperate breaks if needed (ie when word exceeds current line width).
void LineBreaker::considerGreedyBreakCandidate(size_t candIndex) {
    const Candidate* cand = &mCandidates[candIndex];
    float leftOverhang, rightOverhang;
    // TODO: Only works correctly for unidirectional text. Needs changes for bidi text.
    if (cand->isRtl) {
        leftOverhang = cand->firstOverhang;
        rightOverhang = mLastGreedyBreak->secondOverhang;
    } else {
        leftOverhang = mLastGreedyBreak->secondOverhang;
        rightOverhang = cand->firstOverhang;
    }
    while (!fitsOnCurrentLine(cand->postBreak - mLastGreedyBreak->preBreak,
            leftOverhang, rightOverhang)) {
        // This break would create an overfull line, pick the best break and break there (greedy).
        // We do this in a loop, since there's no guarantee that after a break the remaining text
        // would fit on the next line.

        if (mBestGreedyBreaks.empty()) {
            // If no break has been found since last break but we are inside this loop, the
            // section between the last line break and this candidate doesn't fit in the available
            // space. So we need to consider desperate breaks.

            // Add desperate breaks starting immediately after the last break.
            addDesperateBreaks(mLastGreedyBreak->preBreak, mLastGreedyBreak->offset, cand->offset);
            break;
        } else {
            // Break at the best known break.
            addGreedyBreak(popBestGreedyBreak());
            if (cand->isRtl) {
                rightOverhang = mLastGreedyBreak->secondOverhang;
            } else {
                leftOverhang = mLastGreedyBreak->secondOverhang;
            }
        }
    }
    insertGreedyBreakCandidate(candIndex, cand->penalty);
}

void LineBreaker::pushBreak(int offset, float width, MinikinExtent extent, uint8_t hyphenEdit) {
    mBreaks.push_back(offset);
    mWidths.push_back(width);
    mAscents.push_back(extent.ascent);
    mDescents.push_back(extent.descent);
    const int flags = ((mFirstTabIndex < mBreaks.back()) << kTab_Shift) | hyphenEdit;
    mFlags.push_back(flags);
    mFirstTabIndex = INT_MAX;
}

LineBreaker::ParaWidth LineBreaker::computeBreaksGreedyPartial() {
    size_t firstCandidate;
    if (mLastConsideredGreedyCandidate == SIZE_MAX) {
        // Clear results and reset greedy line breaker state if we are here for the first time.
        clearResults();
        mBestGreedyBreaks.clear();
        mLastGreedyBreak = &mCandidates[0];
        mFirstTabIndex = INT_MAX;
        firstCandidate = 1;
    } else {
        firstCandidate = mLastConsideredGreedyCandidate + 1;
    }

    const size_t lastCandidate = mCandidates.size() - 1;
    for (size_t cand = firstCandidate; cand <= lastCandidate; cand++) {
        considerGreedyBreakCandidate(cand);
    }
    mLastConsideredGreedyCandidate = lastCandidate;
    return mLastGreedyBreak->preBreak;
}

void LineBreaker::addReplacement(size_t start, size_t end, float width, uint32_t localeListId) {
    mCharWidths[start] = width;
    std::fill(&mCharWidths[start + 1], &mCharWidths[end], 0.0f);
    // TODO: Get the extents information from the caller.
    std::fill(&mCharExtents[start], &mCharExtents[end], (MinikinExtent) {0.0f, 0.0f, 0.0f});
    addStyleRunInternal(nullptr, nullptr, start, end, false, localeListId);
}

// Get the width of a space. May return 0 if there are no spaces.
// Note: if there are multiple different widths for spaces (for example, because of mixing of
// fonts), it's only guaranteed to pick one.
float LineBreaker::getSpaceWidth() const {
    for (size_t i = 0; i < mTextBuf.size(); i++) {
        if (isWordSpace(mTextBuf[i])) {
            return mCharWidths[i];
        }
    }
    return 0.0f;
}

bool LineBreaker::fitsOnCurrentLine(float width, float leftOverhang, float rightOverhang) const {
    const size_t lineNo = mBreaks.size();
    const float availableWidth = mLineWidthDelegate->getLineWidth(lineNo);
    const float availableLeftPadding = mLineWidthDelegate->getLeftPadding(lineNo);
    const float availableRightPadding = mLineWidthDelegate->getRightPadding(lineNo);
    const float remainingLeftOverhang = std::max(0.0f, leftOverhang - availableLeftPadding);
    const float remainingRightOverhang = std::max(0.0f, rightOverhang - availableRightPadding);
    return width + remainingLeftOverhang + remainingRightOverhang <= availableWidth;
}

void LineBreaker::computeBreaksGreedy() {
    computeBreaksGreedyPartial();
    // All breaks but the last have been added by computeBreaksGreedyPartial() already.
    const Candidate* lastCandidate = &mCandidates.back();
    if (mCandidates.size() == 1 || mLastGreedyBreak != lastCandidate) {
        pushBreak(lastCandidate->offset,
                lastCandidate->postBreak - mLastGreedyBreak->preBreak,
                computeMaxExtent(mLastGreedyBreak->offset, lastCandidate->offset),
                HyphenEdit::editForNextLine(mLastGreedyBreak->hyphenType));
        // No need to update mLastGreedyBreak because we're done.
    }
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
        int flags = HyphenEdit::editForThisLine(mCandidates[i].hyphenType);
        if (prev > 0) {
            flags |= HyphenEdit::editForNextLine(mCandidates[prev].hyphenType);
        }
        mFlags.push_back(flags);
    }
    std::reverse(mBreaks.begin(), mBreaks.end());
    std::reverse(mWidths.begin(), mWidths.end());
    std::reverse(mFlags.begin(), mFlags.end());
}

//TODO: Support desperate breaks in optimal line breaking.
void LineBreaker::computeBreaksOptimal() {
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
        float width = mLineWidthDelegate->getLineWidth(lineNumberLast);

        ParaWidth leftEdge = mCandidates[i].postBreak - width;
        float bestHope = 0;

        // "j" iterates through candidates for the beginning of the line.
        for (size_t j = active; j < i; j++) {
            const size_t lineNumber = breaksData[j].lineNumber;
            if (lineNumber != lineNumberLast) {
                const float widthNew = mLineWidthDelegate->getLineWidth(lineNumber);
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
            } else if (atEnd && mStrategy != kBreakStrategy_Balanced) {
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

size_t LineBreaker::computeBreaks() {
    if (mStrategy == kBreakStrategy_Greedy) {
        computeBreaksGreedy();
    } else {
        computeBreaksOptimal();
    }
    return mBreaks.size();
}

void LineBreaker::finish() {
    mWordBreaker->finish();
    mWidth = 0;
    mCandidates.clear();
    mBestGreedyBreaks.clear();
    mCurrentLocaleListId = LocaleListCache::kInvalidListId;
    clearResults();
    if (mTextBuf.size() > MAX_TEXT_BUF_RETAIN) {
        mTextBuf.clear();
        mTextBuf.shrink_to_fit();
        mCharWidths.clear();
        mCharWidths.shrink_to_fit();
        mCharExtents.clear();
        mCharExtents.shrink_to_fit();
        mCharOverhangs.clear();
        mCharOverhangs.shrink_to_fit();

        mCandidates.shrink_to_fit();
        mBestGreedyBreaks.shrink_to_fit();

        mBreaks.shrink_to_fit();
        mWidths.shrink_to_fit();
        mAscents.shrink_to_fit();
        mDescents.shrink_to_fit();
        mFlags.shrink_to_fit();
    }
    mStrategy = kBreakStrategy_Greedy;
    mHyphenationFrequency = kHyphenationFrequency_Normal;
    mLinePenalty = 0.0f;
    mJustified = false;
    mLineWidthDelegate.reset();
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
