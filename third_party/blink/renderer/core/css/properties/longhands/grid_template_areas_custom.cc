// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/grid_template_areas.h"

#include "third_party/blink/renderer/core/css/css_grid_template_areas_value.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_token.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_token_range.h"
#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/css/resolver/style_builder_converter.h"
#include "third_party/blink/renderer/core/css_value_keywords.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/style/grid_area.h"

namespace blink {
namespace css_longhand {

const CSSValue* GridTemplateAreas::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext&,
    const CSSParserLocalContext&) const {
  if (range.Peek().Id() == CSSValueID::kNone)
    return css_property_parser_helpers::ConsumeIdent(range);

  NamedGridAreaMap grid_area_map;
  size_t row_count = 0;
  size_t column_count = 0;

  while (range.Peek().GetType() == kStringToken) {
    if (!css_parsing_utils::ParseGridTemplateAreasRow(
            range.ConsumeIncludingWhitespace().Value().ToString(),
            grid_area_map, row_count, column_count))
      return nullptr;
    ++row_count;
  }

  if (row_count == 0)
    return nullptr;
  DCHECK(column_count);
  return MakeGarbageCollected<cssvalue::CSSGridTemplateAreasValue>(
      grid_area_map, row_count, column_count);
}

const CSSValue* GridTemplateAreas::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  if (!style.NamedGridAreaRowCount()) {
    DCHECK(!style.NamedGridAreaColumnCount());
    return CSSIdentifierValue::Create(CSSValueID::kNone);
  }

  return MakeGarbageCollected<cssvalue::CSSGridTemplateAreasValue>(
      style.NamedGridArea(), style.NamedGridAreaRowCount(),
      style.NamedGridAreaColumnCount());
}

void GridTemplateAreas::ApplyInitial(StyleResolverState& state) const {
  state.Style()->SetNamedGridArea(
      ComputedStyleInitialValues::InitialNamedGridArea());
  state.Style()->SetNamedGridAreaRowCount(
      ComputedStyleInitialValues::InitialNamedGridAreaRowCount());
  state.Style()->SetNamedGridAreaColumnCount(
      ComputedStyleInitialValues::InitialNamedGridAreaColumnCount());
}

void GridTemplateAreas::ApplyInherit(StyleResolverState& state) const {
  state.Style()->SetNamedGridArea(state.ParentStyle()->NamedGridArea());
  state.Style()->SetNamedGridAreaRowCount(
      state.ParentStyle()->NamedGridAreaRowCount());
  state.Style()->SetNamedGridAreaColumnCount(
      state.ParentStyle()->NamedGridAreaColumnCount());
}

void GridTemplateAreas::ApplyValue(StyleResolverState& state,
                                   const CSSValue& value) const {
  if (auto* identifier_value = DynamicTo<CSSIdentifierValue>(value)) {
    // FIXME: Shouldn't we clear the grid-area values
    DCHECK_EQ(identifier_value->GetValueID(), CSSValueID::kNone);
    return;
  }

  const auto& grid_template_areas_value =
      To<cssvalue::CSSGridTemplateAreasValue>(value);
  const NamedGridAreaMap& new_named_grid_areas =
      grid_template_areas_value.GridAreaMap();

  NamedGridLinesMap named_grid_column_lines;
  NamedGridLinesMap named_grid_row_lines;
  StyleBuilderConverter::ConvertOrderedNamedGridLinesMapToNamedGridLinesMap(
      state.Style()->OrderedNamedGridColumnLines(), named_grid_column_lines);
  StyleBuilderConverter::ConvertOrderedNamedGridLinesMapToNamedGridLinesMap(
      state.Style()->OrderedNamedGridRowLines(), named_grid_row_lines);
  StyleBuilderConverter::CreateImplicitNamedGridLinesFromGridArea(
      new_named_grid_areas, named_grid_column_lines, kForColumns);
  StyleBuilderConverter::CreateImplicitNamedGridLinesFromGridArea(
      new_named_grid_areas, named_grid_row_lines, kForRows);
  state.Style()->SetNamedGridColumnLines(named_grid_column_lines);
  state.Style()->SetNamedGridRowLines(named_grid_row_lines);

  state.Style()->SetNamedGridArea(new_named_grid_areas);
  state.Style()->SetNamedGridAreaRowCount(grid_template_areas_value.RowCount());
  state.Style()->SetNamedGridAreaColumnCount(
      grid_template_areas_value.ColumnCount());
}

}  // namespace css_longhand
}  // namespace blink
