// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/performance_manager/graph/node_base.h"

#include <utility>

#include "chrome/browser/performance_manager/graph/graph.h"
#include "chrome/browser/performance_manager/observers/coordination_unit_graph_observer.h"
#include "services/resource_coordinator/public/cpp/coordination_unit_id.h"

namespace performance_manager {

NodeBase::NodeBase(resource_coordinator::CoordinationUnitType node_type,
                   Graph* graph)
    : graph_(graph),
      id_(node_type, resource_coordinator::CoordinationUnitID::RANDOM_ID) {}

NodeBase::~NodeBase() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void NodeBase::JoinGraph() {}

void NodeBase::LeaveGraph() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (auto& observer : observers_)
    observer.OnBeforeNodeRemoved(this);
}

void NodeBase::AddObserver(GraphObserver* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.AddObserver(observer);
}

void NodeBase::RemoveObserver(GraphObserver* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.RemoveObserver(observer);
}

bool NodeBase::NodeInGraph(const NodeBase* other_node) const {
  return graph_->GetNodeByID(other_node->id()) == other_node;
}

void NodeBase::OnEventReceived(resource_coordinator::mojom::Event event) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (auto& observer : observers())
    observer.OnEventReceived(this, event);
}

void NodeBase::SendEvent(resource_coordinator::mojom::Event event) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  OnEventReceived(event);
}

}  // namespace performance_manager
