// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/site_per_process_browsertest.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <vector>

#include "base/command_line.h"
#include "base/location.h"
#include "base/macros.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/pattern.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/test_timeouts.h"
#include "base/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "content/browser/frame_host/cross_process_frame_connector.h"
#include "content/browser/frame_host/frame_tree.h"
#include "content/browser/frame_host/navigator.h"
#include "content/browser/frame_host/render_frame_proxy_host.h"
#include "content/browser/frame_host/render_widget_host_view_child_frame.h"
#include "content/browser/gpu/compositor_util.h"
#include "content/browser/renderer_host/render_view_host_impl.h"
#include "content/browser/renderer_host/render_widget_host_input_event_router.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/common/frame_messages.h"
#include "content/common/view_messages.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_types.h"
#include "content/public/browser/resource_dispatcher_host.h"
#include "content/public/common/browser_side_navigation_policy.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/url_constants.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "content/public/test/test_utils.h"
#include "content/shell/browser/shell.h"
#include "content/test/content_browser_test_utils_internal.h"
#include "content/test/test_frame_navigation_observer.h"
#include "ipc/ipc_security_test_util.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "third_party/WebKit/public/web/WebInputEvent.h"
#include "third_party/WebKit/public/web/WebSandboxFlags.h"
#include "ui/gfx/switches.h"

#if defined(OS_MACOSX)
#include "ui/base/test/scoped_preferred_scroller_style_mac.h"
#endif

namespace content {

namespace {

// Helper function to send a postMessage and wait for a reply message.  The
// |post_message_script| is executed on the |sender_ftn| frame, and the sender
// frame is expected to post |reply_status| from the DOMAutomationController
// when it receives a reply.
void PostMessageAndWaitForReply(FrameTreeNode* sender_ftn,
                                const std::string& post_message_script,
                                const std::string& reply_status) {
  // Subtle: msg_queue needs to be declared before the ExecuteScript below, or
  // else it might miss the message of interest.  See https://crbug.com/518729.
  DOMMessageQueue msg_queue;

  bool success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      sender_ftn->current_frame_host(),
      "window.domAutomationController.send(" + post_message_script + ");",
      &success));
  EXPECT_TRUE(success);

  std::string status;
  while (msg_queue.WaitForMessage(&status)) {
    if (status == reply_status)
      break;
  }
}

// Helper function to extract and return "window.receivedMessages" from the
// |sender_ftn| frame.  This variable is used in post_message.html to count the
// number of messages received via postMessage by the current window.
int GetReceivedMessages(FrameTreeNode* ftn) {
  int received_messages = 0;
  EXPECT_TRUE(ExecuteScriptAndExtractInt(
      ftn->current_frame_host(),
      "window.domAutomationController.send(window.receivedMessages);",
      &received_messages));
  return received_messages;
}

// Helper function to perform a window.open from the |caller_frame| targeting a
// frame with the specified name.
void NavigateNamedFrame(const ToRenderFrameHost& caller_frame,
                        const GURL& url,
                        const std::string& name) {
  bool success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      caller_frame,
      "window.domAutomationController.send("
      "    !!window.open('" + url.spec() + "', '" + name + "'));",
      &success));
  EXPECT_TRUE(success);
}

// Helper function to generate a click on the given RenderWidgetHost.  The
// mouse event is forwarded directly to the RenderWidgetHost without any
// hit-testing.
void SimulateMouseClick(RenderWidgetHost* rwh, int x, int y) {
  blink::WebMouseEvent mouse_event;
  mouse_event.type = blink::WebInputEvent::MouseDown;
  mouse_event.button = blink::WebPointerProperties::ButtonLeft;
  mouse_event.x = x;
  mouse_event.y = y;
  rwh->ForwardMouseEvent(mouse_event);
}

// Retrieve document.origin for the frame |ftn|.
std::string GetDocumentOrigin(FrameTreeNode* ftn) {
  std::string origin;
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      ftn->current_frame_host(),
      "domAutomationController.send(document.origin)", &origin));
  return origin;
}

class RenderWidgetHostMouseEventMonitor {
 public:
  explicit RenderWidgetHostMouseEventMonitor(RenderWidgetHost* host)
      : host_(host), event_received_(false) {
    host_->AddMouseEventCallback(
        base::Bind(&RenderWidgetHostMouseEventMonitor::MouseEventCallback,
                   base::Unretained(this)));
  }
  ~RenderWidgetHostMouseEventMonitor() {
    host_->RemoveMouseEventCallback(
        base::Bind(&RenderWidgetHostMouseEventMonitor::MouseEventCallback,
                   base::Unretained(this)));
  }
  bool EventWasReceived() const { return event_received_; }
  void ResetEventReceived() { event_received_ = false; }
  const blink::WebMouseEvent& event() const { return event_; }

 private:
  bool MouseEventCallback(const blink::WebMouseEvent& event) {
    event_received_ = true;
    event_ = event;
    return false;
  }
  RenderWidgetHost* host_;
  bool event_received_;
  blink::WebMouseEvent event_;

  DISALLOW_COPY_AND_ASSIGN(RenderWidgetHostMouseEventMonitor);
};

// Helper function that performs a surface hittest.
void SurfaceHitTestTestHelper(
    Shell* shell,
    net::test_server::EmbeddedTestServer* embedded_test_server) {
  GURL main_url(embedded_test_server->GetURL(
      "/frame_tree/page_with_positioned_frame.html"));
  NavigateToURL(shell, main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell->web_contents())
                            ->GetFrameTree()
                            ->root();
  ASSERT_EQ(1U, root->child_count());

  FrameTreeNode* child_node = root->child_at(0);
  GURL site_url(embedded_test_server->GetURL("baz.com", "/title1.html"));
  EXPECT_EQ(site_url, child_node->current_url());
  EXPECT_NE(shell->web_contents()->GetSiteInstance(),
            child_node->current_frame_host()->GetSiteInstance());

  // Create listeners for mouse events.
  RenderWidgetHostMouseEventMonitor main_frame_monitor(
      root->current_frame_host()->GetRenderWidgetHost());
  RenderWidgetHostMouseEventMonitor child_frame_monitor(
      child_node->current_frame_host()->GetRenderWidgetHost());

  RenderWidgetHostInputEventRouter* router =
      static_cast<WebContentsImpl*>(shell->web_contents())
          ->GetInputEventRouter();

  RenderWidgetHostViewBase* root_view = static_cast<RenderWidgetHostViewBase*>(
      root->current_frame_host()->GetRenderWidgetHost()->GetView());
  RenderWidgetHostViewBase* rwhv_child = static_cast<RenderWidgetHostViewBase*>(
      child_node->current_frame_host()->GetRenderWidgetHost()->GetView());

  SurfaceHitTestReadyNotifier notifier(
      static_cast<RenderWidgetHostViewChildFrame*>(rwhv_child));
  notifier.WaitForSurfaceReady();

  // Target input event to child frame.
  blink::WebMouseEvent child_event;
  child_event.type = blink::WebInputEvent::MouseDown;
  child_event.button = blink::WebPointerProperties::ButtonLeft;
  child_event.x = 75;
  child_event.y = 75;
  child_event.clickCount = 1;
  main_frame_monitor.ResetEventReceived();
  child_frame_monitor.ResetEventReceived();
  router->RouteMouseEvent(root_view, &child_event);

  EXPECT_TRUE(child_frame_monitor.EventWasReceived());
  EXPECT_EQ(23, child_frame_monitor.event().x);
  EXPECT_EQ(23, child_frame_monitor.event().y);
  EXPECT_FALSE(main_frame_monitor.EventWasReceived());

  child_frame_monitor.ResetEventReceived();
  main_frame_monitor.ResetEventReceived();

  // Target input event to main frame.
  blink::WebMouseEvent main_event;
  main_event.type = blink::WebInputEvent::MouseDown;
  main_event.button = blink::WebPointerProperties::ButtonLeft;
  main_event.x = 1;
  main_event.y = 1;
  main_event.clickCount = 1;
  // Ladies and gentlemen, THIS is the main_event!
  router->RouteMouseEvent(root_view, &main_event);

  EXPECT_FALSE(child_frame_monitor.EventWasReceived());
  EXPECT_TRUE(main_frame_monitor.EventWasReceived());
  EXPECT_EQ(1, main_frame_monitor.event().x);
  EXPECT_EQ(1, main_frame_monitor.event().y);
}

class RedirectNotificationObserver : public NotificationObserver {
 public:
  // Register to listen for notifications of the given type from either a
  // specific source, or from all sources if |source| is
  // NotificationService::AllSources().
  RedirectNotificationObserver(int notification_type,
                               const NotificationSource& source);
  ~RedirectNotificationObserver() override;

  // Wait until the specified notification occurs.  If the notification was
  // emitted between the construction of this object and this call then it
  // returns immediately.
  void Wait();

  // Returns NotificationService::AllSources() if we haven't observed a
  // notification yet.
  const NotificationSource& source() const {
    return source_;
  }

  const NotificationDetails& details() const {
    return details_;
  }

  // NotificationObserver:
  void Observe(int type,
               const NotificationSource& source,
               const NotificationDetails& details) override;

 private:
  bool seen_;
  bool seen_twice_;
  bool running_;
  NotificationRegistrar registrar_;

  NotificationSource source_;
  NotificationDetails details_;
  scoped_refptr<MessageLoopRunner> message_loop_runner_;

  DISALLOW_COPY_AND_ASSIGN(RedirectNotificationObserver);
};

RedirectNotificationObserver::RedirectNotificationObserver(
    int notification_type,
    const NotificationSource& source)
    : seen_(false),
      running_(false),
      source_(NotificationService::AllSources()) {
  registrar_.Add(this, notification_type, source);
}

RedirectNotificationObserver::~RedirectNotificationObserver() {}

void RedirectNotificationObserver::Wait() {
  if (seen_ && seen_twice_)
    return;

  running_ = true;
  message_loop_runner_ = new MessageLoopRunner;
  message_loop_runner_->Run();
  EXPECT_TRUE(seen_);
}

void RedirectNotificationObserver::Observe(
    int type,
    const NotificationSource& source,
    const NotificationDetails& details) {
  source_ = source;
  details_ = details;
  seen_twice_ = seen_;
  seen_ = true;
  if (!running_)
    return;

  message_loop_runner_->Quit();
  running_ = false;
}

// This observer keeps track of the number of created RenderFrameHosts.  Tests
// can use this to ensure that a certain number of child frames has been
// created after navigating.
class RenderFrameHostCreatedObserver : public WebContentsObserver {
 public:
  RenderFrameHostCreatedObserver(WebContents* web_contents,
                                 int expected_frame_count)
      : WebContentsObserver(web_contents),
        expected_frame_count_(expected_frame_count),
        frames_created_(0),
        message_loop_runner_(new MessageLoopRunner) {}

  ~RenderFrameHostCreatedObserver() override;

  // Runs a nested message loop and blocks until the expected number of
  // RenderFrameHosts is created.
  void Wait();

 private:
  // WebContentsObserver
  void RenderFrameCreated(RenderFrameHost* render_frame_host) override;

  // The number of RenderFrameHosts to wait for.
  int expected_frame_count_;

  // The number of RenderFrameHosts that have been created.
  int frames_created_;

  // The MessageLoopRunner used to spin the message loop.
  scoped_refptr<MessageLoopRunner> message_loop_runner_;

  DISALLOW_COPY_AND_ASSIGN(RenderFrameHostCreatedObserver);
};

RenderFrameHostCreatedObserver::~RenderFrameHostCreatedObserver() {
}

void RenderFrameHostCreatedObserver::Wait() {
  message_loop_runner_->Run();
}

void RenderFrameHostCreatedObserver::RenderFrameCreated(
    RenderFrameHost* render_frame_host) {
  frames_created_++;
  if (frames_created_ == expected_frame_count_) {
    message_loop_runner_->Quit();
  }
}

// This observer is used to wait for its owner FrameTreeNode to become focused.
class FrameFocusedObserver : public FrameTreeNode::Observer {
 public:
  FrameFocusedObserver(FrameTreeNode* owner)
      : owner_(owner), message_loop_runner_(new MessageLoopRunner) {
    owner->AddObserver(this);
  }

  ~FrameFocusedObserver() override { owner_->RemoveObserver(this); }

  void Wait() { message_loop_runner_->Run(); }

 private:
  // FrameTreeNode::Observer
  void OnFrameTreeNodeFocused(FrameTreeNode* node) override {
    if (node == owner_)
      message_loop_runner_->Quit();
  }

  FrameTreeNode* owner_;
  scoped_refptr<MessageLoopRunner> message_loop_runner_;

  DISALLOW_COPY_AND_ASSIGN(FrameFocusedObserver);
};

// This observer is used to wait for its owner FrameTreeNode to become deleted.
class FrameDeletedObserver : public FrameTreeNode::Observer {
 public:
  FrameDeletedObserver(FrameTreeNode* owner)
      : owner_(owner), message_loop_runner_(new MessageLoopRunner) {
    owner->AddObserver(this);
  }

  void Wait() { message_loop_runner_->Run(); }

 private:
  // FrameTreeNode::Observer
  void OnFrameTreeNodeDestroyed(FrameTreeNode* node) override {
    if (node == owner_)
      message_loop_runner_->Quit();
  }

  FrameTreeNode* owner_;
  scoped_refptr<MessageLoopRunner> message_loop_runner_;

  DISALLOW_COPY_AND_ASSIGN(FrameDeletedObserver);
};

// Helper function to focus a frame by sending it a mouse click and then
// waiting for it to become focused.
void FocusFrame(FrameTreeNode* frame) {
  FrameFocusedObserver focus_observer(frame);
  SimulateMouseClick(frame->current_frame_host()->GetRenderWidgetHost(), 1, 1);
  focus_observer.Wait();
}

// A WebContentsDelegate that catches messages sent to the console.
class ConsoleObserverDelegate : public WebContentsDelegate {
 public:
  ConsoleObserverDelegate(WebContents* web_contents, const std::string& filter)
      : web_contents_(web_contents),
        filter_(filter),
        message_(""),
        message_loop_runner_(new MessageLoopRunner) {}

  ~ConsoleObserverDelegate() override {}

  bool AddMessageToConsole(WebContents* source,
                           int32_t level,
                           const base::string16& message,
                           int32_t line_no,
                           const base::string16& source_id) override;

  std::string message() { return message_; }

  void Wait();

 private:
  WebContents* web_contents_;
  std::string filter_;
  std::string message_;

  // The MessageLoopRunner used to spin the message loop.
  scoped_refptr<MessageLoopRunner> message_loop_runner_;

  DISALLOW_COPY_AND_ASSIGN(ConsoleObserverDelegate);
};

void ConsoleObserverDelegate::Wait() {
  message_loop_runner_->Run();
}

bool ConsoleObserverDelegate::AddMessageToConsole(
    WebContents* source,
    int32_t level,
    const base::string16& message,
    int32_t line_no,
    const base::string16& source_id) {
  DCHECK(source == web_contents_);

  std::string ascii_message = base::UTF16ToASCII(message);
  if (base::MatchPattern(ascii_message, filter_)) {
    message_ = ascii_message;
    message_loop_runner_->Quit();
  }
  return false;
}

// A BrowserMessageFilter that drops SwapOut ACK messages.
class SwapoutACKMessageFilter : public BrowserMessageFilter {
 public:
  SwapoutACKMessageFilter() : BrowserMessageFilter(FrameMsgStart) {}

 protected:
  ~SwapoutACKMessageFilter() override {}

 private:
  // BrowserMessageFilter:
  bool OnMessageReceived(const IPC::Message& message) override {
    return message.type() == FrameHostMsg_SwapOut_ACK::ID;
  }

  DISALLOW_COPY_AND_ASSIGN(SwapoutACKMessageFilter);
};

class RenderWidgetHostVisibilityObserver : public NotificationObserver {
 public:
  explicit RenderWidgetHostVisibilityObserver(RenderWidgetHostImpl* rwhi,
                                              bool expected_visibility_state)
      : expected_visibility_state_(expected_visibility_state),
        was_observed_(false),
        did_fail_(false),
        source_(rwhi) {
    registrar_.Add(this, NOTIFICATION_RENDER_WIDGET_VISIBILITY_CHANGED,
                   source_);
    message_loop_runner_ = new MessageLoopRunner;
  }

  bool WaitUntilSatisfied() {
    if (!was_observed_)
      message_loop_runner_->Run();
    registrar_.Remove(this, NOTIFICATION_RENDER_WIDGET_VISIBILITY_CHANGED,
                      source_);
    return !did_fail_;
  }

 private:
  void Observe(int type,
               const NotificationSource& source,
               const NotificationDetails& details) override {
    was_observed_ = true;
    did_fail_ = expected_visibility_state_ !=
                (*static_cast<const Details<bool>&>(details).ptr());
    if (message_loop_runner_->loop_running())
      message_loop_runner_->Quit();
  }

  bool expected_visibility_state_;
  scoped_refptr<MessageLoopRunner> message_loop_runner_;
  NotificationRegistrar registrar_;
  bool was_observed_;
  bool did_fail_;
  Source<RenderWidgetHost> source_;

  DISALLOW_COPY_AND_ASSIGN(RenderWidgetHostVisibilityObserver);
};

}  // namespace

//
// SitePerProcessBrowserTest
//

SitePerProcessBrowserTest::SitePerProcessBrowserTest() {
};

std::string SitePerProcessBrowserTest::DepictFrameTree(FrameTreeNode* node) {
  return visualizer_.DepictFrameTree(node);
}

void SitePerProcessBrowserTest::SetUpCommandLine(
    base::CommandLine* command_line) {
  IsolateAllSitesForTesting(command_line);
};

void SitePerProcessBrowserTest::SetUpOnMainThread() {
  host_resolver()->AddRule("*", "127.0.0.1");
  ASSERT_TRUE(embedded_test_server()->Start());
  SetupCrossSiteRedirector(embedded_test_server());
}

//
// SitePerProcessHighDPIBrowserTest
//


class SitePerProcessHighDPIBrowserTest : public SitePerProcessBrowserTest {
 public:
  SitePerProcessHighDPIBrowserTest() {}

 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    SitePerProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(switches::kForceDeviceScaleFactor,
                                    base::StringPrintf("2"));
  }
};

// SitePerProcessIgnoreCertErrorsBrowserTest

class SitePerProcessIgnoreCertErrorsBrowserTest
    : public SitePerProcessBrowserTest {
 public:
  SitePerProcessIgnoreCertErrorsBrowserTest() {}

 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    SitePerProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitch(switches::kIgnoreCertificateErrors);
  }
};

// Ensure that navigating subframes in --site-per-process mode works and the
// correct documents are committed.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, CrossSiteIframe) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a,a(a,a(a)))"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(shell()->web_contents())->
          GetFrameTree()->root();

  TestNavigationObserver observer(shell()->web_contents());

  // Load same-site page into iframe.
  FrameTreeNode* child = root->child_at(0);
  GURL http_url(embedded_test_server()->GetURL("a.com", "/title1.html"));
  NavigateFrameToURL(child, http_url);
  EXPECT_EQ(http_url, observer.last_navigation_url());
  EXPECT_TRUE(observer.last_navigation_succeeded());
  {
    // There should be only one RenderWidgetHost when there are no
    // cross-process iframes.
    std::set<RenderWidgetHostView*> views_set =
        static_cast<WebContentsImpl*>(shell()->web_contents())
            ->GetRenderWidgetHostViewsInTree();
    EXPECT_EQ(1U, views_set.size());
  }

  EXPECT_EQ(
      " Site A\n"
      "   |--Site A\n"
      "   +--Site A\n"
      "        |--Site A\n"
      "        +--Site A\n"
      "             +--Site A\n"
      "Where A = http://a.com/",
      DepictFrameTree(root));

  // Load cross-site page into iframe.
  GURL url = embedded_test_server()->GetURL("foo.com", "/title2.html");
  NavigateFrameToURL(root->child_at(0), url);
  // Verify that the navigation succeeded and the expected URL was loaded.
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url, observer.last_navigation_url());

  // Ensure that we have created a new process for the subframe.
  ASSERT_EQ(2U, root->child_count());
  SiteInstance* site_instance = child->current_frame_host()->GetSiteInstance();
  RenderViewHost* rvh = child->current_frame_host()->render_view_host();
  RenderProcessHost* rph = child->current_frame_host()->GetProcess();
  EXPECT_NE(shell()->web_contents()->GetRenderViewHost(), rvh);
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(), site_instance);
  EXPECT_NE(shell()->web_contents()->GetRenderProcessHost(), rph);
  {
    // There should be now two RenderWidgetHosts, one for each process
    // rendering a frame.
    std::set<RenderWidgetHostView*> views_set =
        static_cast<WebContentsImpl*>(shell()->web_contents())
            ->GetRenderWidgetHostViewsInTree();
    EXPECT_EQ(2U, views_set.size());
  }
  RenderFrameProxyHost* proxy_to_parent =
      child->render_manager()->GetProxyToParent();
  EXPECT_TRUE(proxy_to_parent);
  EXPECT_TRUE(proxy_to_parent->cross_process_frame_connector());
  // The out-of-process iframe should have its own RenderWidgetHost,
  // independent of any RenderViewHost.
  EXPECT_NE(
      rvh->GetWidget()->GetView(),
      proxy_to_parent->cross_process_frame_connector()->get_view_for_testing());
  EXPECT_TRUE(child->current_frame_host()->GetRenderWidgetHost());

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site A ------- proxies for B\n"
      "        |--Site A -- proxies for B\n"
      "        +--Site A -- proxies for B\n"
      "             +--Site A -- proxies for B\n"
      "Where A = http://a.com/\n"
      "      B = http://foo.com/",
      DepictFrameTree(root));

  // Load another cross-site page into the same iframe.
  url = embedded_test_server()->GetURL("bar.com", "/title3.html");
  NavigateFrameToURL(root->child_at(0), url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url, observer.last_navigation_url());

  // Check again that a new process is created and is different from the
  // top level one and the previous one.
  ASSERT_EQ(2U, root->child_count());
  child = root->child_at(0);
  EXPECT_NE(shell()->web_contents()->GetRenderViewHost(),
            child->current_frame_host()->render_view_host());
  EXPECT_NE(rvh, child->current_frame_host()->render_view_host());
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(),
            child->current_frame_host()->GetSiteInstance());
  EXPECT_NE(site_instance,
            child->current_frame_host()->GetSiteInstance());
  EXPECT_NE(shell()->web_contents()->GetRenderProcessHost(),
            child->current_frame_host()->GetProcess());
  EXPECT_NE(rph, child->current_frame_host()->GetProcess());
  {
    std::set<RenderWidgetHostView*> views_set =
        static_cast<WebContentsImpl*>(shell()->web_contents())
            ->GetRenderWidgetHostViewsInTree();
    EXPECT_EQ(2U, views_set.size());
  }
  EXPECT_EQ(proxy_to_parent, child->render_manager()->GetProxyToParent());
  EXPECT_TRUE(proxy_to_parent->cross_process_frame_connector());
  EXPECT_NE(
      child->current_frame_host()->render_view_host()->GetWidget()->GetView(),
      proxy_to_parent->cross_process_frame_connector()->get_view_for_testing());
  EXPECT_TRUE(child->current_frame_host()->GetRenderWidgetHost());

  EXPECT_EQ(
      " Site A ------------ proxies for C\n"
      "   |--Site C ------- proxies for A\n"
      "   +--Site A ------- proxies for C\n"
      "        |--Site A -- proxies for C\n"
      "        +--Site A -- proxies for C\n"
      "             +--Site A -- proxies for C\n"
      "Where A = http://a.com/\n"
      "      C = http://bar.com/",
      DepictFrameTree(root));
}

// Test that mouse events are being routed to the correct RenderWidgetHostView
// based on coordinates.
#if defined(OS_ANDROID)
// Browser process hit testing is not implemented on Android.
// https://crbug.com/491334
#define MAYBE_SurfaceHitTestTest DISABLED_SurfaceHitTestTest
#else
#define MAYBE_SurfaceHitTestTest SurfaceHitTestTest
#endif
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, MAYBE_SurfaceHitTestTest) {
  SurfaceHitTestTestHelper(shell(), embedded_test_server());
}

// Same test as above, but runs in high-dpi mode.
#if defined(OS_ANDROID) || defined(OS_WIN)
// Browser process hit testing is not implemented on Android.
// https://crbug.com/491334
// Windows is disabled because of https://crbug.com/545547.
#define MAYBE_HighDPISurfaceHitTestTest DISABLED_SurfaceHitTestTest
#else
#define MAYBE_HighDPISurfaceHitTestTest SurfaceHitTestTest
#endif
IN_PROC_BROWSER_TEST_F(SitePerProcessHighDPIBrowserTest,
                       MAYBE_HighDPISurfaceHitTestTest) {
  SurfaceHitTestTestHelper(shell(), embedded_test_server());
}

