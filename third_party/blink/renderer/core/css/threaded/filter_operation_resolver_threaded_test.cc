// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/resolver/filter_operation_resolver.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/css/parser/css_parser.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_context.h"
#include "third_party/blink/renderer/core/css/threaded/multi_threaded_test_util.h"
#include "third_party/blink/renderer/core/style/filter_operation.h"
#include "third_party/blink/renderer/platform/fonts/font.h"
#include "third_party/blink/renderer/platform/fonts/font_description.h"

namespace blink {

TSAN_TEST(FilterOperationResolverThreadedTest, SimpleMatrixFilter) {
  RunOnThreads([]() {
    const CSSValue* value = CSSParser::ParseSingleValue(
        CSSPropertyID::kFilter, "sepia(50%)",
        StrictCSSParserContext(SecureContextMode::kInsecureContext));
    ASSERT_TRUE(value);

    FontDescription font_description;
    Font font(font_description);
    FilterOperations fo =
        FilterOperationResolver::CreateOffscreenFilterOperations(*value, font);
    ASSERT_EQ(fo.size(), 1ul);
    EXPECT_EQ(*fo.at(0), *BasicColorMatrixFilterOperation::Create(
                             0.5, FilterOperation::SEPIA));
  });
}

TSAN_TEST(FilterOperationResolverThreadedTest, SimpleTransferFilter) {
  RunOnThreads([]() {
    const CSSValue* value = CSSParser::ParseSingleValue(
        CSSPropertyID::kFilter, "brightness(50%)",
        StrictCSSParserContext(SecureContextMode::kInsecureContext));
    ASSERT_TRUE(value);

    FontDescription font_description;
    Font font(font_description);
    FilterOperations fo =
        FilterOperationResolver::CreateOffscreenFilterOperations(*value, font);
    ASSERT_EQ(fo.size(), 1ul);
    EXPECT_EQ(*fo.at(0), *BasicComponentTransferFilterOperation::Create(
                             0.5, FilterOperation::BRIGHTNESS));
  });
}

TSAN_TEST(FilterOperationResolverThreadedTest, SimpleBlurFilter) {
  RunOnThreads([]() {
    const CSSValue* value = CSSParser::ParseSingleValue(
        CSSPropertyID::kFilter, "blur(10px)",
        StrictCSSParserContext(SecureContextMode::kInsecureContext));
    ASSERT_TRUE(value);

    FontDescription font_description;
    Font font(font_description);
    FilterOperations fo =
        FilterOperationResolver::CreateOffscreenFilterOperations(*value, font);
    ASSERT_EQ(fo.size(), 1ul);
    EXPECT_EQ(*fo.at(0), *BlurFilterOperation::Create(Length::Fixed(10)));
  });
}

TSAN_TEST(FilterOperationResolverThreadedTest, SimpleDropShadow) {
  RunOnThreads([]() {
    const CSSValue* value = CSSParser::ParseSingleValue(
        CSSPropertyID::kFilter, "drop-shadow(10px 5px 1px black)",
        StrictCSSParserContext(SecureContextMode::kInsecureContext));
    ASSERT_TRUE(value);

    FontDescription font_description;
    Font font(font_description);
    FilterOperations fo =
        FilterOperationResolver::CreateOffscreenFilterOperations(*value, font);
    ASSERT_EQ(fo.size(), 1ul);
    EXPECT_EQ(*fo.at(0), *DropShadowFilterOperation::Create(ShadowData(
                             FloatPoint(10, 5), 1, 0, ShadowStyle::kNormal,
                             StyleColor(Color::kBlack))));
  });
}

TSAN_TEST(FilterOperationResolverThreadedTest, CompoundFilter) {
  RunOnThreads([]() {
    const CSSValue* value = CSSParser::ParseSingleValue(
        CSSPropertyID::kFilter, "sepia(50%) brightness(50%)",
        StrictCSSParserContext(SecureContextMode::kInsecureContext));
    ASSERT_TRUE(value);

    FontDescription font_description;
    Font font(font_description);
    FilterOperations fo =
        FilterOperationResolver::CreateOffscreenFilterOperations(*value, font);
    EXPECT_FALSE(fo.IsEmpty());
    ASSERT_EQ(fo.size(), 2ul);
    EXPECT_EQ(*fo.at(0), *BasicColorMatrixFilterOperation::Create(
                             0.5, FilterOperation::SEPIA));
    EXPECT_EQ(*fo.at(1), *BasicComponentTransferFilterOperation::Create(
                             0.5, FilterOperation::BRIGHTNESS));
  });
}

}  // namespace blink
