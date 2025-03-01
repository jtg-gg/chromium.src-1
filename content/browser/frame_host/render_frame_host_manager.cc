// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/frame_host/render_frame_host_manager.h"

#include <stddef.h>

#include <algorithm>
#include <utility>

#include "base/command_line.h"
#include "base/debug/crash_logging.h"
#include "base/debug/dump_without_crashing.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "base/trace_event/trace_event.h"
#include "content/browser/child_process_security_policy_impl.h"
#include "content/browser/devtools/render_frame_devtools_agent_host.h"
#include "content/browser/frame_host/cross_site_transferring_request.h"
#include "content/browser/frame_host/debug_urls.h"
#include "content/browser/frame_host/frame_navigation_entry.h"
#include "content/browser/frame_host/interstitial_page_impl.h"
#include "content/browser/frame_host/navigation_controller_impl.h"
#include "content/browser/frame_host/navigation_entry_impl.h"
#include "content/browser/frame_host/navigation_handle_impl.h"
#include "content/browser/frame_host/navigation_request.h"
#include "content/browser/frame_host/navigator.h"
#include "content/browser/frame_host/render_frame_host_factory.h"
#include "content/browser/frame_host/render_frame_host_impl.h"
#include "content/browser/frame_host/render_frame_proxy_host.h"
#include "content/browser/renderer_host/render_process_host_impl.h"
#include "content/browser/renderer_host/render_view_host_factory.h"
#include "content/browser/renderer_host/render_view_host_impl.h"
#include "content/browser/site_instance_impl.h"
#include "content/browser/webui/web_ui_controller_factory_registry.h"
#include "content/common/frame_messages.h"
#include "content/common/site_isolation_policy.h"
#include "content/common/view_messages.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/render_process_host_observer.h"
#include "content/public/browser/render_widget_host_iterator.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/user_metrics.h"
#include "content/public/common/browser_plugin_guest_mode.h"
#include "content/public/common/browser_side_navigation_policy.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/referrer.h"
#include "content/public/common/url_constants.h"