// This test tests that browser process hittesting ignores frames with
// pointer-events: none.
#if defined(OS_ANDROID)
// Browser process hit testing is not implemented on Android.
// https://crbug.com/491334
#define MAYBE_SurfaceHitTestPointerEventsNone \
  DISABLED_SurfaceHitTestPointerEventsNone
#elif defined(THREAD_SANITIZER)
// Flaky on TSAN. https://crbug.com/582277
#define MAYBE_SurfaceHitTestPointerEventsNone \
  DISABLED_SurfaceHitTestPointerEventsNone
#else
#define MAYBE_SurfaceHitTestPointerEventsNone SurfaceHitTestPointerEventsNone
#endif
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       MAYBE_SurfaceHitTestPointerEventsNone) {
  GURL main_url(embedded_test_server()->GetURL(
      "/frame_tree/page_with_positioned_frame_pointer-events_none.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  ASSERT_EQ(1U, root->child_count());

  FrameTreeNode* child_node = root->child_at(0);
  GURL site_url(embedded_test_server()->GetURL("baz.com", "/title1.html"));
  EXPECT_EQ(site_url, child_node->current_url());
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(),
            child_node->current_frame_host()->GetSiteInstance());

  // Create listeners for mouse events.
  RenderWidgetHostMouseEventMonitor main_frame_monitor(
      root->current_frame_host()->GetRenderWidgetHost());
  RenderWidgetHostMouseEventMonitor child_frame_monitor(
      child_node->current_frame_host()->GetRenderWidgetHost());

  RenderWidgetHostInputEventRouter* router =
      static_cast<WebContentsImpl*>(shell()->web_contents())
          ->GetInputEventRouter();

  RenderWidgetHostViewBase* root_view = static_cast<RenderWidgetHostViewBase*>(
      root->current_frame_host()->GetRenderWidgetHost()->GetView());
  RenderWidgetHostViewBase* rwhv_child = static_cast<RenderWidgetHostViewBase*>(
      child_node->current_frame_host()->GetRenderWidgetHost()->GetView());

  SurfaceHitTestReadyNotifier notifier(
      static_cast<RenderWidgetHostViewChildFrame*>(rwhv_child));
  notifier.WaitForSurfaceReady();

  // Target input event to child frame.
  blink::WebMouseEvent child_event;
  child_event.type = blink::WebInputEvent::MouseDown;
  child_event.button = blink::WebPointerProperties::ButtonLeft;
  child_event.x = 75;
  child_event.y = 75;
  child_event.clickCount = 1;
  main_frame_monitor.ResetEventReceived();
  child_frame_monitor.ResetEventReceived();
  router->RouteMouseEvent(root_view, &child_event);

  EXPECT_TRUE(main_frame_monitor.EventWasReceived());
  EXPECT_EQ(75, main_frame_monitor.event().x);
  EXPECT_EQ(75, main_frame_monitor.event().y);
  EXPECT_FALSE(child_frame_monitor.EventWasReceived());
}

// Tests OOPIF rendering by checking that the RWH of the iframe generates
// OnSwapCompositorFrame message.
#if defined(OS_ANDROID)
// http://crbug.com/471850
#define MAYBE_CompositorFrameSwapped DISABLED_CompositorFrameSwapped
#else
#define MAYBE_CompositorFrameSwapped CompositorFrameSwapped
#endif
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       MAYBE_CompositorFrameSwapped) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(baz)"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  ASSERT_EQ(1U, root->child_count());

  FrameTreeNode* child_node = root->child_at(0);
  GURL site_url(embedded_test_server()->GetURL(
      "baz.com", "/cross_site_iframe_factory.html?baz()"));
  EXPECT_EQ(site_url, child_node->current_url());
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(),
            child_node->current_frame_host()->GetSiteInstance());
  RenderWidgetHostViewBase* rwhv_base = static_cast<RenderWidgetHostViewBase*>(
      child_node->current_frame_host()->GetRenderWidgetHost()->GetView());

  // Wait for OnSwapCompositorFrame message.
  while (rwhv_base->RendererFrameNumber() <= 0) {
    // TODO(lazyboy): Find a better way to avoid sleeping like this. See
    // http://crbug.com/405282 for details.
    base::RunLoop run_loop;
    base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(),
        base::TimeDelta::FromMilliseconds(10));
    run_loop.Run();
  }
}

// Ensure that OOPIFs are deleted after navigating to a new main frame.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, CleanupCrossSiteIframe) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a,a(a,a(a)))"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(shell()->web_contents())->
          GetFrameTree()->root();

  TestNavigationObserver observer(shell()->web_contents());

  // Load a cross-site page into both iframes.
  GURL foo_url = embedded_test_server()->GetURL("foo.com", "/title2.html");
  NavigateFrameToURL(root->child_at(0), foo_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(foo_url, observer.last_navigation_url());
  NavigateFrameToURL(root->child_at(1), foo_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(foo_url, observer.last_navigation_url());

  // Ensure that we have created a new process for the subframes.
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://a.com/\n"
      "      B = http://foo.com/",
      DepictFrameTree(root));

  int subframe_process_id = root->child_at(0)
                                ->current_frame_host()
                                ->GetSiteInstance()
                                ->GetProcess()
                                ->GetID();
  int subframe_rvh_id = root->child_at(0)
                            ->current_frame_host()
                            ->render_view_host()
                            ->GetRoutingID();
  EXPECT_TRUE(RenderViewHost::FromID(subframe_process_id, subframe_rvh_id));

  // Use Javascript in the parent to remove one of the frames and ensure that
  // the subframe goes away.
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(),
                            "document.body.removeChild("
                            "document.querySelectorAll('iframe')[0])"));
  ASSERT_EQ(1U, root->child_count());

  // Load a new same-site page in the top-level frame and ensure the other
  // subframe goes away.
  GURL new_url(embedded_test_server()->GetURL("a.com", "/title1.html"));
  NavigateToURL(shell(), new_url);
  ASSERT_EQ(0U, root->child_count());

  // Ensure the RVH for the subframe gets cleaned up when the frame goes away.
  EXPECT_FALSE(RenderViewHost::FromID(subframe_process_id, subframe_rvh_id));
}

// Ensure that root frames cannot be detached.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, RestrictFrameDetach) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a,a(a,a(a)))"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(shell()->web_contents())->
          GetFrameTree()->root();

  TestNavigationObserver observer(shell()->web_contents());

  // Load cross-site pages into both iframes.
  GURL foo_url = embedded_test_server()->GetURL("foo.com", "/title2.html");
  NavigateFrameToURL(root->child_at(0), foo_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(foo_url, observer.last_navigation_url());
  GURL bar_url = embedded_test_server()->GetURL("bar.com", "/title2.html");
  NavigateFrameToURL(root->child_at(1), bar_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(bar_url, observer.last_navigation_url());

  // Ensure that we have created new processes for the subframes.
  ASSERT_EQ(2U, root->child_count());
  FrameTreeNode* foo_child = root->child_at(0);
  SiteInstance* foo_site_instance =
      foo_child->current_frame_host()->GetSiteInstance();
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(), foo_site_instance);
  FrameTreeNode* bar_child = root->child_at(1);
  SiteInstance* bar_site_instance =
      bar_child->current_frame_host()->GetSiteInstance();
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(), bar_site_instance);

  EXPECT_EQ(
      " Site A ------------ proxies for B C\n"
      "   |--Site B ------- proxies for A C\n"
      "   +--Site C ------- proxies for A B\n"
      "Where A = http://a.com/\n"
      "      B = http://foo.com/\n"
      "      C = http://bar.com/",
      DepictFrameTree(root));

  // Simulate an attempt to detach the root frame from foo_site_instance.  This
  // should kill foo_site_instance's process.
  RenderFrameProxyHost* foo_mainframe_rfph =
      root->render_manager()->GetRenderFrameProxyHost(foo_site_instance);
  content::RenderProcessHostWatcher foo_terminated(
      foo_mainframe_rfph->GetProcess(),
      content::RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
  FrameHostMsg_Detach evil_msg2(foo_mainframe_rfph->GetRoutingID());
  IPC::IpcSecurityTestUtil::PwnMessageReceived(
      foo_mainframe_rfph->GetProcess()->GetChannel(), evil_msg2);
  foo_terminated.Wait();

  EXPECT_EQ(
      " Site A ------------ proxies for B C\n"
      "   |--Site B ------- proxies for A C\n"
      "   +--Site C ------- proxies for A B\n"
      "Where A = http://a.com/\n"
      "      B = http://foo.com/ (no process)\n"
      "      C = http://bar.com/",
      DepictFrameTree(root));
}

IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, NavigateRemoteFrame) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a,a(a,a(a)))"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(shell()->web_contents())->
          GetFrameTree()->root();

  TestNavigationObserver observer(shell()->web_contents());

  // Load same-site page into iframe.
  FrameTreeNode* child = root->child_at(0);
  GURL http_url(embedded_test_server()->GetURL("a.com", "/title1.html"));
  NavigateFrameToURL(child, http_url);
  EXPECT_EQ(http_url, observer.last_navigation_url());
  EXPECT_TRUE(observer.last_navigation_succeeded());

  // Load cross-site page into iframe.
  GURL url = embedded_test_server()->GetURL("foo.com", "/title2.html");
  NavigateFrameToURL(root->child_at(0), url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url, observer.last_navigation_url());

  // Ensure that we have created a new process for the subframe.
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site A ------- proxies for B\n"
      "        |--Site A -- proxies for B\n"
      "        +--Site A -- proxies for B\n"
      "             +--Site A -- proxies for B\n"
      "Where A = http://a.com/\n"
      "      B = http://foo.com/",
      DepictFrameTree(root));
  SiteInstance* site_instance = child->current_frame_host()->GetSiteInstance();
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(), site_instance);

  // Emulate the main frame changing the src of the iframe such that it
  // navigates cross-site.
  url = embedded_test_server()->GetURL("bar.com", "/title3.html");
  NavigateIframeToURL(shell()->web_contents(), "child-0", url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url, observer.last_navigation_url());

  // Check again that a new process is created and is different from the
  // top level one and the previous one.
  EXPECT_EQ(
      " Site A ------------ proxies for C\n"
      "   |--Site C ------- proxies for A\n"
      "   +--Site A ------- proxies for C\n"
      "        |--Site A -- proxies for C\n"
      "        +--Site A -- proxies for C\n"
      "             +--Site A -- proxies for C\n"
      "Where A = http://a.com/\n"
      "      C = http://bar.com/",
      DepictFrameTree(root));

  // Navigate back to the parent's origin and ensure we return to the
  // parent's process.
  NavigateFrameToURL(child, http_url);
  EXPECT_EQ(http_url, observer.last_navigation_url());
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(shell()->web_contents()->GetSiteInstance(),
            child->current_frame_host()->GetSiteInstance());
}

IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       NavigateRemoteFrameToBlankAndDataURLs) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a,a(a))"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(shell()->web_contents())->
          GetFrameTree()->root();

  TestNavigationObserver observer(shell()->web_contents());

  // Load same-site page into iframe.
  FrameTreeNode* child = root->child_at(0);
  GURL http_url(embedded_test_server()->GetURL("a.com", "/title1.html"));
  NavigateFrameToURL(child, http_url);
  EXPECT_EQ(http_url, observer.last_navigation_url());
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(
      " Site A\n"
      "   |--Site A\n"
      "   +--Site A\n"
      "        +--Site A\n"
      "Where A = http://a.com/",
      DepictFrameTree(root));

  // Load cross-site page into iframe.
  GURL url = embedded_test_server()->GetURL("foo.com", "/title2.html");
  NavigateFrameToURL(root->child_at(0), url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url, observer.last_navigation_url());
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site A ------- proxies for B\n"
      "        +--Site A -- proxies for B\n"
      "Where A = http://a.com/\n"
      "      B = http://foo.com/",
      DepictFrameTree(root));

  // Navigate iframe to a data URL. The navigation happens from a script in the
  // parent frame, so the data URL should be committed in the same SiteInstance
  // as the parent frame.
  GURL data_url("data:text/html,dataurl");
  NavigateIframeToURL(shell()->web_contents(), "child-0", data_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(data_url, observer.last_navigation_url());

  // Ensure that we have navigated using the top level process.
  EXPECT_EQ(
      " Site A\n"
      "   |--Site A\n"
      "   +--Site A\n"
      "        +--Site A\n"
      "Where A = http://a.com/",
      DepictFrameTree(root));

  // Load cross-site page into iframe.
  url = embedded_test_server()->GetURL("bar.com", "/title2.html");
  NavigateFrameToURL(root->child_at(0), url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url, observer.last_navigation_url());
  EXPECT_EQ(
      " Site A ------------ proxies for C\n"
      "   |--Site C ------- proxies for A\n"
      "   +--Site A ------- proxies for C\n"
      "        +--Site A -- proxies for C\n"
      "Where A = http://a.com/\n"
      "      C = http://bar.com/",
      DepictFrameTree(root));

  // Navigate iframe to about:blank. The navigation happens from a script in the
  // parent frame, so it should be committed in the same SiteInstance as the
  // parent frame.
  GURL about_blank_url("about:blank");
  NavigateIframeToURL(shell()->web_contents(), "child-0", about_blank_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(about_blank_url, observer.last_navigation_url());

  // Ensure that we have navigated using the top level process.
  EXPECT_EQ(
      " Site A\n"
      "   |--Site A\n"
      "   +--Site A\n"
      "        +--Site A\n"
      "Where A = http://a.com/",
      DepictFrameTree(root));
}

// This test checks that killing a renderer process of a remote frame
// and then navigating some other frame to the same SiteInstance of the killed
// process works properly.
// This can be illustrated as follows,
// where 1/2/3 are FrameTreeNode-s and A/B are processes and B* is the killed
// B process:
//
//     1        A                  A                           A
//    / \  ->  / \  -> Kill B ->  / \  -> Navigate 3 to B ->  / \  .
//   2   3    B   A              B*  A                       B*  B
//
// Initially, node1.proxy_hosts_ = {B}
// After we kill B, we make sure B stays in node1.proxy_hosts_, then we navigate
// 3 to B and we expect that to complete normally.
// See http://crbug.com/432107.
//
// Note that due to http://crbug.com/450681, node2 cannot be re-navigated to
// site B and stays in not rendered state.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       NavigateRemoteFrameToKilledProcess) {
  GURL main_url(embedded_test_server()->GetURL(
      "foo.com", "/cross_site_iframe_factory.html?foo.com(bar.com, foo.com)"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(shell()->web_contents())->
          GetFrameTree()->root();

  TestNavigationObserver observer(shell()->web_contents());
  ASSERT_EQ(2U, root->child_count());

  // Make sure node2 points to the correct cross-site page.
  GURL site_b_url = embedded_test_server()->GetURL(
      "bar.com", "/cross_site_iframe_factory.html?bar.com()");
  FrameTreeNode* node2 = root->child_at(0);
  EXPECT_EQ(site_b_url, node2->current_url());

  // Kill that cross-site renderer.
  RenderProcessHost* child_process =
      node2->current_frame_host()->GetProcess();
  RenderProcessHostWatcher crash_observer(
      child_process, RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
  child_process->Shutdown(0, false);
  crash_observer.Wait();

  // Now navigate the second iframe (node3) to the same site as the node2.
  FrameTreeNode* node3 = root->child_at(1);
  NavigateFrameToURL(node3, site_b_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(site_b_url, observer.last_navigation_url());
}

// This test is similar to
// SitePerProcessBrowserTest.NavigateRemoteFrameToKilledProcess with
// addition that node2 also has a cross-origin frame to site C.
//
//     1          A                  A                       A
//    / \        / \                / \                     / \  .
//   2   3 ->   B   A -> Kill B -> B*   A -> Navigate 3 -> B*  B
//  /          /
// 4          C
//
// Initially, node1.proxy_hosts_ = {B, C}
// After we kill B, we make sure B stays in node1.proxy_hosts_, but
// C gets cleared from node1.proxy_hosts_.
//
// Note that due to http://crbug.com/450681, node2 cannot be re-navigated to
// site B and stays in not rendered state.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       NavigateRemoteFrameToKilledProcessWithSubtree) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(bar(baz), a)"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(shell()->web_contents())->
          GetFrameTree()->root();
  TestNavigationObserver observer(shell()->web_contents());

  ASSERT_EQ(2U, root->child_count());

  GURL site_b_url(embedded_test_server()->GetURL(
      "bar.com", "/cross_site_iframe_factory.html?bar(baz())"));
  // We can't use a TestNavigationObserver to verify the URL here,
  // since the frame has children that may have clobbered it in the observer.
  EXPECT_EQ(site_b_url, root->child_at(0)->current_url());

  // Ensure that a new process is created for node2.
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(),
            root->child_at(0)->current_frame_host()->GetSiteInstance());
  // Ensure that a new process is *not* created for node3.
  EXPECT_EQ(shell()->web_contents()->GetSiteInstance(),
            root->child_at(1)->current_frame_host()->GetSiteInstance());

  ASSERT_EQ(1U, root->child_at(0)->child_count());

  // Make sure node4 points to the correct cross-site page.
  FrameTreeNode* node4 = root->child_at(0)->child_at(0);
  GURL site_c_url(embedded_test_server()->GetURL(
      "baz.com", "/cross_site_iframe_factory.html?baz()"));
  EXPECT_EQ(site_c_url, node4->current_url());

  // |site_instance_c| is expected to go away once we kill |child_process_b|
  // below, so create a local scope so we can extend the lifetime of
  // |site_instance_c| with a refptr.
  {
    // Initially each frame has proxies for the other sites.
    EXPECT_EQ(
        " Site A ------------ proxies for B C\n"
        "   |--Site B ------- proxies for A C\n"
        "   |    +--Site C -- proxies for A B\n"
        "   +--Site A ------- proxies for B C\n"
        "Where A = http://a.com/\n"
        "      B = http://bar.com/\n"
        "      C = http://baz.com/",
        DepictFrameTree(root));

    // Kill the render process for Site B.
    RenderProcessHost* child_process_b =
        root->child_at(0)->current_frame_host()->GetProcess();
    RenderProcessHostWatcher crash_observer(
        child_process_b, RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
    child_process_b->Shutdown(0, false);
    crash_observer.Wait();

    // The Site C frame (a child of the crashed Site B frame) should go away,
    // and there should be no remaining proxies for site C anywhere.
    EXPECT_EQ(
        " Site A ------------ proxies for B\n"
        "   |--Site B ------- proxies for A\n"
        "   +--Site A ------- proxies for B\n"
        "Where A = http://a.com/\n"
        "      B = http://bar.com/ (no process)",
        DepictFrameTree(root));
  }

  // Now navigate the second iframe (node3) to Site B also.
  FrameTreeNode* node3 = root->child_at(1);
  GURL url = embedded_test_server()->GetURL("bar.com", "/title1.html");
  NavigateFrameToURL(node3, url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url, observer.last_navigation_url());

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://a.com/\n"
      "      B = http://bar.com/",
      DepictFrameTree(root));
}

// Ensure that the renderer process doesn't crash when the main frame navigates
// a remote child to a page that results in a network error.
// See https://crbug.com/558016.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, NavigateRemoteAfterError) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a)"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  // Load same-site page into iframe.
  {
    TestNavigationObserver observer(shell()->web_contents());
    FrameTreeNode* child = root->child_at(0);
    GURL http_url(embedded_test_server()->GetURL("a.com", "/title1.html"));
    NavigateFrameToURL(child, http_url);
    EXPECT_EQ(http_url, observer.last_navigation_url());
    EXPECT_TRUE(observer.last_navigation_succeeded());
    observer.Wait();
  }

  // Load cross-site page into iframe.
  {
    TestNavigationObserver observer(shell()->web_contents());
    FrameTreeNode* child = root->child_at(0);
    GURL url = embedded_test_server()->GetURL("foo.com", "/title2.html");
    NavigateFrameToURL(root->child_at(0), url);
    EXPECT_TRUE(observer.last_navigation_succeeded());
    EXPECT_EQ(url, observer.last_navigation_url());
    observer.Wait();

    // Ensure that we have created a new process for the subframe.
    EXPECT_EQ(
        " Site A ------------ proxies for B\n"
        "   +--Site B ------- proxies for A\n"
        "Where A = http://a.com/\n"
        "      B = http://foo.com/",
        DepictFrameTree(root));
    SiteInstance* site_instance =
        child->current_frame_host()->GetSiteInstance();
    EXPECT_NE(shell()->web_contents()->GetSiteInstance(), site_instance);
  }

  // Stop the test server and try to navigate the remote frame.
  {
    GURL url = embedded_test_server()->GetURL("bar.com", "/title3.html");
    EXPECT_TRUE(embedded_test_server()->ShutdownAndWaitUntilComplete());
    NavigateIframeToURL(shell()->web_contents(), "child-0", url);
  }
}

// Ensure that a cross-site page ends up in the correct process when it
// successfully loads after earlier encountering a network error for it.
// See https://crbug.com/560511.
// TODO(creis): Make the net error page show in the correct process as well,
// per https://crbug.com/588314.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, ProcessTransferAfterError) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a)"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  FrameTreeNode* child = root->child_at(0);
  GURL url_a = child->current_url();

  // Disable host resolution in the test server and try to navigate the subframe
  // cross-site, which will lead to a committed net error (which looks like
  // success to the TestNavigationObserver).
  GURL url_b = embedded_test_server()->GetURL("b.com", "/title3.html");
  host_resolver()->ClearRules();
  TestNavigationObserver observer(shell()->web_contents());
  NavigateIframeToURL(shell()->web_contents(), "child-0", url_b);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url_b, observer.last_navigation_url());

  // The FrameTreeNode should update its URL (so that we don't affect other uses
  // of the API), but the frame's last_successful_url shouldn't change and the
  // origin should be empty.
  EXPECT_EQ(url_a, child->current_frame_host()->last_successful_url());
  EXPECT_EQ(url_b, child->current_url());
  EXPECT_EQ("null", child->current_origin().Serialize());

  // Try again after re-enabling host resolution.
  host_resolver()->AddRule("*", "127.0.0.1");
  NavigateIframeToURL(shell()->web_contents(), "child-0", url_b);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url_b, observer.last_navigation_url());

  // The FrameTreeNode should have updated its URL and origin.
  EXPECT_EQ(url_b, child->current_frame_host()->last_successful_url());
  EXPECT_EQ(url_b, child->current_url());
  EXPECT_EQ(url_b.GetOrigin().spec(),
            child->current_origin().Serialize() + '/');

  // Ensure that we have created a new process for the subframe.
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/",
      DepictFrameTree(root));
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(),
            child->current_frame_host()->GetSiteInstance());

  // Make sure that the navigation replaced the error page and that going back
  // ends up on the original site.
  EXPECT_EQ(2, shell()->web_contents()->GetController().GetEntryCount());
  {
    TestNavigationObserver back_load_observer(shell()->web_contents());
    shell()->web_contents()->GetController().GoBack();
    back_load_observer.Wait();
  }
  EXPECT_EQ(
      " Site A\n"
      "   +--Site A\n"
      "Where A = http://a.com/",
      DepictFrameTree(root));
  EXPECT_EQ(shell()->web_contents()->GetSiteInstance(),
            child->current_frame_host()->GetSiteInstance());
  EXPECT_EQ(url_a, child->current_frame_host()->last_successful_url());
  EXPECT_EQ(url_a, child->current_url());
  EXPECT_EQ(url_a.GetOrigin().spec(),
            child->current_origin().Serialize() + '/');
}

// Verify that killing a cross-site frame's process B and then navigating a
// frame to B correctly recreates all proxies in B.
//
//      1           A                    A          A
//    / | \       / | \                / | \      / | \  .
//   2  3  4 ->  B  A  A -> Kill B -> B* A  A -> B* B  A
//
// After the last step, the test sends a postMessage from node 3 to node 4,
// verifying that a proxy for node 4 has been recreated in process B.  This
// verifies the fix for https://crbug.com/478892.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       NavigatingToKilledProcessRestoresAllProxies) {
  // Navigate to a page with three frames: one cross-site and two same-site.
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/frame_tree/page_with_three_frames.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  TestNavigationObserver observer(shell()->web_contents());

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   |--Site A ------- proxies for B\n"
      "   +--Site A ------- proxies for B\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/",
      DepictFrameTree(root));

  // Kill the first subframe's b.com renderer.
  RenderProcessHost* child_process =
      root->child_at(0)->current_frame_host()->GetProcess();
  RenderProcessHostWatcher crash_observer(
      child_process, RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
  child_process->Shutdown(0, false);
  crash_observer.Wait();

  // Navigate the second subframe to b.com to recreate the b.com process.
  GURL b_url = embedded_test_server()->GetURL("b.com", "/post_message.html");
  NavigateFrameToURL(root->child_at(1), b_url);
  // TODO(alexmos): This can be removed once TestFrameNavigationObserver is
  // fixed to use DidFinishLoad.
  EXPECT_TRUE(
      WaitForRenderFrameReady(root->child_at(1)->current_frame_host()));
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(b_url, observer.last_navigation_url());
  EXPECT_TRUE(root->child_at(1)->current_frame_host()->IsRenderFrameLive());

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site A ------- proxies for B\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/",
      DepictFrameTree(root));

  // Check that third subframe's proxy is available in the b.com process by
  // sending it a postMessage from second subframe, and waiting for a reply.
  PostMessageAndWaitForReply(root->child_at(1),
                             "postToSibling('subframe-msg','frame3')",
                             "\"done-frame2\"");
}

