#include <ros/ros.h>
#include <actionlib/client/simple_action_client.h>

#include <std_srvs/Empty.h>

#include <geometry_msgs/Pose.h>
#include <geometry_msgs/Quaternion.h>
#include <sensor_msgs/JointState.h>
#include <control_msgs/FollowJointTrajectoryAction.h>

#include <tf/transform_datatypes.h>
#include <ur_kin.h>
#include <tutorial_0905/SetTargetPose.h>
#include <tutorial_0905/MyPose.h>

// Gripper header
#include <robotiq_c_model_control/CModel_robot_output.h>

#include "widget.hpp"

typedef actionlib::SimpleActionClient< control_msgs::FollowJointTrajectoryAction > TrajClient;

class RobotArm {
 private:
  TrajClient *traj_client_;
  control_msgs::FollowJointTrajectoryGoal goal_;
  ros::NodeHandle nh_;
  ros::Publisher pub_end_effector_pose_;
  // gripper cmd publisher
  ros::Publisher pub_gripper_cmd;
  ros::Subscriber sub_joint_state_;
  ros::ServiceServer goto_pose_srv_;
  ros::ServiceServer goto_home_srv_;
  // gripper cmd
  robotiq_c_model_control::CModel_robot_output cmd;  
  
  int num_sols_;
  double joint_[6], tool_length_, planning_time_, finger_dist_;
  double validAngle(double angle) {
    if (abs(angle) > 2*M_PI) angle = fmod(angle, 2*M_PI);
    if (angle > M_PI) angle -= 2*M_PI;
    else if (angle < -M_PI) angle += 2*M_PI;
    return angle;
  }
  void JointStateCallback(const sensor_msgs::JointState &msg) {
    // ************************* Student Implementation Part 1 *************************
    joint_[0] = msg.position[0];
    joint_[1] = msg.position[1];
    joint_[2] = msg.position[2];
    joint_[3] = msg.position[3];
    joint_[4] = msg.position[4];
    joint_[5] = msg.position[5];
    // ************************* Student Implementation Part 1 *************************
    
    tutorial_0905::MyPose pose;
    double T[16] = {0};
    ur_kinematics::forward(joint_, T);
    // ************************* Student Implementation Part 2 *************************
    pose.x = T[3];
    pose.y = T[7];
    pose.z = T[11];
    pose.yaw = atan2(T[4], T[0]);
    pose.pitch = -asin(T[8]);
    pose.roll = atan2(T[9], T[10]);     
    pub_end_effector_pose_.publish(pose);
    // ************************* Student Implementation Part 2 *************************
  }
  void PoseToDH(geometry_msgs::Pose pose, double *T) {
    double roll = 0, pitch = 0, yaw = 0;
    geometry_msgs::Point &p = pose.position;
    tf::Quaternion q;
    tf::quaternionMsgToTF(pose.orientation, q);
    tf::Matrix3x3(q).getRPY(roll, pitch, yaw);
    double sinr = sin(roll), cosr = cos(roll);
    double sinp = sin(pitch), cosp = cos(pitch);
    double siny = sin(yaw), cosy = cos(yaw);
    
    // DH matrix, ZYX convention
    T[0] = cosy*cosp;
    T[1] = cosy*sinp*sinr - cosr*siny;
    T[2] = siny*sinr + cosy*cosr*sinp;
    T[3] = p.x;
    T[4] = cosp*siny;
    T[5] = cosy*cosr + siny*sinp*sinr;
    T[6] = cosr*siny*sinp - cosy*sinr;
    T[7] = p.y;
    T[8] = -sinp;
    T[9] = cosp*sinr;
    T[10] = cosp*cosr;
    T[11] = p.z;
    T[15] = 1;
  }  
  int PerformIK(geometry_msgs::Pose target_pose, double *sol) {
    double T[16] = {0};
    PoseToDH(target_pose, T);
    // tcp_link to ee_link
    // ************************* Student Implementation Part 3 *************************
    for (int i = 0; i < 3; ++i)
      T[i*4+3] -= tool_length_*T[i*4];
    // ************************* Student Implementation Part 3 *************************

    double q_sols[8*6], min = 1e6, dist = 0;
    int sols = ur_kinematics::inverse(T, q_sols), index = 0;
    for (int i = 0; i < sols; ++i) {
      // Preprocess joint angle to -pi ~ pi.      
      for (int j = 0; j < 6; ++j) {
        q_sols[i*6 + j] = validAngle(q_sols[i*6 + j]); 
        dist += pow(q_sols[i*6 + j] - joint_[j], 2);
      }
      // Find solution with minimun joint angle difference.
      if (min > dist) {
        min = dist;
        index = i; 
      } dist = 0;
    }

    for (int i = 0; i < 6; ++i) 
      sol[i] = q_sols[index*6 + i];
    return (num_sols_ = sols);
  }
  void initial_gripper(void)
  {
    // For detail, see: https://goo.gl/GjsXtH
    cmd.rACT = 1; // active 
    cmd.rGTO = 1; // go to
    cmd.rATR = 0; // automatic release
    cmd.rPR  = 0; // position request
    cmd.rSP  = 100; // speed
    cmd.rFR  = 40; // force
    pub_gripper_cmd.publish(cmd);
    // sleep 0.5s to make sure gripper receive the command
    ros::Duration(0.5).sleep();
  }
  //TODO: measure tool length
 public:
  RobotArm() : joint_(), tool_length_(0.18), planning_time_(10.0), finger_dist_(0), num_sols_(1) {
    // tell the action client that we want to spin a thread by default
    traj_client_ = new TrajClient("/follow_joint_trajectory", true);
    // wait for action server to come up
    while (!traj_client_->waitForServer(ros::Duration(5.0))) 
      ROS_INFO("Waiting for the joint_trajectory_action server");
    
    // Subscribe to /ariac/joint_state
    sub_joint_state_ = nh_.subscribe("/joint_states", 1, &RobotArm::JointStateCallback, this);
    pub_end_effector_pose_ = nh_.advertise<tutorial_0905::MyPose>("/ur3_control/cartesian_state", 1);
    pub_gripper_cmd = nh_.advertise<robotiq_c_model_control::CModel_robot_output>("/CModelRobotOutput", 1);
    goto_pose_srv_ = nh_.advertiseService("/ur3_control/goto_pose", &RobotArm::GotoPoseService, this);
    goto_home_srv_ = nh_.advertiseService("/ur3_control/goto_home", &RobotArm::GotoHomeService, this);
    // Initialize the gripper
    initial_gripper();
    trajectory_msgs::JointTrajectory &t = goal_.trajectory;
    t.joint_names.resize(6);
    t.joint_names[0] = "shoulder_pan_joint";
    t.joint_names[1] = "shoulder_lift_joint";
    t.joint_names[2] = "elbow_joint";
    t.joint_names[3] = "wrist_1_joint";
    t.joint_names[4] = "wrist_2_joint";
    t.joint_names[5] = "wrist_3_joint";

    // Go to pose "home"    
    t.points.resize(2); 
    t.points[0].positions.resize(6);
    t.points[0].velocities.resize(6);
    t.points[1].positions.resize(6);
    t.points[1].velocities.resize(6);
    
    ros::spinOnce();
    for (int i = 0; i < 6; ++i) {
      t.points[0].positions[i] = joint_[i];
      t.points[0].velocities[i] = 
      t.points[1].positions[i] =
      t.points[1].velocities[i] = 0;
    }
    t.points[1].positions[1] = t.points[1].positions[3] = -M_PI/2;
    t.points[0].time_from_start = ros::Duration(0);
    t.points[1].time_from_start = ros::Duration(planning_time_);
    StartTrajectory(goal_);
  }
  ~RobotArm() {
    delete traj_client_;
  }
  bool GotoPoseService(tutorial_0905::SetTargetPose::Request &req, tutorial_0905::SetTargetPose::Response &res) {
    finger_dist_ = req.gripping_force / 100.0 * 255; // 0: Fully open; 255: closed
    planning_time_ = req.planning_time;
    StartTrajectory(ArmToDesiredPoseTrajectory(req.target_pose));
    
    res.plan_result = num_sols_ == 0 ? "fail_to_find_solution" : "find_one_feasible_solution";
    return true;
  }
  bool GotoHomeService(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res) {
    trajectory_msgs::JointTrajectory &t = goal_.trajectory;
    t.joint_names.resize(6);
    t.joint_names[0] = "shoulder_pan_joint";
    t.joint_names[1] = "shoulder_lift_joint";
    t.joint_names[2] = "elbow_joint";
    t.joint_names[3] = "wrist_1_joint";
    t.joint_names[4] = "wrist_2_joint";
    t.joint_names[5] = "wrist_3_joint";

    // Go to pose "home"    
    t.points.resize(2); 
    t.points[0].positions.resize(6);
    t.points[0].velocities.resize(6);
    t.points[1].positions.resize(6);
    t.points[1].velocities.resize(6);
    
    ros::spinOnce();
    for (int i = 0; i < 6; ++i) {
      t.points[0].positions[i] = joint_[i];
      t.points[0].velocities[i] = 
      t.points[1].positions[i] =
      t.points[1].velocities[i] = 0;
    }
    t.points[1].positions[1] = t.points[1].positions[3] = -M_PI/2;
    t.points[0].time_from_start = ros::Duration(0);
    t.points[1].time_from_start = ros::Duration(planning_time_);
    StartTrajectory(goal_);
  }
  void StartTrajectory(control_msgs::FollowJointTrajectoryGoal goal) {
    goal.trajectory.header.stamp = ros::Time::now();
    traj_client_->sendGoal(goal);
    // Wait for ur3 to finish joint trajectory
    while(!traj_client_->getState().isDone() && ros::ok()) usleep(100000);
    
    // Wait for gripper to finish joint trajectory
    if (num_sols_ != 0) {
      cmd.rPR = finger_dist_;
      pub_gripper_cmd.publish(cmd);
    }   
  }
  control_msgs::FollowJointTrajectoryGoal ArmToDesiredPoseTrajectory(geometry_msgs::Pose pose) {
    // Get closest joint space solution
    double sol[6] = {0};
    if (!PerformIK(pose, sol)) {
      for (int i = 0; i < 6; ++i) 
        sol[i] = joint_[i];
    }
      
    trajectory_msgs::JointTrajectory &t = goal_.trajectory;
    t.points[0].time_from_start = ros::Duration(planning_time_);
    
    for (int i = 0; i < 6; ++i) {
      t.points[0].positions[i] = joint_[i];
      t.points[1].positions[i] = sol[i];
      t.points[0].velocities[i] = 
      t.points[1].velocities[i] = 0;
    }
    t.points[0].time_from_start = ros::Duration(0);
    t.points[1].time_from_start = ros::Duration(10.0);
    return goal_;
  }
  actionlib::SimpleClientGoalState getState() {
    return traj_client_->getState();
  }
};

int main(int argc, char** argv)
{
  // Create an controller instance
  ros::init(argc, argv, "ur3_control");
  RobotArm arm;
  ros::spin();

  return 0;
}
