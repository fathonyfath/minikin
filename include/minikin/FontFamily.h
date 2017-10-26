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

#ifndef MINIKIN_FONT_FAMILY_H
#define MINIKIN_FONT_FAMILY_H

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <utils/TypeHelpers.h>

#include "minikin/SparseBitSet.h"

namespace minikin {

class MinikinFont;

// Must be the same value as FontConfig.java
enum class FontVariant : uint8_t {
    DEFAULT = 0,  // Must be the same as FontConfig.VARIANT_DEFAULT
    COMPACT = 1,  // Must be the same as FontConfig.VARIANT_COMPACT
    ELEGANT = 2,  // Must be the same as FontConfig.VARIANT_ELEGANT
};

enum class FontWeight : uint16_t {
    THIN        = 100,
    EXTRA_LIGHT = 200,
    LIGHT       = 300,
    NORMAL      = 400,
    MEDIUM      = 500,
    SEMI_BOLD   = 600,
    BOLD        = 700,
    EXTRA_BOLD  = 800,
    BLACK       = 900,
    EXTRA_BLACK = 1000,
};

enum class FontSlant : bool {
    ITALIC  = true,
    UPRIGHT = false,
};

// FontStyle represents style information needed to select an actual font from a collection.
struct FontStyle {
    FontStyle() : FontStyle(FontVariant::DEFAULT, FontWeight::NORMAL, FontSlant::UPRIGHT) {}
    explicit FontStyle(FontWeight weight)
        : FontStyle(FontVariant::DEFAULT, weight, FontSlant::UPRIGHT) {}
    explicit FontStyle(FontSlant slant)
        : FontStyle(FontVariant::DEFAULT, FontWeight::NORMAL, slant) {}
    FontStyle(FontWeight weight, FontSlant slant)
        : FontStyle(FontVariant::DEFAULT, weight, slant) {}
    FontStyle(uint16_t weight, FontSlant slant)
        : FontStyle(FontVariant::DEFAULT, weight, slant) {}
    FontStyle(FontVariant variant, FontWeight weight, FontSlant slant)
        : FontStyle(variant, static_cast<uint16_t>(weight), slant) {}
    FontStyle(FontVariant variant, uint16_t weight, FontSlant slant)
        : weight(weight), slant(slant), variant(variant) {}

    uint16_t weight;
    FontSlant slant;
    FontVariant variant;

    inline bool operator==(const FontStyle& other) const {
        return weight == other.weight && slant == other.slant && variant== other.variant;
    }

    inline android::hash_t hash() const {
        return (weight << 16) | (static_cast<uint8_t>(variant) << 8) | static_cast<uint8_t>(slant);
    }
};

// attributes representing transforms (fake bold, fake italic) to match styles
class FontFakery {
public:
    FontFakery() : mFakeBold(false), mFakeItalic(false) { }
    FontFakery(bool fakeBold, bool fakeItalic) : mFakeBold(fakeBold), mFakeItalic(fakeItalic) { }
    // TODO: want to support graded fake bolding
    bool isFakeBold() { return mFakeBold; }
    bool isFakeItalic() { return mFakeItalic; }
private:
    bool mFakeBold;
    bool mFakeItalic;
};

struct FakedFont {
    // ownership is the enclosing FontCollection
    MinikinFont* font;
    FontFakery fakery;
};

typedef uint32_t AxisTag;

struct Font {
    Font(const std::shared_ptr<MinikinFont>& typeface, FontStyle style);
    Font(std::shared_ptr<MinikinFont>&& typeface, FontStyle style);
    Font(Font&& o);
    Font(const Font& o);

    std::shared_ptr<MinikinFont> typeface;
    FontStyle style;

    std::unordered_set<AxisTag> getSupportedAxesLocked() const;
};

struct FontVariation {
    FontVariation(AxisTag axisTag, float value) : axisTag(axisTag), value(value) {}
    AxisTag axisTag;
    float value;
};

class FontFamily {
public:
    explicit FontFamily(std::vector<Font>&& fonts);
    FontFamily(FontVariant variant, std::vector<Font>&& fonts);
    FontFamily(uint32_t localeListId, FontVariant variant, std::vector<Font>&& fonts);

    // TODO: Good to expose FontUtil.h.
    static bool analyzeStyle(const std::shared_ptr<MinikinFont>& typeface, int* weight,
            bool* italic);
    FakedFont getClosestMatch(FontStyle style) const;

    uint32_t localeListId() const { return mLocaleListId; }
    FontVariant variant() const { return mVariant; }

    // API's for enumerating the fonts in a family. These don't guarantee any particular order
    size_t getNumFonts() const { return mFonts.size(); }
    const std::shared_ptr<MinikinFont>& getFont(size_t index) const {
        return mFonts[index].typeface;
    }
    FontStyle getStyle(size_t index) const { return mFonts[index].style; }
    bool isColorEmojiFamily() const;
    const std::unordered_set<AxisTag>& supportedAxes() const { return mSupportedAxes; }

    // Get Unicode coverage.
    const SparseBitSet& getCoverage() const { return mCoverage; }

    // Returns true if the font has a glyph for the code point and variation selector pair.
    // Caller should acquire a lock before calling the method.
    bool hasGlyph(uint32_t codepoint, uint32_t variationSelector) const;

    // Returns true if this font family has a variaion sequence table (cmap format 14 subtable).
    bool hasVSTable() const { return !mCmapFmt14Coverage.empty(); }

    // Creates new FontFamily based on this family while applying font variations. Returns nullptr
    // if none of variations apply to this family.
    std::shared_ptr<FontFamily> createFamilyWithVariation(
            const std::vector<FontVariation>& variations) const;

private:
    void computeCoverage();

    uint32_t mLocaleListId;
    FontVariant mVariant;
    std::vector<Font> mFonts;
    std::unordered_set<AxisTag> mSupportedAxes;

    SparseBitSet mCoverage;
    std::vector<std::unique_ptr<SparseBitSet>> mCmapFmt14Coverage;

    // Forbid copying and assignment.
    FontFamily(const FontFamily&) = delete;
    void operator=(const FontFamily&) = delete;
};

}  // namespace minikin

#endif  // MINIKIN_FONT_FAMILY_H
