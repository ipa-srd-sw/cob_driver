/*!
 *****************************************************************
 * \file
 *
 * \note
 *   Copyright (c) 2011 \n
 *   Fraunhofer Institute for Manufacturing Engineering
 *   and Automation (IPA) \n\n
 *
 *****************************************************************
 *
 * \note
 *   Project name: care-o-bot
 * \note
 *   ROS stack name: cob_driver
 * \note
 *   ROS package name: cob_trajectory_controller
 *
 * \author
 *   Author: Alexander Bubeck, email:alexander.bubeck@ipa.fhg.de
 * \author
 *   Supervised by: Alexander Bubeck, email:alexander.bubeck@ipa.fhg.de
 *
 * \date Date of creation: March 2011
 *
 * \brief
 *   Implementation of ROS node for powercube_chain.
 *
 *****************************************************************
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     - Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer. \n
 *     - Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution. \n
 *     - Neither the name of the Fraunhofer Institute for Manufacturing
 *       Engineering and Automation (IPA) nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission. \n
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License LGPL as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License LGPL for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License LGPL along with this program.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 ****************************************************************/



#include "ros/ros.h"
#include <sensor_msgs/JointState.h>
#include <control_msgs/JointTrajectoryControllerState.h>
#include <actionlib/server/simple_action_server.h>
#include <control_msgs/FollowJointTrajectoryAction.h>

#include <brics_actuator/JointVelocities.h>
#include <cob_trajectory_controller/genericArmCtrl.h>
// ROS service includes
#include <cob_srvs/Trigger.h>
#include <cob_srvs/SetOperationMode.h>
#include <cob_trajectory_controller/SetFloat.h>

#include <dynamic_reconfigure/server.h>
#include <cob_trajectory_controller/CobTrajectoryControllerConfig.h>


#define HZ 100

class cob_trajectory_controller_node
{
private:
    ros::NodeHandle n_;
    
    ros::Publisher joint_vel_pub_;
    ros::Subscriber joint_state_sub_;
    ros::Subscriber controller_state_;
    ros::Subscriber operation_mode_;
    ros::ServiceServer srvServer_Stop_;
    ros::ServiceServer srvServer_SetVel_;
    ros::ServiceServer srvServer_SetAcc_;
    ros::ServiceClient srvClient_SetOperationMode;

    actionlib::SimpleActionServer<control_msgs::FollowJointTrajectoryAction> as_follow_;
 
  
    //std::string action_name_;
    std::string action_name_follow_;  
    std::string current_operation_mode_;
    XmlRpc::XmlRpcValue JointNames_param_;
    std::vector<std::string> JointNames_;
    bool executing_;
    bool failure_;
    bool rejected_;
    bool preemted_;
    int DOF;
    double velocity_timeout_;

    int watchdog_counter;
    genericArmCtrl* traj_generator_;
    trajectory_msgs::JointTrajectory traj_;
    trajectory_msgs::JointTrajectory traj_2_;
    std::vector<double> q_current, startposition_, joint_distance_;
    
    dynamic_reconfigure::Server<cob_trajectory_controller::CobTrajectoryControllerConfig> reconfigure_server;

public:

