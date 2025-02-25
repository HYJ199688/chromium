// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/clip_path.h"

#include "third_party/blink/renderer/core/css/basic_shape_functions.h"
#include "third_party/blink/renderer/core/css/css_uri_value.h"
#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/style/reference_clip_path_operation.h"
#include "third_party/blink/renderer/core/style/shape_clip_path_operation.h"

namespace blink {
namespace css_longhand {

const CSSValue* ClipPath::ParseSingleValue(CSSParserTokenRange& range,
                                           const CSSParserContext& context,
                                           const CSSParserLocalContext&) const {
  if (range.Peek().Id() == CSSValueID::kNone)
    return css_property_parser_helpers::ConsumeIdent(range);
  if (cssvalue::CSSURIValue* url =
          css_property_parser_helpers::ConsumeUrl(range, &context))
    return url;
  return css_parsing_utils::ConsumeBasicShape(range, context);
}

const CSSValue* ClipPath::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  if (ClipPathOperation* operation = style.ClipPath()) {
    if (operation->GetType() == ClipPathOperation::SHAPE) {
      return ValueForBasicShape(
          style, To<ShapeClipPathOperation>(operation)->GetBasicShape());
    }
    if (operation->GetType() == ClipPathOperation::REFERENCE) {
      return cssvalue::CSSURIValue::Create(
          To<ReferenceClipPathOperation>(operation)->Url());
    }
  }
  return CSSIdentifierValue::Create(CSSValueID::kNone);
}

}  // namespace css_longhand
}  // namespace blink
