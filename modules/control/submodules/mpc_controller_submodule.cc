/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/control/submodules/mpc_controller_submodule.h"
#include "modules/common/adapters/adapter_gflags.h"
#include "modules/common/time/time.h"
#include "modules/common/vehicle_state/vehicle_state_provider.h"
#include "modules/control/common/control_gflags.h"

namespace apollo {
namespace control {

using apollo::canbus::Chassis;
using apollo::common::ErrorCode;
using apollo::common::Status;
using apollo::common::VehicleStateProvider;
using apollo::common::time::Clock;
using apollo::localization::LocalizationEstimate;
using apollo::planning::ADCTrajectory;

MPCControllerSubmodule::MPCControllerSubmodule()
    : monitor_logger_buffer_(common::monitor::MonitorMessageItem::CONTROL) {}

MPCControllerSubmodule::~MPCControllerSubmodule() {}

std::string MPCControllerSubmodule::Name() const {
  return FLAGS_mpc_controller_submodule_name;
}

bool MPCControllerSubmodule::Init() {
  if (!cyber::common::GetProtoFromFile(FLAGS_mpc_controller_conf_file,
                                       &mpc_controller_conf_)) {
    AERROR << "Unable to load control conf file: " +
                  FLAGS_mpc_controller_conf_file;
    return false;
  }

  // MPC controller
  if (!mpc_controller_.Init(&mpc_controller_conf_).ok()) {
    monitor_logger_buffer_.ERROR(
        "MPC Control init controller failed! Stopping...");
    return false;
  }
  // initialize readers and writers
  cyber::ReaderConfig chassis_reader_config;
  chassis_reader_config.channel_name = FLAGS_chassis_topic;
  chassis_reader_config.pending_queue_size = FLAGS_chassis_pending_queue_size;

  chassis_reader_ =
      node_->CreateReader<Chassis>(chassis_reader_config, nullptr);
  CHECK(chassis_reader_ != nullptr);

  cyber::ReaderConfig planning_reader_config;
  planning_reader_config.channel_name = FLAGS_planning_trajectory_topic;
  planning_reader_config.pending_queue_size = FLAGS_planning_pending_queue_size;

  trajectory_reader_ =
      node_->CreateReader<ADCTrajectory>(planning_reader_config, nullptr);
  CHECK(trajectory_reader_ != nullptr);

  cyber::ReaderConfig localization_reader_config;
  localization_reader_config.channel_name = FLAGS_localization_topic;
  localization_reader_config.pending_queue_size =
      FLAGS_localization_pending_queue_size;

  localization_reader_ = node_->CreateReader<LocalizationEstimate>(
      localization_reader_config, nullptr);
  CHECK(localization_reader_ != nullptr);

  cyber::ReaderConfig pad_msg_reader_config;
  pad_msg_reader_config.channel_name = FLAGS_pad_topic;
  pad_msg_reader_config.pending_queue_size = FLAGS_pad_msg_pending_queue_size;

  pad_msg_reader_ =
      node_->CreateReader<PadMessage>(pad_msg_reader_config, nullptr);
  CHECK(pad_msg_reader_ != nullptr);

  control_command_writer_ =
      node_->CreateWriter<ControlCommand>(FLAGS_control_command_topic);
  CHECK(control_command_writer_ != nullptr);

  // set initial vehicle state by cmd
  // need to sleep, because advertised channel is not ready immediately
  // simple test shows a short delay of 80 ms or so
  AINFO << "Control resetting vehicle state, sleeping for 1000 ms ...";
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  // should init_vehicle first, let car enter work status, then use status msg
  // trigger control
  AINFO << "Control default driving action is "
        << DrivingAction_Name(mpc_controller_conf_.action());
  pad_msg_.set_action(mpc_controller_conf_.action());

  return true;
}

bool MPCControllerSubmodule::Proc() {
  // will uncomment during implementation
  // double start_timestamp = Clock::NowInSeconds();

  chassis_reader_->Observe();
  const auto &chassis_msg = chassis_reader_->GetLatestObserved();
  if (chassis_msg == nullptr) {
    AERROR << "Chassis msg is not ready!";
    return false;
  }
  OnChassis(chassis_msg);

  // TODO(Shu): implementation
  ControlCommand control_command;
  Status status = ProduceControlCommand(&control_command);
  AERROR_IF(!status.ok()) << "Failed to produce control command:"
                          << status.error_message();
  control_command_writer_->Write(
      std::make_shared<ControlCommand>(control_command));
  return true;
}

Status MPCControllerSubmodule::ProduceControlCommand(
    ControlCommand *control_command) {
  // TODO(SHU): move to pre_processing submodule

  {
    std::lock_guard<std::mutex> lock(mutex_);
    local_view_.chassis = latest_chassis_;
    local_view_.trajectory = latest_trajectory_;
    local_view_.localization = latest_localization_;
  }

  Status status = CheckInput(&local_view_);
  // check data

  if (!status.ok()) {
    AERROR_EVERY(100) << "Control input data failed: "
                      << status.error_message();
    control_command->mutable_engage_advice()->set_advice(
        apollo::common::EngageAdvice::DISALLOW_ENGAGE);
    control_command->mutable_engage_advice()->set_reason(
        status.error_message());
    estop_ = true;
    estop_reason_ = status.error_message();
  } else {
    Status status_ts = CheckTimestamp(local_view_);
    if (!status_ts.ok()) {
      AERROR << "Input messages timeout";
      // estop_ = true;
      status = status_ts;
      if (local_view_.chassis.driving_mode() !=
          apollo::canbus::Chassis::COMPLETE_AUTO_DRIVE) {
        control_command->mutable_engage_advice()->set_advice(
            apollo::common::EngageAdvice::DISALLOW_ENGAGE);
        control_command->mutable_engage_advice()->set_reason(
            status.error_message());
      }
    } else {
      control_command->mutable_engage_advice()->set_advice(
          apollo::common::EngageAdvice::READY_TO_ENGAGE);
    }
  }

  // check estop
  estop_ = mpc_controller_conf_.enable_persistent_estop()
               ? estop_ || local_view_.trajectory.estop().is_estop()
               : local_view_.trajectory.estop().is_estop();

  if (local_view_.trajectory.estop().is_estop()) {
    estop_ = true;
    estop_reason_ = "estop from planning : ";
    estop_reason_ += local_view_.trajectory.estop().reason();
  }

  if (local_view_.trajectory.trajectory_point().empty()) {
    AWARN_EVERY(100) << "planning has no trajectory point. ";
    estop_ = true;
    estop_reason_ = "estop for empty planning trajectory, planning headers: " +
                    local_view_.trajectory.header().ShortDebugString();
  }

  if (FLAGS_enable_gear_dirve_negative_speed_protection) {
    const double kEpsilon = 0.001;
    auto first_trajectory_point = local_view_.trajectory.trajectory_point(0);
    if (local_view_.chassis.gear_location() == Chassis::GEAR_DRIVE &&
        first_trajectory_point.v() < -1 * kEpsilon) {
      estop_ = true;
      estop_reason_ = "estop for negative speed when gear_drive";
    }
  }

  if (!estop_) {
    if (local_view_.chassis.driving_mode() == Chassis::COMPLETE_MANUAL) {
      mpc_controller_.Reset();
      AINFO_EVERY(100) << "Reset Controllers in Manual Mode";
    }

    auto debug = control_command->mutable_debug()->mutable_input_debug();
    debug->mutable_localization_header()->CopyFrom(
        local_view_.localization.header());
    debug->mutable_canbus_header()->CopyFrom(local_view_.chassis.header());
    debug->mutable_trajectory_header()->CopyFrom(
        local_view_.trajectory.header());

    if (local_view_.trajectory.is_replan()) {
      latest_replan_trajectory_header_.CopyFrom(
          local_view_.trajectory.header());
    }

    if (latest_replan_trajectory_header_.has_sequence_num()) {
      debug->mutable_latest_replan_trajectory_header()->CopyFrom(
          latest_replan_trajectory_header_);
    }

    Status status_compute = mpc_controller_.ComputeControlCommand(
        &local_view_.localization, &local_view_.chassis,
        &local_view_.trajectory, control_command);

    if (!status_compute.ok()) {
      AERROR << "Control main function failed"
             << " with localization: "
             << local_view_.localization.ShortDebugString()
             << " with chassis: " << local_view_.chassis.ShortDebugString()
             << " with trajectory: "
             << local_view_.trajectory.ShortDebugString()
             << " with cmd: " << control_command->ShortDebugString()
             << " status:" << status_compute.error_message();
      estop_ = true;
      estop_reason_ = status_compute.error_message();
      status = status_compute;
    }
  }

  // if planning set estop, then no control process triggered
  if (estop_) {
    AWARN_EVERY(100) << "Estop triggered! No control core method executed!";
    // set Estop command
    control_command->set_speed(0);
    control_command->set_throttle(0);
    control_command->set_brake(mpc_controller_conf_.soft_estop_brake());
    control_command->set_gear_location(Chassis::GEAR_DRIVE);
  }
  // check signal
  if (local_view_.trajectory.decision().has_vehicle_signal()) {
    control_command->mutable_signal()->CopyFrom(
        local_view_.trajectory.decision().vehicle_signal());
  }
  return status;
}

void MPCControllerSubmodule::OnChassis(
    const std::shared_ptr<Chassis> &chassis) {
  ADEBUG << "Received chassis data: run chassis callback.";
  std::lock_guard<std::mutex> lock(mutex_);
  latest_chassis_.CopyFrom(*chassis);
}

void MPCControllerSubmodule::OnPad(const std::shared_ptr<PadMessage> &pad) {
  pad_msg_.CopyFrom(*pad);
  ADEBUG << "Received Pad Msg:" << pad_msg_.DebugString();
  AERROR_IF(!pad_msg_.has_action()) << "pad message check failed!";

  // do something according to pad message
  if (pad_msg_.action() == DrivingAction::RESET) {
    AINFO << "Control received RESET action!";
    estop_ = false;
    estop_reason_.clear();
  }
  pad_received_ = true;
}

void MPCControllerSubmodule::OnPlanning(
    const std::shared_ptr<ADCTrajectory> &trajectory) {
  ADEBUG << "Received chassis data: run trajectory callback.";
  std::lock_guard<std::mutex> lock(mutex_);
  latest_trajectory_.CopyFrom(*trajectory);
}

void MPCControllerSubmodule::OnLocalization(
    const std::shared_ptr<LocalizationEstimate> &localization) {
  ADEBUG << "Received control data: run localization message callback.";
  std::lock_guard<std::mutex> lock(mutex_);
  latest_localization_.CopyFrom(*localization);
}

void MPCControllerSubmodule::OnMonitor(
    const common::monitor::MonitorMessage &monitor_message) {
  for (const auto &item : monitor_message.item()) {
    if (item.log_level() == common::monitor::MonitorMessageItem::FATAL) {
      estop_ = true;
      return;
    }
  }
}

Status MPCControllerSubmodule::CheckInput(LocalView *local_view) {
  ADEBUG << "Received localization:"
         << local_view->localization.ShortDebugString();
  ADEBUG << "Received chassis:" << local_view->chassis.ShortDebugString();

  if (!local_view->trajectory.estop().is_estop() &&
      local_view->trajectory.trajectory_point().empty()) {
    AWARN_EVERY(100) << "planning has no trajectory point. ";
    std::string msg("planning has no trajectory point. planning_seq_num:");
    msg += std::to_string(local_view->trajectory.header().sequence_num());
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, msg);
  }

