// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/ng/ng_physical_box_fragment.h"

#include "third_party/blink/renderer/core/dom/document_lifecycle.h"
#include "third_party/blink/renderer/core/layout/layout_block.h"
#include "third_party/blink/renderer/core/layout/ng/geometry/ng_border_edges.h"
#include "third_party/blink/renderer/core/layout/ng/geometry/ng_box_strut.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_inline_node.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_physical_line_box_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_physical_text_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/ng_block_node.h"
#include "third_party/blink/renderer/core/layout/ng/ng_break_token.h"
#include "third_party/blink/renderer/core/layout/ng/ng_fragment_builder.h"
#include "third_party/blink/renderer/core/layout/ng/ng_physical_box_fragment.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/platform/fonts/shaping/shape_result_view.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"

namespace blink {
namespace {

struct SameSizeAsNGPhysicalFragment
    : RefCounted<const NGPhysicalFragment, NGPhysicalFragmentTraits> {
  void* pointers[2];
  NGPhysicalSize size;
  unsigned flags;
};

static_assert(sizeof(NGPhysicalFragment) ==
                  sizeof(SameSizeAsNGPhysicalFragment),
              "NGPhysicalFragment should stay small");

bool AppendFragmentOffsetAndSize(
    const NGPhysicalFragment* fragment,
    base::Optional<NGPhysicalOffset> fragment_offset,
    StringBuilder* builder,
    NGPhysicalFragment::DumpFlags flags,
    bool has_content) {
  if (flags & NGPhysicalFragment::DumpOffset) {
    if (has_content)
      builder->Append(" ");
    builder->Append("offset:");
    if (fragment_offset)
      builder->Append(fragment_offset->ToString());
    else
      builder->Append("unplaced");
    has_content = true;
  }
  if (flags & NGPhysicalFragment::DumpSize) {
    if (has_content)
      builder->Append(" ");
    builder->Append("size:");
    builder->Append(fragment->Size().ToString());
    has_content = true;
  }
  return has_content;
}

String StringForBoxType(const NGPhysicalFragment& fragment) {
  StringBuilder result;
  switch (fragment.BoxType()) {
    case NGPhysicalFragment::NGBoxType::kNormalBox:
      break;
    case NGPhysicalFragment::NGBoxType::kInlineBox:
      result.Append("inline");
      break;
    case NGPhysicalFragment::NGBoxType::kColumnBox:
      result.Append("column");
      break;
    case NGPhysicalFragment::NGBoxType::kAtomicInline:
      result.Append("atomic-inline");
      break;
    case NGPhysicalFragment::NGBoxType::kFloating:
      result.Append("floating");
      break;
    case NGPhysicalFragment::NGBoxType::kOutOfFlowPositioned:
      result.Append("out-of-flow-positioned");
      break;
    case NGPhysicalFragment::NGBoxType::kBlockFlowRoot:
      result.Append("block-flow-root");
      break;
  }
  if (fragment.IsOldLayoutRoot()) {
    if (result.length())
      result.Append(" ");
    result.Append("old-layout-root");
  }
  if (fragment.IsBlockFlow()) {
    if (result.length())
      result.Append(" ");
    result.Append("block-flow");
  }
  if (fragment.IsRenderedLegend()) {
    if (result.length())
      result.Append(" ");
    result.Append("rendered-legend");
  }
  if (fragment.IsFieldsetContainer()) {
    if (result.length())
      result.Append(" ");
    result.Append("fieldset-container");
  }
  if (fragment.IsBox() &&
      static_cast<const NGPhysicalBoxFragment&>(fragment).ChildrenInline()) {
    if (result.length())
      result.Append(" ");
    result.Append("children-inline");
  }

  return result.ToString();
}

void AppendFragmentToString(const NGPhysicalFragment* fragment,
                            base::Optional<NGPhysicalOffset> fragment_offset,
                            StringBuilder* builder,
                            NGPhysicalFragment::DumpFlags flags,
                            unsigned indent = 2) {
  if (flags & NGPhysicalFragment::DumpIndentation) {
    for (unsigned i = 0; i < indent; i++)
      builder->Append(" ");
  }

  bool has_content = false;
  if (const auto* box = DynamicTo<NGPhysicalBoxFragment>(fragment)) {
    if (flags & NGPhysicalFragment::DumpType) {
      if (fragment->IsRenderedLegend())
        builder->Append("RenderedLegend");
      else
        builder->Append("Box");
      String box_type = StringForBoxType(*fragment);
      has_content = true;
      if (!box_type.IsEmpty()) {
        builder->Append(" (");
        builder->Append(box_type);
        builder->Append(")");
      }
      if (flags & NGPhysicalFragment::DumpSelfPainting &&
          box->HasSelfPaintingLayer()) {
        if (box_type.IsEmpty())
          builder->Append(" ");
        builder->Append("(self paint)");
      }
    }
    has_content = AppendFragmentOffsetAndSize(fragment, fragment_offset,
                                              builder, flags, has_content);

    if (flags & NGPhysicalFragment::DumpNodeName &&
        fragment->GetLayoutObject()) {
      if (has_content)
        builder->Append(" ");
      builder->Append(fragment->GetLayoutObject()->DebugName());
    }
    builder->Append("\n");

    if (flags & NGPhysicalFragment::DumpSubtree) {
      for (auto& child : box->Children()) {
        AppendFragmentToString(child.get(), child.Offset(), builder, flags,
                               indent + 2);
      }
    }
    return;
  }

  if (const auto* line_box = DynamicTo<NGPhysicalLineBoxFragment>(fragment)) {
    if (flags & NGPhysicalFragment::DumpType) {
      builder->Append("LineBox");
      has_content = true;
    }
    has_content = AppendFragmentOffsetAndSize(fragment, fragment_offset,
                                              builder, flags, has_content);
    builder->Append("\n");

    if (flags & NGPhysicalFragment::DumpSubtree) {
      for (auto& child : line_box->Children()) {
        AppendFragmentToString(child.get(), child.Offset(), builder, flags,
                               indent + 2);
      }
      return;
    }
  }

  if (const auto* text = DynamicTo<NGPhysicalTextFragment>(fragment)) {
    if (flags & NGPhysicalFragment::DumpType) {
      builder->Append("Text");
      has_content = true;
    }
    has_content = AppendFragmentOffsetAndSize(fragment, fragment_offset,
                                              builder, flags, has_content);

    if (flags & NGPhysicalFragment::DumpTextOffsets) {
      if (has_content)
        builder->Append(" ");
      builder->Append("start: ");
      builder->Append(String::Format("%u", text->StartOffset()));
      builder->Append(" end: ");
      builder->Append(String::Format("%u", text->EndOffset()));
      has_content = true;
    }
    builder->Append("\n");
    return;
  }

  if (flags & NGPhysicalFragment::DumpType) {
    builder->Append("Unknown fragment type");
    has_content = true;
  }
  has_content = AppendFragmentOffsetAndSize(fragment, fragment_offset, builder,
                                            flags, has_content);
  builder->Append("\n");
}

LayoutUnit BorderWidth(unsigned edges, unsigned edge, float border_width) {
  return (edges & edge) ? LayoutUnit(border_width) : LayoutUnit();
}

}  // namespace

// static
void NGPhysicalFragmentTraits::Destruct(const NGPhysicalFragment* fragment) {
  fragment->Destroy();
}

NGPhysicalFragment::NGPhysicalFragment(NGFragmentBuilder* builder,
                                       NGFragmentType type,
                                       unsigned sub_type)
    : layout_object_(builder->layout_object_),
      size_(ToNGPhysicalSize(builder->size_, builder->GetWritingMode())),
      break_token_(std::move(builder->break_token_)),
      type_(type),
      sub_type_(sub_type),
      style_variant_((unsigned)builder->style_variant_),
      has_floating_descendants_(false),
      is_fieldset_container_(false),
      is_old_layout_root_(false) {}

NGPhysicalFragment::NGPhysicalFragment(LayoutObject* layout_object,
                                       NGStyleVariant style_variant,
                                       NGPhysicalSize size,
                                       NGFragmentType type,
                                       unsigned sub_type,
                                       scoped_refptr<NGBreakToken> break_token)
    : layout_object_(layout_object),
      size_(size),
      break_token_(std::move(break_token)),
      type_(type),
      sub_type_(sub_type),
      style_variant_((unsigned)style_variant),
      has_floating_descendants_(false),
      is_fieldset_container_(false),
      is_old_layout_root_(false) {}

// Keep the implementation of the destructor here, to avoid dependencies on
// ComputedStyle in the header file.
NGPhysicalFragment::~NGPhysicalFragment() = default;

void NGPhysicalFragment::Destroy() const {
  switch (Type()) {
    case kFragmentBox:
    case kFragmentRenderedLegend:
      delete static_cast<const NGPhysicalBoxFragment*>(this);
      break;
    case kFragmentText:
      delete static_cast<const NGPhysicalTextFragment*>(this);
      break;
    case kFragmentLineBox:
      delete static_cast<const NGPhysicalLineBoxFragment*>(this);
      break;
    default:
      NOTREACHED();
      break;
  }
}

const ComputedStyle& NGPhysicalFragment::Style() const {
  if (auto* line_box = DynamicTo<NGPhysicalLineBoxFragment>(this))
    return line_box->Style();
  switch (StyleVariant()) {
    case NGStyleVariant::kStandard:
      DCHECK(GetLayoutObject());
      return *GetLayoutObject()->Style();
    case NGStyleVariant::kFirstLine:
      DCHECK(GetLayoutObject());
      return *GetLayoutObject()->FirstLineStyle();
    case NGStyleVariant::kEllipsis:
      return To<NGPhysicalTextFragment>(this)->Style();
  }
  NOTREACHED();
  return *GetLayoutObject()->Style();
}

Node* NGPhysicalFragment::GetNode() const {
  return layout_object_ ? layout_object_->GetNode() : nullptr;
}

bool NGPhysicalFragment::HasLayer() const {
  return layout_object_ && layout_object_->HasLayer();
}

PaintLayer* NGPhysicalFragment::Layer() const {
  if (!HasLayer())
    return nullptr;

  // If the underlying LayoutObject has a layer it's guaranteed to be a
  // LayoutBoxModelObject.
  return static_cast<LayoutBoxModelObject*>(layout_object_)->Layer();
}

bool NGPhysicalFragment::HasSelfPaintingLayer() const {
  if (!HasLayer())
    return false;

  // If the underlying LayoutObject has a layer it's guaranteed to be a
  // LayoutBoxModelObject.
  return static_cast<LayoutBoxModelObject*>(layout_object_)
      ->HasSelfPaintingLayer();
}

bool NGPhysicalFragment::HasOverflowClip() const {
  return layout_object_ && layout_object_->HasOverflowClip();
}

bool NGPhysicalFragment::ShouldClipOverflow() const {
  return layout_object_ && layout_object_->ShouldClipOverflow();
}

bool NGPhysicalFragment::IsBlockFlow() const {
  return layout_object_ && layout_object_->IsLayoutBlockFlow();
}

bool NGPhysicalFragment::IsListMarker() const {
  return layout_object_ && layout_object_->IsLayoutNGListMarker();
}

bool NGPhysicalFragment::IsPlacedByLayoutNG() const {
  // TODO(kojii): Move this to a flag for |LayoutNGBlockFlow::UpdateBlockLayout|
  // to set.
  if (!layout_object_)
    return false;
  const LayoutBlock* container = layout_object_->ContainingBlock();
  if (!container)
    return false;
  return container->IsLayoutNGMixin() || container->IsLayoutNGFlexibleBox();
}

NGPixelSnappedPhysicalBoxStrut NGPhysicalFragment::BorderWidths() const {
  unsigned edges = BorderEdges();
  NGPhysicalBoxStrut box_strut(
      BorderWidth(edges, NGBorderEdges::kTop, Style().BorderTopWidth()),
      BorderWidth(edges, NGBorderEdges::kRight, Style().BorderRightWidth()),
      BorderWidth(edges, NGBorderEdges::kBottom, Style().BorderBottomWidth()),
      BorderWidth(edges, NGBorderEdges::kLeft, Style().BorderLeftWidth()));
  return box_strut.SnapToDevicePixels();
}

#if DCHECK_IS_ON()
void NGPhysicalFragment::CheckCanUpdateInkOverflow() const {
  if (!GetLayoutObject())
    return;
  const DocumentLifecycle& lifecycle =
      GetLayoutObject()->GetDocument().Lifecycle();
  DCHECK(lifecycle.GetState() >= DocumentLifecycle::kLayoutClean &&
         lifecycle.GetState() < DocumentLifecycle::kCompositingClean)
      << lifecycle.GetState();
}
#endif

NGPhysicalOffsetRect NGPhysicalFragment::ScrollableOverflow() const {
  switch (Type()) {
    case kFragmentBox:
    case kFragmentRenderedLegend:
      return To<NGPhysicalBoxFragment>(*this).ScrollableOverflow();
    case kFragmentText:
      return {{}, Size()};
    case kFragmentLineBox:
      NOTREACHED()
          << "You must call NGLineBoxFragment::ScrollableOverflow explicitly.";
      break;
  }
  NOTREACHED();
  return {{}, Size()};
}

NGPhysicalOffsetRect NGPhysicalFragment::ScrollableOverflowForPropagation(
    const LayoutObject* container) const {
  DCHECK(container);
  NGPhysicalOffsetRect overflow = ScrollableOverflow();
  if (GetLayoutObject() &&
      GetLayoutObject()->ShouldUseTransformFromContainer(container)) {
    TransformationMatrix transform;
    GetLayoutObject()->GetTransformFromContainer(container, LayoutSize(),
                                                 transform);
    overflow = NGPhysicalOffsetRect(transform.MapRect(overflow.ToLayoutRect()));
  }
  return overflow;
}

const Vector<NGInlineItem>& NGPhysicalFragment::InlineItemsOfContainingBlock()
    const {
  DCHECK(IsInline());
  DCHECK(GetLayoutObject());
  LayoutBlockFlow* block_flow = GetLayoutObject()->ContainingNGBlockFlow();
  // TODO(xiaochengh): Code below is copied from ng_offset_mapping.cc with
  // modification. Unify them.
  DCHECK(block_flow);
  DCHECK(block_flow->ChildrenInline());
  NGBlockNode block_node = NGBlockNode(block_flow);
  DCHECK(block_node.CanUseNewLayout());
  NGLayoutInputNode node = block_node.FirstChild();

  // TODO(xiaochengh): Handle ::first-line.
  return To<NGInlineNode>(node).ItemsData(false).items;
}

TouchAction NGPhysicalFragment::EffectiveWhitelistedTouchAction() const {
  DCHECK(GetLayoutObject());
  return GetLayoutObject()->EffectiveWhitelistedTouchAction();
}

UBiDiLevel NGPhysicalFragment::BidiLevel() const {
  switch (Type()) {
    case kFragmentText:
      return To<NGPhysicalTextFragment>(*this).BidiLevel();
    case kFragmentBox:
    case kFragmentRenderedLegend:
      return To<NGPhysicalBoxFragment>(*this).BidiLevel();
    case kFragmentLineBox:
      break;
  }
  NOTREACHED();
  return 0;
}

TextDirection NGPhysicalFragment::ResolvedDirection() const {
  switch (Type()) {
    case kFragmentText:
      return To<NGPhysicalTextFragment>(*this).ResolvedDirection();
    case kFragmentBox:
    case kFragmentRenderedLegend:
      DCHECK(IsInline() && IsAtomicInline());
      // TODO(xiaochengh): Store direction in |base_direction_| flag.
      return DirectionFromLevel(BidiLevel());
    case kFragmentLineBox:
      break;
  }
  NOTREACHED();
  return TextDirection::kLtr;
}

String NGPhysicalFragment::ToString() const {
  StringBuilder output;
  output.Append(String::Format("Type: '%d' Size: '%s'", Type(),
                               Size().ToString().Ascii().data()));
  switch (Type()) {
    case kFragmentBox:
    case kFragmentRenderedLegend:
      output.Append(String::Format(", BoxType: '%s'",
                                   StringForBoxType(*this).Ascii().data()));
      break;
    case kFragmentText: {
      const auto& text = To<NGPhysicalTextFragment>(*this);
      output.Append(String::Format(", TextType: %u, Text: (%u,%u) \"",
                                   text.TextType(), text.StartOffset(),
                                   text.EndOffset()));
      output.Append(text.Text());
      output.Append("\"");
      break;
    }
    case kFragmentLineBox:
      break;
  }
  return output.ToString();
}

String NGPhysicalFragment::DumpFragmentTree(
    DumpFlags flags,
    base::Optional<NGPhysicalOffset> fragment_offset,
    unsigned indent) const {
  StringBuilder string_builder;
  if (flags & DumpHeaderText)
    string_builder.Append(".:: LayoutNG Physical Fragment Tree ::.\n");
  AppendFragmentToString(this, fragment_offset, &string_builder, flags, indent);
  return string_builder.ToString();
}

#ifndef NDEBUG
void NGPhysicalFragment::ShowFragmentTree() const {
  DumpFlags dump_flags = DumpAll;
  LOG(INFO) << "\n" << DumpFragmentTree(dump_flags).Utf8().data();
}
#endif  // !NDEBUG

NGPhysicalOffsetRect NGPhysicalFragmentWithOffset::RectInContainerBox() const {
  return {offset_to_container_box, fragment->Size()};
}

}  // namespace blink
