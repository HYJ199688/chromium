// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/layout/layout_embedded_content.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/core/paint/paint_property_tree_printer.h"
#include "third_party/blink/renderer/core/testing/core_unit_test_helper.h"
#include "third_party/blink/renderer/platform/graphics/paint/geometry_mapper.h"
#include "third_party/blink/renderer/platform/testing/paint_test_configurations.h"

namespace blink {

class VisualRectMappingTest : public PaintTestConfigurations,
                              public RenderingTest {
 public:
  VisualRectMappingTest()
      : RenderingTest(SingleChildLocalFrameClient::Create()) {}

 protected:
  LayoutView& GetLayoutView() const { return *GetDocument().GetLayoutView(); }

  enum Flags { kContainsEnclosingIntRect = 1 << 0 };

  void CheckPaintInvalidationVisualRect(
      const LayoutObject& object,
      const LayoutBoxModelObject& ancestor,
      const LayoutRect& expected_visual_rect_in_ancestor) {
    LayoutRect rect = object.LocalVisualRect();
    if (object.IsBox())
      ToLayoutBox(object).FlipForWritingMode(rect);

    if (!RuntimeEnabledFeatures::CompositeAfterPaintEnabled())
      EXPECT_EQ(&ancestor, &object.ContainerForPaintInvalidation());

    CheckVisualRect(object, ancestor, rect, expected_visual_rect_in_ancestor);
  }

  void CheckVisualRect(const LayoutObject& object,
                       const LayoutBoxModelObject& ancestor,
                       const LayoutRect& local_rect,
                       const LayoutRect& expected_visual_rect_in_ancestor,
                       unsigned flags = 0) {
    LayoutRect slow_map_rect = local_rect;
    object.MapToVisualRectInAncestorSpace(&ancestor, slow_map_rect);

    FloatClipRect geometry_mapper_rect((FloatRect(local_rect)));
    const FragmentData& fragment_data = object.FirstFragment();
    if (fragment_data.HasLocalBorderBoxProperties()) {
      LayoutRect local_rect_copy(local_rect);
      object.MapToVisualRectInAncestorSpace(&ancestor, local_rect_copy,
                                            kUseGeometryMapper);
      geometry_mapper_rect.SetRect(FloatRect(local_rect_copy));
    }

    if (expected_visual_rect_in_ancestor.IsEmpty()) {
      EXPECT_TRUE(slow_map_rect.IsEmpty());
      if (fragment_data.HasLocalBorderBoxProperties())
        EXPECT_TRUE(geometry_mapper_rect.Rect().IsEmpty());
      return;
    }

    // The following condition can be false if paintInvalidationContainer is
    // a LayoutView and compositing is not enabled.
    if (!RuntimeEnabledFeatures::CompositeAfterPaintEnabled() &&
        ancestor.IsPaintInvalidationContainer()) {
      PaintLayer::MapRectInPaintInvalidationContainerToBacking(ancestor,
                                                               slow_map_rect);
      LayoutRect temp(geometry_mapper_rect.Rect());
      PaintLayer::MapRectInPaintInvalidationContainerToBacking(ancestor, temp);
      geometry_mapper_rect = FloatClipRect(FloatRect(temp));
    }

    if (flags & kContainsEnclosingIntRect) {
      EXPECT_TRUE(
          EnclosingIntRect(slow_map_rect)
              .Contains(EnclosingIntRect(expected_visual_rect_in_ancestor)));

      if (object.FirstFragment().HasLocalBorderBoxProperties()) {
        EXPECT_TRUE(
            EnclosingIntRect(geometry_mapper_rect.Rect())
                .Contains(EnclosingIntRect(expected_visual_rect_in_ancestor)));
      }
    } else {
      EXPECT_EQ(expected_visual_rect_in_ancestor, slow_map_rect);
      if (object.FirstFragment().HasLocalBorderBoxProperties()) {
        EXPECT_EQ(expected_visual_rect_in_ancestor,
                  LayoutRect(geometry_mapper_rect.Rect()));
      }
    }
  }

  // Checks the result of MapToVisualRectInAncestorSpace with and without
  // geometry mapper.
  void CheckMapToVisualRectInAncestorSpace(LayoutRect rect,
                                           LayoutRect expected,
                                           const LayoutObject* object,
                                           const LayoutBoxModelObject* ancestor,
                                           VisualRectFlags flags,
                                           bool expected_retval) {
    LayoutRect result(rect);
    EXPECT_EQ(expected_retval,
              object->MapToVisualRectInAncestorSpace(ancestor, result, flags));
    EXPECT_EQ(result, expected);
    result = rect;
    EXPECT_EQ(expected_retval,
              object->MapToVisualRectInAncestorSpace(
                  ancestor, result,
                  static_cast<VisualRectFlags>(flags | kUseGeometryMapper)));
    EXPECT_EQ(result, expected);
  }
};

INSTANTIATE_PAINT_TEST_SUITE_P(VisualRectMappingTest);

TEST_P(VisualRectMappingTest, LayoutText) {
  SetBodyInnerHTML(R"HTML(
    <style>body { margin: 0; }</style>
    <div id='container' style='vertical-align: bottom; overflow: scroll;
        width: 50px; height: 50px'>
      <span><img style='width: 20px; height: 100px'></span>
      <span id='text'>text text text text text text text</span>
    </div>
  )HTML");

  auto* container = ToLayoutBlock(GetLayoutObjectByElementId("container"));
  auto* text = GetLayoutObjectByElementId("text")->SlowFirstChild();

  container->SetScrollTop(LayoutUnit(50));
  UpdateAllLifecyclePhasesForTest();

  LayoutRect original_rect(0, 60, 20, 80);
  LayoutRect rect = original_rect;
  EXPECT_TRUE(text->MapToVisualRectInAncestorSpace(container, rect));
  rect.Move(-container->ScrolledContentOffset());
  EXPECT_EQ(rect, LayoutRect(0, 10, 20, 80));

  rect = original_rect;
  EXPECT_TRUE(text->MapToVisualRectInAncestorSpace(&GetLayoutView(), rect));
  EXPECT_EQ(rect, LayoutRect(0, 10, 20, 40));

  rect = LayoutRect(0, 60, 80, 0);
  EXPECT_TRUE(
      text->MapToVisualRectInAncestorSpace(container, rect, kEdgeInclusive));
  rect.Move(-container->ScrolledContentOffset());
  EXPECT_EQ(rect, LayoutRect(0, 10, 80, 0));
}

TEST_P(VisualRectMappingTest, LayoutInline) {
  GetDocument().SetBaseURLOverride(KURL("http://test.com"));
  SetBodyInnerHTML(R"HTML(
    <style>body { margin: 0; }</style>
    <div id='container' style='overflow: scroll; width: 50px; height: 50px'>
      <span><img style='width: 20px; height: 100px'></span>
      <span id='leaf'></span>
    </div>
  )HTML");

  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  LayoutObject* leaf = container->LastChild();

  container->SetScrollTop(LayoutUnit(50));
  UpdateAllLifecyclePhasesForTest();

  LayoutRect original_rect(0, 60, 20, 80);
  LayoutRect rect = original_rect;
  EXPECT_TRUE(leaf->MapToVisualRectInAncestorSpace(container, rect));
  rect.Move(-container->ScrolledContentOffset());
  EXPECT_EQ(rect, LayoutRect(0, 10, 20, 80));

  rect = original_rect;
  EXPECT_TRUE(leaf->MapToVisualRectInAncestorSpace(&GetLayoutView(), rect));
  EXPECT_EQ(rect, LayoutRect(0, 10, 20, 40));

  // The span is empty.
  CheckPaintInvalidationVisualRect(*leaf, GetLayoutView(), LayoutRect());

  rect = LayoutRect(0, 60, 80, 0);
  EXPECT_TRUE(
      leaf->MapToVisualRectInAncestorSpace(container, rect, kEdgeInclusive));
  rect.Move(-container->ScrolledContentOffset());
  EXPECT_EQ(rect, LayoutRect(0, 10, 80, 0));
}

TEST_P(VisualRectMappingTest, LayoutView) {
  GetDocument().SetBaseURLOverride(KURL("http://test.com"));
  SetBodyInnerHTML(R"HTML(
    <style>body { margin: 0; }</style>
    <div id=frameContainer>
      <iframe src='http://test.com' width='50' height='50'
          frameBorder='0'></iframe>
    </div>
  )HTML");
  SetChildFrameHTML(
      "<style>body { margin: 0; }</style>"
      "<span><img style='width: 20px; height: 100px'></span>text text text");
  UpdateAllLifecyclePhasesForTest();

  LayoutBlock* frame_container =
      ToLayoutBlock(GetLayoutObjectByElementId("frameContainer"));
  LayoutBlock* frame_body =
      ToLayoutBlock(ChildDocument().body()->GetLayoutObject());
  LayoutText* frame_text = ToLayoutText(frame_body->LastChild());

  // This case involves clipping: frame height is 50, y-coordinate of result
  // rect is 13, so height should be clipped to (50 - 13) == 37.
  ChildDocument().View()->LayoutViewport()->SetScrollOffset(
      ScrollOffset(0, 47), kProgrammaticScroll);
  UpdateAllLifecyclePhasesForTest();

  LayoutRect original_rect(4, 60, 20, 80);
  LayoutRect rect = original_rect;
  EXPECT_TRUE(
      frame_text->MapToVisualRectInAncestorSpace(frame_container, rect));
  EXPECT_EQ(rect, LayoutRect(4, 13, 20, 37));

  rect = original_rect;
  EXPECT_TRUE(
      frame_text->MapToVisualRectInAncestorSpace(&GetLayoutView(), rect));
  EXPECT_EQ(rect, LayoutRect(4, 13, 20, 37));

  CheckPaintInvalidationVisualRect(*frame_text, GetLayoutView(), LayoutRect());

  rect = LayoutRect(4, 60, 0, 80);
  EXPECT_TRUE(frame_text->MapToVisualRectInAncestorSpace(frame_container, rect,
                                                         kEdgeInclusive));
  EXPECT_EQ(rect, LayoutRect(4, 13, 0, 37));
}

TEST_P(VisualRectMappingTest, LayoutViewSubpixelRounding) {
  GetDocument().SetBaseURLOverride(KURL("http://test.com"));
  SetBodyInnerHTML(R"HTML(
    <style>body { margin: 0; }</style>
    <div id=frameContainer style='position: relative; left: 0.5px'>
      <iframe style='position: relative; left: 0.5px' width='200'
          height='200' src='http://test.com' frameBorder='0'></iframe>
    </div>
  )HTML");
  SetChildFrameHTML(R"HTML(
    <style>body { margin: 0; }</style>
    <div id='target' style='position: relative; width: 100px; height: 100px;
        left: 0.5px'></div>
  )HTML");

  UpdateAllLifecyclePhasesForTest();

  LayoutBlock* frame_container =
      ToLayoutBlock(GetLayoutObjectByElementId("frameContainer"));
  LayoutObject* target =
      ChildDocument().getElementById("target")->GetLayoutObject();
  LayoutRect rect(0, 0, 100, 100);
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(frame_container, rect));
  // When passing from the iframe to the parent frame, the rect of (0.5, 0, 100,
  // 100) is expanded to (0, 0, 100, 100), and then offset by the 0.5 offset of
  // frameContainer.
  EXPECT_EQ(LayoutRect(LayoutPoint(DoublePoint(0.5, 0)), LayoutSize(101, 100)),
            rect);
}

