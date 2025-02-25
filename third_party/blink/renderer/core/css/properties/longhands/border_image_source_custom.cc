// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/border_image_source.h"

#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {

class CSSParserLocalContext;

namespace css_longhand {

const CSSValue* BorderImageSource::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  return css_property_parser_helpers::ConsumeImageOrNone(range, &context);
}

const CSSValue* BorderImageSource::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node*,
    bool allow_visited_style) const {
  if (style.BorderImageSource())
    return style.BorderImageSource()->ComputedCSSValue();
  return CSSIdentifierValue::Create(CSSValueID::kNone);
}

const CSSValue* BorderImageSource::InitialValue() const {
  DEFINE_STATIC_LOCAL(Persistent<CSSValue>, value,
                      (CSSIdentifierValue::Create(CSSValueID::kNone)));
  return value;
}

void BorderImageSource::ApplyValue(StyleResolverState& state,
                                   const CSSValue& value) const {
  state.Style()->SetBorderImageSource(
      state.GetStyleImage(CSSPropertyID::kBorderImageSource, value));
}

}  // namespace css_longhand
}  // namespace blink
