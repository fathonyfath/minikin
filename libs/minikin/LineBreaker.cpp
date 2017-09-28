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

#define VERBOSE_DEBUG 0

#define LOG_TAG "Minikin"

#include <limits>

#include <log/log.h>

#include "LayoutUtils.h"
#include "WordBreaker.h"
#include <minikin/Layout.h>
#include <minikin/LineBreaker.h>

using std::vector;

namespace minikin {

constexpr uint16_t CHAR_TAB = 0x0009;
constexpr uint16_t CHAR_NBSP = 0x00A0;

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

// When the text buffer is within this limit, capacity of vectors is retained at finish(),
// to avoid allocation.
const size_t MAX_TEXT_BUF_RETAIN = 32678;

// Maximum amount that spaces can shrink, in justified text.
const float SHRINKABILITY = 1.0 / 3.0;

LineBreaker::LineBreaker() : mWordBreaker(std::make_unique<WordBreaker>()) { }

LineBreaker::LineBreaker(std::unique_ptr<WordBreaker>&& breaker)
        : mWordBreaker(std::move(breaker)) { }

LineBreaker::~LineBreaker() {}

void LineBreaker::setLocales(const char* locales, const std::vector<Hyphenator*>& hyphenators,
        size_t restartFrom) {
    bool goodLocaleFound = false;
    const ssize_t numLocales = hyphenators.size();
    // For now, we ignore all locales except the first valid one.
    // TODO: Support selecting the locale based on the script of the text.
    const char* localeStart = locales;
    icu::Locale locale;
    for (ssize_t i = 0; i < numLocales - 1; i++) { // Loop over all locales, except the last one.
        const char* localeEnd = strchr(localeStart, ',');
        const size_t localeNameLength = localeEnd - localeStart;
        char localeName[localeNameLength + 1];
        strncpy(localeName, localeStart, localeNameLength);
        localeName[localeNameLength] = '\0';
        locale = icu::Locale::createFromName(localeName);
        goodLocaleFound = !locale.isBogus();
        if (goodLocaleFound) {
            mHyphenator = hyphenators[i];
            break;
        } else {
            localeStart = localeEnd + 1;
        }
    }
    if (!goodLocaleFound) { // Try the last locale.
        locale = icu::Locale::createFromName(localeStart);
        if (locale.isBogus()) {
            // No good locale.
            locale = icu::Locale::getRoot();
            mHyphenator = nullptr;
        } else {
            mHyphenator = numLocales == 0 ? nullptr : hyphenators[numLocales - 1];
        }
    }
    mWordBreaker->followingWithLocale(locale, restartFrom);
}

void LineBreaker::setText() {
    mWordBreaker->setText(mTextBuf.data(), mTextBuf.size());

    // handle initial break here because addStyleRun may never be called
    mCandidates.clear();
    Candidate cand = {
            0, 0, 0.0, 0.0, 0.0, 0.0, 0, 0, {0.0, 0.0, 0.0}, HyphenationType::DONT_BREAK};
    mCandidates.push_back(cand);

    // reset greedy breaker state
    mBreaks.clear();
    mWidths.clear();
    mAscents.clear();
    mDescents.clear();
    mFlags.clear();
    mLastBreak = 0;
    mBestBreak = 0;
    mBestScore = SCORE_INFTY;
    mPreBreak = 0;
    mLastHyphenation = HyphenEdit::NO_EDIT;
    mFirstTabIndex = INT_MAX;
    mSpaceCount = 0;
}

// This function determines whether a character is a space that disappears at end of line.
// It is the Unicode set: [[:General_Category=Space_Separator:]-[:Line_Break=Glue:]],
// plus '\n'.
// Note: all such characters are in the BMP, so it's ok to use code units for this.
static bool isLineEndSpace(uint16_t c) {
    return c == '\n' || c == ' ' || c == 0x1680 || (0x2000 <= c && c <= 0x200A && c != 0x2007) ||
            c == 0x205F || c == 0x3000;
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
// extent is a pointer to the variable that keeps track of the maximum vertical extents seen since
//     the last time addWordBreak() was called. It will be reset to 0 after every call.
// bidiFlags keep the bidi flags to determine the direction of text for layout and other
//     calculations. It may only be kBidi_Force_RTL or kBidi_Force_LTR.
//
// The following parameters are needed to be passed to addWordBreak:
// postBreak is the width that would be seen if we decide to break at the end of the word (so it
//     doesn't count any line-ending space after the word).
// postSpaceCount is the number of spaces that would be seen if we decide to break at the end of the
//     word (and like postBreak it doesn't count any line-ending space after the word).
// hyphenPenalty is the amount of penalty for hyphenation.
void LineBreaker::addHyphenationCandidates(MinikinPaint* paint,
        const std::shared_ptr<FontCollection>& typeface, FontStyle style, size_t runStart,
        size_t afterWord, size_t lastBreak, ParaWidth lastBreakWidth, ParaWidth postBreak,
        size_t postSpaceCount, MinikinExtent* extent, float hyphenPenalty, int bidiFlags) {
    const bool isRtl = (bidiFlags == kBidi_Force_RTL);
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
                mTextBuf.size(), bidiFlags, style, *paint, typeface, advances.data(),
                nullptr /* extent */, overhangs.data());
        const LayoutOverhang overhang = computeOverhang(firstPartWidth, advances, overhangs, isRtl);
        const ParaWidth hyphPostBreak = lastBreakWidth + firstPartWidth
                + (isRtl ? overhang.left : overhang.right);

        paint->hyphenEdit = HyphenEdit::editForNextLine(hyph); // XXX may need to be NO_EDIT.
        const size_t secondPartLen = afterWord - j;
        const float secondPartWidth = Layout::measureText(mTextBuf.data(), j, secondPartLen,
                mTextBuf.size(), bidiFlags, style, *paint, typeface, nullptr /* advances */,
                nullptr /* extent */, nullptr /* overhangs */);
        const ParaWidth hyphPreBreak = postBreak - secondPartWidth;

        addWordBreak(j, hyphPreBreak, hyphPostBreak, postSpaceCount,
                postSpaceCount, *extent, hyphenPenalty, hyph);
        extent->reset();

        paint->hyphenEdit = HyphenEdit::NO_EDIT;
    }
}

