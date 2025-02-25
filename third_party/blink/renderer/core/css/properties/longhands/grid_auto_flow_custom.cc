// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/grid_auto_flow.h"

#include "third_party/blink/renderer/core/css/css_identifier_value.h"
#include "third_party/blink/renderer/core/css/css_value_list.h"
#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {
namespace css_longhand {

const CSSValue* GridAutoFlow::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  CSSIdentifierValue* row_or_column_value =
      css_property_parser_helpers::ConsumeIdent<CSSValueID::kRow,
                                                CSSValueID::kColumn>(range);
  CSSIdentifierValue* dense_algorithm =
      css_property_parser_helpers::ConsumeIdent<CSSValueID::kDense>(range);
  if (!row_or_column_value) {
    row_or_column_value =
        css_property_parser_helpers::ConsumeIdent<CSSValueID::kRow,
                                                  CSSValueID::kColumn>(range);
    if (!row_or_column_value && !dense_algorithm)
      return nullptr;
  }
  CSSValueList* parsed_values = CSSValueList::CreateSpaceSeparated();
  if (row_or_column_value)
    parsed_values->Append(*row_or_column_value);
  if (dense_algorithm)
    parsed_values->Append(*dense_algorithm);
  return parsed_values;
}

const CSSValue* GridAutoFlow::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  CSSValueList* list = CSSValueList::CreateSpaceSeparated();
  switch (style.GetGridAutoFlow()) {
    case kAutoFlowRow:
    case kAutoFlowRowDense:
      list->Append(*CSSIdentifierValue::Create(CSSValueID::kRow));
      break;
    case kAutoFlowColumn:
    case kAutoFlowColumnDense:
      list->Append(*CSSIdentifierValue::Create(CSSValueID::kColumn));
      break;
    default:
      NOTREACHED();
  }

  switch (style.GetGridAutoFlow()) {
    case kAutoFlowRowDense:
    case kAutoFlowColumnDense:
      list->Append(*CSSIdentifierValue::Create(CSSValueID::kDense));
      break;
    default:
      // Do nothing.
      break;
  }

  return list;
}

}  // namespace css_longhand
}  // namespace blink