// Verify that proxy creation doesn't recreate a crashed process if no frame
// will be created in it.
//
//      1           A                    A          A
//    / | \       / | \                / | \      / | \    .
//   2  3  4 ->  B  A  A -> Kill B -> B* A  A -> B* A  A
//                                                      \  .
//                                                       A
//
// The test kills process B (node 2), creates a child frame of node 4 in
// process A, and then checks that process B isn't resurrected to create a
// proxy for the new child frame.  See https://crbug.com/476846.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       CreateChildFrameAfterKillingProcess) {
  // Navigate to a page with three frames: one cross-site and two same-site.
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/frame_tree/page_with_three_frames.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   |--Site A ------- proxies for B\n"
      "   +--Site A ------- proxies for B\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/",
      DepictFrameTree(root));
  SiteInstance* b_site_instance =
      root->child_at(0)->current_frame_host()->GetSiteInstance();

  // Kill the first subframe's renderer (B).
  RenderProcessHost* child_process =
      root->child_at(0)->current_frame_host()->GetProcess();
  RenderProcessHostWatcher crash_observer(
      child_process, RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
  child_process->Shutdown(0, false);
  crash_observer.Wait();

  // Add a new child frame to the third subframe.
  RenderFrameHostCreatedObserver frame_observer(shell()->web_contents(), 1);
  EXPECT_TRUE(ExecuteScript(
      root->child_at(2)->current_frame_host(),
      "document.body.appendChild(document.createElement('iframe'));"));
  frame_observer.Wait();

  // The new frame should have a RenderFrameProxyHost for B, but it should not
  // be alive, and B should still not have a process (verified by last line of
  // expected DepictFrameTree output).
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   |--Site A ------- proxies for B\n"
      "   +--Site A ------- proxies for B\n"
      "        +--Site A -- proxies for B\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/ (no process)",
      DepictFrameTree(root));
  FrameTreeNode* grandchild = root->child_at(2)->child_at(0);
  RenderFrameProxyHost* grandchild_rfph =
      grandchild->render_manager()->GetRenderFrameProxyHost(b_site_instance);
  EXPECT_FALSE(grandchild_rfph->is_render_frame_proxy_live());

  // Navigate the second subframe to b.com to recreate process B.
  TestNavigationObserver observer(shell()->web_contents());
  GURL b_url = embedded_test_server()->GetURL("b.com", "/title1.html");
  NavigateFrameToURL(root->child_at(1), b_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(b_url, observer.last_navigation_url());

  // Ensure that the grandchild RenderFrameProxy in B was created when process
  // B was restored.
  EXPECT_TRUE(grandchild_rfph->is_render_frame_proxy_live());
}

// Verify that creating a child frame after killing and reloading an opener
// process doesn't crash. See https://crbug.com/501152.
//   1. Navigate to site A.
//   2. Open a popup with window.open and navigate it cross-process to site B.
//   3. Kill process A for the original tab.
//   4. Reload the original tab to resurrect process A.
//   5. Add a child frame to the top-level frame in the popup tab B.
// In step 5, we try to create proxies for the child frame in all SiteInstances
// for which its parent has proxies.  This includes A.  However, even though
// process A is live (step 4), the parent proxy in A is not live (which was
// incorrectly assumed previously).  This is because step 4 does not resurrect
// proxies for popups opened before the crash.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       CreateChildFrameAfterKillingOpener) {
  GURL main_url(embedded_test_server()->GetURL("a.com", "/title1.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  SiteInstance* site_instance_a = root->current_frame_host()->GetSiteInstance();

  // Open a popup and navigate it cross-process to b.com.
  ShellAddedObserver new_shell_observer;
  EXPECT_TRUE(ExecuteScript(root->current_frame_host(),
                            "popup = window.open('about:blank');"));
  Shell* popup = new_shell_observer.GetShell();
  GURL popup_url(embedded_test_server()->GetURL("b.com", "/title2.html"));
  EXPECT_TRUE(NavigateToURL(popup, popup_url));

  // Verify that each top-level frame has proxies in the other's SiteInstance.
  FrameTreeNode* popup_root =
      static_cast<WebContentsImpl*>(popup->web_contents())
          ->GetFrameTree()
          ->root();
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/",
      DepictFrameTree(root));
  EXPECT_EQ(
      " Site B ------------ proxies for A\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/",
      DepictFrameTree(popup_root));

  // Kill the first window's renderer (a.com).
  RenderProcessHost* child_process = root->current_frame_host()->GetProcess();
  RenderProcessHostWatcher crash_observer(
      child_process, RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
  child_process->Shutdown(0, false);
  crash_observer.Wait();
  EXPECT_FALSE(root->current_frame_host()->IsRenderFrameLive());

  // The proxy for the popup in a.com should've died.
  RenderFrameProxyHost* rfph =
      popup_root->render_manager()->GetRenderFrameProxyHost(site_instance_a);
  EXPECT_FALSE(rfph->is_render_frame_proxy_live());

  // Recreate the a.com renderer.
  EXPECT_TRUE(NavigateToURL(shell(), main_url));
  EXPECT_TRUE(root->current_frame_host()->IsRenderFrameLive());

  // The popup's proxy in a.com should still not be live. Re-navigating the
  // main window to a.com doesn't reinitialize a.com proxies for popups
  // previously opened from the main window.
  EXPECT_FALSE(rfph->is_render_frame_proxy_live());

  // Add a new child frame on the popup.
  RenderFrameHostCreatedObserver frame_observer(popup->web_contents(), 1);
  EXPECT_TRUE(ExecuteScript(
      popup->web_contents(),
      "document.body.appendChild(document.createElement('iframe'));"));
  frame_observer.Wait();

  // Both the child frame's and its parent's proxies should still not be live.
  // The main page can't reach them since it lost reference to the popup after
  // it crashed, so there is no need to create them.
  EXPECT_FALSE(rfph->is_render_frame_proxy_live());
  RenderFrameProxyHost* child_rfph =
      popup_root->child_at(0)->render_manager()->GetRenderFrameProxyHost(
          site_instance_a);
  EXPECT_TRUE(child_rfph);
  EXPECT_FALSE(child_rfph->is_render_frame_proxy_live());
}

// In A-embed-B-embed-C scenario, verify that killing process B clears proxies
// of C from the tree.
//
//     1          A                  A
//    / \        / \                / \    .
//   2   3 ->   B   A -> Kill B -> B*  A
//  /          /
// 4          C
//
// node1 is the root.
// Initially, both node1.proxy_hosts_ and node3.proxy_hosts_ contain C.
// After we kill B, make sure proxies for C are cleared.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       KillingRendererClearsDescendantProxies) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/frame_tree/page_with_two_frames_nested.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(shell()->web_contents())->
          GetFrameTree()->root();
  ASSERT_EQ(2U, root->child_count());

  GURL site_b_url(
      embedded_test_server()->GetURL(
          "bar.com", "/frame_tree/page_with_one_frame.html"));
  // We can't use a TestNavigationObserver to verify the URL here,
  // since the frame has children that may have clobbered it in the observer.
  EXPECT_EQ(site_b_url, root->child_at(0)->current_url());

  // Ensure that a new process is created for node2.
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(),
            root->child_at(0)->current_frame_host()->GetSiteInstance());
  // Ensure that a new process is *not* created for node3.
  EXPECT_EQ(shell()->web_contents()->GetSiteInstance(),
            root->child_at(1)->current_frame_host()->GetSiteInstance());

  ASSERT_EQ(1U, root->child_at(0)->child_count());

  // Make sure node4 points to the correct cross-site-page.
  FrameTreeNode* node4 = root->child_at(0)->child_at(0);
  GURL site_c_url(embedded_test_server()->GetURL("baz.com", "/title1.html"));
  EXPECT_EQ(site_c_url, node4->current_url());

  // |site_instance_c|'s frames and proxies are expected to go away once we kill
  // |child_process_b| below.
  scoped_refptr<SiteInstanceImpl> site_instance_c =
      node4->current_frame_host()->GetSiteInstance();

  // Initially proxies for both B and C will be present in the root.
  EXPECT_EQ(
      " Site A ------------ proxies for B C\n"
      "   |--Site B ------- proxies for A C\n"
      "   |    +--Site C -- proxies for A B\n"
      "   +--Site A ------- proxies for B C\n"
      "Where A = http://a.com/\n"
      "      B = http://bar.com/\n"
      "      C = http://baz.com/",
      DepictFrameTree(root));

  EXPECT_GT(site_instance_c->active_frame_count(), 0U);

  // Kill process B.
  RenderProcessHost* child_process_b =
      root->child_at(0)->current_frame_host()->GetProcess();
  RenderProcessHostWatcher crash_observer(
      child_process_b, RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
  child_process_b->Shutdown(0, false);
  crash_observer.Wait();

  // Make sure proxy C has gone from root.
  // Make sure proxy C has gone from node3 as well.
  // Make sure proxy B stays around in root and node3.
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site A ------- proxies for B\n"
      "Where A = http://a.com/\n"
      "      B = http://bar.com/ (no process)",
      DepictFrameTree(root));

  EXPECT_EQ(0U, site_instance_c->active_frame_count());
}

// Crash a subframe and ensures its children are cleared from the FrameTree.
// See http://crbug.com/338508.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, CrashSubframe) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b)"));
  NavigateToURL(shell(), main_url);

  // Check the subframe process.
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(shell()->web_contents())->
          GetFrameTree()->root();
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/",
      DepictFrameTree(root));
  FrameTreeNode* child = root->child_at(0);
  EXPECT_TRUE(
      child->current_frame_host()->render_view_host()->IsRenderViewLive());
  EXPECT_TRUE(child->current_frame_host()->IsRenderFrameLive());

  // Crash the subframe process.
  RenderProcessHost* root_process = root->current_frame_host()->GetProcess();
  RenderProcessHost* child_process = child->current_frame_host()->GetProcess();
  {
    RenderProcessHostWatcher crash_observer(
        child_process,
        RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
    child_process->Shutdown(0, false);
    crash_observer.Wait();
  }

  // Ensure that the child frame still exists but has been cleared.
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/ (no process)",
      DepictFrameTree(root));
  EXPECT_EQ(1U, root->child_count());
  EXPECT_EQ(main_url, root->current_url());
  EXPECT_EQ(GURL(), child->current_url());

  EXPECT_FALSE(
      child->current_frame_host()->render_view_host()->IsRenderViewLive());
  EXPECT_FALSE(child->current_frame_host()->IsRenderFrameLive());
  EXPECT_FALSE(child->current_frame_host()->render_frame_created_);

  // Now crash the top-level page to clear the child frame.
  {
    RenderProcessHostWatcher crash_observer(
        root_process,
        RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
    root_process->Shutdown(0, false);
    crash_observer.Wait();
  }
  EXPECT_EQ(0U, root->child_count());
  EXPECT_EQ(GURL(), root->current_url());
}

// When a new subframe is added, related SiteInstances that can reach the
// subframe should create proxies for it (https://crbug.com/423587).  This test
// checks that if A embeds B and later adds a new subframe A2, A2 gets a proxy
// in B's process.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, CreateProxiesForNewFrames) {
  GURL main_url(embedded_test_server()->GetURL(
      "b.com", "/frame_tree/page_with_one_frame.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  ASSERT_EQ(1U, root->child_count());

  // Make sure the frame starts out at the correct cross-site URL.
  EXPECT_EQ(embedded_test_server()->GetURL("baz.com", "/title1.html"),
            root->child_at(0)->current_url());

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://b.com/\n"
      "      B = http://baz.com/",
      DepictFrameTree(root));

  // Add a new child frame to the top-level frame.
  RenderFrameHostCreatedObserver frame_observer(shell()->web_contents(), 1);
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(),
                            "window.domAutomationController.send("
                            "    addFrame('data:text/html,foo'));"));
  frame_observer.Wait();

  // The new frame should have a proxy in Site B, for use by the old frame.
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site A ------- proxies for B\n"
      "Where A = http://b.com/\n"
      "      B = http://baz.com/",
      DepictFrameTree(root));
}

// TODO(nasko): Disable this test until out-of-process iframes is ready and the
// security checks are back in place.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       DISABLED_CrossSiteIframeRedirectOnce) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(https_server.Start());

  GURL main_url(embedded_test_server()->GetURL("/site_per_process_main.html"));
  GURL http_url(embedded_test_server()->GetURL("/title1.html"));
  GURL https_url(https_server.GetURL("/title1.html"));

  NavigateToURL(shell(), main_url);

  TestNavigationObserver observer(shell()->web_contents());
  {
    // Load cross-site client-redirect page into Iframe.
    // Should be blocked.
    GURL client_redirect_https_url(
        https_server.GetURL("/client-redirect?/title1.html"));
    EXPECT_TRUE(NavigateIframeToURL(shell()->web_contents(), "test",
                                    client_redirect_https_url));
    // DidFailProvisionalLoad when navigating to client_redirect_https_url.
    EXPECT_EQ(observer.last_navigation_url(), client_redirect_https_url);
    EXPECT_FALSE(observer.last_navigation_succeeded());
  }

  {
    // Load cross-site server-redirect page into Iframe,
    // which redirects to same-site page.
    GURL server_redirect_http_url(
        https_server.GetURL("/server-redirect?" + http_url.spec()));
    EXPECT_TRUE(NavigateIframeToURL(shell()->web_contents(), "test",
                                    server_redirect_http_url));
    EXPECT_EQ(observer.last_navigation_url(), http_url);
    EXPECT_TRUE(observer.last_navigation_succeeded());
  }

  {
    // Load cross-site server-redirect page into Iframe,
    // which redirects to cross-site page.
    GURL server_redirect_http_url(
        https_server.GetURL("/server-redirect?/title1.html"));
    EXPECT_TRUE(NavigateIframeToURL(shell()->web_contents(), "test",
                                    server_redirect_http_url));
    // DidFailProvisionalLoad when navigating to https_url.
    EXPECT_EQ(observer.last_navigation_url(), https_url);
    EXPECT_FALSE(observer.last_navigation_succeeded());
  }

  {
    // Load same-site server-redirect page into Iframe,
    // which redirects to cross-site page.
    GURL server_redirect_http_url(
        embedded_test_server()->GetURL("/server-redirect?" + https_url.spec()));
    EXPECT_TRUE(NavigateIframeToURL(shell()->web_contents(), "test",
                                    server_redirect_http_url));

    EXPECT_EQ(observer.last_navigation_url(), https_url);
    EXPECT_FALSE(observer.last_navigation_succeeded());
   }

  {
    // Load same-site client-redirect page into Iframe,
    // which redirects to cross-site page.
    GURL client_redirect_http_url(
        embedded_test_server()->GetURL("/client-redirect?" + https_url.spec()));

    RedirectNotificationObserver load_observer2(
        NOTIFICATION_LOAD_STOP,
        Source<NavigationController>(
            &shell()->web_contents()->GetController()));

    EXPECT_TRUE(NavigateIframeToURL(shell()->web_contents(), "test",
                                    client_redirect_http_url));

    // Same-site Client-Redirect Page should be loaded successfully.
    EXPECT_EQ(observer.last_navigation_url(), client_redirect_http_url);
    EXPECT_TRUE(observer.last_navigation_succeeded());

    // Redirecting to Cross-site Page should be blocked.
    load_observer2.Wait();
    EXPECT_EQ(observer.last_navigation_url(), https_url);
    EXPECT_FALSE(observer.last_navigation_succeeded());
  }

  {
    // Load same-site server-redirect page into Iframe,
    // which redirects to same-site page.
    GURL server_redirect_http_url(
        embedded_test_server()->GetURL("/server-redirect?/title1.html"));
    EXPECT_TRUE(NavigateIframeToURL(shell()->web_contents(), "test",
                                    server_redirect_http_url));
    EXPECT_EQ(observer.last_navigation_url(), http_url);
    EXPECT_TRUE(observer.last_navigation_succeeded());
   }

  {
    // Load same-site client-redirect page into Iframe,
    // which redirects to same-site page.
    GURL client_redirect_http_url(
        embedded_test_server()->GetURL("/client-redirect?" + http_url.spec()));
    RedirectNotificationObserver load_observer2(
        NOTIFICATION_LOAD_STOP,
        Source<NavigationController>(
            &shell()->web_contents()->GetController()));

    EXPECT_TRUE(NavigateIframeToURL(shell()->web_contents(), "test",
                                    client_redirect_http_url));

    // Same-site Client-Redirect Page should be loaded successfully.
    EXPECT_EQ(observer.last_navigation_url(), client_redirect_http_url);
    EXPECT_TRUE(observer.last_navigation_succeeded());

    // Redirecting to Same-site Page should be loaded successfully.
    load_observer2.Wait();
    EXPECT_EQ(observer.last_navigation_url(), http_url);
    EXPECT_TRUE(observer.last_navigation_succeeded());
  }
}

// TODO(nasko): Disable this test until out-of-process iframes is ready and the
// security checks are back in place.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       DISABLED_CrossSiteIframeRedirectTwice) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(https_server.Start());

  GURL main_url(embedded_test_server()->GetURL("/site_per_process_main.html"));
  GURL http_url(embedded_test_server()->GetURL("/title1.html"));
  GURL https_url(https_server.GetURL("/title1.html"));

  NavigateToURL(shell(), main_url);

  TestNavigationObserver observer(shell()->web_contents());
  {
    // Load client-redirect page pointing to a cross-site client-redirect page,
    // which eventually redirects back to same-site page.
    GURL client_redirect_https_url(
        https_server.GetURL("/client-redirect?" + http_url.spec()));
    GURL client_redirect_http_url(embedded_test_server()->GetURL(
        "/client-redirect?" + client_redirect_https_url.spec()));

    // We should wait until second client redirect get cancelled.
    RedirectNotificationObserver load_observer2(
        NOTIFICATION_LOAD_STOP,
        Source<NavigationController>(
            &shell()->web_contents()->GetController()));

    EXPECT_TRUE(NavigateIframeToURL(shell()->web_contents(), "test",
                                    client_redirect_http_url));

    // DidFailProvisionalLoad when navigating to client_redirect_https_url.
    load_observer2.Wait();
    EXPECT_EQ(observer.last_navigation_url(), client_redirect_https_url);
    EXPECT_FALSE(observer.last_navigation_succeeded());
  }

  {
    // Load server-redirect page pointing to a cross-site server-redirect page,
    // which eventually redirect back to same-site page.
    GURL server_redirect_https_url(
        https_server.GetURL("/server-redirect?" + http_url.spec()));
    GURL server_redirect_http_url(embedded_test_server()->GetURL(
        "/server-redirect?" + server_redirect_https_url.spec()));
    EXPECT_TRUE(NavigateIframeToURL(shell()->web_contents(), "test",
                                    server_redirect_http_url));
    EXPECT_EQ(observer.last_navigation_url(), http_url);
    EXPECT_TRUE(observer.last_navigation_succeeded());
  }

  {
    // Load server-redirect page pointing to a cross-site server-redirect page,
    // which eventually redirects back to cross-site page.
    GURL server_redirect_https_url(
        https_server.GetURL("/server-redirect?" + https_url.spec()));
    GURL server_redirect_http_url(embedded_test_server()->GetURL(
        "/server-redirect?" + server_redirect_https_url.spec()));
    EXPECT_TRUE(NavigateIframeToURL(shell()->web_contents(), "test",
                                    server_redirect_http_url));

    // DidFailProvisionalLoad when navigating to https_url.
    EXPECT_EQ(observer.last_navigation_url(), https_url);
    EXPECT_FALSE(observer.last_navigation_succeeded());
  }

  {
    // Load server-redirect page pointing to a cross-site client-redirect page,
    // which eventually redirects back to same-site page.
    GURL client_redirect_http_url(
        https_server.GetURL("/client-redirect?" + http_url.spec()));
    GURL server_redirect_http_url(embedded_test_server()->GetURL(
        "/server-redirect?" + client_redirect_http_url.spec()));
    EXPECT_TRUE(NavigateIframeToURL(shell()->web_contents(), "test",
                                    server_redirect_http_url));

    // DidFailProvisionalLoad when navigating to client_redirect_http_url.
    EXPECT_EQ(observer.last_navigation_url(), client_redirect_http_url);
    EXPECT_FALSE(observer.last_navigation_succeeded());
  }
}

// Ensure that when navigating a frame cross-process RenderFrameProxyHosts are
// created in the FrameTree skipping the subtree of the navigating frame.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       ProxyCreationSkipsSubtree) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a,a(a,a(a)))"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(shell()->web_contents())->
          GetFrameTree()->root();

  EXPECT_TRUE(root->child_at(1) != NULL);
  EXPECT_EQ(2U, root->child_at(1)->child_count());

  {
    // Load same-site page into iframe.
    TestNavigationObserver observer(shell()->web_contents());
    GURL http_url(embedded_test_server()->GetURL("a.com", "/title1.html"));
    NavigateFrameToURL(root->child_at(0), http_url);
    EXPECT_EQ(http_url, observer.last_navigation_url());
    EXPECT_TRUE(observer.last_navigation_succeeded());
    EXPECT_EQ(
        " Site A\n"
        "   |--Site A\n"
        "   +--Site A\n"
        "        |--Site A\n"
        "        +--Site A\n"
        "             +--Site A\n"
        "Where A = http://a.com/",
        DepictFrameTree(root));
  }

  // Create the cross-site URL to navigate to.
  GURL cross_site_url =
      embedded_test_server()->GetURL("foo.com", "/frame_tree/title2.html");

  // Load cross-site page into the second iframe without waiting for the
  // navigation to complete. Once LoadURLWithParams returns, we would expect
  // proxies to have been created in the frame tree, but children of the
  // navigating frame to still be present. The reason is that we don't run the
  // message loop, so no IPCs that alter the frame tree can be processed.
  FrameTreeNode* child = root->child_at(1);
  SiteInstance* site = NULL;
  bool browser_side_navigation = IsBrowserSideNavigationEnabled();
  std::string cross_site_rfh_type =
      browser_side_navigation ? "speculative" : "pending";
  {
    TestNavigationObserver observer(shell()->web_contents());
    TestFrameNavigationObserver navigation_observer(child);
    NavigationController::LoadURLParams params(cross_site_url);
    params.transition_type = PageTransitionFromInt(ui::PAGE_TRANSITION_LINK);
    params.frame_tree_node_id = child->frame_tree_node_id();
    child->navigator()->GetController()->LoadURLWithParams(params);

    if (browser_side_navigation) {
      site = child->render_manager()
                 ->speculative_frame_host()
                 ->GetSiteInstance();
    } else {
      site = child->render_manager()->pending_frame_host()->GetSiteInstance();
    }
    EXPECT_NE(shell()->web_contents()->GetSiteInstance(), site);

    std::string tree = base::StringPrintf(
        " Site A ------------ proxies for B\n"
        "   |--Site A ------- proxies for B\n"
        "   +--Site A (B %s)\n"
        "        |--Site A\n"
        "        +--Site A\n"
        "             +--Site A\n"
        "Where A = http://a.com/\n"
        "      B = http://foo.com/",
        cross_site_rfh_type.c_str());
    EXPECT_EQ(tree, DepictFrameTree(root));

    // Now that the verification is done, run the message loop and wait for the
    // navigation to complete.
    navigation_observer.Wait();
    EXPECT_FALSE(child->render_manager()->pending_frame_host());
    EXPECT_TRUE(observer.last_navigation_succeeded());
    EXPECT_EQ(cross_site_url, observer.last_navigation_url());

    EXPECT_EQ(
        " Site A ------------ proxies for B\n"
        "   |--Site A ------- proxies for B\n"
        "   +--Site B ------- proxies for A\n"
        "Where A = http://a.com/\n"
        "      B = http://foo.com/",
        DepictFrameTree(root));
  }

  // Load another cross-site page into the same iframe.
  cross_site_url = embedded_test_server()->GetURL("bar.com", "/title3.html");
  {
    // Perform the same checks as the first cross-site navigation, since
    // there have been issues in subsequent cross-site navigations. Also ensure
    // that the SiteInstance has properly changed.
    // TODO(nasko): Once we have proper cleanup of resources, add code to
    // verify that the intermediate SiteInstance/RenderFrameHost have been
    // properly cleaned up.
    TestNavigationObserver observer(shell()->web_contents());
    TestFrameNavigationObserver navigation_observer(child);
    NavigationController::LoadURLParams params(cross_site_url);
    params.transition_type = PageTransitionFromInt(ui::PAGE_TRANSITION_LINK);
    params.frame_tree_node_id = child->frame_tree_node_id();
    child->navigator()->GetController()->LoadURLWithParams(params);

    SiteInstance* site2;
    if (browser_side_navigation) {
      site2 = child->render_manager()
                  ->speculative_frame_host()
                  ->GetSiteInstance();
    } else {
      site2 = child->render_manager()->pending_frame_host()->GetSiteInstance();
    }
    EXPECT_NE(shell()->web_contents()->GetSiteInstance(), site2);
    EXPECT_NE(site, site2);

    std::string tree = base::StringPrintf(
        " Site A ------------ proxies for B C\n"
        "   |--Site A ------- proxies for B C\n"
        "   +--Site B (C %s) -- proxies for A\n"
        "Where A = http://a.com/\n"
        "      B = http://foo.com/\n"
        "      C = http://bar.com/",
        cross_site_rfh_type.c_str());
    EXPECT_EQ(tree, DepictFrameTree(root));

    navigation_observer.Wait();
    EXPECT_TRUE(observer.last_navigation_succeeded());
    EXPECT_EQ(cross_site_url, observer.last_navigation_url());
    EXPECT_EQ(0U, child->child_count());
  }
}

