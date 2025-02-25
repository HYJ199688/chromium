// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/webkit_box_reflect.h"

#include "third_party/blink/renderer/core/css/css_primitive_value.h"
#include "third_party/blink/renderer/core/css/css_reflect_value.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_context.h"
#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {
namespace {

CSSValue* ConsumeReflect(CSSParserTokenRange& range,
                         const CSSParserContext& context) {
  CSSIdentifierValue* direction = css_property_parser_helpers::ConsumeIdent<
      CSSValueID::kAbove, CSSValueID::kBelow, CSSValueID::kLeft,
      CSSValueID::kRight>(range);
  if (!direction)
    return nullptr;

  CSSPrimitiveValue* offset = nullptr;
  if (range.AtEnd()) {
    offset = CSSPrimitiveValue::Create(0, CSSPrimitiveValue::UnitType::kPixels);
  } else {
    offset = ConsumeLengthOrPercent(
        range, context.Mode(), kValueRangeAll,
        css_property_parser_helpers::UnitlessQuirk::kForbid);
    if (!offset)
      return nullptr;
  }

  CSSValue* mask = nullptr;
  if (!range.AtEnd()) {
    mask = css_parsing_utils::ConsumeWebkitBorderImage(range, context);
    if (!mask)
      return nullptr;
  }
  return MakeGarbageCollected<cssvalue::CSSReflectValue>(direction, offset,
                                                         mask);
}

}  // namespace
namespace css_longhand {

const CSSValue* WebkitBoxReflect::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  return ConsumeReflect(range, context);
}

const CSSValue* WebkitBoxReflect::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node*,
    bool allow_visited_style) const {
  return ComputedStyleUtils::ValueForReflection(style.BoxReflect(), style);
}

}  // namespace css_longhand
}  // namespace blink
