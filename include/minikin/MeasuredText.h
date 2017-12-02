/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef MINIKIN_MEASURED_TEXT_H
#define MINIKIN_MEASURED_TEXT_H

#include <deque>
#include <vector>

#include "minikin/FontCollection.h"
#include "minikin/Layout.h"
#include "minikin/Macros.h"
#include "minikin/MinikinFont.h"
#include "minikin/Range.h"
#include "minikin/U16StringPiece.h"

namespace minikin {

class Run {
public:
    Run(const Range& range) : mRange(range) {}
    virtual ~Run() {}

    // Returns true if this run is RTL. Otherwise returns false.
    virtual bool isRtl() const = 0;

    // Returns true if this run is a target of hyphenation. Otherwise return false.
    virtual bool canHyphenate() const = 0;

    // Returns the locale list ID for this run.
    virtual uint32_t getLocaleListId() const = 0;

    // Fills the each character's advances, extents and overhangs.
    virtual void getMetrics(const U16StringPiece& text, float* advances, MinikinExtent* extents,
                            LayoutOverhang* overhangs) const = 0;

    // Following two methods are only called when the implementation returns true for
    // canHyphenate method.

    // Returns the paint pointer used for this run.
    // Returns null if canHyphenate has not returned true.
    virtual const MinikinPaint* getPaint() const { return nullptr; }

    // Measures the hyphenation piece and fills each character's advances and overhangs.
    virtual float measureHyphenPiece(const U16StringPiece& /* text */,
                                     const Range& /* hyphenPieceRange */,
                                     StartHyphenEdit /* startHyphen */,
                                     EndHyphenEdit /* endHyphen */, float* /* advances */,
                                     LayoutOverhang* /* overhangs */) const {
        return 0.0;
    }

    inline const Range& getRange() const { return mRange; }

protected:
    const Range mRange;
};

class StyleRun : public Run {
public:
    StyleRun(const Range& range, MinikinPaint&& paint, std::shared_ptr<FontCollection>&& collection,
             bool isRtl)
            : Run(range),
              mPaint(std::move(paint)),
              mCollection(std::move(collection)),
              mIsRtl(isRtl) {}

    bool canHyphenate() const override { return true; }
    uint32_t getLocaleListId() const override { return mPaint.localeListId; }
    bool isRtl() const override { return mIsRtl; }

    void getMetrics(const U16StringPiece& text, float* advances, MinikinExtent* extents,
                    LayoutOverhang* overhangs) const override {
        Bidi bidiFlag = mIsRtl ? Bidi::FORCE_RTL : Bidi::FORCE_LTR;
        Layout::measureText(text, mRange, bidiFlag, mPaint, mCollection, advances, extents,
                            overhangs);
    }

    const MinikinPaint* getPaint() const override { return &mPaint; }

    float measureHyphenPiece(const U16StringPiece& text, const Range& range,
                             StartHyphenEdit startHyphen, EndHyphenEdit endHyphen, float* advances,
                             LayoutOverhang* overhangs) const override {
        Bidi bidiFlag = mIsRtl ? Bidi::FORCE_RTL : Bidi::FORCE_LTR;
        return Layout::measureText(text, range, bidiFlag, mPaint, startHyphen, endHyphen,
                                   mCollection, advances, nullptr /* extent */, overhangs);
    }

private:
    MinikinPaint mPaint;
    std::shared_ptr<FontCollection> mCollection;
    const bool mIsRtl;
};

class ReplacementRun : public Run {
public:
    ReplacementRun(const Range& range, float width, uint32_t localeListId)
            : Run(range), mWidth(width), mLocaleListId(localeListId) {}

    bool isRtl() const { return false; }
    bool canHyphenate() const { return false; }
    uint32_t getLocaleListId() const { return mLocaleListId; }

    void getMetrics(const U16StringPiece& /* unused */, float* advances,
                    MinikinExtent* /* unused */, LayoutOverhang* /* unused */) const override {
        advances[0] = mWidth;
        // TODO: Get the extents information from the caller.
    }

private:
    const float mWidth;
    const uint32_t mLocaleListId;
};

class MeasuredText {
public:
    // Following three vectors have the same length.
    std::vector<float> widths;
    std::vector<MinikinExtent> extents;
    std::vector<LayoutOverhang> overhangs;

    // The style information.
    std::vector<std::unique_ptr<Run>> runs;

    MeasuredText(MeasuredText&&) = default;
    MeasuredText& operator=(MeasuredText&&) = default;

    PREVENT_COPY_AND_ASSIGN(MeasuredText);

private:
    friend class MeasuredTextBuilder;

    void measure(const U16StringPiece& textBuf);

    // Use MeasuredTextBuilder instead.
    MeasuredText(const U16StringPiece& textBuf, std::vector<std::unique_ptr<Run>>&& runs)
            : widths(textBuf.size()),
              extents(textBuf.size()),
              overhangs(textBuf.size()),
              runs(std::move(runs)) {
        measure(textBuf);
    }
};

class MeasuredTextBuilder {
public:
    MeasuredTextBuilder() {}

    void addStyleRun(int32_t start, int32_t end, MinikinPaint&& paint,
                     std::shared_ptr<FontCollection> collection, bool isRtl) {
        mRuns.emplace_back(std::make_unique<StyleRun>(Range(start, end), std::move(paint),
                                                      std::move(collection), isRtl));
    }

    void addReplacementRun(int32_t start, int32_t end, float width, uint32_t localeListId) {
        mRuns.emplace_back(
                std::make_unique<ReplacementRun>(Range(start, end), width, localeListId));
    }

    template <class T, typename... Args>
    void addCustomRun(Args&&... args) {
        mRuns.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
    }

    std::unique_ptr<MeasuredText> build(const U16StringPiece& textBuf) {
        // Unable to use make_unique here since make_unique is not a friend of MeasuredText.
        return std::unique_ptr<MeasuredText>(new MeasuredText(textBuf, std::move(mRuns)));
    }

    PREVENT_COPY_ASSIGN_AND_MOVE(MeasuredTextBuilder);

private:
    std::vector<std::unique_ptr<Run>> mRuns;
};

}  // namespace minikin

#endif  // MINIKIN_MEASURED_TEXT_H
