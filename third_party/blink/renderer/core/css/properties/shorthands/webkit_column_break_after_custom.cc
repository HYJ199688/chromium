// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/shorthands/webkit_column_break_after.h"

#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {
namespace css_shorthand {

bool WebkitColumnBreakAfter::ParseShorthand(
    bool important,
    CSSParserTokenRange& range,
    const CSSParserContext&,
    const CSSParserLocalContext&,
    HeapVector<CSSPropertyValue, 256>& properties) const {
  CSSValueID value;
  if (!css_parsing_utils::ConsumeFromColumnBreakBetween(range, value)) {
    return false;
  }

  css_property_parser_helpers::AddProperty(
      CSSPropertyID::kBreakAfter, CSSPropertyID::kWebkitColumnBreakAfter,
      *CSSIdentifierValue::Create(value), important,
      css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
      properties);
  return true;
}

const CSSValue* WebkitColumnBreakAfter::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  return ComputedStyleUtils::ValueForWebkitColumnBreakBetween(
      style.BreakAfter());
}

}  // namespace css_shorthand
}  // namespace blink