namespace content {

RenderFrameHostManager::RenderFrameHostManager(
    FrameTreeNode* frame_tree_node,
    RenderFrameHostDelegate* render_frame_delegate,
    RenderViewHostDelegate* render_view_delegate,
    RenderWidgetHostDelegate* render_widget_delegate,
    Delegate* delegate)
    : frame_tree_node_(frame_tree_node),
      delegate_(delegate),
      render_frame_delegate_(render_frame_delegate),
      render_view_delegate_(render_view_delegate),
      render_widget_delegate_(render_widget_delegate),
      interstitial_page_(nullptr),
      weak_factory_(this) {
  DCHECK(frame_tree_node_);
}

RenderFrameHostManager::~RenderFrameHostManager() {
  if (pending_render_frame_host_)
    UnsetPendingRenderFrameHost();

  if (speculative_render_frame_host_)
    UnsetSpeculativeRenderFrameHost();

  // Delete any RenderFrameProxyHosts and swapped out RenderFrameHosts.
  // It is important to delete those prior to deleting the current
  // RenderFrameHost, since the CrossProcessFrameConnector (owned by
  // RenderFrameProxyHost) points to the RenderWidgetHostView associated with
  // the current RenderFrameHost and uses it during its destructor.
  ResetProxyHosts();

  // We should always have a current RenderFrameHost except in some tests.
  SetRenderFrameHost(scoped_ptr<RenderFrameHostImpl>());
}

void RenderFrameHostManager::Init(SiteInstance* site_instance,
                                  int32_t view_routing_id,
                                  int32_t frame_routing_id,
                                  int32_t widget_routing_id) {
  DCHECK(site_instance);
  // TODO(avi): While RenderViewHostImpl is-a RenderWidgetHostImpl, this must
  // hold true to avoid having two RenderWidgetHosts for the top-level frame.
  // https://crbug.com/545684
  DCHECK(!frame_tree_node_->IsMainFrame() ||
         view_routing_id == widget_routing_id);
  int flags = delegate_->IsHidden() ? CREATE_RF_HIDDEN : 0;
  SetRenderFrameHost(CreateRenderFrameHost(site_instance, view_routing_id,
                                           frame_routing_id, widget_routing_id,
                                           flags));

  // Notify the delegate of the creation of the current RenderFrameHost.
  // Do this only for subframes, as the main frame case is taken care of by
  // WebContentsImpl::Init.
  if (!frame_tree_node_->IsMainFrame()) {
    delegate_->NotifySwappedFromRenderManager(
        nullptr, render_frame_host_.get(), false);
  }
}

RenderViewHostImpl* RenderFrameHostManager::current_host() const {
  if (!render_frame_host_)
    return nullptr;
  return render_frame_host_->render_view_host();
}

RenderViewHostImpl* RenderFrameHostManager::pending_render_view_host() const {
  if (!pending_render_frame_host_)
    return nullptr;
  return pending_render_frame_host_->render_view_host();
}

WebUIImpl* RenderFrameHostManager::GetNavigatingWebUI() const {
  if (IsBrowserSideNavigationEnabled()) {
    if (speculative_render_frame_host_)
      return speculative_render_frame_host_->web_ui();
  } else {
    if (pending_render_frame_host_)
      return pending_render_frame_host_->web_ui();
  }
  return render_frame_host_->pending_web_ui();
}

RenderWidgetHostView* RenderFrameHostManager::GetRenderWidgetHostView() const {
  if (interstitial_page_)
    return interstitial_page_->GetView();
  if (render_frame_host_)
    return render_frame_host_->GetView();
  return nullptr;
}

bool RenderFrameHostManager::ForInnerDelegate() {
  return delegate_->GetOuterDelegateFrameTreeNodeId() !=
      FrameTreeNode::kFrameTreeNodeInvalidId;
}

RenderWidgetHostImpl*
RenderFrameHostManager::GetOuterRenderWidgetHostForKeyboardInput() {
  if (!ForInnerDelegate() || !frame_tree_node_->IsMainFrame())
    return nullptr;

  FrameTreeNode* outer_contents_frame_tree_node =
      FrameTreeNode::GloballyFindByID(
          delegate_->GetOuterDelegateFrameTreeNodeId());
  return outer_contents_frame_tree_node->parent()
      ->current_frame_host()
      ->render_view_host()
      ->GetWidget();
}

FrameTreeNode* RenderFrameHostManager::GetOuterDelegateNode() {
  int outer_contents_frame_tree_node_id =
      delegate_->GetOuterDelegateFrameTreeNodeId();
  return FrameTreeNode::GloballyFindByID(outer_contents_frame_tree_node_id);
}

RenderFrameProxyHost* RenderFrameHostManager::GetProxyToParent() {
  if (frame_tree_node_->IsMainFrame())
    return nullptr;

  return GetRenderFrameProxyHost(frame_tree_node_->parent()
                                     ->render_manager()
                                     ->current_frame_host()
                                     ->GetSiteInstance());
}

RenderFrameProxyHost* RenderFrameHostManager::GetProxyToOuterDelegate() {
  int outer_contents_frame_tree_node_id =
      delegate_->GetOuterDelegateFrameTreeNodeId();
  FrameTreeNode* outer_contents_frame_tree_node =
      FrameTreeNode::GloballyFindByID(outer_contents_frame_tree_node_id);
  if (!outer_contents_frame_tree_node ||
      !outer_contents_frame_tree_node->parent()) {
    return nullptr;
  }

  return GetRenderFrameProxyHost(outer_contents_frame_tree_node->parent()
                                     ->current_frame_host()
                                     ->GetSiteInstance());
}

void RenderFrameHostManager::RemoveOuterDelegateFrame() {
  FrameTreeNode* outer_delegate_frame_tree_node =
      FrameTreeNode::GloballyFindByID(
          delegate_->GetOuterDelegateFrameTreeNodeId());
  DCHECK(outer_delegate_frame_tree_node->parent());
  outer_delegate_frame_tree_node->frame_tree()->RemoveFrame(
      outer_delegate_frame_tree_node);
}

RenderFrameHostImpl* RenderFrameHostManager::Navigate(
    const GURL& dest_url,
    const FrameNavigationEntry& frame_entry,
    const NavigationEntryImpl& entry) {
  TRACE_EVENT1("navigation", "RenderFrameHostManager:Navigate",
               "FrameTreeNode id", frame_tree_node_->frame_tree_node_id());
  // Create a pending RenderFrameHost to use for the navigation.
  RenderFrameHostImpl* dest_render_frame_host = UpdateStateForNavigate(
      dest_url,
      // TODO(creis): Move source_site_instance to FNE.
      entry.source_site_instance(), frame_entry.site_instance(),
      entry.GetTransitionType(),
      entry.restore_type() != NavigationEntryImpl::RESTORE_NONE,
      entry.IsViewSourceMode(), entry.transferred_global_request_id(),
      entry.bindings());
  if (!dest_render_frame_host)
    return nullptr;  // We weren't able to create a pending render frame host.

  // If the current render_frame_host_ isn't live, we should create it so
  // that we don't show a sad tab while the dest_render_frame_host fetches
  // its first page.  (Bug 1145340)
  if (dest_render_frame_host != render_frame_host_.get() &&
      !render_frame_host_->IsRenderFrameLive()) {
    // Note: we don't call InitRenderView here because we are navigating away
    // soon anyway, and we don't have the NavigationEntry for this host.
    delegate_->CreateRenderViewForRenderManager(
        render_frame_host_->render_view_host(), MSG_ROUTING_NONE,
        MSG_ROUTING_NONE, frame_tree_node_->current_replication_state());
  }

  // If the renderer isn't live, then try to create a new one to satisfy this
  // navigation request.
  if (!dest_render_frame_host->IsRenderFrameLive()) {
    // Instruct the destination render frame host to set up a Mojo connection
    // with the new render frame if necessary.  Note that this call needs to
    // occur before initializing the RenderView; the flow of creating the
    // RenderView can cause browser-side code to execute that expects the this
    // RFH's ServiceRegistry to be initialized (e.g., if the site is a WebUI
    // site that is handled via Mojo, then Mojo WebUI code in //chrome will
    // add a service to this RFH's ServiceRegistry).
    dest_render_frame_host->SetUpMojoIfNeeded();

    // Recreate the opener chain.
    CreateOpenerProxies(dest_render_frame_host->GetSiteInstance(),
                        frame_tree_node_);
    if (!InitRenderView(dest_render_frame_host->render_view_host(), nullptr))
      return nullptr;

    if (GetNavigatingWebUI()) {
      // A new RenderView was created and there is a navigating WebUI which
      // never interacted with it. So notify the WebUI using RenderViewCreated.
      GetNavigatingWebUI()->RenderViewCreated(
          dest_render_frame_host->render_view_host());
    }

    // Now that we've created a new renderer, be sure to hide it if it isn't
    // our primary one.  Otherwise, we might crash if we try to call Show()
    // on it later.
    if (dest_render_frame_host != render_frame_host_.get()) {
      if (dest_render_frame_host->GetView())
        dest_render_frame_host->GetView()->Hide();
    } else {
      // After a renderer crash we'd have marked the host as invisible, so we
      // need to set the visibility of the new View to the correct value here
      // after reload.
      if (dest_render_frame_host->GetView() &&
          dest_render_frame_host->render_view_host()
                  ->GetWidget()
                  ->is_hidden() != delegate_->IsHidden()) {
        if (delegate_->IsHidden()) {
          dest_render_frame_host->GetView()->Hide();
        } else {
          dest_render_frame_host->GetView()->Show();
        }
      }

      // TODO(nasko): This is a very ugly hack. The Chrome extensions process
      // manager still uses NotificationService and expects to see a
      // RenderViewHost changed notification after WebContents and
      // RenderFrameHostManager are completely initialized. This should be
      // removed once the process manager moves away from NotificationService.
      // See https://crbug.com/462682.
      delegate_->NotifyMainFrameSwappedFromRenderManager(
          nullptr, render_frame_host_->render_view_host());
    }
  }

  // If entry includes the request ID of a request that is being transferred,
  // the destination render frame will take ownership, so release ownership of
  // the request.
  if (cross_site_transferring_request_.get() &&
      cross_site_transferring_request_->request_id() ==
          entry.transferred_global_request_id()) {
    cross_site_transferring_request_->ReleaseRequest();

    // The navigating RenderFrameHost should take ownership of the
    // NavigationHandle that came from the transferring RenderFrameHost.
    DCHECK(transfer_navigation_handle_);
    dest_render_frame_host->SetNavigationHandle(
        std::move(transfer_navigation_handle_));
  }
  DCHECK(!transfer_navigation_handle_);

  return dest_render_frame_host;
}

void RenderFrameHostManager::Stop() {
  render_frame_host_->Stop();

  // If a cross-process navigation is happening, the pending RenderFrameHost
  // should stop. This will lead to a DidFailProvisionalLoad, which will
  // properly destroy it.
  if (pending_render_frame_host_) {
    pending_render_frame_host_->Send(new FrameMsg_Stop(
        pending_render_frame_host_->GetRoutingID()));
  }

  // PlzNavigate: a loading speculative RenderFrameHost should also stop.
  if (IsBrowserSideNavigationEnabled()) {
    if (speculative_render_frame_host_ &&
        speculative_render_frame_host_->is_loading()) {
      speculative_render_frame_host_->Send(
          new FrameMsg_Stop(speculative_render_frame_host_->GetRoutingID()));
    }
  }
}

void RenderFrameHostManager::SetIsLoading(bool is_loading) {
  render_frame_host_->render_view_host()->GetWidget()->SetIsLoading(is_loading);
  if (pending_render_frame_host_) {
    pending_render_frame_host_->render_view_host()->GetWidget()->SetIsLoading(
        is_loading);
  }
}

bool RenderFrameHostManager::ShouldCloseTabOnUnresponsiveRenderer() {
  // If we're waiting for a close ACK, then the tab should close whether there's
  // a navigation in progress or not.  Unfortunately, we also need to check for
  // cases that we arrive here with no navigation in progress, since there are
  // some tab closure paths that don't set is_waiting_for_close_ack to true.
  // TODO(creis): Clean this up in http://crbug.com/418266.
  if (!pending_render_frame_host_ ||
      render_frame_host_->render_view_host()->is_waiting_for_close_ack())
    return true;

  // We should always have a pending RFH when there's a cross-process navigation
  // in progress.  Sanity check this for http://crbug.com/276333.
  CHECK(pending_render_frame_host_);

  // Unload handlers run in the background, so we should never get an
  // unresponsiveness warning for them.
  CHECK(!render_frame_host_->IsWaitingForUnloadACK());

  // If the tab becomes unresponsive during beforeunload while doing a
  // cross-process navigation, proceed with the navigation.  (This assumes that
  // the pending RenderFrameHost is still responsive.)
  if (render_frame_host_->is_waiting_for_beforeunload_ack()) {
    // Haven't gotten around to starting the request, because we're still
    // waiting for the beforeunload handler to finish.  We'll pretend that it
    // did finish, to let the navigation proceed.  Note that there's a danger
    // that the beforeunload handler will later finish and possibly return
    // false (meaning the navigation should not proceed), but we'll ignore it
    // in this case because it took too long.
    if (pending_render_frame_host_->are_navigations_suspended()) {
      pending_render_frame_host_->SetNavigationsSuspended(
          false, base::TimeTicks::Now());
    }
  }
  return false;
}

void RenderFrameHostManager::OnBeforeUnloadACK(
    bool for_cross_site_transition,
    bool proceed,
    const base::TimeTicks& proceed_time) {
  if (for_cross_site_transition) {
    DCHECK(!IsBrowserSideNavigationEnabled());
    // Ignore if we're not in a cross-process navigation.
    if (!pending_render_frame_host_)
      return;

    if (proceed) {
      // Ok to unload the current page, so proceed with the cross-process
      // navigation.  Note that if navigations are not currently suspended, it
      // might be because the renderer was deemed unresponsive and this call was
      // already made by ShouldCloseTabOnUnresponsiveRenderer.  In that case, it
      // is ok to do nothing here.
      if (pending_render_frame_host_ &&
          pending_render_frame_host_->are_navigations_suspended()) {
        pending_render_frame_host_->SetNavigationsSuspended(false,
                                                            proceed_time);
      }
    } else {
      // Current page says to cancel.
      CancelPending();
    }
  } else {
    // Non-cross-process transition means closing the entire tab.
    bool proceed_to_fire_unload;
    delegate_->BeforeUnloadFiredFromRenderManager(proceed, proceed_time,
                                                  &proceed_to_fire_unload);

    if (proceed_to_fire_unload) {
      // If we're about to close the tab and there's a pending RFH, cancel it.
      // Otherwise, if the navigation in the pending RFH completes before the
      // close in the current RFH, we'll lose the tab close.
      if (pending_render_frame_host_) {
        CancelPending();
      }

      // PlzNavigate: clean up the speculative RenderFrameHost if there is one.
      if (IsBrowserSideNavigationEnabled() && speculative_render_frame_host_)
        CleanUpNavigation();

      // This is not a cross-process navigation; the tab is being closed.
      render_frame_host_->render_view_host()->ClosePage();
    }
  }
}

void RenderFrameHostManager::OnCrossSiteResponse(
    RenderFrameHostImpl* pending_render_frame_host,
    const GlobalRequestID& global_request_id,
    scoped_ptr<CrossSiteTransferringRequest> cross_site_transferring_request,
    const std::vector<GURL>& transfer_url_chain,
    const Referrer& referrer,
    ui::PageTransition page_transition,
    bool should_replace_current_entry) {
  // We should only get here for transfer navigations.  Most cross-process
  // navigations can just continue and wait to run the unload handler (by
  // swapping out) when the new navigation commits.
  CHECK(cross_site_transferring_request);

  // A transfer should only have come from our pending or current RFH.
  // TODO(creis): We need to handle the case that the pending RFH has changed
  // in the mean time, while this was being posted from the IO thread.  We
  // should probably cancel the request in that case.
  DCHECK(pending_render_frame_host == pending_render_frame_host_.get() ||
         pending_render_frame_host == render_frame_host_.get());

  // Check if the FrameTreeNode is loading. This will be used later to notify
  // the FrameTreeNode that the load stop if the transfer fails.
  bool frame_tree_node_was_loading = frame_tree_node_->IsLoading();

  // Store the transferring request so that we can release it if the transfer
  // navigation matches.
  cross_site_transferring_request_ = std::move(cross_site_transferring_request);

  // Store the NavigationHandle to give it to the appropriate RenderFrameHost
  // after it started navigating.
  transfer_navigation_handle_ =
      pending_render_frame_host->PassNavigationHandleOwnership();
  DCHECK(transfer_navigation_handle_);

  // Set the transferring RenderFrameHost as not loading, so that it does not
  // emit a DidStopLoading notification if it is destroyed when creating the
  // new navigating RenderFrameHost.
  pending_render_frame_host->set_is_loading(false);

  // Sanity check that the params are for the correct frame and process.
  // These should match the RenderFrameHost that made the request.
  // If it started as a cross-process navigation via OpenURL, this is the
  // pending one.  If it wasn't cross-process until the transfer, this is
  // the current one.
  int render_frame_id = pending_render_frame_host_
                            ? pending_render_frame_host_->GetRoutingID()
                            : render_frame_host_->GetRoutingID();
  DCHECK_EQ(render_frame_id, pending_render_frame_host->GetRoutingID());
  int process_id = pending_render_frame_host_ ?
      pending_render_frame_host_->GetProcess()->GetID() :
      render_frame_host_->GetProcess()->GetID();
  DCHECK_EQ(process_id, global_request_id.child_id);

  // Treat the last URL in the chain as the destination and the remainder as
  // the redirect chain.
  CHECK(transfer_url_chain.size());
  GURL transfer_url = transfer_url_chain.back();
  std::vector<GURL> rest_of_chain = transfer_url_chain;
  rest_of_chain.pop_back();

  pending_render_frame_host->frame_tree_node()->navigator()->RequestTransferURL(
      pending_render_frame_host, transfer_url, nullptr, rest_of_chain, referrer,
      page_transition, global_request_id, should_replace_current_entry);

  // The transferring request was only needed during the RequestTransferURL
  // call, so it is safe to clear at this point.
  cross_site_transferring_request_.reset();

  // If the navigation continued, the NavigationHandle should have been
  // transfered to a RenderFrameHost. In the other cases, it should be cleared.
  transfer_navigation_handle_.reset();

  // If the navigation in the new renderer did not start, inform the
  // FrameTreeNode that it stopped loading.
  if (!frame_tree_node_->IsLoading() && frame_tree_node_was_loading)
    frame_tree_node_->DidStopLoading();
}

void RenderFrameHostManager::DidNavigateFrame(
    RenderFrameHostImpl* render_frame_host,
    bool was_caused_by_user_gesture) {
  CommitPendingIfNecessary(render_frame_host, was_caused_by_user_gesture);

  // Make sure any dynamic changes to this frame's sandbox flags that were made
  // prior to navigation take effect.
  CommitPendingSandboxFlags();
}

void RenderFrameHostManager::CommitPendingIfNecessary(
    RenderFrameHostImpl* render_frame_host,
    bool was_caused_by_user_gesture) {
  if (!pending_render_frame_host_ && !speculative_render_frame_host_) {
    // There's no pending/speculative RenderFrameHost so it must be that the
    // current renderer process completed a navigation.

    // We should only hear this from our current renderer.
    DCHECK_EQ(render_frame_host_.get(), render_frame_host);

    // If the current RenderFrameHost has a pending WebUI it must be committed.
    // Note: When one tries to move same-site commit logic into RenderFrameHost
    // itself, mind that the focus setting logic inside CommitPending also needs
    // to be moved there.
    if (render_frame_host_->pending_web_ui())
      CommitPendingWebUI();
    return;
  }

  if (render_frame_host == pending_render_frame_host_.get() ||
      render_frame_host == speculative_render_frame_host_.get()) {
    // A cross-process navigation completed, so show the new renderer. If a
    // same-process navigation is also ongoing, it will be canceled when the
    // pending/speculative RenderFrameHost replaces the current one in the
    // commit call below.
    CommitPending();
  } else if (render_frame_host == render_frame_host_.get()) {
    // A same-process navigation committed while a simultaneous cross-process
    // navigation is still ongoing.

    // If the current RenderFrameHost has a pending WebUI it must be committed.
    if (render_frame_host_->pending_web_ui())
      CommitPendingWebUI();

    // Decide on canceling the ongoing cross-process navigation.
    if (IsBrowserSideNavigationEnabled()) {
      CleanUpNavigation();
    } else {
      if (was_caused_by_user_gesture) {
        // A navigation in the original page has taken place.  Cancel the
        // pending one. Only do it for user gesture originated navigations to
        // prevent page doing any shenanigans to prevent user from navigating.
        // See https://code.google.com/p/chromium/issues/detail?id=75195
        CancelPending();
      }
    }
  } else {
    // No one else should be sending us DidNavigate in this state.
    NOTREACHED();
  }
}

void RenderFrameHostManager::DidChangeOpener(
    int opener_routing_id,
    SiteInstance* source_site_instance) {
  FrameTreeNode* opener = nullptr;
  if (opener_routing_id != MSG_ROUTING_NONE) {
    RenderFrameHostImpl* opener_rfhi = RenderFrameHostImpl::FromID(
        source_site_instance->GetProcess()->GetID(), opener_routing_id);
    // If |opener_rfhi| is null, the opener RFH has already disappeared.  In
    // this case, clear the opener rather than keeping the old opener around.
    if (opener_rfhi)
      opener = opener_rfhi->frame_tree_node();
  }

  if (frame_tree_node_->opener() == opener)
    return;

  frame_tree_node_->SetOpener(opener);

  for (const auto& pair : proxy_hosts_) {
    if (pair.second->GetSiteInstance() == source_site_instance)
      continue;
    pair.second->UpdateOpener();
  }

  if (render_frame_host_->GetSiteInstance() != source_site_instance)
    render_frame_host_->UpdateOpener();

  // Notify the pending and speculative RenderFrameHosts as well.  This is
  // necessary in case a process swap has started while the message was in
  // flight.
  if (pending_render_frame_host_ &&
      pending_render_frame_host_->GetSiteInstance() != source_site_instance) {
    pending_render_frame_host_->UpdateOpener();
  }

  if (speculative_render_frame_host_ &&
      speculative_render_frame_host_->GetSiteInstance() !=
          source_site_instance) {
    speculative_render_frame_host_->UpdateOpener();
  }
}

void RenderFrameHostManager::CommitPendingSandboxFlags() {
  // Return early if there were no pending sandbox flags updates.
  if (!frame_tree_node_->CommitPendingSandboxFlags())
    return;

  // Sandbox flags updates can only happen when the frame has a parent.
  CHECK(frame_tree_node_->parent());

  // Notify all of the frame's proxies about updated sandbox flags, excluding
  // the parent process since it already knows the latest flags.
  SiteInstance* parent_site_instance =
      frame_tree_node_->parent()->current_frame_host()->GetSiteInstance();
  for (const auto& pair : proxy_hosts_) {
    if (pair.second->GetSiteInstance() != parent_site_instance) {
      pair.second->Send(new FrameMsg_DidUpdateSandboxFlags(
          pair.second->GetRoutingID(),
          frame_tree_node_->current_replication_state().sandbox_flags));
    }
  }
}

void RenderFrameHostManager::SwapOutOldFrame(
    scoped_ptr<RenderFrameHostImpl> old_render_frame_host) {
  TRACE_EVENT1("navigation", "RenderFrameHostManager::SwapOutOldFrame",
               "FrameTreeNode id", frame_tree_node_->frame_tree_node_id());

  // Tell the renderer to suppress any further modal dialogs so that we can swap
  // it out.  This must be done before canceling any current dialog, in case
  // there is a loop creating additional dialogs.
  // TODO(creis): Handle modal dialogs in subframe processes.
  old_render_frame_host->render_view_host()->SuppressDialogsUntilSwapOut();

  // Now close any modal dialogs that would prevent us from swapping out.  This
  // must be done separately from SwapOut, so that the PageGroupLoadDeferrer is
  // no longer on the stack when we send the SwapOut message.
  delegate_->CancelModalDialogsForRenderManager();

  // If the old RFH is not live, just return as there is no further work to do.
  // It will be deleted and there will be no proxy created.
  if (!old_render_frame_host->IsRenderFrameLive())
    return;

  // If there are no active frames besides this one, we can delete the old
  // RenderFrameHost once it runs its unload handler, without replacing it with
  // a proxy.
  if (old_render_frame_host->GetSiteInstance()->active_frame_count() <= 1) {
    // Tell the old RenderFrameHost to swap out, with no proxy to replace it.
    old_render_frame_host->SwapOut(nullptr, true);
    MoveToPendingDeleteHosts(std::move(old_render_frame_host));
    return;
  }

  // Otherwise there are active views and we need a proxy for the old RFH.
  // (There should not be one yet.)
  RenderFrameProxyHost* proxy =
      CreateRenderFrameProxyHost(old_render_frame_host->GetSiteInstance(),
                                 old_render_frame_host->render_view_host());

  // Tell the old RenderFrameHost to swap out and be replaced by the proxy.
  old_render_frame_host->SwapOut(proxy, true);

  // SwapOut creates a RenderFrameProxy, so set the proxy to be initialized.
  proxy->set_render_frame_proxy_created(true);

  if (SiteIsolationPolicy::IsSwappedOutStateForbidden()) {
    // In --site-per-process, frames delete their RFH rather than storing it
    // in the proxy.  Schedule it for deletion once the SwapOutACK comes in.
    // TODO(creis): This will be the default when we remove swappedout://.
    MoveToPendingDeleteHosts(std::move(old_render_frame_host));
  } else {
    // We shouldn't get here for subframes, since we only swap subframes when
    // --site-per-process is used.
    DCHECK(frame_tree_node_->IsMainFrame());

    // The old RenderFrameHost will stay alive inside the proxy so that existing
    // JavaScript window references to it stay valid.
    proxy->TakeFrameHostOwnership(std::move(old_render_frame_host));
  }
}

void RenderFrameHostManager::DiscardUnusedFrame(
    scoped_ptr<RenderFrameHostImpl> render_frame_host) {
  // TODO(carlosk): this code is very similar to what can be found in
  // SwapOutOldFrame and we should see that these are unified at some point.

  // If the SiteInstance for the pending RFH is being used by others don't
  // delete the RFH. Just swap it out and it can be reused at a later point.
  // In --site-per-process, RenderFrameHosts are not kept around and are
  // deleted when not used, replaced by RenderFrameProxyHosts.
  SiteInstanceImpl* site_instance = render_frame_host->GetSiteInstance();
  if (site_instance->HasSite() && site_instance->active_frame_count() > 1) {
    // Any currently suspended navigations are no longer needed.
    render_frame_host->CancelSuspendedNavigations();

    // If a proxy already exists for the |site_instance|, just reuse it instead
    // of creating a new one. There is no need to call SwapOut on the
    // |render_frame_host|, as this method is only called to discard a pending
    // or speculative RenderFrameHost, i.e. one that has never hosted an actual
    // document.
    RenderFrameProxyHost* proxy = GetRenderFrameProxyHost(site_instance);
    if (!proxy) {
      proxy = CreateRenderFrameProxyHost(site_instance,
                                         render_frame_host->render_view_host());
    }

    if (!SiteIsolationPolicy::IsSwappedOutStateForbidden()) {
      DCHECK(frame_tree_node_->IsMainFrame());

      // When using swapped out RenderFrameHosts, it is possible for the pending
      // RenderFrameHost to be an existing one in swapped out state. Since it
      // has been used to start a navigation, it could have committed a
      // document. Check if |render_frame_host| is already swapped out, to avoid
      // swapping it out again.
      if (!render_frame_host->is_swapped_out())
        render_frame_host->SwapOut(proxy, false);

      proxy->TakeFrameHostOwnership(std::move(render_frame_host));
    }
  }

  render_frame_host.reset();
}

void RenderFrameHostManager::MoveToPendingDeleteHosts(
    scoped_ptr<RenderFrameHostImpl> render_frame_host) {
  // If this is the main frame going away and there are no more references to
  // its RenderViewHost, mark it for deletion as well so that we don't try to
  // reuse it.
  if (render_frame_host->frame_tree_node()->IsMainFrame() &&
      render_frame_host->render_view_host()->ref_count() <= 1) {
    render_frame_host->render_view_host()->set_pending_deletion();
  }

  // |render_frame_host| will be deleted when its SwapOut ACK is received, or
  // when the timer times out, or when the RFHM itself is deleted (whichever
  // comes first).
  pending_delete_hosts_.push_back(std::move(render_frame_host));
}

bool RenderFrameHostManager::IsPendingDeletion(
    RenderFrameHostImpl* render_frame_host) {
  for (const auto& rfh : pending_delete_hosts_) {
    if (rfh.get() == render_frame_host)
      return true;
  }
  return false;
}

bool RenderFrameHostManager::DeleteFromPendingList(
    RenderFrameHostImpl* render_frame_host) {
  for (RFHPendingDeleteList::iterator iter = pending_delete_hosts_.begin();
       iter != pending_delete_hosts_.end();
       iter++) {
    if (iter->get() == render_frame_host) {
      pending_delete_hosts_.erase(iter);
      return true;
    }
  }
  return false;
}

void RenderFrameHostManager::ResetProxyHosts() {
  for (const auto& pair : proxy_hosts_) {
    static_cast<SiteInstanceImpl*>(pair.second->GetSiteInstance())
        ->RemoveObserver(this);
  }
  proxy_hosts_.clear();
}

void RenderFrameHostManager::ClearRFHsPendingShutdown() {
  pending_delete_hosts_.clear();
}

void RenderFrameHostManager::ClearWebUIInstances() {
  current_frame_host()->ClearAllWebUI();
  if (pending_render_frame_host_)
    pending_render_frame_host_->ClearAllWebUI();
  // PlzNavigate
  if (speculative_render_frame_host_)
    speculative_render_frame_host_->ClearAllWebUI();
}

// PlzNavigate
void RenderFrameHostManager::DidCreateNavigationRequest(
    const NavigationRequest& request) {
  CHECK(IsBrowserSideNavigationEnabled());
  RenderFrameHostImpl* dest_rfh = GetFrameHostForNavigation(request);
  DCHECK(dest_rfh);
}

// PlzNavigate
RenderFrameHostImpl* RenderFrameHostManager::GetFrameHostForNavigation(
    const NavigationRequest& request) {
  CHECK(IsBrowserSideNavigationEnabled());

  SiteInstance* current_site_instance = render_frame_host_->GetSiteInstance();

  SiteInstance* candidate_site_instance =
      speculative_render_frame_host_
          ? speculative_render_frame_host_->GetSiteInstance()
          : nullptr;

  scoped_refptr<SiteInstance> dest_site_instance = GetSiteInstanceForNavigation(
      request.common_params().url, request.source_site_instance(),
      request.dest_site_instance(), candidate_site_instance,
      request.common_params().transition,
      request.restore_type() != NavigationEntryImpl::RESTORE_NONE,
      request.is_view_source());

  // The appropriate RenderFrameHost to commit the navigation.
  RenderFrameHostImpl* navigation_rfh = nullptr;

  bool notify_webui_of_rv_creation = false;

  // Reuse the current RenderFrameHost if its SiteInstance matches the
  // navigation's.
  bool no_renderer_swap = current_site_instance == dest_site_instance.get();
  if (SiteIsolationPolicy::AreCrossProcessFramesPossible()) {
    // Check if the current renderer should be changed for the navigation.
    no_renderer_swap |=
        render_frame_host_->IsRenderFrameLive() &&
        ShouldMakeNetworkRequestForURL(request.common_params().url) &&
        !IsRendererTransferNeededForNavigation(render_frame_host_.get(),
                                               request.common_params().url);
  } else {
    // Subframe navigations will use the current renderer.
    no_renderer_swap |= !frame_tree_node_->IsMainFrame();

    // Renderer-initiated main frame navigations that may require a
    // SiteInstance swap are sent to the browser via the OpenURL IPC and are
    // afterwards treated as browser-initiated navigations. NavigationRequests
    // marked as renderer-initiated are created by receiving a BeginNavigation
    // IPC, and will then proceed in the same renderer that sent the IPC due to
    // the condition below.
    // TODO(carlosk): Have renderer-initated main frame navigations swap
    // processes if needed when it no longer breaks OAuth popups (see
    // https://crbug.com/440266).
    no_renderer_swap |= !request.browser_initiated();
  }

  if (no_renderer_swap) {
    // GetFrameHostForNavigation will be called more than once during a
    // navigation (currently twice, on request and when it's about to commit in
    // the renderer). In the follow up calls an existing pending WebUI should
    // not be recreated if the URL didn't change. So instead of calling
    // CleanUpNavigation just discard the speculative RenderFrameHost if one
    // exists.
    if (speculative_render_frame_host_)
      DiscardUnusedFrame(UnsetSpeculativeRenderFrameHost());

    UpdatePendingWebUIOnCurrentFrameHost(request.common_params().url,
                                         request.bindings());

    navigation_rfh = render_frame_host_.get();

    DCHECK(!speculative_render_frame_host_);
  } else {
    // If the current RenderFrameHost cannot be used a speculative one is
    // created with the SiteInstance for the current URL. If a speculative
    // RenderFrameHost already exists we try as much as possible to reuse it and
    // its associated WebUI.

    // Check if an existing speculative RenderFrameHost can be reused.
    if (!speculative_render_frame_host_ ||
        speculative_render_frame_host_->GetSiteInstance() !=
            dest_site_instance.get()) {
      // If a previous speculative RenderFrameHost didn't exist or if its
      // SiteInstance differs from the one for the current URL, a new one needs
      // to be created.
      CleanUpNavigation();
      bool success = CreateSpeculativeRenderFrameHost(current_site_instance,
                                                      dest_site_instance.get());
      DCHECK(success);
    }
    DCHECK(speculative_render_frame_host_);

    bool changed_web_ui = speculative_render_frame_host_->UpdatePendingWebUI(
        request.common_params().url, request.bindings());
    speculative_render_frame_host_->CommitPendingWebUI();
    DCHECK_EQ(GetNavigatingWebUI(), speculative_render_frame_host_->web_ui());
    notify_webui_of_rv_creation =
        changed_web_ui && speculative_render_frame_host_->web_ui();

    navigation_rfh = speculative_render_frame_host_.get();

    // Check if our current RFH is live.
    if (!render_frame_host_->IsRenderFrameLive()) {
      // The current RFH is not live.  There's no reason to sit around with a
      // sad tab or a newly created RFH while we wait for the navigation to
      // complete. Just switch to the speculative RFH now and go back to normal.
      // (Note that we don't care about on{before}unload handlers if the current
      // RFH isn't live.)
      CommitPending();
    }
  }
  DCHECK(navigation_rfh &&
         (navigation_rfh == render_frame_host_.get() ||
          navigation_rfh == speculative_render_frame_host_.get()));

  // If the RenderFrame that needs to navigate is not live (its process was just
  // created or has crashed), initialize it.
  if (!navigation_rfh->IsRenderFrameLive()) {
    // Recreate the opener chain.
    CreateOpenerProxies(navigation_rfh->GetSiteInstance(), frame_tree_node_);
    if (!InitRenderView(navigation_rfh->render_view_host(), nullptr))
      return nullptr;
    notify_webui_of_rv_creation = true;

    if (navigation_rfh == render_frame_host_.get()) {
      // TODO(nasko): This is a very ugly hack. The Chrome extensions process
      // manager still uses NotificationService and expects to see a
      // RenderViewHost changed notification after WebContents and
      // RenderFrameHostManager are completely initialized. This should be
      // removed once the process manager moves away from NotificationService.
      // See https://crbug.com/462682.
      delegate_->NotifyMainFrameSwappedFromRenderManager(
          nullptr, render_frame_host_->render_view_host());
    }
    DCHECK(navigation_rfh->IsRenderFrameLive());
  }

  // If a WebUI was created in a speculative RenderFrameHost or a new RenderView
  // was created then the WebUI never interacted with the RenderView. Notify
  // using RenderViewCreated.
  if (notify_webui_of_rv_creation && GetNavigatingWebUI())
    GetNavigatingWebUI()->RenderViewCreated(navigation_rfh->render_view_host());

  return navigation_rfh;
}

// PlzNavigate
void RenderFrameHostManager::CleanUpNavigation() {
  CHECK(IsBrowserSideNavigationEnabled());
  render_frame_host_->ClearPendingWebUI();
  if (speculative_render_frame_host_) {
    bool was_loading = speculative_render_frame_host_->is_loading();
    DiscardUnusedFrame(UnsetSpeculativeRenderFrameHost());
    if (was_loading)
      frame_tree_node_->DidStopLoading();
  }
}

// PlzNavigate
scoped_ptr<RenderFrameHostImpl>
RenderFrameHostManager::UnsetSpeculativeRenderFrameHost() {
  CHECK(IsBrowserSideNavigationEnabled());
  speculative_render_frame_host_->GetProcess()->RemovePendingView();
  return std::move(speculative_render_frame_host_);
}

void RenderFrameHostManager::OnDidStartLoading() {
  for (const auto& pair : proxy_hosts_) {
    pair.second->Send(
        new FrameMsg_DidStartLoading(pair.second->GetRoutingID()));
  }
}

void RenderFrameHostManager::OnDidStopLoading() {
  for (const auto& pair : proxy_hosts_) {
    pair.second->Send(new FrameMsg_DidStopLoading(pair.second->GetRoutingID()));
  }
}

void RenderFrameHostManager::OnDidUpdateName(const std::string& name,
                                             const std::string& unique_name) {
  // The window.name message may be sent outside of --site-per-process when
  // report_frame_name_changes renderer preference is set (used by
  // WebView).  Don't send the update to proxies in those cases.
  // TODO(nick,nasko): Should this be IsSwappedOutStateForbidden, to match
  // OnDidUpdateOrigin?
  if (!SiteIsolationPolicy::AreCrossProcessFramesPossible())
    return;

  for (const auto& pair : proxy_hosts_) {
    pair.second->Send(new FrameMsg_DidUpdateName(pair.second->GetRoutingID(),
                                                 name, unique_name));
  }
}

void RenderFrameHostManager::OnEnforceStrictMixedContentChecking(
    bool should_enforce) {
  if (!SiteIsolationPolicy::AreCrossProcessFramesPossible())
    return;

  for (const auto& pair : proxy_hosts_) {
    pair.second->Send(new FrameMsg_EnforceStrictMixedContentChecking(
        pair.second->GetRoutingID(), should_enforce));
  }
}

void RenderFrameHostManager::OnDidUpdateOrigin(const url::Origin& origin) {
  if (!SiteIsolationPolicy::IsSwappedOutStateForbidden())
    return;

  for (const auto& pair : proxy_hosts_) {
    pair.second->Send(
        new FrameMsg_DidUpdateOrigin(pair.second->GetRoutingID(), origin));
  }
}

RenderFrameHostManager::SiteInstanceDescriptor::SiteInstanceDescriptor(
    BrowserContext* browser_context,
    GURL dest_url,
    bool related_to_current)
    : existing_site_instance(nullptr),
      new_is_related_to_current(related_to_current) {
  new_site_url = SiteInstance::GetSiteForURL(browser_context, dest_url);
}

void RenderFrameHostManager::RenderProcessGone(SiteInstanceImpl* instance) {
  GetRenderFrameProxyHost(instance)->set_render_frame_proxy_created(false);
}

void RenderFrameHostManager::ActiveFrameCountIsZero(
    SiteInstanceImpl* site_instance) {
  // |site_instance| no longer contains any active RenderFrameHosts, so we don't
  // need to maintain a proxy there anymore.
  RenderFrameProxyHost* proxy = GetRenderFrameProxyHost(site_instance);
  CHECK(proxy);

  // Delete the proxy.  If it is for a main frame (and the RFH is stored
  // in the proxy) and it was still pending swap out, move the RFH to the
  // pending deletion list first.
  if (frame_tree_node_->IsMainFrame() && proxy->render_frame_host() &&
      proxy->render_frame_host()->rfh_state() ==
          RenderFrameHostImpl::STATE_PENDING_SWAP_OUT) {
    DCHECK(!SiteIsolationPolicy::IsSwappedOutStateForbidden());
    scoped_ptr<RenderFrameHostImpl> swapped_out_rfh =
        proxy->PassFrameHostOwnership();
    MoveToPendingDeleteHosts(std::move(swapped_out_rfh));
  }

  DeleteRenderFrameProxyHost(site_instance);
}

RenderFrameProxyHost* RenderFrameHostManager::CreateRenderFrameProxyHost(
    SiteInstance* site_instance,
    RenderViewHostImpl* rvh) {
  int site_instance_id = site_instance->GetId();
  CHECK(proxy_hosts_.find(site_instance_id) == proxy_hosts_.end())
      << "A proxy already existed for this SiteInstance.";
  RenderFrameProxyHost* proxy_host =
      new RenderFrameProxyHost(site_instance, rvh, frame_tree_node_);
  proxy_hosts_[site_instance_id] = make_scoped_ptr(proxy_host);
  static_cast<SiteInstanceImpl*>(site_instance)->AddObserver(this);
  return proxy_host;
}

void RenderFrameHostManager::DeleteRenderFrameProxyHost(
    SiteInstance* site_instance) {
  static_cast<SiteInstanceImpl*>(site_instance)->RemoveObserver(this);
  proxy_hosts_.erase(site_instance->GetId());
}

bool RenderFrameHostManager::ShouldTransitionCrossSite() {
  // The logic below is weaker than "are all sites isolated" -- it asks instead,
  // "is any site isolated". That's appropriate here since we're just trying to
  // figure out if we're in any kind of site isolated mode, and in which case,
  // we ignore the kSingleProcess and kProcessPerTab settings.
  //
  // TODO(nick): Move all handling of kSingleProcess/kProcessPerTab into
  // SiteIsolationPolicy so we have a consistent behavior around the interaction
  // of the process model flags.
  if (SiteIsolationPolicy::AreCrossProcessFramesPossible())
    return true;

  // False in the single-process mode, as it makes RVHs to accumulate
  // in swapped_out_hosts_.
  // True if we are using process-per-site-instance (default) or
  // process-per-site (kProcessPerSite).
  // TODO(nick): Move handling of kSingleProcess and kProcessPerTab into
  // SiteIsolationPolicy.
  return !base::CommandLine::ForCurrentProcess()->HasSwitch(
             switches::kSingleProcess) &&
         !base::CommandLine::ForCurrentProcess()->HasSwitch(
             switches::kProcessPerTab);
}

bool RenderFrameHostManager::ShouldSwapBrowsingInstancesForNavigation(
    const GURL& current_effective_url,
    bool current_is_view_source_mode,
    SiteInstance* new_site_instance,
    const GURL& new_effective_url,
    bool new_is_view_source_mode) const {
  // A subframe must stay in the same BrowsingInstance as its parent.
  // TODO(nasko): Ensure that SiteInstance swap is still triggered for subframes
  // in the cases covered by the rest of the checks in this method.
  if (!frame_tree_node_->IsMainFrame())
    return false;

  // If new_entry already has a SiteInstance, assume it is correct.  We only
  // need to force a swap if it is in a different BrowsingInstance.
  if (new_site_instance) {
    return !new_site_instance->IsRelatedSiteInstance(
        render_frame_host_->GetSiteInstance());
  }

  // Check for reasons to swap processes even if we are in a process model that
  // doesn't usually swap (e.g., process-per-tab).  Any time we return true,
  // the new_entry will be rendered in a new SiteInstance AND BrowsingInstance.
  BrowserContext* browser_context =
      delegate_->GetControllerForRenderManager().GetBrowserContext();

  // Don't force a new BrowsingInstance for debug URLs that are handled in the
  // renderer process, like javascript: or chrome://crash.
  if (IsRendererDebugURL(new_effective_url))
    return false;

  // For security, we should transition between processes when one is a Web UI
  // page and one isn't, or if the WebUI types differ.
  if (ChildProcessSecurityPolicyImpl::GetInstance()->HasWebUIBindings(
          render_frame_host_->GetProcess()->GetID()) ||
      WebUIControllerFactoryRegistry::GetInstance()->UseWebUIForURL(
          browser_context, current_effective_url)) {
    // If so, force a swap if destination is not an acceptable URL for Web UI.
    // Here, data URLs are never allowed.
    if (!WebUIControllerFactoryRegistry::GetInstance()->IsURLAcceptableForWebUI(
            browser_context, new_effective_url)) {
      return true;
    }

    // Force swap if the current WebUI type differs from the one for the
    // destination.
    if (WebUIControllerFactoryRegistry::GetInstance()->GetWebUIType(
            browser_context, current_effective_url) !=
        WebUIControllerFactoryRegistry::GetInstance()->GetWebUIType(
            browser_context, new_effective_url)) {
      return true;
    }
  } else {
    // Force a swap if it's a Web UI URL.
    if (WebUIControllerFactoryRegistry::GetInstance()->UseWebUIForURL(
            browser_context, new_effective_url)) {
      return true;
    }
  }

  // Check with the content client as well.  Important to pass
  // current_effective_url here, which uses the SiteInstance's site if there is
  // no current_entry.
  if (GetContentClient()->browser()->ShouldSwapBrowsingInstancesForNavigation(
          render_frame_host_->GetSiteInstance(),
          current_effective_url, new_effective_url)) {
    return true;
  }

  // We can't switch a RenderView between view source and non-view source mode
  // without screwing up the session history sometimes (when navigating between
  // "view-source:http://foo.com/" and "http://foo.com/", Blink doesn't treat
  // it as a new navigation). So require a BrowsingInstance switch.
  if (current_is_view_source_mode != new_is_view_source_mode)
    return true;

  return false;
}

SiteInstance* RenderFrameHostManager::GetSiteInstanceForNavigation(
    const GURL& dest_url,
    SiteInstance* source_instance,
    SiteInstance* dest_instance,
    SiteInstance* candidate_instance,
    ui::PageTransition transition,
    bool dest_is_restore,
    bool dest_is_view_source_mode) {
  SiteInstance* current_instance = render_frame_host_->GetSiteInstance();

  // We do not currently swap processes for navigations in webview tag guests.
  if (current_instance->GetSiteURL().SchemeIs(kGuestScheme))
    return current_instance;

  // Determine if we need a new BrowsingInstance for this entry.  If true, this
  // implies that it will get a new SiteInstance (and likely process), and that
  // other tabs in the current BrowsingInstance will be unable to script it.
  // This is used for cases that require a process swap even in the
  // process-per-tab model, such as WebUI pages.
  // TODO(clamy): Remove the dependency on the current entry.
  const NavigationEntry* current_entry =
      delegate_->GetLastCommittedNavigationEntryForRenderManager();
  BrowserContext* browser_context =
      delegate_->GetControllerForRenderManager().GetBrowserContext();
  const GURL& current_effective_url = current_entry ?
      SiteInstanceImpl::GetEffectiveURL(browser_context,
                                        current_entry->GetURL()) :
      render_frame_host_->GetSiteInstance()->GetSiteURL();
  bool current_is_view_source_mode = current_entry ?
      current_entry->IsViewSourceMode() : dest_is_view_source_mode;
  bool force_swap = ShouldSwapBrowsingInstancesForNavigation(
      current_effective_url,
      current_is_view_source_mode,
      dest_instance,
      SiteInstanceImpl::GetEffectiveURL(browser_context, dest_url),
      dest_is_view_source_mode);
  SiteInstanceDescriptor new_instance_descriptor =
      SiteInstanceDescriptor(current_instance);
  if (ShouldTransitionCrossSite() || force_swap) {
    new_instance_descriptor = DetermineSiteInstanceForURL(
        dest_url, source_instance, current_instance, dest_instance, transition,
        dest_is_restore, dest_is_view_source_mode, force_swap);
  }

  SiteInstance* new_instance =
      ConvertToSiteInstance(new_instance_descriptor, candidate_instance);

  // If |force_swap| is true, we must use a different SiteInstance than the
  // current one. If we didn't, we would have two RenderFrameHosts in the same
  // SiteInstance and the same frame, resulting in page_id conflicts for their
  // NavigationEntries.
  if (force_swap)
    CHECK_NE(new_instance, current_instance);
  return new_instance;
}

RenderFrameHostManager::SiteInstanceDescriptor
RenderFrameHostManager::DetermineSiteInstanceForURL(
    const GURL& dest_url,
    SiteInstance* source_instance,
    SiteInstance* current_instance,
    SiteInstance* dest_instance,
    ui::PageTransition transition,
    bool dest_is_restore,
    bool dest_is_view_source_mode,
    bool force_browsing_instance_swap) {
  SiteInstanceImpl* current_instance_impl =
      static_cast<SiteInstanceImpl*>(current_instance);
  NavigationControllerImpl& controller =
      delegate_->GetControllerForRenderManager();
  BrowserContext* browser_context = controller.GetBrowserContext();

  // If the entry has an instance already we should use it.
  if (dest_instance) {
    // If we are forcing a swap, this should be in a different BrowsingInstance.
    if (force_browsing_instance_swap) {
      CHECK(!dest_instance->IsRelatedSiteInstance(
                render_frame_host_->GetSiteInstance()));
    }
    return SiteInstanceDescriptor(dest_instance);
  }

  // If a swap is required, we need to force the SiteInstance AND
  // BrowsingInstance to be different ones, using CreateForURL.
  if (force_browsing_instance_swap)
    return SiteInstanceDescriptor(browser_context, dest_url, false);

  // (UGLY) HEURISTIC, process-per-site only:
  //
  // If this navigation is generated, then it probably corresponds to a search
  // query.  Given that search results typically lead to users navigating to
  // other sites, we don't really want to use the search engine hostname to
  // determine the site instance for this navigation.
  //
  // NOTE: This can be removed once we have a way to transition between
  //       RenderViews in response to a link click.
  //
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kProcessPerSite) &&
      ui::PageTransitionCoreTypeIs(transition, ui::PAGE_TRANSITION_GENERATED)) {
    return SiteInstanceDescriptor(current_instance_impl);
  }

  // If we haven't used our SiteInstance (and thus RVH) yet, then we can use it
  // for this entry.  We won't commit the SiteInstance to this site until the
  // navigation commits (in DidNavigate), unless the navigation entry was
  // restored or it's a Web UI as described below.
  if (!current_instance_impl->HasSite()) {
    // If we've already created a SiteInstance for our destination, we don't
    // want to use this unused SiteInstance; use the existing one.  (We don't
    // do this check if the current_instance has a site, because for now, we
    // want to compare against the current URL and not the SiteInstance's site.
    // In this case, there is no current URL, so comparing against the site is
    // ok.  See additional comments below.)
    //
    // Also, if the URL should use process-per-site mode and there is an
    // existing process for the site, we should use it.  We can call
    // GetRelatedSiteInstance() for this, which will eagerly set the site and
    // thus use the correct process.
    bool use_process_per_site =
        RenderProcessHost::ShouldUseProcessPerSite(browser_context, dest_url) &&
        RenderProcessHostImpl::GetProcessHostForSite(browser_context, dest_url);
    if (current_instance_impl->HasRelatedSiteInstance(dest_url) ||
        use_process_per_site) {
      return SiteInstanceDescriptor(browser_context, dest_url, true);
    }

    // For extensions, Web UI URLs (such as the new tab page), and apps we do
    // not want to use the |current_instance_impl| if it has no site, since it
    // will have a RenderProcessHost of PRIV_NORMAL. Create a new SiteInstance
    // for this URL instead (with the correct process type).
    if (current_instance_impl->HasWrongProcessForURL(dest_url))
      return SiteInstanceDescriptor(browser_context, dest_url, true);

    // View-source URLs must use a new SiteInstance and BrowsingInstance.
    // TODO(nasko): This is the same condition as later in the function. This
    // should be taken into account when refactoring this method as part of
    // http://crbug.com/123007.
    if (dest_is_view_source_mode)
      return SiteInstanceDescriptor(browser_context, dest_url, false);

    // If we are navigating from a blank SiteInstance to a WebUI, make sure we
    // create a new SiteInstance.
    if (WebUIControllerFactoryRegistry::GetInstance()->UseWebUIForURL(
            browser_context, dest_url)) {
      return SiteInstanceDescriptor(browser_context, dest_url, false);
    }

    // Normally the "site" on the SiteInstance is set lazily when the load
    // actually commits. This is to support better process sharing in case
    // the site redirects to some other site: we want to use the destination
    // site in the site instance.
    //
    // In the case of session restore, as it loads all the pages immediately
    // we need to set the site first, otherwise after a restore none of the
    // pages would share renderers in process-per-site.
    //
    // The embedder can request some urls never to be assigned to SiteInstance
    // through the ShouldAssignSiteForURL() content client method, so that
    // renderers created for particular chrome urls (e.g. the chrome-native://
    // scheme) can be reused for subsequent navigations in the same WebContents.
    // See http://crbug.com/386542.
    if (dest_is_restore &&
        GetContentClient()->browser()->ShouldAssignSiteForURL(dest_url)) {
      current_instance_impl->SetSite(dest_url);
    }

    return SiteInstanceDescriptor(current_instance_impl);
  }

  // Otherwise, only create a new SiteInstance for a cross-process navigation.

  // TODO(creis): Once we intercept links and script-based navigations, we
  // will be able to enforce that all entries in a SiteInstance actually have
  // the same site, and it will be safe to compare the URL against the
  // SiteInstance's site, as follows:
  // const GURL& current_url = current_instance_impl->site();
  // For now, though, we're in a hybrid model where you only switch
  // SiteInstances if you type in a cross-site URL.  This means we have to
  // compare the entry's URL to the last committed entry's URL.
  NavigationEntry* current_entry = controller.GetLastCommittedEntry();
  if (interstitial_page_) {
    // The interstitial is currently the last committed entry, but we want to
    // compare against the last non-interstitial entry.
    current_entry = controller.GetEntryAtOffset(-1);
  }

  // View-source URLs must use a new SiteInstance and BrowsingInstance.
  // We don't need a swap when going from view-source to a debug URL like
  // chrome://crash, however.
  // TODO(creis): Refactor this method so this duplicated code isn't needed.
  // See http://crbug.com/123007.
  if (current_entry &&
      current_entry->IsViewSourceMode() != dest_is_view_source_mode &&
      !IsRendererDebugURL(dest_url)) {
    return SiteInstanceDescriptor(browser_context, dest_url, false);
  }

  // Use the source SiteInstance in case of data URLs or about:blank pages,
  // because the content is then controlled and/or scriptable by the source
  // SiteInstance.
  GURL about_blank(url::kAboutBlankURL);
  if (source_instance &&
      (dest_url == about_blank || dest_url.scheme() == url::kDataScheme)) {
    return SiteInstanceDescriptor(source_instance);
  }

  // Use the current SiteInstance for same site navigations, as long as the
  // process type is correct.  (The URL may have been installed as an app since
  // the last time we visited it.)
  const GURL& current_url = GetCurrentURLForSiteInstance(current_instance_impl);
  if (SiteInstance::IsSameWebSite(browser_context, current_url, dest_url) &&
      !current_instance_impl->HasWrongProcessForURL(dest_url)) {
    return SiteInstanceDescriptor(current_instance_impl);
  }

  // Start the new renderer in a new SiteInstance, but in the current
  // BrowsingInstance.  It is important to immediately give this new
  // SiteInstance to a RenderViewHost (if it is different than our current
  // SiteInstance), so that it is ref counted.  This will happen in
  // CreateRenderView.
  return SiteInstanceDescriptor(browser_context, dest_url, true);
}