    cob_trajectory_controller_node():
    as_follow_(n_, "follow_joint_trajectory", boost::bind(&cob_trajectory_controller_node::executeFollowTrajectory, this, _1), true),
    action_name_follow_("follow_joint_trajectory")
    {
        joint_vel_pub_ = n_.advertise<brics_actuator::JointVelocities>("command_vel", 1);
        joint_state_sub_ = n_.subscribe("/joint_states", 1, &cob_trajectory_controller_node::joint_state_callback, this);
        controller_state_ = n_.subscribe("state", 1, &cob_trajectory_controller_node::state_callback, this);
        operation_mode_ = n_.subscribe("current_operationmode", 1, &cob_trajectory_controller_node::operationmode_callback, this);
        srvServer_Stop_ = n_.advertiseService("stop", &cob_trajectory_controller_node::srvCallback_Stop, this);
        srvServer_SetVel_ = n_.advertiseService("set_joint_velocity", &cob_trajectory_controller_node::srvCallback_setVel, this);
        srvServer_SetAcc_ = n_.advertiseService("set_joint_acceleration", &cob_trajectory_controller_node::srvCallback_setAcc, this);
        srvClient_SetOperationMode = n_.serviceClient<cob_srvs::SetOperationMode>("set_operation_mode");
        //while(!srvClient_SetOperationMode.exists())
        //{
            //ROS_INFO("Waiting for operationmode service to become available");
            //sleep(1);
        //}
        executing_ = false;
        failure_ = false;
        rejected_ = false;
        preemted_ = false;
        watchdog_counter = 0;
        current_operation_mode_ = "undefined";
        double PTPvel = 0.7;
        double PTPacc = 0.2;
        double maxError = 0.7;
        double overlap_time = 0.4;
        velocity_timeout_ = 2.0;
        DOF = 7;
        // get JointNames from parameter server
        ROS_INFO("getting JointNames from parameter server");
        if (n_.hasParam("joint_names"))
        {
            n_.getParam("joint_names", JointNames_param_);
        }
        else
        {
            ROS_ERROR("Parameter joint_names not set");
        }
        JointNames_.resize(JointNames_param_.size());
        for (int i = 0; i<JointNames_param_.size(); i++ )
        {
            JointNames_[i] = (std::string)JointNames_param_[i];
        }
        DOF = JointNames_param_.size();
        
        if (n_.hasParam("ptp_vel"))
        {
            n_.getParam("ptp_vel", PTPvel);
        }
        if (n_.hasParam("ptp_acc"))
        {
            n_.getParam("ptp_acc", PTPacc);
        }
        if (n_.hasParam("max_error"))
        {
            n_.getParam("max_error", maxError);
        }
        if (n_.hasParam("overlap_time"))
        {
            n_.getParam("overlap_time", overlap_time);
        }
        if (n_.hasParam("operation_mode"))
        {
            n_.getParam("operation_mode", current_operation_mode_);
        }
        q_current.resize(DOF);
        ROS_INFO("starting controller with DOF: %d PTPvel: %f PTPAcc: %f maxError %f", DOF, PTPvel, PTPacc, maxError);
        traj_generator_ = new genericArmCtrl(DOF, PTPvel, PTPacc, maxError);
        traj_generator_->overlap_time = overlap_time;
        
        reconfigure_server.setCallback(boost::bind(&cob_trajectory_controller_node::dynamic_reconfigure_cb, this, _1, _2));
    }
    
    
    void dynamic_reconfigure_cb(cob_trajectory_controller::CobTrajectoryControllerConfig &config, uint32_t level)
    {
        ROS_INFO("Dynamically reconfigure cob_trajectory_controller parameter!");
        traj_generator_->SetPTPvel(config.ptp_vel);
        traj_generator_->SetPTPvel(config.ptp_acc);
        traj_generator_->m_AllowedError = config.max_error;
        traj_generator_->overlap_time = config.overlap_time;
        
        switch(config.operation_mode)
        {
          case 0:  //"undefined"
            this->current_operation_mode_ = "undefined";
            break;
          case 1:  //"velocity"
            this->current_operation_mode_ = "velocity";
            break;
          case 2:  //"position"
            this->current_operation_mode_ = "position";
            break;
          default:
            ROS_ERROR("Unknown operation_mode");
            this->current_operation_mode_ = "undefined";
            break;            
        }
    }

    double getFrequency()
    {
        double frequency;
        if (n_.hasParam("frequency"))                                                                   
        {                                                                                                     
            n_.getParam("frequency", frequency);                                                              
            ROS_INFO("Setting controller frequency to %f HZ", frequency);                                       
        }                                                                                                     
        else                                                                                                    
        {                                                                                                     
            frequency = 100; //Hz                                                                               
            ROS_WARN("Parameter frequency not available, setting to default value: %f Hz", frequency);          
        }
        return frequency;
    }

    bool srvCallback_Stop(cob_srvs::Trigger::Request &req, cob_srvs::Trigger::Response &res)
    {
        ROS_INFO("Stopping trajectory controller.");
        
        // stop trajectory controller
        executing_ = false;
        res.success.data = true;
        traj_generator_->isMoving = false;
        //as_.setPreemted();
        failure_ = true;
        return true;
    }
    
    bool srvCallback_setVel(cob_trajectory_controller::SetFloat::Request &req, cob_trajectory_controller::SetFloat::Response &res)
    {
        ROS_INFO("Setting velocity to %f", req.value.data);
        traj_generator_->SetPTPvel(req.value.data);
        res.success.data = true;
        return true;
    }
    
