/*
 * Copyright (C) 2013 The Android Open Source Project
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

#define LOG_TAG "Minikin"

#include "minikin/Layout.h"

#include <cmath>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <hb-icu.h>
#include <hb-ot.h>
#include <log/log.h>
#include <unicode/ubidi.h>
#include <unicode/utf16.h>
#include <utils/JenkinsHash.h>
#include <utils/LruCache.h>

#include "minikin/Emoji.h"
#include "minikin/HbUtils.h"
#include "minikin/LayoutCache.h"
#include "minikin/LayoutPieces.h"
#include "minikin/Macros.h"

#include "BidiUtils.h"
#include "LayoutUtils.h"
#include "LocaleListCache.h"
#include "MinikinInternal.h"

namespace minikin {

void Layout::doLayout(const U16StringPiece& textBuf, const Range& range, Bidi bidiFlags,
                      const MinikinPaint& paint, StartHyphenEdit startHyphen,
                      EndHyphenEdit endHyphen) {
    const uint32_t count = range.getLength();
    mAdvances.resize(count, 0);
    mGlyphs.reserve(count);
    for (const BidiText::RunInfo& runInfo : BidiText(textBuf, range, bidiFlags)) {
        doLayoutRunCached(textBuf, runInfo.range, runInfo.isRtl, paint, range.getStart(),
                          startHyphen, endHyphen, nullptr, this, nullptr, nullptr, nullptr,
                          nullptr);
    }
}

void Layout::doLayoutWithPrecomputedPieces(const U16StringPiece& textBuf, const Range& range,
                                           Bidi bidiFlags, const MinikinPaint& paint,
                                           StartHyphenEdit startHyphen, EndHyphenEdit endHyphen,
                                           const LayoutPieces& lpIn) {
    const uint32_t count = range.getLength();
    mAdvances.resize(count, 0);
    mGlyphs.reserve(count);
    for (const BidiText::RunInfo& runInfo : BidiText(textBuf, range, bidiFlags)) {
        doLayoutRunCached(textBuf, runInfo.range, runInfo.isRtl, paint, range.getStart(),
                          startHyphen, endHyphen, &lpIn, this, nullptr, nullptr, nullptr, nullptr);
    }
}

std::pair<float, MinikinRect> Layout::getBoundsWithPrecomputedPieces(const U16StringPiece& textBuf,
                                                                     const Range& range,
                                                                     Bidi bidiFlags,
                                                                     const MinikinPaint& paint,
                                                                     const LayoutPieces& pieces) {
    MinikinRect rect;
    float advance = 0;
    for (const BidiText::RunInfo& runInfo : BidiText(textBuf, range, bidiFlags)) {
        advance += doLayoutRunCached(textBuf, runInfo.range, runInfo.isRtl, paint, 0,
                                     StartHyphenEdit::NO_EDIT, EndHyphenEdit::NO_EDIT, &pieces,
                                     nullptr, nullptr, nullptr, &rect, nullptr);
    }
    return std::make_pair(advance, rect);
}

MinikinExtent Layout::getExtentWithPrecomputedPieces(const U16StringPiece& textBuf,
                                                     const Range& range, Bidi bidiFlags,
                                                     const MinikinPaint& paint,
                                                     const LayoutPieces& pieces) {
    MinikinExtent extent;
    for (const BidiText::RunInfo& runInfo : BidiText(textBuf, range, bidiFlags)) {
        doLayoutRunCached(textBuf, runInfo.range, runInfo.isRtl, paint, 0, StartHyphenEdit::NO_EDIT,
                          EndHyphenEdit::NO_EDIT, &pieces, nullptr, nullptr, &extent, nullptr,
                          nullptr);
    }
    return extent;
}

float Layout::measureText(const U16StringPiece& textBuf, const Range& range, Bidi bidiFlags,
                          const MinikinPaint& paint, StartHyphenEdit startHyphen,
                          EndHyphenEdit endHyphen, float* advances, LayoutPieces* pieces) {
    float advance = 0;
    for (const BidiText::RunInfo& runInfo : BidiText(textBuf, range, bidiFlags)) {
        const size_t offset = range.toRangeOffset(runInfo.range.getStart());
        float* advancesForRun = advances ? advances + offset : nullptr;
        advance += doLayoutRunCached(textBuf, runInfo.range, runInfo.isRtl, paint, 0, startHyphen,
                                     endHyphen, nullptr, nullptr, advancesForRun, nullptr, nullptr,
                                     pieces);
    }
    return advance;
}

float Layout::doLayoutRunCached(const U16StringPiece& textBuf, const Range& range, bool isRtl,
                                const MinikinPaint& paint, size_t dstStart,
                                StartHyphenEdit startHyphen, EndHyphenEdit endHyphen,
                                const LayoutPieces* lpIn, Layout* layout, float* advances,
                                MinikinExtent* extent, MinikinRect* bounds, LayoutPieces* lpOut) {
    if (!range.isValid()) {
        return 0.0f;  // ICU failed to retrieve the bidi run?
    }
    const uint16_t* buf = textBuf.data();
    const uint32_t bufSize = textBuf.size();
    const uint32_t start = range.getStart();
    const uint32_t end = range.getEnd();
    float advance = 0;
    if (!isRtl) {
        // left to right
        uint32_t wordstart =
                start == bufSize ? start : getPrevWordBreakForCache(buf, start + 1, bufSize);
        uint32_t wordend;
        for (size_t iter = start; iter < end; iter = wordend) {
            wordend = getNextWordBreakForCache(buf, iter, bufSize);
            const uint32_t wordcount = std::min(end, wordend) - iter;
            const uint32_t offset = iter - start;
            advance +=
                    doLayoutWord(buf + wordstart, iter - wordstart, wordcount, wordend - wordstart,
                                 isRtl, paint, iter - dstStart,
                                 // Only apply hyphen to the first or last word in the string.
                                 iter == start ? startHyphen : StartHyphenEdit::NO_EDIT,
                                 wordend >= end ? endHyphen : EndHyphenEdit::NO_EDIT, lpIn, layout,
                                 advances ? advances + offset : nullptr, extent, bounds, lpOut);
            wordstart = wordend;
        }
    } else {
        // right to left
        uint32_t wordstart;
        uint32_t wordend = end == 0 ? 0 : getNextWordBreakForCache(buf, end - 1, bufSize);
        for (size_t iter = end; iter > start; iter = wordstart) {
            wordstart = getPrevWordBreakForCache(buf, iter, bufSize);
            uint32_t bufStart = std::max(start, wordstart);
            const uint32_t offset = bufStart - start;
            advance += doLayoutWord(buf + wordstart, bufStart - wordstart, iter - bufStart,
                                    wordend - wordstart, isRtl, paint, bufStart - dstStart,
                                    // Only apply hyphen to the first (rightmost) or last (leftmost)
                                    // word in the string.
                                    wordstart <= start ? startHyphen : StartHyphenEdit::NO_EDIT,
                                    iter == end ? endHyphen : EndHyphenEdit::NO_EDIT, lpIn, layout,
                                    advances ? advances + offset : nullptr, extent, bounds, lpOut);
            wordend = wordstart;
        }
    }
    return advance;
}

class LayoutAppendFunctor {
public:
    LayoutAppendFunctor(const U16StringPiece& textBuf, const Range& range,
                        const MinikinPaint& paint, bool dir, StartHyphenEdit startEdit,
                        EndHyphenEdit endEdit, Layout* layout, float* advances,
                        MinikinExtent* extent, LayoutPieces* pieces, float* totalAdvance,
                        MinikinRect* bounds, uint32_t outOffset, float wordSpacing)
            : mTextBuf(textBuf),
              mRange(range),
              mPaint(paint),
              mDir(dir),
              mStartEdit(startEdit),
              mEndEdit(endEdit),
              mLayout(layout),
              mAdvances(advances),
              mExtent(extent),
              mPieces(pieces),
              mTotalAdvance(totalAdvance),
              mBounds(bounds),
              mOutOffset(outOffset),
              mWordSpacing(wordSpacing) {}

    void operator()(const LayoutPiece& layoutPiece) {
        if (mLayout) {
            mLayout->appendLayout(layoutPiece, mOutOffset, mWordSpacing);
        }
        if (mAdvances) {
            const std::vector<float>& advances = layoutPiece.advances();
            std::copy(advances.begin(), advances.end(), mAdvances);
        }
        if (mTotalAdvance) {
            *mTotalAdvance = layoutPiece.advance();
        }
        if (mExtent) {
            mExtent->extendBy(layoutPiece.extent());
        }
        if (mBounds) {
            mBounds->join(layoutPiece.bounds());
        }
        if (mPieces) {
            mPieces->insert(mTextBuf, mRange, mPaint, mDir, mStartEdit, mEndEdit, layoutPiece);
        }
    }

private:
    const U16StringPiece& mTextBuf;
    const Range& mRange;
    const MinikinPaint& mPaint;
    bool mDir;
    StartHyphenEdit mStartEdit;
    EndHyphenEdit mEndEdit;
    Layout* mLayout;
    float* mAdvances;
    MinikinExtent* mExtent;
    LayoutPieces* mPieces;
    float* mTotalAdvance;
    MinikinRect* mBounds;
    const uint32_t mOutOffset;
    const float mWordSpacing;
};

float Layout::doLayoutWord(const uint16_t* buf, size_t start, size_t count, size_t bufSize,
                           bool isRtl, const MinikinPaint& paint, size_t bufStart,
                           StartHyphenEdit startHyphen, EndHyphenEdit endHyphen,
                           const LayoutPieces* lpIn, Layout* layout, float* advances,
                           MinikinExtent* extents, MinikinRect* bounds, LayoutPieces* lpOut) {
    float wordSpacing = count == 1 && isWordSpace(buf[start]) ? paint.wordSpacing : 0;
    float totalAdvance;

    const U16StringPiece textBuf(buf, bufSize);
    const Range range(start, start + count);
    LayoutAppendFunctor f(textBuf, range, paint, isRtl, startHyphen, endHyphen, layout, advances,
                          extents, lpOut, &totalAdvance, bounds, bufStart, wordSpacing);
    if (lpIn != nullptr) {
        lpIn->getOrCreate(textBuf, range, paint, isRtl, startHyphen, endHyphen, f);
    } else {
        LayoutCache::getInstance().getOrCreate(textBuf, range, paint, isRtl, startHyphen, endHyphen,
                                               f);
    }

    if (wordSpacing != 0) {
        totalAdvance += wordSpacing;
        if (advances) {
            advances[0] += wordSpacing;
        }
    }
    return totalAdvance;
}

void Layout::appendLayout(const LayoutPiece& src, size_t start, float extraAdvance) {
    for (size_t i = 0; i < src.glyphCount(); i++) {
        mGlyphs.emplace_back(src.fontAt(i), src.glyphIdAt(i), mAdvance + src.pointAt(i).x,
                             src.pointAt(i).y);
    }
    const std::vector<float>& advances = src.advances();
    for (size_t i = 0; i < advances.size(); i++) {
        mAdvances[i + start] = advances[i];
        if (i == 0) {
            mAdvances[start] += extraAdvance;
        }
    }
    MinikinRect srcBounds(src.bounds());
    srcBounds.offset(mAdvance, 0);
    mBounds.join(srcBounds);
    mAdvance += src.advance() + extraAdvance;
}

void Layout::purgeCaches() {
    LayoutCache::getInstance().clear();
}

void Layout::dumpMinikinStats(int fd) {
    LayoutCache::getInstance().dumpStats(fd);
}

}  // namespace minikin