bool RenderFrameHostManager::IsRendererTransferNeededForNavigation(
    RenderFrameHostImpl* rfh,
    const GURL& dest_url) {
  // A transfer is not needed if the current SiteInstance doesn't yet have a
  // site.  This is the case for tests that use NavigateToURL.
  if (!rfh->GetSiteInstance()->HasSite())
    return false;

  // We do not currently swap processes for navigations in webview tag guests.
  if (rfh->GetSiteInstance()->GetSiteURL().SchemeIs(kGuestScheme))
    return false;

  // Don't swap processes for extensions embedded in DevTools. See
  // https://crbug.com/564216.
  if (rfh->GetSiteInstance()->GetSiteURL().SchemeIs(kChromeDevToolsScheme)) {
    // TODO(nick): https://crbug.com/570483 Check to see if |dest_url| is a
    // devtools extension, and swap processes if not.
    return false;
  }

  BrowserContext* context = rfh->GetSiteInstance()->GetBrowserContext();
  GURL effective_url = SiteInstanceImpl::GetEffectiveURL(context, dest_url);

  // TODO(nasko, nick): These following --site-per-process checks are
  // overly simplistic. Update them to match all the cases
  // considered by DetermineSiteInstanceForURL.
  if (SiteInstance::IsSameWebSite(rfh->GetSiteInstance()->GetBrowserContext(),
                                  rfh->GetSiteInstance()->GetSiteURL(),
                                  dest_url)) {
    return false;  // The same site, no transition needed.
  }

  // The sites differ. If either one requires a dedicated process,
  // then a transfer is needed.
  return rfh->GetSiteInstance()->RequiresDedicatedProcess() ||
         SiteInstanceImpl::DoesSiteRequireDedicatedProcess(context,
                                                           effective_url);
}

