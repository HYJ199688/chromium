// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/shorthands/font_variant.h"

#include "third_party/blink/renderer/core/css/css_identifier_value.h"
#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/parser/font_variant_east_asian_parser.h"
#include "third_party/blink/renderer/core/css/parser/font_variant_ligatures_parser.h"
#include "third_party/blink/renderer/core/css/parser/font_variant_numeric_parser.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {
namespace css_shorthand {

bool FontVariant::ParseShorthand(
    bool important,
    CSSParserTokenRange& range,
    const CSSParserContext&,
    const CSSParserLocalContext&,
    HeapVector<CSSPropertyValue, 256>& properties) const {
  if (css_property_parser_helpers::IdentMatches<CSSValueID::kNormal,
                                                CSSValueID::kNone>(
          range.Peek().Id())) {
    css_property_parser_helpers::AddProperty(
        CSSPropertyID::kFontVariantLigatures, CSSPropertyID::kFontVariant,
        *css_property_parser_helpers::ConsumeIdent(range), important,
        css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
        properties);
    css_property_parser_helpers::AddProperty(
        CSSPropertyID::kFontVariantCaps, CSSPropertyID::kFontVariant,
        *CSSIdentifierValue::Create(CSSValueID::kNormal), important,
        css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
        properties);
    css_property_parser_helpers::AddProperty(
        CSSPropertyID::kFontVariantNumeric, CSSPropertyID::kFontVariant,
        *CSSIdentifierValue::Create(CSSValueID::kNormal), important,
        css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
        properties);
    css_property_parser_helpers::AddProperty(
        CSSPropertyID::kFontVariantEastAsian, CSSPropertyID::kFontVariant,
        *CSSIdentifierValue::Create(CSSValueID::kNormal), important,
        css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
        properties);
    return range.AtEnd();
  }

  CSSIdentifierValue* caps_value = nullptr;
  FontVariantLigaturesParser ligatures_parser;
  FontVariantNumericParser numeric_parser;
  FontVariantEastAsianParser east_asian_parser;
  do {
    FontVariantLigaturesParser::ParseResult ligatures_parse_result =
        ligatures_parser.ConsumeLigature(range);
    FontVariantNumericParser::ParseResult numeric_parse_result =
        numeric_parser.ConsumeNumeric(range);
    FontVariantEastAsianParser::ParseResult east_asian_parse_result =
        east_asian_parser.ConsumeEastAsian(range);
    if (ligatures_parse_result ==
            FontVariantLigaturesParser::ParseResult::kConsumedValue ||
        numeric_parse_result ==
            FontVariantNumericParser::ParseResult::kConsumedValue ||
        east_asian_parse_result ==
            FontVariantEastAsianParser::ParseResult::kConsumedValue)
      continue;

    if (ligatures_parse_result ==
            FontVariantLigaturesParser::ParseResult::kDisallowedValue ||
        numeric_parse_result ==
            FontVariantNumericParser::ParseResult::kDisallowedValue ||
        east_asian_parse_result ==
            FontVariantEastAsianParser::ParseResult::kDisallowedValue)
      return false;

    CSSValueID id = range.Peek().Id();
    switch (id) {
      case CSSValueID::kSmallCaps:
      case CSSValueID::kAllSmallCaps:
      case CSSValueID::kPetiteCaps:
      case CSSValueID::kAllPetiteCaps:
      case CSSValueID::kUnicase:
      case CSSValueID::kTitlingCaps:
        // Only one caps value permitted in font-variant grammar.
        if (caps_value)
          return false;
        caps_value = css_property_parser_helpers::ConsumeIdent(range);
        break;
      default:
        return false;
    }
  } while (!range.AtEnd());

  css_property_parser_helpers::AddProperty(
      CSSPropertyID::kFontVariantLigatures, CSSPropertyID::kFontVariant,
      *ligatures_parser.FinalizeValue(), important,
      css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
      properties);
  css_property_parser_helpers::AddProperty(
      CSSPropertyID::kFontVariantNumeric, CSSPropertyID::kFontVariant,
      *numeric_parser.FinalizeValue(), important,
      css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
      properties);
  css_property_parser_helpers::AddProperty(
      CSSPropertyID::kFontVariantEastAsian, CSSPropertyID::kFontVariant,
      *east_asian_parser.FinalizeValue(), important,
      css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
      properties);
  css_property_parser_helpers::AddProperty(
      CSSPropertyID::kFontVariantCaps, CSSPropertyID::kFontVariant,
      caps_value ? *caps_value
                 : *CSSIdentifierValue::Create(CSSValueID::kNormal),
      important, css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
      properties);
  return true;
}

const CSSValue* FontVariant::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject* layout_object,
    Node* styled_node,
    bool allow_visited_style) const {
  return ComputedStyleUtils::ValuesForFontVariantProperty(
      style, layout_object, styled_node, allow_visited_style);
}

}  // namespace css_shorthand
}  // namespace blink
