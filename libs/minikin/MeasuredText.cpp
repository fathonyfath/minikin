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

#define LOG_TAG "Minikin"
#include "minikin/MeasuredText.h"

#include "minikin/Layout.h"

#include "LineBreakerUtil.h"

namespace minikin {

void MeasuredText::measure(const U16StringPiece& textBuf, bool computeHyphenation,
                           bool computeLayout) {
    if (textBuf.size() == 0) {
        return;
    }
    CharProcessor proc(textBuf);
    for (const auto& run : runs) {
        const Range& range = run->getRange();
        const uint32_t runOffset = range.getStart();
        run->getMetrics(textBuf, widths.data() + runOffset, extents.data() + runOffset);

        if (computeLayout) {
            run->addToLayoutPieces(textBuf, &layoutPieces);
        }

        if (!computeHyphenation || !run->canHyphenate()) {
            continue;
        }

        proc.updateLocaleIfNecessary(*run);
        for (uint32_t i = range.getStart(); i < range.getEnd(); ++i) {
            proc.feedChar(i, textBuf[i], widths[i]);

            const uint32_t nextCharOffset = i + 1;
            if (nextCharOffset != proc.nextWordBreak) {
                continue;  // Wait until word break point.
            }

            populateHyphenationPoints(textBuf, *run, *proc.hyphenator, proc.contextRange(),
                                      proc.wordRange(), &hyphenBreaks);
        }
    }
}

bool MeasuredText::buildLayout(const U16StringPiece& /*textBuf*/, const Range& range,
                               const MinikinPaint& paint, Bidi /*bidiFlag*/, int mtOffset,
                               StartHyphenEdit startHyphen, EndHyphenEdit endHyphen,
                               Layout* layout) {
    if (paint.wordSpacing != 0.0f || startHyphen != StartHyphenEdit::NO_EDIT ||
        endHyphen != EndHyphenEdit::NO_EDIT) {
        // TODO: Use layout result as much as possible even if justified lines and hyphenated lines.
        return false;
    }

    uint32_t start = range.getStart() + mtOffset;
    const uint32_t end = range.getEnd() + mtOffset;
    LayoutCompositer compositer(range.getLength());
    while (start < end) {
        auto ite = layoutPieces.offsetMap.find(start);
        if (ite == layoutPieces.offsetMap.end()) {
            // The layout result not found, possibly due to hyphenation or desperate breaks.
            // TODO: Do layout here only for necessary piece and keep composing final layout.
            return false;
        }
        if (start + ite->second.advances().size() > end) {
            // The width of the layout piece exceeds the end of line, possibly due to hyphenation
            // or desperate breaks.
            // TODO: Do layout here only for necessary piece and keep composing final layout.
            return false;
        }
        compositer.append(ite->second, start - mtOffset, 0);
        start += ite->second.advances().size();
    }
    *layout = std::move(compositer.build());
    return true;
}

}  // namespace minikin
