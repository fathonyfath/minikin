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

#ifndef MINIKIN_LAYOUT_H
#define MINIKIN_LAYOUT_H

#include <memory>
#include <unordered_map>
#include <vector>

#include <gtest/gtest_prod.h>

#include "minikin/FontCollection.h"
#include "minikin/Range.h"
#include "minikin/U16StringPiece.h"

namespace minikin {

class Layout;
struct LayoutPieces;

struct LayoutGlyph {
    // index into mFaces and mHbFonts vectors. We could imagine
    // moving this into a run length representation, because it's
    // more efficient for long strings, and we'll probably need
    // something like that for paint attributes (color, underline,
    // fake b/i, etc), as having those per-glyph is bloated.
    int font_ix;

    unsigned int glyph_id;
    float x;
    float y;
};

// Must be the same value with Paint.java
enum class Bidi : uint8_t {
    LTR = 0b0000,          // Must be same with Paint.BIDI_LTR
    RTL = 0b0001,          // Must be same with Paint.BIDI_RTL
    DEFAULT_LTR = 0b0010,  // Must be same with Paint.BIDI_DEFAULT_LTR
    DEFAULT_RTL = 0b0011,  // Must be same with Paint.BIDI_DEFAULT_RTL
    FORCE_LTR = 0b0100,    // Must be same with Paint.BIDI_FORCE_LTR
    FORCE_RTL = 0b0101,    // Must be same with Paint.BIDI_FORCE_RTL
};

inline bool isRtl(Bidi bidi) {
    return static_cast<uint8_t>(bidi) & 0b0001;
}
inline bool isOverride(Bidi bidi) {
    return static_cast<uint8_t>(bidi) & 0b0100;
}

// Lifecycle and threading assumptions for Layout:
// The object is assumed to be owned by a single thread; multiple threads
// may not mutate it at the same time.
class Layout {
public:
    Layout()
            : mGlyphs(),
              mAdvances(),
              mExtents(),
              mFaces(),
              mAdvance(0),
              mBounds() {
        mBounds.setEmpty();
    }

    Layout(Layout&& layout) = default;

    Layout(const Layout&) = default;
    Layout& operator=(const Layout&) = default;

    void dump() const;

    void doLayout(const U16StringPiece& str, const Range& range, Bidi bidiFlags,
                  const MinikinPaint& paint, StartHyphenEdit startHyphen, EndHyphenEdit endHyphen);

    static void addToLayoutPieces(const U16StringPiece& textBuf, const Range& range, Bidi bidiFlag,
                                  const MinikinPaint& paint, LayoutPieces* out);

    static float measureText(const U16StringPiece& str, const Range& range, Bidi bidiFlags,
                             const MinikinPaint& paint, StartHyphenEdit startHyphen,
                             EndHyphenEdit endHyphen, float* advances, MinikinExtent* extents);

    inline const std::vector<float>& advances() const { return mAdvances; }

    // public accessors
    size_t nGlyphs() const;
    const MinikinFont* getFont(int i) const;
    FontFakery getFakery(int i) const;
    unsigned int getGlyphId(int i) const;
    float getX(int i) const;
    float getY(int i) const;

    float getAdvance() const;

    // Get advances, copying into caller-provided buffer. The size of this
    // buffer must match the length of the string (count arg to doLayout).
    void getAdvances(float* advances) const;

    // Get extents, copying into caller-provided buffer. The size of this buffer must match the
    // length of the string (count arg to doLayout).
    void getExtents(MinikinExtent* extents) const;

    // The i parameter is an offset within the buf relative to start, it is < count, where
    // start and count are the parameters to doLayout
    float getCharAdvance(size_t i) const { return mAdvances[i]; }

    void getBounds(MinikinRect* rect) const;

    // Purge all caches, useful in low memory conditions
    static void purgeCaches();

    // Dump minikin internal statistics, cache usage, cache hit ratio, etc.
    static void dumpMinikinStats(int fd);

    uint32_t getMemoryUsage() const {
        return sizeof(LayoutGlyph) * nGlyphs() + sizeof(float) * mAdvances.size() +
               sizeof(MinikinExtent) * mExtents.size() + sizeof(FakedFont) * mFaces.size() +
               sizeof(float /* mAdvance */) + sizeof(MinikinRect /* mBounds */);
    }

    // Append another layout (for example, cached value) into this one
    void appendLayout(const Layout& src, size_t start, float extraAdvance);

private:
    friend class LayoutCacheKey;
    friend class LayoutCache;

    // TODO: Remove friend class with decoupling building logic from Layout.
    friend class LayoutCompositer;

    // TODO: Remove friend test by doing text layout in unit test.
    FRIEND_TEST(MeasuredTextTest, buildLayoutTest);

    // Find a face in the mFaces vector. If not found, push back the entry to mFaces.
    uint8_t findOrPushBackFace(const FakedFont& face);

    // Clears layout, ready to be used again
    void reset();

    // Lay out a single bidi run
    // When layout is not null, layout info will be stored in the object.
    // When advances is not null, measurement results will be stored in the array.
    static float doLayoutRunCached(const U16StringPiece& textBuf, const Range& range, bool isRtl,
                                   const MinikinPaint& paint, size_t dstStart,
                                   StartHyphenEdit startHyphen, EndHyphenEdit endHyphen,
                                   Layout* layout, float* advances, MinikinExtent* extents,
                                   LayoutPieces* lpOut);

    // Lay out a single word
    static float doLayoutWord(const uint16_t* buf, size_t start, size_t count, size_t bufSize,
                              bool isRtl, const MinikinPaint& paint, size_t bufStart,
                              StartHyphenEdit startHyphen, EndHyphenEdit endHyphen, Layout* layout,
                              float* advances, MinikinExtent* extents, LayoutPieces* lpOut);

    // Lay out a single bidi run
    void doLayoutRun(const uint16_t* buf, size_t start, size_t count, size_t bufSize, bool isRtl,
                     const MinikinPaint& paint, StartHyphenEdit startHyphen,
                     EndHyphenEdit endHyphen);

    std::vector<LayoutGlyph> mGlyphs;

    // The following three vectors are defined per code unit, so their length is identical to the
    // input text.
    std::vector<float> mAdvances;
    std::vector<MinikinExtent> mExtents;

    std::vector<FakedFont> mFaces;
    float mAdvance;
    MinikinRect mBounds;
};

struct LayoutPieces {
    // TODO: Sorted vector of pairs may be faster?
    std::unordered_map<uint32_t, Layout> offsetMap;  // start offset to layout index map.

    uint32_t getMemoryUsage() const {
        uint32_t result = 0;
        for (const auto& i : offsetMap) {
            result += i.second.getMemoryUsage();
        }
        return result;
    }
};

class LayoutCompositer {
public:
    LayoutCompositer(uint32_t size) {
        mLayout.reset();
        mLayout.mAdvances.resize(size, 0);
        mLayout.mExtents.resize(size);
    }

    void append(const Layout& layout, uint32_t start, float extraAdvance) {
        mLayout.appendLayout(layout, start, extraAdvance);
    }

    Layout build() { return std::move(mLayout); }

private:
    Layout mLayout;
};

}  // namespace minikin

#endif  // MINIKIN_LAYOUT_H