TEST_P(VisualRectMappingTest, LayoutViewDisplayNone) {
  GetDocument().SetBaseURLOverride(KURL("http://test.com"));
  SetBodyInnerHTML(R"HTML(
    <style>body { margin: 0; }</style>
    <div id=frameContainer>
      <iframe id='frame' src='http://test.com' width='50' height='50'
          frameBorder='0'></iframe>
    </div>
  )HTML");
  SetChildFrameHTML(
      "<style>body { margin: 0; }</style>"
      "<div style='width:100px;height:100px;'></div>");
  UpdateAllLifecyclePhasesForTest();

  LayoutBlock* frame_container =
      ToLayoutBlock(GetLayoutObjectByElementId("frameContainer"));
  LayoutBlock* frame_body =
      ToLayoutBlock(ChildDocument().body()->GetLayoutObject());
  LayoutBlock* frame_div = ToLayoutBlock(frame_body->LastChild());

  // This part is copied from the LayoutView test, just to ensure that the
  // mapped rect is valid before display:none is set on the iframe.
  ChildDocument().View()->LayoutViewport()->SetScrollOffset(
      ScrollOffset(0, 47), kProgrammaticScroll);
  UpdateAllLifecyclePhasesForTest();

  LayoutRect original_rect(4, 60, 20, 80);
  LayoutRect rect = original_rect;
  EXPECT_TRUE(frame_div->MapToVisualRectInAncestorSpace(frame_container, rect));
  EXPECT_EQ(rect, LayoutRect(4, 13, 20, 37));

  Element* frame_element = GetDocument().getElementById("frame");
  frame_element->SetInlineStyleProperty(CSSPropertyID::kDisplay, "none");
  UpdateAllLifecyclePhasesForTest();

  frame_body = ToLayoutBlock(ChildDocument().body()->GetLayoutObject());
  EXPECT_EQ(nullptr, frame_body);
}

