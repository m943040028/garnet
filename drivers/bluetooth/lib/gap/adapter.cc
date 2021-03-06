// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "adapter.h"

#include <endian.h>

#include "garnet/drivers/bluetooth/lib/gap/remote_device.h"
#include "garnet/drivers/bluetooth/lib/hci/connection.h"
#include "garnet/drivers/bluetooth/lib/hci/legacy_low_energy_advertiser.h"
#include "garnet/drivers/bluetooth/lib/hci/low_energy_connector.h"
#include "garnet/drivers/bluetooth/lib/hci/sequential_command_runner.h"
#include "garnet/drivers/bluetooth/lib/hci/transport.h"
#include "garnet/drivers/bluetooth/lib/hci/util.h"
#include "garnet/drivers/bluetooth/lib/l2cap/channel_manager.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/random/uuid.h"

#include "low_energy_advertising_manager.h"
#include "low_energy_connection_manager.h"
#include "low_energy_discovery_manager.h"

namespace btlib {
namespace gap {

Adapter::Adapter(fxl::RefPtr<hci::Transport> hci)
    : identifier_(fxl::GenerateUUID()),
      hci_(hci),
      init_state_(State::kNotInitialized),
      weak_ptr_factory_(this) {
  FXL_DCHECK(hci_);

  auto message_loop = fsl::MessageLoop::GetCurrent();
  FXL_DCHECK(message_loop)
      << "gap: Adapter: Must be created on a valid MessageLoop";

  task_runner_ = message_loop->task_runner();
  init_seq_runner_ =
      std::make_unique<hci::SequentialCommandRunner>(task_runner_, hci_);

  auto self = weak_ptr_factory_.GetWeakPtr();
  hci_->SetTransportClosedCallback(
      [self] {
        if (self)
          self->OnTransportClosed();
      },
      task_runner_);
}

Adapter::~Adapter() {
  if (IsInitialized())
    ShutDown();
}

bool Adapter::Initialize(const InitializeCallback& callback,
                         const fxl::Closure& transport_closed_cb) {
  FXL_DCHECK(task_runner_->RunsTasksOnCurrentThread());
  FXL_DCHECK(callback);
  FXL_DCHECK(transport_closed_cb);

  if (IsInitialized()) {
    FXL_LOG(WARNING) << "gap: Adapter: Already initialized";
    return false;
  }

  FXL_DCHECK(!IsInitializing());

  if (!hci_->Initialize()) {
    FXL_LOG(ERROR) << "gap: Adapter: Failed to initialize HCI transport";
    return false;
  }

  init_state_ = State::kInitializing;

  FXL_DCHECK(init_seq_runner_->IsReady());
  FXL_DCHECK(!init_seq_runner_->HasQueuedCommands());

  transport_closed_cb_ = transport_closed_cb;

  // Start by resetting the controller to a clean state and then send
  // informational parameter commands that are not specific to LE or BR/EDR. The
  // commands sent here are mandatory for all LE controllers.
  //
  // NOTE: It's safe to pass capture |this| directly in the callbacks as
  // |init_seq_runner_| will internally invalidate the callbacks if it ever gets
  // deleted.

  // HCI_Reset
  init_seq_runner_->QueueCommand(hci::CommandPacket::New(hci::kReset));

  // HCI_Read_Local_Version_Information
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kReadLocalVersionInfo),
      [this](const hci::EventPacket& cmd_complete) {
        auto params =
            cmd_complete.return_params<hci::ReadLocalVersionInfoReturnParams>();
        state_.hci_version_ = params->hci_version;
      });

  // HCI_Read_Local_Supported_Commands
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kReadLocalSupportedCommands),
      [this](const hci::EventPacket& cmd_complete) {
        auto params =
            cmd_complete
                .return_params<hci::ReadLocalSupportedCommandsReturnParams>();
        std::memcpy(state_.supported_commands_, params->supported_commands,
                    sizeof(params->supported_commands));
      });

  // HCI_Read_Local_Supported_Features
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kReadLocalSupportedFeatures),
      [this](const hci::EventPacket& cmd_complete) {
        auto params =
            cmd_complete
                .return_params<hci::ReadLocalSupportedFeaturesReturnParams>();
        state_.lmp_features_[0] = le64toh(params->lmp_features);
      });

  // HCI_Read_BD_ADDR
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kReadBDADDR),
      [this](const hci::EventPacket& cmd_complete) {
        auto params = cmd_complete.return_params<hci::ReadBDADDRReturnParams>();
        state_.controller_address_ = params->bd_addr;
      });

  init_seq_runner_->RunCommands([callback, this](bool success) {
    if (!success) {
      FXL_LOG(ERROR)
          << "gap: Adapter: Failed to obtain initial controller information";
      CleanUp();
      callback(false);
      return;
    }

    InitializeStep2(callback);
  });

  return true;
}