SiteInstance* RenderFrameHostManager::ConvertToSiteInstance(
    const SiteInstanceDescriptor& descriptor,
    SiteInstance* candidate_instance) {
  SiteInstance* current_instance = render_frame_host_->GetSiteInstance();

  // Note: If the |candidate_instance| matches the descriptor, it will already
  // be set to |descriptor.existing_site_instance|.
  if (descriptor.existing_site_instance)
    return descriptor.existing_site_instance;

  // Note: If the |candidate_instance| matches the descriptor,
  // GetRelatedSiteInstance will return it.
  if (descriptor.new_is_related_to_current)
    return current_instance->GetRelatedSiteInstance(descriptor.new_site_url);

  // At this point we know an unrelated site instance must be returned. First
  // check if the candidate matches.
  if (candidate_instance &&
      !current_instance->IsRelatedSiteInstance(candidate_instance) &&
      candidate_instance->GetSiteURL() == descriptor.new_site_url) {
    return candidate_instance;
  }

  // Otherwise return a newly created one.
  return SiteInstance::CreateForURL(
      delegate_->GetControllerForRenderManager().GetBrowserContext(),
      descriptor.new_site_url);
}

const GURL& RenderFrameHostManager::GetCurrentURLForSiteInstance(
    SiteInstance* current_instance) {
  // Use the current RenderFrameHost's last successful URL if it has one.  This
  // excludes commits of net errors, since net errors do not currently swap
  // processes for transfer navigations.  Thus, we compare against the last
  // successful commit when deciding whether to swap this time.
  // (Note: browser-initiated net errors do swap processes, but the frame's last
  // successful URL will be empty in that case, causing us to fall back to the
  // SiteInstance's URL below.)
  if (!render_frame_host_->last_successful_url().is_empty())
    return render_frame_host_->last_successful_url();

  // Fall back to the SiteInstance's Site URL if the FrameTreeNode doen't have a
  // current URL.
  return current_instance->GetSiteURL();
}

