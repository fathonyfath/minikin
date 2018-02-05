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
#include <string>
#include <vector>

#include <hb-icu.h>
#include <hb-ot.h>
#include <log/log.h>
#include <unicode/ubidi.h>
#include <unicode/utf16.h>
#include <utils/JenkinsHash.h>
#include <utils/LruCache.h>
#include <utils/Singleton.h>

#include "minikin/Emoji.h"

#include "HbFontCache.h"
#include "LayoutUtils.h"
#include "LocaleListCache.h"
#include "MinikinInternal.h"

namespace minikin {

namespace {

struct SkiaArguments {
    const MinikinFont* font;
    const MinikinPaint* paint;
    FontFakery fakery;
};

}  // namespace

static inline UBiDiLevel bidiToUBidiLevel(Bidi bidi) {
    switch (bidi) {
        case Bidi::LTR:
            return 0x00;
        case Bidi::RTL:
            return 0x01;
        case Bidi::DEFAULT_LTR:
            return UBIDI_DEFAULT_LTR;
        case Bidi::DEFAULT_RTL:
            return UBIDI_DEFAULT_RTL;
        case Bidi::FORCE_LTR:
        case Bidi::FORCE_RTL:
            MINIKIN_NOT_REACHED("FORCE_LTR/FORCE_RTL can not be converted to UBiDiLevel.");
            return 0x00;
        default:
            MINIKIN_NOT_REACHED("Unknown Bidi value.");
            return 0x00;
    }
}

struct LayoutContext {
    LayoutContext(const MinikinPaint& paint) : paint(paint) {}

    const MinikinPaint& paint;

    std::vector<hb_font_t*> hbFonts;  // parallel to mFaces

    void clearHbFonts() {
        for (size_t i = 0; i < hbFonts.size(); i++) {
            hb_font_set_funcs(hbFonts[i], nullptr, nullptr, nullptr);
            hb_font_destroy(hbFonts[i]);
        }
        hbFonts.clear();
    }
};

// Layout cache datatypes

class LayoutCacheKey {
public:
    LayoutCacheKey(const MinikinPaint& paint, const uint16_t* chars, size_t start, size_t count,
                   size_t nchars, bool dir, StartHyphenEdit startHyphen, EndHyphenEdit endHyphen)
            : mChars(chars),
              mNchars(nchars),
              mStart(start),
              mCount(count),
              mId(paint.font->getId()),
              mStyle(paint.fontStyle),
              mSize(paint.size),
              mScaleX(paint.scaleX),
              mSkewX(paint.skewX),
              mLetterSpacing(paint.letterSpacing),
              mPaintFlags(paint.paintFlags),
              mLocaleListId(paint.localeListId),
              mFamilyVariant(paint.familyVariant),
              mStartHyphen(startHyphen),
              mEndHyphen(endHyphen),
              mIsRtl(dir),
              mHash(computeHash()) {}
    bool operator==(const LayoutCacheKey& other) const;

    android::hash_t hash() const { return mHash; }

    void copyText() {
        uint16_t* charsCopy = new uint16_t[mNchars];
        memcpy(charsCopy, mChars, mNchars * sizeof(uint16_t));
        mChars = charsCopy;
    }
    void freeText() {
        delete[] mChars;
        mChars = NULL;
    }

    void doLayout(Layout* layout, LayoutContext* ctx) const {
        layout->mAdvances.resize(mCount, 0);
        layout->mExtents.resize(mCount);
        ctx->clearHbFonts();
        layout->doLayoutRun(mChars, mStart, mCount, mNchars, mIsRtl, ctx, mStartHyphen, mEndHyphen);
    }

private:
    const uint16_t* mChars;
    size_t mNchars;
    size_t mStart;
    size_t mCount;
    uint32_t mId;  // for the font collection
    FontStyle mStyle;
    float mSize;
    float mScaleX;
    float mSkewX;
    float mLetterSpacing;
    int32_t mPaintFlags;
    uint32_t mLocaleListId;
    FontFamily::Variant mFamilyVariant;
    StartHyphenEdit mStartHyphen;
    EndHyphenEdit mEndHyphen;
    bool mIsRtl;
    // Note: any fields added to MinikinPaint must also be reflected here.
    // TODO: language matching (possibly integrate into style)
    android::hash_t mHash;

    android::hash_t computeHash() const;
};

class LayoutCache : private android::OnEntryRemoved<LayoutCacheKey, Layout*> {
public:
    LayoutCache() : mCache(kMaxEntries), mRequestCount(0), mCacheHitCount(0) {
        mCache.setOnEntryRemovedListener(this);
    }

    void clear() { mCache.clear(); }

    Layout* get(LayoutCacheKey& key, LayoutContext* ctx) {
        Layout* layout = mCache.get(key);
        mRequestCount++;
        if (layout == NULL) {
            key.copyText();
            layout = new Layout();
            key.doLayout(layout, ctx);
            mCache.put(key, layout);
        } else {
            mCacheHitCount++;
        }
        return layout;
    }

    void dumpStats(int fd) {
        dprintf(fd, "\nLayout Cache Info:\n");
        dprintf(fd, "  Usage: %zd/%zd entries\n", mCache.size(), kMaxEntries);
        float ratio = (mRequestCount == 0) ? 0 : mCacheHitCount / (float)mRequestCount;
        dprintf(fd, "  Hit ratio: %d/%d (%f)\n", mCacheHitCount, mRequestCount, ratio);
    }

private:
    // callback for OnEntryRemoved
    void operator()(LayoutCacheKey& key, Layout*& value) {
        key.freeText();
        delete value;
    }

    android::LruCache<LayoutCacheKey, Layout*> mCache;

    int32_t mRequestCount;
    int32_t mCacheHitCount;

    // static const size_t kMaxEntries = LruCache<LayoutCacheKey, Layout*>::kUnlimitedCapacity;

