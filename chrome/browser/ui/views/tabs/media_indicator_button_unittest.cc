// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/tabs/media_indicator_button.h"

#include "chrome/browser/ui/views/tabs/fake_base_tab_strip_controller.h"
#include "chrome/browser/ui/views/tabs/tab.h"
#include "chrome/browser/ui/views/tabs/tab_strip.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/views/test/views_test_base.h"

class MediaIndicatorButtonTest : public views::ViewsTestBase {
 public:
  MediaIndicatorButtonTest()
      : controller_(nullptr),
        tab_strip_(nullptr) {
  }

  ~MediaIndicatorButtonTest() override {}

  void SetUp() override {
    views::ViewsTestBase::SetUp();

    controller_ = new FakeBaseTabStripController;
    tab_strip_ = new TabStrip(controller_);
    controller_->set_tab_strip(tab_strip_);
    // The tab strip must be added to the view hierarchy for it to create the
    // buttons.
    parent_.AddChildView(tab_strip_);
    parent_.set_owned_by_client();

    widget_.reset(new views::Widget);
    views::Widget::InitParams init_params =
        CreateParams(views::Widget::InitParams::TYPE_POPUP);
    init_params.ownership =
        views::Widget::InitParams::WIDGET_OWNS_NATIVE_WIDGET;
    init_params.bounds = gfx::Rect(0, 0, 400, 400);
    widget_->Init(init_params);
    widget_->SetContentsView(&parent_);
  }

  void TearDown() override {
    // All windows need to be closed before tear down.
    widget_.reset();

    views::ViewsTestBase::TearDown();
  }

 protected:
  bool showing_close_button(Tab* tab) const {
    return tab->showing_close_button_;
  }
  bool showing_icon(Tab* tab) const { return tab->showing_icon_; }
  bool showing_media_indicator(Tab* tab) const {
    return tab->showing_media_indicator_;
  }

  void StopAnimation(Tab* tab) {
    ASSERT_TRUE(tab->media_indicator_button_->fade_animation_);
    tab->media_indicator_button_->fade_animation_->Stop();
  }

  // Owned by TabStrip.
  FakeBaseTabStripController* controller_;
  // Owns |tab_strip_|.
  views::View parent_;
  TabStrip* tab_strip_;
  scoped_ptr<views::Widget> widget_;

 private:
  DISALLOW_COPY_AND_ASSIGN(MediaIndicatorButtonTest);
};

// This test verifies that the tab has its icon state updated when the media
// animation fade-out finishes.
TEST_F(MediaIndicatorButtonTest, ButtonUpdateOnAudioStateAnimation) {
  controller_->AddPinnedTab(0, false);
  controller_->AddTab(1, true);
  Tab* media_tab = tab_strip_->tab_at(0);

  // Pinned inactive tab only has an icon.
  EXPECT_TRUE(showing_icon(media_tab));
  EXPECT_FALSE(showing_media_indicator(media_tab));
  EXPECT_FALSE(showing_close_button(media_tab));

  TabRendererData start_media;
  start_media.media_state = TAB_MEDIA_STATE_AUDIO_PLAYING;
  media_tab->SetData(start_media);

  // When audio starts, pinned inactive tab shows indicator.
  EXPECT_FALSE(showing_icon(media_tab));
  EXPECT_TRUE(showing_media_indicator(media_tab));
  EXPECT_FALSE(showing_close_button(media_tab));

  TabRendererData stop_media;
  stop_media.media_state = TAB_MEDIA_STATE_NONE;
  media_tab->SetData(stop_media);

  // When audio ends, pinned inactive tab fades out indicator.
  EXPECT_FALSE(showing_icon(media_tab));
  EXPECT_TRUE(showing_media_indicator(media_tab));
  EXPECT_FALSE(showing_close_button(media_tab));

  // Rather than flakily waiting some unknown number of seconds for the fade
  // out animation to stop, reach out and stop the fade animation directly,
  // to make sure that it updates the tab appropriately when it's done.
  StopAnimation(media_tab);

  EXPECT_TRUE(showing_icon(media_tab));
  EXPECT_FALSE(showing_media_indicator(media_tab));
  EXPECT_FALSE(showing_close_button(media_tab));
}