TEST_P(VisualRectMappingTest, SelfFlippedWritingMode) {
  SetBodyInnerHTML(R"HTML(
    <div id='target' style='writing-mode: vertical-rl;
        box-shadow: 40px 20px black; width: 100px; height: 50px;
        position: absolute; top: 111px; left: 222px'>
    </div>
  )HTML");

  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect local_visual_rect = target->LocalVisualRect();
  // -40 = -box_shadow_offset_x(40) (with target's top-right corner as the
  // origin)
  // 140 = width(100) + box_shadow_offset_x(40)
  // 70 = height(50) + box_shadow_offset_y(20)
  EXPECT_EQ(LayoutRect(-40, 0, 140, 70), local_visual_rect);

  LayoutRect rect = local_visual_rect;
  // TODO(wkorman): The calls to flipForWritingMode() here and in other test
  // cases below are necessary because mapToVisualRectInAncestorSpace()
  // currently expects the input rect to be in "physical coordinates" (*not*
  // "physical coordinates with flipped block-flow direction"), see
  // LayoutBoxModelObject.h.
  target->FlipForWritingMode(rect);
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(target, rect));
  // This rect is in physical coordinates of target.
  EXPECT_EQ(LayoutRect(0, 0, 140, 70), rect);

  CheckPaintInvalidationVisualRect(*target, GetLayoutView(),
                                   LayoutRect(222, 111, 140, 70));
}

TEST_P(VisualRectMappingTest, ContainerFlippedWritingMode) {
  SetBodyInnerHTML(R"HTML(
    <div id='container' style='writing-mode: vertical-rl;
        position: absolute; top: 111px; left: 222px'>
      <div id='target' style='box-shadow: 40px 20px black; width: 100px;
          height: 90px'></div>
      <div style='width: 100px; height: 100px'></div>
    </div>
  )HTML");

  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect target_local_visual_rect = target->LocalVisualRect();
  // -40 = -box_shadow_offset_x(40) (with target's top-right corner as the
  // origin)
  // 140 = width(100) + box_shadow_offset_x(40)
  // 110 = height(90) + box_shadow_offset_y(20)
  EXPECT_EQ(LayoutRect(-40, 0, 140, 110), target_local_visual_rect);

  LayoutRect rect = target_local_visual_rect;
  target->FlipForWritingMode(rect);
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(target, rect));
  // This rect is in physical coordinates of target.
  EXPECT_EQ(LayoutRect(0, 0, 140, 110), rect);

  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  rect = target_local_visual_rect;
  target->FlipForWritingMode(rect);
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(container, rect));
  // 100 is the physical x location of target in container.
  EXPECT_EQ(LayoutRect(100, 0, 140, 110), rect);

  CheckPaintInvalidationVisualRect(*target, GetLayoutView(),
                                   LayoutRect(322, 111, 140, 110));

  LayoutRect container_local_visual_rect = container->LocalVisualRect();
  EXPECT_EQ(LayoutRect(0, 0, 200, 100), container_local_visual_rect);
  rect = container_local_visual_rect;
  container->FlipForWritingMode(rect);
  EXPECT_TRUE(container->MapToVisualRectInAncestorSpace(container, rect));
  EXPECT_EQ(LayoutRect(0, 0, 200, 100), rect);
  rect = container_local_visual_rect;
  container->FlipForWritingMode(rect);
  EXPECT_TRUE(
      container->MapToVisualRectInAncestorSpace(&GetLayoutView(), rect));
  EXPECT_EQ(LayoutRect(222, 111, 200, 100), rect);
}

