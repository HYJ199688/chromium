// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/vertical_align.h"

#include "third_party/blink/renderer/core/css/parser/css_parser_context.h"
#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {
namespace css_longhand {

const CSSValue* VerticalAlign::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  CSSValue* parsed_value = css_property_parser_helpers::ConsumeIdentRange(
      range, CSSValueID::kBaseline, CSSValueID::kWebkitBaselineMiddle);
  if (!parsed_value) {
    parsed_value = css_property_parser_helpers::ConsumeLengthOrPercent(
        range, context.Mode(), kValueRangeAll,
        css_property_parser_helpers::UnitlessQuirk::kAllow);
  }
  return parsed_value;
}

const CSSValue* VerticalAlign::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  switch (style.VerticalAlign()) {
    case EVerticalAlign::kBaseline:
      return CSSIdentifierValue::Create(CSSValueID::kBaseline);
    case EVerticalAlign::kMiddle:
      return CSSIdentifierValue::Create(CSSValueID::kMiddle);
    case EVerticalAlign::kSub:
      return CSSIdentifierValue::Create(CSSValueID::kSub);
    case EVerticalAlign::kSuper:
      return CSSIdentifierValue::Create(CSSValueID::kSuper);
    case EVerticalAlign::kTextTop:
      return CSSIdentifierValue::Create(CSSValueID::kTextTop);
    case EVerticalAlign::kTextBottom:
      return CSSIdentifierValue::Create(CSSValueID::kTextBottom);
    case EVerticalAlign::kTop:
      return CSSIdentifierValue::Create(CSSValueID::kTop);
    case EVerticalAlign::kBottom:
      return CSSIdentifierValue::Create(CSSValueID::kBottom);
    case EVerticalAlign::kBaselineMiddle:
      return CSSIdentifierValue::Create(CSSValueID::kWebkitBaselineMiddle);
    case EVerticalAlign::kLength:
      return ComputedStyleUtils::ZoomAdjustedPixelValueForLength(
          style.GetVerticalAlignLength(), style);
  }
  NOTREACHED();
  return nullptr;
}

void VerticalAlign::ApplyInherit(StyleResolverState& state) const {
  EVerticalAlign vertical_align = state.ParentStyle()->VerticalAlign();
  state.Style()->SetVerticalAlign(vertical_align);
  if (vertical_align == EVerticalAlign::kLength) {
    state.Style()->SetVerticalAlignLength(
        state.ParentStyle()->GetVerticalAlignLength());
  }
}

void VerticalAlign::ApplyValue(StyleResolverState& state,
                               const CSSValue& value) const {
  if (auto* identifier_value = DynamicTo<CSSIdentifierValue>(value)) {
    state.Style()->SetVerticalAlign(
        identifier_value->ConvertTo<EVerticalAlign>());
  } else {
    state.Style()->SetVerticalAlignLength(
        To<CSSPrimitiveValue>(value).ConvertToLength(
            state.CssToLengthConversionData()));
  }
}

}  // namespace css_longhand
}  // namespace blink