    bool srvCallback_setAcc(cob_trajectory_controller::SetFloat::Request &req, cob_trajectory_controller::SetFloat::Response &res)
    {
        ROS_INFO("Setting acceleration to %f", req.value.data);
        traj_generator_->SetPTPacc(req.value.data);
        res.success.data = true;
        return true;
    }

    void operationmode_callback(const std_msgs::StringPtr& message)
    {
        ROS_INFO("Setting operation_mode: %s", (message->data).c_str());
        current_operation_mode_ = message->data;
    }
    
    void state_callback(const control_msgs::JointTrajectoryControllerStatePtr& message)
    {
        std::vector<double> positions = message->actual.positions;
        for(unsigned int i = 0; i < positions.size(); i++)
        {
            q_current[i] = positions[i];
        }
    }
    
    void joint_state_callback(const sensor_msgs::JointStatePtr& message)
    {
        for(unsigned int i = 0; i < message->name.size(); i++)
        {
            for(unsigned int j = 0; j < DOF; j++)
            {
                if(message->name[i]==JointNames_[j])
                    q_current[j] = message->position[i];
            }
        }
    }

    void spawnTrajector(trajectory_msgs::JointTrajectory trajectory)
    {
        if(!executing_ || preemted_)
        {
            //set component to velocity mode
            cob_srvs::SetOperationMode opmode;
            opmode.request.operation_mode.data = "velocity";
            srvClient_SetOperationMode.call(opmode);
            ros::Time begin = ros::Time::now();
            while(current_operation_mode_ != "velocity")
            {
                ROS_INFO("waiting for component to go to velocity mode");
                usleep(100000);
                //add timeout and set action to rejected
                if((ros::Time::now() - begin).toSec() > velocity_timeout_)
                {
                    rejected_ = true;
                    return;
                }  
            }
            
            std::vector<double> traj_start;
            if(preemted_ == true) //Calculate trajectory for runtime modification of trajectories
            {
                ROS_INFO("There is a old trajectory currently running");
                traj_start = traj_generator_->last_q;
                trajectory_msgs::JointTrajectory temp_traj;
                temp_traj = trajectory;
                //Insert the saved point as first point of trajectory, then generate SPLINE trajectory
                trajectory_msgs::JointTrajectoryPoint p;
                p.positions.resize(DOF);
                p.velocities.resize(DOF);
                p.accelerations.resize(DOF);
                for(int i = 0; i<DOF; i++)
                {
                    p.positions.at(i) = traj_start.at(i);
                    p.velocities.at(i) = 0.0;
                    p.accelerations.at(i) = 0.0;
                }
                std::vector<trajectory_msgs::JointTrajectoryPoint>::iterator it;
                it = temp_traj.points.begin();
                temp_traj.points.insert(it,p);
                //Now insert the current as first point of trajectory, then generate SPLINE trajectory
                for(int i = 0; i<DOF; i++)
                {
                    p.positions.at(i) = traj_generator_->last_q1.at(i);
                    p.velocities.at(i) = 0.0;
                    p.accelerations.at(i) = 0.0;
                }
                it = temp_traj.points.begin();
                temp_traj.points.insert(it,p);
                for(int i = 0; i<DOF; i++)
                {
                    p.positions.at(i) = traj_generator_->last_q2.at(i);
                    p.velocities.at(i) = 0.0;
                    p.accelerations.at(i) = 0.0;
                }
                it = temp_traj.points.begin();
                temp_traj.points.insert(it,p);
                for(int i = 0; i<DOF; i++)
                {
                    p.positions.at(i) = traj_generator_->last_q3.at(i);
                    p.velocities.at(i) = 0.0;
                    p.accelerations.at(i) = 0.0;
                }
                it = temp_traj.points.begin();
                temp_traj.points.insert(it,p);
                for(int i = 0; i<DOF; i++)
                {
                    p.positions.at(i) = q_current.at(i);
                    p.velocities.at(i) = 0.0;
                    p.accelerations.at(i) = 0.0;
                }
                it = temp_traj.points.begin();
                temp_traj.points.insert(it,p);
                traj_generator_->isMoving = false ;
                traj_generator_->moveTrajectory(temp_traj, traj_start);
            }
            else //Normal calculation of trajectories
            {
                traj_start = q_current;
                trajectory_msgs::JointTrajectory temp_traj;
                temp_traj = trajectory;
                if(temp_traj.points.size() == 1)
                {
                    traj_generator_->isMoving = false ;
                    traj_generator_->moveThetas(temp_traj.points[0].positions, traj_start);
                }
                else
                {
                    //Insert the current point as first point of trajectory, then generate SPLINE trajectory
                    trajectory_msgs::JointTrajectoryPoint p;
                    p.positions.resize(DOF);
                    p.velocities.resize(DOF);
                    p.accelerations.resize(DOF);
                    for(int i = 0; i<DOF; i++)
                    {
                        p.positions.at(i) = traj_start.at(i);
                        p.velocities.at(i) = 0.0;
                        p.accelerations.at(i) = 0.0;
                    }
                    std::vector<trajectory_msgs::JointTrajectoryPoint>::iterator it;
                    it = temp_traj.points.begin();
                    temp_traj.points.insert(it,p);
                    traj_generator_->isMoving = false ;
                    traj_generator_->moveTrajectory(temp_traj, traj_start);
                }
            }
            
            executing_ = true;
            startposition_ = q_current;
            preemted_ = false;
            
        }
        else //suspend current movement and start new one
        {
        }
        while(executing_)
        {
            if(!preemted_)
            {
                usleep(1000);
            }
            else
            {
                return;
            }
        }
    }


