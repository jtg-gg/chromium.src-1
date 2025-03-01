// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/test/layouttest_support.h"

#include <stddef.h>
#include <utility>

#include "base/callback.h"
#include "base/lazy_instance.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "components/test_runner/test_common.h"
#include "components/test_runner/web_frame_test_proxy.h"
#include "components/test_runner/web_test_proxy.h"
#include "content/browser/bluetooth/bluetooth_dispatcher_host.h"
#include "content/browser/renderer_host/render_process_host_impl.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/child/geofencing/web_geofencing_provider_impl.h"
#include "content/common/gpu/image_transport_surface.h"
#include "content/common/site_isolation_policy.h"
#include "content/public/common/page_state.h"
#include "content/public/renderer/renderer_gamepad_provider.h"
#include "content/renderer/fetchers/manifest_fetcher.h"
#include "content/renderer/history_entry.h"
#include "content/renderer/history_serialization.h"
#include "content/renderer/render_frame_impl.h"
#include "content/renderer/render_thread_impl.h"
#include "content/renderer/render_view_impl.h"
#include "content/renderer/renderer_blink_platform_impl.h"
#include "content/shell/common/shell_switches.h"
#include "device/bluetooth/bluetooth_adapter.h"
#include "third_party/WebKit/public/platform/WebGamepads.h"
#include "third_party/WebKit/public/platform/modules/device_orientation/WebDeviceMotionData.h"
#include "third_party/WebKit/public/platform/modules/device_orientation/WebDeviceOrientationData.h"
#include "third_party/WebKit/public/web/WebHistoryItem.h"
#include "third_party/WebKit/public/web/WebView.h"

#if defined(OS_MACOSX)
#include "content/browser/frame_host/popup_menu_helper_mac.h"
#elif defined(OS_WIN)
#include "content/child/font_warmup_win.h"
#include "content/public/common/dwrite_font_platform_win.h"
#include "third_party/WebKit/public/web/win/WebFontRendering.h"
#include "third_party/skia/include/ports/SkFontMgr.h"
#include "third_party/skia/include/ports/SkTypeface_win.h"
#include "ui/gfx/win/direct_write.h"
#endif

using blink::WebDeviceMotionData;
using blink::WebDeviceOrientationData;
using blink::WebGamepad;
using blink::WebGamepads;
using blink::WebRect;
using blink::WebSize;

