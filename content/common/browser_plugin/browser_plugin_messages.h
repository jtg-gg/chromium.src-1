// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Multiply-included message header, no traditional include guard.

#include <string>

#include "base/process/process.h"
#include "cc/surfaces/surface.h"
#include "content/common/content_export.h"
#include "content/common/content_param_traits.h"
#include "content/common/cursors/webcursor.h"
#include "content/common/edit_command.h"
#include "content/public/common/common_param_traits.h"
#include "content/public/common/drop_data.h"
#include "ipc/ipc_channel_handle.h"
#include "ipc/ipc_message_macros.h"
#include "ipc/ipc_message_utils.h"
#include "third_party/WebKit/public/platform/WebFocusType.h"
#include "third_party/WebKit/public/web/WebCompositionUnderline.h"
#include "third_party/WebKit/public/web/WebDragOperation.h"
#include "third_party/WebKit/public/web/WebDragStatus.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/ipc/gfx_param_traits.h"
#include "url/gurl.h"

#undef IPC_MESSAGE_EXPORT
#define IPC_MESSAGE_EXPORT CONTENT_EXPORT

#define IPC_MESSAGE_START BrowserPluginMsgStart


IPC_ENUM_TRAITS_MAX_VALUE(blink::WebDragStatus, blink::WebDragStatusLast)

IPC_STRUCT_BEGIN(BrowserPluginHostMsg_Attach_Params)
  IPC_STRUCT_MEMBER(bool, focused)
  IPC_STRUCT_MEMBER(bool, visible)
  // The new size of the guest view.
  IPC_STRUCT_MEMBER(gfx::Rect, view_rect)
  // Whether the browser plugin is a full page plugin document.
  IPC_STRUCT_MEMBER(bool, is_full_page_plugin)
IPC_STRUCT_END()

// Browser plugin messages

// -----------------------------------------------------------------------------
// These messages are from the embedder to the browser process.
// Most messages from the embedder to the browser process are CONTROL because
// they are routed to the appropriate BrowserPluginGuest based on the
// browser_plugin_instance_id which is unique per embedder process.

// This message is sent from BrowserPlugin to BrowserPluginGuest to issue an
// edit command.
IPC_MESSAGE_CONTROL2(BrowserPluginHostMsg_ExecuteEditCommand,
                     int /* browser_plugin_instance_id */,
                     std::string /* command */)

// This message must be sent just before sending a key event.
IPC_MESSAGE_CONTROL2(BrowserPluginHostMsg_SetEditCommandsForNextKeyEvent,
                     int /* browser_plugin_instance_id */,
                     std::vector<content::EditCommand> /* edit_commands */)

// This message is sent from BrowserPlugin to BrowserPluginGuest whenever IME
// composition state is updated.
IPC_MESSAGE_CONTROL5(
    BrowserPluginHostMsg_ImeSetComposition,
    int /* browser_plugin_instance_id */,
    std::string /* text */,
    std::vector<blink::WebCompositionUnderline> /* underlines */,
    int /* selectiont_start */,
    int /* selection_end */)

// This message is sent from BrowserPlugin to BrowserPluginGuest to notify that
// confirming the current composition is requested.
IPC_MESSAGE_CONTROL3(BrowserPluginHostMsg_ImeConfirmComposition,
                     int /* browser_plugin_instance_id */,
                     std::string /* text */,
                     bool /* keep selection */)

// Deletes the current selection plus the specified number of characters before
// and after the selection or caret.
IPC_MESSAGE_CONTROL3(BrowserPluginHostMsg_ExtendSelectionAndDelete,
                     int /* browser_plugin_instance_id */,
                     int /* before */,
                     int /* after */)

// This message is sent to the browser process to indicate that the
// BrowserPlugin identified by |browser_plugin_instance_id| is ready to serve
// as container for a guest. |params| is the state of the BrowserPlugin.
// This message is routed because we create a BrowserPluginEmbedder object
// the first time we see this message arrive to a WebContents. We need a routing
// ID to get this message to a particular WebContents.
IPC_MESSAGE_ROUTED2(BrowserPluginHostMsg_Attach,
                    int /* browser_plugin_instance_id */,
                    BrowserPluginHostMsg_Attach_Params /* params */)