TEST_P(VisualRectMappingTest, ContainerOverflowScroll) {
  SetBodyInnerHTML(R"HTML(
    <div id='container' style='position: absolute; top: 111px; left: 222px;
        border: 10px solid red; overflow: scroll; width: 50px;
        height: 80px'>
      <div id='target' style='box-shadow: 40px 20px black; width: 100px;
          height: 90px'></div>
    </div>
  )HTML");

  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  EXPECT_EQ(LayoutUnit(), container->ScrollTop());
  EXPECT_EQ(LayoutUnit(), container->ScrollLeft());
  container->SetScrollTop(LayoutUnit(7));
  container->SetScrollLeft(LayoutUnit(8));
  UpdateAllLifecyclePhasesForTest();

  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect target_local_visual_rect = target->LocalVisualRect();
  // 140 = width(100) + box_shadow_offset_x(40)
  // 110 = height(90) + box_shadow_offset_y(20)
  EXPECT_EQ(LayoutRect(0, 0, 140, 110), target_local_visual_rect);
  LayoutRect rect = target_local_visual_rect;
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(target, rect));
  EXPECT_EQ(LayoutRect(0, 0, 140, 110), rect);

  rect = target_local_visual_rect;
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(container, rect));
  rect.Move(-container->ScrolledContentOffset());
  // 2 = target_x(0) + container_border_left(10) - scroll_left(8)
  // 3 = target_y(0) + container_border_top(10) - scroll_top(7)
  // Rect is not clipped by container's overflow clip because of
  // overflow:scroll.
  EXPECT_EQ(LayoutRect(2, 3, 140, 110), rect);

  // (2, 3, 140, 100) is first clipped by container's overflow clip, to
  // (10, 10, 50, 80), then is by added container's offset in LayoutView
  // (222, 111).
  CheckPaintInvalidationVisualRect(*target, GetLayoutView(),
                                   LayoutRect(232, 121, 50, 80));

  LayoutRect container_local_visual_rect = container->LocalVisualRect();
  // Because container has overflow clip, its visual overflow doesn't include
  // overflow from children.
  // 70 = width(50) + border_left_width(10) + border_right_width(10)
  // 100 = height(80) + border_top_width(10) + border_bottom_width(10)
  EXPECT_EQ(LayoutRect(0, 0, 70, 100), container_local_visual_rect);
  rect = container_local_visual_rect;
  EXPECT_TRUE(container->MapToVisualRectInAncestorSpace(container, rect));
  // Container should not apply overflow clip on its own overflow rect.
  EXPECT_EQ(LayoutRect(0, 0, 70, 100), rect);

  CheckPaintInvalidationVisualRect(*container, GetLayoutView(),
                                   LayoutRect(222, 111, 70, 100));
}

TEST_P(VisualRectMappingTest, ContainerFlippedWritingModeAndOverflowScroll) {
  SetBodyInnerHTML(R"HTML(
    <div id='container' style='writing-mode: vertical-rl;
        position: absolute; top: 111px; left: 222px; border: solid red;
        border-width: 10px 20px 30px 40px; overflow: scroll; width: 50px;
        height: 80px'>
      <div id='target' style='box-shadow: 40px 20px black; width: 100px;
          height: 90px'></div>
      <div style='width: 100px; height: 100px'></div>
    </div>
  )HTML");

  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  EXPECT_EQ(LayoutUnit(), container->ScrollTop());
  // The initial scroll offset is to the left-most because of flipped blocks
  // writing mode.
  // 150 = total_layout_overflow(100 + 100) - width(50)
  EXPECT_EQ(LayoutUnit(150), container->ScrollLeft());
  container->SetScrollTop(LayoutUnit(7));
  // Scroll to the right by 8 pixels.
  container->SetScrollLeft(LayoutUnit(142));
  UpdateAllLifecyclePhasesForTest();

  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect target_local_visual_rect = target->LocalVisualRect();
  // -40 = -box_shadow_offset_x(40) (with target's top-right corner as the
  // origin)
  // 140 = width(100) + box_shadow_offset_x(40)
  // 110 = height(90) + box_shadow_offset_y(20)
  EXPECT_EQ(LayoutRect(-40, 0, 140, 110), target_local_visual_rect);

  LayoutRect rect = target_local_visual_rect;
  target->FlipForWritingMode(rect);
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(target, rect));
  // This rect is in physical coordinates of target.
  EXPECT_EQ(LayoutRect(0, 0, 140, 110), rect);

  rect = target_local_visual_rect;
  target->FlipForWritingMode(rect);
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(container, rect));
  rect.Move(-container->ScrolledContentOffset());
  // -2 = target_physical_x(100) + container_border_left(40) - scroll_left(142)
  // 3 = target_y(0) + container_border_top(10) - scroll_top(7)
  // Rect is clipped by container's overflow clip because of overflow:scroll.
  EXPECT_EQ(LayoutRect(-2, 3, 140, 110), rect);

  // (-2, 3, 140, 100) is first clipped by container's overflow clip, to
  // (40, 10, 50, 80), then is added by container's offset in LayoutView
  // (222, 111).

  LayoutRect expectation(262, 121, 50, 80);
  if (!RuntimeEnabledFeatures::LayoutNGEnabled()) {
    // TODO(crbug.com/600039): rect.X() should be 262 (left + border-left), but
    // is offset by extra horizontal border-widths because of layout error.
    expectation = LayoutRect(322, 121, 50, 80);
  }
  CheckPaintInvalidationVisualRect(*target, GetLayoutView(), expectation);

  LayoutRect container_local_visual_rect = container->LocalVisualRect();
  // Because container has overflow clip, its visual overflow doesn't include
  // overflow from children.
  // 110 = width(50) + border_left_width(40) + border_right_width(20)
  // 120 = height(80) + border_top_width(10) + border_bottom_width(30)
  EXPECT_EQ(LayoutRect(0, 0, 110, 120), container_local_visual_rect);

  rect = container_local_visual_rect;
  container->FlipForWritingMode(rect);
  EXPECT_TRUE(container->MapToVisualRectInAncestorSpace(container, rect));
  EXPECT_EQ(LayoutRect(0, 0, 110, 120), rect);

  expectation = LayoutRect(222, 111, 110, 120);
  if (!RuntimeEnabledFeatures::LayoutNGEnabled()) {
    // TODO(crbug.com/600039): rect.x() should be 222 (left), but is offset by
    // extra horizontal border-widths because of layout error.
    expectation = LayoutRect(282, 111, 110, 120);
  }
  CheckPaintInvalidationVisualRect(*container, GetLayoutView(), expectation);
}

