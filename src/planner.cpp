// Copyright (c) 2015, Robot Control and Pattern Recognition Group,
// Institute of Control and Computation Engineering
// Warsaw University of Technology
//
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the Warsaw University of Technology nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL <COPYright HOLDER> BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Author: Dawid Seredynski
//

#include <ros/ros.h>
#include "ros/package.h"
#include <sensor_msgs/JointState.h>
#include <visualization_msgs/MarkerArray.h>
#include <tf/transform_broadcaster.h>
#include <interactive_markers/interactive_marker_server.h>

// MoveIt!
#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit/planning_scene_monitor/planning_scene_monitor.h>
#include <moveit/planning_pipeline/planning_pipeline.h>
#include <moveit_msgs/GetMotionPlan.h>
#include <moveit_msgs/ApplyPlanningScene.h>

#include <moveit/kinematic_constraints/utils.h>
#include <eigen_conversions/eigen_msg.h>
#include <eigen_conversions/eigen_kdl.h>

#include <string>
#include <stdlib.h>
#include <stdio.h>

#include "Eigen/Dense"
#include "Eigen/LU"

#include <collision_convex_model/collision_convex_model.h>
#include "kin_dyn_model/kin_model.h"
#include "planer_utils/marker_publisher.h"
#include "planer_utils/random_uniform.h"
#include "planer_utils/utilities.h"
#include "planer_utils/double_joint_collision_checker.h"
#include "std_srvs/Trigger.h"

class Planner {
private:
    ros::NodeHandle nh_;
    ros::Publisher joint_state_pub_;
    ros::ServiceServer service_reset_;
    ros::ServiceServer service_plan_;
    ros::ServiceServer service_processWorld_;

    MarkerPublisher markers_pub_;
    tf::TransformBroadcaster br;

    const double PI;

    KDL::Frame int_marker_pose_;

    std::string robot_description_str_;
    std::string robot_semantic_description_str_;

    boost::shared_ptr<self_collision::CollisionModel> col_model_;
    boost::shared_ptr<KinematicModel > kin_model_;
    std::vector<KDL::Frame > links_fk_;


//    robot_model_loader::RobotModelLoaderPtr robot_model_loader_;
//    planning_scene_monitor::PlanningSceneMonitorPtr planning_scene_monitor_;

    robot_model::RobotModelPtr robot_model_;
    planning_scene::PlanningScenePtr planning_scene_;
    planning_pipeline::PlanningPipelinePtr planning_pipeline_;

    // ROS parameters
    std::vector<double > wcc_l_constraint_polygon_;
    int wcc_l_joint0_idx_;
    int wcc_l_joint1_idx_;

    std::vector<double > wcc_r_constraint_polygon_;
    int wcc_r_joint0_idx_;
    int wcc_r_joint1_idx_;


