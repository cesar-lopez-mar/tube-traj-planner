/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Eitan Marder-Eppstein
*********************************************************************/

#include <base_local_planner/trajectory_planner.h>
#include <costmap_2d/footprint.h>
#include <string>
#include <sstream>
#include <math.h>
#include <angles/angles.h>



#include <boost/algorithm/string.hpp>

#include <ros/console.h>

//for computing path distance
#include <queue>

using namespace std;
using namespace costmap_2d;

namespace base_local_planner{

  void TrajectoryPlanner::reconfigure(BaseLocalPlannerConfig &cfg)
  {
      BaseLocalPlannerConfig config(cfg);

      boost::mutex::scoped_lock l(configuration_mutex_);

      acc_lim_x_ = config.acc_lim_x;
      acc_lim_y_ = config.acc_lim_y;
      acc_lim_theta_ = config.acc_lim_theta;

      max_vel_x_ = config.max_vel_x;
      min_vel_x_ = config.min_vel_x;

      max_vel_y_ = config.max_vel_y;
      min_vel_y_ = config.min_vel_y;

      max_vel_th_ = config.max_vel_theta;
      min_vel_th_ = config.min_vel_theta;
      min_in_place_vel_th_ = config.min_in_place_vel_theta;

      sim_time_ = config.sim_time;
      sim_granularity_ = config.sim_granularity;
      angular_sim_granularity_ = config.angular_sim_granularity;

      pdist_scale_ = config.pdist_scale;
      gdist_scale_ = config.gdist_scale;
      occdist_scale_ = config.occdist_scale;
      hdiff_scale_ = config.hdiff_scale;
      path_distance_max_ = config.path_distance_max;

      if (meter_scoring_) {
        //if we use meter scoring, then we want to multiply the biases by the resolution of the costmap
        double resolution = costmap_.getResolution();
        gdist_scale_ *= resolution;
        pdist_scale_ *= resolution;
        occdist_scale_ *= resolution;
      }

      oscillation_reset_dist_ = config.oscillation_reset_dist;
      escape_reset_dist_ = config.escape_reset_dist;
      escape_reset_theta_ = config.escape_reset_theta;

      vx_samples_ = config.vx_samples;
      vy_samples_ = config.vy_samples;
      vtheta_samples_ = config.vtheta_samples;

      if (vx_samples_ <= 0) {
          config.vx_samples = 1;
          vx_samples_ = config.vx_samples;
          ROS_WARN("You've specified that you don't want any samples in the x dimension. We'll at least assume that you want to sample one value... so we're going to set vx_samples to 1 instead");
      }
      if(vtheta_samples_ <= 0) {
          config.vtheta_samples = 1;
          vtheta_samples_ = config.vtheta_samples;
          ROS_WARN("You've specified that you don't want any samples in the theta dimension. We'll at least assume that you want to sample one value... so we're going to set vtheta_samples to 1 instead");
      }

      heading_lookahead_ = config.heading_lookahead;

      holonomic_robot_ = config.holonomic_robot;

      backup_vel_ = config.escape_vel;

      dwa_ = config.dwa;

      heading_scoring_ = config.heading_scoring;
      heading_scoring_timestep_ = config.heading_scoring_timestep;

      simple_attractor_ = config.simple_attractor;

      //y-vels
      string y_string = config.y_vels;
      vector<string> y_strs;
      boost::split(y_strs, y_string, boost::is_any_of(", "), boost::token_compress_on);

      vector<double>y_vels;
      for(vector<string>::iterator it=y_strs.begin(); it != y_strs.end(); ++it) {
          istringstream iss(*it);
          double temp;
          iss >> temp;
          y_vels.push_back(temp);
          //ROS_INFO("Adding y_vel: %e", temp);
      }

      y_vels_ = y_vels;

  }

  TrajectoryPlanner::TrajectoryPlanner(WorldModel& world_model,
      const Costmap2D& costmap,
      std::vector<geometry_msgs::Point> footprint_spec,
      double acc_lim_x, double acc_lim_y, double acc_lim_theta,
      double sim_time, double sim_granularity,
      int vx_samples, int vtheta_samples,
      double pdist_scale, double gdist_scale, double occdist_scale,
      double hdiff_scale,
      double heading_lookahead, double oscillation_reset_dist,
      double escape_reset_dist, double escape_reset_theta,
      bool holonomic_robot,
      double max_vel_x, double min_vel_x,
      double max_vel_th, double min_vel_th, double min_in_place_vel_th,
      double backup_vel,
      bool dwa, bool heading_scoring, double heading_scoring_timestep, bool meter_scoring, bool simple_attractor,
      vector<double> y_vels, double stop_time_buffer, double sim_period, double angular_sim_granularity, double path_distance_max)
    : path_map_(costmap.getSizeInCellsX(), costmap.getSizeInCellsY()),
      goal_map_(costmap.getSizeInCellsX(), costmap.getSizeInCellsY()),
      costmap_(costmap),
    world_model_(world_model), footprint_spec_(footprint_spec),
    sim_time_(sim_time), sim_granularity_(sim_granularity), angular_sim_granularity_(angular_sim_granularity),
    vx_samples_(vx_samples), vtheta_samples_(vtheta_samples),
    pdist_scale_(pdist_scale), gdist_scale_(gdist_scale), occdist_scale_(occdist_scale),
    hdiff_scale_(hdiff_scale),
    acc_lim_x_(acc_lim_x), acc_lim_y_(acc_lim_y), acc_lim_theta_(acc_lim_theta),
    prev_x_(0), prev_y_(0), escape_x_(0), escape_y_(0), escape_theta_(0), heading_lookahead_(heading_lookahead),
    oscillation_reset_dist_(oscillation_reset_dist), escape_reset_dist_(escape_reset_dist),
    escape_reset_theta_(escape_reset_theta), holonomic_robot_(holonomic_robot),
    max_vel_x_(max_vel_x), min_vel_x_(min_vel_x),
    max_vel_th_(max_vel_th), min_vel_th_(min_vel_th), min_in_place_vel_th_(min_in_place_vel_th),
    backup_vel_(backup_vel),
    dwa_(dwa), heading_scoring_(heading_scoring), heading_scoring_timestep_(heading_scoring_timestep),
    simple_attractor_(simple_attractor), y_vels_(y_vels), stop_time_buffer_(stop_time_buffer), sim_period_(sim_period), path_distance_max_(path_distance_max)
  {
    //the robot is not stuck to begin with
    stuck_left = false;
    stuck_right = false;
    stuck_left_strafe = false;
    stuck_right_strafe = false;
    rotating_left = false;
    rotating_right = false;
    strafe_left = false;
    strafe_right = false;

    escaping_ = false;
    final_goal_position_valid_ = false;


    costmap_2d::calculateMinAndMaxDistances(footprint_spec_, inscribed_radius_, circumscribed_radius_);
  }