// Verify that "scrolling" property on frame elements propagates to child frames
// correctly.
// Does not work on android since android has scrollbars overlayed.
#if defined(OS_ANDROID)
#define MAYBE_FrameOwnerPropertiesPropagationScrolling \
        DISABLED_FrameOwnerPropertiesPropagationScrolling
#else
#define MAYBE_FrameOwnerPropertiesPropagationScrolling \
        FrameOwnerPropertiesPropagationScrolling
#endif
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       MAYBE_FrameOwnerPropertiesPropagationScrolling) {
#if defined(OS_MACOSX)
  ui::test::ScopedPreferredScrollerStyle scroller_style_override(false);
#endif
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/frame_owner_properties_scrolling.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  ASSERT_EQ(1u, root->child_count());

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/",
      DepictFrameTree(root));

  FrameTreeNode* child = root->child_at(0);

  // If the available client width within the iframe is smaller than the
  // frame element's width, we assume there's a scrollbar.
  // Also note that just comparing clientHeight and scrollHeight of the frame's
  // document will not work.
  auto has_scrollbar = [](RenderFrameHostImpl* rfh) {
    int client_width;
    EXPECT_TRUE(ExecuteScriptAndExtractInt(rfh,
        "window.domAutomationController.send(document.body.clientWidth);",
        &client_width));
    const int kFrameElementWidth = 200;
    return client_width < kFrameElementWidth;
  };

  auto set_scrolling_property = [](RenderFrameHostImpl* parent_rfh,
                                   const std::string& value) {
    EXPECT_TRUE(ExecuteScript(
        parent_rfh,
        base::StringPrintf(
            "document.getElementById('child-1').setAttribute("
            "    'scrolling', '%s');", value.c_str())));
  };

  // Run the test over variety of parent/child cases.
  GURL urls[] = {
    // Remote to remote.
    embedded_test_server()->GetURL("c.com", "/tall_page.html"),
    // Remote to local.
    embedded_test_server()->GetURL("a.com", "/tall_page.html"),
    // Local to remote.
    embedded_test_server()->GetURL("b.com", "/tall_page.html")
  };
  const std::string scrolling_values[] = {
    "yes", "auto", "no"
  };

  for (size_t i = 0; i < arraysize(scrolling_values); ++i) {
    bool expect_scrollbar = scrolling_values[i] != "no";
    set_scrolling_property(root->current_frame_host(), scrolling_values[i]);
    for (size_t j = 0; j < arraysize(urls); ++j) {
      NavigateFrameToURL(child, urls[j]);

      // TODO(alexmos): This can be removed once TestFrameNavigationObserver is
      // fixed to use DidFinishLoad.
      EXPECT_TRUE(WaitForRenderFrameReady(child->current_frame_host()));

      EXPECT_EQ(expect_scrollbar, has_scrollbar(child->current_frame_host()));
    }
  }
}

// Verify that "marginwidth" and "marginheight" properties on frame elements
// propagate to child frames correctly.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       FrameOwnerPropertiesPropagationMargin) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/frame_owner_properties_margin.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  ASSERT_EQ(1u, root->child_count());

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/",
      DepictFrameTree(root));

  FrameTreeNode* child = root->child_at(0);

  std::string margin_width;
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      child->current_frame_host(),
      "window.domAutomationController.send("
      "document.body.getAttribute('marginwidth'));",
      &margin_width));
  EXPECT_EQ("10", margin_width);

  std::string margin_height;
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      child->current_frame_host(),
      "window.domAutomationController.send("
      "document.body.getAttribute('marginheight'));",
      &margin_height));
  EXPECT_EQ("50", margin_height);

  // Run the test over variety of parent/child cases.
  GURL urls[] = {
    // Remote to remote.
    embedded_test_server()->GetURL("c.com", "/title2.html"),
    // Remote to local.
    embedded_test_server()->GetURL("a.com", "/title1.html"),
    // Local to remote.
    embedded_test_server()->GetURL("b.com", "/title2.html")
  };

  int current_margin_width = 15;
  int current_margin_height = 25;

  // Before each navigation, we change the marginwidth and marginheight
  // properties of the frame. We then check whether those properties are applied
  // correctly after the navigation has completed.
  for (size_t i = 0; i < arraysize(urls); ++i) {
    // Change marginwidth and marginheight before navigating.
    EXPECT_TRUE(ExecuteScript(
        root->current_frame_host(),
        base::StringPrintf(
            "document.getElementById('child-1').setAttribute("
            "    'marginwidth', '%d');", current_margin_width)));
    EXPECT_TRUE(ExecuteScript(
        root->current_frame_host(),
        base::StringPrintf(
            "document.getElementById('child-1').setAttribute("
            "    'marginheight', '%d');", current_margin_height)));

    NavigateFrameToURL(child, urls[i]);
    // TODO(alexmos): This can be removed once TestFrameNavigationObserver is
    // fixed to use DidFinishLoad.
    EXPECT_TRUE(WaitForRenderFrameReady(child->current_frame_host()));

    std::string actual_margin_width;
    EXPECT_TRUE(ExecuteScriptAndExtractString(
        child->current_frame_host(),
        "window.domAutomationController.send("
        "document.body.getAttribute('marginwidth'));",
        &actual_margin_width));
    EXPECT_EQ(base::IntToString(current_margin_width), actual_margin_width);

    std::string actual_margin_height;
    EXPECT_TRUE(ExecuteScriptAndExtractString(
        child->current_frame_host(),
        "window.domAutomationController.send("
        "document.body.getAttribute('marginheight'));",
        &actual_margin_height));
    EXPECT_EQ(base::IntToString(current_margin_height), actual_margin_height);

    current_margin_width += 5;
    current_margin_height += 10;
  }
}

// Verify origin replication with an A-embed-B-embed-C-embed-A hierarchy.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, OriginReplication) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b(c(a),b), a)"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  EXPECT_EQ(
      " Site A ------------ proxies for B C\n"
      "   |--Site B ------- proxies for A C\n"       // tiptop_child
      "   |    |--Site C -- proxies for A B\n"       // middle_child
      "   |    |    +--Site A -- proxies for B C\n"  // lowest_child
      "   |    +--Site B -- proxies for A C\n"
      "   +--Site A ------- proxies for B C\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/\n"
      "      C = http://c.com/",
      DepictFrameTree(root));

  std::string a_origin = embedded_test_server()->GetURL("a.com", "/").spec();
  std::string b_origin = embedded_test_server()->GetURL("b.com", "/").spec();
  std::string c_origin = embedded_test_server()->GetURL("c.com", "/").spec();
  FrameTreeNode* tiptop_child = root->child_at(0);
  FrameTreeNode* middle_child = root->child_at(0)->child_at(0);
  FrameTreeNode* lowest_child = root->child_at(0)->child_at(0)->child_at(0);

  // Check that b.com frame's location.ancestorOrigins contains the correct
  // origin for the parent.  The origin should have been replicated as part of
  // the ViewMsg_New message that created the parent's RenderFrameProxy in
  // b.com's process.
  int ancestor_origins_length = 0;
  EXPECT_TRUE(ExecuteScriptAndExtractInt(
      tiptop_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins.length);",
      &ancestor_origins_length));
  EXPECT_EQ(1, ancestor_origins_length);
  std::string result;
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      tiptop_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins[0]);",
      &result));
  EXPECT_EQ(a_origin, result + "/");

  // Check that c.com frame's location.ancestorOrigins contains the correct
  // origin for its two ancestors. The topmost parent origin should be
  // replicated as part of ViewMsg_New, and the middle frame (b.com's) origin
  // should be replicated as part of FrameMsg_NewFrameProxy sent for b.com's
  // frame in c.com's process.
  EXPECT_TRUE(ExecuteScriptAndExtractInt(
      middle_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins.length);",
      &ancestor_origins_length));
  EXPECT_EQ(2, ancestor_origins_length);
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      middle_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins[0]);",
      &result));
  EXPECT_EQ(b_origin, result + "/");
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      middle_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins[1]);",
      &result));
  EXPECT_EQ(a_origin, result + "/");

  // Check that the nested a.com frame's location.ancestorOrigins contains the
  // correct origin for its three ancestors.
  EXPECT_TRUE(ExecuteScriptAndExtractInt(
      lowest_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins.length);",
      &ancestor_origins_length));
  EXPECT_EQ(3, ancestor_origins_length);
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      lowest_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins[0]);",
      &result));
  EXPECT_EQ(c_origin, result + "/");
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      lowest_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins[1]);",
      &result));
  EXPECT_EQ(b_origin, result + "/");
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      lowest_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins[2]);",
      &result));
  EXPECT_EQ(a_origin, result + "/");
}

// Check that iframe sandbox flags are replicated correctly.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, SandboxFlagsReplication) {
  GURL main_url(embedded_test_server()->GetURL("/sandboxed_frames.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  TestNavigationObserver observer(shell()->web_contents());

  // Navigate the second (sandboxed) subframe to a cross-site page with a
  // subframe.
  GURL foo_url(
      embedded_test_server()->GetURL("foo.com", "/frame_tree/1-1.html"));
  NavigateFrameToURL(root->child_at(1), foo_url);
  EXPECT_TRUE(WaitForLoadStop(shell()->web_contents()));

  // We can't use a TestNavigationObserver to verify the URL here,
  // since the frame has children that may have clobbered it in the observer.
  EXPECT_EQ(foo_url, root->child_at(1)->current_url());

  // Load cross-site page into subframe's subframe.
  ASSERT_EQ(2U, root->child_at(1)->child_count());
  GURL bar_url(embedded_test_server()->GetURL("bar.com", "/title1.html"));
  NavigateFrameToURL(root->child_at(1)->child_at(0), bar_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(bar_url, observer.last_navigation_url());

  // Opening a popup in the sandboxed foo.com iframe should fail.
  bool success = false;
  EXPECT_TRUE(
      ExecuteScriptAndExtractBool(root->child_at(1)->current_frame_host(),
                                  "window.domAutomationController.send("
                                  "!window.open('data:text/html,dataurl'));",
                                  &success));
  EXPECT_TRUE(success);
  EXPECT_EQ(1u, Shell::windows().size());

  // Opening a popup in a frame whose parent is sandboxed should also fail.
  // Here, bar.com frame's sandboxed parent frame is a remote frame in
  // bar.com's process.
  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      root->child_at(1)->child_at(0)->current_frame_host(),
      "window.domAutomationController.send("
      "!window.open('data:text/html,dataurl'));",
      &success));
  EXPECT_TRUE(success);
  EXPECT_EQ(1u, Shell::windows().size());

  // Same, but now try the case where bar.com frame's sandboxed parent is a
  // local frame in bar.com's process.
  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      root->child_at(2)->child_at(0)->current_frame_host(),
      "window.domAutomationController.send("
      "!window.open('data:text/html,dataurl'));",
      &success));
  EXPECT_TRUE(success);
  EXPECT_EQ(1u, Shell::windows().size());

  // Check that foo.com frame's location.ancestorOrigins contains the correct
  // origin for the parent, which should be unaffected by sandboxing.
  int ancestor_origins_length = 0;
  EXPECT_TRUE(ExecuteScriptAndExtractInt(
      root->child_at(1)->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins.length);",
      &ancestor_origins_length));
  EXPECT_EQ(1, ancestor_origins_length);
  std::string result;
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      root->child_at(1)->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins[0]);",
      &result));
  EXPECT_EQ(result + "/", main_url.GetOrigin().spec());

  // Now check location.ancestorOrigins for the bar.com frame. The middle frame
  // (foo.com's) origin should be unique, since that frame is sandboxed, and
  // the top frame should match |main_url|.
  FrameTreeNode* bottom_child = root->child_at(1)->child_at(0);
  EXPECT_TRUE(ExecuteScriptAndExtractInt(
      bottom_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins.length);",
      &ancestor_origins_length));
  EXPECT_EQ(2, ancestor_origins_length);
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      bottom_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins[0]);",
      &result));
  EXPECT_EQ("null", result);
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      bottom_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins[1]);",
      &result));
  EXPECT_EQ(main_url.GetOrigin().spec(), result + "/");
}

// Check that dynamic updates to iframe sandbox flags are propagated correctly.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, DynamicSandboxFlags) {
  GURL main_url(
      embedded_test_server()->GetURL("/frame_tree/page_with_two_frames.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  TestNavigationObserver observer(shell()->web_contents());
  ASSERT_EQ(2U, root->child_count());

  // Make sure first frame starts out at the correct cross-site page.
  EXPECT_EQ(embedded_test_server()->GetURL("bar.com", "/title1.html"),
            root->child_at(0)->current_url());

  // Navigate second frame to another cross-site page.
  GURL baz_url(embedded_test_server()->GetURL("baz.com", "/title1.html"));
  NavigateFrameToURL(root->child_at(1), baz_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(baz_url, observer.last_navigation_url());

  // Both frames should not be sandboxed to start with.
  EXPECT_EQ(blink::WebSandboxFlags::None,
            root->child_at(0)->pending_sandbox_flags());
  EXPECT_EQ(blink::WebSandboxFlags::None,
            root->child_at(0)->effective_sandbox_flags());
  EXPECT_EQ(blink::WebSandboxFlags::None,
            root->child_at(1)->pending_sandbox_flags());
  EXPECT_EQ(blink::WebSandboxFlags::None,
            root->child_at(1)->effective_sandbox_flags());

  // Dynamically update sandbox flags for the first frame.
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(),
                            "window.domAutomationController.send("
                            "document.querySelector('iframe').sandbox="
                            "'allow-scripts');"));

  // Check that updated sandbox flags are propagated to browser process.
  // The new flags should be reflected in pending_sandbox_flags(), while
  // effective_sandbox_flags() should still reflect the old flags, because
  // sandbox flag updates take place only after navigations. "allow-scripts"
  // resets both SandboxFlags::Scripts and SandboxFlags::AutomaticFeatures bits
  // per blink::parseSandboxPolicy().
  blink::WebSandboxFlags expected_flags =
      blink::WebSandboxFlags::All & ~blink::WebSandboxFlags::Scripts &
      ~blink::WebSandboxFlags::AutomaticFeatures;
  EXPECT_EQ(expected_flags, root->child_at(0)->pending_sandbox_flags());
  EXPECT_EQ(blink::WebSandboxFlags::None,
            root->child_at(0)->effective_sandbox_flags());

  // Navigate the first frame to a page on the same site.  The new sandbox
  // flags should take effect.
  GURL bar_url(
      embedded_test_server()->GetURL("bar.com", "/frame_tree/2-4.html"));
  NavigateFrameToURL(root->child_at(0), bar_url);
  // (The new page has a subframe; wait for it to load as well.)
  ASSERT_TRUE(WaitForLoadStop(shell()->web_contents()));
  EXPECT_EQ(bar_url, root->child_at(0)->current_url());
  ASSERT_EQ(1U, root->child_at(0)->child_count());

  EXPECT_EQ(
      " Site A ------------ proxies for B C\n"
      "   |--Site B ------- proxies for A C\n"
      "   |    +--Site B -- proxies for A C\n"
      "   +--Site C ------- proxies for A B\n"
      "Where A = http://127.0.0.1/\n"
      "      B = http://bar.com/\n"
      "      C = http://baz.com/",
      DepictFrameTree(root));

  // Confirm that the browser process has updated the frame's current sandbox
  // flags.
  EXPECT_EQ(expected_flags, root->child_at(0)->pending_sandbox_flags());
  EXPECT_EQ(expected_flags, root->child_at(0)->effective_sandbox_flags());

  // Opening a popup in the now-sandboxed frame should fail.
  bool success = false;
  EXPECT_TRUE(
      ExecuteScriptAndExtractBool(root->child_at(0)->current_frame_host(),
                                  "window.domAutomationController.send("
                                  "!window.open('data:text/html,dataurl'));",
                                  &success));
  EXPECT_TRUE(success);
  EXPECT_EQ(1u, Shell::windows().size());

  // Navigate the child of the now-sandboxed frame to a page on baz.com.  The
  // child should inherit the latest sandbox flags from its parent frame, which
  // is currently a proxy in baz.com's renderer process.  This checks that the
  // proxies of |root->child_at(0)| were also updated with the latest sandbox
  // flags.
  GURL baz_child_url(embedded_test_server()->GetURL("baz.com", "/title2.html"));
  NavigateFrameToURL(root->child_at(0)->child_at(0), baz_child_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(baz_child_url, observer.last_navigation_url());

  EXPECT_EQ(
      " Site A ------------ proxies for B C\n"
      "   |--Site B ------- proxies for A C\n"
      "   |    +--Site C -- proxies for A B\n"
      "   +--Site C ------- proxies for A B\n"
      "Where A = http://127.0.0.1/\n"
      "      B = http://bar.com/\n"
      "      C = http://baz.com/",
      DepictFrameTree(root));

  // Opening a popup in the child of a sandboxed frame should fail.
  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      root->child_at(0)->child_at(0)->current_frame_host(),
      "window.domAutomationController.send("
      "!window.open('data:text/html,dataurl'));",
      &success));
  EXPECT_TRUE(success);
  EXPECT_EQ(1u, Shell::windows().size());

  // Child of a sandboxed frame should also be sandboxed on the browser side.
  EXPECT_EQ(expected_flags,
            root->child_at(0)->child_at(0)->effective_sandbox_flags());
}

// Check that dynamic updates to iframe sandbox flags are propagated correctly.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       DynamicSandboxFlagsRemoteToLocal) {
  GURL main_url(
      embedded_test_server()->GetURL("/frame_tree/page_with_two_frames.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  TestNavigationObserver observer(shell()->web_contents());
  ASSERT_EQ(2U, root->child_count());

  // Make sure the two frames starts out at correct URLs.
  EXPECT_EQ(embedded_test_server()->GetURL("bar.com", "/title1.html"),
            root->child_at(0)->current_url());
  EXPECT_EQ(embedded_test_server()->GetURL("/title1.html"),
            root->child_at(1)->current_url());

  // Update the second frame's sandbox flags.
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(),
                            "window.domAutomationController.send("
                            "document.querySelectorAll('iframe')[1].sandbox="
                            "'allow-scripts');"));

  // Check that the current sandbox flags are updated but the effective
  // sandbox flags are not.
  blink::WebSandboxFlags expected_flags =
      blink::WebSandboxFlags::All & ~blink::WebSandboxFlags::Scripts &
      ~blink::WebSandboxFlags::AutomaticFeatures;
  EXPECT_EQ(expected_flags, root->child_at(1)->pending_sandbox_flags());
  EXPECT_EQ(blink::WebSandboxFlags::None,
            root->child_at(1)->effective_sandbox_flags());

  // Navigate the second subframe to a page on bar.com.  This will trigger a
  // remote-to-local frame swap in bar.com's process.  The target page has
  // another frame, so use TestFrameNavigationObserver to wait for all frames
  // to be loaded.
  TestFrameNavigationObserver frame_observer(root->child_at(1), 2);
  GURL bar_url(embedded_test_server()->GetURL(
      "bar.com", "/frame_tree/page_with_one_frame.html"));
  NavigateFrameToURL(root->child_at(1), bar_url);
  frame_observer.Wait();
  EXPECT_EQ(bar_url, root->child_at(1)->current_url());
  ASSERT_EQ(1U, root->child_at(1)->child_count());

  // Confirm that the browser process has updated the current sandbox flags.
  EXPECT_EQ(expected_flags, root->child_at(1)->pending_sandbox_flags());
  EXPECT_EQ(expected_flags, root->child_at(1)->effective_sandbox_flags());

  // Opening a popup in the sandboxed second frame should fail.
  bool success = false;
  EXPECT_TRUE(
      ExecuteScriptAndExtractBool(root->child_at(1)->current_frame_host(),
                                  "window.domAutomationController.send("
                                  "!window.open('data:text/html,dataurl'));",
                                  &success));
  EXPECT_TRUE(success);
  EXPECT_EQ(1u, Shell::windows().size());

  // Make sure that the child frame inherits the sandbox flags of its
  // now-sandboxed parent frame.
  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      root->child_at(1)->child_at(0)->current_frame_host(),
      "window.domAutomationController.send("
      "!window.open('data:text/html,dataurl'));",
      &success));
  EXPECT_TRUE(success);
  EXPECT_EQ(1u, Shell::windows().size());
}

// Check that dynamic updates to iframe sandbox flags are propagated correctly.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       DynamicSandboxFlagsRendererInitiatedNavigation) {
  GURL main_url(
      embedded_test_server()->GetURL("/frame_tree/page_with_one_frame.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  TestNavigationObserver observer(shell()->web_contents());
  ASSERT_EQ(1U, root->child_count());

  // Make sure the frame starts out at the correct cross-site page.
  EXPECT_EQ(embedded_test_server()->GetURL("baz.com", "/title1.html"),
            root->child_at(0)->current_url());

  // The frame should not be sandboxed to start with.
  EXPECT_EQ(blink::WebSandboxFlags::None,
            root->child_at(0)->pending_sandbox_flags());
  EXPECT_EQ(blink::WebSandboxFlags::None,
            root->child_at(0)->effective_sandbox_flags());

  // Dynamically update the frame's sandbox flags.
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(),
                            "window.domAutomationController.send("
                            "document.querySelector('iframe').sandbox="
                            "'allow-scripts');"));

  // Check that updated sandbox flags are propagated to browser process.
  // The new flags should be set in pending_sandbox_flags(), while
  // effective_sandbox_flags() should still reflect the old flags, because
  // sandbox flag updates take place only after navigations. "allow-scripts"
  // resets both SandboxFlags::Scripts and SandboxFlags::AutomaticFeatures bits
  // per blink::parseSandboxPolicy().
  blink::WebSandboxFlags expected_flags =
      blink::WebSandboxFlags::All & ~blink::WebSandboxFlags::Scripts &
      ~blink::WebSandboxFlags::AutomaticFeatures;
  EXPECT_EQ(expected_flags, root->child_at(0)->pending_sandbox_flags());
  EXPECT_EQ(blink::WebSandboxFlags::None,
            root->child_at(0)->effective_sandbox_flags());

  // Perform a renderer-initiated same-site navigation in the first frame. The
  // new sandbox flags should take effect.
  TestFrameNavigationObserver frame_observer(root->child_at(0));
  ASSERT_TRUE(ExecuteScript(root->child_at(0)->current_frame_host(),
                            "window.location.href='/title2.html'"));
  frame_observer.Wait();
  EXPECT_EQ(embedded_test_server()->GetURL("baz.com", "/title2.html"),
            root->child_at(0)->current_url());

  // Confirm that the browser process has updated the frame's current sandbox
  // flags.
  EXPECT_EQ(expected_flags, root->child_at(0)->pending_sandbox_flags());
  EXPECT_EQ(expected_flags, root->child_at(0)->effective_sandbox_flags());

  // Opening a popup in the now-sandboxed frame should fail.
  bool success = false;
  EXPECT_TRUE(
      ExecuteScriptAndExtractBool(root->child_at(0)->current_frame_host(),
                                  "window.domAutomationController.send("
                                  "!window.open('data:text/html,dataurl'));",
                                  &success));
  EXPECT_TRUE(success);
  EXPECT_EQ(1u, Shell::windows().size());
}

