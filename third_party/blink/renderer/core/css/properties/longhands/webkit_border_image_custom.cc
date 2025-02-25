// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/webkit_border_image.h"

#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {
namespace css_longhand {

const CSSValue* WebkitBorderImage::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  return css_parsing_utils::ConsumeWebkitBorderImage(range, context);
}

const CSSValue* WebkitBorderImage::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node*,
    bool allow_visited_style) const {
  return ComputedStyleUtils::ValueForNinePieceImage(style.BorderImage(), style);
}

void WebkitBorderImage::ApplyValue(StyleResolverState& state,
                                   const CSSValue& value) const {
  NinePieceImage image;
  CSSToStyleMap::MapNinePieceImage(state, CSSPropertyID::kWebkitBorderImage,
                                   value, image);
  state.Style()->SetBorderImage(image);
}

}  // namespace css_longhand
}  // namespace blink