  TrajectoryPlanner::~TrajectoryPlanner(){}

  bool TrajectoryPlanner::getCellCosts(int cx, int cy, float &path_cost, float &goal_cost, float &occ_cost, float &total_cost) {
    MapCell cell = path_map_(cx, cy);
    MapCell goal_cell = goal_map_(cx, cy);
    if (cell.within_robot) {
        return false;
    }
    occ_cost = costmap_.getCost(cx, cy);
    if (cell.target_dist == path_map_.obstacleCosts() ||
        cell.target_dist == path_map_.unreachableCellCosts() ||
        occ_cost >= costmap_2d::INSCRIBED_INFLATED_OBSTACLE) {
        return false;
    }
    path_cost = cell.target_dist;
    goal_cost = goal_cell.target_dist;
    total_cost = pdist_scale_ * path_cost + gdist_scale_ * goal_cost + occdist_scale_ * occ_cost;
    return true;
  }

  /**
   * create and score a trajectory given the current pose of the robot and selected velocities
   */
  void TrajectoryPlanner::generateTrajectory(
      double x, double y, double theta,
      double vx, double vy, double vtheta,
      double vx_samp, double vy_samp, double vtheta_samp,
      double acc_x, double acc_y, double acc_theta,
      double impossible_cost,
      Trajectory& traj) {

    // make sure the configuration doesn't change mid run
    boost::mutex::scoped_lock l(configuration_mutex_);

    double x_i = x;
    double y_i = y;
    double theta_i = theta;

    double vx_i, vy_i, vtheta_i;

    vx_i = vx;
    vy_i = vy;
    vtheta_i = vtheta;

    //compute the magnitude of the velocities
    double vmag = hypot(vx_samp, vy_samp);

    traj.path_dist_traj_ = -2.0;
    //compute the number of steps we must take along this trajectory to be "safe"
    int num_steps;
    if(!heading_scoring_) {
      num_steps = int(max((vmag * sim_time_) / sim_granularity_, fabs(vtheta_samp) / angular_sim_granularity_) + 0.5);
    } else {
      num_steps = int(sim_time_ / sim_granularity_ + 0.5);
    }

    //we at least want to take one step... even if we won't move, we want to score our current position
    if(num_steps == 0) {
      num_steps = 1;
    }

    double dt = sim_time_ / num_steps;
    double time = 0.0;

    //create a potential trajectory
    traj.resetPoints();
    traj.xv_ = vx_samp;
    traj.yv_ = vy_samp;
    traj.thetav_ = vtheta_samp;
    traj.cost_ = -3.0;

    //initialize the costs for the trajectory
    double path_dist = 0.0;
    double goal_dist = 0.0;
    double occ_cost = 0.0;
    double heading_diff = 0.0;

    for(int i = 0; i < num_steps; ++i){
      //get map coordinates of a point
      unsigned int cell_x, cell_y;

      //we don't want a path that goes off the know map
      if(!costmap_.worldToMap(x_i, y_i, cell_x, cell_y)){
        traj.cost_ = -4.0;
        return;
      }

      //check the point on the trajectory for legality
      double footprint_cost = footprintCost(x_i, y_i, theta_i);

      //if the footprint hits an obstacle this trajectory is invalid
      if(footprint_cost < 0){
        traj.cost_ = -5.0;
        return;
        //TODO: Really look at getMaxSpeedToStopInTime... dues to discretization errors and high acceleration limits,
        //it can actually cause the robot to hit obstacles. There may be something to be done to fix, but I'll have to
        //come back to it when I have time. Right now, pulling it out as it'll just make the robot a bit more conservative,
        //but safe.
        /*
        double max_vel_x, max_vel_y, max_vel_th;
        //we want to compute the max allowable speeds to be able to stop
        //to be safe... we'll make sure we can stop some time before we actually hit
        getMaxSpeedToStopInTime(time - stop_time_buffer_ - dt, max_vel_x, max_vel_y, max_vel_th);

        //check if we can stop in time
        if(fabs(vx_samp) < max_vel_x && fabs(vy_samp) < max_vel_y && fabs(vtheta_samp) < max_vel_th){
          ROS_ERROR("v: (%.2f, %.2f, %.2f), m: (%.2f, %.2f, %.2f) t:%.2f, st: %.2f, dt: %.2f", vx_samp, vy_samp, vtheta_samp, max_vel_x, max_vel_y, max_vel_th, time, stop_time_buffer_, dt);
          //if we can stop... we'll just break out of the loop here.. no point in checking future points
          break;
        }
        else{
          traj.cost_ = -1.0;
          return;
        }
        */
      }

      occ_cost = std::max(std::max(occ_cost, footprint_cost), double(costmap_.getCost(cell_x, cell_y)));

      //do we want to follow blindly
      if (simple_attractor_) {
        goal_dist = (x_i - global_plan_[global_plan_.size() -1].pose.position.x) *
          (x_i - global_plan_[global_plan_.size() -1].pose.position.x) +
          (y_i - global_plan_[global_plan_.size() -1].pose.position.y) *
          (y_i - global_plan_[global_plan_.size() -1].pose.position.y);
      } else {

        bool update_path_and_goal_distances = false;

        // with heading scoring, we take into account heading diff, and also only score
        // path and goal distance for one point of the trajectory
//         if (i == (num_steps-1) && heading_scoring_) {
        if (i == (num_steps-1) && heading_scoring_) {
//           if (time >= heading_scoring_timestep_ && time < heading_scoring_timestep_ + dt) {
            heading_diff = headingDiff(cell_x, cell_y, x_i, y_i, theta_i, goal_dist, path_dist);
//           } else {
//             update_path_and_goal_distances = false;
//           }
            update_path_and_goal_distances = true;
        }else if(!heading_scoring_)
        {
            update_path_and_goal_distances = true;
        }

        if (update_path_and_goal_distances) {
          //update path and goal distances

          if(!heading_scoring_)
          {
            path_dist = path_map_(cell_x, cell_y).target_dist;
            goal_dist = goal_map_(cell_x, cell_y).target_dist;
          }

          //if a point on this trajectory has no clear path to goal it is invalid
          if(impossible_cost <= goal_dist || impossible_cost <= path_dist){
//            ROS_DEBUG("No path to goal with goal distance = %f, path_distance = %f and max cost = %f",
//                goal_dist, path_dist, impossible_cost);
            traj.cost_ = -2.0;
            return;
          }
          //ROS_INFO("path_dist %f",path_dist);
          double path_dist_internal = (double) path_dist;

          if ( meter_scoring_ )
              //path_dist_internal *= costmap_.getResolution();

//           if(path_distance_max_ > 0.0 && !rotating_left && !rotating_right && path_dist_internal > path_distance_max_){
          traj.path_dist_traj_ = path_dist_internal;
          if(path_distance_max_ > 0.0 && path_dist_internal <= path_distance_max_){
               path_dist = 0.0;
//              traj.cost_ = -3.0;
//             return;
          }
          else
          {
            //path_dist*=100.0;
            //traj.cost_ = -9.0;
            //traj.goal_cost_traj_ = 1e3;
            //return;
          }

          if(fabs(heading_diff)<0.2)
            heading_diff = 0.0;
        }
      }


      //the point is legal... add it to the trajectory
      traj.addPoint(x_i, y_i, theta_i);

      //calculate velocities
      vx_i = computeNewVelocity(vx_samp, vx_i, acc_x, dt);
      vy_i = computeNewVelocity(vy_samp, vy_i, acc_y, dt);
      vtheta_i = computeNewVelocity(vtheta_samp, vtheta_i, acc_theta, dt);

      //calculate positions
      x_i = computeNewXPosition(x_i, vx_i, vy_i, theta_i, dt);
      y_i = computeNewYPosition(y_i, vx_i, vy_i, theta_i, dt);
      theta_i = computeNewThetaPosition(theta_i, vtheta_i, dt);

      //increment time
      time += dt;
    } // end for i < numsteps

    //ROS_INFO("OccCost: %f, vx: %.2f, vy: %.2f, vtheta: %.2f", occ_cost, vx_samp, vy_samp, vtheta_samp);
    double cost;
    if (!heading_scoring_) {
      cost = pdist_scale_ * path_dist + goal_dist * gdist_scale_ + occdist_scale_ * occ_cost;
    } else {
      cost = occdist_scale_ * occ_cost + pdist_scale_ * path_dist + heading_diff * hdiff_scale_ + goal_dist * gdist_scale_;
    }


    occ_dist_  =  occ_cost;
    occ_cost_  =  occdist_scale_ * occ_cost;

    path_dist_ = path_dist;
    path_cost_ = pdist_scale_ * path_dist;

    head_diff_ = heading_diff;
    head_cost_ =  heading_diff * hdiff_scale_;

    goal_dist_ = goal_dist;
    goal_cost_ = goal_dist * gdist_scale_;


    traj.cost_ = cost;
    traj.goal_cost_traj_ = goal_cost_;

  }