void Adapter::ShutDown() {
  FXL_DCHECK(task_runner_->RunsTasksOnCurrentThread());
  FXL_DCHECK(IsInitialized());

  CleanUp();

  // TODO(armansito): Clean up all protocol layers and send HCI Reset.
}

void Adapter::InitializeStep2(const InitializeCallback& callback) {
  FXL_DCHECK(task_runner_->RunsTasksOnCurrentThread());
  FXL_DCHECK(IsInitializing());

  // Low Energy MUST be supported. We don't support BR/EDR-only controllers.
  if (!state_.IsLowEnergySupported()) {
    FXL_LOG(ERROR)
        << "gap: Adapter: Bluetooth Low Energy not supported by controller";
    CleanUp();
    callback(false);
    return;
  }

  // Check the HCI version. We officially only support 4.2+ only but for now we
  // just log a warning message if the version is legacy.
  if (state_.hci_version() < hci::HCIVersion::k4_2) {
    FXL_LOG(WARNING) << "gap: Adapter: controller is using legacy HCI version: "
                     << hci::HCIVersionToString(state_.hci_version());
  }

  FXL_DCHECK(init_seq_runner_->IsReady());

  // If the controller supports the Read Buffer Size command then send it.
  // Otherwise we'll default to 0 when initializing the ACLDataChannel.
  if (state_.IsCommandSupported(14, hci::SupportedCommand::kReadBufferSize)) {
    // HCI_Read_Buffer_Size
    init_seq_runner_->QueueCommand(
        hci::CommandPacket::New(hci::kReadBufferSize),
        [this](const hci::EventPacket& cmd_complete) {
          auto params =
              cmd_complete.return_params<hci::ReadBufferSizeReturnParams>();
          uint16_t mtu = le16toh(params->hc_acl_data_packet_length);
          uint16_t max_count = le16toh(params->hc_total_num_acl_data_packets);
          if (mtu && max_count) {
            state_.bredr_data_buffer_info_ =
                hci::DataBufferInfo(mtu, max_count);
          }
        });
  }

  // HCI_LE_Read_Local_Supported_Features
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kLEReadLocalSupportedFeatures),
      [this](const hci::EventPacket& cmd_complete) {
        auto params =
            cmd_complete
                .return_params<hci::LEReadLocalSupportedFeaturesReturnParams>();
        state_.le_state_.supported_features_ = le64toh(params->le_features);
      });

  // HCI_LE_Read_Supported_States
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kLEReadSupportedStates),
      [this](const hci::EventPacket& cmd_complete) {
        auto params =
            cmd_complete
                .return_params<hci::LEReadSupportedStatesReturnParams>();
        state_.le_state_.supported_states_ = le64toh(params->le_states);
      });

  // HCI_LE_Read_Buffer_Size
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kLEReadBufferSize),
      [this](const hci::EventPacket& cmd_complete) {
        auto params =
            cmd_complete.return_params<hci::LEReadBufferSizeReturnParams>();
        uint16_t mtu = le16toh(params->hc_le_acl_data_packet_length);
        uint8_t max_count = params->hc_total_num_le_acl_data_packets;
        if (mtu && max_count) {
          state_.le_state_.data_buffer_info_ =
              hci::DataBufferInfo(mtu, max_count);
        }
      });

  // If there are extended features then try to read the first page of the
  // extended features.
  if (state_.HasLMPFeatureBit(0u, hci::LMPFeature::kExtendedFeatures)) {
    // Page index 1 must be available.
    state_.max_lmp_feature_page_index_ = 1;

    // HCI_Read_Local_Extended_Features
    auto cmd_packet = hci::CommandPacket::New(
        hci::kReadLocalExtendedFeatures,
        sizeof(hci::ReadLocalExtendedFeaturesCommandParams));

    // Try to read page 1.
    cmd_packet->mutable_view()
        ->mutable_payload<hci::ReadLocalExtendedFeaturesCommandParams>()
        ->page_number = 1;

    init_seq_runner_->QueueCommand(
        std::move(cmd_packet), [this](const hci::EventPacket& cmd_complete) {
          auto params =
              cmd_complete
                  .return_params<hci::ReadLocalExtendedFeaturesReturnParams>();
          state_.lmp_features_[1] = le64toh(params->extended_lmp_features);
          state_.max_lmp_feature_page_index_ = params->maximum_page_number;
        });
  }

  init_seq_runner_->RunCommands([callback, this](bool success) {
    if (!success) {
      FXL_LOG(ERROR) << "gap: Adapter: Failed to obtain initial controller "
                        "information (step 2)";
      CleanUp();
      callback(false);
      return;
    }

    InitializeStep3(callback);
  });
}