  for (auto &trajectory_point :
       *local_view->trajectory.mutable_trajectory_point()) {
    if (std::abs(trajectory_point.v()) <
            mpc_controller_conf_.minimum_speed_resolution() &&
        std::abs(trajectory_point.a()) <
            mpc_controller_conf_.max_acceleration_when_stopped()) {
      trajectory_point.set_v(0.0);
      trajectory_point.set_a(0.0);
    }
  }

  VehicleStateProvider::Instance()->Update(local_view->localization,
                                           local_view->chassis);

  return Status::OK();
}

Status MPCControllerSubmodule::CheckTimestamp(const LocalView &local_view) {
  if (!mpc_controller_conf_.enable_input_timestamp_check() ||
      mpc_controller_conf_.is_control_test_mode()) {
    ADEBUG << "Skip input timestamp check by gflags.";
    return Status::OK();
  }
  double current_timestamp = Clock::NowInSeconds();
  double localization_diff =
      current_timestamp - local_view.localization.header().timestamp_sec();
  if (localization_diff > (mpc_controller_conf_.max_localization_miss_num() *
                           mpc_controller_conf_.localization_period())) {
    AERROR << "Localization msg lost for " << std::setprecision(6)
           << localization_diff << "s";
    monitor_logger_buffer_.ERROR("Localization msg lost");
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "Localization msg timeout");
  }

  double chassis_diff =
      current_timestamp - local_view.chassis.header().timestamp_sec();
  if (chassis_diff > (mpc_controller_conf_.max_chassis_miss_num() *
                      mpc_controller_conf_.chassis_period())) {
    AERROR << "Chassis msg lost for " << std::setprecision(6) << chassis_diff
           << "s";
    monitor_logger_buffer_.ERROR("Chassis msg lost");
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "Chassis msg timeout");
  }

  double trajectory_diff =
      current_timestamp - local_view.trajectory.header().timestamp_sec();
  if (trajectory_diff > (mpc_controller_conf_.max_planning_miss_num() *
                         mpc_controller_conf_.trajectory_period())) {
    AERROR << "Trajectory msg lost for " << std::setprecision(6)
           << trajectory_diff << "s";
    monitor_logger_buffer_.ERROR("Trajectory msg lost");
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "Trajectory msg timeout");
  }
  return Status::OK();
}

}  // namespace control
}  // namespace apollo