    void executeFollowTrajectory(const control_msgs::FollowJointTrajectoryGoalConstPtr &goal) 
    {
        ROS_INFO("Received new goal trajectory with %lu points",goal->trajectory.points.size());
        spawnTrajector(goal->trajectory);
        // only set to succeeded if component could reach position. this is currently not the care for e.g. by emergency stop, hardware error or exceeds limit.
        if(rejected_)
            as_follow_.setAborted(); //setRejected not implemented in simpleactionserver ?
        else
        {
            if(failure_)
                as_follow_.setAborted();
            else
                as_follow_.setSucceeded();
        }
        rejected_ = false;
        failure_ = false;
    }
    
    void run()
    {
        if(executing_)
        {
            failure_ = false;
            watchdog_counter = 0;
            //if (as_follow_.isPreemptRequested() || !ros::ok() || current_operation_mode_ != "velocity")
            if (!ros::ok() || current_operation_mode_ != "velocity")
            {
                // set the action state to preempted
                executing_ = false;
                traj_generator_->isMoving = false;
                //as_.setPreempted();
                failure_ = true;
                return;
            }
            if (as_follow_.isPreemptRequested())
            {
                //as_follow_.setAborted()
                failure_ = true;
                preemted_ = true;
                ROS_INFO("Preempted trajectory action");
                return;
            }
            std::vector<double> des_vel;
            if(traj_generator_->step(q_current, des_vel))
            {
                if(!traj_generator_->isMoving) //Finished trajectory
                {
                    executing_ = false;
                    preemted_ = false;
                }
                brics_actuator::JointVelocities target_joint_vel;
                target_joint_vel.velocities.resize(DOF);
                for(int i=0; i<DOF; i++)
                {
                    target_joint_vel.velocities[i].joint_uri = JointNames_[i].c_str();
                    target_joint_vel.velocities[i].unit = "rad";
                    target_joint_vel.velocities[i].value = des_vel.at(i);
                }
                
                //send everything
                joint_vel_pub_.publish(target_joint_vel);
            }
            else
            {
                ROS_INFO("An controller error occured!");
                failure_ = true;
                executing_ = false;
            }
        }
        else
        {  //WATCHDOG TODO: don't always send
            if(watchdog_counter < 10)
            {
                brics_actuator::JointVelocities target_joint_vel;
                target_joint_vel.velocities.resize(DOF);
                for (int i = 0; i < DOF; i += 1)
                {
                    target_joint_vel.velocities[i].joint_uri = JointNames_[i].c_str();
                    target_joint_vel.velocities[i].unit = "rad";
                    target_joint_vel.velocities[i].value = 0;
                }
                joint_vel_pub_.publish(target_joint_vel);
            }
            watchdog_counter++;
        }
    }
    
};



int main(int argc, char ** argv)
{
    ros::init(argc, argv, "cob_trajectory_controller");

    cob_trajectory_controller_node tm;

    /// get main loop parameters
    double frequency = tm.getFrequency();

    ros::Rate loop_rate(frequency);
    while (ros::ok())
    {
        tm.run();
        ros::spinOnce();
        loop_rate.sleep();
    }
}