void RenderFrameHostManager::CreatePendingRenderFrameHost(
    SiteInstance* old_instance,
    SiteInstance* new_instance) {
  int create_render_frame_flags = 0;
  if (delegate_->IsHidden())
    create_render_frame_flags |= CREATE_RF_HIDDEN;

  if (pending_render_frame_host_)
    CancelPending();

  // The process for the new SiteInstance may (if we're sharing a process with
  // another host that already initialized it) or may not (we have our own
  // process or the existing process crashed) have been initialized. Calling
  // Init multiple times will be ignored, so this is safe.
  if (!new_instance->GetProcess()->Init())
    return;

  CreateProxiesForNewRenderFrameHost(old_instance, new_instance);

  // Create a non-swapped-out RFH with the given opener.
  pending_render_frame_host_ =
      CreateRenderFrame(new_instance, create_render_frame_flags, nullptr);
}

void RenderFrameHostManager::CreateProxiesForNewRenderFrameHost(
    SiteInstance* old_instance,
    SiteInstance* new_instance) {
  // Only create opener proxies if they are in the same BrowsingInstance.
  if (new_instance->IsRelatedSiteInstance(old_instance)) {
    CreateOpenerProxies(new_instance, frame_tree_node_);
  } else if (SiteIsolationPolicy::AreCrossProcessFramesPossible()) {
    // Ensure that the frame tree has RenderFrameProxyHosts for the
    // new SiteInstance in all nodes except the current one.  We do this for
    // all frames in the tree, whether they are in the same BrowsingInstance or
    // not.  If |new_instance| is in the same BrowsingInstance as
    // |old_instance|, this will be done as part of CreateOpenerProxies above;
    // otherwise, we do this here.  We will still check whether two frames are
    // in the same BrowsingInstance before we allow them to interact (e.g.,
    // postMessage).
    frame_tree_node_->frame_tree()->CreateProxiesForSiteInstance(
        frame_tree_node_, new_instance);
  }
}

void RenderFrameHostManager::CreateProxiesForNewNamedFrame() {
  if (!SiteIsolationPolicy::AreCrossProcessFramesPossible())
    return;

  DCHECK(!frame_tree_node_->frame_name().empty());

  // If this is a top-level frame, create proxies for this node in the
  // SiteInstances of its opener's ancestors, which are allowed to discover
  // this frame by name (see https://crbug.com/511474 and part 4 of
  // https://html.spec.whatwg.org/#the-rules-for-choosing-a-browsing-context-
  // given-a-browsing-context-name).
  FrameTreeNode* opener = frame_tree_node_->opener();
  if (!opener || !frame_tree_node_->IsMainFrame())
    return;
  SiteInstance* current_instance = render_frame_host_->GetSiteInstance();

  // Start from opener's parent.  There's no need to create a proxy in the
  // opener's SiteInstance, since new windows are always first opened in the
  // same SiteInstance as their opener, and if the new window navigates
  // cross-site, that proxy would be created as part of swapping out.
  for (FrameTreeNode* ancestor = opener->parent(); ancestor;
       ancestor = ancestor->parent()) {
    RenderFrameHostImpl* ancestor_rfh = ancestor->current_frame_host();
    if (ancestor_rfh->GetSiteInstance() != current_instance)
      CreateRenderFrameProxy(ancestor_rfh->GetSiteInstance());
  }
}

scoped_ptr<RenderFrameHostImpl> RenderFrameHostManager::CreateRenderFrameHost(
    SiteInstance* site_instance,
    int32_t view_routing_id,
    int32_t frame_routing_id,
    int32_t widget_routing_id,
    int flags) {
  if (frame_routing_id == MSG_ROUTING_NONE)
    frame_routing_id = site_instance->GetProcess()->GetNextRoutingID();

  bool swapped_out = !!(flags & CREATE_RF_SWAPPED_OUT);
  bool hidden = !!(flags & CREATE_RF_HIDDEN);

  // Create a RVH for main frames, or find the existing one for subframes.
  FrameTree* frame_tree = frame_tree_node_->frame_tree();
  RenderViewHostImpl* render_view_host = nullptr;
  if (frame_tree_node_->IsMainFrame()) {
    render_view_host = frame_tree->CreateRenderViewHost(
        site_instance, view_routing_id, frame_routing_id, swapped_out, hidden);
    // TODO(avi): It's a bit bizarre that this logic lives here instead of in
    // CreateRenderFrame(). It turns out that FrameTree::CreateRenderViewHost
    // doesn't /always/ create a new RenderViewHost. It first tries to find an
    // already existing one to reuse by a SiteInstance lookup. If it finds one,
    // then the supplied routing IDs are completely ignored.
    // CreateRenderFrame() could do this lookup too, but it seems redundant to
    // do this lookup in two places. This is a good yak shave to clean up, or,
    // if just ignored, should be an easy cleanup once RenderViewHostImpl has-a
    // RenderWidgetHostImpl. https://crbug.com/545684
    if (view_routing_id == MSG_ROUTING_NONE) {
      widget_routing_id = render_view_host->GetRoutingID();
    } else {
      DCHECK_EQ(view_routing_id, render_view_host->GetRoutingID());
    }
  } else {
    render_view_host = frame_tree->GetRenderViewHost(site_instance);
    CHECK(render_view_host);
  }

  return RenderFrameHostFactory::Create(
      site_instance, render_view_host, render_frame_delegate_,
      render_widget_delegate_, frame_tree, frame_tree_node_, frame_routing_id,
      widget_routing_id, flags);
}