void Adapter::InitializeStep3(const InitializeCallback& callback) {
  FXL_DCHECK(task_runner_->RunsTasksOnCurrentThread());
  FXL_DCHECK(IsInitializing());

  if (!state_.bredr_data_buffer_info().IsAvailable() &&
      !state_.low_energy_state().data_buffer_info().IsAvailable()) {
    FXL_LOG(ERROR)
        << "gap: Adapter: Both BR/EDR and LE buffers are unavailable";
    CleanUp();
    callback(false);
    return;
  }

  // Now that we have all the ACL data buffer information it's time to
  // initialize the ACLDataChannel.
  if (!hci_->InitializeACLDataChannel(
          state_.bredr_data_buffer_info(),
          state_.low_energy_state().data_buffer_info())) {
    FXL_LOG(ERROR)
        << "gap: Adapter: Failed to initialize ACLDataChannel (step 3)";
    CleanUp();
    callback(false);
    return;
  }

  FXL_DCHECK(init_seq_runner_->IsReady());
  FXL_DCHECK(!init_seq_runner_->HasQueuedCommands());

  // HCI_Set_Event_Mask
  {
    uint64_t event_mask = BuildEventMask();
    auto cmd_packet = hci::CommandPacket::New(
        hci::kSetEventMask, sizeof(hci::SetEventMaskCommandParams));
    cmd_packet->mutable_view()
        ->mutable_payload<hci::SetEventMaskCommandParams>()
        ->event_mask = htole64(event_mask);
    init_seq_runner_->QueueCommand(std::move(cmd_packet));
  }

  // HCI_LE_Set_Event_Mask
  {
    uint64_t event_mask = BuildLEEventMask();
    auto cmd_packet = hci::CommandPacket::New(
        hci::kLESetEventMask, sizeof(hci::LESetEventMaskCommandParams));
    cmd_packet->mutable_view()
        ->mutable_payload<hci::LESetEventMaskCommandParams>()
        ->le_event_mask = htole64(event_mask);
    init_seq_runner_->QueueCommand(std::move(cmd_packet));
  }

  // HCI_Write_LE_Host_Support if the appropriate feature bit is not set AND if
  // the controller supports this command.
  if (!state_.HasLMPFeatureBit(1, hci::LMPFeature::kLESupportedHost) &&
      state_.IsCommandSupported(24,
                                hci::SupportedCommand::kWriteLEHostSupport)) {
    auto cmd_packet = hci::CommandPacket::New(
        hci::kWriteLEHostSupport, sizeof(hci::WriteLEHostSupportCommandParams));
    auto params = cmd_packet->mutable_view()
                      ->mutable_payload<hci::WriteLEHostSupportCommandParams>();
    params->le_supported_host = hci::GenericEnableParam::kEnable;
    params->simultaneous_le_host = 0x00;
    init_seq_runner_->QueueCommand(std::move(cmd_packet));
  }

  // If we know that Page 2 of the extended features bitfield is available, then
  // request it.
  if (state_.max_lmp_feature_page_index_ > 1) {
    auto cmd_packet = hci::CommandPacket::New(
        hci::kReadLocalExtendedFeatures,
        sizeof(hci::ReadLocalExtendedFeaturesCommandParams));

    // Try to read page 2.
    cmd_packet->mutable_view()
        ->mutable_payload<hci::ReadLocalExtendedFeaturesCommandParams>()
        ->page_number = 2;

    // HCI_Read_Local_Extended_Features
    init_seq_runner_->QueueCommand(
        std::move(cmd_packet), [this](const hci::EventPacket& cmd_complete) {
          auto params =
              cmd_complete
                  .return_params<hci::ReadLocalExtendedFeaturesReturnParams>();
          state_.lmp_features_[2] = le64toh(params->extended_lmp_features);
          state_.max_lmp_feature_page_index_ = params->maximum_page_number;
        });
  }

  init_seq_runner_->RunCommands([callback, this](bool success) {
    if (!success) {
      FXL_LOG(ERROR) << "gap: Adapter: Failed to obtain initial controller "
                        "information (step 3)";
      CleanUp();
      callback(false);
      return;
    }

    InitializeStep4(callback);
  });
}