// Verify that when a new child frame is added, the proxies created for it in
// other SiteInstances have correct sandbox flags and origin.
//
//     A         A           A
//    /         / \         / \    .
//   B    ->   B   A   ->  B   A
//                              \  .
//                               B
//
// The test checks sandbox flags and origin for the proxy added in step 2, by
// checking whether the grandchild frame added in step 3 sees proper sandbox
// flags and origin for its (remote) parent.  This wasn't addressed when
// https://crbug.com/423587 was fixed.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       ProxiesForNewChildFramesHaveCorrectReplicationState) {
  GURL main_url(
      embedded_test_server()->GetURL("/frame_tree/page_with_one_frame.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  TestNavigationObserver observer(shell()->web_contents());

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://127.0.0.1/\n"
      "      B = http://baz.com/",
      DepictFrameTree(root));

  // In the root frame, add a new sandboxed local frame, which itself has a
  // child frame on baz.com.  Wait for three RenderFrameHosts to be created:
  // the new sandboxed local frame, its child (while it's still local), and a
  // pending RFH when starting the cross-site navigation to baz.com.
  RenderFrameHostCreatedObserver frame_observer(shell()->web_contents(), 3);
  EXPECT_TRUE(
      ExecuteScript(root->current_frame_host(),
                    "window.domAutomationController.send("
                    "    addFrame('/frame_tree/page_with_one_frame.html',"
                    "             'allow-scripts allow-same-origin'))"));
  frame_observer.Wait();

  // Wait for the cross-site navigation to baz.com in the grandchild to finish.
  FrameTreeNode* bottom_child = root->child_at(1)->child_at(0);
  TestFrameNavigationObserver navigation_observer(bottom_child);
  navigation_observer.Wait();

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site A ------- proxies for B\n"
      "        +--Site B -- proxies for A\n"
      "Where A = http://127.0.0.1/\n"
      "      B = http://baz.com/",
      DepictFrameTree(root));

  // Use location.ancestorOrigins to check that the grandchild on baz.com sees
  // correct origin for its parent.
  int ancestor_origins_length = 0;
  EXPECT_TRUE(ExecuteScriptAndExtractInt(
      bottom_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins.length);",
      &ancestor_origins_length));
  EXPECT_EQ(2, ancestor_origins_length);
  std::string parent_origin;
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      bottom_child->current_frame_host(),
      "window.domAutomationController.send(location.ancestorOrigins[0]);",
      &parent_origin));
  EXPECT_EQ(main_url.GetOrigin().spec(), parent_origin + "/");

  // Check that the sandbox flags in the browser process are correct.
  // "allow-scripts" resets both WebSandboxFlags::Scripts and
  // WebSandboxFlags::AutomaticFeatures bits per blink::parseSandboxPolicy().
  blink::WebSandboxFlags expected_flags =
      blink::WebSandboxFlags::All & ~blink::WebSandboxFlags::Scripts &
      ~blink::WebSandboxFlags::AutomaticFeatures &
      ~blink::WebSandboxFlags::Origin;
  EXPECT_EQ(expected_flags, root->child_at(1)->effective_sandbox_flags());

  // The child of the sandboxed frame should've inherited sandbox flags, so it
  // should not be able to create popups.
  EXPECT_EQ(expected_flags, bottom_child->effective_sandbox_flags());
  bool success = false;
  EXPECT_TRUE(
      ExecuteScriptAndExtractBool(bottom_child->current_frame_host(),
                                  "window.domAutomationController.send("
                                  "!window.open('data:text/html,dataurl'));",
                                  &success));
  EXPECT_TRUE(success);
  EXPECT_EQ(1u, Shell::windows().size());
}

// Verify that a child frame can retrieve the name property set by its parent.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, WindowNameReplication) {
  GURL main_url(embedded_test_server()->GetURL("/frame_tree/2-4.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  TestNavigationObserver observer(shell()->web_contents());

  // Load cross-site page into iframe.
  GURL frame_url =
      embedded_test_server()->GetURL("foo.com", "/frame_tree/3-1.html");
  NavigateFrameToURL(root->child_at(0), frame_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(frame_url, observer.last_navigation_url());

  // Ensure that a new process is created for the subframe.
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(),
            root->child_at(0)->current_frame_host()->GetSiteInstance());

  // Check that the window.name seen by the frame matches the name attribute
  // specified by its parent in the iframe tag.
  std::string result;
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      root->child_at(0)->current_frame_host(),
      "window.domAutomationController.send(window.name);", &result));
  EXPECT_EQ("3-1-name", result);
}

// Verify that dynamic updates to a frame's window.name propagate to the
// frame's proxies, so that the latest frame names can be used in navigations.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, DynamicWindowName) {
  GURL main_url(embedded_test_server()->GetURL("/frame_tree/2-4.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  TestNavigationObserver observer(shell()->web_contents());

  // Load cross-site page into iframe.
  GURL frame_url =
      embedded_test_server()->GetURL("foo.com", "/frame_tree/3-1.html");
  NavigateFrameToURL(root->child_at(0), frame_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(frame_url, observer.last_navigation_url());

  // Browser process should know the child frame's original window.name
  // specified in the iframe element.
  EXPECT_EQ(root->child_at(0)->frame_name(), "3-1-name");

  // Update the child frame's window.name.
  EXPECT_TRUE(ExecuteScript(root->child_at(0)->current_frame_host(),
                            "window.domAutomationController.send("
                            "window.name = 'updated-name');"));

  // The change should propagate to the browser process.
  EXPECT_EQ(root->child_at(0)->frame_name(), "updated-name");

  // The proxy in the parent process should also receive the updated name.
  // Check that it can reference the child frame by its new name.
  bool success = false;
  EXPECT_TRUE(
      ExecuteScriptAndExtractBool(shell()->web_contents(),
                                  "window.domAutomationController.send("
                                  "frames['updated-name'] == frames[0]);",
                                  &success));
  EXPECT_TRUE(success);

  // Issue a renderer-initiated navigation from the root frame to the child
  // frame using the frame's name. Make sure correct frame is navigated.
  //
  // TODO(alexmos): When blink::createWindow is refactored to handle
  // RemoteFrames, this should also be tested via window.open(url, frame_name)
  // and a more complicated frame hierarchy (https://crbug.com/463742)
  TestFrameNavigationObserver frame_observer(root->child_at(0));
  GURL foo_url(embedded_test_server()->GetURL("foo.com", "/title1.html"));
  std::string script = base::StringPrintf(
      "window.domAutomationController.send("
      "frames['updated-name'].location.href = '%s');",
      foo_url.spec().c_str());
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(), script));
  frame_observer.Wait();
  EXPECT_EQ(foo_url, root->child_at(0)->current_url());
}

// Verify that when a frame is navigated to a new origin, the origin update
// propagates to the frame's proxies.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, OriginUpdatesReachProxies) {
  GURL main_url(
      embedded_test_server()->GetURL("/frame_tree/page_with_two_frames.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  TestNavigationObserver observer(shell()->web_contents());

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site A ------- proxies for B\n"
      "Where A = http://127.0.0.1/\n"
      "      B = http://bar.com/",
      DepictFrameTree(root));

  // Navigate second subframe to a baz.com.  This should send an origin update
  // to the frame's proxy in the bar.com (first frame's) process.
  GURL frame_url = embedded_test_server()->GetURL("baz.com", "/title2.html");
  NavigateFrameToURL(root->child_at(1), frame_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(frame_url, observer.last_navigation_url());

  // The first frame can't directly observe the second frame's origin with
  // JavaScript.  Instead, try to navigate the second frame from the first
  // frame.  This should fail with a console error message, which should
  // contain the second frame's updated origin (see blink::Frame::canNavigate).
  scoped_ptr<ConsoleObserverDelegate> console_delegate(
      new ConsoleObserverDelegate(
          shell()->web_contents(),
          "Unsafe JavaScript attempt to initiate navigation*"));
  shell()->web_contents()->SetDelegate(console_delegate.get());

  // frames[1] can't be used due to a bug where RemoteFrames are created out of
  // order (https://crbug.com/478792).  Instead, target second frame by name.
  EXPECT_TRUE(ExecuteScript(
      root->child_at(0)->current_frame_host(),
      "window.domAutomationController.send("
      "    parent.frames['frame2'].location.href = 'data:text/html,foo');"));
  console_delegate->Wait();

  std::string frame_origin = root->child_at(1)->current_origin().Serialize();
  EXPECT_EQ(frame_origin + "/", frame_url.GetOrigin().spec());
  EXPECT_TRUE(
      base::MatchPattern(console_delegate->message(), "*" + frame_origin + "*"))
      << "Error message does not contain the frame's latest origin ("
      << frame_origin << ")";
}

// Ensure that navigating subframes in --site-per-process mode properly fires
// the DidStopLoading event on WebContentsObserver.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, CrossSiteDidStopLoading) {
  GURL main_url(embedded_test_server()->GetURL("/site_per_process_main.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(shell()->web_contents())->
          GetFrameTree()->root();

  TestNavigationObserver observer(shell()->web_contents());

  // Load same-site page into iframe.
  FrameTreeNode* child = root->child_at(0);
  GURL http_url(embedded_test_server()->GetURL("/title1.html"));
  NavigateFrameToURL(child, http_url);
  EXPECT_EQ(http_url, observer.last_navigation_url());
  EXPECT_TRUE(observer.last_navigation_succeeded());

  // Load cross-site page into iframe.
  TestNavigationObserver nav_observer(shell()->web_contents(), 1);
  GURL url = embedded_test_server()->GetURL("foo.com", "/title2.html");
  NavigationController::LoadURLParams params(url);
  params.transition_type = ui::PAGE_TRANSITION_LINK;
  params.frame_tree_node_id = child->frame_tree_node_id();
  child->navigator()->GetController()->LoadURLWithParams(params);
  nav_observer.Wait();

  // Verify that the navigation succeeded and the expected URL was loaded.
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url, observer.last_navigation_url());
}

// Ensure that the renderer does not crash when navigating a frame that has a
// sibling RemoteFrame.  See https://crbug.com/426953.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       NavigateWithSiblingRemoteFrame) {
  GURL main_url(
      embedded_test_server()->GetURL("/frame_tree/page_with_two_frames.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  TestNavigationObserver observer(shell()->web_contents());

  // Make sure the first frame is out of process.
  ASSERT_EQ(2U, root->child_count());
  FrameTreeNode* node2 = root->child_at(0);
  EXPECT_NE(root->current_frame_host()->GetSiteInstance(),
            node2->current_frame_host()->GetSiteInstance());

  // Make sure the second frame is in the parent's process.
  FrameTreeNode* node3 = root->child_at(1);
  EXPECT_EQ(root->current_frame_host()->GetSiteInstance(),
            node3->current_frame_host()->GetSiteInstance());

  // Navigate the second iframe (node3) to a URL in its own process.
  GURL title_url = embedded_test_server()->GetURL("/title2.html");
  NavigateFrameToURL(node3, title_url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(title_url, observer.last_navigation_url());
  EXPECT_EQ(root->current_frame_host()->GetSiteInstance(),
            node3->current_frame_host()->GetSiteInstance());
  EXPECT_TRUE(node3->current_frame_host()->IsRenderFrameLive());
}

// Ensure that the renderer does not crash when a local frame with a remote
// parent frame is swapped from local to remote, then back to local again.
// See https://crbug.com/585654.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       NavigateSiblingsToSameProcess) {
  GURL main_url(
      embedded_test_server()->GetURL("/frame_tree/page_with_two_frames.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  FrameTreeNode* node2 = root->child_at(0);
  FrameTreeNode* node3 = root->child_at(1);

  // Navigate the second iframe to the same process as the first.
  GURL frame_url = embedded_test_server()->GetURL("bar.com", "/title1.html");
  NavigateFrameToURL(node3, frame_url);

  // Verify that they are in the same process.
  EXPECT_EQ(node2->current_frame_host()->GetSiteInstance(),
            node3->current_frame_host()->GetSiteInstance());
  EXPECT_NE(root->current_frame_host()->GetSiteInstance(),
            node3->current_frame_host()->GetSiteInstance());

  // Navigate the first iframe into its parent's process.
  GURL title_url = embedded_test_server()->GetURL("/title2.html");
  NavigateFrameToURL(node2, title_url);
  EXPECT_NE(node2->current_frame_host()->GetSiteInstance(),
            node3->current_frame_host()->GetSiteInstance());

  // Return the first iframe to the same process as its sibling, and ensure
  // that it does not crash.
  NavigateFrameToURL(node2, frame_url);
  EXPECT_EQ(node2->current_frame_host()->GetSiteInstance(),
            node3->current_frame_host()->GetSiteInstance());
  EXPECT_TRUE(node2->current_frame_host()->IsRenderFrameLive());
}

// Verify that load events for iframe elements work when the child frame is
// out-of-process.  In such cases, the load event is forwarded from the child
// frame to the parent frame via the browser process.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, LoadEventForwarding) {
  // Load a page with a cross-site frame.  The parent page has an onload
  // handler in the iframe element that appends "LOADED" to the document title.
  {
    GURL main_url(
        embedded_test_server()->GetURL("/frame_with_load_event.html"));
    base::string16 expected_title(base::UTF8ToUTF16("LOADED"));
    TitleWatcher title_watcher(shell()->web_contents(), expected_title);
    EXPECT_TRUE(NavigateToURL(shell(), main_url));
    EXPECT_EQ(title_watcher.WaitAndGetTitle(), expected_title);
  }

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  // Load another cross-site page into the iframe and check that the load event
  // is fired.
  {
    GURL foo_url(embedded_test_server()->GetURL("foo.com", "/title1.html"));
    base::string16 expected_title(base::UTF8ToUTF16("LOADEDLOADED"));
    TitleWatcher title_watcher(shell()->web_contents(), expected_title);
    TestNavigationObserver observer(shell()->web_contents());
    NavigateFrameToURL(root->child_at(0), foo_url);
    EXPECT_TRUE(observer.last_navigation_succeeded());
    EXPECT_EQ(foo_url, observer.last_navigation_url());
    EXPECT_EQ(title_watcher.WaitAndGetTitle(), expected_title);
  }
}

// Check that postMessage can be routed between cross-site iframes.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, SubframePostMessage) {
  GURL main_url(embedded_test_server()->GetURL(
      "/frame_tree/page_with_post_message_frames.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  ASSERT_EQ(2U, root->child_count());

  // Verify the frames start at correct URLs.  First frame should be
  // same-site; second frame should be cross-site.
  GURL same_site_url(embedded_test_server()->GetURL("/post_message.html"));
  EXPECT_EQ(same_site_url, root->child_at(0)->current_url());
  GURL foo_url(embedded_test_server()->GetURL("foo.com",
                                              "/post_message.html"));
  EXPECT_EQ(foo_url, root->child_at(1)->current_url());
  EXPECT_NE(root->child_at(0)->current_frame_host()->GetSiteInstance(),
            root->child_at(1)->current_frame_host()->GetSiteInstance());

  // Send a message from first, same-site frame to second, cross-site frame.
  // Expect the second frame to reply back to the first frame.
  PostMessageAndWaitForReply(root->child_at(0),
                             "postToSibling('subframe-msg','subframe2')",
                             "\"done-subframe1\"");

  // Send a postMessage from second, cross-site frame to its parent.  Expect
  // parent to send a reply to the frame.
  base::string16 expected_title(base::ASCIIToUTF16("subframe-msg"));
  TitleWatcher title_watcher(shell()->web_contents(), expected_title);
  PostMessageAndWaitForReply(root->child_at(1), "postToParent('subframe-msg')",
                             "\"done-subframe2\"");
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());

  // Verify the total number of received messages for each subframe.  First
  // frame should have one message (reply from second frame).  Second frame
  // should have two messages (message from first frame and reply from parent).
  // Parent should have one message (from second frame).
  EXPECT_EQ(1, GetReceivedMessages(root->child_at(0)));
  EXPECT_EQ(2, GetReceivedMessages(root->child_at(1)));
  EXPECT_EQ(1, GetReceivedMessages(root));
}

// Check that postMessage can be sent from a subframe on a cross-process opener
// tab, and that its event.source points to a valid proxy.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       PostMessageWithSubframeOnOpenerChain) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/frame_tree/page_with_post_message_frames.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  ASSERT_EQ(2U, root->child_count());

  // Verify the initial state of the world.  First frame should be same-site;
  // second frame should be cross-site.
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site A ------- proxies for B\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://a.com/\n"
      "      B = http://foo.com/",
      DepictFrameTree(root));

  // Open a popup from the first subframe (so that popup's window.opener points
  // to the subframe) and navigate it to bar.com.
  ShellAddedObserver new_shell_observer;
  EXPECT_TRUE(ExecuteScript(root->child_at(0)->current_frame_host(),
                            "openPopup('about:blank');"));
  Shell* popup = new_shell_observer.GetShell();
  GURL popup_url(
      embedded_test_server()->GetURL("bar.com", "/post_message.html"));
  EXPECT_TRUE(NavigateToURL(popup, popup_url));

  // From the popup, open another popup for baz.com.  This will be used to
  // check that the whole opener chain is processed when creating proxies and
  // not just an immediate opener.
  ShellAddedObserver new_shell_observer2;
  EXPECT_TRUE(
      ExecuteScript(popup->web_contents(), "openPopup('about:blank');"));
  Shell* popup2 = new_shell_observer2.GetShell();
  GURL popup2_url(
      embedded_test_server()->GetURL("baz.com", "/post_message.html"));
  EXPECT_TRUE(NavigateToURL(popup2, popup2_url));

  // Ensure that we've created proxies for SiteInstances of both popups (C, D)
  // in the main window's frame tree.
  EXPECT_EQ(
      " Site A ------------ proxies for B C D\n"
      "   |--Site A ------- proxies for B C D\n"
      "   +--Site B ------- proxies for A C D\n"
      "Where A = http://a.com/\n"
      "      B = http://foo.com/\n"
      "      C = http://bar.com/\n"
      "      D = http://baz.com/",
      DepictFrameTree(root));

  // Check the first popup's frame tree as well.  Note that it doesn't have a
  // proxy for foo.com, since foo.com can't reach the popup.  It does have a
  // proxy for its opener a.com (which can reach it via the window.open
  // reference) and second popup (which can reach it via window.opener).
  FrameTreeNode* popup_root =
      static_cast<WebContentsImpl*>(popup->web_contents())
          ->GetFrameTree()
          ->root();
  EXPECT_EQ(
      " Site C ------------ proxies for A D\n"
      "Where A = http://a.com/\n"
      "      C = http://bar.com/\n"
      "      D = http://baz.com/",
      DepictFrameTree(popup_root));

  // Send a message from first subframe on main page to the first popup and
  // wait for a reply back. The reply verifies that the proxy for the opener
  // tab's subframe is targeted properly.
  PostMessageAndWaitForReply(root->child_at(0), "postToPopup('subframe-msg')",
                             "\"done-subframe1\"");

  // Send a postMessage from the popup to window.opener and ensure that it
  // reaches subframe1.  This verifies that the subframe opener information
  // propagated to the popup's RenderFrame.  Wait for subframe1 to send a reply
  // message to the popup.
  EXPECT_TRUE(ExecuteScript(popup->web_contents(), "window.name = 'popup';"));
  PostMessageAndWaitForReply(popup_root, "postToOpener('subframe-msg', '*')",
                             "\"done-popup\"");

  // Second a postMessage from popup2 to window.opener.opener, which should
  // resolve to subframe1.  This tests opener chains of length greater than 1.
  // As before, subframe1 will send a reply to popup2.
  FrameTreeNode* popup2_root =
      static_cast<WebContentsImpl*>(popup2->web_contents())
          ->GetFrameTree()
          ->root();
  EXPECT_TRUE(ExecuteScript(popup2->web_contents(), "window.name = 'popup2';"));
  PostMessageAndWaitForReply(popup2_root,
                             "postToOpenerOfOpener('subframe-msg', '*')",
                             "\"done-popup2\"");

  // Verify the total number of received messages for each subframe:
  //  - 3 for first subframe (two from first popup, one from second popup)
  //  - 2 for popup (both from first subframe)
  //  - 1 for popup2 (reply from first subframe)
  //  - 0 for other frames
  EXPECT_EQ(0, GetReceivedMessages(root));
  EXPECT_EQ(3, GetReceivedMessages(root->child_at(0)));
  EXPECT_EQ(0, GetReceivedMessages(root->child_at(1)));
  EXPECT_EQ(2, GetReceivedMessages(popup_root));
  EXPECT_EQ(1, GetReceivedMessages(popup2_root));
}

// Check that parent.frames[num] references correct sibling frames when the
// parent is remote.  See https://crbug.com/478792.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, IndexedFrameAccess) {
  // Start on a page with three same-site subframes.
  GURL main_url(
      embedded_test_server()->GetURL("a.com", "/frame_tree/top.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  ASSERT_EQ(3U, root->child_count());
  FrameTreeNode* child0 = root->child_at(0);
  FrameTreeNode* child1 = root->child_at(1);
  FrameTreeNode* child2 = root->child_at(2);

  // Send each of the frames to a different site.  Each new renderer will first
  // create proxies for the parent and two sibling subframes and then create
  // and insert the new RenderFrame into the frame tree.
  GURL b_url(embedded_test_server()->GetURL("b.com", "/post_message.html"));
  GURL c_url(embedded_test_server()->GetURL("c.com", "/post_message.html"));
  GURL d_url(embedded_test_server()->GetURL("d.com", "/post_message.html"));
  NavigateFrameToURL(child0, b_url);
  // TODO(alexmos): The calls to WaitForRenderFrameReady can be removed once
  // TestFrameNavigationObserver is fixed to use DidFinishLoad.
  EXPECT_TRUE(WaitForRenderFrameReady(child0->current_frame_host()));
  NavigateFrameToURL(child1, c_url);
  EXPECT_TRUE(WaitForRenderFrameReady(child1->current_frame_host()));
  NavigateFrameToURL(child2, d_url);
  EXPECT_TRUE(WaitForRenderFrameReady(child2->current_frame_host()));

  EXPECT_EQ(
      " Site A ------------ proxies for B C D\n"
      "   |--Site B ------- proxies for A C D\n"
      "   |--Site C ------- proxies for A B D\n"
      "   +--Site D ------- proxies for A B C\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/\n"
      "      C = http://c.com/\n"
      "      D = http://d.com/",
      DepictFrameTree(root));

  // Check that each subframe sees itself at correct index in parent.frames.
  bool success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      child0->current_frame_host(),
      "window.domAutomationController.send(window === parent.frames[0]);",
      &success));
  EXPECT_TRUE(success);

  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      child1->current_frame_host(),
      "window.domAutomationController.send(window === parent.frames[1]);",
      &success));
  EXPECT_TRUE(success);

  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      child2->current_frame_host(),
      "window.domAutomationController.send(window === parent.frames[2]);",
      &success));
  EXPECT_TRUE(success);

  // Send a postMessage from B to parent.frames[1], which should go to C, and
  // wait for reply.
  PostMessageAndWaitForReply(child0, "postToSibling('subframe-msg', 1)",
                             "\"done-1-1-name\"");

  // Send a postMessage from C to parent.frames[2], which should go to D, and
  // wait for reply.
  PostMessageAndWaitForReply(child1, "postToSibling('subframe-msg', 2)",
                             "\"done-1-2-name\"");

  // Verify the total number of received messages for each subframe.
  EXPECT_EQ(1, GetReceivedMessages(child0));
  EXPECT_EQ(2, GetReceivedMessages(child1));
  EXPECT_EQ(1, GetReceivedMessages(child2));
}

IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, RFPHDestruction) {
  GURL main_url(embedded_test_server()->GetURL("/site_per_process_main.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  TestNavigationObserver observer(shell()->web_contents());

  // Load cross-site page into iframe.
  FrameTreeNode* child = root->child_at(0);
  GURL url = embedded_test_server()->GetURL("foo.com", "/title2.html");
  NavigateFrameToURL(root->child_at(0), url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url, observer.last_navigation_url());
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site A ------- proxies for B\n"
      "        |--Site A -- proxies for B\n"
      "        +--Site A -- proxies for B\n"
      "             +--Site A -- proxies for B\n"
      "Where A = http://127.0.0.1/\n"
      "      B = http://foo.com/",
      DepictFrameTree(root));

  // Load another cross-site page.
  url = embedded_test_server()->GetURL("bar.com", "/title3.html");
  NavigateIframeToURL(shell()->web_contents(), "test", url);
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(url, observer.last_navigation_url());
  EXPECT_EQ(
      " Site A ------------ proxies for C\n"
      "   |--Site C ------- proxies for A\n"
      "   +--Site A ------- proxies for C\n"
      "        |--Site A -- proxies for C\n"
      "        +--Site A -- proxies for C\n"
      "             +--Site A -- proxies for C\n"
      "Where A = http://127.0.0.1/\n"
      "      C = http://bar.com/",
      DepictFrameTree(root));

  // Navigate back to the parent's origin.
  url = embedded_test_server()->GetURL("/title1.html");
  NavigateFrameToURL(child, url);
  EXPECT_EQ(url, observer.last_navigation_url());
  EXPECT_TRUE(observer.last_navigation_succeeded());
  EXPECT_EQ(
      " Site A\n"
      "   |--Site A\n"
      "   +--Site A\n"
      "        |--Site A\n"
      "        +--Site A\n"
      "             +--Site A\n"
      "Where A = http://127.0.0.1/",
      DepictFrameTree(root));
}

IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, OpenPopupWithRemoteParent) {
  GURL main_url(
      embedded_test_server()->GetURL("a.com", "/site_per_process_main.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  // Navigate first child cross-site.
  GURL frame_url(embedded_test_server()->GetURL("b.com", "/title1.html"));
  NavigateFrameToURL(root->child_at(0), frame_url);

  // Open a popup from the first child.
  Shell* new_shell = OpenPopup(root->child_at(0)->current_frame_host(),
                               GURL(url::kAboutBlankURL), "");
  EXPECT_TRUE(new_shell);

  // Check that the popup's opener is correct on both the browser and renderer
  // sides.
  FrameTreeNode* popup_root =
      static_cast<WebContentsImpl*>(new_shell->web_contents())
          ->GetFrameTree()
          ->root();
  EXPECT_EQ(root->child_at(0), popup_root->opener());

  std::string opener_url;
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      popup_root->current_frame_host(),
      "window.domAutomationController.send(window.opener.location.href);",
      &opener_url));
  EXPECT_EQ(frame_url.spec(), opener_url);

  // Now try the same with a cross-site popup and make sure it ends up in a new
  // process and with a correct opener.
  GURL popup_url(embedded_test_server()->GetURL("c.com", "/title2.html"));
  Shell* cross_site_popup =
      OpenPopup(root->child_at(0)->current_frame_host(), popup_url, "");
  EXPECT_TRUE(cross_site_popup);

  FrameTreeNode* cross_site_popup_root =
      static_cast<WebContentsImpl*>(cross_site_popup->web_contents())
          ->GetFrameTree()
          ->root();
  EXPECT_EQ(cross_site_popup_root->current_url(), popup_url);

  EXPECT_NE(shell()->web_contents()->GetSiteInstance(),
            cross_site_popup->web_contents()->GetSiteInstance());
  EXPECT_NE(root->child_at(0)->current_frame_host()->GetSiteInstance(),
            cross_site_popup->web_contents()->GetSiteInstance());

  EXPECT_EQ(root->child_at(0), cross_site_popup_root->opener());

  // Ensure the popup's window.opener points to the right subframe.  Note that
  // we can't check the opener's location as above since it's cross-origin.
  bool success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      cross_site_popup_root->current_frame_host(),
      "window.domAutomationController.send("
      "    window.opener === window.opener.top.frames[0]);",
      &success));
  EXPECT_TRUE(success);
}

// Test that cross-process popups can't be navigated to disallowed URLs by
// their opener.  This ensures that proper URL validation is performed when
// RenderFrameProxyHosts are navigated.  See https://crbug.com/595339.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, NavigatePopupToIllegalURL) {
  GURL main_url(embedded_test_server()->GetURL("a.com", "/title1.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // Open a cross-site popup.
  GURL popup_url(embedded_test_server()->GetURL("b.com", "/title2.html"));
  Shell* popup = OpenPopup(shell()->web_contents(), popup_url, "foo");
  EXPECT_TRUE(popup);
  EXPECT_NE(popup->web_contents()->GetSiteInstance(),
            shell()->web_contents()->GetSiteInstance());

  // From the opener, navigate the popup to a file:/// URL.  This should be
  // disallowed and result in an about:blank navigation.
  GURL file_url("file:///");
  NavigateNamedFrame(shell()->web_contents(), file_url, "foo");
  EXPECT_TRUE(WaitForLoadStop(popup->web_contents()));
  EXPECT_EQ(GURL(url::kAboutBlankURL),
            popup->web_contents()->GetLastCommittedURL());

  // Navigate popup back to a cross-site URL.
  EXPECT_TRUE(NavigateToURL(popup, popup_url));
  EXPECT_NE(popup->web_contents()->GetSiteInstance(),
            shell()->web_contents()->GetSiteInstance());

  // Now try the same test with a chrome:// URL.
  GURL chrome_url(std::string(kChromeUIScheme) + "://" +
                  std::string(kChromeUIGpuHost));
  NavigateNamedFrame(shell()->web_contents(), chrome_url, "foo");
  EXPECT_TRUE(WaitForLoadStop(popup->web_contents()));
  EXPECT_EQ(GURL(url::kAboutBlankURL),
            popup->web_contents()->GetLastCommittedURL());
}

// Verify that named frames are discoverable from their opener's ancestors.
// See https://crbug.com/511474.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       DiscoverNamedFrameFromAncestorOfOpener) {
  GURL main_url(
      embedded_test_server()->GetURL("a.com", "/site_per_process_main.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  // Navigate first child cross-site.
  GURL frame_url(embedded_test_server()->GetURL("b.com", "/title1.html"));
  NavigateFrameToURL(root->child_at(0), frame_url);

  // Open a popup named "foo" from the first child.
  Shell* foo_shell = OpenPopup(root->child_at(0)->current_frame_host(),
                               GURL(url::kAboutBlankURL), "foo");
  EXPECT_TRUE(foo_shell);

  // Check that a proxy was created for the "foo" popup in a.com.
  FrameTreeNode* foo_root =
      static_cast<WebContentsImpl*>(foo_shell->web_contents())
          ->GetFrameTree()
          ->root();
  SiteInstance* site_instance_a = root->current_frame_host()->GetSiteInstance();
  RenderFrameProxyHost* popup_rfph_for_a =
      foo_root->render_manager()->GetRenderFrameProxyHost(site_instance_a);
  EXPECT_TRUE(popup_rfph_for_a);

  // Verify that the main frame can find the "foo" popup by name.  If
  // window.open targets the correct frame, the "foo" popup's current URL
  // should be updated to |named_frame_url|.
  GURL named_frame_url(embedded_test_server()->GetURL("c.com", "/title2.html"));
  NavigateNamedFrame(shell()->web_contents(), named_frame_url, "foo");
  EXPECT_TRUE(WaitForLoadStop(foo_shell->web_contents()));
  EXPECT_EQ(named_frame_url, foo_root->current_url());

  // Navigate the popup cross-site and ensure it's still reachable via
  // window.open from the main frame.
  GURL d_url(embedded_test_server()->GetURL("d.com", "/title3.html"));
  NavigateToURL(foo_shell, d_url);
  EXPECT_EQ(d_url, foo_root->current_url());
  NavigateNamedFrame(shell()->web_contents(), named_frame_url, "foo");
  EXPECT_TRUE(WaitForLoadStop(foo_shell->web_contents()));
  EXPECT_EQ(named_frame_url, foo_root->current_url());
}

// Similar to DiscoverNamedFrameFromAncestorOfOpener, but check that if a
// window is created without a name and acquires window.name later, it will
// still be discoverable from its opener's ancestors.  Also, instead of using
// an opener's ancestor, this test uses a popup with same origin as that
// ancestor. See https://crbug.com/511474.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       DiscoverFrameAfterSettingWindowName) {
  GURL main_url(
      embedded_test_server()->GetURL("a.com", "/site_per_process_main.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  // Open a same-site popup from the main frame.
  GURL a_com_url(embedded_test_server()->GetURL("a.com", "/title3.html"));
  Shell* a_com_shell =
      OpenPopup(root->child_at(0)->current_frame_host(), a_com_url, "");
  EXPECT_TRUE(a_com_shell);

  // Navigate first child on main frame cross-site.
  GURL frame_url(embedded_test_server()->GetURL("b.com", "/title1.html"));
  NavigateFrameToURL(root->child_at(0), frame_url);

  // Open an unnamed popup from the first child frame.
  Shell* foo_shell = OpenPopup(root->child_at(0)->current_frame_host(),
                               GURL(url::kAboutBlankURL), "");
  EXPECT_TRUE(foo_shell);

  // There should be no proxy created for the "foo" popup in a.com, since
  // there's no way for the two a.com frames to access it yet.
  FrameTreeNode* foo_root =
      static_cast<WebContentsImpl*>(foo_shell->web_contents())
          ->GetFrameTree()
          ->root();
  SiteInstance* site_instance_a = root->current_frame_host()->GetSiteInstance();
  EXPECT_FALSE(
      foo_root->render_manager()->GetRenderFrameProxyHost(site_instance_a));

  // Set window.name in the popup's frame.
  EXPECT_TRUE(ExecuteScript(foo_shell->web_contents(), "window.name = 'foo'"));

  // A proxy for the popup should now exist in a.com.
  EXPECT_TRUE(
      foo_root->render_manager()->GetRenderFrameProxyHost(site_instance_a));

  // Verify that the a.com popup can now find the "foo" popup by name.
  GURL named_frame_url(embedded_test_server()->GetURL("c.com", "/title2.html"));
  NavigateNamedFrame(a_com_shell->web_contents(), named_frame_url, "foo");
  EXPECT_TRUE(WaitForLoadStop(foo_shell->web_contents()));
  EXPECT_EQ(named_frame_url, foo_root->current_url());
}

// Check that frame opener updates work with subframes.  Set up a window with a
// popup and update openers for the popup's main frame and subframe to
// subframes on first window, as follows:
//
//    foo      +---- bar
//    / \      |     / \      .
// bar   foo <-+  bar   foo
//  ^                    |
//  +--------------------+
//
// The sites are carefully set up so that both opener updates are cross-process
// but still allowed by Blink's navigation checks.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, UpdateSubframeOpener) {
  GURL main_url = embedded_test_server()->GetURL(
      "foo.com", "/frame_tree/page_with_two_frames.html");
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  EXPECT_EQ(2U, root->child_count());

  // From the top frame, open a popup and navigate it to a cross-site page with
  // two subframes.
  Shell* popup_shell =
      OpenPopup(shell()->web_contents(), GURL(url::kAboutBlankURL), "popup");
  EXPECT_TRUE(popup_shell);
  GURL popup_url(embedded_test_server()->GetURL(
      "bar.com", "/frame_tree/page_with_post_message_frames.html"));
  NavigateToURL(popup_shell, popup_url);

  FrameTreeNode* popup_root =
      static_cast<WebContentsImpl*>(popup_shell->web_contents())
          ->GetFrameTree()
          ->root();
  EXPECT_EQ(2U, popup_root->child_count());

  // Popup's opener should point to main frame to start with.
  EXPECT_EQ(root, popup_root->opener());

  // Update the popup's opener to the second subframe on the main page (which
  // is same-origin with the top frame, i.e., foo.com).
  bool success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      root->child_at(1)->current_frame_host(),
      "window.domAutomationController.send(!!window.open('','popup'));",
      &success));
  EXPECT_TRUE(success);

  // Check that updated opener propagated to the browser process and the
  // popup's bar.com process.
  EXPECT_EQ(root->child_at(1), popup_root->opener());

  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      popup_shell->web_contents(),
      "window.domAutomationController.send("
      "    window.opener === window.opener.parent.frames['frame2']);",
      &success));
  EXPECT_TRUE(success);

  // Now update opener on the popup's second subframe (foo.com) to the main
  // page's first subframe (bar.com).
  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      root->child_at(0)->current_frame_host(),
      "window.domAutomationController.send(!!window.open('','subframe2'));",
      &success));
  EXPECT_TRUE(success);

  // Check that updated opener propagated to the browser process and the
  // foo.com process.
  EXPECT_EQ(root->child_at(0), popup_root->child_at(1)->opener());

  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      popup_root->child_at(1)->current_frame_host(),
      "window.domAutomationController.send("
      "    window.opener === window.opener.parent.frames['frame1']);",
      &success));
  EXPECT_TRUE(success);
}

// Check that when a subframe navigates to a new SiteInstance, the new
// SiteInstance will get a proxy for the opener of subframe's parent.  I.e.,
// accessing parent.opener from the subframe should still work after a
// cross-process navigation.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       NavigatingSubframePreservesOpenerInParent) {
  GURL main_url = embedded_test_server()->GetURL("a.com", "/post_message.html");
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  // Open a popup with a cross-site page that has a subframe.
  GURL popup_url(embedded_test_server()->GetURL(
      "b.com", "/cross_site_iframe_factory.html?b(b)"));
  Shell* popup_shell = OpenPopup(shell()->web_contents(), popup_url, "popup");
  EXPECT_TRUE(popup_shell);
  FrameTreeNode* popup_root =
      static_cast<WebContentsImpl*>(popup_shell->web_contents())
          ->GetFrameTree()
          ->root();
  EXPECT_EQ(1U, popup_root->child_count());

  // Check that the popup's opener is correct in the browser process.
  EXPECT_EQ(root, popup_root->opener());

  // Navigate popup's subframe to another site.
  GURL frame_url(embedded_test_server()->GetURL("c.com", "/post_message.html"));
  NavigateFrameToURL(popup_root->child_at(0), frame_url);
  EXPECT_TRUE(
      WaitForRenderFrameReady(popup_root->child_at(0)->current_frame_host()));

  // Check that the new subframe process still sees correct opener for its
  // parent by sending a postMessage to subframe's parent.opener.
  bool success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      popup_root->child_at(0)->current_frame_host(),
      "window.domAutomationController.send(!!parent.opener);", &success));
  EXPECT_TRUE(success);

  base::string16 expected_title = base::ASCIIToUTF16("msg");
  TitleWatcher title_watcher(shell()->web_contents(), expected_title);
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      popup_root->child_at(0)->current_frame_host(),
      "window.domAutomationController.send(postToOpenerOfParent('msg','*'));",
      &success));
  EXPECT_TRUE(success);
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
}

// Check that if a subframe has an opener, that opener is preserved when the
// subframe navigates cross-site.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, NavigateSubframeWithOpener) {
  GURL main_url(embedded_test_server()->GetURL(
      "foo.com", "/frame_tree/page_with_two_frames.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site B ------- proxies for A\n"
      "   +--Site A ------- proxies for B\n"
      "Where A = http://foo.com/\n"
      "      B = http://bar.com/",
      DepictFrameTree(root));

  // Update the first (cross-site) subframe's opener to root frame.
  bool success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      root->current_frame_host(),
      "window.domAutomationController.send(!!window.open('','frame1'));",
      &success));
  EXPECT_TRUE(success);

  // Check that updated opener propagated to the browser process and subframe's
  // process.
  EXPECT_EQ(root, root->child_at(0)->opener());

  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      root->child_at(0)->current_frame_host(),
      "window.domAutomationController.send(window.opener === window.parent);",
      &success));
  EXPECT_TRUE(success);

  // Navigate the subframe with opener to another site.
  GURL frame_url(embedded_test_server()->GetURL("baz.com", "/title1.html"));
  NavigateFrameToURL(root->child_at(0), frame_url);

  // Check that the subframe still sees correct opener in its new process.
  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      root->child_at(0)->current_frame_host(),
      "window.domAutomationController.send(window.opener === window.parent);",
      &success));
  EXPECT_TRUE(success);

  // Navigate second subframe to a new site.  Check that the proxy that's
  // created for the first subframe in the new SiteInstance has correct opener.
  GURL frame2_url(embedded_test_server()->GetURL("qux.com", "/title1.html"));
  NavigateFrameToURL(root->child_at(1), frame2_url);

  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      root->child_at(1)->current_frame_host(),
      "window.domAutomationController.send("
      "    parent.frames['frame1'].opener === parent);",
      &success));
  EXPECT_TRUE(success);
}

// Check that if a subframe has an opener, that opener is preserved when a new
// RenderFrameProxy is created for that subframe in another renderer process.
// Similar to NavigateSubframeWithOpener, but this test verifies the subframe
// opener plumbing for FrameMsg_NewFrameProxy, whereas
// NavigateSubframeWithOpener targets FrameMsg_NewFrame.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       NewRenderFrameProxyPreservesOpener) {
  GURL main_url(
      embedded_test_server()->GetURL("foo.com", "/post_message.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  // Open a popup with a cross-site page that has two subframes.
  GURL popup_url(embedded_test_server()->GetURL(
      "bar.com", "/frame_tree/page_with_post_message_frames.html"));
  Shell* popup_shell = OpenPopup(shell()->web_contents(), popup_url, "popup");
  EXPECT_TRUE(popup_shell);
  FrameTreeNode* popup_root =
      static_cast<WebContentsImpl*>(popup_shell->web_contents())
          ->GetFrameTree()
          ->root();
  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   |--Site A ------- proxies for B\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://bar.com/\n"
      "      B = http://foo.com/",
      DepictFrameTree(popup_root));

  // Update the popup's second subframe's opener to root frame.  This is
  // allowed because that subframe is in the same foo.com SiteInstance as the
  // root frame.
  bool success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      root->current_frame_host(),
      "window.domAutomationController.send(!!window.open('','subframe2'));",
      &success));
  EXPECT_TRUE(success);

  // Check that the opener update propagated to the browser process and bar.com
  // process.
  EXPECT_EQ(root, popup_root->child_at(1)->opener());
  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      popup_root->child_at(0)->current_frame_host(),
      "window.domAutomationController.send("
      "    parent.frames['subframe2'].opener && "
      "        parent.frames['subframe2'].opener === parent.opener);",
      &success));
  EXPECT_TRUE(success);

  // Navigate the popup's first subframe to another site.
  GURL frame_url(
      embedded_test_server()->GetURL("baz.com", "/post_message.html"));
  NavigateFrameToURL(popup_root->child_at(0), frame_url);
  EXPECT_TRUE(
      WaitForRenderFrameReady(popup_root->child_at(0)->current_frame_host()));

  // Check that the second subframe's opener is still correct in the first
  // subframe's new process.  Verify it both in JS and with a postMessage.
  success = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      popup_root->child_at(0)->current_frame_host(),
      "window.domAutomationController.send("
      "    parent.frames['subframe2'].opener && "
      "        parent.frames['subframe2'].opener === parent.opener);",
      &success));
  EXPECT_TRUE(success);

  base::string16 expected_title = base::ASCIIToUTF16("msg");
  TitleWatcher title_watcher(shell()->web_contents(), expected_title);
  EXPECT_TRUE(ExecuteScriptAndExtractBool(
      popup_root->child_at(0)->current_frame_host(),
      "window.domAutomationController.send("
      "    postToOpenerOfSibling('subframe2', 'msg', '*'));",
      &success));
  EXPECT_TRUE(success);
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
}

// Test for https://crbug.com/515302.  Perform two navigations, A->B->A, and
// delay the SwapOut ACK from the A->B navigation, so that the second B->A
// navigation is initiated before the first page receives the SwapOut ACK.
// Ensure that the RVH(A) that's pending deletion is not reused in that case.
// crbug.com/554825
#if defined(THREAD_SANITIZER)
#define MAYBE_RenderViewHostPendingDeletionIsNotReused \
        DISABLED_RenderViewHostPendingDeletionIsNotReused
#else
#define MAYBE_RenderViewHostPendingDeletionIsNotReused \
        RenderViewHostPendingDeletionIsNotReused
#endif
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       MAYBE_RenderViewHostPendingDeletionIsNotReused) {
  GURL a_url(embedded_test_server()->GetURL("a.com", "/title1.html"));
  NavigateToURL(shell(), a_url);

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  RenderFrameHostImpl* rfh = root->current_frame_host();
  RenderViewHostImpl* rvh = rfh->render_view_host();
  RenderFrameDeletedObserver deleted_observer(rfh);

  // Install a BrowserMessageFilter to drop SwapOut ACK messages in A's
  // process.
  scoped_refptr<SwapoutACKMessageFilter> filter = new SwapoutACKMessageFilter();
  rfh->GetProcess()->AddFilter(filter.get());

  // Navigate to B.  This must wait for DidCommitProvisionalLoad, as opposed to
  // DidStopLoading, since otherwise the SwapOut timer might call OnSwappedOut
  // and destroy |rvh| before its pending deletion status is checked.
  GURL b_url(embedded_test_server()->GetURL("b.com", "/title2.html"));
  TestFrameNavigationObserver commit_observer(root);
  shell()->LoadURL(b_url);
  commit_observer.Wait();

  // Since the SwapOut ACK for A->B is dropped, the first page's
  // RenderFrameHost and RenderViewHost should be pending deletion after the
  // last navigation.
  EXPECT_TRUE(root->render_manager()->IsPendingDeletion(rfh));
  EXPECT_TRUE(rvh->is_pending_deletion());

  // Wait for process A to exit so we can reinitialize it cleanly for the next
  // navigation. This can be removed once https://crbug.com/535246 is fixed.
  RenderProcessHostWatcher process_exit_observer(
      rvh->GetProcess(),
      RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
  process_exit_observer.Wait();

  // Start a navigation back to A and check that the RenderViewHost wasn't
  // reused.
  TestNavigationObserver navigation_observer(shell()->web_contents());
  shell()->LoadURL(a_url);
  RenderViewHostImpl* pending_rvh =
      IsBrowserSideNavigationEnabled()
          ? root->render_manager()->speculative_frame_host()->render_view_host()
          : root->render_manager()->pending_render_view_host();
  EXPECT_EQ(rvh->GetSiteInstance(), pending_rvh->GetSiteInstance());
  EXPECT_NE(rvh, pending_rvh);

  // Simulate that the dropped SwapOut ACK message arrives now on the original
  // RenderFrameHost, which should now get deleted.
  rfh->OnSwappedOut();
  EXPECT_TRUE(deleted_observer.deleted());

  // Make sure the last navigation finishes without crashing.
  navigation_observer.Wait();
}

// Check that when a cross-process frame acquires focus, the old focused frame
// loses focus and fires blur events.  Starting on a page with a cross-site
// subframe, simulate mouse clicks to switch focus from root frame to subframe
// and then back to root frame.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       CrossProcessFocusChangeFiresBlurEvents) {
  GURL main_url(
      embedded_test_server()->GetURL("a.com", "/page_with_input_field.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  EXPECT_EQ(
      " Site A ------------ proxies for B\n"
      "   +--Site B ------- proxies for A\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/",
      DepictFrameTree(root));

  // Focus the main frame's text field.  The return value "input-focus"
  // indicates that the focus event was fired correctly.
  std::string result;
  EXPECT_TRUE(ExecuteScriptAndExtractString(shell()->web_contents(),
                                            "focusInputField()", &result));
  EXPECT_EQ(result, "input-focus");

  // The main frame should be focused.
  EXPECT_EQ(root, root->frame_tree()->GetFocusedFrame());

  DOMMessageQueue msg_queue;

  // Click on the cross-process subframe.
  SimulateMouseClick(
      root->child_at(0)->current_frame_host()->GetRenderWidgetHost(), 1, 1);

  // Check that the main frame lost focus and fired blur event on the input
  // text field.
  std::string status;
  while (msg_queue.WaitForMessage(&status)) {
    if (status == "\"input-blur\"")
      break;
  }

  // The subframe should now be focused.
  EXPECT_EQ(root->child_at(0), root->frame_tree()->GetFocusedFrame());

  // Click on the root frame.
  SimulateMouseClick(
      shell()->web_contents()->GetRenderViewHost()->GetWidget(), 1, 1);

  // Check that the subframe lost focus and fired blur event on its
  // document's body.
  while (msg_queue.WaitForMessage(&status)) {
    if (status == "\"document-blur\"")
      break;
  }

  // The root frame should be focused again.
  EXPECT_EQ(root, root->frame_tree()->GetFocusedFrame());
}

// Check that when a cross-process subframe is focused, its parent's
// document.activeElement correctly returns the corresponding <iframe> element.
// The test sets up an A-embed-B-embed-C page and shifts focus A->B->A->C,
// checking document.activeElement after each change.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, DocumentActiveElement) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b(c))"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  EXPECT_EQ(
      " Site A ------------ proxies for B C\n"
      "   +--Site B ------- proxies for A C\n"
      "        +--Site C -- proxies for A B\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/\n"
      "      C = http://c.com/",
      DepictFrameTree(root));

  FrameTreeNode* child = root->child_at(0);
  FrameTreeNode* grandchild = root->child_at(0)->child_at(0);

  // The main frame should be focused to start with.
  EXPECT_EQ(root, root->frame_tree()->GetFocusedFrame());

  // Focus the b.com frame.
  FocusFrame(child);
  EXPECT_EQ(child, root->frame_tree()->GetFocusedFrame());

  // Helper function to check a property of document.activeElement in the
  // specified frame.
  auto verify_active_element_property = [](RenderFrameHost* rfh,
                                           const std::string& property,
                                           const std::string& expected_value) {
    std::string script = base::StringPrintf(
        "window.domAutomationController.send(document.activeElement.%s);",
        property.c_str());
    std::string result;
    EXPECT_TRUE(ExecuteScriptAndExtractString(rfh, script, &result));
    EXPECT_EQ(expected_value, base::ToLowerASCII(result));
  };

  // Verify that document.activeElement on main frame points to the <iframe>
  // element for the b.com frame.
  RenderFrameHost* root_rfh = root->current_frame_host();
  verify_active_element_property(root_rfh, "tagName", "iframe");
  verify_active_element_property(root_rfh, "src", child->current_url().spec());

  // Focus the a.com main frame again.
  FocusFrame(root);
  EXPECT_EQ(root, root->frame_tree()->GetFocusedFrame());

  // Main frame document's <body> should now be the active element.
  verify_active_element_property(root_rfh, "tagName", "body");

  // Now shift focus from main frame to c.com frame.
  FocusFrame(grandchild);

  // Check document.activeElement in main frame.  It should still point to
  // <iframe> for the b.com frame, since Blink computes the focused iframe
  // element by walking the parent chain of the focused frame until it hits the
  // current frame.  This logic should still work with remote frames.
  verify_active_element_property(root_rfh, "tagName", "iframe");
  verify_active_element_property(root_rfh, "src", child->current_url().spec());

  // Check document.activeElement in b.com subframe.  It should point to
  // <iframe> for the c.com frame.  This is a tricky case where B needs to find
  // out that focus changed from one remote frame to another (A to C).
  RenderFrameHost* child_rfh = child->current_frame_host();
  verify_active_element_property(child_rfh, "tagName", "iframe");
  verify_active_element_property(child_rfh, "src",
                                 grandchild->current_url().spec());
}