TEST_P(VisualRectMappingTest, ContainerOverflowHidden) {
  SetBodyInnerHTML(R"HTML(
    <div id='container' style='position: absolute; top: 111px; left: 222px;
        border: 10px solid red; overflow: hidden; width: 50px;
        height: 80px;'>
      <div id='target' style='box-shadow: 40px 20px black; width: 100px;
          height: 90px'></div>
    </div>
  )HTML");

  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  EXPECT_EQ(LayoutUnit(), container->ScrollTop());
  EXPECT_EQ(LayoutUnit(), container->ScrollLeft());
  container->SetScrollTop(LayoutUnit(27));
  container->SetScrollLeft(LayoutUnit(28));
  UpdateAllLifecyclePhasesForTest();

  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect target_local_visual_rect = target->LocalVisualRect();
  // 140 = width(100) + box_shadow_offset_x(40)
  // 110 = height(90) + box_shadow_offset_y(20)
  EXPECT_EQ(LayoutRect(0, 0, 140, 110), target_local_visual_rect);
  LayoutRect rect = target_local_visual_rect;
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(target, rect));
  EXPECT_EQ(LayoutRect(0, 0, 140, 110), rect);

  rect = target_local_visual_rect;
  // Rect is not clipped by container's overflow clip.
  CheckVisualRect(*target, *container, rect, LayoutRect(10, 10, 140, 110));
}

TEST_P(VisualRectMappingTest, ContainerFlippedWritingModeAndOverflowHidden) {
  SetBodyInnerHTML(R"HTML(
    <div id='container' style='writing-mode: vertical-rl;
        position: absolute; top: 111px; left: 222px; border: solid red;
        border-width: 10px 20px 30px 40px; overflow: hidden; width: 50px;
        height: 80px'>
      <div id='target' style='box-shadow: 40px 20px black; width: 100px;
          height: 90px'></div>
      <div style='width: 100px; height: 100px'></div>
    </div>
  )HTML");

  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  EXPECT_EQ(LayoutUnit(), container->ScrollTop());
  // The initial scroll offset is to the left-most because of flipped blocks
  // writing mode.
  // 150 = total_layout_overflow(100 + 100) - width(50)
  EXPECT_EQ(LayoutUnit(150), container->ScrollLeft());
  container->SetScrollTop(LayoutUnit(7));
  container->SetScrollLeft(LayoutUnit(82));  // Scroll to the right by 8 pixels.
  UpdateAllLifecyclePhasesForTest();

  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect target_local_visual_rect = target->LocalVisualRect();
  // -40 = -box_shadow_offset_x(40) (with target's top-right corner as the
  // origin)
  // 140 = width(100) + box_shadow_offset_x(40)
  // 110 = height(90) + box_shadow_offset_y(20)
  EXPECT_EQ(LayoutRect(-40, 0, 140, 110), target_local_visual_rect);

  LayoutRect rect = target_local_visual_rect;
  target->FlipForWritingMode(rect);
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(target, rect));
  // This rect is in physical coordinates of target.
  EXPECT_EQ(LayoutRect(0, 0, 140, 110), rect);

  rect = target_local_visual_rect;
  target->FlipForWritingMode(rect);
  // 58 = target_physical_x(100) + container_border_left(40) - scroll_left(58)
  CheckVisualRect(*target, *container, rect, LayoutRect(-10, 10, 140, 110));
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(container, rect));
}

TEST_P(VisualRectMappingTest, ContainerAndTargetDifferentFlippedWritingMode) {
  SetBodyInnerHTML(R"HTML(
    <div id='container' style='writing-mode: vertical-rl;
        position: absolute; top: 111px; left: 222px; border: solid red;
        border-width: 10px 20px 30px 40px; overflow: scroll; width: 50px;
        height: 80px'>
      <div id='target' style='writing-mode: vertical-lr; width: 100px;
          height: 90px; box-shadow: 40px 20px black'></div>
      <div style='width: 100px; height: 100px'></div>
    </div>
  )HTML");

  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  EXPECT_EQ(LayoutUnit(), container->ScrollTop());
  // The initial scroll offset is to the left-most because of flipped blocks
  // writing mode.
  // 150 = total_layout_overflow(100 + 100) - width(50)
  EXPECT_EQ(LayoutUnit(150), container->ScrollLeft());
  container->SetScrollTop(LayoutUnit(7));
  container->SetScrollLeft(
      LayoutUnit(142));  // Scroll to the right by 8 pixels.
  UpdateAllLifecyclePhasesForTest();

  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect target_local_visual_rect = target->LocalVisualRect();
  // 140 = width(100) + box_shadow_offset_x(40)
  // 110 = height(90) + box_shadow_offset_y(20)
  EXPECT_EQ(LayoutRect(0, 0, 140, 110), target_local_visual_rect);

  LayoutRect rect = target_local_visual_rect;
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(target, rect));
  // This rect is in physical coordinates of target.
  EXPECT_EQ(LayoutRect(0, 0, 140, 110), rect);

  rect = target_local_visual_rect;
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(container, rect));
  rect.Move(-container->ScrolledContentOffset());
  // -2 = target_physical_x(100) + container_border_left(40) - scroll_left(142)
  // 3 = target_y(0) + container_border_top(10) - scroll_top(7)
  // Rect is not clipped by container's overflow clip.
  EXPECT_EQ(LayoutRect(-2, 3, 140, 110), rect);
}