void Adapter::InitializeStep4(const InitializeCallback& callback) {
  // Initialize the scan manager based on current feature support.
  if (state_.low_energy_state().IsFeatureSupported(
          hci::LESupportedFeature::kLEExtendedAdvertising)) {
    FXL_LOG(INFO) << "gap: adapter: controller supports extended advertising";
    FXL_LOG(INFO) << "gap: adapter: host doesn't support 5.0 extended features,"
                  << " defaulting to legacy procedures.";

    // TODO(armansito): Initialize |hci_le_*| objects here with extended-mode
    // versions.
  }

  // Called by |hci_le_connector_| when a connection was created due to an
  // incoming connection. This callback routes the received |link| to
  // |hci_le_advertiser_| for it to be matched to an advertisement instance.
  auto self = weak_ptr_factory_.GetWeakPtr();
  auto incoming_conn_cb = [self](std::unique_ptr<hci::Connection> link) {
    if (self && self->hci_le_advertiser_) {
      self->hci_le_advertiser_->OnIncomingConnection(std::move(link));
    }
  };

  hci_le_advertiser_ = std::make_unique<hci::LegacyLowEnergyAdvertiser>(hci_);
  hci_le_connector_ = std::make_unique<hci::LowEnergyConnector>(
      hci_, task_runner_, std::move(incoming_conn_cb));

  le_discovery_manager_ = std::make_unique<LowEnergyDiscoveryManager>(
      Mode::kLegacy, hci_, &device_cache_);

  l2cap_ = std::make_unique<l2cap::ChannelManager>(hci_, task_runner_);
  le_connection_manager_ = std::make_unique<LowEnergyConnectionManager>(
      hci_, hci_le_connector_.get(), &device_cache_, l2cap_.get());
  le_advertising_manager_ =
      std::make_unique<LowEnergyAdvertisingManager>(hci_le_advertiser_.get());

  // This completes the initialization sequence.
  init_state_ = State::kInitialized;
  callback(true);
}

uint64_t Adapter::BuildEventMask() {
  uint64_t event_mask = 0;

  // Enable events that are needed for basic flow control.
  event_mask |=
      static_cast<uint64_t>(hci::EventMask::kDisconnectionCompleteEvent);
  event_mask |= static_cast<uint64_t>(hci::EventMask::kHardwareErrorEvent);
  event_mask |= static_cast<uint64_t>(hci::EventMask::kLEMetaEvent);

  return event_mask;
}

uint64_t Adapter::BuildLEEventMask() {
  uint64_t event_mask = 0;

  event_mask |= static_cast<uint64_t>(hci::LEEventMask::kLEConnectionComplete);
  event_mask |= static_cast<uint64_t>(hci::LEEventMask::kLEAdvertisingReport);
  event_mask |=
      static_cast<uint64_t>(hci::LEEventMask::kLEConnectionUpdateComplete);

  return event_mask;
}

void Adapter::CleanUp() {
  FXL_DCHECK(task_runner_->RunsTasksOnCurrentThread());

  init_state_ = State::kNotInitialized;
  state_ = AdapterState();
  transport_closed_cb_ = nullptr;

  le_advertising_manager_ = nullptr;
  le_connection_manager_ = nullptr;
  le_discovery_manager_ = nullptr;

  hci_le_connector_ = nullptr;
  hci_le_advertiser_ = nullptr;

  if (hci_->IsInitialized())
    hci_->ShutDown();
}

void Adapter::OnTransportClosed() {
  FXL_LOG(INFO) << "gap: Adapter: HCI transport was closed";
  if (transport_closed_cb_)
    transport_closed_cb_();
}

}  // namespace gap
}  // namespace btlib