    // TODO: eviction based on memory footprint; for now, we just use a constant
    // number of strings
    static const size_t kMaxEntries = 5000;
};

static unsigned int disabledDecomposeCompatibility(hb_unicode_funcs_t*, hb_codepoint_t,
                                                   hb_codepoint_t*, void*) {
    return 0;
}

class LayoutEngine : public ::android::Singleton<LayoutEngine> {
public:
    LayoutEngine() {
        unicodeFunctions = hb_unicode_funcs_create(hb_icu_get_unicode_funcs());
        /* Disable the function used for compatibility decomposition */
        hb_unicode_funcs_set_decompose_compatibility_func(
                unicodeFunctions, disabledDecomposeCompatibility, NULL, NULL);
        hbBuffer = hb_buffer_create();
        hb_buffer_set_unicode_funcs(hbBuffer, unicodeFunctions);
    }

    hb_buffer_t* hbBuffer;
    hb_unicode_funcs_t* unicodeFunctions;
    LayoutCache layoutCache;
};

bool LayoutCacheKey::operator==(const LayoutCacheKey& other) const {
    return mId == other.mId && mStart == other.mStart && mCount == other.mCount &&
           mStyle == other.mStyle && mSize == other.mSize && mScaleX == other.mScaleX &&
           mSkewX == other.mSkewX && mLetterSpacing == other.mLetterSpacing &&
           mPaintFlags == other.mPaintFlags && mLocaleListId == other.mLocaleListId &&
           mFamilyVariant == other.mFamilyVariant && mStartHyphen == other.mStartHyphen &&
           mEndHyphen == other.mEndHyphen && mIsRtl == other.mIsRtl && mNchars == other.mNchars &&
           !memcmp(mChars, other.mChars, mNchars * sizeof(uint16_t));
}

android::hash_t LayoutCacheKey::computeHash() const {
    uint32_t hash = android::JenkinsHashMix(0, mId);
    hash = android::JenkinsHashMix(hash, mStart);
    hash = android::JenkinsHashMix(hash, mCount);
    hash = android::JenkinsHashMix(hash, android::hash_type(mStyle.identifier()));
    hash = android::JenkinsHashMix(hash, android::hash_type(mSize));
    hash = android::JenkinsHashMix(hash, android::hash_type(mScaleX));
    hash = android::JenkinsHashMix(hash, android::hash_type(mSkewX));
    hash = android::JenkinsHashMix(hash, android::hash_type(mLetterSpacing));
    hash = android::JenkinsHashMix(hash, android::hash_type(mPaintFlags));
    hash = android::JenkinsHashMix(hash, android::hash_type(mLocaleListId));
    hash = android::JenkinsHashMix(hash, android::hash_type(static_cast<uint8_t>(mFamilyVariant)));
    hash = android::JenkinsHashMix(
            hash,
            android::hash_type(static_cast<uint8_t>(packHyphenEdit(mStartHyphen, mEndHyphen))));
    hash = android::JenkinsHashMix(hash, android::hash_type(mIsRtl));
    hash = android::JenkinsHashMixShorts(hash, mChars, mNchars);
    return android::JenkinsHashWhiten(hash);
}

android::hash_t hash_type(const LayoutCacheKey& key) {
    return key.hash();
}

void MinikinRect::join(const MinikinRect& r) {
    if (isEmpty()) {
        set(r);
    } else if (!r.isEmpty()) {
        mLeft = std::min(mLeft, r.mLeft);
        mTop = std::min(mTop, r.mTop);
        mRight = std::max(mRight, r.mRight);
        mBottom = std::max(mBottom, r.mBottom);
    }
}

void Layout::reset() {
    mGlyphs.clear();
    mFaces.clear();
    mBounds.setEmpty();
    mAdvances.clear();
    mExtents.clear();
    mAdvance = 0;
}

static hb_position_t harfbuzzGetGlyphHorizontalAdvance(hb_font_t* /* hbFont */, void* fontData,
                                                       hb_codepoint_t glyph, void* /* userData */) {
    SkiaArguments* args = reinterpret_cast<SkiaArguments*>(fontData);
    float advance = args->font->GetHorizontalAdvance(glyph, *args->paint, args->fakery);
    return 256 * advance + 0.5;
}

static hb_bool_t harfbuzzGetGlyphHorizontalOrigin(hb_font_t* /* hbFont */, void* /* fontData */,
                                                  hb_codepoint_t /* glyph */,
                                                  hb_position_t* /* x */, hb_position_t* /* y */,
                                                  void* /* userData */) {
    // Just return true, following the way that Harfbuzz-FreeType
    // implementation does.
    return true;
}

hb_font_funcs_t* getHbFontFuncs(bool forColorBitmapFont) {
    assertMinikinLocked();

    static hb_font_funcs_t* hbFuncs = nullptr;
    static hb_font_funcs_t* hbFuncsForColorBitmap = nullptr;

    hb_font_funcs_t** funcs = forColorBitmapFont ? &hbFuncs : &hbFuncsForColorBitmap;
    if (*funcs == nullptr) {
        *funcs = hb_font_funcs_create();
        if (forColorBitmapFont) {
            // Don't override the h_advance function since we use HarfBuzz's implementation for
            // emoji for performance reasons.
            // Note that it is technically possible for a TrueType font to have outline and embedded
            // bitmap at the same time. We ignore modified advances of hinted outline glyphs in that
            // case.
        } else {
            // Override the h_advance function since we can't use HarfBuzz's implemenation. It may
            // return the wrong value if the font uses hinting aggressively.
            hb_font_funcs_set_glyph_h_advance_func(*funcs, harfbuzzGetGlyphHorizontalAdvance, 0, 0);
        }
        hb_font_funcs_set_glyph_h_origin_func(*funcs, harfbuzzGetGlyphHorizontalOrigin, 0, 0);
        hb_font_funcs_make_immutable(*funcs);
    }
    return *funcs;
}

static bool isColorBitmapFont(hb_font_t* font) {
    hb_face_t* face = hb_font_get_face(font);
    HbBlob cbdt(hb_face_reference_table(face, HB_TAG('C', 'B', 'D', 'T')));
    return cbdt.size() > 0;
}

static float HBFixedToFloat(hb_position_t v) {
    return scalbnf(v, -8);
}

static hb_position_t HBFloatToFixed(float v) {
    return scalbnf(v, +8);
}

void Layout::dump() const {
    for (size_t i = 0; i < mGlyphs.size(); i++) {
        const LayoutGlyph& glyph = mGlyphs[i];
        std::cout << glyph.glyph_id << ": " << glyph.x << ", " << glyph.y << std::endl;
    }
}

int Layout::findOrPushBackFace(const FakedFont& face, LayoutContext* ctx) {
    unsigned int ix;
    for (ix = 0; ix < mFaces.size(); ix++) {
        if (mFaces[ix].font == face.font) {
            return ix;
        }
    }

    mFaces.push_back(face);
    // Note: ctx == nullptr means we're copying from the cache, no need to create corresponding
    // hb_font object.
    if (ctx != nullptr) {
        hb_font_t* font = getHbFontLocked(face.font);
        hb_font_set_funcs(font, getHbFontFuncs(isColorBitmapFont(font)),
                          new SkiaArguments({face.font, &ctx->paint, face.fakery}),
                          [](void* data) { delete reinterpret_cast<SkiaArguments*>(data); });
        ctx->hbFonts.push_back(font);
    }
    return ix;
}

static hb_script_t codePointToScript(hb_codepoint_t codepoint) {
    static hb_unicode_funcs_t* u = 0;
    if (!u) {
        u = LayoutEngine::getInstance().unicodeFunctions;
    }
    return hb_unicode_script(u, codepoint);
}

static hb_codepoint_t decodeUtf16(const uint16_t* chars, size_t len, ssize_t* iter) {
    UChar32 result;
    U16_NEXT(chars, *iter, (ssize_t)len, result);
    if (U_IS_SURROGATE(result)) {  // isolated surrogate
        result = 0xFFFDu;          // U+FFFD REPLACEMENT CHARACTER
    }
    return (hb_codepoint_t)result;
}

static hb_script_t getScriptRun(const uint16_t* chars, size_t len, ssize_t* iter) {
    if (size_t(*iter) == len) {
        return HB_SCRIPT_UNKNOWN;
    }
    uint32_t cp = decodeUtf16(chars, len, iter);
    hb_script_t current_script = codePointToScript(cp);
    for (;;) {
        if (size_t(*iter) == len) break;
        const ssize_t prev_iter = *iter;
        cp = decodeUtf16(chars, len, iter);
        const hb_script_t script = codePointToScript(cp);
        if (script != current_script) {
            if (current_script == HB_SCRIPT_INHERITED || current_script == HB_SCRIPT_COMMON) {
                current_script = script;
            } else if (script == HB_SCRIPT_INHERITED || script == HB_SCRIPT_COMMON) {
                continue;
            } else {
                *iter = prev_iter;
                break;
            }
        }
    }
    if (current_script == HB_SCRIPT_INHERITED) {
        current_script = HB_SCRIPT_COMMON;
    }

    return current_script;
}

/**
 * Disable certain scripts (mostly those with cursive connection) from having letterspacing
 * applied. See https://github.com/behdad/harfbuzz/issues/64 for more details.
 */
static bool isScriptOkForLetterspacing(hb_script_t script) {
    return !(script == HB_SCRIPT_ARABIC || script == HB_SCRIPT_NKO ||
             script == HB_SCRIPT_PSALTER_PAHLAVI || script == HB_SCRIPT_MANDAIC ||
             script == HB_SCRIPT_MONGOLIAN || script == HB_SCRIPT_PHAGS_PA ||
             script == HB_SCRIPT_DEVANAGARI || script == HB_SCRIPT_BENGALI ||
             script == HB_SCRIPT_GURMUKHI || script == HB_SCRIPT_MODI ||
             script == HB_SCRIPT_SHARADA || script == HB_SCRIPT_SYLOTI_NAGRI ||
             script == HB_SCRIPT_TIRHUTA || script == HB_SCRIPT_OGHAM);
}

class BidiText {
public:
    class Iter {
    public:
        struct RunInfo {
            int32_t mRunStart;
            int32_t mRunLength;
            bool mIsRtl;
        };