// Check that document.hasFocus() works properly with out-of-process iframes.
// The test builds a page with four cross-site frames and then focuses them one
// by one, checking the value of document.hasFocus() in all frames.  For any
// given focused frame, document.hasFocus() should return true for that frame
// and all its ancestor frames.
// Disabled due to flakes; see https://crbug.com/559273.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, DISABLED_DocumentHasFocus) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b(c),d)"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  EXPECT_EQ(
      " Site A ------------ proxies for B C D\n"
      "   |--Site B ------- proxies for A C D\n"
      "   |    +--Site C -- proxies for A B D\n"
      "   +--Site D ------- proxies for A B C\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/\n"
      "      C = http://c.com/\n"
      "      D = http://d.com/",
      DepictFrameTree(root));

  FrameTreeNode* child1 = root->child_at(0);
  FrameTreeNode* child2 = root->child_at(1);
  FrameTreeNode* grandchild = root->child_at(0)->child_at(0);

  // Helper function to check document.hasFocus() for a given frame.
  auto document_has_focus = [](FrameTreeNode* node) {
    bool hasFocus = false;
    EXPECT_TRUE(ExecuteScriptAndExtractBool(
        node->current_frame_host(),
        "window.domAutomationController.send(document.hasFocus())",
        &hasFocus));
    return hasFocus;
  };

  // The main frame should be focused to start with.
  EXPECT_EQ(root, root->frame_tree()->GetFocusedFrame());

  EXPECT_TRUE(document_has_focus(root));
  EXPECT_FALSE(document_has_focus(child1));
  EXPECT_FALSE(document_has_focus(grandchild));
  EXPECT_FALSE(document_has_focus(child2));

  FocusFrame(child1);
  EXPECT_EQ(child1, root->frame_tree()->GetFocusedFrame());

  EXPECT_TRUE(document_has_focus(root));
  EXPECT_TRUE(document_has_focus(child1));
  EXPECT_FALSE(document_has_focus(grandchild));
  EXPECT_FALSE(document_has_focus(child2));

  FocusFrame(grandchild);
  EXPECT_EQ(grandchild, root->frame_tree()->GetFocusedFrame());

  EXPECT_TRUE(document_has_focus(root));
  EXPECT_TRUE(document_has_focus(child1));
  EXPECT_TRUE(document_has_focus(grandchild));
  EXPECT_FALSE(document_has_focus(child2));

  FocusFrame(child2);
  EXPECT_EQ(child2, root->frame_tree()->GetFocusedFrame());

  EXPECT_TRUE(document_has_focus(root));
  EXPECT_FALSE(document_has_focus(child1));
  EXPECT_FALSE(document_has_focus(grandchild));
  EXPECT_TRUE(document_has_focus(child2));
}

// Check that window.focus works for cross-process subframes.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, SubframeWindowFocus) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b,c)"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  EXPECT_EQ(
      " Site A ------------ proxies for B C\n"
      "   |--Site B ------- proxies for A C\n"
      "   +--Site C ------- proxies for A B\n"
      "Where A = http://a.com/\n"
      "      B = http://b.com/\n"
      "      C = http://c.com/",
      DepictFrameTree(root));

  FrameTreeNode* child1 = root->child_at(0);
  FrameTreeNode* child2 = root->child_at(1);

  // The main frame should be focused to start with.
  EXPECT_EQ(root, root->frame_tree()->GetFocusedFrame());

  DOMMessageQueue msg_queue;

  // Register focus and blur events that will send messages when each frame's
  // window gets or loses focus.
  const char kSetupFocusEvents[] =
      "window.addEventListener('focus', function() {"
      "  domAutomationController.setAutomationId(0);"
      "  domAutomationController.send('%s-got-focus');"
      "});"
      "window.addEventListener('blur', function() {"
      "  domAutomationController.setAutomationId(0);"
      "  domAutomationController.send('%s-lost-focus');"
      "});";
  std::string script = base::StringPrintf(kSetupFocusEvents, "main", "main");
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(), script));
  script = base::StringPrintf(kSetupFocusEvents, "child1", "child1");
  EXPECT_TRUE(ExecuteScript(child1->current_frame_host(), script));
  script = base::StringPrintf(kSetupFocusEvents, "child2", "child2");
  EXPECT_TRUE(ExecuteScript(child2->current_frame_host(), script));

  // Execute window.focus on the B subframe from the A main frame.
  EXPECT_TRUE(ExecuteScript(root->current_frame_host(), "frames[0].focus()"));

  // Helper to wait for two specified messages to arrive on the specified
  // DOMMessageQueue, assuming that the two messages can arrive in any order.
  auto wait_for_two_messages = [](DOMMessageQueue* msg_queue,
                                  const std::string& msg1,
                                  const std::string& msg2) {
    bool msg1_received = false;
    bool msg2_received = false;
    std::string status;
    while (msg_queue->WaitForMessage(&status)) {
      if (status == msg1)
        msg1_received = true;
      if (status == msg2)
        msg2_received = true;
      if (msg1_received && msg2_received)
        break;
    }
  };

  // Process A should fire a blur event, and process B should fire a focus
  // event.  Wait for both events.
  wait_for_two_messages(&msg_queue, "\"main-lost-focus\"",
                        "\"child1-got-focus\"");

  // The B subframe should now be focused in the browser process.
  EXPECT_EQ(child1, root->frame_tree()->GetFocusedFrame());

  // Now, execute window.focus on the C subframe from A main frame.  This
  // checks that we can shift focus from one remote frame to another.
  EXPECT_TRUE(ExecuteScript(root->current_frame_host(), "frames[1].focus()"));

  // Wait for the two subframes (B and C) to fire blur and focus events.
  wait_for_two_messages(&msg_queue, "\"child1-lost-focus\"",
                        "\"child2-got-focus\"");

  // The C subframe should now be focused.
  EXPECT_EQ(child2, root->frame_tree()->GetFocusedFrame());

  // window.focus the main frame from the C subframe.
  EXPECT_TRUE(ExecuteScript(child2->current_frame_host(), "parent.focus()"));

  // Wait for the C subframe to blur and main frame to focus.
  wait_for_two_messages(&msg_queue, "\"child2-lost-focus\"",
                        "\"main-got-focus\"");

  // The main frame should now be focused.
  EXPECT_EQ(root, root->frame_tree()->GetFocusedFrame());
}

// There are no cursors on Android.
#if !defined(OS_ANDROID)
class CursorMessageFilter : public content::BrowserMessageFilter {
 public:
  CursorMessageFilter()
      : content::BrowserMessageFilter(ViewMsgStart),
        message_loop_runner_(new content::MessageLoopRunner),
        last_set_cursor_routing_id_(MSG_ROUTING_NONE) {}

  bool OnMessageReceived(const IPC::Message& message) override {
    if (message.type() == ViewHostMsg_SetCursor::ID) {
      content::BrowserThread::PostTask(
          content::BrowserThread::UI, FROM_HERE,
          base::Bind(&CursorMessageFilter::OnSetCursor, this,
                     message.routing_id()));
    }
    return false;
  }

  void OnSetCursor(int routing_id) {
    last_set_cursor_routing_id_ = routing_id;
    message_loop_runner_->Quit();
  }

  int last_set_cursor_routing_id() const { return last_set_cursor_routing_id_; }

  void Wait() {
    last_set_cursor_routing_id_ = MSG_ROUTING_NONE;
    message_loop_runner_->Run();
  }

 private:
  ~CursorMessageFilter() override {}

  scoped_refptr<content::MessageLoopRunner> message_loop_runner_;
  int last_set_cursor_routing_id_;

  DISALLOW_COPY_AND_ASSIGN(CursorMessageFilter);
};

// Verify that we receive a mouse cursor update message when we mouse over
// a text field contained in an out-of-process iframe.
// Fails under TSan.  http://crbug.com/545237
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       DISABLED_CursorUpdateFromReceivedFromCrossSiteIframe) {
  GURL main_url(embedded_test_server()->GetURL(
      "/frame_tree/page_with_positioned_frame.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  FrameTreeNode* child_node = root->child_at(0);
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(),
            child_node->current_frame_host()->GetSiteInstance());

  scoped_refptr<CursorMessageFilter> filter = new CursorMessageFilter();
  child_node->current_frame_host()->GetProcess()->AddFilter(filter.get());

  // Send a MouseMove to the subframe. The frame contains text, and moving the
  // mouse over it should cause the renderer to send a mouse cursor update.
  blink::WebMouseEvent mouse_event;
  mouse_event.type = blink::WebInputEvent::MouseMove;
  mouse_event.x = 60;
  mouse_event.y = 60;
  RenderWidgetHost* rwh_child =
      root->child_at(0)->current_frame_host()->GetRenderWidgetHost();
  RenderWidgetHostViewBase* root_view = static_cast<RenderWidgetHostViewBase*>(
      root->current_frame_host()->GetRenderWidgetHost()->GetView());
  static_cast<WebContentsImpl*>(shell()->web_contents())
      ->GetInputEventRouter()
      ->RouteMouseEvent(root_view, &mouse_event);

  // CursorMessageFilter::Wait() implicitly tests whether we receive a
  // ViewHostMsg_SetCursor message from the renderer process, because it does
  // does not return otherwise.
  filter->Wait();
  EXPECT_EQ(filter->last_set_cursor_routing_id(), rwh_child->GetRoutingID());
}
#endif

// Tests that we are using the correct RenderFrameProxy when navigating an
// opener window.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, OpenerSetLocation) {
  // Navigate the main window.
  GURL main_url(embedded_test_server()->GetURL("/title1.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(), main_url);

  // Load cross-site page into a new window.
  GURL cross_url = embedded_test_server()->GetURL("foo.com", "/title1.html");
  Shell* popup = OpenPopup(shell()->web_contents(), cross_url, "");
  EXPECT_EQ(popup->web_contents()->GetLastCommittedURL(), cross_url);

  // Use new window to navigate main window.
  std::string script =
      "window.opener.location.href = '" + cross_url.spec() + "'";
  EXPECT_TRUE(ExecuteScript(popup->web_contents(), script));
  EXPECT_TRUE(WaitForLoadStop(shell()->web_contents()));
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(), cross_url);
}

// Ensure that a cross-process subframe can receive keyboard events when in
// focus.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       SubframeKeyboardEventRouting) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/frame_tree/page_with_one_frame.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  WebContentsImpl* web_contents =
      static_cast<WebContentsImpl*>(shell()->web_contents());
  FrameTreeNode* root = web_contents->GetFrameTree()->root();

  GURL frame_url(
      embedded_test_server()->GetURL("b.com", "/page_with_input_field.html"));
  NavigateFrameToURL(root->child_at(0), frame_url);
  EXPECT_TRUE(WaitForRenderFrameReady(root->child_at(0)->current_frame_host()));

  // Focus the subframe and then its input field.  The return value
  // "input-focus" will be sent once the input field's focus event fires.
  FocusFrame(root->child_at(0));
  std::string result;
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      root->child_at(0)->current_frame_host(), "focusInputField()", &result));
  EXPECT_EQ(result, "input-focus");

  // The subframe should now be focused.
  EXPECT_EQ(root->child_at(0), root->frame_tree()->GetFocusedFrame());

  // Generate a few keyboard events and route them to currently focused frame.
  SimulateKeyPress(web_contents, ui::VKEY_F, false, false, false, false);
  SimulateKeyPress(web_contents, ui::VKEY_O, false, false, false, false);
  SimulateKeyPress(web_contents, ui::VKEY_O, false, false, false, false);

  // Verify that the input field in the subframe received the keystrokes.
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      root->child_at(0)->current_frame_host(),
      "window.domAutomationController.send(getInputFieldText());", &result));
  EXPECT_EQ("FOO", result);
}

// Ensure that sequential focus navigation (advancing focused elements with
// <tab> and <shift-tab>) works across cross-process subframes.
// The test sets up six inputs fields in a page with two cross-process
// subframes:
//                 child1            child2
//             /------------\    /------------\.
//             | 2. <input> |    | 4. <input> |
//  1. <input> | 3. <input> |    | 5. <input> |  6. <input>
//             \------------/    \------------/.
//
// The test then presses <tab> six times to cycle through focused elements 1-6.
// The test then repeats this with <shift-tab> to cycle in reverse order.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, SequentialFocusNavigation) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b,c)"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  WebContents* contents = shell()->web_contents();
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(contents)->GetFrameTree()->root();

  // Assign a name to each frame.  This will be sent along in test messages
  // from focus events.
  EXPECT_TRUE(ExecuteScript(root->current_frame_host(),
                            "window.name = 'root';"));
  EXPECT_TRUE(ExecuteScript(root->child_at(0)->current_frame_host(),
                            "window.name = 'child1';"));
  EXPECT_TRUE(ExecuteScript(root->child_at(1)->current_frame_host(),
                            "window.name = 'child2';"));

  // This script will insert two <input> fields in the document, one at the
  // beginning and one at the end.  For root frame, this means that we will
  // have an <input>, then two <iframe> elements, then another <input>.
  std::string script =
      "function onFocus(e) {"
      "  domAutomationController.setAutomationId(0);"
      "  domAutomationController.send(window.name + '-focused-' + e.target.id);"
      "}"
      "var input1 = document.createElement('input');"
      "input1.id = 'input1';"
      "var input2 = document.createElement('input');"
      "input2.id = 'input2';"
      "document.body.insertBefore(input1, document.body.firstChild);"
      "document.body.appendChild(input2);"
      "input1.addEventListener('focus', onFocus, false);"
      "input2.addEventListener('focus', onFocus, false);";

  // Add two input fields to each of the three frames.
  EXPECT_TRUE(ExecuteScript(root->current_frame_host(), script));
  EXPECT_TRUE(ExecuteScript(root->child_at(0)->current_frame_host(), script));
  EXPECT_TRUE(ExecuteScript(root->child_at(1)->current_frame_host(), script));

  // Helper to simulate a tab press and wait for a focus message.
  auto press_tab_and_wait_for_message = [contents](bool reverse) {
    DOMMessageQueue msg_queue;
    std::string reply;
    SimulateKeyPress(contents, ui::VKEY_TAB, false, reverse /* shift */, false,
                     false);
    EXPECT_TRUE(msg_queue.WaitForMessage(&reply));
    return reply;
  };

  // Press <tab> six times to focus each of the <input> elements in turn.
  EXPECT_EQ("\"root-focused-input1\"", press_tab_and_wait_for_message(false));
  EXPECT_EQ(root, root->frame_tree()->GetFocusedFrame());
  EXPECT_EQ("\"child1-focused-input1\"", press_tab_and_wait_for_message(false));
  EXPECT_EQ(root->child_at(0), root->frame_tree()->GetFocusedFrame());
  EXPECT_EQ("\"child1-focused-input2\"", press_tab_and_wait_for_message(false));
  EXPECT_EQ("\"child2-focused-input1\"", press_tab_and_wait_for_message(false));
  EXPECT_EQ(root->child_at(1), root->frame_tree()->GetFocusedFrame());
  EXPECT_EQ("\"child2-focused-input2\"", press_tab_and_wait_for_message(false));
  EXPECT_EQ("\"root-focused-input2\"", press_tab_and_wait_for_message(false));
  EXPECT_EQ(root, root->frame_tree()->GetFocusedFrame());

  // Now, press <shift-tab> to navigate focus in the reverse direction.
  EXPECT_EQ("\"child2-focused-input2\"", press_tab_and_wait_for_message(true));
  EXPECT_EQ(root->child_at(1), root->frame_tree()->GetFocusedFrame());
  EXPECT_EQ("\"child2-focused-input1\"", press_tab_and_wait_for_message(true));
  EXPECT_EQ("\"child1-focused-input2\"", press_tab_and_wait_for_message(true));
  EXPECT_EQ(root->child_at(0), root->frame_tree()->GetFocusedFrame());
  EXPECT_EQ("\"child1-focused-input1\"", press_tab_and_wait_for_message(true));
  EXPECT_EQ("\"root-focused-input1\"", press_tab_and_wait_for_message(true));
  EXPECT_EQ(root, root->frame_tree()->GetFocusedFrame());
}

// A WebContentsDelegate to capture ContextMenu creation events.
class ContextMenuObserverDelegate : public WebContentsDelegate {
 public:
  ContextMenuObserverDelegate()
      : context_menu_created_(false),
        message_loop_runner_(new MessageLoopRunner) {}

  ~ContextMenuObserverDelegate() override {}

  bool HandleContextMenu(const content::ContextMenuParams& params) override {
    context_menu_created_ = true;
    menu_params_ = params;
    message_loop_runner_->Quit();
    return true;
  }

  ContextMenuParams getParams() { return menu_params_; }

  void Wait() {
    if (!context_menu_created_)
      message_loop_runner_->Run();
    context_menu_created_ = false;
  }

 private:
  bool context_menu_created_;
  ContextMenuParams menu_params_;

  // The MessageLoopRunner used to spin the message loop.
  scoped_refptr<MessageLoopRunner> message_loop_runner_;

  DISALLOW_COPY_AND_ASSIGN(ContextMenuObserverDelegate);
};

// Test that a mouse right-click to an out-of-process iframe causes a context
// menu to be generated with the correct screen position.
#if defined(OS_ANDROID)
// Browser process hit testing is not implemented on Android.
// https://crbug.com/491334
#define MAYBE_CreateContextMenuTest DISABLED_CreateContextMenuTest
#else
#define MAYBE_CreateContextMenuTest CreateContextMenuTest
#endif
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, MAYBE_CreateContextMenuTest) {
  GURL main_url(embedded_test_server()->GetURL(
      "/frame_tree/page_with_positioned_frame.html"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  ASSERT_EQ(1U, root->child_count());

  FrameTreeNode* child_node = root->child_at(0);
  GURL site_url(embedded_test_server()->GetURL("baz.com", "/title1.html"));
  EXPECT_EQ(site_url, child_node->current_url());
  EXPECT_NE(shell()->web_contents()->GetSiteInstance(),
            child_node->current_frame_host()->GetSiteInstance());

  RenderWidgetHostViewBase* root_view = static_cast<RenderWidgetHostViewBase*>(
      root->current_frame_host()->GetRenderWidgetHost()->GetView());
  RenderWidgetHostViewBase* rwhv_child = static_cast<RenderWidgetHostViewBase*>(
      child_node->current_frame_host()->GetRenderWidgetHost()->GetView());

  // Ensure that the child process renderer is ready to have input events
  // routed to it. This happens when the browser process has received
  // updated compositor surfaces from both renderer processes.
  gfx::Point point(75, 75);
  gfx::Point transformed_point;
  while (root_view->SurfaceIdNamespaceAtPoint(nullptr, point,
                                              &transformed_point) !=
         rwhv_child->GetSurfaceIdNamespace()) {
    base::RunLoop run_loop;
    base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), TestTimeouts::tiny_timeout());
    run_loop.Run();
  }

  // A WebContentsDelegate to listen for the ShowContextMenu message.
  ContextMenuObserverDelegate context_menu_delegate;
  shell()->web_contents()->SetDelegate(&context_menu_delegate);

  RenderWidgetHostInputEventRouter* router =
      static_cast<WebContentsImpl*>(shell()->web_contents())
          ->GetInputEventRouter();

  // Target right-click event to child frame.
  blink::WebMouseEvent click_event;
  click_event.type = blink::WebInputEvent::MouseDown;
  click_event.button = blink::WebPointerProperties::ButtonRight;
  click_event.x = 75;
  click_event.y = 75;
  click_event.clickCount = 1;
  router->RouteMouseEvent(root_view, &click_event);

  // We also need a MouseUp event, needed by Windows.
  click_event.type = blink::WebInputEvent::MouseUp;
  click_event.x = 75;
  click_event.y = 75;
  router->RouteMouseEvent(root_view, &click_event);

  context_menu_delegate.Wait();

  ContextMenuParams params = context_menu_delegate.getParams();

  EXPECT_EQ(point.x(), params.x);
  EXPECT_EQ(point.y(), params.y);
}

// Test for https://crbug.com/526304, where a parent frame executes a
// remote-to-local navigation on a child frame and immediately removes the same
// child frame.  This test exercises the path where the detach happens before
// the provisional local frame is created.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       NavigateProxyAndDetachBeforeProvisionalFrameCreation) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b,b)"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  WebContents* contents = shell()->web_contents();
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(contents)->GetFrameTree()->root();
  EXPECT_EQ(2U, root->child_count());

  // Navigate the first child frame to 'about:blank' (which is a
  // remote-to-local transition), and then detach it.
  FrameDeletedObserver observer(root->child_at(0));
  std::string script =
      "var f = document.querySelector('iframe');"
      "f.contentWindow.location.href = 'about:blank';"
      "setTimeout(function() { document.body.removeChild(f); }, 0);";
  EXPECT_TRUE(ExecuteScript(root->current_frame_host(), script));
  observer.Wait();
  EXPECT_EQ(1U, root->child_count());

  // Make sure the main frame renderer does not crash and ignores the
  // navigation to the frame that's already been deleted.
  int child_count = 0;
  EXPECT_TRUE(ExecuteScriptAndExtractInt(
      root->current_frame_host(),
      "domAutomationController.send(frames.length)",
      &child_count));
  EXPECT_EQ(1, child_count);
}

// Test for a variation of https://crbug.com/526304, where a child frame does a
// remote-to-local navigation, and the parent frame removes that child frame
// after the provisional local frame is created and starts to navigate, but
// before it commits.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       NavigateProxyAndDetachBeforeCommit) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b,b)"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  WebContents* contents = shell()->web_contents();
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(contents)->GetFrameTree()->root();
  EXPECT_EQ(2U, root->child_count());
  FrameTreeNode* child = root->child_at(0);

  // Start a remote-to-local navigation for the child, but don't wait for
  // commit.
  GURL same_site_url(embedded_test_server()->GetURL("a.com", "/title1.html"));
  NavigationController::LoadURLParams params(same_site_url);
  params.transition_type = ui::PAGE_TRANSITION_LINK;
  params.frame_tree_node_id = child->frame_tree_node_id();
  child->navigator()->GetController()->LoadURLWithParams(params);

  // Tell parent to remove the first child.  This should happen after the
  // previous navigation starts but before it commits.
  FrameDeletedObserver observer(child);
  EXPECT_TRUE(ExecuteScript(
      root->current_frame_host(),
      "document.body.removeChild(document.querySelector('iframe'));"));
  observer.Wait();
  EXPECT_EQ(1U, root->child_count());

  // Make sure the a.com renderer does not crash.
  int child_count = 0;
  EXPECT_TRUE(ExecuteScriptAndExtractInt(
      root->current_frame_host(),
      "domAutomationController.send(frames.length)",
      &child_count));
  EXPECT_EQ(1, child_count);
}

