// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/blink/web_to_cc_animation_delegate_adapter.h"
#include "third_party/WebKit/public/platform/WebCompositorAnimationDelegate.h"

namespace cc_blink {

WebToCCAnimationDelegateAdapter::WebToCCAnimationDelegateAdapter(
    blink::WebCompositorAnimationDelegate* delegate)
    : delegate_(delegate) {
}

void WebToCCAnimationDelegateAdapter::NotifyAnimationStarted(
    base::TimeTicks monotonic_time,
    cc::TargetProperty::Type target_property,
    int group) {
  delegate_->notifyAnimationStarted(
      (monotonic_time - base::TimeTicks()).InSecondsF(), group);
}

void WebToCCAnimationDelegateAdapter::NotifyAnimationFinished(
    base::TimeTicks monotonic_time,
    cc::TargetProperty::Type target_property,
    int group) {
  delegate_->notifyAnimationFinished(
      (monotonic_time - base::TimeTicks()).InSecondsF(), group);
}

void WebToCCAnimationDelegateAdapter::NotifyAnimationAborted(
    base::TimeTicks monotonic_time,
    cc::TargetProperty::Type target_property,
    int group) {
  delegate_->notifyAnimationAborted(
      (monotonic_time - base::TimeTicks()).InSecondsF(), group);
}

}  // namespace cc_blink