    boost::shared_ptr<DoubleJointCC > wcc_l_;
    boost::shared_ptr<DoubleJointCC > wcc_r_;

public:
    Planner() :
        nh_("planner"),
        PI(3.141592653589793),
        markers_pub_(nh_)
    {
        joint_state_pub_ = nh_.advertise<sensor_msgs::JointState>("/joint_states", 10);
        service_reset_ = nh_.advertiseService("reset", &Planner::reset, this);
        service_plan_ = nh_.advertiseService("plan", &Planner::plan, this);
        service_processWorld_ = nh_.advertiseService("processWorld", &Planner::processWorld, this);

        nh_.getParam("/robot_description", robot_description_str_);
        nh_.getParam("/robot_semantic_description", robot_semantic_description_str_);

        nh_.getParam("/velma_core_cs/wcc_l/constraint_polygon", wcc_l_constraint_polygon_);
        nh_.getParam("/velma_core_cs/wcc_l/joint0_idx", wcc_l_joint0_idx_);
        nh_.getParam("/velma_core_cs/wcc_l/joint1_idx", wcc_l_joint1_idx_);

        nh_.getParam("/velma_core_cs/wcc_r/constraint_polygon", wcc_r_constraint_polygon_);
        nh_.getParam("/velma_core_cs/wcc_r/joint0_idx", wcc_r_joint0_idx_);
        nh_.getParam("/velma_core_cs/wcc_r/joint1_idx", wcc_r_joint1_idx_);

        if (wcc_l_constraint_polygon_.size() == 0 || (wcc_l_constraint_polygon_.size()%2) != 0) {
            ROS_ERROR("property \'constraint_polygon\' (l) has wrong size: %lu", wcc_l_constraint_polygon_.size());
        }

        if (wcc_l_joint0_idx_ == wcc_l_joint1_idx_) {
            ROS_ERROR("properties \'joint0_idx\' and \'joint1_idx\' (l) have the same value: %d", wcc_l_joint0_idx_);
        }

        if (wcc_r_constraint_polygon_.size() == 0 || (wcc_r_constraint_polygon_.size()%2) != 0) {
            ROS_ERROR("property \'constraint_polygon\' (r) has wrong size: %lu", wcc_r_constraint_polygon_.size());
        }

        if (wcc_r_joint0_idx_ == wcc_r_joint1_idx_) {
            ROS_ERROR("properties \'joint0_idx\' and \'joint1_idx\' (r) have the same value: %d", wcc_r_joint0_idx_);
        }

        wcc_l_.reset(new DoubleJointCC(0.0, wcc_l_constraint_polygon_));
        wcc_r_.reset(new DoubleJointCC(0.0, wcc_r_constraint_polygon_));

        //
        // collision model
        //
        col_model_ = self_collision::CollisionModel::parseURDF(robot_description_str_);
	    col_model_->parseSRDF(robot_semantic_description_str_);
        col_model_->generateCollisionPairs();
        links_fk_.resize(col_model_->getLinksCount());

        std::string xml_out = robot_description_str_;
        self_collision::CollisionModel::convertSelfCollisionsInURDF(robot_description_str_, xml_out);
//        std::cout << xml_out << std::endl;

        //
        // moveit
        //
        robot_model_loader::RobotModelLoader robot_model_loader( robot_model_loader::RobotModelLoader::Options(xml_out, robot_semantic_description_str_) );
        robot_model_ = robot_model_loader.getModel();

        planning_scene_.reset( new planning_scene::PlanningScene(robot_model_) );

        planning_scene_->setStateFeasibilityPredicate( boost::bind(&Planner::isStateValid, this, _1, _2) );

        planning_pipeline_.reset( new planning_pipeline::PlanningPipeline(robot_model_, nh_, "planning_plugin", "request_adapters") );

//        robot_model_loader_.reset( new robot_model_loader::RobotModelLoader(robot_model_loader::RobotModelLoader::Options(robot_description_str_, robot_semantic_description_str_)) );
//        planning_scene_monitor_.reset( new planning_scene_monitor::PlanningSceneMonitor(robot_model_loader_) );
//        planning_scene_monitor_->getRobotModel();

    }

    ~Planner() {
    }

    bool reset(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& res) {
        res.success = true;
        return true;
    }

    bool processWorld(moveit_msgs::ApplyPlanningScene::Request& req, moveit_msgs::ApplyPlanningScene::Response& res) {
        if (!planning_scene_->processPlanningSceneWorldMsg(req.scene.world)) {
          ROS_ERROR("Error in processPlanningSceneWorldMsg");
          res.success = false;
        }
        res.success = true;
        return true;
    }

    bool plan(moveit_msgs::GetMotionPlan::Request& req, moveit_msgs::GetMotionPlan::Response& res) {

        planning_interface::MotionPlanResponse response;

        planning_interface::MotionPlanRequest request = req.motion_plan_request;

        planning_pipeline_->generatePlan(planning_scene_, request, response);

        response.getMessage( res.motion_plan_response );

        /* Check that the planning was successful */
        if (response.error_code_.val != response.error_code_.SUCCESS)
        {
          ROS_ERROR("Could not compute plan successfully, error: %d. For more detailed error description please refer to moveit_msgs/MoveItErrorCodes", response.error_code_.val);
          return false;
        }

        return true;
    }

    bool isStateValid(const robot_state::RobotState& ss, bool verbose) {
        DoubleJointCC::Joints q_r(ss.getVariablePosition("right_arm_5_joint"), ss.getVariablePosition("right_arm_6_joint"));
        if ( wcc_r_->inCollision(q_r) ) {
            return false;
        }

        DoubleJointCC::Joints q_l(ss.getVariablePosition("left_arm_5_joint"), ss.getVariablePosition("left_arm_6_joint"));
        if ( wcc_l_->inCollision(q_l) ) {
            return false;
        }

        return true;
    }

    void spin() {
        while (ros::ok()) {
            ros::spinOnce();
            ros::Duration(0.1).sleep();
        }
    }
};

int main(int argc, char** argv) {
    ros::init(argc, argv, "planner");
    Planner planner;
    planner.spin();
    return 0;
}


