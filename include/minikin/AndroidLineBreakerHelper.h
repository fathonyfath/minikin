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

#ifndef MINIKIN_ANDROID_LINE_BREAKER_HELPERS_H
#define MINIKIN_ANDROID_LINE_BREAKER_HELPERS_H

#include "minikin/LineBreaker.h"

namespace minikin {
namespace android {

class LineWidth : public LineBreaker::LineWidthDelegate {
    public:
        LineWidth(float firstWidth, int32_t firstLineCount, float restWidth,
                const std::vector<float>& indents, const std::vector<float>& leftPaddings,
                const std::vector<float>& rightPaddings, int32_t indentsAndPaddingsOffset)
            : mFirstWidth(firstWidth), mFirstLineCount(firstLineCount), mRestWidth(restWidth),
              mIndents(indents), mLeftPaddings(leftPaddings),
              mRightPaddings(rightPaddings), mOffset(indentsAndPaddingsOffset) {}

        float getLineWidth(size_t lineNo) override {
            const float width = ((ssize_t)lineNo < (ssize_t)mFirstLineCount)
                    ? mFirstWidth : mRestWidth;
            return width - get(mIndents, lineNo);
        }

        float getMinLineWidth() override {
            // A simpler algorithm would have been simply looping until the larger of
            // mFirstLineCount and mIndents.size()-mOffset, but that does unnecessary calculations
            // when mFirstLineCount is large. Instead, we measure the first line, all the lines that
            // have an indent, and the first line after firstWidth ends and restWidth starts.
            float minWidth = std::min(getLineWidth(0), getLineWidth(mFirstLineCount));
            for (size_t lineNo = 1; lineNo + mOffset < mIndents.size(); lineNo++) {
                minWidth = std::min(minWidth, getLineWidth(lineNo));
            }
            return minWidth;
        }

        float getLeftPadding(size_t lineNo) override {
            return get(mLeftPaddings, lineNo);
        }

        float getRightPadding(size_t lineNo) override {
            return get(mRightPaddings, lineNo);
        }

    private:
        float get(const std::vector<float>& vec, size_t lineNo) {
            if (vec.empty()) {
                return 0;
            }
            const size_t index = lineNo + mOffset;
            if (index < vec.size()) {
                return vec[index];
            } else {
                return vec.back();
            }
        }

        const float mFirstWidth;
        const int32_t mFirstLineCount;
        const float mRestWidth;
        const std::vector<float>& mIndents;
        const std::vector<float>& mLeftPaddings;
        const std::vector<float>& mRightPaddings;
        const int32_t mOffset;
};

class StyleRun : public Run {
    public:
        StyleRun(const Range& range, MinikinPaint&& paint,
                 std::shared_ptr<FontCollection>&& collection, bool isRtl)
            : Run(range), mPaint(std::move(paint)),
              mCollection(std::move(collection)), mIsRtl(isRtl) {}

        bool canHyphenate() const override { return true; }
        uint32_t getLocaleListId() const override { return mPaint.localeListId; }
        bool isRtl() const override { return mIsRtl; }

        void getMetrics(const U16StringPiece& text, float* advances,
                        MinikinExtent* extents,
                        LayoutOverhang* overhangs) const override {
            Bidi bidiFlag = mIsRtl ? Bidi::FORCE_RTL : Bidi::FORCE_LTR;
            Layout::measureText(text, mRange, bidiFlag, mPaint, mCollection, advances, extents,
                                overhangs);
        }

        const MinikinPaint* getPaint() const override {
            return &mPaint;
        }

        float measureHyphenPiece(const U16StringPiece& text, const Range& range,
                                 StartHyphenEdit startHyphen, EndHyphenEdit endHyphen,
                                 float* advances, LayoutOverhang* overhangs) const override {
            Bidi bidiFlag = mIsRtl ? Bidi::FORCE_RTL : Bidi::FORCE_LTR;
            return Layout::measureText(
                    text, range, bidiFlag, mPaint, startHyphen, endHyphen, mCollection,
                    advances, nullptr /* extent */, overhangs);
        }

    private:
        MinikinPaint mPaint;
        std::shared_ptr<FontCollection> mCollection;
        const bool mIsRtl;
};

class Replacement : public Run {
    public:
        Replacement(const Range& range, float width, uint32_t localeListId)
            : Run(range), mWidth(width), mLocaleListId(localeListId) {}

        bool isRtl() const { return false; }
        bool canHyphenate() const { return false; }
        uint32_t getLocaleListId() const { return mLocaleListId; }

        void getMetrics(const U16StringPiece& /* unused */, float* advances,
                        MinikinExtent* /* unused */, LayoutOverhang* /* unused */) const override {
            advances[mRange.getStart()] = mWidth;
            // TODO: Get the extents information from the caller.
        }

    private:
        const float mWidth;
        const uint32_t mLocaleListId;
};

class StaticLayoutNative {
    public:
        StaticLayoutNative(
                BreakStrategy strategy, HyphenationFrequency frequency,
                bool isJustified, std::vector<float>&& indents, std::vector<float>&& leftPaddings,
                std::vector<float>&& rightPaddings)
            : mStrategy(strategy), mFrequency(frequency), mIsJustified(isJustified),
              mIndents(std::move(indents)), mLeftPaddings(std::move(leftPaddings)),
              mRightPaddings(std::move(rightPaddings)) {}

        void addStyleRun(int32_t start, int32_t end, MinikinPaint&& paint,
                         std::shared_ptr<FontCollection> collection, bool isRtl) {
            mRuns.emplace_back(std::make_unique<StyleRun>(
                    Range(start, end), std::move(paint), std::move(collection), isRtl));
        }

        void addReplacementRun(int32_t start, int32_t end, float width, uint32_t localeListId) {
            mRuns.emplace_back(
                    std::make_unique<Replacement>(Range(start, end), width, localeListId));
        }

        // Only valid while this instance is alive.
        inline std::unique_ptr<LineBreaker::LineWidthDelegate> buildLineWidthDelegate(
                float firstWidth, int32_t firstLineCount, float restWidth,
                int32_t indentsAndPaddingsOffset) {
            return std::make_unique<LineWidth>(firstWidth, firstLineCount, restWidth, mIndents,
                    mLeftPaddings, mRightPaddings, indentsAndPaddingsOffset);
        }

        void addRuns(LineBreaker* lineBreaker) {
            for (const auto& run : mRuns) {
                lineBreaker->addRun(*run);
            }
        }

        void clearRuns() {
            mRuns.clear();
        }

        inline BreakStrategy getStrategy() const { return mStrategy; }
        inline HyphenationFrequency getFrequency() const { return mFrequency; }
        inline bool isJustified() const { return mIsJustified; }

    private:
        const BreakStrategy mStrategy;
        const HyphenationFrequency mFrequency;
        const bool mIsJustified;
        const std::vector<float> mIndents;
        const std::vector<float> mLeftPaddings;
        const std::vector<float> mRightPaddings;

        std::vector<std::unique_ptr<Run>> mRuns;
};

}  // namespace android
}  // namespace minikin

#endif  // MINIKIN_ANDROID_LINE_BREAKER_HELPERS_H
