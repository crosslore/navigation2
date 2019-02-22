// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dwb_controller/dwb_controller.hpp"

#include <chrono>
#include <memory>
#include <string>

#include "dwb_core/exceptions.hpp"
#include "nav_2d_utils/conversions.hpp"

using namespace std::chrono_literals;
using nav2_tasks::TaskStatus;

namespace nav2_dwb_controller
{

DwbController::DwbController()
: LifecycleNode("dwb_controller")
{
  RCLCPP_INFO(get_logger(), "Creating");

  costmap_ros_ = std::make_shared<nav2_costmap_2d::Costmap2DROS>("dwb_controller_local_costmap");
  costmap_thread_ = std::make_unique<std::thread>([](rclcpp_lifecycle::LifecycleNode::SharedPtr node) {rclcpp::spin(node->get_node_base_interface());}, costmap_ros_);
}

DwbController::~DwbController()
{
  RCLCPP_INFO(get_logger(), "Destroying");
  costmap_thread_->join();
}

nav2_lifecycle::CallbackReturn
DwbController::onConfigure(const rclcpp_lifecycle::State & state)
{
  RCLCPP_INFO(get_logger(), "onConfigure");

  costmap_ros_->onConfigure(state);

  planner_ = std::make_unique<dwb_core::DWBLocalPlanner>(shared_from_this(), costmap_ros_->getTfBuffer(), costmap_ros_);
  planner_->onConfigure(state);

  odom_sub_ = std::make_shared<nav_2d_utils::OdomSubscriber>(*this);
  vel_pub_ = create_publisher<geometry_msgs::msg::Twist>("/cmd_vel", 1);

  task_server_ = std::make_unique<nav2_tasks::FollowPathTaskServer>(shared_from_this());
  task_server_->onConfigure(state);
  task_server_->setExecuteCallback(
    std::bind(&DwbController::followPath, this, std::placeholders::_1));

  return nav2_lifecycle::CallbackReturn::SUCCESS;
}

nav2_lifecycle::CallbackReturn
DwbController::onActivate(const rclcpp_lifecycle::State & state)
{
  RCLCPP_INFO(get_logger(), "onActivate");

  planner_->onActivate(state);
  costmap_ros_->onActivate(state);
  vel_pub_->on_activate();
  task_server_->onActivate(state);

  return nav2_lifecycle::CallbackReturn::SUCCESS;
}

nav2_lifecycle::CallbackReturn
DwbController::onDeactivate(const rclcpp_lifecycle::State & state)
{
  RCLCPP_INFO(get_logger(), "onDeactivate");

  planner_->onDeactivate(state);
  costmap_ros_->onDeactivate(state);
  vel_pub_->on_deactivate();
  task_server_->onDeactivate(state);

  return nav2_lifecycle::CallbackReturn::SUCCESS;
}

nav2_lifecycle::CallbackReturn
DwbController::onCleanup(const rclcpp_lifecycle::State & state)
{
  RCLCPP_INFO(get_logger(), "onCleanup");

  // Cleanup the helper classes
  planner_->onCleanup(state);
  costmap_ros_->onCleanup(state);
  task_server_->onCleanup(state);

  // Release any allocated resources
  planner_.reset();
  odom_sub_.reset();
  vel_pub_.reset();
  task_server_.reset();

  return nav2_lifecycle::CallbackReturn::SUCCESS;
}

TaskStatus
DwbController::followPath(const nav2_tasks::FollowPathCommand::SharedPtr command)
{
  RCLCPP_INFO(get_logger(), "Starting controller");
  try {
    auto path = nav_2d_utils::pathToPath2D(*command);
    auto nh = shared_from_this();

    planner_->setPlan(path);
    RCLCPP_INFO(get_logger(), "Initialized");

    rclcpp::Rate loop_rate(10);
    while (rclcpp::ok()) {
      nav_2d_msgs::msg::Pose2DStamped pose2d;
      if (!getRobotPose(pose2d)) {
        RCLCPP_INFO(get_logger(), "No pose. Stopping robot");
        publishZeroVelocity();
      } else {
        if (isGoalReached(pose2d)) {
          break;
        }
        auto velocity = odom_sub_->getTwist();
        auto cmd_vel_2d = planner_->computeVelocityCommands(pose2d, velocity);
        publishVelocity(cmd_vel_2d);

        // TODO(mjeronimo): The rclcpp_lifecycle::LifecycleNode doesn't yet support sim time
        // RCLCPP_INFO(get_logger(), "Publishing velocity at time %.2f", now().seconds());
        RCLCPP_INFO(get_logger(), "Publishing velocity");

        // Check if this task has been canceled
        if (task_server_->cancelRequested()) {
          RCLCPP_INFO(this->get_logger(), "execute: task has been canceled");
          task_server_->setCanceled();
          publishZeroVelocity();
          return TaskStatus::CANCELED;
        }

        // Check if there is an update to the path to follow
        if (task_server_->updateRequested()) {
          // Get the new, updated path
          auto path_cmd = std::make_shared<nav2_tasks::FollowPathCommand>();
          task_server_->getCommandUpdate(path_cmd);
          task_server_->setUpdated();

          // and pass it to the local planner
          auto path = nav_2d_utils::pathToPath2D(*path_cmd);
          planner_->setPlan(path);
        }
      }
	  loop_rate.sleep();
    }
  } catch (nav_core2::PlannerException & e) {
    RCLCPP_INFO(this->get_logger(), e.what());
    publishZeroVelocity();
    return TaskStatus::FAILED;
  }

  nav2_tasks::FollowPathResult result;
  task_server_->setResult(result);
  publishZeroVelocity();

  return TaskStatus::SUCCEEDED;
}

void DwbController::publishVelocity(const nav_2d_msgs::msg::Twist2DStamped & velocity)
{
  auto cmd_vel = nav_2d_utils::twist2Dto3D(velocity.velocity);
  vel_pub_->publish(cmd_vel);
}

void DwbController::publishZeroVelocity()
{
  nav_2d_msgs::msg::Twist2DStamped velocity;
  velocity.velocity.x = 0;
  velocity.velocity.y = 0;
  velocity.velocity.theta = 0;
  publishVelocity(velocity);
}

bool DwbController::isGoalReached(const nav_2d_msgs::msg::Pose2DStamped & pose2d)
{
  nav_2d_msgs::msg::Twist2D velocity = odom_sub_->getTwist();
  return planner_->isGoalReached(pose2d, velocity);
}

bool DwbController::getRobotPose(nav_2d_msgs::msg::Pose2DStamped & pose2d)
{
  geometry_msgs::msg::PoseStamped current_pose;
  if (!costmap_ros_->getRobotPose(current_pose)) {
    RCLCPP_ERROR(this->get_logger(), "Could not get robot pose");
    return false;
  }
  pose2d = nav_2d_utils::poseStampedToPose2D(current_pose);
  return true;
}

}  // namespace nav2_dwb_controller
