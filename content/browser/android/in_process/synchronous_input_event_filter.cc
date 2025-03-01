// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/android/in_process/synchronous_input_event_filter.h"

#include "base/callback.h"
#include "content/browser/android/in_process/synchronous_compositor_impl.h"
#include "content/browser/android/in_process/synchronous_compositor_registry_in_proc.h"
#include "content/public/browser/browser_thread.h"
#include "ui/events/blink/synchronous_input_handler_proxy.h"
#include "ui/events/latency_info.h"

using blink::WebInputEvent;

namespace content {

SynchronousInputEventFilter::SynchronousInputEventFilter() {
}

SynchronousInputEventFilter::~SynchronousInputEventFilter() {
}

InputEventAckState SynchronousInputEventFilter::HandleInputEvent(
    int routing_id,
    const blink::WebInputEvent& input_event) {
  // The handler will be empty both before renderer initialization and after
  // renderer destruction. It's possible that this will be reached in such a
  // state. While not good, it should also not be fatal.
  if (handler_.is_null())
    return INPUT_EVENT_ACK_STATE_NO_CONSUMER_EXISTS;
  ui::LatencyInfo latency;
  return handler_.Run(routing_id, &input_event, &latency);
}

void SynchronousInputEventFilter::SetBoundHandler(const Handler& handler) {
  BrowserThread::PostTask(
      BrowserThread::UI, FROM_HERE,
      base::Bind(&SynchronousInputEventFilter::SetBoundHandlerOnUIThread,
                 base::Unretained(this), handler));
}

void SynchronousInputEventFilter::SetBoundHandlerOnUIThread(
    const Handler& handler) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  handler_ = handler;
}

void SynchronousInputEventFilter::DidAddInputHandler(
    int routing_id,
    ui::SynchronousInputHandlerProxy* synchronous_input_handler_proxy) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  SynchronousCompositorRegistryInProc::GetInstance()->RegisterInputHandler(
      routing_id, synchronous_input_handler_proxy);
}

void SynchronousInputEventFilter::DidRemoveInputHandler(int routing_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  SynchronousCompositorRegistryInProc::GetInstance()->UnregisterInputHandler(
      routing_id);
}

void SynchronousInputEventFilter::DidOverscroll(
    int routing_id,
    const DidOverscrollParams& params) {
  // The SynchronusCompositorImpl can be NULL if the WebContents that it's
  // bound to has already been deleted.
  SynchronousCompositorImpl* compositor =
      SynchronousCompositorImpl::FromRoutingID(routing_id);
  if (compositor)
    compositor->DidOverscroll(params);
}

void SynchronousInputEventFilter::DidStopFlinging(int routing_id) {
  // The SynchronusCompositorImpl can be NULL if the WebContents that it's
  // bound to has already been deleted.
  SynchronousCompositorImpl* compositor =
      SynchronousCompositorImpl::FromRoutingID(routing_id);
  if (compositor)
    compositor->DidStopFlinging();
}

void SynchronousInputEventFilter::NonBlockingInputEventHandled(
    int routing_id,
    blink::WebInputEvent::Type type) {}

}  // namespace content
