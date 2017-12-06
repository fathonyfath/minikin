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

#include "minikin/MeasuredText.h"

namespace minikin {

void MeasuredText::measure(const U16StringPiece& textBuf) {
    for (const auto& run : runs) {
        const uint32_t runOffset = run->getRange().getStart();
        run->getMetrics(textBuf, widths.data() + runOffset, extents.data() + runOffset,
                        overhangs.data() + runOffset);
    }
}

}  // namespace minikin
