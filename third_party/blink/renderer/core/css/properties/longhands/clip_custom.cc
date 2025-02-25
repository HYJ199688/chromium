// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/clip.h"

#include "third_party/blink/renderer/core/css/css_quad_value.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_context.h"
#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {
namespace {

CSSValue* ConsumeClipComponent(CSSParserTokenRange& range,
                               CSSParserMode css_parser_mode) {
  if (range.Peek().Id() == CSSValueID::kAuto)
    return css_property_parser_helpers::ConsumeIdent(range);
  return css_property_parser_helpers::ConsumeLength(
      range, css_parser_mode, kValueRangeAll,
      css_property_parser_helpers::UnitlessQuirk::kAllow);
}

}  // namespace
namespace css_longhand {

const CSSValue* Clip::ParseSingleValue(CSSParserTokenRange& range,
                                       const CSSParserContext& context,
                                       const CSSParserLocalContext&) const {
  if (range.Peek().Id() == CSSValueID::kAuto)
    return css_property_parser_helpers::ConsumeIdent(range);

  if (range.Peek().FunctionId() != CSSValueID::kRect)
    return nullptr;

  CSSParserTokenRange args =
      css_property_parser_helpers::ConsumeFunction(range);
  // rect(t, r, b, l) || rect(t r b l)
  CSSValue* top = ConsumeClipComponent(args, context.Mode());
  if (!top)
    return nullptr;
  bool needs_comma =
      css_property_parser_helpers::ConsumeCommaIncludingWhitespace(args);
  CSSValue* right = ConsumeClipComponent(args, context.Mode());
  if (!right ||
      (needs_comma &&
       !css_property_parser_helpers::ConsumeCommaIncludingWhitespace(args)))
    return nullptr;
  CSSValue* bottom = ConsumeClipComponent(args, context.Mode());
  if (!bottom ||
      (needs_comma &&
       !css_property_parser_helpers::ConsumeCommaIncludingWhitespace(args)))
    return nullptr;
  CSSValue* left = ConsumeClipComponent(args, context.Mode());
  if (!left || !args.AtEnd())
    return nullptr;
  return MakeGarbageCollected<CSSQuadValue>(top, right, bottom, left,
                                            CSSQuadValue::kSerializeAsRect);
}

const CSSValue* Clip::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  if (style.HasAutoClip())
    return CSSIdentifierValue::Create(CSSValueID::kAuto);
  CSSValue* top = ComputedStyleUtils::ZoomAdjustedPixelValueOrAuto(
      style.Clip().Top(), style);
  CSSValue* right = ComputedStyleUtils::ZoomAdjustedPixelValueOrAuto(
      style.Clip().Right(), style);
  CSSValue* bottom = ComputedStyleUtils::ZoomAdjustedPixelValueOrAuto(
      style.Clip().Bottom(), style);
  CSSValue* left = ComputedStyleUtils::ZoomAdjustedPixelValueOrAuto(
      style.Clip().Left(), style);
  return MakeGarbageCollected<CSSQuadValue>(top, right, bottom, left,
                                            CSSQuadValue::kSerializeAsRect);
}

}  // namespace css_longhand
}  // namespace blink
