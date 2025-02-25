// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/shorthands/transition.h"

#include "third_party/blink/renderer/core/css/css_identifier_value.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_context.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_local_context.h"
#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/style_property_shorthand.h"

namespace blink {
namespace {

CSSValue* ConsumeTransitionValue(CSSPropertyID property,
                                 CSSParserTokenRange& range,
                                 const CSSParserContext& context,
                                 bool use_legacy_parsing) {
  switch (property) {
    case CSSPropertyID::kTransitionDelay:
      return css_property_parser_helpers::ConsumeTime(range, kValueRangeAll);
    case CSSPropertyID::kTransitionDuration:
      return css_property_parser_helpers::ConsumeTime(range,
                                                      kValueRangeNonNegative);
    case CSSPropertyID::kTransitionProperty:
      return css_parsing_utils::ConsumeTransitionProperty(range, context);
    case CSSPropertyID::kTransitionTimingFunction:
      return css_parsing_utils::ConsumeAnimationTimingFunction(range);
    default:
      NOTREACHED();
      return nullptr;
  }
}

}  // namespace
namespace css_shorthand {

bool Transition::ParseShorthand(
    bool important,
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext& local_context,
    HeapVector<CSSPropertyValue, 256>& properties) const {
  const StylePropertyShorthand shorthand = transitionShorthandForParsing();
  const unsigned longhand_count = shorthand.length();

  HeapVector<Member<CSSValueList>, css_parsing_utils::kMaxNumAnimationLonghands>
      longhands(longhand_count);
  if (!css_parsing_utils::ConsumeAnimationShorthand(
          shorthand, longhands, ConsumeTransitionValue, range, context,
          local_context.UseAliasParsing())) {
    return false;
  }

  for (unsigned i = 0; i < longhand_count; ++i) {
    if (shorthand.properties()[i]->IDEquals(
            CSSPropertyID::kTransitionProperty) &&
        !css_parsing_utils::IsValidPropertyList(*longhands[i]))
      return false;
  }

  for (unsigned i = 0; i < longhand_count; ++i) {
    css_property_parser_helpers::AddProperty(
        shorthand.properties()[i]->PropertyID(), shorthand.id(), *longhands[i],
        important,
        css_property_parser_helpers::IsImplicitProperty::kNotImplicit,
        properties);
  }

  return range.AtEnd();
}

const CSSValue* Transition::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  const CSSTransitionData* transition_data = style.Transitions();
  if (transition_data) {
    CSSValueList* transitions_list = CSSValueList::CreateCommaSeparated();
    for (wtf_size_t i = 0; i < transition_data->PropertyList().size(); ++i) {
      CSSValueList* list = CSSValueList::CreateSpaceSeparated();
      list->Append(*ComputedStyleUtils::CreateTransitionPropertyValue(
          transition_data->PropertyList()[i]));
      list->Append(*CSSPrimitiveValue::Create(
          CSSTimingData::GetRepeated(transition_data->DurationList(), i),
          CSSPrimitiveValue::UnitType::kSeconds));
      list->Append(*ComputedStyleUtils::CreateTimingFunctionValue(
          CSSTimingData::GetRepeated(transition_data->TimingFunctionList(), i)
              .get()));
      list->Append(*CSSPrimitiveValue::Create(
          CSSTimingData::GetRepeated(transition_data->DelayList(), i),
          CSSPrimitiveValue::UnitType::kSeconds));
      transitions_list->Append(*list);
    }
    return transitions_list;
  }

  CSSValueList* list = CSSValueList::CreateSpaceSeparated();
  // transition-property default value.
  list->Append(*CSSIdentifierValue::Create(CSSValueID::kAll));
  list->Append(
      *CSSPrimitiveValue::Create(CSSTransitionData::InitialDuration(),
                                 CSSPrimitiveValue::UnitType::kSeconds));
  list->Append(*ComputedStyleUtils::CreateTimingFunctionValue(
      CSSTransitionData::InitialTimingFunction().get()));
  list->Append(
      *CSSPrimitiveValue::Create(CSSTransitionData::InitialDelay(),
                                 CSSPrimitiveValue::UnitType::kSeconds));
  return list;
}

}  // namespace css_shorthand
}  // namespace blink