        Iter(UBiDi* bidi, size_t start, size_t end, size_t runIndex, size_t runCount, bool isRtl);

        bool operator!=(const Iter& other) const {
            return mIsEnd != other.mIsEnd || mNextRunIndex != other.mNextRunIndex ||
                   mBidi != other.mBidi;
        }

        const RunInfo& operator*() const { return mRunInfo; }

        const Iter& operator++() {
            updateRunInfo();
            return *this;
        }

    private:
        UBiDi* const mBidi;
        bool mIsEnd;
        size_t mNextRunIndex;
        const size_t mRunCount;
        const int32_t mStart;
        const int32_t mEnd;
        RunInfo mRunInfo;

        void updateRunInfo();
    };

    BidiText(const uint16_t* buf, size_t start, size_t count, size_t bufSize, Bidi bidiFlags);
    BidiText(const U16StringPiece& textBuf, const Range& range, Bidi bidiFlags)
            : BidiText(textBuf.data(), range.getStart(), range.getLength(), textBuf.size(),
                       bidiFlags){};

    ~BidiText() {
        if (mBidi) {
            ubidi_close(mBidi);
        }
    }

    Iter begin() const { return Iter(mBidi, mStart, mEnd, 0, mRunCount, mIsRtl); }

    Iter end() const { return Iter(mBidi, mStart, mEnd, mRunCount, mRunCount, mIsRtl); }

private:
    const size_t mStart;
    const size_t mEnd;
    const size_t mBufSize;
    UBiDi* mBidi;
    size_t mRunCount;
    bool mIsRtl;

