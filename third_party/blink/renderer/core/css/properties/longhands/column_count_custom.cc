// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/column_count.h"

#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {
namespace css_longhand {

const CSSValue* ColumnCount::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  return css_parsing_utils::ConsumeColumnCount(range);
}

const CSSValue* ColumnCount::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  if (style.HasAutoColumnCount())
    return CSSIdentifierValue::Create(CSSValueID::kAuto);
  return CSSPrimitiveValue::Create(style.ColumnCount(),
                                   CSSPrimitiveValue::UnitType::kNumber);
}

}  // namespace css_longhand
}  // namespace blink
