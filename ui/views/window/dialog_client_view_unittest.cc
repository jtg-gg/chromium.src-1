// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/macros.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "ui/base/ui_base_types.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/test/test_views.h"
#include "ui/views/test/views_test_base.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_client_view.h"
#include "ui/views/window/dialog_delegate.h"

namespace views {

class TestDialogClientView : public DialogClientView {
 public:
  TestDialogClientView(View* contents_view,
                       DialogDelegate* dialog_delegate)
      : DialogClientView(contents_view),
        dialog_(dialog_delegate) {}
  ~TestDialogClientView() override {}

  // DialogClientView implementation.
  DialogDelegate* GetDialogDelegate() const override { return dialog_; }

  View* GetContentsView() { return contents_view(); }

  void CreateExtraViews() {
    CreateExtraView();
    CreateFootnoteView();
  }

 private:
  DialogDelegate* dialog_;

  DISALLOW_COPY_AND_ASSIGN(TestDialogClientView);
};

class DialogClientViewTest : public ViewsTestBase,
                             public DialogDelegateView {
 public:
  DialogClientViewTest()
      : dialog_buttons_(ui::DIALOG_BUTTON_NONE),
        extra_view_(NULL),
        footnote_view_(NULL) {}
  ~DialogClientViewTest() override {}

  // testing::Test implementation.
  void SetUp() override {
    dialog_buttons_ = ui::DIALOG_BUTTON_NONE;
    contents_.reset(new StaticSizedView(gfx::Size(100, 200)));
    client_view_.reset(new TestDialogClientView(contents_.get(), this));

    ViewsTestBase::SetUp();
  }

  // DialogDelegateView implementation.
  View* GetContentsView() override { return contents_.get(); }
  View* CreateExtraView() override { return extra_view_; }
  bool GetExtraViewPadding(int* padding) override {
    if (extra_view_padding_)
      *padding = *extra_view_padding_;
    return extra_view_padding_.get() != nullptr;
  }
  View* CreateFootnoteView() override { return footnote_view_; }
  int GetDialogButtons() const override { return dialog_buttons_; }

 protected:
  gfx::Rect GetUpdatedClientBounds() {
    client_view_->SizeToPreferredSize();
    client_view_->Layout();
    return client_view_->bounds();
  }

  // Makes sure that the content view is sized correctly. Width must be at least
  // the requested amount, but height should always match exactly.
  void CheckContentsIsSetToPreferredSize() {
    const gfx::Rect client_bounds = GetUpdatedClientBounds();
    const gfx::Size preferred_size = contents_->GetPreferredSize();
    EXPECT_EQ(preferred_size.height(), contents_->bounds().height());
    EXPECT_LE(preferred_size.width(), contents_->bounds().width());
    EXPECT_EQ(contents_->bounds().origin(), client_bounds.origin());
    EXPECT_EQ(contents_->bounds().right(), client_bounds.right());
  }

  // Sets the buttons to show in the dialog and refreshes the dialog.
  void SetDialogButtons(int dialog_buttons) {
    dialog_buttons_ = dialog_buttons;
    client_view_->UpdateDialogButtons();
  }

  // Sets the extra view.
  void SetExtraView(View* view) {
    DCHECK(!extra_view_);
    extra_view_ = view;
    client_view_->CreateExtraViews();
  }

  // Sets the extra view padding.
  void SetExtraViewPadding(int padding) {
    DCHECK(!extra_view_padding_);
    extra_view_padding_.reset(new int(padding));
    client_view_->Layout();
  }

  // Sets the footnote view.
  void SetFootnoteView(View* view) {
    DCHECK(!footnote_view_);
    footnote_view_ = view;
    client_view_->CreateExtraViews();
  }

  TestDialogClientView* client_view() { return client_view_.get(); }

 private:
  // The contents of the dialog.
  scoped_ptr<View> contents_;
  // The DialogClientView that's being tested.
  scoped_ptr<TestDialogClientView> client_view_;
  // The bitmask of buttons to show in the dialog.
  int dialog_buttons_;
  View* extra_view_;  // weak
  scoped_ptr<int> extra_view_padding_;  // Null by default.
  View* footnote_view_;  // weak

  DISALLOW_COPY_AND_ASSIGN(DialogClientViewTest);
};

TEST_F(DialogClientViewTest, UpdateButtons) {
  // This dialog should start with no buttons.
  EXPECT_EQ(GetDialogButtons(), ui::DIALOG_BUTTON_NONE);
  EXPECT_EQ(NULL, client_view()->ok_button());
  EXPECT_EQ(NULL, client_view()->cancel_button());
  const int height_without_buttons = GetUpdatedClientBounds().height();

  // Update to use both buttons.
  SetDialogButtons(ui::DIALOG_BUTTON_OK | ui::DIALOG_BUTTON_CANCEL);
  EXPECT_TRUE(client_view()->ok_button()->is_default());
  EXPECT_FALSE(client_view()->cancel_button()->is_default());
  const int height_with_buttons = GetUpdatedClientBounds().height();
  EXPECT_GT(height_with_buttons, height_without_buttons);

  // Remove the dialog buttons.
  SetDialogButtons(ui::DIALOG_BUTTON_NONE);
  EXPECT_EQ(NULL, client_view()->ok_button());
  EXPECT_EQ(NULL, client_view()->cancel_button());
  EXPECT_EQ(GetUpdatedClientBounds().height(), height_without_buttons);

  // Reset with just an ok button.
  SetDialogButtons(ui::DIALOG_BUTTON_OK);
  EXPECT_TRUE(client_view()->ok_button()->is_default());
  EXPECT_EQ(NULL, client_view()->cancel_button());
  EXPECT_EQ(GetUpdatedClientBounds().height(), height_with_buttons);

  // Reset with just a cancel button.
  SetDialogButtons(ui::DIALOG_BUTTON_CANCEL);
  EXPECT_EQ(NULL, client_view()->ok_button());
  EXPECT_TRUE(client_view()->cancel_button()->is_default());
  EXPECT_EQ(GetUpdatedClientBounds().height(), height_with_buttons);
}

TEST_F(DialogClientViewTest, RemoveAndUpdateButtons) {
  // Removing buttons from another context should clear the local pointer.
  SetDialogButtons(ui::DIALOG_BUTTON_OK | ui::DIALOG_BUTTON_CANCEL);
  delete client_view()->ok_button();
  EXPECT_EQ(NULL, client_view()->ok_button());
  delete client_view()->cancel_button();
  EXPECT_EQ(NULL, client_view()->cancel_button());

  // Updating should restore the requested buttons properly.
  SetDialogButtons(ui::DIALOG_BUTTON_OK | ui::DIALOG_BUTTON_CANCEL);
  EXPECT_TRUE(client_view()->ok_button()->is_default());
  EXPECT_FALSE(client_view()->cancel_button()->is_default());
}

// Test that views inside the dialog client view have the correct focus order.
TEST_F(DialogClientViewTest, SetupFocusChain) {
#if defined(OS_WIN) || defined(OS_CHROMEOS)
  const bool kIsOkButtonOnLeftSide = true;
#else
  const bool kIsOkButtonOnLeftSide = false;
#endif

  // Initially the dialog client view only contains the content view.
  EXPECT_EQ(nullptr, client_view()->GetContentsView()->GetNextFocusableView());

  // Add OK and cancel buttons.
  SetDialogButtons(ui::DIALOG_BUTTON_OK | ui::DIALOG_BUTTON_CANCEL);

  if (kIsOkButtonOnLeftSide) {
    EXPECT_EQ(client_view()->ok_button(),
              client_view()->GetContentsView()->GetNextFocusableView());
    EXPECT_EQ(client_view()->cancel_button(),
              client_view()->ok_button()->GetNextFocusableView());
    EXPECT_EQ(nullptr, client_view()->cancel_button()->GetNextFocusableView());
  } else {
    EXPECT_EQ(client_view()->cancel_button(),
              client_view()->GetContentsView()->GetNextFocusableView());
    EXPECT_EQ(client_view()->ok_button(),
              client_view()->cancel_button()->GetNextFocusableView());
    EXPECT_EQ(nullptr, client_view()->ok_button()->GetNextFocusableView());
  }

  // Add extra view and remove OK button.
  View* extra_view = new StaticSizedView(gfx::Size(200, 200));
  SetExtraView(extra_view);
  SetDialogButtons(ui::DIALOG_BUTTON_CANCEL);

  EXPECT_EQ(extra_view,
            client_view()->GetContentsView()->GetNextFocusableView());
  EXPECT_EQ(client_view()->cancel_button(), extra_view->GetNextFocusableView());
  EXPECT_EQ(nullptr, client_view()->cancel_button()->GetNextFocusableView());
}

// Test that the contents view gets its preferred size in the basic dialog
// configuration.
TEST_F(DialogClientViewTest, ContentsSize) {
  CheckContentsIsSetToPreferredSize();
  EXPECT_EQ(GetContentsView()->bounds().bottom(),
            client_view()->bounds().bottom());
}

// Test the effect of the button strip on layout.
TEST_F(DialogClientViewTest, LayoutWithButtons) {
  SetDialogButtons(ui::DIALOG_BUTTON_OK | ui::DIALOG_BUTTON_CANCEL);
  CheckContentsIsSetToPreferredSize();
  EXPECT_LT(GetContentsView()->bounds().bottom(),
            client_view()->bounds().bottom());
  gfx::Size no_extra_view_size = client_view()->bounds().size();

  View* extra_view = new StaticSizedView(gfx::Size(200, 200));
  SetExtraView(extra_view);
  CheckContentsIsSetToPreferredSize();
  EXPECT_GT(client_view()->bounds().height(), no_extra_view_size.height());
  int width_of_dialog = client_view()->bounds().width();
  int width_of_extra_view = extra_view->bounds().width();

  // Try with an adjusted padding for the extra view.
  SetExtraViewPadding(250);
  CheckContentsIsSetToPreferredSize();
  EXPECT_GT(client_view()->bounds().width(), width_of_dialog);

  // Visibility of extra view is respected.
  extra_view->SetVisible(false);
  CheckContentsIsSetToPreferredSize();
  EXPECT_EQ(no_extra_view_size.height(), client_view()->bounds().height());
  EXPECT_EQ(no_extra_view_size.width(), client_view()->bounds().width());

  // Try with a reduced-size dialog.
  extra_view->SetVisible(true);
  client_view()->SetBoundsRect(gfx::Rect(gfx::Point(0, 0), no_extra_view_size));
  client_view()->Layout();
  EXPECT_GT(width_of_extra_view, extra_view->bounds().width());
}

// Test the effect of the footnote view on layout.
TEST_F(DialogClientViewTest, LayoutWithFootnote) {
  CheckContentsIsSetToPreferredSize();
  gfx::Size no_footnote_size = client_view()->bounds().size();

  View* footnote_view = new StaticSizedView(gfx::Size(200, 200));
  SetFootnoteView(footnote_view);
  CheckContentsIsSetToPreferredSize();
  EXPECT_GT(client_view()->bounds().height(), no_footnote_size.height());
  EXPECT_EQ(200, footnote_view->bounds().height());
  gfx::Size with_footnote_size = client_view()->bounds().size();
  EXPECT_EQ(with_footnote_size.width(), footnote_view->bounds().width());

  SetDialogButtons(ui::DIALOG_BUTTON_CANCEL);
  CheckContentsIsSetToPreferredSize();
  EXPECT_LE(with_footnote_size.height(), client_view()->bounds().height());
  EXPECT_LE(with_footnote_size.width(), client_view()->bounds().width());
  gfx::Size with_footnote_and_button_size = client_view()->bounds().size();

  SetDialogButtons(ui::DIALOG_BUTTON_NONE);
  footnote_view->SetVisible(false);
  CheckContentsIsSetToPreferredSize();
  EXPECT_EQ(no_footnote_size.height(), client_view()->bounds().height());
  EXPECT_EQ(no_footnote_size.width(), client_view()->bounds().width());
}

// Test that GetHeightForWidth is respected for the footnote view.
TEST_F(DialogClientViewTest, LayoutWithFootnoteHeightForWidth) {
  CheckContentsIsSetToPreferredSize();
  gfx::Size no_footnote_size = client_view()->bounds().size();

  View* footnote_view = new ProportionallySizedView(3);
  SetFootnoteView(footnote_view);
  CheckContentsIsSetToPreferredSize();
  EXPECT_GT(client_view()->bounds().height(), no_footnote_size.height());
  EXPECT_EQ(footnote_view->bounds().width() * 3,
            footnote_view->bounds().height());
}

}  // namespace views