// This message is sent to the browser process to indicate that the
// BrowserPlugin identified by |browser_plugin_instance_id| will no longer serve
// as a container for a guest.
IPC_MESSAGE_CONTROL1(BrowserPluginHostMsg_Detach,
                     int /* browser_plugin_instance_id */)

// Tells the guest to focus or defocus itself.
IPC_MESSAGE_CONTROL3(BrowserPluginHostMsg_SetFocus,
                     int /* browser_plugin_instance_id */,
                     bool /* enable */,
                     blink::WebFocusType /* focus_type */)

// Sends an input event to the guest.
IPC_MESSAGE_CONTROL2(BrowserPluginHostMsg_HandleInputEvent,
                     int /* browser_plugin_instance_id */,
                     IPC::WebInputEventPointer /* event */)

// Tells the guest it has been shown or hidden.
IPC_MESSAGE_CONTROL2(BrowserPluginHostMsg_SetVisibility,
                     int /* browser_plugin_instance_id */,
                     bool /* visible */)

// Tells the guest that a drag event happened on the plugin.
IPC_MESSAGE_CONTROL5(BrowserPluginHostMsg_DragStatusUpdate,
                     int /* browser_plugin_instance_id */,
                     blink::WebDragStatus /* drag_status */,
                     content::DropData /* drop_data */,
                     blink::WebDragOperationsMask /* operation_mask */,
                     gfx::Point /* plugin_location */)

// Sends a PointerLock Lock ACK to the BrowserPluginGuest.
IPC_MESSAGE_CONTROL2(BrowserPluginHostMsg_LockMouse_ACK,
                     int /* browser_plugin_instance_id */,
                     bool /* succeeded */)

// Sends a PointerLock Unlock ACK to the BrowserPluginGuest.
IPC_MESSAGE_CONTROL1(BrowserPluginHostMsg_UnlockMouse_ACK,
                     int /* browser_plugin_instance_id */)

// Sent when plugin's position has changed.
IPC_MESSAGE_CONTROL2(BrowserPluginHostMsg_UpdateGeometry,
                     int /* browser_plugin_instance_id */,
                     gfx::Rect /* view_rect */)

IPC_MESSAGE_ROUTED2(BrowserPluginHostMsg_SatisfySequence,
                    int /* browser_plugin_instance_id */,
                    cc::SurfaceSequence /* sequence */)

IPC_MESSAGE_ROUTED3(BrowserPluginHostMsg_RequireSequence,
                    int /* browser_plugin_instance_id */,
                    cc::SurfaceId /* surface_id */,
                    cc::SurfaceSequence /* sequence */)

// -----------------------------------------------------------------------------
// These messages are from the browser process to the embedder.

// When the guest crashes, the browser process informs the embedder through this
// message.
IPC_MESSAGE_CONTROL1(BrowserPluginMsg_GuestGone,
                     int /* browser_plugin_instance_id */)

// When the user tabs to the end of the tab stops of a guest, the browser
// process informs the embedder to tab out of the browser plugin.
IPC_MESSAGE_CONTROL2(BrowserPluginMsg_AdvanceFocus,
                     int /* browser_plugin_instance_id */,
                     bool /* reverse */)

// When the guest starts/stops listening to touch events, it needs to notify the
// plugin in the embedder about it.
IPC_MESSAGE_CONTROL2(BrowserPluginMsg_ShouldAcceptTouchEvents,
                     int /* browser_plugin_instance_id */,
                     bool /* accept */)

// Inform the embedder of the cursor the guest wishes to display.
IPC_MESSAGE_CONTROL2(BrowserPluginMsg_SetCursor,
                     int /* browser_plugin_instance_id */,
                     content::WebCursor /* cursor */)

IPC_MESSAGE_CONTROL5(BrowserPluginMsg_SetChildFrameSurface,
                     int /* browser_plugin_instance_id */,
                     cc::SurfaceId /* surface_id */,
                     gfx::Size /* frame_size */,
                     float /* scale_factor */,
                     cc::SurfaceSequence /* sequence */)

// Forwards a PointerLock Unlock request to the BrowserPlugin.
IPC_MESSAGE_CONTROL2(BrowserPluginMsg_SetMouseLock,
                     int /* browser_plugin_instance_id */,
                     bool /* enable */)

// Sends text to be displayed in tooltip.
IPC_MESSAGE_CONTROL2(BrowserPluginMsg_SetTooltipText,
                     int /* browser_plugin_instance_id */,
                     base::string16 /* tooltip_text */)