  double TrajectoryPlanner::headingDiff(int cell_x, int cell_y, double x, double y, double heading, double &goal_dist_traj, double &path_dist_traj){
    unsigned int goal_cell_x, goal_cell_y;

    // find closest current position to global plan and take the heading from there
    double dist_to_path_min = 1e3;
    double dist_to_path;
    double dist_to_goal = 0.0;
    std::vector<double> dist_to_goal_v(global_plan_.size()) ;
    tf::Pose pose_temp;
    tf::Quaternion quat_temp;
    int look_ahead_samples  = 1;
    int index_plan;
    int i_curr_loc = 0;;
    double yaw, pitch, roll;
    dist_to_goal_v[global_plan_.size() - 1] = 0.0;
    for (int i = global_plan_.size() - 2; i >=0; --i) {
        dist_to_goal = dist_to_goal + hypot(global_plan_[i].pose.position.x-global_plan_[i+1].pose.position.x,
                                            global_plan_[i].pose.position.y-global_plan_[i+1].pose.position.y);
        dist_to_goal_v[i] = dist_to_goal;
        dist_to_path = hypot(global_plan_[i].pose.position.x-x,global_plan_[i].pose.position.y-y);
        if(dist_to_path < dist_to_path_min){
            dist_to_path_min = dist_to_path;
             i_curr_loc = i;
        }
    }

    index_plan = std::min<int>(i_curr_loc + look_ahead_samples, global_plan_.size() - 1);
    quat_temp.setW(global_plan_[index_plan].pose.orientation.w);
    quat_temp.setX(global_plan_[index_plan].pose.orientation.x);
    quat_temp.setY(global_plan_[index_plan].pose.orientation.y);
    quat_temp.setZ(global_plan_[index_plan].pose.orientation.z);
    pose_temp.setRotation(quat_temp);
    pose_temp.getBasis().getEulerYPR(yaw, pitch, roll);

    goal_dist_traj = dist_to_goal_v[index_plan]+ ( (double)(global_plan_.size()-1-index_plan) )/global_plan_.size();
    if(goal_dist_traj == 0.0)
    {
        goal_dist_traj = hypot(global_plan_[global_plan_.size() - 1].pose.position.x-x,global_plan_[global_plan_.size() - 1].pose.position.y-y);
    }

    path_dist_traj = dist_to_path_min;


    //ROS_INFO("READ HEADING: %f, %f %d, %d\n", heading, yaw, global_plan_.size(), i_curr_loc);
    angle1_ = heading;
    angle2_ = yaw;
    return fabs(AngleDifference(heading, yaw) );

    //if ( index_plan > i && costmap_.worldToMap(global_plan_[index_plan].pose.position.x, global_plan_[index_plan].pose.position.y, goal_cell_x, goal_cell_y) ) {
        //double gx, gy;
        //costmap_.mapToWorld(goal_cell_x, goal_cell_y, gx, gy);
        //ROS_WARN("READ HEADING MIDDLE: %f, %f\n", heading, atan2(global_plan_[index_plan].pose.position.y - global_plan_[i].pose.position.y, global_plan_[index_plan].pose.position.x - global_plan_[i].pose.position.x) );
        ////return fabs(angles::shortest_angular_distance(heading, atan2(gy - y, gx - x)));
        //return fabs(AngleDifference(heading, atan2(global_plan_[index_plan].pose.position.y - global_plan_[i].pose.position.y, global_plan_[index_plan].pose.position.x - global_plan_[i].pose.position.x)));
    //}else{
        //ROS_WARN("READ HEADING REACHED END: %f, %f\n", heading, yaw);
        //return fabs(AngleDifference(heading, yaw) );
    //}


  }
double TrajectoryPlanner::AngleDifference( double angle1, double angle2 )
{
    return fabs(angles::shortest_angular_distance(angle1, angle2));
}

