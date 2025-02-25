// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/shorthands/flex.h"

#include "third_party/blink/renderer/core/css/css_identifier_value.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_context.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_local_context.h"
#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/style_property_shorthand.h"

namespace blink {
namespace css_shorthand {

bool Flex::ParseShorthand(bool important,
                          CSSParserTokenRange& range,
                          const CSSParserContext& context,
                          const CSSParserLocalContext&,
                          HeapVector<CSSPropertyValue, 256>& properties) const {
  static const double kUnsetValue = -1;
  double flex_grow = kUnsetValue;
  double flex_shrink = kUnsetValue;
  CSSValue* flex_basis = nullptr;

  if (range.Peek().Id() == CSSValueID::kNone) {
    flex_grow = 0;
    flex_shrink = 0;
    flex_basis = CSSIdentifierValue::Create(CSSValueID::kAuto);
    range.ConsumeIncludingWhitespace();
  } else {
    unsigned index = 0;
    while (!range.AtEnd() && index++ < 3) {
      double num;
      if (css_property_parser_helpers::ConsumeNumberRaw(range, num)) {
        if (num < 0)
          return false;
        if (flex_grow == kUnsetValue) {
          flex_grow = num;
        } else if (flex_shrink == kUnsetValue) {
          flex_shrink = num;
        } else if (!num) {
          // flex only allows a basis of 0 (sans units) if
          // flex-grow and flex-shrink values have already been
          // set.
          flex_basis = CSSPrimitiveValue::Create(
              0, CSSPrimitiveValue::UnitType::kPixels);
        } else {
          return false;
        }
      } else if (!flex_basis) {
        if (range.Peek().Id() == CSSValueID::kAuto)
          flex_basis = css_property_parser_helpers::ConsumeIdent(range);
        if (!flex_basis) {
          flex_basis = css_property_parser_helpers::ConsumeLengthOrPercent(
              range, context.Mode(), kValueRangeNonNegative);
        }
        if (index == 2 && !range.AtEnd())
          return false;
      }
    }
    if (index == 0)
      return false;
    if (flex_grow == kUnsetValue)
      flex_grow = 1;
    if (flex_shrink == kUnsetValue)
      flex_shrink = 1;
    if (!flex_basis) {
      flex_basis = CSSPrimitiveValue::Create(
          0, CSSPrimitiveValue::UnitType::kPercentage);
    }
  }

  if (!range.AtEnd())
    return false;
  css_property_parser_helpers::AddProperty(
      CSSPropertyID::kFlexGrow, CSSPropertyID::kFlex,
      *CSSPrimitiveValue::Create(clampTo<float>(flex_grow),
                                 CSSPrimitiveValue::UnitType::kNumber),
      important, css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
      properties);
  css_property_parser_helpers::AddProperty(
      CSSPropertyID::kFlexShrink, CSSPropertyID::kFlex,
      *CSSPrimitiveValue::Create(clampTo<float>(flex_shrink),
                                 CSSPrimitiveValue::UnitType::kNumber),
      important, css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
      properties);

  css_property_parser_helpers::AddProperty(
      CSSPropertyID::kFlexBasis, CSSPropertyID::kFlex, *flex_basis, important,
      css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
      properties);

  return true;
}

const CSSValue* Flex::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject* layout_object,
    Node* styled_node,
    bool allow_visited_style) const {
  return ComputedStyleUtils::ValuesForShorthandProperty(
      flexShorthand(), style, layout_object, styled_node, allow_visited_style);
}

}  // namespace css_shorthand
}  // namespace blink