    BidiText(const BidiText&) = delete;
    void operator=(const BidiText&) = delete;
};

BidiText::Iter::Iter(UBiDi* bidi, size_t start, size_t end, size_t runIndex, size_t runCount,
                     bool isRtl)
        : mBidi(bidi),
          mIsEnd(runIndex == runCount),
          mNextRunIndex(runIndex),
          mRunCount(runCount),
          mStart(start),
          mEnd(end),
          mRunInfo() {
    if (mRunCount == 1) {
        mRunInfo.mRunStart = start;
        mRunInfo.mRunLength = end - start;
        mRunInfo.mIsRtl = isRtl;
        mNextRunIndex = mRunCount;
        return;
    }
    updateRunInfo();
}

void BidiText::Iter::updateRunInfo() {
    if (mNextRunIndex == mRunCount) {
        // All runs have been iterated.
        mIsEnd = true;
        return;
    }
    int32_t startRun = -1;
    int32_t lengthRun = -1;
    const UBiDiDirection runDir = ubidi_getVisualRun(mBidi, mNextRunIndex, &startRun, &lengthRun);
    mNextRunIndex++;
    if (startRun == -1 || lengthRun == -1) {
        ALOGE("invalid visual run");
        // skip the invalid run.
        updateRunInfo();
        return;
    }
    const int32_t runEnd = std::min(startRun + lengthRun, mEnd);
    mRunInfo.mRunStart = std::max(startRun, mStart);
    mRunInfo.mRunLength = runEnd - mRunInfo.mRunStart;
    if (mRunInfo.mRunLength <= 0) {
        // skip the empty run.
        updateRunInfo();
        return;
    }
    mRunInfo.mIsRtl = (runDir == UBIDI_RTL);
}

BidiText::BidiText(const uint16_t* buf, size_t start, size_t count, size_t bufSize, Bidi bidiFlags)
        : mStart(start),
          mEnd(start + count),
          mBufSize(bufSize),
          mBidi(NULL),
          mRunCount(1),
          mIsRtl(isRtl(bidiFlags)) {
    if (isOverride(bidiFlags)) {
        // force single run.
        return;
    }
    mBidi = ubidi_open();
    if (!mBidi) {
        ALOGE("error creating bidi object");
        return;
    }
    UErrorCode status = U_ZERO_ERROR;
    // Set callbacks to override bidi classes of new emoji
    ubidi_setClassCallback(mBidi, emojiBidiOverride, nullptr, nullptr, nullptr, &status);
    if (!U_SUCCESS(status)) {
        ALOGE("error setting bidi callback function, status = %d", status);
        return;
    }

    const UBiDiLevel bidiReq = bidiToUBidiLevel(bidiFlags);

    ubidi_setPara(mBidi, reinterpret_cast<const UChar*>(buf), mBufSize, bidiReq, NULL, &status);
    if (!U_SUCCESS(status)) {
        ALOGE("error calling ubidi_setPara, status = %d", status);
        return;
    }
    // RTL paragraphs get an odd level, while LTR paragraphs get an even level,
    const bool paraIsRTL = ubidi_getParaLevel(mBidi) & 0x01;
    const ssize_t rc = ubidi_countRuns(mBidi, &status);
    if (!U_SUCCESS(status) || rc < 0) {
        ALOGW("error counting bidi runs, status = %d", status);
    }
    if (!U_SUCCESS(status) || rc <= 0) {
        mIsRtl = paraIsRTL;
        return;
    }
    if (rc == 1) {
        const UBiDiDirection runDir = ubidi_getVisualRun(mBidi, 0, nullptr, nullptr);
        mIsRtl = (runDir == UBIDI_RTL);
        return;
    }
    mRunCount = rc;
}

void Layout::doLayout(const U16StringPiece& textBuf, const Range& range, Bidi bidiFlags,
                      const MinikinPaint& paint, StartHyphenEdit startHyphen,
                      EndHyphenEdit endHyphen) {
    android::AutoMutex _l(gMinikinLock);

    LayoutContext ctx(paint);

    const uint32_t count = range.getLength();
    reset();
    mAdvances.resize(count, 0);
    mExtents.resize(count);

    for (const BidiText::Iter::RunInfo& runInfo : BidiText(textBuf, range, bidiFlags)) {
        doLayoutRunCached(textBuf.data(), runInfo.mRunStart, runInfo.mRunLength, textBuf.size(),
                          runInfo.mIsRtl, &ctx, range.getStart(), startHyphen, endHyphen, this,
                          nullptr, nullptr, nullptr);
    }
    ctx.clearHbFonts();
}

// static
void Layout::addToLayoutPieces(const U16StringPiece& textBuf, const Range& range, Bidi bidiFlag,
                               const MinikinPaint& paint,
                               LayoutPieces* out) {
    android::AutoMutex _l(gMinikinLock);
    LayoutContext ctx(paint);

    float advance = 0;
    for (const BidiText::Iter::RunInfo& runInfo : BidiText(textBuf, range, bidiFlag)) {
        advance += doLayoutRunCached(textBuf.data(), runInfo.mRunStart, runInfo.mRunLength,
                                     textBuf.size(), runInfo.mIsRtl, &ctx,
                                     0,                         // Destination start. Not used.
                                     StartHyphenEdit::NO_EDIT,  // Hyphen edit, not used.
                                     EndHyphenEdit::NO_EDIT,    // Hyphen edit, not used.
                                     nullptr,  // output layout. Not used
                                     nullptr,  // advances. Not used
                                     nullptr,  // extents. Not used.
                                     out);
    }

    ctx.clearHbFonts();
}

float Layout::measureText(const U16StringPiece& textBuf, const Range& range, Bidi bidiFlags,
                          const MinikinPaint& paint, StartHyphenEdit startHyphen,
                          EndHyphenEdit endHyphen, float* advances, MinikinExtent* extents) {
    android::AutoMutex _l(gMinikinLock);

    LayoutContext ctx(paint);

    float advance = 0;
    for (const BidiText::Iter::RunInfo& runInfo : BidiText(textBuf, range, bidiFlags)) {
        const size_t offset = range.toRangeOffset(runInfo.mRunStart);
        float* advancesForRun = advances ? advances + offset : nullptr;
        MinikinExtent* extentsForRun = extents ? extents + offset : nullptr;
        advance += doLayoutRunCached(textBuf.data(), runInfo.mRunStart, runInfo.mRunLength,
                                     textBuf.size(), runInfo.mIsRtl, &ctx, 0, startHyphen,
                                     endHyphen, NULL, advancesForRun, extentsForRun, nullptr);
    }

    ctx.clearHbFonts();
    return advance;
}

float Layout::doLayoutRunCached(const uint16_t* buf, size_t start, size_t count, size_t bufSize,
                                bool isRtl, LayoutContext* ctx, size_t dstStart,
                                StartHyphenEdit startHyphen, EndHyphenEdit endHyphen,
                                Layout* layout, float* advances, MinikinExtent* extents,
                                LayoutPieces* lpOut) {
    float advance = 0;
    if (!isRtl) {
        // left to right
        size_t wordstart =
                start == bufSize ? start : getPrevWordBreakForCache(buf, start + 1, bufSize);
        size_t wordend;
        for (size_t iter = start; iter < start + count; iter = wordend) {
            wordend = getNextWordBreakForCache(buf, iter, bufSize);
            const size_t wordcount = std::min(start + count, wordend) - iter;
            const size_t offset = iter - start;
            advance += doLayoutWord(buf + wordstart, iter - wordstart, wordcount,
                                    wordend - wordstart, isRtl, ctx, iter - dstStart,
                                    // Only apply hyphen to the first or last word in the string.
                                    iter == start ? startHyphen : StartHyphenEdit::NO_EDIT,
                                    wordend >= start + count ? endHyphen : EndHyphenEdit::NO_EDIT,
                                    layout, advances ? advances + offset : nullptr,
                                    extents ? extents + offset : nullptr, lpOut);
            wordstart = wordend;
        }
    } else {
        // right to left
        size_t wordstart;
        size_t end = start + count;
        size_t wordend = end == 0 ? 0 : getNextWordBreakForCache(buf, end - 1, bufSize);
        for (size_t iter = end; iter > start; iter = wordstart) {
            wordstart = getPrevWordBreakForCache(buf, iter, bufSize);
            size_t bufStart = std::max(start, wordstart);
            const size_t offset = bufStart - start;
            advance += doLayoutWord(buf + wordstart, bufStart - wordstart, iter - bufStart,
                                    wordend - wordstart, isRtl, ctx, bufStart - dstStart,
                                    // Only apply hyphen to the first (rightmost) or last (leftmost)
                                    // word in the string.
                                    wordstart <= start ? startHyphen : StartHyphenEdit::NO_EDIT,
                                    iter == end ? endHyphen : EndHyphenEdit::NO_EDIT, layout,
                                    advances ? advances + offset : nullptr,
                                    extents ? extents + offset : nullptr, lpOut);
            wordend = wordstart;
        }
    }
    return advance;
}

float Layout::doLayoutWord(const uint16_t* buf, size_t start, size_t count, size_t bufSize,
                           bool isRtl, LayoutContext* ctx, size_t bufStart,
                           StartHyphenEdit startHyphen, EndHyphenEdit endHyphen, Layout* layout,
                           float* advances, MinikinExtent* extents, LayoutPieces* lpOut) {
    LayoutCache& cache = LayoutEngine::getInstance().layoutCache;
    LayoutCacheKey key(ctx->paint, buf, start, count, bufSize, isRtl, startHyphen, endHyphen);

    float wordSpacing = count == 1 && isWordSpace(buf[start]) ? ctx->paint.wordSpacing : 0;

    float advance;
    if (ctx->paint.skipCache()) {
        Layout layoutForWord;
        key.doLayout(&layoutForWord, ctx);
        if (layout) {
            layout->appendLayout(&layoutForWord, bufStart, wordSpacing);
        }
        if (advances) {
            layoutForWord.getAdvances(advances);
        }
        if (extents) {
            layoutForWord.getExtents(extents);
        }
        advance = layoutForWord.getAdvance();
        if (lpOut != nullptr) {
            lpOut->offsetMap.insert(std::make_pair(bufStart, layoutForWord));
        }
    } else {
        Layout* layoutForWord = cache.get(key, ctx);
        if (layout) {
            layout->appendLayout(layoutForWord, bufStart, wordSpacing);
        }
        if (advances) {
            layoutForWord->getAdvances(advances);
        }
        if (extents) {
            layoutForWord->getExtents(extents);
        }
        advance = layoutForWord->getAdvance();
        if (lpOut != nullptr) {
            lpOut->offsetMap.insert(std::make_pair(bufStart, *layoutForWord));
        }
    }

    if (wordSpacing != 0) {
        advance += wordSpacing;
        if (advances) {
            advances[0] += wordSpacing;
        }
    }
    return advance;
}

static void addFeatures(const std::string& str, std::vector<hb_feature_t>* features) {
    SplitIterator it(str, ',');
    while (it.hasNext()) {
        StringPiece featureStr = it.next();
        static hb_feature_t feature;
        /* We do not allow setting features on ranges.  As such, reject any
         * setting that has non-universal range. */
        if (hb_feature_from_string(featureStr.data(), featureStr.size(), &feature) &&
            feature.start == 0 && feature.end == (unsigned int)-1) {
            features->push_back(feature);
        }
    }
}

static inline hb_codepoint_t determineHyphenChar(hb_codepoint_t preferredHyphen, hb_font_t* font) {
    hb_codepoint_t glyph;
    if (preferredHyphen == 0x058A    /* ARMENIAN_HYPHEN */
        || preferredHyphen == 0x05BE /* HEBREW PUNCTUATION MAQAF */
        || preferredHyphen == 0x1400 /* CANADIAN SYLLABIC HYPHEN */) {
        if (hb_font_get_nominal_glyph(font, preferredHyphen, &glyph)) {
            return preferredHyphen;
        } else {
            // The original hyphen requested was not supported. Let's try and see if the
            // Unicode hyphen is supported.
            preferredHyphen = CHAR_HYPHEN;
        }
    }
    if (preferredHyphen == CHAR_HYPHEN) { /* HYPHEN */
        // Fallback to ASCII HYPHEN-MINUS if the font didn't have a glyph for the preferred hyphen.
        // Note that we intentionally don't do anything special if the font doesn't have a
        // HYPHEN-MINUS either, so a tofu could be shown, hinting towards something missing.
        if (!hb_font_get_nominal_glyph(font, preferredHyphen, &glyph)) {
            return 0x002D;  // HYPHEN-MINUS
        }
    }
    return preferredHyphen;
}

template <typename HyphenEdit>
static inline void addHyphenToHbBuffer(hb_buffer_t* buffer, hb_font_t* font, HyphenEdit hyphen,
                                       uint32_t cluster) {
    const uint32_t* chars;
    size_t size;
    std::tie(chars, size) = getHyphenString(hyphen);
    for (size_t i = 0; i < size; i++) {
        hb_buffer_add(buffer, determineHyphenChar(chars[i], font), cluster);
    }
}

// Returns the cluster value assigned to the first codepoint added to the buffer, which can be used
// to translate cluster values returned by HarfBuzz to input indices.
static inline uint32_t addToHbBuffer(hb_buffer_t* buffer, const uint16_t* buf, size_t start,
                                     size_t count, size_t bufSize, ssize_t scriptRunStart,
                                     ssize_t scriptRunEnd, StartHyphenEdit inStartHyphen,
                                     EndHyphenEdit inEndHyphen, hb_font_t* hbFont) {
    // Only hyphenate the very first script run for starting hyphens.
    const StartHyphenEdit startHyphen =
            (scriptRunStart == 0) ? inStartHyphen : StartHyphenEdit::NO_EDIT;
    // Only hyphenate the very last script run for ending hyphens.
    const EndHyphenEdit endHyphen =
            (static_cast<size_t>(scriptRunEnd) == count) ? inEndHyphen : EndHyphenEdit::NO_EDIT;

    // In the following code, we drop the pre-context and/or post-context if there is a
    // hyphen edit at that end. This is not absolutely necessary, since HarfBuzz uses
    // contexts only for joining scripts at the moment, e.g. to determine if the first or
    // last letter of a text range to shape should take a joining form based on an
    // adjacent letter or joiner (that comes from the context).
    //
    // TODO: Revisit this for:
    // 1. Desperate breaks for joining scripts like Arabic (where it may be better to keep
    //    the context);
    // 2. Special features like start-of-word font features (not implemented in HarfBuzz
    //    yet).

    // We don't have any start-of-line replacement edit yet, so we don't need to check for
    // those.
    if (isInsertion(startHyphen)) {
        // A cluster value of zero guarantees that the inserted hyphen will be in the same
        // cluster with the next codepoint, since there is no pre-context.
        addHyphenToHbBuffer(buffer, hbFont, startHyphen, 0 /* cluster */);
    }

    const uint16_t* hbText;
    int hbTextLength;
    unsigned int hbItemOffset;
    unsigned int hbItemLength = scriptRunEnd - scriptRunStart;  // This is >= 1.

    const bool hasEndInsertion = isInsertion(endHyphen);
    const bool hasEndReplacement = isReplacement(endHyphen);
    if (hasEndReplacement) {
        // Skip the last code unit while copying the buffer for HarfBuzz if it's a replacement. We
        // don't need to worry about non-BMP characters yet since replacements are only done for
        // code units at the moment.
        hbItemLength -= 1;
    }

    if (startHyphen == StartHyphenEdit::NO_EDIT) {
        // No edit at the beginning. Use the whole pre-context.
        hbText = buf;
        hbItemOffset = start + scriptRunStart;
    } else {
        // There's an edit at the beginning. Drop the pre-context and start the buffer at where we
        // want to start shaping.
        hbText = buf + start + scriptRunStart;
        hbItemOffset = 0;
    }

    if (endHyphen == EndHyphenEdit::NO_EDIT) {
        // No edit at the end, use the whole post-context.
        hbTextLength = (buf + bufSize) - hbText;
    } else {
        // There is an edit at the end. Drop the post-context.
        hbTextLength = hbItemOffset + hbItemLength;
    }

    hb_buffer_add_utf16(buffer, hbText, hbTextLength, hbItemOffset, hbItemLength);

    unsigned int numCodepoints;
    hb_glyph_info_t* cpInfo = hb_buffer_get_glyph_infos(buffer, &numCodepoints);

    // Add the hyphen at the end, if there's any.
    if (hasEndInsertion || hasEndReplacement) {
        // When a hyphen is inserted, by assigning the added hyphen and the last
        // codepoint added to the HarfBuzz buffer to the same cluster, we can make sure
        // that they always remain in the same cluster, even if the last codepoint gets
        // merged into another cluster (for example when it's a combining mark).
        //
        // When a replacement happens instead, we want it to get the cluster value of
        // the character it's replacing, which is one "codepoint length" larger than
        // the last cluster. But since the character replaced is always just one
        // code unit, we can just add 1.
        uint32_t hyphenCluster;
        if (numCodepoints == 0) {
            // Nothing was added to the HarfBuzz buffer. This can only happen if
            // we have a replacement that is replacing a one-code unit script run.
            hyphenCluster = 0;
        } else {
            hyphenCluster = cpInfo[numCodepoints - 1].cluster + (uint32_t)hasEndReplacement;
        }
        addHyphenToHbBuffer(buffer, hbFont, endHyphen, hyphenCluster);
        // Since we have just added to the buffer, cpInfo no longer necessarily points to
        // the right place. Refresh it.
        cpInfo = hb_buffer_get_glyph_infos(buffer, nullptr /* we don't need the size */);
    }
    return cpInfo[0].cluster;
}

void Layout::doLayoutRun(const uint16_t* buf, size_t start, size_t count, size_t bufSize,
                         bool isRtl, LayoutContext* ctx, StartHyphenEdit startHyphen,
                         EndHyphenEdit endHyphen) {
    hb_buffer_t* buffer = LayoutEngine::getInstance().hbBuffer;
    std::vector<FontCollection::Run> items;
    ctx->paint.font->itemize(buf + start, count, ctx->paint, &items);

    std::vector<hb_feature_t> features;
    // Disable default-on non-required ligature features if letter-spacing
    // See http://dev.w3.org/csswg/css-text-3/#letter-spacing-property
    // "When the effective spacing between two characters is not zero (due to
    // either justification or a non-zero value of letter-spacing), user agents
    // should not apply optional ligatures."
    if (fabs(ctx->paint.letterSpacing) > 0.03) {
        static const hb_feature_t no_liga = {HB_TAG('l', 'i', 'g', 'a'), 0, 0, ~0u};
        static const hb_feature_t no_clig = {HB_TAG('c', 'l', 'i', 'g'), 0, 0, ~0u};
        features.push_back(no_liga);
        features.push_back(no_clig);
    }
    addFeatures(ctx->paint.fontFeatureSettings, &features);

    double size = ctx->paint.size;
    double scaleX = ctx->paint.scaleX;

    float x = mAdvance;
    float y = 0;
    for (int run_ix = isRtl ? items.size() - 1 : 0;
         isRtl ? run_ix >= 0 : run_ix < static_cast<int>(items.size());
         isRtl ? --run_ix : ++run_ix) {
        FontCollection::Run& run = items[run_ix];
        const FakedFont& fakedFont = run.fakedFont;
        if (fakedFont.font == NULL) {
            ALOGE("no font for run starting u+%04x length %d", buf[run.start], run.end - run.start);
            continue;
        }
        const int font_ix = findOrPushBackFace(fakedFont, ctx);
        hb_font_t* hbFont = ctx->hbFonts[font_ix];

        MinikinExtent verticalExtent;
        fakedFont.font->GetFontExtent(&verticalExtent, ctx->paint, fakedFont.fakery);
        std::fill(&mExtents[run.start], &mExtents[run.end], verticalExtent);

        hb_font_set_ppem(hbFont, size * scaleX, size);
        hb_font_set_scale(hbFont, HBFloatToFixed(size * scaleX), HBFloatToFixed(size));

        const bool is_color_bitmap_font = isColorBitmapFont(hbFont);

        // TODO: if there are multiple scripts within a font in an RTL run,
        // we need to reorder those runs. This is unlikely with our current
        // font stack, but should be done for correctness.

        // Note: scriptRunStart and scriptRunEnd, as well as run.start and run.end, run between 0
        // and count.
        ssize_t scriptRunEnd;
        for (ssize_t scriptRunStart = run.start; scriptRunStart < run.end;
             scriptRunStart = scriptRunEnd) {
            scriptRunEnd = scriptRunStart;
            hb_script_t script = getScriptRun(buf + start, run.end, &scriptRunEnd /* iterator */);
            // After the last line, scriptRunEnd is guaranteed to have increased, since the only
            // time getScriptRun does not increase its iterator is when it has already reached the
            // end of the buffer. But that can't happen, since if we have already reached the end
            // of the buffer, we should have had (scriptRunEnd == run.end), which means
            // (scriptRunStart == run.end) which is impossible due to the exit condition of the for
            // loop. So we can be sure that scriptRunEnd > scriptRunStart.

            double letterSpace = 0.0;
            double letterSpaceHalfLeft = 0.0;
            double letterSpaceHalfRight = 0.0;

            if (ctx->paint.letterSpacing != 0.0 && isScriptOkForLetterspacing(script)) {
                letterSpace = ctx->paint.letterSpacing * size * scaleX;
                if ((ctx->paint.paintFlags & LinearTextFlag) == 0) {
                    letterSpace = round(letterSpace);
                    letterSpaceHalfLeft = floor(letterSpace * 0.5);
                } else {
                    letterSpaceHalfLeft = letterSpace * 0.5;
                }
                letterSpaceHalfRight = letterSpace - letterSpaceHalfLeft;
            }

            hb_buffer_clear_contents(buffer);
            hb_buffer_set_script(buffer, script);
            hb_buffer_set_direction(buffer, isRtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
            const LocaleList& localeList = LocaleListCache::getById(ctx->paint.localeListId);
            if (localeList.size() != 0) {
                hb_language_t hbLanguage = localeList.getHbLanguage(0);
                for (size_t i = 0; i < localeList.size(); ++i) {
                    if (localeList[i].supportsHbScript(script)) {
                        hbLanguage = localeList.getHbLanguage(i);
                        break;
                    }
                }
                hb_buffer_set_language(buffer, hbLanguage);
            }

            const uint32_t clusterStart =
                    addToHbBuffer(buffer, buf, start, count, bufSize, scriptRunStart, scriptRunEnd,
                                  startHyphen, endHyphen, hbFont);

            hb_shape(hbFont, buffer, features.empty() ? NULL : &features[0], features.size());
            unsigned int numGlyphs;
            hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &numGlyphs);
            hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, NULL);

            // At this point in the code, the cluster values in the info buffer correspond to the
            // input characters with some shift. The cluster value clusterStart corresponds to the
            // first character passed to HarfBuzz, which is at buf[start + scriptRunStart] whose
            // advance needs to be saved into mAdvances[scriptRunStart]. So cluster values need to
            // be reduced by (clusterStart - scriptRunStart) to get converted to indices of
            // mAdvances.
            const ssize_t clusterOffset = clusterStart - scriptRunStart;

            if (numGlyphs) {
                mAdvances[info[0].cluster - clusterOffset] += letterSpaceHalfLeft;
                x += letterSpaceHalfLeft;
            }
            for (unsigned int i = 0; i < numGlyphs; i++) {
                const size_t clusterBaseIndex = info[i].cluster - clusterOffset;
                if (i > 0 && info[i - 1].cluster != info[i].cluster) {
                    mAdvances[info[i - 1].cluster - clusterOffset] += letterSpaceHalfRight;
                    mAdvances[clusterBaseIndex] += letterSpaceHalfLeft;
                    x += letterSpace;
                }

                hb_codepoint_t glyph_ix = info[i].codepoint;
                float xoff = HBFixedToFloat(positions[i].x_offset);
                float yoff = -HBFixedToFloat(positions[i].y_offset);
                xoff += yoff * ctx->paint.skewX;
                LayoutGlyph glyph = {font_ix, glyph_ix, x + xoff, y + yoff};
                mGlyphs.push_back(glyph);
                float xAdvance = HBFixedToFloat(positions[i].x_advance);
                if ((ctx->paint.paintFlags & LinearTextFlag) == 0) {
                    xAdvance = roundf(xAdvance);
                }
                MinikinRect glyphBounds;
                hb_glyph_extents_t extents = {};
                if (is_color_bitmap_font && hb_font_get_glyph_extents(hbFont, glyph_ix, &extents)) {
                    // Note that it is technically possible for a TrueType font to have outline and
                    // embedded bitmap at the same time. We ignore modified bbox of hinted outline
                    // glyphs in that case.
                    glyphBounds.mLeft = roundf(HBFixedToFloat(extents.x_bearing));
                    glyphBounds.mTop = roundf(HBFixedToFloat(-extents.y_bearing));
                    glyphBounds.mRight = roundf(HBFixedToFloat(extents.x_bearing + extents.width));
                    glyphBounds.mBottom =
                            roundf(HBFixedToFloat(-extents.y_bearing - extents.height));
                } else {
                    fakedFont.font->GetBounds(&glyphBounds, glyph_ix, ctx->paint, fakedFont.fakery);
                }
                glyphBounds.offset(xoff, yoff);

                if (clusterBaseIndex < count) {
                    mAdvances[clusterBaseIndex] += xAdvance;
                } else {
                    ALOGE("cluster %zu (start %zu) out of bounds of count %zu", clusterBaseIndex,
                          start, count);
                }
                glyphBounds.offset(x, y);
                mBounds.join(glyphBounds);
                x += xAdvance;
            }
            if (numGlyphs) {
                mAdvances[info[numGlyphs - 1].cluster - clusterOffset] += letterSpaceHalfRight;
                x += letterSpaceHalfRight;
            }
        }
    }
    mAdvance = x;
}