  //calculate the cost of a ray-traced line
  double TrajectoryPlanner::lineCost(int x0, int x1,
      int y0, int y1){
    //Bresenham Ray-Tracing
    int deltax = abs(x1 - x0);        // The difference between the x's
    int deltay = abs(y1 - y0);        // The difference between the y's
    int x = x0;                       // Start x off at the first pixel
    int y = y0;                       // Start y off at the first pixel

    int xinc1, xinc2, yinc1, yinc2;
    int den, num, numadd, numpixels;

    double line_cost = 0.0;
    double point_cost = -6.0;

    if (x1 >= x0)                 // The x-values are increasing
    {
      xinc1 = 1;
      xinc2 = 1;
    }
    else                          // The x-values are decreasing
    {
      xinc1 = -1;
      xinc2 = -1;
    }

    if (y1 >= y0)                 // The y-values are increasing
    {
      yinc1 = 1;
      yinc2 = 1;
    }
    else                          // The y-values are decreasing
    {
      yinc1 = -1;
      yinc2 = -1;
    }

    if (deltax >= deltay)         // There is at least one x-value for every y-value
    {
      xinc1 = 0;                  // Don't change the x when numerator >= denominator
      yinc2 = 0;                  // Don't change the y for every iteration
      den = deltax;
      num = deltax / 2;
      numadd = deltay;
      numpixels = deltax;         // There are more x-values than y-values
    } else {                      // There is at least one y-value for every x-value
      xinc2 = 0;                  // Don't change the x for every iteration
      yinc1 = 0;                  // Don't change the y when numerator >= denominator
      den = deltay;
      num = deltay / 2;
      numadd = deltax;
      numpixels = deltay;         // There are more y-values than x-values
    }

    for (int curpixel = 0; curpixel <= numpixels; curpixel++) {
      point_cost = pointCost(x, y); //Score the current point

      if (point_cost < 0) {
        return -1;
      }

      if (line_cost < point_cost) {
        line_cost = point_cost;
      }

      num += numadd;              // Increase the numerator by the top of the fraction
      if (num >= den) {           // Check if numerator >= denominator
        num -= den;               // Calculate the new numerator value
        x += xinc1;               // Change the x as appropriate
        y += yinc1;               // Change the y as appropriate
      }
      x += xinc2;                 // Change the x as appropriate
      y += yinc2;                 // Change the y as appropriate
    }

    return line_cost;
  }

  double TrajectoryPlanner::pointCost(int x, int y){
    unsigned char cost = costmap_.getCost(x, y);
    //if the cell is in an obstacle the path is invalid
    if(cost == LETHAL_OBSTACLE || cost == INSCRIBED_INFLATED_OBSTACLE || cost == NO_INFORMATION){
      return -1;
    }

    return cost;
  }

  void TrajectoryPlanner::updatePlan(const vector<geometry_msgs::PoseStamped>& new_plan, bool compute_dists){
    global_plan_.resize(new_plan.size());
    for(unsigned int i = 0; i < new_plan.size(); ++i){
      global_plan_[i] = new_plan[i];
    }

    if( global_plan_.size() > 0 ){
      geometry_msgs::PoseStamped& final_goal_pose = global_plan_[ global_plan_.size() - 1 ];
      final_goal_x_ = final_goal_pose.pose.position.x;
      final_goal_y_ = final_goal_pose.pose.position.y;
      final_goal_position_valid_ = true;
    } else {
      final_goal_position_valid_ = false;
    }

    if (compute_dists) {
      //reset the map for new operations
      path_map_.resetPathDist();
      goal_map_.resetPathDist();

      //make sure that we update our path based on the global plan and compute costs
      path_map_.setTargetCells(costmap_, global_plan_);
      goal_map_.setLocalGoal(costmap_, global_plan_);
      ROS_DEBUG("Path/Goal distance computed");
    }
  }

  bool TrajectoryPlanner::checkTrajectory(double x, double y, double theta, double vx, double vy,
      double vtheta, double vx_samp, double vy_samp, double vtheta_samp){
    Trajectory t;

    double cost = scoreTrajectory(x, y, theta, vx, vy, vtheta, vx_samp, vy_samp, vtheta_samp);

    //if the trajectory is a legal one... the check passes
    if(cost >= 0) {
      return true;
    }
    ROS_WARN("Invalid Trajectory %f, %f, %f, cost: %f", vx_samp, vy_samp, vtheta_samp, cost);

    //otherwise the check fails
    return false;
  }

  double TrajectoryPlanner::scoreTrajectory(double x, double y, double theta, double vx, double vy,
      double vtheta, double vx_samp, double vy_samp, double vtheta_samp) {
    Trajectory t;
    double impossible_cost = path_map_.obstacleCosts();
    generateTrajectory(x, y, theta,
                       vx, vy, vtheta,
                       vx_samp, vy_samp, vtheta_samp,
                       acc_lim_x_, acc_lim_y_, acc_lim_theta_,
                       impossible_cost, t);

    // return the cost.
    return double( t.cost_ );
  }

