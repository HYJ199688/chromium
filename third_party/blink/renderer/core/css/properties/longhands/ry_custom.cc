// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/ry.h"

#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"

namespace blink {
namespace css_longhand {

const CSSValue* Ry::ParseSingleValue(CSSParserTokenRange& range,
                                     const CSSParserContext& context,
                                     const CSSParserLocalContext&) const {
  if (range.Peek().Id() == CSSValueID::kAuto)
    return css_property_parser_helpers::ConsumeIdent(range);
  return css_property_parser_helpers::ConsumeSVGGeometryPropertyLength(
      range, context, kValueRangeNonNegative);
}

const CSSValue* Ry::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle& svg_style,
    const LayoutObject*,
    Node*,
    bool allow_visited_style) const {
  return ComputedStyleUtils::ZoomAdjustedPixelValueForLength(svg_style.Ry(),
                                                             style);
}

}  // namespace css_longhand
}  // namespace blink
