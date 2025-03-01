// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/bluetooth/bluetooth_remote_gatt_service_win.h"

#include "base/bind.h"
#include "device/bluetooth/bluetooth_adapter_win.h"
#include "device/bluetooth/bluetooth_device_win.h"
#include "device/bluetooth/bluetooth_remote_gatt_characteristic_win.h"
#include "device/bluetooth/bluetooth_task_manager_win.h"

namespace device {

BluetoothRemoteGattServiceWin::BluetoothRemoteGattServiceWin(
    BluetoothDeviceWin* device,
    base::FilePath service_path,
    BluetoothUUID service_uuid,
    uint16_t service_attribute_handle,
    bool is_primary,
    BluetoothRemoteGattServiceWin* parent_service,
    scoped_refptr<base::SequencedTaskRunner>& ui_task_runner)
    : device_(device),
      service_path_(service_path),
      service_uuid_(service_uuid),
      service_attribute_handle_(service_attribute_handle),
      is_primary_(is_primary),
      parent_service_(parent_service),
      ui_task_runner_(ui_task_runner),
      discovery_complete_notified_(false),
      included_characteristics_discovered_(false),
      weak_ptr_factory_(this) {
  DCHECK(ui_task_runner_->RunsTasksOnCurrentThread());
  DCHECK(!service_path_.empty());
  DCHECK(service_uuid_.IsValid());
  DCHECK(service_attribute_handle_);
  DCHECK(device_);
  if (!is_primary_)
    DCHECK(parent_service_);

  adapter_ = static_cast<BluetoothAdapterWin*>(device_->GetAdapter());
  DCHECK(adapter_);
  task_manager_ = adapter_->GetWinBluetoothTaskManager();
  DCHECK(task_manager_);
  service_identifier_ = device_->GetIdentifier() + "/" + service_uuid_.value() +
                        "_" + std::to_string(service_attribute_handle_);
  Update();
}

BluetoothRemoteGattServiceWin::~BluetoothRemoteGattServiceWin() {
  DCHECK(ui_task_runner_->RunsTasksOnCurrentThread());

  adapter_->NotifyGattServiceRemoved(this);
}

std::string BluetoothRemoteGattServiceWin::GetIdentifier() const {
  return service_identifier_;
}

BluetoothUUID BluetoothRemoteGattServiceWin::GetUUID() const {
  return const_cast<BluetoothUUID&>(service_uuid_);
}

bool BluetoothRemoteGattServiceWin::IsLocal() const {
  return false;
}

bool BluetoothRemoteGattServiceWin::IsPrimary() const {
  return is_primary_;
}

BluetoothDevice* BluetoothRemoteGattServiceWin::GetDevice() const {
  return device_;
}

std::vector<BluetoothGattCharacteristic*>
BluetoothRemoteGattServiceWin::GetCharacteristics() const {
  std::vector<BluetoothGattCharacteristic*> has_characteristics;
  for (const auto& c : included_characteristics_)
    has_characteristics.push_back(c.second.get());
  return has_characteristics;
}

std::vector<BluetoothGattService*>
BluetoothRemoteGattServiceWin::GetIncludedServices() const {
  NOTIMPLEMENTED();
  // TODO(crbug.com/590008): Needs implementation.
  return std::vector<BluetoothGattService*>();
}

BluetoothGattCharacteristic* BluetoothRemoteGattServiceWin::GetCharacteristic(
    const std::string& identifier) const {
  GattCharacteristicsMap::const_iterator it =
      included_characteristics_.find(identifier);
  if (it != included_characteristics_.end())
    return it->second.get();
  return nullptr;
}

bool BluetoothRemoteGattServiceWin::AddCharacteristic(
    device::BluetoothGattCharacteristic* characteristic) {
  NOTIMPLEMENTED();
  return false;
}

bool BluetoothRemoteGattServiceWin::AddIncludedService(
    device::BluetoothGattService* service) {
  NOTIMPLEMENTED();
  return false;
}

void BluetoothRemoteGattServiceWin::Register(
    const base::Closure& callback,
    const ErrorCallback& error_callback) {
  NOTIMPLEMENTED();
  error_callback.Run();
}

void BluetoothRemoteGattServiceWin::Unregister(
    const base::Closure& callback,
    const ErrorCallback& error_callback) {
  NOTIMPLEMENTED();
  error_callback.Run();
}

void BluetoothRemoteGattServiceWin::GattCharacteristicDiscoveryComplete(
    BluetoothRemoteGattCharacteristicWin* characteristic) {
  DCHECK(ui_task_runner_->RunsTasksOnCurrentThread());
  DCHECK(included_characteristics_.find(characteristic->GetIdentifier()) !=
         included_characteristics_.end());

  discovery_completed_included_charateristics_.insert(
      characteristic->GetIdentifier());
  adapter_->NotifyGattCharacteristicAdded(characteristic);
  NotifyGattDiscoveryCompleteForServiceIfNecessary();
}

void BluetoothRemoteGattServiceWin::Update() {
  DCHECK(ui_task_runner_->RunsTasksOnCurrentThread());

  task_manager_->PostGetGattIncludedCharacteristics(
      service_path_, service_uuid_, service_attribute_handle_,
      base::Bind(&BluetoothRemoteGattServiceWin::OnGetIncludedCharacteristics,
                 weak_ptr_factory_.GetWeakPtr()));
}

void BluetoothRemoteGattServiceWin::OnGetIncludedCharacteristics(
    scoped_ptr<BTH_LE_GATT_CHARACTERISTIC> characteristics,
    uint16_t num,
    HRESULT hr) {
  DCHECK(ui_task_runner_->RunsTasksOnCurrentThread());

  UpdateIncludedCharacteristics(characteristics.get(), num);
  included_characteristics_discovered_ = true;
  NotifyGattDiscoveryCompleteForServiceIfNecessary();
}

void BluetoothRemoteGattServiceWin::UpdateIncludedCharacteristics(
    PBTH_LE_GATT_CHARACTERISTIC characteristics,
    uint16_t num) {
  if (num == 0) {
    if (included_characteristics_.size() > 0) {
      ClearIncludedCharacteristics();
      adapter_->NotifyGattServiceChanged(this);
    }
    return;
  }

  // First, remove characteristics that no longer exist.
  std::vector<std::string> to_be_removed;
  for (const auto& c : included_characteristics_) {
    if (!DoesCharacteristicExist(characteristics, num, c.second.get()))
      to_be_removed.push_back(c.second->GetIdentifier());
  }
  for (const auto& id : to_be_removed) {
    RemoveIncludedCharacteristic(id);
  }

  // Update previously known characteristics.
  for (auto& c : included_characteristics_)
    c.second->Update();

  // Return if no new characteristics have been added.
  if (included_characteristics_.size() == num)
    return;

  // Add new characteristics.
  for (uint16_t i = 0; i < num; i++) {
    if (!IsCharacteristicDiscovered(characteristics[i].CharacteristicUuid,
                                    characteristics[i].AttributeHandle)) {
      PBTH_LE_GATT_CHARACTERISTIC info = new BTH_LE_GATT_CHARACTERISTIC();
      *info = characteristics[i];
      BluetoothRemoteGattCharacteristicWin* characteristic_object =
          new BluetoothRemoteGattCharacteristicWin(this, info, ui_task_runner_);
      included_characteristics_[characteristic_object->GetIdentifier()] =
          make_scoped_ptr(characteristic_object);
    }
  }

  if (included_characteristics_discovered_)
    adapter_->NotifyGattServiceChanged(this);
}

void BluetoothRemoteGattServiceWin::
    NotifyGattDiscoveryCompleteForServiceIfNecessary() {
  if (discovery_completed_included_charateristics_.size() ==
          included_characteristics_.size() &&
      included_characteristics_discovered_ && !discovery_complete_notified_) {
    adapter_->NotifyGattDiscoveryComplete(this);
    discovery_complete_notified_ = true;
  }
}

bool BluetoothRemoteGattServiceWin::IsCharacteristicDiscovered(
    BTH_LE_UUID& uuid,
    uint16_t attribute_handle) {
  BluetoothUUID bt_uuid =
      BluetoothTaskManagerWin::BluetoothLowEnergyUuidToBluetoothUuid(uuid);
  for (const auto& c : included_characteristics_) {
    if (bt_uuid == c.second->GetUUID() &&
        attribute_handle == c.second->GetAttributeHandle()) {
      return true;
    }
  }
  return false;
}

bool BluetoothRemoteGattServiceWin::DoesCharacteristicExist(
    PBTH_LE_GATT_CHARACTERISTIC characteristics,
    uint16_t num,
    BluetoothRemoteGattCharacteristicWin* characteristic) {
  for (uint16_t i = 0; i < num; i++) {
    BluetoothUUID uuid =
        BluetoothTaskManagerWin::BluetoothLowEnergyUuidToBluetoothUuid(
            characteristics[i].CharacteristicUuid);
    if (characteristic->GetUUID() == uuid &&
        characteristic->GetAttributeHandle() ==
            characteristics[i].AttributeHandle) {
      return true;
    }
  }
  return false;
}

void BluetoothRemoteGattServiceWin::RemoveIncludedCharacteristic(
    std::string identifier) {
  discovery_completed_included_charateristics_.erase(identifier);
  included_characteristics_.erase(identifier);
}

void BluetoothRemoteGattServiceWin::ClearIncludedCharacteristics() {
  discovery_completed_included_charateristics_.clear();
  included_characteristics_.clear();
}

}  // namespace device.
