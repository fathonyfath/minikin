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
#include <vector>

#include "minikin/FontCollection.h"

namespace minikin {

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

struct LayoutOverhang {
    // The amount of space needed to draw a glyph or glyph cluster beyond its advance box.
    float left = 0.0;
    float right = 0.0;
};

// Internal state used during layout operation
struct LayoutContext;

// Must be the same value with Paint.java
enum class Bidi : uint8_t {
    LTR         = 0b0000,  // Must be same with Paint.BIDI_LTR
    RTL         = 0b0001,  // Must be same with Paint.BIDI_RTL
    DEFAULT_LTR = 0b0010,  // Must be same with Paint.BIDI_DEFAULT_LTR
    DEFAULT_RTL = 0b0011,  // Must be same with Paint.BIDI_DEFAULT_RTL
    FORCE_LTR   = 0b0100,  // Must be same with Paint.BIDI_FORCE_LTR
    FORCE_RTL   = 0b0101,  // Must be same with Paint.BIDI_FORCE_RTL
};

inline bool isRtl(Bidi bidi) {  return static_cast<uint8_t>(bidi) & 0b0001; }
inline bool isOverride(Bidi bidi) { return static_cast<uint8_t>(bidi) & 0b0100; }

// Lifecycle and threading assumptions for Layout:
// The object is assumed to be owned by a single thread; multiple threads
// may not mutate it at the same time.
class Layout {
public:

    Layout() : mGlyphs(), mAdvances(), mExtents(), mClusterBounds(), mFaces(), mAdvance(0),
            mBounds() {
        mBounds.setEmpty();
    }

    Layout(Layout&& layout) = default;

    // Forbid copying and assignment.
    Layout(const Layout&) = delete;
    void operator=(const Layout&) = delete;

    void dump() const;

    void doLayout(const uint16_t* buf, size_t start, size_t count, size_t bufSize,
        Bidi bidiFlags, const FontStyle &style, const MinikinPaint &paint,
        const std::shared_ptr<FontCollection>& collection);

    static float measureText(const uint16_t* buf, size_t start, size_t count, size_t bufSize,
        Bidi bidiFlags, const FontStyle &style, const MinikinPaint &paint,
        const std::shared_ptr<FontCollection>& collection, float* advances,
        MinikinExtent* extents, LayoutOverhang* overhangs);

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
    void getAdvances(float* advances);

    // Get extents, copying into caller-provided buffer. The size of this buffer must match the
    // length of the string (count arg to doLayout).
    void getExtents(MinikinExtent* extents);

    // Get overhangs, copying into caller-provided buffer. The size of this buffer must match the
    // length of the string (count arg to doLayout).
    void getOverhangs(LayoutOverhang* overhangs);

    // The i parameter is an offset within the buf relative to start, it is < count, where
    // start and count are the parameters to doLayout
    float getCharAdvance(size_t i) const { return mAdvances[i]; }

    void getBounds(MinikinRect* rect) const;

    // Purge all caches, useful in low memory conditions
    static void purgeCaches();

    // Dump minikin internal statistics, cache usage, cache hit ratio, etc.
    static void dumpMinikinStats(int fd);

private:
    friend class LayoutCacheKey;

    // Find a face in the mFaces vector, or create a new entry
    int findFace(const FakedFont& face, LayoutContext* ctx);

    // Clears layout, ready to be used again
    void reset();

    // Lay out a single bidi run
    // When layout is not null, layout info will be stored in the object.
    // When advances is not null, measurement results will be stored in the array.
    static float doLayoutRunCached(const uint16_t* buf, size_t runStart, size_t runLength,
        size_t bufSize, bool isRtl, LayoutContext* ctx, size_t dstStart,
        const std::shared_ptr<FontCollection>& collection, Layout* layout, float* advances,
        MinikinExtent* extents, LayoutOverhang* overhangs);

    // Lay out a single word
    static float doLayoutWord(const uint16_t* buf, size_t start, size_t count, size_t bufSize,
        bool isRtl, LayoutContext* ctx, size_t bufStart,
        const std::shared_ptr<FontCollection>& collection, Layout* layout, float* advances,
        MinikinExtent* extents, LayoutOverhang* overhangs);

    // Lay out a single bidi run
    void doLayoutRun(const uint16_t* buf, size_t start, size_t count, size_t bufSize,
        bool isRtl, LayoutContext* ctx, const std::shared_ptr<FontCollection>& collection);

    // Append another layout (for example, cached value) into this one
    void appendLayout(Layout* src, size_t start, float extraAdvance);

    std::vector<LayoutGlyph> mGlyphs;

    // The following three vectors are defined per code unit, so their length is identical to the
    // input text.
    std::vector<float> mAdvances;
    std::vector<MinikinExtent> mExtents;
    std::vector<MinikinRect> mClusterBounds; // per-cluster bounds, without a shift based on
                                             // previously-shaped text. Used to calculate overhangs.

    std::vector<FakedFont> mFaces;
    float mAdvance;
    MinikinRect mBounds;
};

}  // namespace minikin

#endif  // MINIKIN_LAYOUT_H
