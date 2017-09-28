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

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "StringPiece.h"

namespace minikin {

size_t StringPiece::find(size_t from, char c) const {
    if (from >= mLength) {
        return mLength;
    }
    const char* p = static_cast<const char*>(memchr(mData + from, c, mLength - from));
    return p == nullptr ? mLength : p - mData;
}

StringPiece SplitIterator::next() {
    if (!hasNext()) {
        return StringPiece();
    }
    const size_t searchFrom = mStarted ? mCurrent + 1 : 0;
    mStarted = true;
    mCurrent = mString.find(searchFrom, mDelimiter);
    return mString.substr(searchFrom, mCurrent - searchFrom);
}

}  // namespace minikin
