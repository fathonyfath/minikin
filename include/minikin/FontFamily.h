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

#include "minikin/FontStyle.h"
#include "minikin/SparseBitSet.h"

namespace minikin {

class MinikinFont;

// attributes representing transforms (fake bold, fake italic) to match styles
class FontFakery {
public:
    FontFakery() : mFakeBold(false), mFakeItalic(false) {}
    FontFakery(bool fakeBold, bool fakeItalic) : mFakeBold(fakeBold), mFakeItalic(fakeItalic) {}
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

    Font(Font&& o) = default;
    Font& operator=(Font&& o) = default;
    // prevent copy constructor and assign operator.
    Font(const Font& o) = delete;
    Font& operator=(const Font& o) = delete;

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
    // Must be the same value as FontConfig.java
    enum class Variant : uint8_t {
        DEFAULT = 0,  // Must be the same as FontConfig.VARIANT_DEFAULT
        COMPACT = 1,  // Must be the same as FontConfig.VARIANT_COMPACT
        ELEGANT = 2,  // Must be the same as FontConfig.VARIANT_ELEGANT
    };

    explicit FontFamily(std::vector<Font>&& fonts);
    FontFamily(Variant variant, std::vector<Font>&& fonts);
    FontFamily(uint32_t localeListId, Variant variant, std::vector<Font>&& fonts);

    // TODO: Good to expose FontUtil.h.
    static bool analyzeStyle(const std::shared_ptr<MinikinFont>& typeface, int* weight,
                             bool* italic);
    FakedFont getClosestMatch(FontStyle style) const;

    uint32_t localeListId() const { return mLocaleListId; }
    Variant variant() const { return mVariant; }

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
    Variant mVariant;
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