  /*
   * create the trajectories we wish to score
   */
  Trajectory TrajectoryPlanner::createTrajectories(double x, double y, double theta,
      double vx, double vy, double vtheta,
      double acc_x, double acc_y, double acc_theta) {
    //compute feasible velocity limits in robot space
    double max_vel_x = max_vel_x_, max_vel_y = max_vel_y_, max_vel_theta;
    double min_vel_x, min_vel_y, min_vel_theta;

    if( final_goal_position_valid_ ){
      double final_goal_dist = hypot( final_goal_x_ - x, final_goal_y_ - y );
      max_vel_x = min( max_vel_x, final_goal_dist / sim_time_ );
      min_vel_x = min( min_vel_x_, max_vel_x);
      max_vel_y = min( max_vel_y, final_goal_dist / sim_time_ );
      min_vel_y = -max_vel_y;
    }

    //should we use the dynamic window approach?
    if (dwa_) {
      max_vel_x = max(min(max_vel_x, vx + acc_x * sim_period_), min_vel_x_);
      min_vel_x = max(min_vel_x, vx - acc_x * sim_period_);

      max_vel_y = min(max_vel_y, vx + acc_y * sim_period_);
      min_vel_y = max(min_vel_y, vx - acc_y * sim_period_);

      max_vel_theta = min(max_vel_th_, vtheta + acc_theta * sim_period_);
      min_vel_theta = max(min_vel_th_, vtheta - acc_theta * sim_period_);
    } else {
      max_vel_x = max(min(max_vel_x, vx + acc_x * sim_time_), min_vel_x_);
      //min_vel_x = max(min_vel_x, vx - acc_x * sim_time_);

      //max_vel_y = min(max_vel_y, vx + acc_y * sim_time_);
      //min_vel_y = max(min_vel_y, vx - acc_y * sim_time_);

      max_vel_theta = min(max_vel_th_, vtheta + acc_theta * sim_time_);
      min_vel_theta = max(min_vel_th_, vtheta - acc_theta * sim_time_);
    }


    //we want to sample the velocity space regularly
    double dvx = (max_vel_x - min_vel_x) / (vx_samples_ - 1);
    double dvy = (max_vel_y - min_vel_y) / (vy_samples_ - 1);
    double dvtheta = (max_vel_theta - min_vel_theta) / (vtheta_samples_ - 1);

    double vx_samp = min_vel_x;
    double vtheta_samp = min_vel_theta;
    double vy_samp = 0.0;

    //keep track of the best trajectory seen so far
    Trajectory* best_traj = &traj_one;
    best_traj->cost_ = -1.0;

    Trajectory* comp_traj = &traj_two;
    comp_traj->cost_ = -1.0;

    Trajectory* swap = NULL;


    //any cell with a cost greater than the size of the map is impossible
    double impossible_cost = path_map_.obstacleCosts();

    printf("\n\n\n\n Start searching velocities");


// Compute a reference cost, i.e. the current position. If the new traj do not make progress then we do not take it into account
    Trajectory current_pos_traj;
    generateTrajectory(x, y, theta, vx, vy, vtheta, 0, 0, 0,
            acc_x, acc_y, acc_theta, impossible_cost, current_pos_traj);



    //if we're performing an escape we won't allow moving forward
    if (true) {//{ Cesar
//    if (!escaping_) {
      //loop through all x velocities
      for(int i = 0; i < vx_samples_; ++i) {
        vtheta_samp = 0;
        //first sample the straight trajectory
        generateTrajectory(x, y, theta, vx, vy, vtheta, vx_samp, vy_samp, vtheta_samp,
            acc_x, acc_y, acc_theta, impossible_cost, *comp_traj);

        //if the new trajectory is better... let's take it
        if(comp_traj->cost_ >= 0 && (comp_traj->cost_ < best_traj->cost_ || best_traj->cost_ < 0)
           && comp_traj-> goal_cost_traj_ < current_pos_traj.goal_cost_traj_){
          swap = best_traj;
          best_traj = comp_traj;
          comp_traj = swap;
          /*
          printf("\nFound good traj in Vx only sampling vx_samp, vy_samp, vtheta_samp %f, %f, %f\n", vx_samp, vy_samp, vtheta_samp);
          printf("Test vx_samp, vy_samp, vtheta_samp: %f, %f, %f \n", vx_samp, vy_samp, vtheta_samp);
          printf("Dists: Goal %f / Path %f / Heading %f / Occ %f \n", goal_dist_, path_dist_, head_diff_, occ_dist_);
          printf("Costs: Total %f / Goal %f / Path %f / Heading %f / Occ %f \n",best_traj->cost_, goal_cost_, path_cost_, head_cost_, occ_cost_);
          printf("Diff heading, yaw %f, %f \n", angle1_, angle2_);
          */
        }

        vtheta_samp = min_vel_theta;
        //next sample all theta trajectories

        for(int j = 0; j < vtheta_samples_ - 1; ++j){
          generateTrajectory(x, y, theta, vx, vy, vtheta, vx_samp, vy_samp, vtheta_samp,
              acc_x, acc_y, acc_theta, impossible_cost, *comp_traj);

          //if the new trajectory is better... let's take it
          if(comp_traj->cost_ >= 0 && (comp_traj->cost_ < best_traj->cost_ || best_traj->cost_ < 0)
             && comp_traj-> goal_cost_traj_ < current_pos_traj.goal_cost_traj_){
            swap = best_traj;
            best_traj = comp_traj;
            comp_traj = swap;
            /*
            printf("\nFound good traj in Vx/ Theta sampling vx_samp, vy_samp, vtheta_samp %f, %f, %f\n", vx_samp, vy_samp, vtheta_samp);
            printf("Test vx_samp, vy_samp, vtheta_samp: %f, %f, %f \n", vx_samp, vy_samp, vtheta_samp);
            printf("Dists: Goal %f / Path %f / Heading %f / Occ %f \n", goal_dist_, path_dist_, head_diff_, occ_dist_);
            printf("Costs: Total %f / Goal %f / Path %f / Heading %f / Occ %f \n",best_traj->cost_, goal_cost_, path_cost_, head_cost_, occ_cost_);
            printf("Diff heading, yaw %f, %f \n", angle1_, angle2_);
            */
          }
          vtheta_samp += dvtheta;
        }
        vx_samp += dvx;
      }

       if (holonomic_robot_) {
        vtheta_samp = 0.0;
        vx_samp = 0.0;
        vy_samp = min_vel_y;
        for(int j = 0; j < vy_samples_ - 1; ++j){
          if (fabs(vy_samp) < 0.01){
            vy_samp += dvy;
            continue;
          }
          generateTrajectory(x, y, theta, vx, vy, vtheta, vx_samp, vy_samp, vtheta_samp,
              acc_x, acc_y, acc_theta, impossible_cost, *comp_traj);

          //if the new trajectory is better... let's take it
          if(comp_traj->cost_ >= 0 && ( comp_traj->cost_ < best_traj->cost_  || best_traj->cost_ < 0)
          && comp_traj-> goal_cost_traj_ < current_pos_traj.goal_cost_traj_){
            swap = best_traj;
            best_traj = comp_traj;
            comp_traj = swap;
            /*
            printf("\nFound good traj in Vy sampling vx_samp, vy_samp, vtheta_samp %f, %f, %f\n", vx_samp, vy_samp, vtheta_samp);
            printf("Test vx_samp, vy_samp, vtheta_samp: %f, %f, %f \n", vx_samp, vy_samp, vtheta_samp);
            printf("Dists: Goal %f / Path %f / Heading %f / Occ %f \n", goal_dist_, path_dist_, head_diff_, occ_dist_);
            printf("Costs: Total %f / Goal %f / Path %f / Heading %f / Occ %f \n",best_traj->cost_, goal_cost_, path_cost_, head_cost_, occ_cost_);
            printf("Diff heading, yaw %f, %f \n", angle1_, angle2_);
            */
          }else{
          }
          vy_samp += dvy;
        }

        vx_samp = min_vel_x/2;
        for(int i = 0; i < vx_samples_/2; ++i) {
          vtheta_samp = 0.0;
          vy_samp = min_vel_y;
          //next sample all vy trajectories
          for(int j = 0; j < (vy_samples_ - 1); ++j){
            if (fabs(vy_samp) < 0.01)
            {
              vy_samp += dvy;
              continue;
            }
            generateTrajectory(x, y, theta, vx, vy, vtheta, vx_samp, vy_samp, vtheta_samp,
                acc_x, acc_y, acc_theta, impossible_cost, *comp_traj);

            //if the new trajectory is better... let's take it
            if(comp_traj->cost_ >= 0 && (comp_traj->cost_ < best_traj->cost_ || best_traj->cost_ < 0)
            && comp_traj-> goal_cost_traj_ < current_pos_traj.goal_cost_traj_){
              swap = best_traj;
              best_traj = comp_traj;
              comp_traj = swap;
              /*
              printf("\nFound good traj in Vy sampling vx_samp, vy_samp, vtheta_samp %f, %f, %f\n", vx_samp, vy_samp, vtheta_samp);
              printf("Test vx_samp, vy_samp, vtheta_samp: %f, %f, %f \n", vx_samp, vy_samp, vtheta_samp);
              printf("Dists: Goal %f / Path %f / Heading %f / Occ %f \n", goal_dist_, path_dist_, head_diff_, occ_dist_);
              printf("Costs: Total %f / Goal %f / Path %f / Heading %f / Occ %f \n",best_traj->cost_, goal_cost_, path_cost_, head_cost_, occ_cost_);
              printf("Diff heading, yaw %f, %f \n", angle1_, angle2_);
              */
            }
            vy_samp += dvy;
          }
          vx_samp += dvx;
        }
      }

      ////only explore y velocities with holonomic robots
      //if (holonomic_robot_) {
        ////explore trajectories that move forward but also strafe slightly
        //vx_samp = 0.1;
        //vy_samp = 0.1;
        //vtheta_samp = 0.0;
        //generateTrajectory(x, y, theta, vx, vy, vtheta, vx_samp, vy_samp, vtheta_samp,
            //acc_x, acc_y, acc_theta, impossible_cost, *comp_traj);

        ////if the new trajectory is better... let's take it
        //if(comp_traj->cost_ >= 0 && (comp_traj->cost_ < best_traj->cost_ || best_traj->cost_ < 0)){
          //swap = best_traj;
          //best_traj = comp_traj;
          //comp_traj = swap;
        //}

        //vx_samp = 0.1;
        //vy_samp = -0.1;
        //vtheta_samp = 0.0;
        //generateTrajectory(x, y, theta, vx, vy, vtheta, vx_samp, vy_samp, vtheta_samp,
            //acc_x, acc_y, acc_theta, impossible_cost, *comp_traj);

        ////if the new trajectory is better... let's take it
        //if(comp_traj->cost_ >= 0 && (comp_traj->cost_ < best_traj->cost_ || best_traj->cost_ < 0)){
          //swap = best_traj;
          //best_traj = comp_traj;
          //comp_traj = swap;
        //}
      //}


    } // end if not escaping

    // Cesar Lopez: return and do not try anything new.
    // return *best_traj;

    //next we want to generate trajectories for rotating in place:

    // Cesar Lopez: only if we have not found a good trajectory
    vtheta_samp = min_vel_theta;
    vx_samp = 0.0;
    vy_samp = 0.0;

    //let's try to rotate toward open space
    double heading_dist = DBL_MAX;

     if (true){ //  best_traj->cost_ < 0 Cesar added condition
        for(int i = 0; i < vtheta_samples_; ++i) {
        //enforce a minimum rotational velocity because the base can't handle small in-place rotations
        double vtheta_samp_limited = vtheta_samp > 0 ? max(vtheta_samp, min_in_place_vel_th_)
            : min(vtheta_samp, -1.0 * min_in_place_vel_th_);

        generateTrajectory(x, y, theta, vx, vy, vtheta, vx_samp, vy_samp, vtheta_samp_limited,
            acc_x, acc_y, acc_theta, impossible_cost, *comp_traj);


        //if the new trajectory is better... let's take it...
        //note if we can legally rotate in place we prefer to do that rather than move with y velocity
        if(comp_traj->cost_ >= 0
            && (comp_traj->cost_ < best_traj->cost_  && comp_traj->goal_cost_traj_ < best_traj->goal_cost_traj_ || best_traj->cost_ < 0 || (best_traj->yv_ != 0.0 && comp_traj->goal_cost_traj_ < best_traj->goal_cost_traj_ && comp_traj->cost_ < best_traj->cost_))
            && (vtheta_samp > dvtheta || vtheta_samp < -1 * dvtheta)
            && comp_traj-> goal_cost_traj_ < current_pos_traj.goal_cost_traj_){
/* ** New ****/
                 swap = best_traj;
                best_traj = comp_traj;
                comp_traj = swap;
            /*
            printf("\nFound good traj in Theta sampling vx_samp, vy_samp, vtheta_samp %f, %f, %f\n", vx_samp, vy_samp, vtheta_samp);
            printf("Test vx_samp, vy_samp, vtheta_samp: %f, %f, %f \n", vx_samp, vy_samp, vtheta_samp);
            printf("Dists: Goal %f / Path %f / Heading %f / Occ %f \n", goal_dist_, path_dist_, head_diff_, occ_dist_);
            printf("Costs: Total %f / Goal %f / Path %f / Heading %f / Occ %f \n",best_traj->cost_, goal_cost_, path_cost_, head_cost_, occ_cost_);
            printf("Diff heading, yaw %f, %f \n", angle1_, angle2_);
            */
/* ** New ****/
            //double x_r, y_r, th_r;
            //unsigned int cell_x, cell_y;
            //int pointsize = comp_traj->getPointsSize();
            //if (pointsize >= 10)
                //comp_traj->getPoint(10,x_r, y_r, th_r); // Only look at a point close by
            //else
                //continue;
            ////comp_traj->getEndpoint(x_r, y_r, th_r); comp_traj->
            //x_r += heading_lookahead_ * cos(th_r);
            //y_r += heading_lookahead_ * sin(th_r);

            //double dist_to_goal =  hypot( final_goal_x_ - x, final_goal_y_ - y );
            ////make sure that we'll be looking at a legal cell
            //if(final_goal_position_valid_ && dist_to_goal> 0.2){
                //double ahead_gdist =  hypot( final_goal_x_ - x_r, final_goal_y_ - y_r );
           //if (ahead_gdist < heading_dist) {
////                 if we haven't already tried rotating left since we've moved forward
////                 if (vtheta_samp < 0 && !stuck_left) {
////                 swap = best_traj;
////                 best_traj = comp_traj;
////                 comp_traj = swap;
////                 heading_dist = ahead_gdist;
////                 }
////                 //if we haven't already tried rotating right since we've moved forward
////                 else if(vtheta_samp > 0 && !stuck_right) {
                //swap = best_traj;
                //best_traj = comp_traj;
                //comp_traj = swap;
                //heading_dist = ahead_gdist;
////                 ROS_INFO("HEad dist better");
////                 }
           //}
            //}


        }

        vtheta_samp += dvtheta;
        }
     }

    //do we have a legal trajectory
    if (best_traj->cost_ >= 0) {
      // avoid oscillations of in place rotation and in place strafing
      if ( ! (best_traj->xv_ > 0)) {
        if (best_traj->thetav_ < 0) {
          if (rotating_right) {
            stuck_right = true;
          }
          rotating_right = true;
        } else if (best_traj->thetav_ > 0) {
          if (rotating_left){
            stuck_left = true;
          }
          rotating_left = true;
        } else if(best_traj->yv_ > 0) {
          if (strafe_right) {
            stuck_right_strafe = true;
          }
          strafe_right = true;
        } else if(best_traj->yv_ < 0){
          if (strafe_left) {
            stuck_left_strafe = true;
          }
          strafe_left = true;
        }

        //set the position we must move a certain distance away from
        prev_x_ = x;
        prev_y_ = y;
      }

      double dist = hypot(x - prev_x_, y - prev_y_);
      if (dist > oscillation_reset_dist_) {
        rotating_left = false;
        rotating_right = false;
        strafe_left = false;
        strafe_right = false;
        stuck_left = false;
        stuck_right = false;
        stuck_left_strafe = false;
        stuck_right_strafe = false;
      }

      dist = hypot(x - escape_x_, y - escape_y_);
      if(dist > escape_reset_dist_ ||
          fabs(angles::shortest_angular_distance(escape_theta_, theta)) > escape_reset_theta_){
        escaping_ = false;
      }

      return *best_traj;
    }

    //only explore y velocities with holonomic robots
    if (false) { //holonomic_robot_
      //if we can't rotate in place or move forward... maybe we can move sideways and rotate
      vtheta_samp = min_vel_theta;
      vx_samp = 0.0;

      //loop through all y velocities
      for(unsigned int i = 0; i < y_vels_.size(); ++i){
        vtheta_samp = 0;
        vy_samp = y_vels_[i];
        //sample completely horizontal trajectories
        generateTrajectory(x, y, theta, vx, vy, vtheta, vx_samp, vy_samp, vtheta_samp,
            acc_x, acc_y, acc_theta, impossible_cost, *comp_traj);

        //if the new trajectory is better... let's take it
        if(comp_traj->cost_ >= 0 && (comp_traj->cost_ <= best_traj->cost_ || best_traj->cost_ < 0)){
          double x_r, y_r, th_r;
          comp_traj->getEndpoint(x_r, y_r, th_r);
          x_r += heading_lookahead_ * cos(th_r);
          y_r += heading_lookahead_ * sin(th_r);
          unsigned int cell_x, cell_y;

          //make sure that we'll be looking at a legal cell
          if(costmap_.worldToMap(x_r, y_r, cell_x, cell_y)) {
            double ahead_gdist = goal_map_(cell_x, cell_y).target_dist;
            if (ahead_gdist < heading_dist) {
              //if we haven't already tried strafing left since we've moved forward
              if (vy_samp > 0 && !stuck_left_strafe) {
                swap = best_traj;
                best_traj = comp_traj;
                comp_traj = swap;
                heading_dist = ahead_gdist;
              }
              //if we haven't already tried rotating right since we've moved forward
              else if(vy_samp < 0 && !stuck_right_strafe) {
                swap = best_traj;
                best_traj = comp_traj;
                comp_traj = swap;
                heading_dist = ahead_gdist;
              }
            }
          }
        }
      }
    }

    //do we have a legal trajectory
    if (best_traj->cost_ >= 0) {
      if (!(best_traj->xv_ > 0)) {
        if (best_traj->thetav_ < 0) {
          if (rotating_right){
            stuck_right = true;
          }
          rotating_left = true;
        } else if(best_traj->thetav_ > 0) {
          if(rotating_left){
            stuck_left = true;
          }
          rotating_right = true;
        } else if(best_traj->yv_ > 0) {
          if(strafe_right){
            stuck_right_strafe = true;
          }
          strafe_left = true;
        } else if(best_traj->yv_ < 0) {
          if(strafe_left){
            stuck_left_strafe = true;
          }
          strafe_right = true;
        }

        //set the position we must move a certain distance away from
        prev_x_ = x;
        prev_y_ = y;

      }

      double dist = hypot(x - prev_x_, y - prev_y_);
      if(dist > oscillation_reset_dist_) {
        rotating_left = false;
        rotating_right = false;
        strafe_left = false;
        strafe_right = false;
        stuck_left = false;
        stuck_right = false;
        stuck_left_strafe = false;
        stuck_right_strafe = false;
      }

      dist = hypot(x - escape_x_, y - escape_y_);
      if(dist > escape_reset_dist_ || fabs(angles::shortest_angular_distance(escape_theta_, theta)) > escape_reset_theta_) {
        escaping_ = false;
      }

      return *best_traj;
    }

    //and finally, if we can't do anything else, we want to generate trajectories that move backwards slowly
    vtheta_samp = 0.0;
    vx_samp = backup_vel_;
    vy_samp = 0.0;
    generateTrajectory(x, y, theta, vx, vy, vtheta, vx_samp, vy_samp, vtheta_samp,
        acc_x, acc_y, acc_theta, impossible_cost, *comp_traj);

//     if the new trajectory is better... let's take it
       if(comp_traj->cost_ >= 0 && (comp_traj->cost_ < best_traj->cost_ || best_traj->cost_ < 0)){
       swap = best_traj;
       best_traj = comp_traj;
       comp_traj = swap;
       }


    //we'll allow moving backwards slowly even when the static map shows it as blocked
    swap = best_traj;
    best_traj = comp_traj;
    comp_traj = swap;

    double dist = hypot(x - prev_x_, y - prev_y_);
    if (dist > oscillation_reset_dist_) {
      rotating_left = false;
      rotating_right = false;
      strafe_left = false;
      strafe_right = false;
      stuck_left = false;
      stuck_right = false;
      stuck_left_strafe = false;
      stuck_right_strafe = false;
    }

    //only enter escape mode when the planner has given a valid goal point
    if (!escaping_ && best_traj->cost_ > -2.0) {
      escape_x_ = x;
      escape_y_ = y;
      escape_theta_ = theta;
     //  escaping_ = true; // Cesar
    }

    dist = hypot(x - escape_x_, y - escape_y_);

    if (dist > escape_reset_dist_ ||
        fabs(angles::shortest_angular_distance(escape_theta_, theta)) > escape_reset_theta_) {
      escaping_ = false;
    }


    //if the trajectory failed because the footprint hits something, we're still going to back up
    if(best_traj->cost_ == -1.0)
      best_traj->cost_ = 1.0;

   if(stuck_right || stuck_left || stuck_left_strafe || stuck_right_strafe){
     ROS_INFO("stuck") ;
   }

    return *best_traj;

  }