TEST_P(VisualRectMappingTest,
       DifferentPaintInvalidaitionContainerForAbsolutePosition) {
  EnableCompositing();
  GetDocument().GetFrame()->GetSettings()->SetPreferCompositingToLCDTextEnabled(
      true);

  SetBodyInnerHTML(R"HTML(
    <div id='stacking-context' style='opacity: 0.9; background: blue;
        will-change: transform'>
      <div id='scroller' style='overflow: scroll; width: 80px;
          height: 80px'>
        <div id='absolute' style='position: absolute; top: 111px;
            left: 222px; width: 50px; height: 50px; background: green'>
        </div>
        <div id='normal-flow' style='width: 2000px; height: 2000px;
            background: yellow'></div>
      </div>
    </div>
  )HTML");

  LayoutBlock* scroller = ToLayoutBlock(GetLayoutObjectByElementId("scroller"));
  scroller->SetScrollTop(LayoutUnit(77));
  scroller->SetScrollLeft(LayoutUnit(88));
  UpdateAllLifecyclePhasesForTest();

  LayoutBlock* normal_flow =
      ToLayoutBlock(GetLayoutObjectByElementId("normal-flow"));
  if (!RuntimeEnabledFeatures::CompositeAfterPaintEnabled())
    EXPECT_EQ(scroller, &normal_flow->ContainerForPaintInvalidation());

  LayoutRect normal_flow_visual_rect = normal_flow->LocalVisualRect();
  EXPECT_EQ(LayoutRect(0, 0, 2000, 2000), normal_flow_visual_rect);
  LayoutRect rect = normal_flow_visual_rect;
  EXPECT_TRUE(normal_flow->MapToVisualRectInAncestorSpace(scroller, rect));
  EXPECT_EQ(LayoutRect(0, 0, 2000, 2000), rect);
  EXPECT_EQ(rect, normal_flow->FirstFragment().VisualRect());

  LayoutBlock* stacking_context =
      ToLayoutBlock(GetLayoutObjectByElementId("stacking-context"));
  LayoutBlock* absolute = ToLayoutBlock(GetLayoutObjectByElementId("absolute"));
  EXPECT_EQ(stacking_context, absolute->Container());

  EXPECT_EQ(LayoutRect(0, 0, 50, 50), absolute->LocalVisualRect());
  CheckPaintInvalidationVisualRect(*absolute, *stacking_context,
                                   LayoutRect(222, 111, 50, 50));
}

TEST_P(VisualRectMappingTest,
       ContainerOfAbsoluteAbovePaintInvalidationContainer) {
  EnableCompositing();
  GetDocument().GetFrame()->GetSettings()->SetPreferCompositingToLCDTextEnabled(
      true);

  SetBodyInnerHTML(
      "<div id='container' style='position: absolute; top: 88px; left: 99px'>"
      "  <div style='height: 222px'></div>"
      // This div makes stacking-context composited.
      "  <div style='position: absolute; width: 1px; height: 1px; "
      "      background:yellow; will-change: transform'></div>"
      // This stacking context is paintInvalidationContainer of the absolute
      // child, but not a container of it.
      "  <div id='stacking-context' style='opacity: 0.9'>"
      "    <div id='absolute' style='position: absolute; top: 50px; left: 50px;"
      "        width: 50px; height: 50px; background: green'></div>"
      "  </div>"
      "</div>");

  LayoutBlock* stacking_context =
      ToLayoutBlock(GetLayoutObjectByElementId("stacking-context"));
  LayoutBlock* absolute = ToLayoutBlock(GetLayoutObjectByElementId("absolute"));
  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  EXPECT_EQ(absolute->View(), &absolute->ContainerForPaintInvalidation());
  EXPECT_EQ(container, absolute->Container());

  LayoutRect absolute_visual_rect = absolute->LocalVisualRect();
  EXPECT_EQ(LayoutRect(0, 0, 50, 50), absolute_visual_rect);
  LayoutRect rect = absolute_visual_rect;
  EXPECT_TRUE(absolute->MapToVisualRectInAncestorSpace(stacking_context, rect));
  // -172 = top(50) - y_offset_of_stacking_context(222)
  EXPECT_EQ(LayoutRect(50, -172, 50, 50), rect);
  // Call checkPaintInvalidationVisualRect to deal with layer squashing.
  CheckPaintInvalidationVisualRect(*absolute, GetLayoutView(),
                                   LayoutRect(149, 138, 50, 50));
}

TEST_P(VisualRectMappingTest, CSSClip) {
  SetBodyInnerHTML(R"HTML(
    <div id='container' style='position: absolute; top: 0px; left: 0px;
        clip: rect(0px, 200px, 200px, 0px)'>
      <div id='target' style='width: 400px; height: 400px'></div>
    </div>
  )HTML");

  LayoutBox* target = ToLayoutBox(GetLayoutObjectByElementId("target"));

  EXPECT_EQ(LayoutRect(0, 0, 400, 400), target->LocalVisualRect());
  CheckPaintInvalidationVisualRect(*target, GetLayoutView(),
                                   LayoutRect(0, 0, 200, 200));
}

TEST_P(VisualRectMappingTest, ContainPaint) {
  SetBodyInnerHTML(R"HTML(
    <div id='container' style='position: absolute; top: 0px; left: 0px;
        width: 200px; height: 200px; contain: paint'>
      <div id='target' style='width: 400px; height: 400px'></div>
    </div>
  )HTML");

  LayoutBox* target = ToLayoutBox(GetLayoutObjectByElementId("target"));

  EXPECT_EQ(LayoutRect(0, 0, 400, 400), target->LocalVisualRect());
  CheckPaintInvalidationVisualRect(*target, GetLayoutView(),
                                   LayoutRect(0, 0, 200, 200));
}

TEST_P(VisualRectMappingTest, FloatUnderInline) {
  SetBodyInnerHTML(R"HTML(
    <div style='position: absolute; top: 55px; left: 66px'>
      <span id='span' style='position: relative; top: 100px; left: 200px'>
        <div id='target' style='float: left; width: 33px; height: 44px'>
        </div>
      </span>
    </div>
  )HTML");

  LayoutBoxModelObject* span =
      ToLayoutBoxModelObject(GetLayoutObjectByElementId("span"));
  LayoutBox* target = ToLayoutBox(GetLayoutObjectByElementId("target"));

  LayoutRect target_visual_rect = target->LocalVisualRect();
  EXPECT_EQ(LayoutRect(0, 0, 33, 44), target_visual_rect);

  LayoutRect rect = target_visual_rect;
  EXPECT_TRUE(target->MapToVisualRectInAncestorSpace(&GetLayoutView(), rect));
  if (RuntimeEnabledFeatures::LayoutNGEnabled()) {
    // LayoutNG inline-level floats are children of their inline-level
    // containers. As such they are positioned relative to their inline-level
    // container, (and shifted by an additional 200,100 in this case).
    EXPECT_EQ(LayoutRect(266, 155, 33, 44), rect);
  } else {
    EXPECT_EQ(LayoutRect(66, 55, 33, 44), rect);
  }
  EXPECT_EQ(rect, target->FirstFragment().VisualRect());

  rect = target_visual_rect;

  if (RuntimeEnabledFeatures::LayoutNGEnabled()) {
    CheckVisualRect(*target, *span, rect, LayoutRect(0, 0, 33, 44));
  } else {
    CheckVisualRect(*target, *span, rect, LayoutRect(-200, -100, 33, 44));
  }
}

