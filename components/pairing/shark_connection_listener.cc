// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/pairing/shark_connection_listener.h"

#include <utility>

#include "base/logging.h"
#include "base/threading/thread_restrictions.h"
#include "components/pairing/bluetooth_host_pairing_controller.h"

namespace pairing_chromeos {

SharkConnectionListener::SharkConnectionListener(
    const scoped_refptr<base::SingleThreadTaskRunner>& file_task_runner,
    OnConnectedCallback callback)
    : callback_(callback) {
  controller_.reset(new BluetoothHostPairingController(file_task_runner));
  controller_->AddObserver(this);
  controller_->StartPairing();
}

SharkConnectionListener::~SharkConnectionListener() {
  if (controller_)
    controller_->RemoveObserver(this);
}

void SharkConnectionListener::PairingStageChanged(Stage new_stage) {
  if (new_stage == HostPairingController::STAGE_WAITING_FOR_CODE_CONFIRMATION
      // Code confirmation stage can be skipped if devices were paired before.
      || new_stage == HostPairingController::STAGE_SETUP_BASIC_CONFIGURATION) {
    controller_->RemoveObserver(this);
    callback_.Run(std::move(controller_));
    callback_.Reset();
  } else if (new_stage != HostPairingController::STAGE_WAITING_FOR_CONTROLLER) {
    LOG(ERROR) << "Unexpected stage " << new_stage;
  }
}

}  // namespace pairing_chromeos