  //given the current state of the robot, find a good trajectory
  Trajectory TrajectoryPlanner::findBestPath(tf::Stamped<tf::Pose> global_pose, tf::Stamped<tf::Pose> global_vel,
      tf::Stamped<tf::Pose>& drive_velocities){

    Eigen::Vector3f pos(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), tf::getYaw(global_pose.getRotation()));
    Eigen::Vector3f vel(global_vel.getOrigin().getX(), global_vel.getOrigin().getY(), tf::getYaw(global_vel.getRotation()));

    //reset the map for new operations
    path_map_.resetPathDist();
    goal_map_.resetPathDist();

    //temporarily remove obstacles that are within the footprint of the robot
    std::vector<base_local_planner::Position2DInt> footprint_list =
        footprint_helper_.getFootprintCells(
            pos,
            footprint_spec_,
            costmap_,
            true);

    //mark cells within the initial footprint of the robot
    for (unsigned int i = 0; i < footprint_list.size(); ++i) {
      path_map_(footprint_list[i].x, footprint_list[i].y).within_robot = true;
    }

    //make sure that we update our path based on the global plan and compute costs
    path_map_.setTargetCells(costmap_, global_plan_);
    goal_map_.setLocalGoal(costmap_, global_plan_);
    ROS_DEBUG("Path/Goal distance computed");