TEST_P(VisualRectMappingTest, ShouldAccountForPreserve3d) {
  EnableCompositing();
  SetBodyInnerHTML(R"HTML(
    <style>
    * { margin: 0; }
    #container {
      transform: rotateX(-45deg);
      width: 100px; height: 100px;
    }
    #target {
      transform-style: preserve-3d; transform: rotateX(45deg);
      background: lightblue;
      width: 100px; height: 100px;
    }
    </style>
    <div id='container'><div id='target'></div></div>
  )HTML");
  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect original_rect(0, 0, 100, 100);
  // Multiply both matrices together before flattening.
  TransformationMatrix matrix = container->Layer()->CurrentTransform();
  matrix.FlattenTo2d();
  matrix *= target->Layer()->CurrentTransform();
  LayoutRect output(matrix.MapRect(FloatRect(original_rect)));

  CheckVisualRect(*target, *target->View(), original_rect, output,
                  kContainsEnclosingIntRect);
}

TEST_P(VisualRectMappingTest, ShouldAccountForPreserve3dNested) {
  EnableCompositing();
  SetBodyInnerHTML(R"HTML(
    <style>
    * { margin: 0; }
    #container {
      transform-style: preserve-3d;
      transform: rotateX(-45deg);
      width: 100px; height: 100px;
    }
    #target {
      transform-style: preserve-3d; transform: rotateX(45deg);
      background: lightblue;
      width: 100px; height: 100px;
    }
    </style>
    <div id='container'><div id='target'></div></div>
  )HTML");
  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect original_rect(0, 0, 100, 100);
  // Multiply both matrices together before flattening.
  TransformationMatrix matrix = container->Layer()->CurrentTransform();
  matrix *= target->Layer()->CurrentTransform();
  LayoutRect output(matrix.MapRect(FloatRect(original_rect)));

  CheckVisualRect(*target, *target->View(), original_rect, output);
}

TEST_P(VisualRectMappingTest, ShouldAccountForPerspective) {
  EnableCompositing();
  SetBodyInnerHTML(R"HTML(
    <style>
    * { margin: 0; }
    #container {
      transform: rotateX(-45deg); perspective: 100px;
      width: 100px; height: 100px;
    }
    #target {
      transform-style: preserve-3d; transform: rotateX(45deg);
      background: lightblue;
      width: 100px; height: 100px;
    }
    </style>
    <div id='container'><div id='target'></div></div>
  )HTML");
  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect original_rect(0, 0, 100, 100);
  TransformationMatrix matrix = container->Layer()->CurrentTransform();
  matrix.FlattenTo2d();
  TransformationMatrix target_matrix;
  // getTransformfromContainter includes transform and perspective matrix
  // of the container.
  target->GetTransformFromContainer(container, LayoutSize(), target_matrix);
  matrix *= target_matrix;
  LayoutRect output(matrix.MapRect(FloatRect(original_rect)));

  CheckVisualRect(*target, *target->View(), original_rect, output,
                  kContainsEnclosingIntRect);
}

TEST_P(VisualRectMappingTest, ShouldAccountForPerspectiveNested) {
  EnableCompositing();
  SetBodyInnerHTML(R"HTML(
    <style>
    * { margin: 0; }
    #container {
      transform-style: preserve-3d;
      transform: rotateX(-45deg); perspective: 100px;
      width: 100px; height: 100px;
    }
    #target {
      transform-style: preserve-3d; transform: rotateX(45deg);
      background: lightblue;
      width: 100px; height: 100px;
    }
    </style>
    <div id='container'><div id='target'></div></div>
  )HTML");
  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect original_rect(0, 0, 100, 100);
  TransformationMatrix matrix = container->Layer()->CurrentTransform();
  TransformationMatrix target_matrix;
  // getTransformfromContainter includes transform and perspective matrix
  // of the container.
  target->GetTransformFromContainer(container, LayoutSize(), target_matrix);
  matrix *= target_matrix;
  LayoutRect output(matrix.MapRect(FloatRect(original_rect)));

  CheckVisualRect(*target, *target->View(), original_rect, output);
}

TEST_P(VisualRectMappingTest, PerspectivePlusScroll) {
  EnableCompositing();
  SetBodyInnerHTML(R"HTML(
    <style>
    * { margin: 0; }
    #container {
      perspective: 100px;
      width: 100px; height: 100px;
      overflow: scroll;
    }
    #target {
      transform: rotatex(45eg);
      background: lightblue;
      width: 100px; height: 100px;
    }
    #spacer {
      width: 10px; height:2000px;
    }
    </style>
    <div id='container'>
      <div id='target'></div>
      <div id='spacer'></div>
    </div>
  )HTML");
  LayoutBlock* container =
      ToLayoutBlock(GetLayoutObjectByElementId("container"));
  ToElement(container->GetNode())->scrollTo(0, 5);
  UpdateAllLifecyclePhasesForTest();

  LayoutBlock* target = ToLayoutBlock(GetLayoutObjectByElementId("target"));
  LayoutRect originalRect(0, 0, 100, 100);
  TransformationMatrix transform;
  target->GetTransformFromContainer(
      container, target->OffsetFromContainer(container), transform);
  transform.FlattenTo2d();

  LayoutRect output(transform.MapRect(FloatRect(originalRect)));
  output.Intersect(container->ClippingRect(LayoutPoint()));
  CheckVisualRect(*target, *target->View(), originalRect, output);
}

