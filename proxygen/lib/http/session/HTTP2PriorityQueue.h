/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <proxygen/lib/http/codec/HTTPCodec.h>
#include <proxygen/lib/http/codec/experimental/HTTP2Framer.h>
#include <folly/ThreadLocal.h>
#include <folly/IntrusiveList.h>

#include <list>
#include <deque>

namespace proxygen {

class HTTPTransaction;

class HTTP2PriorityQueue : public HTTPCodec::PriorityQueue {

 private:
  class Node;

 public:

  typedef Node* Handle;

 public:
  HTTP2PriorityQueue() {}

  // Notify the queue when a transaction has egress
  void signalPendingEgress(Handle h);

  // Notify the queue when a transaction no longer has egress
  void clearPendingEgress(Handle h);

  void addPriorityNode(HTTPCodec::StreamID id,
                       HTTPCodec::StreamID parent) override{
    addTransaction(id, {parent, false, 0}, nullptr);
  }

  // adds new transaction (possibly nullptr) to the priority tree
  Handle addTransaction(HTTPCodec::StreamID id, http2::PriorityUpdate pri,
                        HTTPTransaction *txn, uint64_t* depth = nullptr);

  // update the priority of an existing node
  Handle updatePriority(Handle handle, http2::PriorityUpdate pri);

  // Remove the transaction from the priority tree
  void removeTransaction(Handle handle);

  // Returns true if there are no transaction with pending egress
  bool empty() const {
    return activeCount_ == 0;
  }

  // The number with pending egress
  uint64_t numPendingEgress() const {
    return activeCount_;
  }

  void iterate(const std::function<bool(HTTPCodec::StreamID,
                                        HTTPTransaction *, double)>& fn,
               const std::function<bool()>& stopFn, bool all) {
    updateEnqueuedWeight();
    root_.iterate(fn, stopFn, all);
  }

  // stopFn is only evaluated once per level
  void iterateBFS(const std::function<bool(HTTPCodec::StreamID,
                                           HTTPTransaction *, double)>& fn,
                  const std::function<bool()>& stopFn, bool all);

  typedef std::vector<std::pair<HTTPTransaction*, double>> NextEgressResult;

  void nextEgress(NextEgressResult& result);

 private:
  // Find the node in priority tree
  Handle find(HTTPCodec::StreamID id, uint64_t* depth = nullptr);

  Handle findInternal(HTTPCodec::StreamID id) {
    if (id == 0) {
      return &root_;
    }
    return root_.findInTree(id, nullptr);
  }

  static bool nextEgressResult(HTTPCodec::StreamID id, HTTPTransaction* txn,
                               double r);

  void updateEnqueuedWeight();

 private:

  class Node {
   public:
    Node(Node* inParent, HTTPCodec::StreamID id,
         uint8_t weight, HTTPTransaction *txn);

    Node* getParent() const {
      return parent_;
    }

    HTTPCodec::StreamID getID() const {
      return id_;
    }

    HTTPCodec::StreamID parentID() const {
      if (parent_) {
        return parent_->id_;
      }
      return 0;
    }

    HTTPTransaction* getTransaction() const {
      return txn_;
    }

    // Add a new node as a child of this node
    Handle emplaceNode(std::unique_ptr<Node> node, bool exclusive);

    // Removes the node from the tree
    void removeFromTree();

    void signalPendingEgress();

    void clearPendingEgress();

    // Set a new weight for this node
    void updateWeight(uint8_t weight);

    Handle reparent(Node* newParent, bool exclusive);

    // Returns true if this is a descendant of node
    bool isDescendantOf(Node *node);

    // True if this Node is in the egress queue
    bool isEnqueued() const {
      return (txn_ != nullptr && enqueued_);
    }

    // True if this Node is in the egress tree even if the node itself is
    // virtual but has enqueued descendants.
    bool inEgressTree() const {
      return isEnqueued() || totalEnqueuedWeight_ > 0;
    }

    // Find the node for the given stream ID in the priority tree
    Node* findInTree(HTTPCodec::StreamID id, uint64_t* depth);

    double getRelativeWeight() const {
      if (!parent_) {
        return 1.0;
      }

      return (double)weight_ / parent_->totalChildWeight_;
    }

    double getRelativeEnqueuedWeight() const {
      if (!parent_) {
        return 1.0;
      }

      return (double)weight_ / parent_->totalEnqueuedWeight_;
    }

    /* Execute the given function on this node and all child nodes presently
     * enqueued, until one of them asks to stop, or the stop function returns
     * true.
     *
     * The all parameter visits every node, even the ones not currently
     * enqueued.
     *
     * The arguments to the function are
     *   txn - HTTPTransaction for the node
     *   ratio - weight of this txn relative to all peers (not just enequeued)
     */
    bool iterate(const std::function<bool(HTTPCodec::StreamID,
                                          HTTPTransaction *, double)>& fn,
                 const std::function<bool()>& stopFn, bool all);

    struct PendingNode {
      HTTPCodec::StreamID id;
      Node* node;
      double ratio;
      PendingNode(HTTPCodec::StreamID i, Node* n, double r) :
          id(i), node(n), ratio(r) {}
    };

    typedef std::deque<PendingNode> PendingList;
    bool visitBFS(double relativeParentWeight,
                  const std::function<bool(HTTPCodec::StreamID,
                                          HTTPTransaction *, double)>& fn,
                  bool all,
                  PendingList& pendingNodes, bool enqueuedChildren);

    void updateEnqueuedWeight(bool activeNodes);

   private:
    Handle addChild(std::unique_ptr<Node> child);

    void addChildren(std::list<std::unique_ptr<Node>>&& children);

    std::unique_ptr<Node> detachChild(Node* node);

    void addEnqueuedChild(HTTP2PriorityQueue::Node* node);

    void removeEnqueuedChild(HTTP2PriorityQueue::Node* node);

    static void propagatePendingEgressSignal(Node *node);

    static void propagatePendingEgressClear(Node* node);

   private:
    Node *parent_{nullptr};
    HTTPCodec::StreamID id_{0};
    uint16_t weight_{16};
    HTTPTransaction *txn_{nullptr};
    bool enqueued_{false};
#ifndef NDEBUG
    uint64_t totalEnqueuedWeightCheck_{0};
#endif
    uint64_t totalEnqueuedWeight_{0};
    uint64_t totalChildWeight_{0};
    std::list<std::unique_ptr<Node>> children_;
    std::list<std::unique_ptr<Node>>::iterator self_;
    folly::IntrusiveListHook enqueuedHook_;
    folly::IntrusiveList<Node, &Node::enqueuedHook_> enqueuedChildren_;
  };

  Node root_{nullptr, 0, 1, nullptr};
  uint64_t activeCount_{0};
  bool pendingWeightChange_{false};
  static folly::ThreadLocalPtr<NextEgressResult> nextEgressResults_;
};


}
