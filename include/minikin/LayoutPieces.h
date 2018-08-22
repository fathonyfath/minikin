/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef MINIKIN_LAYOUT_PIECES_H
#define MINIKIN_LAYOUT_PIECES_H

#include <unordered_map>

#include "minikin/LayoutCache.h"
#include "minikin/LayoutCore.h"

namespace minikin {

struct LayoutPieces {
    struct Key {
        Key(const Range& range, HyphenEdit hyphenEdit) : range(range), hyphenEdit(hyphenEdit) {}

        Range range;
        HyphenEdit hyphenEdit;

        uint32_t hash() const {
            return Hasher()
                    .update(range.getStart())
                    .update(range.getEnd())
                    .update(hyphenEdit)
                    .hash();
        }

        bool operator==(const Key& o) const {
            return range == o.range && hyphenEdit == o.hyphenEdit;
        }

        uint32_t getMemoryUsage() const { return sizeof(Range) + sizeof(HyphenEdit); }
    };

    struct KeyHasher {
        std::size_t operator()(const Key& key) const { return key.hash(); }
    };

    LayoutPieces() {}
    ~LayoutPieces() {}

    std::unordered_map<Key, LayoutPiece, KeyHasher> offsetMap;

    void insert(const Range& range, HyphenEdit edit, const LayoutPiece& layout) {
        offsetMap.emplace(std::piecewise_construct, std::forward_as_tuple(range, edit),
                          std::forward_as_tuple(layout));
    }

    template <typename F>
    void getOrCreate(const U16StringPiece& textBuf, const Range& range, const Range& context,
                     const MinikinPaint& paint, bool dir, StartHyphenEdit startEdit,
                     EndHyphenEdit endEdit, F& f) const {
        const HyphenEdit edit = packHyphenEdit(startEdit, endEdit);
        auto it = offsetMap.find(Key(range, edit));
        if (it == offsetMap.end()) {
            LayoutCache::getInstance().getOrCreate(textBuf.substr(context),
                                                   range - context.getStart(), paint, dir,
                                                   startEdit, endEdit, f);
        } else {
            f(it->second);
        }
    }

    uint32_t getMemoryUsage() const {
        uint32_t result = 0;
        for (const auto& i : offsetMap) {
            result += i.first.getMemoryUsage() + i.second.getMemoryUsage();
        }
        return result;
    }
};

}  // namespace minikin

#endif  // MINIKIN_LAYOUT_PIECES_H
