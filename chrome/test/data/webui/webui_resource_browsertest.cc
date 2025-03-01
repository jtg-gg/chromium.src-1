// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "base/path_service.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/grit/options_test_resources.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "chrome/test/data/grit/webui_test_resources.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test_utils.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/resources/grit/webui_resources.h"

class WebUIResourceBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpOnMainThread() override {
    // Load resources that are only used by browser_tests.
    base::FilePath pak_path;
    ASSERT_TRUE(PathService::Get(base::DIR_MODULE, &pak_path));
    pak_path = pak_path.AppendASCII("browser_tests.pak");
    ResourceBundle::GetSharedInstance().AddDataPackFromPath(
        pak_path, ui::SCALE_FACTOR_NONE);
  }

  // Runs all test functions in |file|, waiting for them to complete.
  void LoadFile(const base::FilePath& file) {
    base::FilePath webui(FILE_PATH_LITERAL("webui"));
    RunTest(ui_test_utils::GetTestUrl(webui, file));
  }

  void LoadResource(int idr) {
    ResourceBundle& bundle = ResourceBundle::GetSharedInstance();
    base::StringPiece resource = bundle.GetRawDataResource(idr);
    RunTest(GURL(std::string("data:text/html,") + resource.as_string()));
  }

  // Queues the library corresponding to |resource_id| for injection into the
  // test. The code injection is performed post-load, so any common test
  // initialization that depends on the library should be placed in a setUp
  // function.
  void AddLibrary(int resource_id) {
    include_libraries_.push_back(resource_id);
  }

 private:
  void RunTest(const GURL& url) {
    ui_test_utils::NavigateToURL(browser(), url);
    content::WebContents* web_contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    ASSERT_TRUE(web_contents);
    EXPECT_TRUE(ExecuteWebUIResourceTest(web_contents, include_libraries_));
  }

  // Resource IDs for internal javascript libraries to inject into the test.
  std::vector<int> include_libraries_;
};

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, ArrayDataModelTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_EVENT_TARGET);
  AddLibrary(IDR_WEBUI_JS_CR_UI_ARRAY_DATA_MODEL);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("array_data_model_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, CrTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_EVENT_TARGET);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("cr_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, EventTargetTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_EVENT_TARGET);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("event_target_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, I18nProcessCssTest) {
  LoadResource(IDR_WEBUI_TEST_I18N_PROCESS_CSS_TEST);
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, I18nProcessTest) {
  AddLibrary(IDR_WEBUI_JS_LOAD_TIME_DATA);
  AddLibrary(IDR_WEBUI_JS_I18N_TEMPLATE_NO_PROCESS);
  AddLibrary(IDR_WEBUI_JS_UTIL);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("i18n_process_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, ListTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_EVENT_TARGET);
  AddLibrary(IDR_WEBUI_JS_CR_UI);
  AddLibrary(IDR_WEBUI_JS_CR_UI_ARRAY_DATA_MODEL);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST_ITEM);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST_SELECTION_CONTROLLER);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST_SELECTION_MODEL);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("list_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, GridTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_EVENT_TARGET);
  AddLibrary(IDR_WEBUI_JS_CR_UI);
  AddLibrary(IDR_WEBUI_JS_CR_UI_ARRAY_DATA_MODEL);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST_ITEM);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST_SELECTION_CONTROLLER);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST_SELECTION_MODEL);
  // Grid must be the last addition as it depends on list libraries.
  AddLibrary(IDR_WEBUI_JS_CR_UI_GRID);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("grid_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, LinkControllerTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_LINK_CONTROLLER);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("link_controller_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, ListSelectionModelTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_EVENT_TARGET);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST_SELECTION_MODEL);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("list_selection_model_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, ListSingleSelectionModelTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_EVENT_TARGET);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST_SINGLE_SELECTION_MODEL);
  LoadFile(base::FilePath(FILE_PATH_LITERAL(
      "list_single_selection_model_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, InlineEditableListTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_EVENT_TARGET);
  AddLibrary(IDR_WEBUI_JS_CR_UI);
  AddLibrary(IDR_WEBUI_JS_CR_UI_ARRAY_DATA_MODEL);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST_ITEM);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST_SELECTION_CONTROLLER);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST_SELECTION_MODEL);
  AddLibrary(IDR_WEBUI_JS_CR_UI_LIST);
  AddLibrary(IDR_WEBUI_JS_LOAD_TIME_DATA);
  AddLibrary(IDR_OPTIONS_DELETABLE_ITEM_LIST);
  AddLibrary(IDR_OPTIONS_INLINE_EDITABLE_LIST);
  LoadFile(base::FilePath(FILE_PATH_LITERAL(
      "inline_editable_list_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, MenuTest) {
  AddLibrary(IDR_WEBUI_JS_ASSERT);
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_UI);
  AddLibrary(IDR_WEBUI_JS_CR_UI_COMMAND);
  AddLibrary(IDR_WEBUI_JS_CR_UI_MENU_ITEM);
  AddLibrary(IDR_WEBUI_JS_CR_UI_MENU);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("menu_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, MockTimerTest) {
  LoadFile(base::FilePath(FILE_PATH_LITERAL("mock_timer_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, ParseHtmlSubsetTest) {
  AddLibrary(IDR_WEBUI_JS_PARSE_HTML_SUBSET);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("parse_html_subset_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, PositionUtilTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_UI_POSITION_UTIL);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("position_util_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, RepeatingButtonTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_UI);
  AddLibrary(IDR_WEBUI_JS_CR_UI_REPEATING_BUTTON);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("repeating_button_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, CommandTest) {
  AddLibrary(IDR_WEBUI_JS_ASSERT);
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_UI);
  AddLibrary(IDR_WEBUI_JS_CR_UI_COMMAND);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("command_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, ContextMenuHandlerTest) {
  AddLibrary(IDR_WEBUI_JS_ASSERT);
  AddLibrary(IDR_WEBUI_JS_EVENT_TRACKER);
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_EVENT_TARGET);
  AddLibrary(IDR_WEBUI_JS_CR_UI);
  AddLibrary(IDR_WEBUI_JS_CR_UI_POSITION_UTIL);
  AddLibrary(IDR_WEBUI_JS_CR_UI_MENU_ITEM);
  AddLibrary(IDR_WEBUI_JS_CR_UI_MENU_BUTTON);
  AddLibrary(IDR_WEBUI_JS_CR_UI_MENU);
  AddLibrary(IDR_WEBUI_JS_CR_UI_CONTEXT_MENU_HANDLER);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("context_menu_handler_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, MenuButtonTest) {
  AddLibrary(IDR_WEBUI_JS_ASSERT);
  AddLibrary(IDR_WEBUI_JS_EVENT_TRACKER);
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_UI);
  AddLibrary(IDR_WEBUI_JS_CR_UI_POSITION_UTIL);
  AddLibrary(IDR_WEBUI_JS_CR_UI_MENU_BUTTON);
  AddLibrary(IDR_WEBUI_JS_CR_UI_MENU);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("menu_button_test.html")));
}

IN_PROC_BROWSER_TEST_F(WebUIResourceBrowserTest, SplitterTest) {
  AddLibrary(IDR_WEBUI_JS_CR);
  AddLibrary(IDR_WEBUI_JS_CR_UI);
  AddLibrary(IDR_WEBUI_JS_CR_UI_SPLITTER);
  LoadFile(base::FilePath(FILE_PATH_LITERAL("splitter_test.html")));
}
