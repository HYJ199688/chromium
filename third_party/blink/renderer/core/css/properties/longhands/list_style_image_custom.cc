// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/list_style_image.h"

#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {

class CSSParserLocalContext;

namespace css_longhand {

const CSSValue* ListStyleImage::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  return css_property_parser_helpers::ConsumeImageOrNone(range, &context);
}

const CSSValue* ListStyleImage::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  if (style.ListStyleImage())
    return style.ListStyleImage()->ComputedCSSValue();
  return CSSIdentifierValue::Create(CSSValueID::kNone);
}

void ListStyleImage::ApplyValue(StyleResolverState& state,
                                const CSSValue& value) const {
  state.Style()->SetListStyleImage(
      state.GetStyleImage(CSSPropertyID::kListStyleImage, value));
}

}  // namespace css_longhand
}  // namespace blink
