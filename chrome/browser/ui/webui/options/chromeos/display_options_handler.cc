// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/options/chromeos/display_options_handler.h"

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "ash/display/display_configuration_controller.h"
#include "ash/display/display_layout_builder.h"
#include "ash/display/display_manager.h"
#include "ash/display/resolution_notification_controller.h"
#include "ash/display/window_tree_host_manager.h"
#include "ash/screen_util.h"
#include "ash/shell.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "chrome/browser/chromeos/display/display_preferences.h"
#include "chrome/grit/generated_resources.h"
#include "chromeos/chromeos_switches.h"
#include "content/public/browser/user_metrics.h"
#include "content/public/browser/web_ui.h"
#include "grit/ash_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/display.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/screen.h"

namespace chromeos {
namespace options {
namespace {

ash::DisplayManager* GetDisplayManager() {
  return ash::Shell::GetInstance()->display_manager();
}

ash::DisplayConfigurationController* GetDisplayConfigurationController() {
  return ash::Shell::GetInstance()->display_configuration_controller();
}

int64_t GetDisplayId(const base::ListValue* args) {
  // Assumes the display ID is specified as the first argument.
  std::string id_value;
  if (!args->GetString(0, &id_value)) {
    LOG(ERROR) << "Can't find ID";
    return gfx::Display::kInvalidDisplayID;
  }

  int64_t display_id = gfx::Display::kInvalidDisplayID;
  if (!base::StringToInt64(id_value, &display_id)) {
    LOG(ERROR) << "Invalid display id: " << id_value;
    return gfx::Display::kInvalidDisplayID;
  }

  return display_id;
}

base::string16 GetColorProfileName(ui::ColorCalibrationProfile profile) {
  switch (profile) {
    case ui::COLOR_PROFILE_STANDARD:
      return l10n_util::GetStringUTF16(
          IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_COLOR_PROFILE_STANDARD);
    case ui::COLOR_PROFILE_DYNAMIC:
      return l10n_util::GetStringUTF16(
          IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_COLOR_PROFILE_DYNAMIC);
    case ui::COLOR_PROFILE_MOVIE:
      return l10n_util::GetStringUTF16(
          IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_COLOR_PROFILE_MOVIE);
    case ui::COLOR_PROFILE_READING:
      return l10n_util::GetStringUTF16(
          IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_COLOR_PROFILE_READING);
    case ui::NUM_COLOR_PROFILES:
      break;
  }

  NOTREACHED();
  return base::string16();
}

int GetIntOrDouble(const base::DictionaryValue* dict,
                   const std::string& field) {
  double double_result = 0;
  if (dict->GetDouble(field, &double_result))
    return static_cast<int>(double_result);

  int result = 0;
  dict->GetInteger(field, &result);
  return result;
}

bool GetFloat(const base::DictionaryValue* dict,
              const std::string& field,
              float* result) {
  double double_result = 0;
  if (dict->GetDouble(field, &double_result)) {
    *result = static_cast<float>(double_result);
    return true;
  }
  return false;
}

bool ConvertValueToDisplayMode(const base::DictionaryValue* dict,
                               ash::DisplayMode* mode) {
  mode->size.set_width(GetIntOrDouble(dict, "originalWidth"));
  mode->size.set_height(GetIntOrDouble(dict, "originalHeight"));
  if (mode->size.IsEmpty()) {
    LOG(ERROR) << "missing width or height.";
    return false;
  }
  if (!GetFloat(dict, "refreshRate", &mode->refresh_rate)) {
    LOG(ERROR) << "missing refreshRate.";
    return false;
  }
  if (!GetFloat(dict, "scale", &mode->ui_scale)) {
    LOG(ERROR) << "missing ui-scale.";
    return false;
  }
  if (!GetFloat(dict, "deviceScaleFactor", &mode->device_scale_factor)) {
    LOG(ERROR) << "missing deviceScaleFactor.";
    return false;
  }
  return true;
}

base::DictionaryValue* ConvertDisplayModeToValue(int64_t display_id,
                                                 const ash::DisplayMode& mode) {
  bool is_internal = gfx::Display::HasInternalDisplay() &&
                     gfx::Display::InternalDisplayId() == display_id;
  base::DictionaryValue* result = new base::DictionaryValue();
  gfx::Size size_dip = mode.GetSizeInDIP(is_internal);
  result->SetInteger("width", size_dip.width());
  result->SetInteger("height", size_dip.height());
  result->SetInteger("originalWidth", mode.size.width());
  result->SetInteger("originalHeight", mode.size.height());
  result->SetDouble("deviceScaleFactor", mode.device_scale_factor);
  result->SetDouble("scale", mode.ui_scale);
  result->SetDouble("refreshRate", mode.refresh_rate);
  result->SetBoolean(
      "isBest", is_internal ? (mode.ui_scale == 1.0f) : mode.native);
  result->SetBoolean("isNative", mode.native);
  result->SetBoolean(
      "selected", mode.IsEquivalent(
          GetDisplayManager()->GetActiveModeForDisplayId(display_id)));
  return result;
}

base::DictionaryValue* ConvertBoundsToValue(const gfx::Rect& bounds) {
  base::DictionaryValue* result = new base::DictionaryValue();
  result->SetInteger("left", bounds.x());
  result->SetInteger("top", bounds.y());
  result->SetInteger("width", bounds.width());
  result->SetInteger("height", bounds.height());
  return result;
}

}  // namespace

DisplayOptionsHandler::DisplayOptionsHandler() {
  // ash::Shell doesn't exist in Athena.
  // See: http://crbug.com/416961
  ash::Shell::GetInstance()->window_tree_host_manager()->AddObserver(this);
}

DisplayOptionsHandler::~DisplayOptionsHandler() {
  // ash::Shell doesn't exist in Athena.
  ash::Shell::GetInstance()->window_tree_host_manager()->RemoveObserver(this);
}

void DisplayOptionsHandler::GetLocalizedValues(
    base::DictionaryValue* localized_strings) {
  DCHECK(localized_strings);
  RegisterTitle(localized_strings, "displayOptionsPage",
                IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_TAB_TITLE);

  localized_strings->SetString(
      "selectedDisplayTitleOptions", l10n_util::GetStringUTF16(
          IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_OPTIONS));
  localized_strings->SetString(
      "selectedDisplayTitleResolution", l10n_util::GetStringUTF16(
          IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_RESOLUTION));
  localized_strings->SetString(
      "selectedDisplayTitleOrientation", l10n_util::GetStringUTF16(
          IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_ORIENTATION));
  localized_strings->SetString(
      "selectedDisplayTitleOverscan", l10n_util::GetStringUTF16(
          IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_OVERSCAN));

  localized_strings->SetString("extendedMode", l10n_util::GetStringUTF16(
      IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_EXTENDED_MODE_LABEL));
  localized_strings->SetString("mirroringMode", l10n_util::GetStringUTF16(
      IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_MIRRORING_MODE_LABEL));
  localized_strings->SetString("mirroringDisplay", l10n_util::GetStringUTF16(
      IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_MIRRORING_DISPLAY_NAME));
  localized_strings->SetString("setPrimary", l10n_util::GetStringUTF16(
      IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_SET_PRIMARY));
  localized_strings->SetString("annotateBest", l10n_util::GetStringUTF16(
      IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_RESOLUTION_ANNOTATION_BEST));
  localized_strings->SetString("annotateNative", l10n_util::GetStringUTF16(
      IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_RESOLUTION_ANNOTATION_NATIVE));
  localized_strings->SetString("orientation0", l10n_util::GetStringUTF16(
      IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_STANDARD_ORIENTATION));
  localized_strings->SetString("orientation90", l10n_util::GetStringUTF16(
      IDS_ASH_STATUS_TRAY_DISPLAY_ORIENTATION_90));
  localized_strings->SetString("orientation180", l10n_util::GetStringUTF16(
      IDS_ASH_STATUS_TRAY_DISPLAY_ORIENTATION_180));
  localized_strings->SetString("orientation270", l10n_util::GetStringUTF16(
      IDS_ASH_STATUS_TRAY_DISPLAY_ORIENTATION_270));
  localized_strings->SetString(
      "startCalibratingOverscan", l10n_util::GetStringUTF16(
          IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_START_CALIBRATING_OVERSCAN));
  localized_strings->SetString(
      "selectedDisplayColorProfile", l10n_util::GetStringUTF16(
          IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_COLOR_PROFILE));
  localized_strings->SetString(
      "enableUnifiedDesktop",
      l10n_util::GetStringUTF16(
          IDS_OPTIONS_SETTINGS_DISPLAY_OPTIONS_ENABLE_UNIFIED_DESKTOP));
}

void DisplayOptionsHandler::InitializePage() {
  DCHECK(web_ui());
  UpdateDisplaySettingsEnabled();
}

void DisplayOptionsHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "getDisplayInfo",
      base::Bind(&DisplayOptionsHandler::HandleDisplayInfo,
                 base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setMirroring",
      base::Bind(&DisplayOptionsHandler::HandleMirroring,
                 base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setPrimary",
      base::Bind(&DisplayOptionsHandler::HandleSetPrimary,
                 base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setDisplayLayout",
      base::Bind(&DisplayOptionsHandler::HandleSetDisplayLayout,
                 base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setDisplayMode",
      base::Bind(&DisplayOptionsHandler::HandleSetDisplayMode,
                 base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setRotation",
      base::Bind(&DisplayOptionsHandler::HandleSetRotation,
                 base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setColorProfile",
      base::Bind(&DisplayOptionsHandler::HandleSetColorProfile,
                 base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setUnifiedDesktopEnabled",
      base::Bind(&DisplayOptionsHandler::HandleSetUnifiedDesktopEnabled,
                 base::Unretained(this)));
}

void DisplayOptionsHandler::OnDisplayConfigurationChanging() {
}

void DisplayOptionsHandler::OnDisplayConfigurationChanged() {
  UpdateDisplaySettingsEnabled();
  SendAllDisplayInfo();
}

void DisplayOptionsHandler::SendAllDisplayInfo() {
  ash::DisplayManager* display_manager = GetDisplayManager();

  std::vector<gfx::Display> displays;
  for (size_t i = 0; i < display_manager->GetNumDisplays(); ++i)
    displays.push_back(display_manager->GetDisplayAt(i));
  SendDisplayInfo(displays);
}

void DisplayOptionsHandler::SendDisplayInfo(
    const std::vector<gfx::Display>& displays) {
  ash::DisplayManager* display_manager = GetDisplayManager();
  ash::DisplayManager::MultiDisplayMode display_mode;
  if (display_manager->IsInMirrorMode())
    display_mode = ash::DisplayManager::MIRRORING;
  else if (display_manager->IsInUnifiedMode())
    display_mode = ash::DisplayManager::UNIFIED;
  else
    display_mode = ash::DisplayManager::EXTENDED;
  base::FundamentalValue mode(static_cast<int>(display_mode));
  int64_t primary_id = gfx::Screen::GetScreen()->GetPrimaryDisplay().id();
  base::ListValue js_displays;
  for (const gfx::Display& display : displays) {
    const ash::DisplayInfo& display_info =
        display_manager->GetDisplayInfo(display.id());
    base::DictionaryValue* js_display = new base::DictionaryValue();
    js_display->SetString("id", base::Int64ToString(display.id()));
    js_display->SetString("name",
                          display_manager->GetDisplayNameForId(display.id()));
    base::DictionaryValue* display_bounds =
        ConvertBoundsToValue(display.bounds());
    js_display->Set("bounds", display_bounds);
    js_display->SetBoolean("isPrimary", display.id() == primary_id);
    js_display->SetBoolean("isInternal", display.IsInternal());
    js_display->SetInteger("rotation", display.RotationAsDegree());

    base::ListValue* js_resolutions = new base::ListValue();
    for (const ash::DisplayMode& display_mode : display_info.display_modes()) {
      js_resolutions->Append(
          ConvertDisplayModeToValue(display.id(), display_mode));
    }
    js_display->Set("resolutions", js_resolutions);

    js_display->SetInteger("colorProfileId", display_info.color_profile());
    base::ListValue* available_color_profiles = new base::ListValue();
    for (const auto& color_profile : display_info.available_color_profiles()) {
      const base::string16 profile_name = GetColorProfileName(color_profile);
      if (profile_name.empty())
        continue;
      base::DictionaryValue* color_profile_dict = new base::DictionaryValue();
      color_profile_dict->SetInteger("profileId", color_profile);
      color_profile_dict->SetString("name", profile_name);
      available_color_profiles->Append(color_profile_dict);
    }
    js_display->Set("availableColorProfiles", available_color_profiles);
    js_displays.Append(js_display);
  }

  scoped_ptr<base::Value> layout_value = base::Value::CreateNullValue();
  scoped_ptr<base::Value> offset_value = base::Value::CreateNullValue();
  if (display_manager->GetNumDisplays() > 1) {
    const ash::DisplayPlacement* placement =
        display_manager->GetCurrentDisplayLayout().placement_list[0];
    layout_value.reset(new base::FundamentalValue(placement->position));
    offset_value.reset(new base::FundamentalValue(placement->offset));
  }

  web_ui()->CallJavascriptFunction(
      "options.DisplayOptions.setDisplayInfo",
      mode, js_displays, *layout_value.get(), *offset_value.get());
}

void DisplayOptionsHandler::UpdateDisplaySettingsEnabled() {
  bool enabled = GetDisplayManager()->num_connected_displays() <= 2;
  bool show_unified_desktop = GetDisplayManager()->unified_desktop_enabled();
  bool multi_display_layout = base::CommandLine::ForCurrentProcess()->HasSwitch(
      chromeos::switches::kEnableMultiDisplayLayout);

  web_ui()->CallJavascriptFunction(
      "options.BrowserOptions.enableDisplaySettings",
      base::FundamentalValue(enabled),
      base::FundamentalValue(show_unified_desktop),
      base::FundamentalValue(multi_display_layout));
}

void DisplayOptionsHandler::HandleDisplayInfo(
    const base::ListValue* unused_args) {
  SendAllDisplayInfo();
}

void DisplayOptionsHandler::HandleMirroring(const base::ListValue* args) {
  DCHECK(!args->empty());
  bool is_mirroring = false;
  if (!args->GetBoolean(0, &is_mirroring))
    NOTREACHED();
  content::RecordAction(
      base::UserMetricsAction("Options_DisplayToggleMirroring"));
  GetDisplayConfigurationController()->SetMirrorMode(is_mirroring,
                                                     true /* user_action */);
}

void DisplayOptionsHandler::HandleSetPrimary(const base::ListValue* args) {
  DCHECK(!args->empty());
  int64_t display_id = GetDisplayId(args);
  if (display_id == gfx::Display::kInvalidDisplayID)
    return;

  content::RecordAction(base::UserMetricsAction("Options_DisplaySetPrimary"));
  GetDisplayConfigurationController()->SetPrimaryDisplayId(
      display_id, true /* user_action */);
}

void DisplayOptionsHandler::HandleSetDisplayLayout(
    const base::ListValue* args) {
  int position, offset;
  if (!args->GetInteger(1, &position))
    NOTREACHED();
  DCHECK_LE(ash::DisplayPlacement::TOP, position);
  DCHECK_GE(ash::DisplayPlacement::LEFT, position);
  if (!args->GetInteger(2, &offset))
    NOTREACHED();
  content::RecordAction(base::UserMetricsAction("Options_DisplayRearrange"));

  ash::DisplayLayoutBuilder builder(
      gfx::Screen::GetScreen()->GetPrimaryDisplay().id());
  builder.SetSecondaryPlacement(
      ash::ScreenUtil::GetSecondaryDisplay().id(),
      static_cast<ash::DisplayPlacement::Position>(position), offset);

  GetDisplayConfigurationController()->SetDisplayLayout(builder.Build(),
                                                        true /* user_action */);
}

void DisplayOptionsHandler::HandleSetDisplayMode(const base::ListValue* args) {
  DCHECK(!args->empty());

  int64_t display_id = GetDisplayId(args);
  if (display_id == gfx::Display::kInvalidDisplayID)
    return;

  const base::DictionaryValue* mode_data = nullptr;
  if (!args->GetDictionary(1, &mode_data)) {
    LOG(ERROR) << "Failed to get mode data";
    return;
  }

  ash::DisplayMode mode;
  if (!ConvertValueToDisplayMode(mode_data, &mode))
    return;

  content::RecordAction(
      base::UserMetricsAction("Options_DisplaySetResolution"));
  ash::DisplayManager* display_manager = GetDisplayManager();
  ash::DisplayMode current_mode =
      display_manager->GetActiveModeForDisplayId(display_id);
  if (!display_manager->SetDisplayMode(display_id, mode)) {
    LOG(ERROR) << "Unable to set display mode for: " << display_id
               << " Mode: " << *mode_data;
    return;
  }
  if (gfx::Display::IsInternalDisplayId(display_id))
    return;
  // For external displays, show a notification confirming the resolution
  // change.
  ash::Shell::GetInstance()
      ->resolution_notification_controller()
      ->PrepareNotification(display_id, current_mode, mode,
                            base::Bind(&chromeos::StoreDisplayPrefs));
}

void DisplayOptionsHandler::HandleSetRotation(const base::ListValue* args) {
  DCHECK(!args->empty());

  int64_t display_id = GetDisplayId(args);
  if (display_id == gfx::Display::kInvalidDisplayID)
    return;

  int rotation_value = 0;
  if (!args->GetInteger(1, &rotation_value)) {
    LOG(ERROR) << "Can't parse rotation: " << args;
    return;
  }
  gfx::Display::Rotation new_rotation = gfx::Display::ROTATE_0;
  if (rotation_value == 90)
    new_rotation = gfx::Display::ROTATE_90;
  else if (rotation_value == 180)
    new_rotation = gfx::Display::ROTATE_180;
  else if (rotation_value == 270)
    new_rotation = gfx::Display::ROTATE_270;
  else if (rotation_value != 0)
    LOG(ERROR) << "Invalid rotation: " << rotation_value << " Falls back to 0";

  content::RecordAction(
      base::UserMetricsAction("Options_DisplaySetOrientation"));
  GetDisplayConfigurationController()->SetDisplayRotation(
      display_id, new_rotation, gfx::Display::ROTATION_SOURCE_USER,
      true /* user_action */);
}

void DisplayOptionsHandler::HandleSetColorProfile(const base::ListValue* args) {
  DCHECK(!args->empty());
  int64_t display_id = GetDisplayId(args);
  if (display_id == gfx::Display::kInvalidDisplayID)
    return;

  std::string profile_value;
  if (!args->GetString(1, &profile_value)) {
    LOG(ERROR) << "Invalid profile_value";
    return;
  }

  int profile_id;
  if (!base::StringToInt(profile_value, &profile_id)) {
    LOG(ERROR) << "Invalid profile: " << profile_value;
    return;
  }

  if (profile_id < ui::COLOR_PROFILE_STANDARD ||
      profile_id > ui::COLOR_PROFILE_READING) {
    LOG(ERROR) << "Invalid profile_id: " << profile_id;
    return;
  }

  content::RecordAction(
      base::UserMetricsAction("Options_DisplaySetColorProfile"));
  GetDisplayManager()->SetColorCalibrationProfile(
      display_id, static_cast<ui::ColorCalibrationProfile>(profile_id));

  SendAllDisplayInfo();
}

void DisplayOptionsHandler::HandleSetUnifiedDesktopEnabled(
    const base::ListValue* args) {
  DCHECK(GetDisplayManager()->unified_desktop_enabled());
  bool enable = false;
  if (!args->GetBoolean(0, &enable))
    NOTREACHED();

  GetDisplayManager()->SetDefaultMultiDisplayModeForCurrentDisplays(
      enable ? ash::DisplayManager::UNIFIED : ash::DisplayManager::EXTENDED);
}

}  // namespace options
}  // namespace chromeos