// PlzNavigate
bool RenderFrameHostManager::CreateSpeculativeRenderFrameHost(
    SiteInstance* old_instance,
    SiteInstance* new_instance) {
  CHECK(new_instance);
  CHECK_NE(old_instance, new_instance);

  // The process for the new SiteInstance may (if we're sharing a process with
  // another host that already initialized it) or may not (we have our own
  // process or the existing process crashed) have been initialized. Calling
  // Init multiple times will be ignored, so this is safe.
  if (!new_instance->GetProcess()->Init())
    return false;

  CreateProxiesForNewRenderFrameHost(old_instance, new_instance);

  int create_render_frame_flags = 0;
  if (delegate_->IsHidden())
    create_render_frame_flags |= CREATE_RF_HIDDEN;
  speculative_render_frame_host_ =
      CreateRenderFrame(new_instance, create_render_frame_flags, nullptr);

  return !!speculative_render_frame_host_;
}

scoped_ptr<RenderFrameHostImpl> RenderFrameHostManager::CreateRenderFrame(
    SiteInstance* instance,
    int flags,
    int* view_routing_id_ptr) {
  bool swapped_out = !!(flags & CREATE_RF_SWAPPED_OUT);
  bool swapped_out_forbidden =
      SiteIsolationPolicy::IsSwappedOutStateForbidden();

  CHECK(instance);
  CHECK(!swapped_out_forbidden || !swapped_out);
  CHECK(SiteIsolationPolicy::AreCrossProcessFramesPossible() ||
        frame_tree_node_->IsMainFrame());

  // Swapped out views should always be hidden.
  DCHECK(!swapped_out || (flags & CREATE_RF_HIDDEN));

  scoped_ptr<RenderFrameHostImpl> new_render_frame_host;
  bool success = true;
  if (view_routing_id_ptr)
    *view_routing_id_ptr = MSG_ROUTING_NONE;

  // We are creating a pending, speculative or swapped out RFH here. We should
  // never create it in the same SiteInstance as our current RFH.
  CHECK_NE(render_frame_host_->GetSiteInstance(), instance);

  // Check if we've already created an RFH for this SiteInstance.  If so, try
  // to re-use the existing one, which has already been initialized.  We'll
  // remove it from the list of proxy hosts below if it will be active.
  RenderFrameProxyHost* proxy = GetRenderFrameProxyHost(instance);
  if (proxy && proxy->render_frame_host()) {
    RenderViewHost* render_view_host = proxy->GetRenderViewHost();
    CHECK(!swapped_out_forbidden);
    if (view_routing_id_ptr)
      *view_routing_id_ptr = proxy->GetRenderViewHost()->GetRoutingID();
    // Delete the existing RenderFrameProxyHost, but reuse the RenderFrameHost.
    // Prevent the process from exiting while we're trying to use it.
    if (!swapped_out) {
      new_render_frame_host = proxy->PassFrameHostOwnership();
      new_render_frame_host->GetProcess()->AddPendingView();

      DeleteRenderFrameProxyHost(instance);
      // NB |proxy| is deleted at this point.

      // If we are reusing the RenderViewHost and it doesn't already have a
      // RenderWidgetHostView, we need to create one if this is the main frame.
      if (render_view_host->IsRenderViewLive() &&
          !render_view_host->GetWidget()->GetView() &&
          frame_tree_node_->IsMainFrame()) {
        delegate_->CreateRenderWidgetHostViewForRenderManager(render_view_host);
      }
    }
  } else {
    // Create a new RenderFrameHost if we don't find an existing one.

    int32_t widget_routing_id = MSG_ROUTING_NONE;

    // A RenderFrame in a different process from its parent RenderFrame
    // requires a RenderWidget for input/layout/painting.
    if (frame_tree_node_->parent() &&
        frame_tree_node_->parent()->current_frame_host()->GetSiteInstance() !=
            instance) {
      CHECK(SiteIsolationPolicy::AreCrossProcessFramesPossible());
      widget_routing_id = instance->GetProcess()->GetNextRoutingID();
    }

    new_render_frame_host = CreateRenderFrameHost(
        instance, MSG_ROUTING_NONE, MSG_ROUTING_NONE, widget_routing_id, flags);
    RenderViewHostImpl* render_view_host =
        new_render_frame_host->render_view_host();

    // Prevent the process from exiting while we're trying to navigate in it.
    // Otherwise, if the new RFH is swapped out already, store it.
    if (!swapped_out) {
      new_render_frame_host->GetProcess()->AddPendingView();
    } else {
      proxy =
          CreateRenderFrameProxyHost(new_render_frame_host->GetSiteInstance(),
                                     new_render_frame_host->render_view_host());
      proxy->TakeFrameHostOwnership(std::move(new_render_frame_host));
    }

    if (frame_tree_node_->IsMainFrame()) {
      success = InitRenderView(render_view_host, proxy);

      // If we are reusing the RenderViewHost and it doesn't already have a
      // RenderWidgetHostView, we need to create one if this is the main frame.
      if (!swapped_out && !render_view_host->GetWidget()->GetView())
        delegate_->CreateRenderWidgetHostViewForRenderManager(render_view_host);
    } else {
      DCHECK(render_view_host->IsRenderViewLive());
    }

    if (success) {
      if (frame_tree_node_->IsMainFrame()) {
        // Don't show the main frame's view until we get a DidNavigate from it.
        // Only the RenderViewHost for the top-level RenderFrameHost has a
        // RenderWidgetHostView; RenderWidgetHosts for out-of-process iframes
        // will be created later and hidden.
        if (render_view_host->GetWidget()->GetView())
          render_view_host->GetWidget()->GetView()->Hide();
      }
      // RenderViewHost for |instance| might exist prior to calling
      // CreateRenderFrame. In such a case, InitRenderView will not create the
      // RenderFrame in the renderer process and it needs to be done
      // explicitly.
      if (swapped_out_forbidden) {
        // Init the RFH, so a RenderFrame is created in the renderer.
        DCHECK(new_render_frame_host);
        success = InitRenderFrame(new_render_frame_host.get());
      }
    }

    if (success) {
      if (view_routing_id_ptr)
        *view_routing_id_ptr = render_view_host->GetRoutingID();
    }
  }

  // Returns the new RFH if it isn't swapped out.
  if (success && !swapped_out) {
    DCHECK(new_render_frame_host->GetSiteInstance() == instance);
    return new_render_frame_host;
  }
  return nullptr;
}

int RenderFrameHostManager::CreateRenderFrameProxy(SiteInstance* instance) {
  // A RenderFrameProxyHost should never be created in the same SiteInstance as
  // the current RFH.
  CHECK(instance);
  CHECK_NE(instance, render_frame_host_->GetSiteInstance());

  RenderViewHostImpl* render_view_host = nullptr;

  // Ensure a RenderViewHost exists for |instance|, as it creates the page
  // level structure in Blink.
  if (SiteIsolationPolicy::IsSwappedOutStateForbidden()) {
    render_view_host =
        frame_tree_node_->frame_tree()->GetRenderViewHost(instance);
    if (!render_view_host) {
      CHECK(frame_tree_node_->IsMainFrame());
      render_view_host = frame_tree_node_->frame_tree()->CreateRenderViewHost(
          instance, MSG_ROUTING_NONE, MSG_ROUTING_NONE, true, true);
    }
  }

  RenderFrameProxyHost* proxy = GetRenderFrameProxyHost(instance);
  if (proxy && proxy->is_render_frame_proxy_live())
    return proxy->GetRoutingID();

  if (!proxy)
    proxy = CreateRenderFrameProxyHost(instance, render_view_host);

  if (SiteIsolationPolicy::IsSwappedOutStateForbidden() &&
      frame_tree_node_->IsMainFrame()) {
    InitRenderView(render_view_host, proxy);
  } else {
    proxy->InitRenderFrameProxy();
  }

  return proxy->GetRoutingID();
}

void RenderFrameHostManager::CreateProxiesForChildFrame(FrameTreeNode* child) {
  RenderFrameProxyHost* outer_delegate_proxy =
      ForInnerDelegate() ? GetProxyToOuterDelegate() : nullptr;
  for (const auto& pair : proxy_hosts_) {
    // Do not create proxies for subframes in the outer delegate's process,
    // since the outer delegate does not need to interact with them.
    if (pair.second.get() == outer_delegate_proxy)
      continue;

    child->render_manager()->CreateRenderFrameProxy(
        pair.second->GetSiteInstance());
  }
}

void RenderFrameHostManager::EnsureRenderViewInitialized(
    RenderViewHostImpl* render_view_host,
    SiteInstance* instance) {
  DCHECK(frame_tree_node_->IsMainFrame());

  if (render_view_host->IsRenderViewLive())
    return;

  // If the proxy in |instance| doesn't exist, this RenderView is not swapped
  // out and shouldn't be reinitialized here.
  RenderFrameProxyHost* proxy = GetRenderFrameProxyHost(instance);
  if (!proxy)
    return;

  InitRenderView(render_view_host, proxy);
}

void RenderFrameHostManager::CreateOuterDelegateProxy(
    SiteInstance* outer_contents_site_instance,
    RenderFrameHostImpl* render_frame_host) {
  CHECK(BrowserPluginGuestMode::UseCrossProcessFramesForGuests());
  RenderFrameProxyHost* proxy =
      CreateRenderFrameProxyHost(outer_contents_site_instance, nullptr);

  // Swap the outer WebContents's frame with the proxy to inner WebContents.
  //
  // We are in the outer WebContents, and its FrameTree would never see
  // a load start for any of its inner WebContents. Eventually, that also makes
  // the FrameTree never see the matching load stop. Therefore, we always pass
  // false to |is_loading| below.
  // TODO(lazyboy): This |is_loading| behavior might not be what we want,
  // investigate and fix.
  render_frame_host->Send(new FrameMsg_SwapOut(
      render_frame_host->GetRoutingID(), proxy->GetRoutingID(),
      false /* is_loading */,
      render_frame_host->frame_tree_node()->current_replication_state()));
  proxy->set_render_frame_proxy_created(true);
}

void RenderFrameHostManager::SetRWHViewForInnerContents(
    RenderWidgetHostView* child_rwhv) {
  DCHECK(ForInnerDelegate() && frame_tree_node_->IsMainFrame());
  GetProxyToOuterDelegate()->SetChildRWHView(child_rwhv);
}

bool RenderFrameHostManager::InitRenderView(
    RenderViewHostImpl* render_view_host,
    RenderFrameProxyHost* proxy) {
  // Ensure the renderer process is initialized before creating the
  // RenderView.
  if (!render_view_host->GetProcess()->Init())
    return false;

  // We may have initialized this RenderViewHost for another RenderFrameHost.
  if (render_view_host->IsRenderViewLive())
    return true;

  int opener_frame_routing_id =
      GetOpenerRoutingID(render_view_host->GetSiteInstance());

  bool created = delegate_->CreateRenderViewForRenderManager(
      render_view_host, opener_frame_routing_id,
      proxy ? proxy->GetRoutingID() : MSG_ROUTING_NONE,
      frame_tree_node_->current_replication_state());

  if (created && proxy)
    proxy->set_render_frame_proxy_created(true);

  return created;
}

