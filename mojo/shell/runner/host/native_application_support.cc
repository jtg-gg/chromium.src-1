// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/shell/runner/host/native_application_support.h"

#include <stddef.h>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "mojo/platform_handle/platform_handle_private_thunks.h"
#include "mojo/public/platform/native/system_thunks.h"

#if defined(NATIVE_APPLICATION_USE_GLES2_IMPL)
#include "mojo/public/platform/native/gles2_impl_chromium_extension_thunks.h"
#include "mojo/public/platform/native/gles2_impl_thunks.h"
#include "mojo/public/platform/native/gles2_thunks.h"
#endif

namespace mojo {
namespace shell {

namespace {

template <typename Thunks>
bool SetThunks(Thunks (*make_thunks)(),
               const char* function_name,
               base::NativeLibrary library) {
  typedef size_t (*SetThunksFn)(const Thunks* thunks);
  SetThunksFn set_thunks = reinterpret_cast<SetThunksFn>(
      base::GetFunctionPointerFromNativeLibrary(library, function_name));
  if (!set_thunks)
    return false;
  Thunks thunks = make_thunks();
  size_t expected_size = set_thunks(&thunks);
  if (expected_size > sizeof(Thunks)) {
    LOG(ERROR) << "Invalid app library: expected " << function_name
               << " to return thunks of size: " << expected_size;
    return false;
  }
  return true;
}

}  // namespace

base::NativeLibrary LoadNativeApplication(const base::FilePath& app_path) {
  DVLOG(2) << "Loading Mojo app in process from library: " << app_path.value();

  base::NativeLibraryLoadError error;
  base::NativeLibrary app_library = base::LoadNativeLibrary(app_path, &error);
  LOG_IF(ERROR, !app_library)
      << "Failed to load app library (error: " << error.ToString() << ")";
  return app_library;
}

bool RunNativeApplication(
    base::NativeLibrary app_library,
    InterfaceRequest<mojom::ShellClient> request) {
  // Tolerate |app_library| being null, to make life easier for callers.
  if (!app_library)
    return false;

// Thunks aren't needed/used in component build, since the thunked methods
// just live in their own dynamically loaded library.
#if !defined(COMPONENT_BUILD)
  if (!SetThunks(&MojoMakeSystemThunks, "MojoSetSystemThunks", app_library)) {
    LOG(ERROR) << "MojoSetSystemThunks not found";
    return false;
  }

#if defined(NATIVE_APPLICATION_USE_GLES2_IMPL)
  if (SetThunks(&MojoMakeGLES2ControlThunks, "MojoSetGLES2ControlThunks",
                app_library)) {
    // If we have the control thunks, we should also have the GLES2
    // implementation thunks.
    if (!SetThunks(&MojoMakeGLES2ImplThunks, "MojoSetGLES2ImplThunks",
                   app_library)) {
      LOG(ERROR)
          << "MojoSetGLES2ControlThunks found, but not MojoSetGLES2ImplThunks";
      return false;
    }

    // If the application is using GLES2 extension points, register those
    // thunks. Applications may use or not use any of these, so don't warn if
    // they are missing.
    SetThunks(MojoMakeGLES2ImplChromiumExtensionThunks,
              "MojoSetGLES2ImplChromiumExtensionThunks", app_library);
  }
#endif

// Unlike system thunks, we don't warn on a lack of GLES2 thunks because
// not everything is a visual app.

#if !defined(OS_WIN)
  // On Windows, initializing base::CommandLine with null parameters gets the
  // process's command line from the OS. Other platforms need it to be passed
  // in. This needs to be passed in before the app initializes the command line,
  // which is done as soon as it loads.
  typedef void (*InitCommandLineArgs)(int, const char* const*);
  InitCommandLineArgs init_command_line_args =
      reinterpret_cast<InitCommandLineArgs>(
          base::GetFunctionPointerFromNativeLibrary(app_library,
                                                    "InitCommandLineArgs"));
  if (init_command_line_args) {
    int argc = 0;
    base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
    const char** argv = new const char*[cmd_line->argv().size()];
    for (auto& arg : cmd_line->argv())
      argv[argc++] = arg.c_str();
    init_command_line_args(argc, argv);
  }
#endif

  // Apps need not include platform handle thunks.
  SetThunks(&MojoMakePlatformHandlePrivateThunks,
            "MojoSetPlatformHandlePrivateThunks", app_library);
#endif

  typedef MojoResult (*MojoMainFunction)(MojoHandle);
  MojoMainFunction main_function = reinterpret_cast<MojoMainFunction>(
      base::GetFunctionPointerFromNativeLibrary(app_library, "MojoMain"));
  if (!main_function) {
    LOG(ERROR) << "MojoMain not found";
    return false;
  }
  // |MojoMain()| takes ownership of the service handle.
  MojoHandle handle = request.PassMessagePipe().release().value();
  MojoResult result = main_function(handle);
  if (result != MOJO_RESULT_OK) {
    LOG(ERROR) << "MojoMain returned error (result: " << result << ")";
  }
  return true;
}

}  // namespace shell
}  // namespace mojo