// Ordinarily, this method measures the text in the range given. However, when paint is nullptr, it
// assumes the character widths, extents, and overhangs have already been calculated and stored in
// the mCharWidths, mCharExtents, and mCharOverhangs buffers.
//
// This method finds the candidate word breaks (using the ICU break iterator) and sends them
// to addCandidate.
float LineBreaker::addStyleRun(MinikinPaint* paint, const std::shared_ptr<FontCollection>& typeface,
        FontStyle style, size_t start, size_t end, bool isRtl, const char* langTags,
        const std::vector<Hyphenator*>& hyphenators) {
    float width = 0.0f;
    const int bidiFlags = isRtl ? kBidi_Force_RTL : kBidi_Force_LTR;

    float hyphenPenalty = 0.0;
    if (paint != nullptr) {
        width = Layout::measureText(mTextBuf.data(), start, end - start, mTextBuf.size(), bidiFlags,
                style, *paint, typeface, mCharWidths.data() + start, mCharExtents.data() + start,
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

    // Caller passes nullptr for langTag if language is not changed.
    if (langTags != nullptr) {
        setLocales(langTags, hyphenators, start);
    }
    size_t current = (size_t) mWordBreaker->current();
    // This will keep the index of last code unit seen that's not a line-ending space, plus one.
    // In other words, the index of the first code unit after a word.
    size_t afterWord = start;

    size_t lastBreak = start; // The index of the previous break point.
    ParaWidth lastBreakWidth = mWidth; // The width of the text as of the previous break point.
    ParaWidth postBreak = mWidth; // The width of text seen if we decide to break here
    size_t postSpaceCount = mSpaceCount;
    MinikinExtent extent = {0.0, 0.0, 0.0};
    for (size_t i = start; i < end; i++) {
        const uint16_t c = mTextBuf[i];
        if (c == CHAR_TAB) {
            mWidth = mPreBreak + mTabStops.nextTab(mWidth - mPreBreak);
            if (mFirstTabIndex == INT_MAX) {
                mFirstTabIndex = (int)i;
            }
            // fall back to greedy; other modes don't know how to deal with tabs
            mStrategy = kBreakStrategy_Greedy;
        } else {
            if (isWordSpace(c)) {
                mSpaceCount += 1;
            }
            mWidth += mCharWidths[i];
            extent.extendBy(mCharExtents[i]);
            if (isLineEndSpace(c)) {
                // If we break a line on a line-ending space, that space goes away. So postBreak
                // and postSpaceCount, which keep the width and number of spaces if we decide to
                // break at this point, don't need to get adjusted.
            } else {
                postBreak = mWidth;
                postSpaceCount = mSpaceCount;
                afterWord = i + 1;
            }
        }
        if (i + 1 == current) { // We are at the end of a word.
            if (paint != nullptr && mHyphenator != nullptr &&
                    mHyphenationFrequency != kHyphenationFrequency_None) {
                addHyphenationCandidates(paint, typeface, style, start, afterWord, lastBreak,
                        lastBreakWidth, postBreak, postSpaceCount, &extent, hyphenPenalty,
                        bidiFlags);
            }

            // Skip break for zero-width characters inside replacement span
            if (paint != nullptr || current == end || mCharWidths[current] > 0) {
                const float penalty = hyphenPenalty * mWordBreaker->breakBadness();
                addWordBreak(current, mWidth, postBreak, mSpaceCount, postSpaceCount, extent,
                        penalty, HyphenationType::DONT_BREAK);
                extent.reset();
            }
            lastBreak = current;
            lastBreakWidth = mWidth;
            current = (size_t)mWordBreaker->next();
        }
    }

    return width;
}

// add a word break (possibly for a hyphenated fragment), and add desperate breaks if
// needed (ie when word exceeds current line width)
void LineBreaker::addWordBreak(size_t offset, ParaWidth preBreak, ParaWidth postBreak,
        size_t preSpaceCount, size_t postSpaceCount, MinikinExtent extent,
        float penalty, HyphenationType hyph) {
    Candidate cand;
    ParaWidth width = mCandidates.back().preBreak;
    if (postBreak - width > currentLineWidth()) {
        // Add desperate breaks.
        // Note: these breaks are based on the shaping of the (non-broken) original text; they
        // are imprecise especially in the presence of kerning, ligatures, and Arabic shaping.
        size_t i = mCandidates.back().offset;
        width += mCharWidths[i++];
        for (; i < offset; i++) {
            float w = mCharWidths[i];
            if (w > 0) {
                cand.offset = i;
                cand.preBreak = width;
                cand.postBreak = width;
                // postSpaceCount doesn't include trailing spaces
                cand.preSpaceCount = postSpaceCount;
                cand.postSpaceCount = postSpaceCount;
                cand.extent = mCharExtents[i];
                cand.penalty = SCORE_DESPERATE;
                cand.hyphenType = HyphenationType::BREAK_AND_DONT_INSERT_HYPHEN;
#if VERBOSE_DEBUG
                ALOGD("desperate cand: %zd %g:%g",
                        mCandidates.size(), cand.postBreak, cand.preBreak);
#endif
                addCandidate(cand);
                width += w;
            }
        }
    }

    cand.offset = offset;
    cand.preBreak = preBreak;
    cand.postBreak = postBreak;
    cand.penalty = penalty;
    cand.preSpaceCount = preSpaceCount;
    cand.postSpaceCount = postSpaceCount;
    cand.extent = extent;
    cand.hyphenType = hyph;
#if VERBOSE_DEBUG
    ALOGD("cand: %zd %g:%g", mCandidates.size(), cand.postBreak, cand.preBreak);
#endif
    addCandidate(cand);
}

// Find the needed extent between the start and end ranges. start and end are inclusive.
MinikinExtent LineBreaker::computeMaxExtent(size_t start, size_t end) const {
    MinikinExtent res = mCandidates[end].extent;
    for (size_t j = start; j < end; j++) {
        res.extendBy(mCandidates[j].extent);
    }
    return res;
}

// Helper method for addCandidate()
void LineBreaker::pushGreedyBreak() {
    const Candidate& bestCandidate = mCandidates[mBestBreak];
    pushBreak(bestCandidate.offset, bestCandidate.postBreak - mPreBreak,
            computeMaxExtent(mLastBreak + 1, mBestBreak),
            mLastHyphenation | HyphenEdit::editForThisLine(bestCandidate.hyphenType));
    mBestScore = SCORE_INFTY;
#if VERBOSE_DEBUG
    ALOGD("break: %d %g", mBreaks.back(), mWidths.back());
#endif
    mLastBreak = mBestBreak;
    mPreBreak = bestCandidate.preBreak;
    mLastHyphenation = HyphenEdit::editForNextLine(bestCandidate.hyphenType);
}

// TODO performance: could avoid populating mCandidates if greedy only
void LineBreaker::addCandidate(Candidate cand) {
    const size_t candIndex = mCandidates.size();
    mCandidates.push_back(cand);

    // mLastBreak is the index of the last line break we decided to do in mCandidates,
    // and mPreBreak is its preBreak value. mBestBreak is the index of the best line breaking
    // candidate we have found since then, and mBestScore is its penalty.
    if (cand.postBreak - mPreBreak > currentLineWidth()) {
        // This break would create an overfull line, pick the best break and break there (greedy)
        if (mBestBreak == mLastBreak) {
            // No good break has been found since last break. Break here.
            mBestBreak = candIndex;
        }
        pushGreedyBreak();
    }

    while (mLastBreak != candIndex && cand.postBreak - mPreBreak > currentLineWidth()) {
        // We should rarely come here. But if we are here, we have broken the line, but the
        // remaining part still doesn't fit. We now need to break at the second best place after the
        // last break, but we have not kept that information, so we need to go back and find it.
        //
        // In some really rare cases, postBreak - preBreak of a candidate itself may be over the
        // current line width. We protect ourselves against an infinite loop in that case by
        // checking that we have not broken the line at this candidate already.
        for (size_t i = mLastBreak + 1; i < candIndex; i++) {
            const float penalty = mCandidates[i].penalty;
            if (penalty <= mBestScore) {
                mBestBreak = i;
                mBestScore = penalty;
            }
        }
        if (mBestBreak == mLastBreak) {
            // We didn't find anything good. Break here.
            mBestBreak = candIndex;
        }
        pushGreedyBreak();
    }

    if (cand.penalty <= mBestScore) {
        mBestBreak = candIndex;
        mBestScore = cand.penalty;
    }
}

void LineBreaker::pushBreak(int offset, float width, MinikinExtent extent, uint8_t hyphenEdit) {
    mBreaks.push_back(offset);
    mWidths.push_back(width);
    mAscents.push_back(extent.ascent);
    mDescents.push_back(extent.descent);
    int flags = (mFirstTabIndex < mBreaks.back()) << kTab_Shift;
    flags |= hyphenEdit;
    mFlags.push_back(flags);
    mFirstTabIndex = INT_MAX;
}

void LineBreaker::addReplacement(size_t start, size_t end, float width) {
    mCharWidths[start] = width;
    std::fill(&mCharWidths[start + 1], &mCharWidths[end], 0.0f);
    // TODO: Get the extents information from the caller.
    std::fill(&mCharExtents[start], &mCharExtents[end], (MinikinExtent) {0.0f, 0.0f, 0.0f});
    addStyleRun(nullptr, nullptr, FontStyle(), start, end, false, nullptr,
            std::vector<Hyphenator*>());
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

float LineBreaker::currentLineWidth() const {
    return mLineWidthDelegate->getLineWidth(mBreaks.size());
}

void LineBreaker::computeBreaksGreedy() {
    // All breaks but the last have been added in addCandidate already.
    size_t nCand = mCandidates.size();
    if (nCand == 1 || mLastBreak != nCand - 1) {
        pushBreak(mCandidates[nCand - 1].offset, mCandidates[nCand - 1].postBreak - mPreBreak,
                computeMaxExtent(mLastBreak + 1, nCand - 1),
                mLastHyphenation);
        // don't need to update mBestScore, because we're done
#if VERBOSE_DEBUG
        ALOGD("final break: %d %g", mBreaks.back(), mWidths.back());
#endif
    }
}

// Follow "prev" links in mCandidates array, and copy to result arrays.
void LineBreaker::finishBreaksOptimal() {
    // clear existing greedy break result
    mBreaks.clear();
    mWidths.clear();
    mAscents.clear();
    mDescents.clear();
    mFlags.clear();

    size_t nCand = mCandidates.size();
    size_t prev;
    for (size_t i = nCand - 1; i > 0; i = prev) {
        prev = mCandidates[i].prev;
        mBreaks.push_back(mCandidates[i].offset);
        mWidths.push_back(mCandidates[i].postBreak - mCandidates[prev].preBreak);
        MinikinExtent extent = computeMaxExtent(prev + 1, i);
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

void LineBreaker::computeBreaksOptimal() {
    size_t active = 0;
    size_t nCand = mCandidates.size();
    float width = mLineWidthDelegate->getLineWidth(0);
    float maxShrink = mJustified ? SHRINKABILITY * getSpaceWidth() : 0.0f;
    std::vector<size_t> lineNumbers;
    lineNumbers.reserve(nCand);
    lineNumbers.push_back(0);  // The first candidate is always at the first line.

    // "i" iterates through candidates for the end of the line.
    for (size_t i = 1; i < nCand; i++) {
        bool atEnd = i == nCand - 1;
        float best = SCORE_INFTY;
        size_t bestPrev = 0;

        size_t lineNumberLast = lineNumbers[active];
        width = mLineWidthDelegate->getLineWidth(lineNumberLast);

        ParaWidth leftEdge = mCandidates[i].postBreak - width;
        float bestHope = 0;

        // "j" iterates through candidates for the beginning of the line.
        for (size_t j = active; j < i; j++) {
            size_t lineNumber = lineNumbers[j];
            if (lineNumber != lineNumberLast) {
                float widthNew = mLineWidthDelegate->getLineWidth(lineNumber);
                if (widthNew != width) {
                    leftEdge = mCandidates[i].postBreak - width;
                    bestHope = 0;
                    width = widthNew;
                }
                lineNumberLast = lineNumber;
            }
            float jScore = mCandidates[j].score;
            if (jScore + bestHope >= best) continue;
            float delta = mCandidates[j].preBreak - leftEdge;

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

            float score = jScore + widthScore + additionalPenalty;
            if (score <= best) {
                best = score;
                bestPrev = j;
            }
        }
        mCandidates[i].score = best + mCandidates[i].penalty + mLinePenalty;
        mCandidates[i].prev = bestPrev;
        lineNumbers.push_back(lineNumbers[bestPrev] + 1);
#if VERBOSE_DEBUG
        ALOGD("break %zd: score=%g, prev=%zd", i, mCandidates[i].score, mCandidates[i].prev);
#endif
    }
    finishBreaksOptimal();
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
    mBreaks.clear();
    mWidths.clear();
    mAscents.clear();
    mDescents.clear();
    mFlags.clear();
    if (mTextBuf.size() > MAX_TEXT_BUF_RETAIN) {
        mTextBuf.clear();
        mTextBuf.shrink_to_fit();
        mCharWidths.clear();
        mCharWidths.shrink_to_fit();
        mCharExtents.clear();
        mCharExtents.shrink_to_fit();
        mCandidates.shrink_to_fit();
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

}  // namespace minikin