    //rollout trajectories and find the minimum cost one
    Trajectory best = createTrajectories(pos[0], pos[1], pos[2],
        vel[0], vel[1], vel[2],
        acc_lim_x_, acc_lim_y_, acc_lim_theta_);
    ROS_DEBUG("Trajectories created");

    /*
    //If we want to print a ppm file to draw goal dist
    char buf[4096];
    sprintf(buf, "base_local_planner.ppm");
    FILE *fp = fopen(buf, "w");
    if(fp){
      fprintf(fp, "P3\n");
      fprintf(fp, "%d %d\n", map_.size_x_, map_.size_y_);
      fprintf(fp, "255\n");
      for(int j = map_.size_y_ - 1; j >= 0; --j){
        for(unsigned int i = 0; i < map_.size_x_; ++i){
          int g_dist = 255 - int(map_(i, j).goal_dist);
          int p_dist = 255 - int(map_(i, j).path_dist);
          if(g_dist < 0)
            g_dist = 0;
          if(p_dist < 0)
            p_dist = 0;
          fprintf(fp, "%d 0 %d ", g_dist, 0);
        }
        fprintf(fp, "\n");
      }
      fclose(fp);
    }
    */

    if(best.cost_ < 0){
      //drive_velocities.setIdentity();
      tf::Vector3 start(0, 0, 0);
      drive_velocities.setOrigin(start);
      tf::Matrix3x3 matrix;
      matrix.setRotation(tf::createQuaternionFromYaw(0));
      drive_velocities.setBasis(matrix);
    }
    else{
      tf::Vector3 start(best.xv_, best.yv_, 0);
      drive_velocities.setOrigin(start);
      tf::Matrix3x3 matrix;
      matrix.setRotation(tf::createQuaternionFromYaw(best.thetav_));
      drive_velocities.setBasis(matrix);
    }

    return best;
  }

  //we need to take the footprint of the robot into account when we calculate cost to obstacles
  double TrajectoryPlanner::footprintCost(double x_i, double y_i, double theta_i){
    //check if the footprint is legal
    return world_model_.footprintCost(x_i, y_i, theta_i, footprint_spec_, inscribed_radius_, circumscribed_radius_);
  }


  void TrajectoryPlanner::getLocalGoal(double& x, double& y){
    x = path_map_.goal_x_;
    y = path_map_.goal_y_;
  }

};