void Layout::appendLayout(Layout* src, size_t start, float extraAdvance) {
    int fontMapStack[16];
    int* fontMap;
    if (src->mFaces.size() < sizeof(fontMapStack) / sizeof(fontMapStack[0])) {
        fontMap = fontMapStack;
    } else {
        fontMap = new int[src->mFaces.size()];
    }
    for (size_t i = 0; i < src->mFaces.size(); i++) {
        int font_ix = findOrPushBackFace(src->mFaces[i], nullptr);
        fontMap[i] = font_ix;
    }
    int x0 = mAdvance;
    for (size_t i = 0; i < src->mGlyphs.size(); i++) {
        LayoutGlyph& srcGlyph = src->mGlyphs[i];
        int font_ix = fontMap[srcGlyph.font_ix];
        unsigned int glyph_id = srcGlyph.glyph_id;
        float x = x0 + srcGlyph.x;
        float y = srcGlyph.y;
        LayoutGlyph glyph = {font_ix, glyph_id, x, y};
        mGlyphs.push_back(glyph);
    }
    for (size_t i = 0; i < src->mAdvances.size(); i++) {
        mAdvances[i + start] = src->mAdvances[i];
        if (i == 0) {
            mAdvances[start] += extraAdvance;
        }
        mExtents[i + start] = src->mExtents[i];
    }
    MinikinRect srcBounds(src->mBounds);
    srcBounds.offset(x0, 0);
    mBounds.join(srcBounds);
    mAdvance += src->mAdvance + extraAdvance;

    if (fontMap != fontMapStack) {
        delete[] fontMap;
    }
}