bool RenderFrameHostManager::InitRenderFrame(
    RenderFrameHostImpl* render_frame_host) {
  if (render_frame_host->IsRenderFrameLive())
    return true;

  SiteInstance* site_instance = render_frame_host->GetSiteInstance();

  int opener_routing_id = MSG_ROUTING_NONE;
  if (frame_tree_node_->opener())
    opener_routing_id = GetOpenerRoutingID(site_instance);

  int parent_routing_id = MSG_ROUTING_NONE;
  if (frame_tree_node_->parent()) {
    parent_routing_id = frame_tree_node_->parent()
                            ->render_manager()
                            ->GetRoutingIdForSiteInstance(site_instance);
    CHECK_NE(parent_routing_id, MSG_ROUTING_NONE);
  }

  // At this point, all RenderFrameProxies for sibling frames have already been
  // created, including any proxies that come after this frame.  To preserve
  // correct order for indexed window access (e.g., window.frames[1]), pass the
  // previous sibling frame so that this frame is correctly inserted into the
  // frame tree on the renderer side.
  int previous_sibling_routing_id = MSG_ROUTING_NONE;
  FrameTreeNode* previous_sibling = frame_tree_node_->PreviousSibling();
  if (previous_sibling) {
    previous_sibling_routing_id =
        previous_sibling->render_manager()->GetRoutingIdForSiteInstance(
            site_instance);
    CHECK_NE(previous_sibling_routing_id, MSG_ROUTING_NONE);
  }

  // Check whether there is an existing proxy for this frame in this
  // SiteInstance. If there is, the new RenderFrame needs to be able to find
  // the proxy it is replacing, so that it can fully initialize itself.
  // NOTE: This is the only time that a RenderFrameProxyHost can be in the same
  // SiteInstance as its RenderFrameHost. This is only the case until the
  // RenderFrameHost commits, at which point it will replace and delete the
  // RenderFrameProxyHost.
  int proxy_routing_id = MSG_ROUTING_NONE;
  RenderFrameProxyHost* existing_proxy = GetRenderFrameProxyHost(site_instance);
  if (existing_proxy) {
    proxy_routing_id = existing_proxy->GetRoutingID();
    CHECK_NE(proxy_routing_id, MSG_ROUTING_NONE);
    if (!existing_proxy->is_render_frame_proxy_live()) {
      // Calling InitRenderFrameProxy on main frames seems to be causing
      // https://crbug.com/575245, so track down how this can happen.
      // TODO(creis): Remove this once we've found the cause.
      if (SiteIsolationPolicy::IsSwappedOutStateForbidden() &&
          !frame_tree_node_->parent()) {
        base::debug::SetCrashKeyValue(
            "initrf_frame_id",
            base::IntToString(render_frame_host->GetRoutingID()));
        base::debug::SetCrashKeyValue("initrf_proxy_id",
                                      base::IntToString(proxy_routing_id));
        base::debug::SetCrashKeyValue(
            "initrf_view_id",
            base::IntToString(
                render_frame_host->render_view_host()->GetRoutingID()));
        base::debug::SetCrashKeyValue(
            "initrf_main_frame_id",
            base::IntToString(render_frame_host->render_view_host()
                                  ->main_frame_routing_id()));
        base::debug::SetCrashKeyValue(
            "initrf_view_is_live",
            render_frame_host->render_view_host()->IsRenderViewLive() ? "yes"
                                                                      : "no");
        base::debug::DumpWithoutCrashing();
      }

      existing_proxy->InitRenderFrameProxy();
    }
  }

  return delegate_->CreateRenderFrameForRenderManager(
      render_frame_host, proxy_routing_id, opener_routing_id, parent_routing_id,
      previous_sibling_routing_id);
}

int RenderFrameHostManager::GetRoutingIdForSiteInstance(
    SiteInstance* site_instance) {
  if (render_frame_host_->GetSiteInstance() == site_instance)
    return render_frame_host_->GetRoutingID();

  // If there is a matching pending RFH, only return it if swapped out is
  // allowed, since otherwise there should be a proxy that should be used
  // instead.
  if (pending_render_frame_host_ &&
      pending_render_frame_host_->GetSiteInstance() == site_instance &&
      !SiteIsolationPolicy::IsSwappedOutStateForbidden())
    return pending_render_frame_host_->GetRoutingID();

  RenderFrameProxyHost* proxy = GetRenderFrameProxyHost(site_instance);
  if (proxy)
    return proxy->GetRoutingID();

  return MSG_ROUTING_NONE;
}

void RenderFrameHostManager::CommitPendingWebUI() {
  TRACE_EVENT1("navigation", "RenderFrameHostManager::CommitPendingWebUI",
               "FrameTreeNode id", frame_tree_node_->frame_tree_node_id());
  DCHECK(render_frame_host_->pending_web_ui());

  // First check whether we're going to want to focus the location bar after
  // this commit.  We do this now because the navigation hasn't formally
  // committed yet, so if we've already cleared the pending WebUI the call chain
  // this triggers won't be able to figure out what's going on.
  bool will_focus_location_bar = delegate_->FocusLocationBarByDefault();

  render_frame_host_->CommitPendingWebUI();

  if (will_focus_location_bar)
    delegate_->SetFocusToLocationBar(false);
}

void RenderFrameHostManager::CommitPending() {
  TRACE_EVENT1("navigation", "RenderFrameHostManager::CommitPending",
               "FrameTreeNode id", frame_tree_node_->frame_tree_node_id());
  DCHECK(pending_render_frame_host_ || speculative_render_frame_host_);

  // First check whether we're going to want to focus the location bar after
  // this commit.  We do this now because the navigation hasn't formally
  // committed yet, so if we've already cleared the pending WebUI the call chain
  // this triggers won't be able to figure out what's going on.
  bool will_focus_location_bar = delegate_->FocusLocationBarByDefault();

  // Remember if the page was focused so we can focus the new renderer in
  // that case.
  bool focus_render_view = !will_focus_location_bar &&
                           render_frame_host_->GetView() &&
                           render_frame_host_->GetView()->HasFocus();

  bool is_main_frame = frame_tree_node_->IsMainFrame();

  // While the old frame is still current, remove its children from the tree.
  frame_tree_node_->ResetForNewProcess();

  // Swap in the pending or speculative frame and make it active. Also ensure
  // the FrameTree stays in sync.
  scoped_ptr<RenderFrameHostImpl> old_render_frame_host;
  if (!IsBrowserSideNavigationEnabled()) {
    DCHECK(!speculative_render_frame_host_);
    old_render_frame_host =
        SetRenderFrameHost(std::move(pending_render_frame_host_));
  } else {
    // PlzNavigate
    DCHECK(speculative_render_frame_host_);
    old_render_frame_host =
        SetRenderFrameHost(std::move(speculative_render_frame_host_));
  }

  // The process will no longer try to exit, so we can decrement the count.
  render_frame_host_->GetProcess()->RemovePendingView();

  // Show the new view (or a sad tab) if necessary.
  bool new_rfh_has_view = !!render_frame_host_->GetView();
  if (!delegate_->IsHidden() && new_rfh_has_view) {
    // In most cases, we need to show the new view.
    render_frame_host_->GetView()->Show();
  }
  if (!new_rfh_has_view) {
    // If the view is gone, then this RenderViewHost died while it was hidden.
    // We ignored the RenderProcessGone call at the time, so we should send it
    // now to make sure the sad tab shows up, etc.
    DCHECK(!render_frame_host_->IsRenderFrameLive());
    DCHECK(!render_frame_host_->render_view_host()->IsRenderViewLive());
    render_frame_host_->ResetLoadingState();
    delegate_->RenderProcessGoneFromRenderManager(
        render_frame_host_->render_view_host());
  }

  // For top-level frames, also hide the old RenderViewHost's view.
  // TODO(creis): As long as show/hide are on RVH, we don't want to hide on
  // subframe navigations or we will interfere with the top-level frame.
  if (is_main_frame &&
      old_render_frame_host->render_view_host()->GetWidget()->GetView()) {
    old_render_frame_host->render_view_host()->GetWidget()->GetView()->Hide();
  }

  // Make sure the size is up to date.  (Fix for bug 1079768.)
  delegate_->UpdateRenderViewSizeForRenderManager();

  if (will_focus_location_bar) {
    delegate_->SetFocusToLocationBar(false);
  } else if (focus_render_view && render_frame_host_->GetView()) {
    if (is_main_frame) {
      render_frame_host_->GetView()->Focus();
    } else {
      // The main frame's view is already focused, but we need to set
      // page-level focus in the subframe's renderer.
      frame_tree_node_->frame_tree()->SetPageFocus(
          render_frame_host_->GetSiteInstance(), true);
    }
  }

  // Notify that we've swapped RenderFrameHosts. We do this before shutting down
  // the RFH so that we can clean up RendererResources related to the RFH first.
  delegate_->NotifySwappedFromRenderManager(
      old_render_frame_host.get(), render_frame_host_.get(), is_main_frame);

  // The RenderViewHost keeps track of the main RenderFrameHost routing id.
  // If this is committing a main frame navigation, update it and set the
  // routing id in the RenderViewHost associated with the old RenderFrameHost
  // to MSG_ROUTING_NONE.
  if (is_main_frame && SiteIsolationPolicy::IsSwappedOutStateForbidden()) {
    render_frame_host_->render_view_host()->set_main_frame_routing_id(
        render_frame_host_->routing_id());
    old_render_frame_host->render_view_host()->set_main_frame_routing_id(
        MSG_ROUTING_NONE);
  }

  // Swap out the old frame now that the new one is visible.
  // This will swap it out and then put it on the proxy list (if there are other
  // active views in its SiteInstance) or schedule it for deletion when the swap
  // out ack arrives (or immediately if the process isn't live).
  // In the --site-per-process case, old subframe RFHs are not kept alive inside
  // the proxy.
  SwapOutOldFrame(std::move(old_render_frame_host));

  if (SiteIsolationPolicy::IsSwappedOutStateForbidden()) {
    // Since the new RenderFrameHost is now committed, there must be no proxies
    // for its SiteInstance. Delete any existing ones.
    DeleteRenderFrameProxyHost(render_frame_host_->GetSiteInstance());
  }

  // If this is a subframe, it should have a CrossProcessFrameConnector
  // created already.  Use it to link the new RFH's view to the proxy that
  // belongs to the parent frame's SiteInstance. If this navigation causes
  // an out-of-process frame to return to the same process as its parent, the
  // proxy would have been removed from proxy_hosts_ above.
  // Note: We do this after swapping out the old RFH because that may create
  // the proxy we're looking for.
  RenderFrameProxyHost* proxy_to_parent = GetProxyToParent();
  if (proxy_to_parent) {
    CHECK(SiteIsolationPolicy::AreCrossProcessFramesPossible());
    proxy_to_parent->SetChildRWHView(render_frame_host_->GetView());
  }

  // After all is done, there must never be a proxy in the list which has the
  // same SiteInstance as the current RenderFrameHost.
  CHECK(!GetRenderFrameProxyHost(render_frame_host_->GetSiteInstance()));
}

RenderFrameHostImpl* RenderFrameHostManager::UpdateStateForNavigate(
    const GURL& dest_url,
    SiteInstance* source_instance,
    SiteInstance* dest_instance,
    ui::PageTransition transition,
    bool dest_is_restore,
    bool dest_is_view_source_mode,
    const GlobalRequestID& transferred_request_id,
    int bindings) {
  if (!frame_tree_node_->IsMainFrame()) {
    // Don't swap for subframes unless we are in an OOPIF-enabled mode.  We can
    // get here in tests for subframes (e.g., NavigateFrameToURL).
    if (!SiteIsolationPolicy::AreCrossProcessFramesPossible())
      return render_frame_host_.get();

    // If dest_url is a unique origin like about:blank, then the need for a swap
    // is determined by the source_instance.
    GURL resolved_url = dest_url;
    if (url::Origin(resolved_url).unique()) {
      // If there is no source_instance for a unique origin, then we should
      // avoid a process swap.
      if (!source_instance)
        return render_frame_host_.get();

      // Use source_instance to determine if a swap is needed.
      resolved_url = source_instance->GetSiteURL();
    }

    // If we are in an OOPIF mode that only applies to some sites, only swap if
    // the policy determines that a transfer would have been needed.  We can get
    // here for session restore.
    if (!IsRendererTransferNeededForNavigation(render_frame_host_.get(),
                                               resolved_url)) {
      DCHECK(!dest_instance ||
             dest_instance == render_frame_host_->GetSiteInstance());
      return render_frame_host_.get();
    }
  }

  SiteInstance* current_instance = render_frame_host_->GetSiteInstance();
  scoped_refptr<SiteInstance> new_instance = GetSiteInstanceForNavigation(
      dest_url, source_instance, dest_instance, nullptr, transition,
      dest_is_restore, dest_is_view_source_mode);

  // If we are currently navigating cross-process to a pending RFH for a
  // different SiteInstance, we want to get back to normal and then navigate as
  // usual.  We will reuse the pending RFH below if it matches the destination
  // SiteInstance.
  if (pending_render_frame_host_ &&
      pending_render_frame_host_->GetSiteInstance() != new_instance)
    CancelPending();

  if (new_instance.get() != current_instance) {
    TRACE_EVENT_INSTANT2(
        "navigation",
        "RenderFrameHostManager::UpdateStateForNavigate:New SiteInstance",
        TRACE_EVENT_SCOPE_THREAD,
        "current_instance id", current_instance->GetId(),
        "new_instance id", new_instance->GetId());

    // New SiteInstance: create a pending RFH to navigate.

    if (!pending_render_frame_host_)
      CreatePendingRenderFrameHost(current_instance, new_instance.get());
    DCHECK(pending_render_frame_host_);
    if (!pending_render_frame_host_)
      return nullptr;
    DCHECK_EQ(new_instance, pending_render_frame_host_->GetSiteInstance());

    pending_render_frame_host_->UpdatePendingWebUI(dest_url, bindings);
    pending_render_frame_host_->CommitPendingWebUI();
    DCHECK_EQ(GetNavigatingWebUI(), pending_render_frame_host_->web_ui());

    // If a WebUI exists in the pending RenderFrameHost it was just created, as
    // well as the RenderView, and they never interacted. So notify it using
    // RenderViewCreated.
    if (pending_render_frame_host_->web_ui()) {
      pending_render_frame_host_->web_ui()->RenderViewCreated(
          pending_render_frame_host_->render_view_host());
    }

    // Check if our current RFH is live before we set up a transition.
    if (!render_frame_host_->IsRenderFrameLive()) {
      // The current RFH is not live.  There's no reason to sit around with a
      // sad tab or a newly created RFH while we wait for the pending RFH to
      // navigate.  Just switch to the pending RFH now and go back to normal.
      // (Note that we don't care about on{before}unload handlers if the current
      // RFH isn't live.)
      CommitPending();
      return render_frame_host_.get();
    }
    // Otherwise, it's safe to treat this as a pending cross-process transition.

    bool is_transfer = transferred_request_id != GlobalRequestID();
    if (is_transfer) {
      // We don't need to stop the old renderer or run beforeunload/unload
      // handlers, because those have already been done.
      DCHECK(cross_site_transferring_request_->request_id() ==
             transferred_request_id);
    } else if (!pending_render_frame_host_->are_navigations_suspended()) {
      // If the pending RFH hasn't already been suspended from a previous
      // attempt to navigate it, then we need to wait for the beforeunload
      // handler to run.  Suspend navigations in the pending RFH until we hear
      // back from the old RFH's beforeunload handler (via OnBeforeUnloadACK or
      // a timeout).  If the handler returns false, we'll have to cancel the
      // request.
      //
      // Also make sure the old render view stops, in case a load is in
      // progress.  (We don't want to do this for transfers, since it will
      // interrupt the transfer with an unexpected DidStopLoading.)
      render_frame_host_->Send(new FrameMsg_Stop(
          render_frame_host_->GetRoutingID()));
      pending_render_frame_host_->SetNavigationsSuspended(true,
                                                          base::TimeTicks());
      render_frame_host_->DispatchBeforeUnload(true);
    }

    return pending_render_frame_host_.get();
  }

  // Otherwise the same SiteInstance can be used.  Navigate render_frame_host_.

  // It's possible to swap out the current RFH and then decide to navigate in it
  // anyway (e.g., a cross-process navigation that redirects back to the
  // original site).  In that case, we have a proxy for the current RFH but
  // haven't deleted it yet.  The new navigation will swap it back in, so we can
  // delete the proxy.
  DeleteRenderFrameProxyHost(new_instance.get());

  UpdatePendingWebUIOnCurrentFrameHost(dest_url, bindings);

  // The renderer can exit view source mode when any error or cancellation
  // happen. We must overwrite to recover the mode.
  if (dest_is_view_source_mode) {
    render_frame_host_->render_view_host()->Send(
        new ViewMsg_EnableViewSourceMode(
            render_frame_host_->render_view_host()->GetRoutingID()));
  }

  return render_frame_host_.get();
}

