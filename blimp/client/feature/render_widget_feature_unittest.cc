// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "blimp/client/feature/render_widget_feature.h"

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "blimp/common/create_blimp_message.h"
#include "blimp/common/proto/blimp_message.pb.h"
#include "blimp/common/proto/compositor.pb.h"
#include "blimp/common/proto/render_widget.pb.h"
#include "blimp/net/test_common.h"
#include "cc/proto/compositor_message.pb.h"
#include "net/base/net_errors.h"
#include "net/base/test_completion_callback.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::_;
using testing::InSequence;
using testing::Sequence;

namespace blimp {
namespace client {

namespace {

class MockRenderWidgetFeatureDelegate
    : public RenderWidgetFeature::RenderWidgetFeatureDelegate {
 public:
  // RenderWidgetFeatureDelegate implementation.
  void OnRenderWidgetCreated(int render_widget_id) override {
    MockableOnRenderWidgetCreated(render_widget_id);
  }
  void OnRenderWidgetInitialized(int render_widget_id) override {
    MockableOnRenderWidgetInitialized(render_widget_id);
  }
  void OnRenderWidgetDeleted(int render_widget_id) override {
    MockableOnRenderWidgetDeleted(render_widget_id);
  }
  void OnCompositorMessageReceived(
      int render_widget_id,
      scoped_ptr<cc::proto::CompositorMessage> message) override {
    MockableOnCompositorMessageReceived(render_widget_id, *message);
  }

  MOCK_METHOD1(MockableOnRenderWidgetCreated, void(int render_widget_id));
  MOCK_METHOD1(MockableOnRenderWidgetInitialized, void(int render_widget_id));
  MOCK_METHOD1(MockableOnRenderWidgetDeleted, void(int render_widget_id));
  MOCK_METHOD2(MockableOnCompositorMessageReceived,
               void(int render_widget_id,
                    const cc::proto::CompositorMessage& message));
};

MATCHER_P2(CompMsgEquals, tab_id, rw_id, "") {
  return arg.compositor().render_widget_id() == rw_id &&
         arg.target_tab_id() == tab_id;
}

void SendRenderWidgetMessage(BlimpMessageProcessor* processor,
                             int tab_id,
                             int rw_id,
                             RenderWidgetMessage::Type message_type) {
  RenderWidgetMessage* details;
  scoped_ptr<BlimpMessage> message = CreateBlimpMessage(&details, tab_id);
  details->set_type(message_type);
  details->set_render_widget_id(rw_id);
  net::TestCompletionCallback cb;
  processor->ProcessMessage(std::move(message), cb.callback());
  EXPECT_EQ(net::OK, cb.WaitForResult());
}

void SendCompositorMessage(BlimpMessageProcessor* processor,
                           int tab_id,
                           int rw_id) {
  CompositorMessage* details;
  scoped_ptr<BlimpMessage> message = CreateBlimpMessage(&details, tab_id);
  details->set_render_widget_id(rw_id);
  net::TestCompletionCallback cb;
  processor->ProcessMessage(std::move(message), cb.callback());
  EXPECT_EQ(net::OK, cb.WaitForResult());
}

}  // namespace

class RenderWidgetFeatureTest : public testing::Test {
 public:
  RenderWidgetFeatureTest()
      : out_input_processor_(nullptr), out_compositor_processor_(nullptr) {}

  void SetUp() override {
    out_input_processor_ = new MockBlimpMessageProcessor();
    out_compositor_processor_ = new MockBlimpMessageProcessor();
    feature_.set_outgoing_input_message_processor(
        make_scoped_ptr(out_input_processor_));
    feature_.set_outgoing_compositor_message_processor(
        make_scoped_ptr(out_compositor_processor_));

    feature_.SetDelegate(1, &delegate1_);
    feature_.SetDelegate(2, &delegate2_);
  }

 protected:
  // These are raw pointers to classes that are owned by the
  // RenderWidgetFeature.
  MockBlimpMessageProcessor* out_input_processor_;
  MockBlimpMessageProcessor* out_compositor_processor_;

  MockRenderWidgetFeatureDelegate delegate1_;
  MockRenderWidgetFeatureDelegate delegate2_;

  RenderWidgetFeature feature_;
};

TEST_F(RenderWidgetFeatureTest, DelegateCallsOK) {
  EXPECT_CALL(delegate1_, MockableOnRenderWidgetCreated(1));
  EXPECT_CALL(delegate1_, MockableOnCompositorMessageReceived(1, _));
  EXPECT_CALL(delegate1_, MockableOnRenderWidgetInitialized(1));
  EXPECT_CALL(delegate2_, MockableOnRenderWidgetCreated(2));
  EXPECT_CALL(delegate1_, MockableOnRenderWidgetDeleted(1));

  SendRenderWidgetMessage(&feature_, 1, 1, RenderWidgetMessage::CREATED);
  SendCompositorMessage(&feature_, 1, 1);
  SendRenderWidgetMessage(&feature_, 1, 1, RenderWidgetMessage::INITIALIZE);
  SendRenderWidgetMessage(&feature_, 2, 2, RenderWidgetMessage::CREATED);
  SendRenderWidgetMessage(&feature_, 1, 1, RenderWidgetMessage::DELETED);
}

TEST_F(RenderWidgetFeatureTest, RepliesHaveCorrectRenderWidgetId) {
  InSequence sequence;

  EXPECT_CALL(*out_compositor_processor_,
              MockableProcessMessage(CompMsgEquals(1, 2), _));
  EXPECT_CALL(*out_compositor_processor_,
              MockableProcessMessage(CompMsgEquals(1, 2), _));
  EXPECT_CALL(*out_compositor_processor_,
              MockableProcessMessage(CompMsgEquals(2, 3), _));

  SendRenderWidgetMessage(&feature_, 1, 2, RenderWidgetMessage::CREATED);
  feature_.SendCompositorMessage(1, 2, cc::proto::CompositorMessage());
  SendRenderWidgetMessage(&feature_, 1, 2, RenderWidgetMessage::INITIALIZE);
  feature_.SendCompositorMessage(1, 2, cc::proto::CompositorMessage());
  SendRenderWidgetMessage(&feature_, 2, 3, RenderWidgetMessage::CREATED);
  feature_.SendCompositorMessage(2, 3, cc::proto::CompositorMessage());
}

}  // namespace client
}  // namespace blimp
