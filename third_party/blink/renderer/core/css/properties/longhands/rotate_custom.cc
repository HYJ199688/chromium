// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/rotate.h"

#include "third_party/blink/renderer/core/css/css_axis_value.h"
#include "third_party/blink/renderer/core/css/css_value_list.h"
#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css_value_keywords.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"

namespace blink {
namespace css_longhand {

const CSSValue* Rotate::ParseSingleValue(CSSParserTokenRange& range,
                                         const CSSParserContext& context,
                                         const CSSParserLocalContext&) const {
  DCHECK(RuntimeEnabledFeatures::CSSIndependentTransformPropertiesEnabled());

  CSSValueID id = range.Peek().Id();
  if (id == CSSValueID::kNone)
    return css_property_parser_helpers::ConsumeIdent(range);

  CSSValueList* list = CSSValueList::CreateSpaceSeparated();

  CSSValue* rotation = css_property_parser_helpers::ConsumeAngle(
      range, &context, base::Optional<WebFeature>());

  CSSValue* axis = css_property_parser_helpers::ConsumeAxis(range);
  if (axis)
    list->Append(*axis);
  else if (!rotation)
    return nullptr;

  if (!rotation) {
    rotation = css_property_parser_helpers::ConsumeAngle(
        range, &context, base::Optional<WebFeature>());
    if (!rotation)
      return nullptr;
  }
  list->Append(*rotation);

  return list;
}

const CSSValue* Rotate::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  if (!style.Rotate())
    return CSSIdentifierValue::Create(CSSValueID::kNone);

  CSSValueList* list = CSSValueList::CreateSpaceSeparated();
  if (style.Rotate()->X() != 0 || style.Rotate()->Y() != 0 ||
      style.Rotate()->Z() != 1) {
    list->Append(*CSSPrimitiveValue::Create(
        style.Rotate()->X(), CSSPrimitiveValue::UnitType::kNumber));
    list->Append(*CSSPrimitiveValue::Create(
        style.Rotate()->Y(), CSSPrimitiveValue::UnitType::kNumber));
    list->Append(*CSSPrimitiveValue::Create(
        style.Rotate()->Z(), CSSPrimitiveValue::UnitType::kNumber));
  }
  list->Append(*CSSPrimitiveValue::Create(
      style.Rotate()->Angle(), CSSPrimitiveValue::UnitType::kDegrees));
  return list;
}

}  // namespace css_longhand
}  // namespace blink