void RenderFrameHostManager::UpdatePendingWebUIOnCurrentFrameHost(
    const GURL& dest_url,
    int entry_bindings) {
  bool pending_webui_changed =
      render_frame_host_->UpdatePendingWebUI(dest_url, entry_bindings);
  DCHECK_EQ(GetNavigatingWebUI(), render_frame_host_->pending_web_ui());

  if (render_frame_host_->pending_web_ui() && pending_webui_changed &&
      render_frame_host_->IsRenderFrameLive()) {
    // If a pending WebUI exists in the current RenderFrameHost and it has been
    // updated and the associated RenderFrame is alive, notify the WebUI about
    // the RenderView.
    // Note: If the RenderFrame is not alive at this point the notification will
    // happen later, when the RenderView is created.
    if (render_frame_host_->pending_web_ui() == render_frame_host_->web_ui()) {
      // If the active WebUI is being reused it has already interacting with
      // this RenderView in the past, so call RenderViewReused.
      render_frame_host_->pending_web_ui()->RenderViewReused(
          render_frame_host_->render_view_host(),
          frame_tree_node_->IsMainFrame());
    } else {
      // If this is a new WebUI it has never interacted with the existing
      // RenderView so call RenderViewCreated.
      render_frame_host_->pending_web_ui()->RenderViewCreated(
          render_frame_host_->render_view_host());
    }
  }
}

void RenderFrameHostManager::CancelPending() {
  TRACE_EVENT1("navigation", "RenderFrameHostManager::CancelPending",
               "FrameTreeNode id", frame_tree_node_->frame_tree_node_id());
  render_frame_host_->ClearPendingWebUI();

  bool pending_was_loading = pending_render_frame_host_->is_loading();
  DiscardUnusedFrame(UnsetPendingRenderFrameHost());
  if (pending_was_loading)
    frame_tree_node_->DidStopLoading();
}

scoped_ptr<RenderFrameHostImpl>
RenderFrameHostManager::UnsetPendingRenderFrameHost() {
  scoped_ptr<RenderFrameHostImpl> pending_render_frame_host =
      std::move(pending_render_frame_host_);

  RenderFrameDevToolsAgentHost::OnCancelPendingNavigation(
      pending_render_frame_host.get(),
      render_frame_host_.get());

  // We no longer need to prevent the process from exiting.
  pending_render_frame_host->GetProcess()->RemovePendingView();

  return pending_render_frame_host;
}

scoped_ptr<RenderFrameHostImpl> RenderFrameHostManager::SetRenderFrameHost(
    scoped_ptr<RenderFrameHostImpl> render_frame_host) {
  // Swap the two.
  scoped_ptr<RenderFrameHostImpl> old_render_frame_host =
      std::move(render_frame_host_);
  render_frame_host_ = std::move(render_frame_host);

  if (frame_tree_node_->IsMainFrame()) {
    // Update the count of top-level frames using this SiteInstance.  All
    // subframes are in the same BrowsingInstance as the main frame, so we only
    // count top-level ones.  This makes the value easier for consumers to
    // interpret.
    if (render_frame_host_) {
      render_frame_host_->GetSiteInstance()->
          IncrementRelatedActiveContentsCount();
    }
    if (old_render_frame_host) {
      old_render_frame_host->GetSiteInstance()->
          DecrementRelatedActiveContentsCount();
    }
  }

  return old_render_frame_host;
}

bool RenderFrameHostManager::IsRVHOnSwappedOutList(
    RenderViewHostImpl* rvh) const {
  RenderFrameProxyHost* proxy = GetRenderFrameProxyHost(rvh->GetSiteInstance());
  if (!proxy)
    return false;
  // If there is a proxy without RFH, it is for a subframe in the SiteInstance
  // of |rvh|. Subframes should be ignored in this case.
  if (!proxy->render_frame_host())
    return false;
  return IsOnSwappedOutList(proxy->render_frame_host());
}

bool RenderFrameHostManager::IsOnSwappedOutList(
    RenderFrameHostImpl* rfh) const {
  if (!rfh->GetSiteInstance())
    return false;

  RenderFrameProxyHost* host = GetRenderFrameProxyHost(rfh->GetSiteInstance());
  if (!host)
    return false;

  return host->render_frame_host() == rfh;
}

RenderViewHostImpl* RenderFrameHostManager::GetSwappedOutRenderViewHost(
   SiteInstance* instance) const {
  RenderFrameProxyHost* proxy = GetRenderFrameProxyHost(instance);
  if (proxy)
    return proxy->GetRenderViewHost();
  return nullptr;
}

RenderFrameProxyHost* RenderFrameHostManager::GetRenderFrameProxyHost(
    SiteInstance* instance) const {
  auto it = proxy_hosts_.find(instance->GetId());
  if (it != proxy_hosts_.end())
    return it->second.get();
  return nullptr;
}

int RenderFrameHostManager::GetProxyCount() {
  return proxy_hosts_.size();
}

void RenderFrameHostManager::CollectOpenerFrameTrees(
    std::vector<FrameTree*>* opener_frame_trees,
    base::hash_set<FrameTreeNode*>* nodes_with_back_links) {
  CHECK(opener_frame_trees);
  opener_frame_trees->push_back(frame_tree_node_->frame_tree());

  // Add the FrameTree of the given node's opener to the list of
  // |opener_frame_trees| if it doesn't exist there already. |visited_index|
  // indicates which FrameTrees in |opener_frame_trees| have already been
  // visited (i.e., those at indices less than |visited_index|).
  // |nodes_with_back_links| collects FrameTreeNodes with openers in FrameTrees
  // that have already been visited (such as those with cycles).
  size_t visited_index = 0;
  while (visited_index < opener_frame_trees->size()) {
    FrameTree* frame_tree = (*opener_frame_trees)[visited_index];
    visited_index++;
    for (FrameTreeNode* node : frame_tree->Nodes()) {
      if (!node->opener())
        continue;

      FrameTree* opener_tree = node->opener()->frame_tree();
      const auto& existing_tree_it = std::find(
          opener_frame_trees->begin(), opener_frame_trees->end(), opener_tree);

      if (existing_tree_it == opener_frame_trees->end()) {
        // This is a new opener tree that we will need to process.
        opener_frame_trees->push_back(opener_tree);
      } else {
        // If this tree is already on our processing list *and* we have visited
        // it,
        // then this node's opener is a back link.  This means the node will
        // need
        // special treatment to process its opener.
        size_t position =
            std::distance(opener_frame_trees->begin(), existing_tree_it);
        if (position < visited_index)
          nodes_with_back_links->insert(node);
      }
    }
  }
}

void RenderFrameHostManager::CreateOpenerProxies(
    SiteInstance* instance,
    FrameTreeNode* skip_this_node) {
  std::vector<FrameTree*> opener_frame_trees;
  base::hash_set<FrameTreeNode*> nodes_with_back_links;

  CollectOpenerFrameTrees(&opener_frame_trees, &nodes_with_back_links);

  // Create opener proxies for frame trees, processing furthest openers from
  // this node first and this node last.  In the common case without cycles,
  // this will ensure that each tree's openers are created before the tree's
  // nodes need to reference them.
  for (int i = opener_frame_trees.size() - 1; i >= 0; i--) {
    opener_frame_trees[i]
        ->root()
        ->render_manager()
        ->CreateOpenerProxiesForFrameTree(instance, skip_this_node);
  }

  // Set openers for nodes in |nodes_with_back_links| in a second pass.
  // The proxies created at these FrameTreeNodes in
  // CreateOpenerProxiesForFrameTree won't have their opener routing ID
  // available when created due to cycles or back links in the opener chain.
  // They must have their openers updated as a separate step after proxy
  // creation.
  for (const auto& node : nodes_with_back_links) {
    RenderFrameProxyHost* proxy =
        node->render_manager()->GetRenderFrameProxyHost(instance);
    // If there is no proxy, the cycle may involve nodes in the same process,
    // or, if this is a subframe, --site-per-process may be off.  Either way,
    // there's nothing more to do.
    if (!proxy)
      continue;

    int opener_routing_id =
        node->render_manager()->GetOpenerRoutingID(instance);
    DCHECK_NE(opener_routing_id, MSG_ROUTING_NONE);
    proxy->Send(new FrameMsg_UpdateOpener(proxy->GetRoutingID(),
                                          opener_routing_id));
  }
}

void RenderFrameHostManager::CreateOpenerProxiesForFrameTree(
    SiteInstance* instance,
    FrameTreeNode* skip_this_node) {
  // Currently, this function is only called on main frames.  It should
  // actually work correctly for subframes as well, so if that need ever
  // arises, it should be sufficient to remove this DCHECK.
  DCHECK(frame_tree_node_->IsMainFrame());

  if (frame_tree_node_ == skip_this_node)
    return;

  FrameTree* frame_tree = frame_tree_node_->frame_tree();
  if (SiteIsolationPolicy::AreCrossProcessFramesPossible()) {
    // Ensure that all the nodes in the opener's FrameTree have
    // RenderFrameProxyHosts for the new SiteInstance.  Only pass the node to
    // be skipped if it's in the same FrameTree.
    if (skip_this_node && skip_this_node->frame_tree() != frame_tree)
      skip_this_node = nullptr;
    frame_tree->CreateProxiesForSiteInstance(skip_this_node, instance);
  } else {
    // If any of the RenderViewHosts (current, pending, or swapped out) for this
    // FrameTree has the same SiteInstance, then we can return early and reuse
    // them.  An exception is if we are in IsSwappedOutStateForbidden mode and
    // find a pending RenderViewHost: in this case, we should still create a
    // proxy, which will allow communicating with the opener until the pending
    // RenderView commits, or if the pending navigation is canceled.
    RenderViewHostImpl* rvh = frame_tree->GetRenderViewHost(instance);
    bool need_proxy_for_pending_rvh =
        SiteIsolationPolicy::IsSwappedOutStateForbidden() &&
        (rvh == pending_render_view_host());
    if (rvh && rvh->IsRenderViewLive() && !need_proxy_for_pending_rvh)
      return;

    if (rvh && !rvh->IsRenderViewLive()) {
      EnsureRenderViewInitialized(rvh, instance);
    } else {
      // Create a swapped out RenderView in the given SiteInstance if none
      // exists. Since an opener can point to a subframe, do this on the root
      // frame of the current opener's frame tree.
      if (SiteIsolationPolicy::IsSwappedOutStateForbidden()) {
        frame_tree->root()->render_manager()->CreateRenderFrameProxy(instance);
      } else {
        frame_tree->root()->render_manager()->CreateRenderFrame(
            instance, CREATE_RF_SWAPPED_OUT | CREATE_RF_HIDDEN, nullptr);
      }
    }
  }
}

int RenderFrameHostManager::GetOpenerRoutingID(SiteInstance* instance) {
  if (!frame_tree_node_->opener())
    return MSG_ROUTING_NONE;

  return frame_tree_node_->opener()
      ->render_manager()
      ->GetRoutingIdForSiteInstance(instance);
}

}  // namespace content
