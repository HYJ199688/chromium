// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/max_height.h"

#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {
namespace css_longhand {

const CSSValue* MaxHeight::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  return css_parsing_utils::ConsumeMaxWidthOrHeight(
      range, context, css_property_parser_helpers::UnitlessQuirk::kAllow);
}

const CSSValue* MaxHeight::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  const Length& max_height = style.MaxHeight();
  if (max_height.IsMaxSizeNone())
    return CSSIdentifierValue::Create(CSSValueID::kNone);
  return ComputedStyleUtils::ZoomAdjustedPixelValueForLength(max_height, style);
}

}  // namespace css_longhand
}  // namespace blink