// Test for https://crbug.com/568670.  In A-embed-B, simultaneously have B
// create a new (local) child frame, and have A detach B's proxy.  The child
// frame creation sends an IPC to create a new proxy in A's process, and if
// that IPC arrives after the detach, the new frame's parent (a proxy) won't be
// available, and this shouldn't cause RenderFrameProxy::CreateFrameProxy to
// crash.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       RaceBetweenCreateChildFrameAndDetachParentProxy) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b)"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  WebContents* contents = shell()->web_contents();
  FrameTreeNode* root =
      static_cast<WebContentsImpl*>(contents)->GetFrameTree()->root();

  // Simulate subframe B creating a new child frame in parallel to main frame A
  // detaching subframe B.  We can't use ExecuteScript in both A and B to do
  // this simultaneously, as that won't guarantee the timing that we want.
  // Instead, tell A to detach B and then send a fake proxy creation IPC to A
  // that would've come from create-child-frame code in B.  Prepare parameters
  // for that IPC ahead of the detach, while B's FrameTreeNode still exists.
  SiteInstance* site_instance_a = root->current_frame_host()->GetSiteInstance();
  RenderProcessHost* process_a =
      root->render_manager()->current_frame_host()->GetProcess();
  int new_routing_id = process_a->GetNextRoutingID();
  int view_routing_id =
      root->frame_tree()->GetRenderViewHost(site_instance_a)->GetRoutingID();
  int parent_routing_id =
      root->child_at(0)->render_manager()->GetProxyToParent()->GetRoutingID();

  // Tell main frame A to delete its subframe B.
  FrameDeletedObserver observer(root->child_at(0));
  EXPECT_TRUE(ExecuteScript(
      root->current_frame_host(),
      "document.body.removeChild(document.querySelector('iframe'));"));

  // Send the message to create a proxy for B's new child frame in A.  This
  // used to crash, as parent_routing_id refers to a proxy that doesn't exist
  // anymore.
  process_a->Send(new FrameMsg_NewFrameProxy(
      new_routing_id, view_routing_id, MSG_ROUTING_NONE, parent_routing_id,
      FrameReplicationState()));

  // Ensure the subframe is detached in the browser process.
  observer.Wait();
  EXPECT_EQ(0U, root->child_count());

  // Make sure process A did not crash.
  int child_count = 0;
  EXPECT_TRUE(ExecuteScriptAndExtractInt(
      root->current_frame_host(),
      "domAutomationController.send(frames.length)",
      &child_count));
  EXPECT_EQ(0, child_count);
}

// This test ensures that the RenderFrame isn't leaked in the renderer process
// if a pending cross-process navigation is cancelled. The test works by trying
// to create a new RenderFrame with the same routing id. If there is an
// entry with the same routing ID, a CHECK is hit and the process crashes.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       SubframePendingAndBackToSameSiteInstance) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b)"));
  NavigateToURL(shell(), main_url);

  // Capture the FrameTreeNode this test will be navigating.
  FrameTreeNode* node = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root()
                            ->child_at(0);
  EXPECT_TRUE(node);
  EXPECT_NE(node->current_frame_host()->GetSiteInstance(),
            node->parent()->current_frame_host()->GetSiteInstance());

  // Navigate to the site of the parent, but to a page that will not commit.
  GURL same_site_url(embedded_test_server()->GetURL("a.com", "/title1.html"));
  NavigationStallDelegate stall_delegate(same_site_url);
  ResourceDispatcherHost::Get()->SetDelegate(&stall_delegate);
  {
    NavigationController::LoadURLParams params(same_site_url);
    params.transition_type = ui::PAGE_TRANSITION_LINK;
    params.frame_tree_node_id = node->frame_tree_node_id();
    node->navigator()->GetController()->LoadURLWithParams(params);
  }

  // Grab the routing id of the pending RenderFrameHost and set up a process
  // observer to ensure there is no crash when a new RenderFrame creation is
  // attempted.
  RenderProcessHost* process =
      IsBrowserSideNavigationEnabled()
          ? node->render_manager()->speculative_frame_host()->GetProcess()
          : node->render_manager()->pending_frame_host()->GetProcess();
  RenderProcessHostWatcher watcher(
      process, RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
  int frame_routing_id =
      IsBrowserSideNavigationEnabled()
          ? node->render_manager()->speculative_frame_host()->GetRoutingID()
          : node->render_manager()->pending_frame_host()->GetRoutingID();
  int proxy_routing_id =
      node->render_manager()->GetProxyToParent()->GetRoutingID();

  // Now go to c.com so the navigation to a.com is cancelled and send an IPC
  // to create a new RenderFrame with the routing id of the previously pending
  // one.
  NavigateFrameToURL(node,
                     embedded_test_server()->GetURL("c.com", "/title2.html"));
  {
    FrameMsg_NewFrame_Params params;
    params.routing_id = frame_routing_id;
    params.proxy_routing_id = proxy_routing_id;
    params.opener_routing_id = MSG_ROUTING_NONE;
    params.parent_routing_id =
        shell()->web_contents()->GetMainFrame()->GetRoutingID();
    params.previous_sibling_routing_id = MSG_ROUTING_NONE;
    params.widget_params.routing_id = MSG_ROUTING_NONE;
    params.widget_params.hidden = true;
    params.replication_state.name = "name";
    params.replication_state.unique_name = "name";

    process->Send(new FrameMsg_NewFrame(params));
  }

  // The test must wait for the process to exit, but if there is no leak, the
  // RenderFrame will be properly created and there will be no crash.
  // Therefore, navigate the main frame to completely different site, which
  // will cause the original process to exit cleanly.
  EXPECT_TRUE(NavigateToURL(
      shell(), embedded_test_server()->GetURL("d.com", "/title3.html")));
  watcher.Wait();
  EXPECT_TRUE(watcher.did_exit_normally());

  ResourceDispatcherHost::Get()->SetDelegate(nullptr);
}

// This test ensures that the RenderFrame isn't leaked in the renderer process
// when a remote parent detaches a child frame. The test works by trying
// to create a new RenderFrame with the same routing id. If there is an
// entry with the same routing ID, a CHECK is hit and the process crashes.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, ParentDetachRemoteChild) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b,b)"));
  NavigateToURL(shell(), main_url);

  WebContentsImpl* web_contents =
      static_cast<WebContentsImpl*>(shell()->web_contents());
  EXPECT_EQ(2U, web_contents->GetFrameTree()->root()->child_count());

  // Capture the FrameTreeNode this test will be navigating.
  FrameTreeNode* node = web_contents->GetFrameTree()->root()->child_at(0);
  EXPECT_TRUE(node);
  EXPECT_NE(node->current_frame_host()->GetSiteInstance(),
            node->parent()->current_frame_host()->GetSiteInstance());

  // Grab the routing id of the first child RenderFrameHost and set up a process
  // observer to ensure there is no crash when a new RenderFrame creation is
  // attempted.
  RenderProcessHost* process = node->current_frame_host()->GetProcess();
  RenderProcessHostWatcher watcher(
      process, RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
  int frame_routing_id = node->current_frame_host()->GetRoutingID();
  int widget_routing_id =
      node->current_frame_host()->GetRenderWidgetHost()->GetRoutingID();
  int parent_routing_id =
      node->parent()->render_manager()->GetRoutingIdForSiteInstance(
          node->current_frame_host()->GetSiteInstance());

  // Have the parent frame remove the child frame from its DOM. This should
  // result in the child RenderFrame being deleted in the remote process.
  EXPECT_TRUE(ExecuteScript(web_contents,
                            "document.body.removeChild("
                            "document.querySelectorAll('iframe')[0])"));
  EXPECT_EQ(1U, web_contents->GetFrameTree()->root()->child_count());

  {
    FrameMsg_NewFrame_Params params;
    params.routing_id = frame_routing_id;
    params.proxy_routing_id = MSG_ROUTING_NONE;
    params.opener_routing_id = MSG_ROUTING_NONE;
    params.parent_routing_id = parent_routing_id;
    params.previous_sibling_routing_id = MSG_ROUTING_NONE;
    params.widget_params.routing_id = widget_routing_id;
    params.widget_params.hidden = true;
    params.replication_state.name = "name";
    params.replication_state.unique_name = "name";

    process->Send(new FrameMsg_NewFrame(params));
  }

  // The test must wait for the process to exit, but if there is no leak, the
  // RenderFrame will be properly created and there will be no crash.
  // Therefore, navigate the remaining subframe to completely different site,
  // which will cause the original process to exit cleanly.
  NavigateFrameToURL(
      web_contents->GetFrameTree()->root()->child_at(0),
      embedded_test_server()->GetURL("d.com", "/title3.html"));
  watcher.Wait();
  EXPECT_TRUE(watcher.did_exit_normally());
}

IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, VisibilityChanged) {
  GURL main_url(
      embedded_test_server()->GetURL("a.com", "/page_with_iframe.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(), main_url);

  GURL cross_site_url =
      embedded_test_server()->GetURL("oopif.com", "/title1.html");

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  TestNavigationObserver observer(shell()->web_contents());

  NavigateFrameToURL(root->child_at(0), cross_site_url);
  EXPECT_EQ(cross_site_url, observer.last_navigation_url());
  EXPECT_TRUE(observer.last_navigation_succeeded());

  RenderWidgetHostImpl* render_widget_host =
      root->child_at(0)->current_frame_host()->GetRenderWidgetHost();
  EXPECT_FALSE(render_widget_host->is_hidden());

  std::string show_script =
      "document.querySelector('iframe').style.visibility = 'visible';";
  std::string hide_script =
      "document.querySelector('iframe').style.visibility = 'hidden';";

  // Verify that hiding leads to a notification from RenderWidgetHost.
  RenderWidgetHostVisibilityObserver hide_observer(
      root->child_at(0)->current_frame_host()->GetRenderWidgetHost(), false);
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(), hide_script));
  EXPECT_TRUE(hide_observer.WaitUntilSatisfied());

  // Verify showing leads to a notification as well.
  RenderWidgetHostVisibilityObserver show_observer(
      root->child_at(0)->current_frame_host()->GetRenderWidgetHost(), true);
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(), show_script));
  EXPECT_TRUE(show_observer.WaitUntilSatisfied());
}

// Verify that sandbox flags inheritance works across multiple levels of
// frames.  See https://crbug.com/576845.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, SandboxFlagsInheritance) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a)"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  // Set sandbox flags for child frame.
  EXPECT_TRUE(ExecuteScript(
      root->current_frame_host(),
      "document.querySelector('iframe').sandbox = 'allow-scripts';"));

  // Calculate expected flags.  Note that "allow-scripts" resets both
  // WebSandboxFlags::Scripts and WebSandboxFlags::AutomaticFeatures bits per
  // blink::parseSandboxPolicy().
  blink::WebSandboxFlags expected_flags =
      blink::WebSandboxFlags::All & ~blink::WebSandboxFlags::Scripts &
      ~blink::WebSandboxFlags::AutomaticFeatures;
  EXPECT_EQ(expected_flags, root->child_at(0)->pending_sandbox_flags());
  EXPECT_EQ(blink::WebSandboxFlags::None,
            root->child_at(0)->effective_sandbox_flags());

  // Navigate child frame so that the sandbox flags take effect.  Use a page
  // with three levels of frames and make sure all frames properly inherit
  // sandbox flags.
  GURL frame_url(embedded_test_server()->GetURL(
      "b.com", "/cross_site_iframe_factory.html?b(c(d))"));
  TestFrameNavigationObserver frame_observer(root->child_at(0));
  NavigateFrameToURL(root->child_at(0), frame_url);
  frame_observer.Wait();
  // Wait for subframes to load as well.
  ASSERT_TRUE(WaitForLoadStop(shell()->web_contents()));

  // Check each new frame's sandbox flags on the browser process side.
  FrameTreeNode* b_child = root->child_at(0);
  FrameTreeNode* c_child = b_child->child_at(0);
  FrameTreeNode* d_child = c_child->child_at(0);
  EXPECT_EQ(expected_flags, b_child->effective_sandbox_flags());
  EXPECT_EQ(expected_flags, c_child->effective_sandbox_flags());
  EXPECT_EQ(expected_flags, d_child->effective_sandbox_flags());

  // Check whether each frame is sandboxed on the renderer side, by seeing if
  // each frame's origin is unique ("null").
  EXPECT_EQ("null", GetDocumentOrigin(b_child));
  EXPECT_EQ("null", GetDocumentOrigin(c_child));
  EXPECT_EQ("null", GetDocumentOrigin(d_child));
}

// Check that sandbox flags are not inherited before they take effect.  Create
// a child frame, update its sandbox flags but don't navigate the frame, and
// ensure that a new cross-site grandchild frame doesn't inherit the new flags
// (which shouldn't have taken effect).
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       SandboxFlagsNotInheritedBeforeNavigation) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a)"));
  NavigateToURL(shell(), main_url);

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  // Set sandbox flags for child frame.
  EXPECT_TRUE(ExecuteScript(
      root->current_frame_host(),
      "document.querySelector('iframe').sandbox = 'allow-scripts';"));

  // These flags should be pending but not take effect, since there's been no
  // navigation.
  blink::WebSandboxFlags expected_flags =
      blink::WebSandboxFlags::All & ~blink::WebSandboxFlags::Scripts &
      ~blink::WebSandboxFlags::AutomaticFeatures;
  FrameTreeNode* child = root->child_at(0);
  EXPECT_EQ(expected_flags, child->pending_sandbox_flags());
  EXPECT_EQ(blink::WebSandboxFlags::None, child->effective_sandbox_flags());

  // Add a new grandchild frame and navigate it cross-site.
  RenderFrameHostCreatedObserver frame_observer(shell()->web_contents(), 1);
  EXPECT_TRUE(ExecuteScript(
      child->current_frame_host(),
      "document.body.appendChild(document.createElement('iframe'));"));
  frame_observer.Wait();

  FrameTreeNode* grandchild = child->child_at(0);
  GURL frame_url(embedded_test_server()->GetURL("b.com", "/title1.html"));
  TestFrameNavigationObserver navigation_observer(grandchild);
  NavigateFrameToURL(grandchild, frame_url);
  navigation_observer.Wait();

  // Since the update flags haven't yet taken effect in its parent, this
  // grandchild frame should not be sandboxed.
  EXPECT_EQ(blink::WebSandboxFlags::None, grandchild->pending_sandbox_flags());
  EXPECT_EQ(blink::WebSandboxFlags::None,
            grandchild->effective_sandbox_flags());

  // Check that the grandchild frame isn't sandboxed on the renderer side.  If
  // sandboxed, its origin would be unique ("null").
  EXPECT_EQ(frame_url.GetOrigin().spec(), GetDocumentOrigin(grandchild) + "/");
}

// Verify that popups opened from sandboxed frames inherit sandbox flags from
// their opener, and that they keep these inherited flags after being navigated
// cross-site.  See https://crbug.com/483584.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       NewPopupInheritsSandboxFlagsFromOpener) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a)"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  // Set sandbox flags for child frame.
  EXPECT_TRUE(ExecuteScript(root->current_frame_host(),
                            "document.querySelector('iframe').sandbox = "
                            "    'allow-scripts allow-popups';"));

  // Calculate expected flags.  Note that "allow-scripts" resets both
  // WebSandboxFlags::Scripts and WebSandboxFlags::AutomaticFeatures bits per
  // blink::parseSandboxPolicy().
  blink::WebSandboxFlags expected_flags =
      blink::WebSandboxFlags::All & ~blink::WebSandboxFlags::Scripts &
      ~blink::WebSandboxFlags::AutomaticFeatures &
      ~blink::WebSandboxFlags::Popups;
  EXPECT_EQ(expected_flags, root->child_at(0)->pending_sandbox_flags());

  // Navigate child frame cross-site.  The sandbox flags should take effect.
  GURL frame_url(embedded_test_server()->GetURL("b.com", "/title1.html"));
  TestFrameNavigationObserver frame_observer(root->child_at(0));
  NavigateFrameToURL(root->child_at(0), frame_url);
  frame_observer.Wait();
  EXPECT_EQ(expected_flags, root->child_at(0)->effective_sandbox_flags());

  // Verify that they've also taken effect on the renderer side.  The sandboxed
  // frame's origin should be unique.
  EXPECT_EQ("null", GetDocumentOrigin(root->child_at(0)));

  // Open a popup named "foo" from the sandboxed child frame.
  Shell* foo_shell = OpenPopup(root->child_at(0)->current_frame_host(),
                               GURL(url::kAboutBlankURL), "foo");
  EXPECT_TRUE(foo_shell);

  FrameTreeNode* foo_root =
      static_cast<WebContentsImpl*>(foo_shell->web_contents())
          ->GetFrameTree()
          ->root();

  // Check that the sandbox flags for new popup are correct in the browser
  // process.
  EXPECT_EQ(expected_flags, foo_root->effective_sandbox_flags());

  // The popup's origin should be unique, since it's sandboxed.
  EXPECT_EQ("null", GetDocumentOrigin(foo_root));

  // Navigate the popup cross-site.  This should keep the unique origin and the
  // inherited sandbox flags.
  GURL c_url(embedded_test_server()->GetURL("c.com", "/title1.html"));
  TestFrameNavigationObserver popup_observer(foo_root);
  EXPECT_TRUE(ExecuteScript(foo_root->current_frame_host(),
                            "location.href = '" + c_url.spec() + "';"));
  popup_observer.Wait();
  EXPECT_EQ(c_url, foo_shell->web_contents()->GetLastCommittedURL());

  // Confirm that the popup is still sandboxed, both on browser and renderer
  // sides.
  EXPECT_EQ(expected_flags, foo_root->effective_sandbox_flags());
  EXPECT_EQ("null", GetDocumentOrigin(foo_root));
}

// Verify that popups opened from frames sandboxed with the
// "allow-popups-to-escape-sandbox" directive do *not* inherit sandbox flags
// from their opener.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest,
                       OpenUnsandboxedPopupFromSandboxedFrame) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(a)"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // It is safe to obtain the root frame tree node here, as it doesn't change.
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();

  // Set sandbox flags for child frame, specifying that popups opened from it
  // should not be sandboxed.
  EXPECT_TRUE(ExecuteScript(
      root->current_frame_host(),
      "document.querySelector('iframe').sandbox = "
      "    'allow-scripts allow-popups allow-popups-to-escape-sandbox';"));

  // Set expected flags for the child frame.  Note that "allow-scripts" resets
  // both WebSandboxFlags::Scripts and WebSandboxFlags::AutomaticFeatures bits
  // per blink::parseSandboxPolicy().
  blink::WebSandboxFlags expected_flags =
      blink::WebSandboxFlags::All & ~blink::WebSandboxFlags::Scripts &
      ~blink::WebSandboxFlags::AutomaticFeatures &
      ~blink::WebSandboxFlags::Popups &
      ~blink::WebSandboxFlags::PropagatesToAuxiliaryBrowsingContexts;
  EXPECT_EQ(expected_flags, root->child_at(0)->pending_sandbox_flags());

  // Navigate child frame cross-site.  The sandbox flags should take effect.
  GURL frame_url(embedded_test_server()->GetURL("b.com", "/title1.html"));
  TestFrameNavigationObserver frame_observer(root->child_at(0));
  NavigateFrameToURL(root->child_at(0), frame_url);
  frame_observer.Wait();
  EXPECT_EQ(expected_flags, root->child_at(0)->effective_sandbox_flags());

  // Open a cross-site popup named "foo" from the child frame.
  GURL b_url(embedded_test_server()->GetURL("c.com", "/title1.html"));
  Shell* foo_shell =
      OpenPopup(root->child_at(0)->current_frame_host(), b_url, "foo");
  EXPECT_TRUE(foo_shell);

  FrameTreeNode* foo_root =
      static_cast<WebContentsImpl*>(foo_shell->web_contents())
          ->GetFrameTree()
          ->root();

  // Check that the sandbox flags for new popup are correct in the browser
  // process.  They should not have been inherited.
  EXPECT_EQ(blink::WebSandboxFlags::None, foo_root->effective_sandbox_flags());

  // The popup's origin should match |b_url|, since it's not sandboxed.
  std::string popup_origin;
  EXPECT_TRUE(ExecuteScriptAndExtractString(
      foo_root->current_frame_host(),
      "domAutomationController.send(document.origin)",
      &popup_origin));
  EXPECT_EQ(b_url.GetOrigin().spec(), popup_origin + "/");
}

// Tests that the WebContents is notified when passive mixed content is
// displayed in an OOPIF. The test ignores cert errors so that an HTTPS
// iframe can be loaded from a site other than localhost (the
// EmbeddedTestServer serves a certificate that is valid for localhost).
IN_PROC_BROWSER_TEST_F(SitePerProcessIgnoreCertErrorsBrowserTest,
                       PassiveMixedContentInIframe) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(https_server.Start());
  SetupCrossSiteRedirector(&https_server);

  GURL iframe_url(
      https_server.GetURL("/mixed-content/basic-passive-in-iframe.html"));
  EXPECT_TRUE(NavigateToURL(shell(), iframe_url));
  EXPECT_TRUE(shell()->web_contents()->DisplayedInsecureContent());

  // When the subframe navigates, the WebContents should still be marked
  // as having displayed insecure content.
  GURL navigate_url(https_server.GetURL("/title1.html"));
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  NavigateFrameToURL(root->child_at(0), navigate_url);
  EXPECT_TRUE(shell()->web_contents()->DisplayedInsecureContent());

  // When the main frame navigates, it should no longer be marked as
  // displaying insecure content.
  EXPECT_TRUE(
      NavigateToURL(shell(), https_server.GetURL("b.com", "/title1.html")));
  EXPECT_FALSE(shell()->web_contents()->DisplayedInsecureContent());
}

// Tests that, when a parent frame is set to strictly block mixed
// content via Content Security Policy, child OOPIFs cannot display
// mixed content.
IN_PROC_BROWSER_TEST_F(SitePerProcessIgnoreCertErrorsBrowserTest,
                       PassiveMixedContentInIframeWithStrictBlocking) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(https_server.Start());
  SetupCrossSiteRedirector(&https_server);

  GURL iframe_url_with_strict_blocking(https_server.GetURL(
      "/mixed-content/basic-passive-in-iframe-with-strict-blocking.html"));
  EXPECT_TRUE(NavigateToURL(shell(), iframe_url_with_strict_blocking));
  EXPECT_FALSE(shell()->web_contents()->DisplayedInsecureContent());

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  EXPECT_TRUE(root->current_replication_state()
                  .should_enforce_strict_mixed_content_checking);
  EXPECT_TRUE(root->child_at(0)
                  ->current_replication_state()
                  .should_enforce_strict_mixed_content_checking);

  // When the subframe navigates, it should still be marked as enforcing
  // strict mixed content.
  GURL navigate_url(https_server.GetURL("/title1.html"));
  NavigateFrameToURL(root->child_at(0), navigate_url);
  EXPECT_TRUE(root->current_replication_state()
                  .should_enforce_strict_mixed_content_checking);
  EXPECT_TRUE(root->child_at(0)
                  ->current_replication_state()
                  .should_enforce_strict_mixed_content_checking);

  // When the main frame navigates, it should no longer be marked as
  // enforcing strict mixed content.
  EXPECT_TRUE(
      NavigateToURL(shell(), https_server.GetURL("b.com", "/title1.html")));
  EXPECT_FALSE(root->current_replication_state()
                   .should_enforce_strict_mixed_content_checking);
}

// Tests that active mixed content is blocked in an OOPIF. The test
// ignores cert errors so that an HTTPS iframe can be loaded from a site
// other than localhost (the EmbeddedTestServer serves a certificate
// that is valid for localhost).
IN_PROC_BROWSER_TEST_F(SitePerProcessIgnoreCertErrorsBrowserTest,
                       ActiveMixedContentInIframe) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(https_server.Start());
  SetupCrossSiteRedirector(&https_server);

  GURL iframe_url(
      https_server.GetURL("/mixed-content/basic-active-in-iframe.html"));
  EXPECT_TRUE(NavigateToURL(shell(), iframe_url));
  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  ASSERT_EQ(1U, root->child_count());
  FrameTreeNode* mixed_child = root->child_at(0)->child_at(0);
  ASSERT_TRUE(mixed_child);
  // The child iframe attempted to create a mixed iframe; this should
  // have been blocked, so the mixed iframe should not have committed a
  // load.
  EXPECT_FALSE(mixed_child->has_committed_real_load());
}

// Test setting a cross-origin iframe to display: none.
IN_PROC_BROWSER_TEST_F(SitePerProcessBrowserTest, CrossSiteIframeDisplayNone) {
  GURL main_url(embedded_test_server()->GetURL(
      "a.com", "/cross_site_iframe_factory.html?a(b)"));
  NavigateToURL(shell(), main_url);

  FrameTreeNode* root = static_cast<WebContentsImpl*>(shell()->web_contents())
                            ->GetFrameTree()
                            ->root();
  RenderWidgetHost* root_render_widget_host =
      root->current_frame_host()->GetRenderWidgetHost();

  // Set the iframe to display: none.
  EXPECT_TRUE(
      ExecuteScript(shell()->web_contents(),
                    "document.querySelector('iframe').style.display = 'none'"));

  // Waits until pending frames are done.
  scoped_ptr<MainThreadFrameObserver> observer(
      new MainThreadFrameObserver(root_render_widget_host));
  observer->Wait();

  // Force the renderer to generate a new frame.
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(),
                            "document.body.style.background = 'black'"));

  // Waits for the next frame.
  observer->Wait();
}

}  // namespace content
