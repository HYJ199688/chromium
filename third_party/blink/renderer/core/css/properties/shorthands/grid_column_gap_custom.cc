// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/shorthands/grid_column_gap.h"

#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/style_property_shorthand.h"

namespace blink {
namespace css_shorthand {

bool GridColumnGap::ParseShorthand(
    bool important,
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&,
    HeapVector<CSSPropertyValue, 256>& properties) const {
  CSSValue* gap_length = css_parsing_utils::ConsumeGapLength(range, context);
  if (!gap_length || !range.AtEnd())
    return false;

  css_property_parser_helpers::AddProperty(
      CSSPropertyID::kColumnGap, CSSPropertyID::kGridColumnGap, *gap_length,
      important, css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
      properties);
  return true;
}

const CSSValue* GridColumnGap::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject* layout_object,
    Node* styled_node,
    bool allow_visited_style) const {
  return ComputedStyleUtils::ValuesForShorthandProperty(
      gridColumnGapShorthand(), style, layout_object, styled_node,
      allow_visited_style);
}

}  // namespace css_shorthand
}  // namespace blink