TEST_P(VisualRectMappingTest, FixedContentsInIframe) {
  GetDocument().SetBaseURLOverride(KURL("http://test.com"));
  GetDocument().GetFrame()->GetSettings()->SetPreferCompositingToLCDTextEnabled(
      true);
  SetBodyInnerHTML(R"HTML(
    <style> * { margin:0; } </style>
    <iframe src='http://test.com' width='500' height='500' frameBorder='0'>
    </iframe>
  )HTML");
  SetChildFrameHTML(R"HTML(
    <style>body { margin:0; } ::-webkit-scrollbar { display:none; }</style>
    <div id='forcescroll' style='height:6000px;'></div>
    <div id='fixed' style='
        position:fixed; top:0; left:0; width:400px; height:300px;'>
    </div>
  )HTML");

  UpdateAllLifecyclePhasesForTest();
  auto* fixed = ChildDocument().getElementById("fixed")->GetLayoutObject();
  auto* root_view = fixed->View();
  while (root_view->GetFrame()->OwnerLayoutObject())
    root_view = root_view->GetFrame()->OwnerLayoutObject()->View();

  CheckMapToVisualRectInAncestorSpace(LayoutRect(0, 0, 400, 300),
                                      LayoutRect(0, 0, 400, 300), fixed,
                                      root_view, kDefaultVisualRectFlags, true);

  ChildDocument().View()->LayoutViewport()->SetScrollOffset(
      ScrollOffset(0, 50), kProgrammaticScroll);
  UpdateAllLifecyclePhasesForTest();

  // The fixed element should not scroll so the mapped visual rect should not
  // have changed.
  CheckMapToVisualRectInAncestorSpace(LayoutRect(0, 0, 400, 300),
                                      LayoutRect(0, 0, 400, 300), fixed,
                                      root_view, kDefaultVisualRectFlags, true);
}

TEST_P(VisualRectMappingTest, FixedContentsWithScrollOffset) {
  GetDocument().SetBaseURLOverride(KURL("http://test.com"));
  GetDocument().GetFrame()->GetSettings()->SetPreferCompositingToLCDTextEnabled(
      true);
  SetBodyInnerHTML(R"HTML(
    <style>body { margin:0; } ::-webkit-scrollbar { display:none; }</style>
    <div id='space' style='height:10px;'></div>
    <div id='ancestor'>
      <div id='fixed' style='
          position:fixed; top:0; left:0; width:400px; height:300px;'>
      </div>
    </div>
    <div id='forcescroll' style='height:1000px;'></div>
  )HTML");

  auto* ancestor =
      ToLayoutBox(GetDocument().getElementById("ancestor")->GetLayoutObject());
  auto* fixed = GetDocument().getElementById("fixed")->GetLayoutObject();

  CheckMapToVisualRectInAncestorSpace(LayoutRect(0, 0, 400, 300),
                                      LayoutRect(0, -10, 400, 300), fixed,
                                      ancestor, kDefaultVisualRectFlags, true);

  GetDocument().View()->LayoutViewport()->SetScrollOffset(ScrollOffset(0, 50),
                                                          kProgrammaticScroll);
  UpdateAllLifecyclePhasesForTest();

  // The fixed element does not scroll but the ancestor does which changes the
  // visual rect.
  CheckMapToVisualRectInAncestorSpace(LayoutRect(0, 0, 400, 300),
                                      LayoutRect(0, 40, 400, 300), fixed,
                                      ancestor, kDefaultVisualRectFlags, true);
}

TEST_P(VisualRectMappingTest, FixedContentsUnderViewWithScrollOffset) {
  GetDocument().GetFrame()->GetSettings()->SetPreferCompositingToLCDTextEnabled(
      true);
  SetBodyInnerHTML(R"HTML(
    <style>body { margin:0; } ::-webkit-scrollbar { display:none; }</style>
    <div id='fixed' style='
        position:fixed; top:0; left:0; width:400px; height:300px;'>
    </div>
    <div id='forcescroll' style='height:1000px;'></div>
  )HTML");

  auto* fixed = GetDocument().getElementById("fixed")->GetLayoutObject();

  CheckMapToVisualRectInAncestorSpace(
      LayoutRect(0, 0, 400, 300), LayoutRect(0, 0, 400, 300), fixed,
      fixed->View(), kDefaultVisualRectFlags, true);

  GetDocument().View()->LayoutViewport()->SetScrollOffset(ScrollOffset(0, 50),
                                                          kProgrammaticScroll);
  UpdateAllLifecyclePhasesForTest();

  // Results of mapping to ancestor are in absolute coordinates of the
  // ancestor. Therefore a fixed-position element is (reverse) offset by scroll.
  CheckMapToVisualRectInAncestorSpace(
      LayoutRect(0, 0, 400, 300), LayoutRect(0, 50, 400, 300), fixed,
      fixed->View(), kDefaultVisualRectFlags, true);
}

TEST_P(VisualRectMappingTest, InclusiveIntersect) {
  GetDocument().SetBaseURLOverride(KURL("http://test.com"));
  SetBodyInnerHTML(R"HTML(
    <style>body { margin:0; }</style>
    <div id='ancestor' style='position: relative'>
      <div style='width: 50px; height: 50px; overflow: hidden'>
        <div id='child' style='width: 10px; height: 10px; position: relative; left: 50px'></div>
      </div>
    </div>
  )HTML");

  auto* ancestor =
      ToLayoutBox(GetDocument().getElementById("ancestor")->GetLayoutObject());
  auto* child =
      ToLayoutBox(GetDocument().getElementById("child")->GetLayoutObject());

  CheckMapToVisualRectInAncestorSpace(LayoutRect(0, 0, 10, 10),
                                      LayoutRect(50, 0, 0, 10), child, ancestor,
                                      kEdgeInclusive, true);

  CheckMapToVisualRectInAncestorSpace(LayoutRect(1, 1, 10, 10), LayoutRect(),
                                      child, ancestor, kEdgeInclusive, false);

  CheckMapToVisualRectInAncestorSpace(LayoutRect(1, 1, 10, 10),
                                      LayoutRect(1, 1, 10, 10), child, child,
                                      kEdgeInclusive, true);

  CheckMapToVisualRectInAncestorSpace(LayoutRect(0, 0, 10, 10), LayoutRect(),
                                      child, ancestor, kDefaultVisualRectFlags,
                                      false);
}

}  // namespace blink
