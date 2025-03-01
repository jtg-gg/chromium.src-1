// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOJO_SHELL_NATIVE_RUNNER_DELEGATE_H_
#define MOJO_SHELL_NATIVE_RUNNER_DELEGATE_H_

namespace base {
class CommandLine;
}

namespace mojo {
namespace shell {

class Identity;

class NativeRunnerDelegate {
 public:
  // Called to adjust the commandline for launching the specified app.
  // WARNING: this is called on a background thread.
  virtual void AdjustCommandLineArgumentsForTarget(
      const Identity& target,
      base::CommandLine* command_line) = 0;

 protected:
  virtual ~NativeRunnerDelegate() {}
};

}  // namespace shell
}  // namespace mojo

#endif  // MOJO_SHELL_NATIVE_RUNNER_DELEGATE_H_