namespace content {

namespace {

base::LazyInstance<
    base::Callback<void(RenderView*, test_runner::WebTestProxyBase*)>>::Leaky
    g_callback = LAZY_INSTANCE_INITIALIZER;

RenderViewImpl* CreateWebTestProxy(CompositorDependencies* compositor_deps,
                                   const ViewMsg_New_Params& params) {
  typedef test_runner::WebTestProxy<RenderViewImpl, CompositorDependencies*,
                                    const ViewMsg_New_Params&> ProxyType;
  ProxyType* render_view_proxy = new ProxyType(compositor_deps, params);
  if (g_callback == 0)
    return render_view_proxy;
  g_callback.Get().Run(render_view_proxy, render_view_proxy);
  return render_view_proxy;
}

test_runner::WebTestProxyBase* GetWebTestProxyBase(
    RenderViewImpl* render_view) {
  typedef test_runner::WebTestProxy<RenderViewImpl, const ViewMsg_New_Params&>
      ViewProxy;

  ViewProxy* render_view_proxy = static_cast<ViewProxy*>(render_view);
  return static_cast<test_runner::WebTestProxyBase*>(render_view_proxy);
}

RenderFrameImpl* CreateWebFrameTestProxy(
    const RenderFrameImpl::CreateParams& params) {
  typedef test_runner::WebFrameTestProxy<
      RenderFrameImpl, const RenderFrameImpl::CreateParams&> FrameProxy;

  FrameProxy* render_frame_proxy = new FrameProxy(params);
  render_frame_proxy->set_base_proxy(GetWebTestProxyBase(params.render_view));

  return render_frame_proxy;
}

#if defined(OS_WIN)
// DirectWrite only has access to %WINDIR%\Fonts by default. For developer
// side-loading, support kRegisterFontFiles to allow access to additional fonts.
void RegisterSideloadedTypefaces(SkFontMgr* fontmgr) {
  RenderThreadImpl::current()->EnsureWebKitInitialized();
  std::vector<std::string> files = switches::GetSideloadFontFiles();
  for (std::vector<std::string>::const_iterator i(files.begin());
       i != files.end();
       ++i) {
    SkTypeface* typeface = fontmgr->createFromFile(i->c_str());
    if (!ShouldUseDirectWriteFontProxyFieldTrial())
      DoPreSandboxWarmupForTypeface(typeface);
    blink::WebFontRendering::addSideloadedFontForTesting(typeface);
  }
}
#endif  // OS_WIN

}  // namespace

void EnableWebTestProxyCreation(
    const base::Callback<void(RenderView*, test_runner::WebTestProxyBase*)>&
        callback) {
  g_callback.Get() = callback;
  RenderViewImpl::InstallCreateHook(CreateWebTestProxy);
  RenderFrameImpl::InstallCreateHook(CreateWebFrameTestProxy);
}

void FetchManifestDoneCallback(
    scoped_ptr<ManifestFetcher> fetcher,
    const FetchManifestCallback& callback,
    const blink::WebURLResponse& response,
    const std::string& data) {
  // |fetcher| will be autodeleted here as it is going out of scope.
  callback.Run(response, data);
}

void FetchManifest(blink::WebView* view, const GURL& url,
                   const FetchManifestCallback& callback) {
  ManifestFetcher* fetcher = new ManifestFetcher(url);
  scoped_ptr<ManifestFetcher> autodeleter(fetcher);

  // Start is called on fetcher which is also bound to the callback.
  // A raw pointer is used instead of a scoped_ptr as base::Passes passes
  // ownership and thus nulls the scoped_ptr. On MSVS this happens before
  // the call to Start, resulting in a crash.
  fetcher->Start(view->mainFrame(),
                 false,
                 base::Bind(&FetchManifestDoneCallback,
                            base::Passed(&autodeleter),
                            callback));
}

void SetMockGamepadProvider(scoped_ptr<RendererGamepadProvider> provider) {
  RenderThreadImpl::current()
      ->blink_platform_impl()
      ->SetPlatformEventObserverForTesting(blink::WebPlatformEventTypeGamepad,
                                           std::move(provider));
}

void SetMockDeviceLightData(const double data) {
  RendererBlinkPlatformImpl::SetMockDeviceLightDataForTesting(data);
}

void SetMockDeviceMotionData(const WebDeviceMotionData& data) {
  RendererBlinkPlatformImpl::SetMockDeviceMotionDataForTesting(data);
}

void SetMockDeviceOrientationData(const WebDeviceOrientationData& data) {
  RendererBlinkPlatformImpl::SetMockDeviceOrientationDataForTesting(data);
}

void EnableRendererLayoutTestMode() {
  RenderThreadImpl::current()->set_layout_test_mode(true);

#if defined(OS_WIN)
  if (gfx::win::ShouldUseDirectWrite()) {
    if (ShouldUseDirectWriteFontProxyFieldTrial())
      RegisterSideloadedTypefaces(SkFontMgr_New_DirectWrite());
    else
      RegisterSideloadedTypefaces(GetPreSandboxWarmupFontMgr());
  }
#endif
}

void EnableBrowserLayoutTestMode() {
#if defined(OS_MACOSX)
  ImageTransportSurface::SetAllowOSMesaForTesting(true);
  PopupMenuHelper::DontShowPopupMenuForTesting();
#endif
  RenderWidgetHostImpl::DisableResizeAckCheckForTesting();
}

int GetLocalSessionHistoryLength(RenderView* render_view) {
  return static_cast<RenderViewImpl*>(render_view)->
      GetLocalSessionHistoryLengthForTesting();
}

void SyncNavigationState(RenderView* render_view) {
  // TODO(creis): Add support for testing in OOPIF-enabled modes.
  // See https://crbug.com/477150.
  if (SiteIsolationPolicy::UseSubframeNavigationEntries())
    return;
  static_cast<RenderViewImpl*>(render_view)->SendUpdateState();
}

void SetFocusAndActivate(RenderView* render_view, bool enable) {
  static_cast<RenderViewImpl*>(render_view)->
      SetFocusAndActivateForTesting(enable);
}

void ForceResizeRenderView(RenderView* render_view,
                           const WebSize& new_size) {
  RenderViewImpl* render_view_impl = static_cast<RenderViewImpl*>(render_view);
  render_view_impl->ForceResizeForTesting(new_size);
}

void SetDeviceScaleFactor(RenderView* render_view, float factor) {
  static_cast<RenderViewImpl*>(render_view)->
      SetDeviceScaleFactorForTesting(factor);
}

void SetDeviceColorProfile(RenderView* render_view, const std::string& name) {
  if (name == "reset") {
    static_cast<RenderViewImpl*>(render_view)->
        ResetDeviceColorProfileForTesting();
    return;
  }

  std::vector<char> color_profile;

  struct TestColorProfile { // A whacked (aka color spin) profile.
    char* data() {
      static unsigned char color_profile_data[] = {
        0x00,0x00,0x01,0xea,0x54,0x45,0x53,0x54,0x00,0x00,0x00,0x00,
        0x6d,0x6e,0x74,0x72,0x52,0x47,0x42,0x20,0x58,0x59,0x5a,0x20,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x61,0x63,0x73,0x70,0x74,0x65,0x73,0x74,0x00,0x00,0x00,0x00,
        0x74,0x65,0x73,0x74,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf6,0xd6,
        0x00,0x01,0x00,0x00,0x00,0x00,0xd3,0x2d,0x74,0x65,0x73,0x74,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x09,
        0x63,0x70,0x72,0x74,0x00,0x00,0x00,0xf0,0x00,0x00,0x00,0x0d,
        0x64,0x65,0x73,0x63,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x8c,
        0x77,0x74,0x70,0x74,0x00,0x00,0x01,0x8c,0x00,0x00,0x00,0x14,
        0x72,0x58,0x59,0x5a,0x00,0x00,0x01,0xa0,0x00,0x00,0x00,0x14,
        0x67,0x58,0x59,0x5a,0x00,0x00,0x01,0xb4,0x00,0x00,0x00,0x14,
        0x62,0x58,0x59,0x5a,0x00,0x00,0x01,0xc8,0x00,0x00,0x00,0x14,
        0x72,0x54,0x52,0x43,0x00,0x00,0x01,0xdc,0x00,0x00,0x00,0x0e,
        0x67,0x54,0x52,0x43,0x00,0x00,0x01,0xdc,0x00,0x00,0x00,0x0e,
        0x62,0x54,0x52,0x43,0x00,0x00,0x01,0xdc,0x00,0x00,0x00,0x0e,
        0x74,0x65,0x78,0x74,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x64,0x65,0x73,0x63,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x10,0x77,0x68,0x61,0x63,0x6b,0x65,0x64,0x2e,
        0x69,0x63,0x63,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x58,0x59,0x5a,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0xf3,0x52,
        0x00,0x01,0x00,0x00,0x00,0x01,0x16,0xcc,0x58,0x59,0x5a,0x20,
        0x00,0x00,0x00,0x00,0x00,0x00,0x34,0x8d,0x00,0x00,0xa0,0x2c,
        0x00,0x00,0x0f,0x95,0x58,0x59,0x5a,0x20,0x00,0x00,0x00,0x00,
        0x00,0x00,0x26,0x31,0x00,0x00,0x10,0x2f,0x00,0x00,0xbe,0x9b,
        0x58,0x59,0x5a,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x9c,0x18,
        0x00,0x00,0x4f,0xa5,0x00,0x00,0x04,0xfc,0x63,0x75,0x72,0x76,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x33
      };

      return reinterpret_cast<char*>(color_profile_data);
    }

    size_t size() {
      const size_t kTestColorProfileSizeInBytes = 490u;
      return kTestColorProfileSizeInBytes;
    }
  };

  struct AdobeRGBColorProfile {
    char* data() {
      static unsigned char color_profile_data[] = {
        0x00,0x00,0x02,0x30,0x41,0x44,0x42,0x45,0x02,0x10,0x00,0x00,
        0x6d,0x6e,0x74,0x72,0x52,0x47,0x42,0x20,0x58,0x59,0x5a,0x20,
        0x07,0xd0,0x00,0x08,0x00,0x0b,0x00,0x13,0x00,0x33,0x00,0x3b,
        0x61,0x63,0x73,0x70,0x41,0x50,0x50,0x4c,0x00,0x00,0x00,0x00,
        0x6e,0x6f,0x6e,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf6,0xd6,
        0x00,0x01,0x00,0x00,0x00,0x00,0xd3,0x2d,0x41,0x44,0x42,0x45,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0a,
        0x63,0x70,0x72,0x74,0x00,0x00,0x00,0xfc,0x00,0x00,0x00,0x32,
        0x64,0x65,0x73,0x63,0x00,0x00,0x01,0x30,0x00,0x00,0x00,0x6b,
        0x77,0x74,0x70,0x74,0x00,0x00,0x01,0x9c,0x00,0x00,0x00,0x14,
        0x62,0x6b,0x70,0x74,0x00,0x00,0x01,0xb0,0x00,0x00,0x00,0x14,
        0x72,0x54,0x52,0x43,0x00,0x00,0x01,0xc4,0x00,0x00,0x00,0x0e,
        0x67,0x54,0x52,0x43,0x00,0x00,0x01,0xd4,0x00,0x00,0x00,0x0e,
        0x62,0x54,0x52,0x43,0x00,0x00,0x01,0xe4,0x00,0x00,0x00,0x0e,
        0x72,0x58,0x59,0x5a,0x00,0x00,0x01,0xf4,0x00,0x00,0x00,0x14,
        0x67,0x58,0x59,0x5a,0x00,0x00,0x02,0x08,0x00,0x00,0x00,0x14,
        0x62,0x58,0x59,0x5a,0x00,0x00,0x02,0x1c,0x00,0x00,0x00,0x14,
        0x74,0x65,0x78,0x74,0x00,0x00,0x00,0x00,0x43,0x6f,0x70,0x79,
        0x72,0x69,0x67,0x68,0x74,0x20,0x32,0x30,0x30,0x30,0x20,0x41,
        0x64,0x6f,0x62,0x65,0x20,0x53,0x79,0x73,0x74,0x65,0x6d,0x73,
        0x20,0x49,0x6e,0x63,0x6f,0x72,0x70,0x6f,0x72,0x61,0x74,0x65,
        0x64,0x00,0x00,0x00,0x64,0x65,0x73,0x63,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x11,0x41,0x64,0x6f,0x62,0x65,0x20,0x52,0x47,
        0x42,0x20,0x28,0x31,0x39,0x39,0x38,0x29,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x58,0x59,0x5a,0x20,0x00,0x00,0x00,0x00,
        0x00,0x00,0xf3,0x51,0x00,0x01,0x00,0x00,0x00,0x01,0x16,0xcc,
        0x58,0x59,0x5a,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x63,0x75,0x72,0x76,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x33,0x00,0x00,
        0x63,0x75,0x72,0x76,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,
        0x02,0x33,0x00,0x00,0x63,0x75,0x72,0x76,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x01,0x02,0x33,0x00,0x00,0x58,0x59,0x5a,0x20,
        0x00,0x00,0x00,0x00,0x00,0x00,0x9c,0x18,0x00,0x00,0x4f,0xa5,
        0x00,0x00,0x04,0xfc,0x58,0x59,0x5a,0x20,0x00,0x00,0x00,0x00,
        0x00,0x00,0x34,0x8d,0x00,0x00,0xa0,0x2c,0x00,0x00,0x0f,0x95,
        0x58,0x59,0x5a,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x26,0x31,
        0x00,0x00,0x10,0x2f,0x00,0x00,0xbe,0x9c
      };

      return reinterpret_cast<char*>(color_profile_data);
    }

    size_t size() {
      const size_t kAdobeRGBColorProfileSizeInBytes = 560u;
      return kAdobeRGBColorProfileSizeInBytes;
    }
  };

  if (name == "sRGB") {
    color_profile.assign(name.data(), name.data() + name.size());
  } else if (name == "test" || name == "whacked") {
    TestColorProfile test;
    color_profile.assign(test.data(), test.data() + test.size());
  } else if (name == "adobeRGB") {
    AdobeRGBColorProfile test;
    color_profile.assign(test.data(), test.data() + test.size());
  }

  static_cast<RenderViewImpl*>(render_view)->
      SetDeviceColorProfileForTesting(color_profile);
}

void SetBluetoothAdapter(int render_process_id,
                         scoped_refptr<device::BluetoothAdapter> adapter) {
  RenderProcessHostImpl* render_process_host_impl =
      static_cast<RenderProcessHostImpl*>(
          RenderProcessHost::FromID(render_process_id));

  BluetoothDispatcherHost* dispatcher_host =
      render_process_host_impl->GetBluetoothDispatcherHost();

  if (dispatcher_host != NULL)
    dispatcher_host->SetBluetoothAdapterForTesting(std::move(adapter));
}

void SetGeofencingMockProvider(bool service_available) {
  static_cast<WebGeofencingProviderImpl*>(
      RenderThreadImpl::current()->blink_platform_impl()->geofencingProvider())
          ->SetMockProvider(service_available);
}

void ClearGeofencingMockProvider() {
  static_cast<WebGeofencingProviderImpl*>(
      RenderThreadImpl::current()->blink_platform_impl()->geofencingProvider())
          ->ClearMockProvider();
}

void SetGeofencingMockPosition(double latitude, double longitude) {
  static_cast<WebGeofencingProviderImpl*>(
      RenderThreadImpl::current()->blink_platform_impl()->geofencingProvider())
          ->SetMockPosition(latitude, longitude);
}

void UseSynchronousResizeMode(RenderView* render_view, bool enable) {
  static_cast<RenderViewImpl*>(render_view)->
      UseSynchronousResizeModeForTesting(enable);
}

void EnableAutoResizeMode(RenderView* render_view,
                          const WebSize& min_size,
                          const WebSize& max_size) {
  static_cast<RenderViewImpl*>(render_view)->
      EnableAutoResizeForTesting(min_size, max_size);
}

void DisableAutoResizeMode(RenderView* render_view, const WebSize& new_size) {
  static_cast<RenderViewImpl*>(render_view)->
      DisableAutoResizeForTesting(new_size);
}

// Returns True if node1 < node2.
bool HistoryEntryCompareLess(HistoryEntry::HistoryNode* node1,
                             HistoryEntry::HistoryNode* node2) {
  base::string16 target1 = node1->item().target();
  base::string16 target2 = node2->item().target();
  return base::CompareCaseInsensitiveASCII(target1, target2) < 0;
}

std::string DumpHistoryItem(HistoryEntry::HistoryNode* node,
                            int indent,
                            bool is_current_index) {
  std::string result;

  const blink::WebHistoryItem& item = node->item();
  if (is_current_index) {
    result.append("curr->");
    result.append(indent - 6, ' '); // 6 == "curr->".length()
  } else {
    result.append(indent, ' ');
  }

  std::string url =
      test_runner::NormalizeLayoutTestURL(item.urlString().utf8());
  result.append(url);
  if (!item.target().isEmpty()) {
    result.append(" (in frame \"");
    result.append(item.target().utf8());
    result.append("\")");
  }
  result.append("\n");

  std::vector<HistoryEntry::HistoryNode*> children = node->children();
  if (!children.empty()) {
    std::sort(children.begin(), children.end(), HistoryEntryCompareLess);
    for (size_t i = 0; i < children.size(); ++i)
      result += DumpHistoryItem(children[i], indent + 4, false);
  }

  return result;
}

std::string DumpBackForwardList(std::vector<PageState>& page_state,
                                size_t current_index) {
  std::string result;
  result.append("\n============== Back Forward List ==============\n");
  for (size_t index = 0; index < page_state.size(); ++index) {
    scoped_ptr<HistoryEntry> entry(
        PageStateToHistoryEntry(page_state[index]));
    result.append(
        DumpHistoryItem(entry->root_history_node(),
                        8,
                        index == current_index));
  }
  result.append("===============================================\n");
  return result;
}

}  // namespace content