size_t Layout::nGlyphs() const {
    return mGlyphs.size();
}

const MinikinFont* Layout::getFont(int i) const {
    const LayoutGlyph& glyph = mGlyphs[i];
    return mFaces[glyph.font_ix].font;
}

FontFakery Layout::getFakery(int i) const {
    const LayoutGlyph& glyph = mGlyphs[i];
    return mFaces[glyph.font_ix].fakery;
}

unsigned int Layout::getGlyphId(int i) const {
    const LayoutGlyph& glyph = mGlyphs[i];
    return glyph.glyph_id;
}

float Layout::getX(int i) const {
    const LayoutGlyph& glyph = mGlyphs[i];
    return glyph.x;
}

float Layout::getY(int i) const {
    const LayoutGlyph& glyph = mGlyphs[i];
    return glyph.y;
}

float Layout::getAdvance() const {
    return mAdvance;
}

void Layout::getAdvances(float* advances) {
    memcpy(advances, &mAdvances[0], mAdvances.size() * sizeof(float));
}

void Layout::getExtents(MinikinExtent* extents) {
    memcpy(extents, &mExtents[0], mExtents.size() * sizeof(MinikinExtent));
}

void Layout::getBounds(MinikinRect* bounds) const {
    bounds->set(mBounds);
}

void Layout::purgeCaches() {
    android::AutoMutex _l(gMinikinLock);
    LayoutCache& layoutCache = LayoutEngine::getInstance().layoutCache;
    layoutCache.clear();
    purgeHbFontCacheLocked();
}

void Layout::dumpMinikinStats(int fd) {
    android::AutoMutex _l(gMinikinLock);
    LayoutEngine::getInstance().layoutCache.dumpStats(fd);
}

}  // namespace minikin

// Unable to define the static data member outside of android.
// TODO: introduce our own Singleton to drop android namespace.
namespace android {
ANDROID_SINGLETON_STATIC_INSTANCE(minikin::LayoutEngine);
}  // namespace android
