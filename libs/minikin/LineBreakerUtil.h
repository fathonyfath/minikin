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

#ifndef MINIKIN_LINE_BREAKER_UTIL_H
#define MINIKIN_LINE_BREAKER_UTIL_H

#include <vector>

#include "minikin/Hyphenator.h"
#include "minikin/U16StringPiece.h"

namespace minikin {

// Hyphenates a string potentially containing non-breaking spaces.
std::vector<HyphenationType> hyphenate(const U16StringPiece& string, const Hyphenator& hypenator);

// This function determines whether a character is a space that disappears at end of line.
// It is the Unicode set: [[:General_Category=Space_Separator:]-[:Line_Break=Glue:]], plus '\n'.
// Note: all such characters are in the BMP, so it's ok to use code units for this.
inline bool isLineEndSpace(uint16_t c) {
    return c == '\n' || c == ' '                           // SPACE
           || c == 0x1680                                  // OGHAM SPACE MARK
           || (0x2000 <= c && c <= 0x200A && c != 0x2007)  // EN QUAD, EM QUAD, EN SPACE, EM SPACE,
           // THREE-PER-EM SPACE, FOUR-PER-EM SPACE,
           // SIX-PER-EM SPACE, PUNCTUATION SPACE,
           // THIN SPACE, HAIR SPACE
           || c == 0x205F  // MEDIUM MATHEMATICAL SPACE
           || c == 0x3000;
}
}  // namespace minikin

#endif  // MINIKIN_LINE_BREAKER_UTIL_H
