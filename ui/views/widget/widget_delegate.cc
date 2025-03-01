// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/views/widget/widget_delegate.h"

#include "base/strings/utf_string_conversions.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/bubble/bubble_delegate.h"
#include "ui/views/view.h"
#include "ui/views/views_delegate.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/client_view.h"

namespace views {

////////////////////////////////////////////////////////////////////////////////
// WidgetDelegate:

WidgetDelegate::WidgetDelegate()
    : default_contents_view_(NULL),
      can_activate_(true) {
}

void WidgetDelegate::OnWidgetMove() {
}

void WidgetDelegate::OnDisplayChanged() {
}

void WidgetDelegate::OnWorkAreaChanged() {
}

View* WidgetDelegate::GetInitiallyFocusedView() {
  return NULL;
}

BubbleDelegateView* WidgetDelegate::AsBubbleDelegate() {
  return NULL;
}

DialogDelegate* WidgetDelegate::AsDialogDelegate() {
  return NULL;
}

bool WidgetDelegate::CanResize() const {
  return false;
}

bool WidgetDelegate::CanMaximize() const {
  return false;
}

bool WidgetDelegate::CanMinimize() const {
  return false;
}

bool WidgetDelegate::CanActivate() const {
  return can_activate_;
}

bool WidgetDelegate::NWCanClose(bool user_force) {
  return true;
}

ui::ModalType WidgetDelegate::GetModalType() const {
  return ui::MODAL_TYPE_NONE;
}

ui::AXRole WidgetDelegate::GetAccessibleWindowRole() const {
  return ui::AX_ROLE_WINDOW;
}

base::string16 WidgetDelegate::GetAccessibleWindowTitle() const {
  return GetWindowTitle();
}

base::string16 WidgetDelegate::GetWindowTitle() const {
  return base::string16();
}

bool WidgetDelegate::ShouldShowWindowTitle() const {
  return true;
}

bool WidgetDelegate::ShouldShowCloseButton() const {
  return true;
}

bool WidgetDelegate::ShouldHandleSystemCommands() const {
  const Widget* widget = GetWidget();
  if (!widget)
    return false;

  return widget->non_client_view() != NULL;
}

bool WidgetDelegate::ShouldHandleOnSize() const {
  return false;
}

gfx::ImageSkia WidgetDelegate::GetWindowAppIcon() {
  // Use the window icon as app icon by default.
  return GetWindowIcon();
}

// Returns the icon to be displayed in the window.
gfx::ImageSkia WidgetDelegate::GetWindowIcon() {
  return gfx::ImageSkia();
}

bool WidgetDelegate::ShouldShowWindowIcon() const {
  return false;
}

bool WidgetDelegate::ExecuteWindowsCommand(int command_id) {
  return false;
}

bool WidgetDelegate::ExecuteAppCommand(int command_id) {
  return ExecuteWindowsCommand(command_id);
}

bool WidgetDelegate::HandleSize(unsigned int param, const gfx::Size& size) {
  return false;
}

std::string WidgetDelegate::GetWindowName() const {
  return std::string();
}

void WidgetDelegate::SaveWindowPlacement(const gfx::Rect& bounds,
                                         ui::WindowShowState show_state) {
  std::string window_name = GetWindowName();
  if (!ViewsDelegate::GetInstance() || window_name.empty())
    return;

  ViewsDelegate::GetInstance()->SaveWindowPlacement(GetWidget(), window_name,
                                                    bounds, show_state);
}

bool WidgetDelegate::GetSavedWindowPlacement(
    const Widget* widget,
    gfx::Rect* bounds,
    ui::WindowShowState* show_state) const {
  std::string window_name = GetWindowName();
  if (!ViewsDelegate::GetInstance() || window_name.empty())
    return false;

  return ViewsDelegate::GetInstance()->GetSavedWindowPlacement(
      widget, window_name, bounds, show_state);
}

bool WidgetDelegate::ShouldRestoreWindowSize() const {
  return true;
}

View* WidgetDelegate::GetContentsView() {
  if (!default_contents_view_)
    default_contents_view_ = new View;
  return default_contents_view_;
}

ClientView* WidgetDelegate::CreateClientView(Widget* widget) {
  return new ClientView(widget, GetContentsView());
}

NonClientFrameView* WidgetDelegate::CreateNonClientFrameView(Widget* widget) {
  return NULL;
}

View* WidgetDelegate::CreateOverlayView() {
  return NULL;
}

bool WidgetDelegate::WillProcessWorkAreaChange() const {
  return false;
}

bool WidgetDelegate::WidgetHasHitTestMask() const {
  return false;
}

void WidgetDelegate::GetWidgetHitTestMask(gfx::Path* mask) const {
  DCHECK(mask);
}

bool WidgetDelegate::ShouldAdvanceFocusToTopLevelWidget() const {
  return false;
}

bool WidgetDelegate::ShouldDescendIntoChildForEventHandling(
    gfx::NativeView child,
    const gfx::Point& location) {
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// WidgetDelegateView:

// static
const char WidgetDelegateView::kViewClassName[] = "WidgetDelegateView";

WidgetDelegateView::WidgetDelegateView() {
  // A WidgetDelegate should be deleted on DeleteDelegate.
  set_owned_by_client();
}

WidgetDelegateView::~WidgetDelegateView() {
}

void WidgetDelegateView::DeleteDelegate() {
  delete this;
}

Widget* WidgetDelegateView::GetWidget() {
  return View::GetWidget();
}

const Widget* WidgetDelegateView::GetWidget() const {
  return View::GetWidget();
}

const char* WidgetDelegateView::GetClassName() const {
  return kViewClassName;
}

}  // namespace views
