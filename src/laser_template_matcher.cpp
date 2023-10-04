/*
 * Copyright (c) 2011, Ivan Dryanovski, William Morris
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the CCNY Robotics Lab nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*  This package uses Canonical Scan Matcher [1], written by
 *  Andrea Censi
 *
 *  [1] A. Censi, "An ICP variant using a point-to-line metric"
 *  Proceedings of the IEEE International Conference
 *  on Robotics and Automation (ICRA), 2008
 */

#include <laser_template_matcher/laser_template_matcher.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/io/pcd_io.h>
#include <boost/assign.hpp>
#include <tf/transform_datatypes.h>

namespace scan_tools
{

  LaserTemplateMatcher::LaserTemplateMatcher(ros::NodeHandle nh, ros::NodeHandle nh_private) : nh_(nh),
                                                                                               nh_private_(nh_private),
                                                                                               initialized_(false),
                                                                                               received_imu_(false),
                                                                                               received_odom_(false),
                                                                                               received_vel_(false),
                                                                                               is_enabled_(false),
                                                                                               model_cloud_(new pcl::PointCloud<pcl::PointXYZ>())
  {
    ROS_INFO("Starting LaserTemplateMatcher");

    // **** init parameters

    initParams();

    // **** state variables
    last_base_in_fixed_.setIdentity();
    last_used_odom_pose_.setIdentity();
    input_.laser[0] = 0.0;
    input_.laser[1] = 0.0;
    input_.laser[2] = 0.0;

    // Initialize output_ vectors as Null for error-checking
    output_.cov_x_m = 0;
    output_.dx_dy1_m = 0;
    output_.dx_dy2_m = 0;

    // **** services
    enable_matching_service_ = nh_.advertiseService("enable_template_matching", &LaserTemplateMatcher::enableMatchingCallback, this);

    // **** publishers
    model_publisher_ = nh_.advertise<sensor_msgs::LaserScan>("laser_model", 5);

    if (publish_pose_)
    {
      pose_publisher_ = nh_.advertise<geometry_msgs::Pose2D>(
          "pose2D", 5);
    }

    if (publish_pose_stamped_)
    {
      pose_stamped_publisher_ = nh_.advertise<geometry_msgs::PoseStamped>(
          "pose_stamped", 5);
    }

    if (publish_pose_with_covariance_)
    {
      pose_with_covariance_publisher_ = nh_.advertise<geometry_msgs::PoseWithCovariance>(
          "pose_with_covariance", 5);
    }

    if (publish_pose_with_covariance_stamped_)
    {
      pose_with_covariance_stamped_publisher_ = nh_.advertise<geometry_msgs::PoseWithCovarianceStamped>(
          "pose_with_covariance_stamped", 5);
    }

    // *** subscribers
    estimate_model_pose_subscriber_ = nh_.subscribe("estimate_model_pose", 1, &LaserTemplateMatcher::estimateModelPoseCallback, this);

    if (use_cloud_input_)
    {
      cloud_subscriber_ = nh_.subscribe(
          "cloud", 1, &LaserTemplateMatcher::cloudCallback, this);
    }
    else
    {
      scan_subscriber_ = nh_.subscribe(
          "scan", 1, &LaserTemplateMatcher::scanCallback, this);
    }

    if (use_imu_)
    {
      imu_subscriber_ = nh_.subscribe(
          "imu/data", 1, &LaserTemplateMatcher::imuCallback, this);
    }
    if (use_odom_)
    {
      odom_subscriber_ = nh_.subscribe(
          "odom", 1, &LaserTemplateMatcher::odomCallback, this);
    }
    if (use_vel_)
    {
      if (stamped_vel_)
        vel_subscriber_ = nh_.subscribe(
            "vel", 1, &LaserTemplateMatcher::velStmpCallback, this);
      else
        vel_subscriber_ = nh_.subscribe(
            "vel", 1, &LaserTemplateMatcher::velCallback, this);
    }
  }

  LaserTemplateMatcher::~LaserTemplateMatcher()
  {
    ROS_INFO("Destroying LaserTemplateMatcher");
  }

  void LaserTemplateMatcher::initParams()
  {
    if (!nh_private_.getParam("base_frame", base_frame_))
      base_frame_ = "base_link";
    if (!nh_private_.getParam("fixed_frame", base_fixed_frame_))
      base_fixed_frame_ = "base_anchor";
    if (!nh_private_.getParam("laser_fixed_frame", laser_fixed_frame_))
      laser_fixed_frame_ = "laser_anchor";

    laser_model.header.frame_id = laser_fixed_frame_;
    nh_private_.getParam("angle_min", laser_model.angle_min);
    nh_private_.getParam("angle_max", laser_model.angle_max);
    nh_private_.getParam("angle_increment", laser_model.angle_increment);
    nh_private_.getParam("time_increment", laser_model.time_increment);
    nh_private_.getParam("scan_time", laser_model.scan_time);
    nh_private_.getParam("range_min", laser_model.range_min);
    nh_private_.getParam("range_max", laser_model.range_max);
    nh_private_.getParam("ranges", laser_model.ranges);
    nh_private_.getParam("intensities", laser_model.intensities);
    sensor_msgs::LaserScan::ConstPtr laser_model_cptr = boost::make_shared<sensor_msgs::LaserScan>(laser_model);
    laserScanToLDP(laser_model_cptr, model_ldp_);

    model_ldp_->estimate[0] = 0.0;
    model_ldp_->estimate[1] = 0.0;
    model_ldp_->estimate[2] = 0.0;

    input_.laser_ref = model_ldp_;

    // **** input type - laser scan, or point clouds?
    // if false, will subscribe to LaserScan msgs on /scan.
    // if true, will subscribe to PointCloud2 msgs on /cloud

    if (!nh_private_.getParam("use_cloud_input", use_cloud_input_))
      use_cloud_input_ = false;

    if (use_cloud_input_)
    {
      if (!nh_private_.getParam("cloud_range_min", cloud_range_min_))
        cloud_range_min_ = 0.1;
      if (!nh_private_.getParam("cloud_range_max", cloud_range_max_))
        cloud_range_max_ = 50.0;
      if (!nh_private_.getParam("cloud_res", cloud_res_))
        cloud_res_ = 0.05;

      input_.min_reading = cloud_range_min_;
      input_.max_reading = cloud_range_max_;
    }

    // **** What predictions are available to speed up the ICP?
    // 1) imu - [theta] from imu yaw angle - /imu topic
    // 2) odom - [x, y, theta] from wheel odometry - /odom topic
    // 3) vel - [x, y, theta] from velocity predictor - see alpha-beta predictors - /vel topic
    // If more than one is enabled, priority is imu > odom > vel

    if (!nh_private_.getParam("use_imu", use_imu_))
      use_imu_ = true;
    if (!nh_private_.getParam("use_odom", use_odom_))
      use_odom_ = true;
    if (!nh_private_.getParam("use_tf", use_tf_))
      use_tf_ = true;
    if (!nh_private_.getParam("tf_timeout", tf_timeout_))
      tf_timeout_ = 0.1;
    if (!nh_private_.getParam("use_vel", use_vel_))
      use_vel_ = false;

    // **** Are velocity input messages stamped?
    // if false, will subscribe to Twist msgs on /vel
    // if true, will subscribe to TwistStamped msgs on /vel
    if (!nh_private_.getParam("stamped_vel", stamped_vel_))
      stamped_vel_ = false;

    // **** How to publish the output?
    // tf (fixed_frame->base_frame),
    // pose message (pose of base frame in the fixed frame)

    if (!nh_private_.getParam("publish_tf", publish_tf_))
      publish_tf_ = true;
    if (!nh_private_.getParam("publish_pose", publish_pose_))
      publish_pose_ = true;
    if (!nh_private_.getParam("publish_pose_stamped", publish_pose_stamped_))
      publish_pose_stamped_ = false;
    if (!nh_private_.getParam("publish_pose_with_covariance", publish_pose_with_covariance_))
      publish_pose_with_covariance_ = false;
    if (!nh_private_.getParam("publish_pose_with_covariance_stamped", publish_pose_with_covariance_stamped_))
      publish_pose_with_covariance_stamped_ = false;

    if (!nh_private_.getParam("position_covariance", position_covariance_))
    {
      position_covariance_.resize(3);
      std::fill(position_covariance_.begin(), position_covariance_.end(), 1e-9);
    }

    if (!nh_private_.getParam("orientation_covariance", orientation_covariance_))
    {
      orientation_covariance_.resize(3);
      std::fill(orientation_covariance_.begin(), orientation_covariance_.end(), 1e-9);
    }
    // **** CSM parameters - comments copied from algos.h (by Andrea Censi)

    // Maximum angular displacement between scans
    if (!nh_private_.getParam("max_angular_correction_deg", input_.max_angular_correction_deg))
      input_.max_angular_correction_deg = 45.0;

    // Maximum translation between scans (m)
    if (!nh_private_.getParam("max_linear_correction", input_.max_linear_correction))
      input_.max_linear_correction = 0.50;

    // Maximum ICP cycle iterations
    if (!nh_private_.getParam("max_iterations", input_.max_iterations))
      input_.max_iterations = 10;

    // A threshold for stopping (m)
    if (!nh_private_.getParam("epsilon_xy", input_.epsilon_xy))
      input_.epsilon_xy = 0.000001;

    // A threshold for stopping (rad)
    if (!nh_private_.getParam("epsilon_theta", input_.epsilon_theta))
      input_.epsilon_theta = 0.000001;

    // Maximum distance for a correspondence to be valid
    if (!nh_private_.getParam("max_correspondence_dist", input_.max_correspondence_dist))
      input_.max_correspondence_dist = 0.3;

    // Noise in the scan (m)
    if (!nh_private_.getParam("sigma", input_.sigma))
      input_.sigma = 0.010;

    // Use smart tricks for finding correspondences.
    if (!nh_private_.getParam("use_corr_tricks", input_.use_corr_tricks))
      input_.use_corr_tricks = 1;

    // Restart: Restart if error is over threshold
    if (!nh_private_.getParam("restart", input_.restart))
      input_.restart = 0;

    // Restart: Threshold for restarting
    if (!nh_private_.getParam("restart_threshold_mean_error", input_.restart_threshold_mean_error))
      input_.restart_threshold_mean_error = 0.01;

    // Restart: displacement for restarting. (m)
    if (!nh_private_.getParam("restart_dt", input_.restart_dt))
      input_.restart_dt = 1.0;

    // Restart: displacement for restarting. (rad)
    if (!nh_private_.getParam("restart_dtheta", input_.restart_dtheta))
      input_.restart_dtheta = 0.1;

    // Max distance for staying in the same clustering
    if (!nh_private_.getParam("clustering_threshold", input_.clustering_threshold))
      input_.clustering_threshold = 0.25;

    // Number of neighbour rays used to estimate the orientation
    if (!nh_private_.getParam("orientation_neighbourhood", input_.orientation_neighbourhood))
      input_.orientation_neighbourhood = 20;

    // If 0, it's vanilla ICP
    if (!nh_private_.getParam("use_point_to_line_distance", input_.use_point_to_line_distance))
      input_.use_point_to_line_distance = 1;

    // Discard correspondences based on the angles
    if (!nh_private_.getParam("do_alpha_test", input_.do_alpha_test))
      input_.do_alpha_test = 0;

    // Discard correspondences based on the angles - threshold angle, in degrees
    if (!nh_private_.getParam("do_alpha_test_thresholdDeg", input_.do_alpha_test_thresholdDeg))
      input_.do_alpha_test_thresholdDeg = 20.0;

    // Percentage of correspondences to consider: if 0.9,
    // always discard the top 10% of correspondences with more error
    if (!nh_private_.getParam("outliers_maxPerc", input_.outliers_maxPerc))
      input_.outliers_maxPerc = 0.90;

    // Parameters describing a simple adaptive algorithm for discarding.
    //  1) Order the errors.
    //  2) Choose the percentile according to outliers_adaptive_order.
    //     (if it is 0.7, get the 70% percentile)
    //  3) Define an adaptive threshold multiplying outliers_adaptive_mult
    //     with the value of the error at the chosen percentile.
    //  4) Discard correspondences over the threshold.
    //  This is useful to be conservative; yet remove the biggest errors.
    if (!nh_private_.getParam("outliers_adaptive_order", input_.outliers_adaptive_order))
      input_.outliers_adaptive_order = 0.7;

    if (!nh_private_.getParam("outliers_adaptive_mult", input_.outliers_adaptive_mult))
      input_.outliers_adaptive_mult = 2.0;

    // If you already have a guess of the solution, you can compute the polar angle
    // of the points of one scan in the new position. If the polar angle is not a monotone
    // function of the readings index, it means that the surface is not visible in the
    // next position. If it is not visible, then we don't use it for matching.
    if (!nh_private_.getParam("do_visibility_test", input_.do_visibility_test))
      input_.do_visibility_test = 0;

    // no two points in laser_sens can have the same corr.
    if (!nh_private_.getParam("outliers_remove_doubles", input_.outliers_remove_doubles))
      input_.outliers_remove_doubles = 1;

    // If 1, computes the covariance of ICP using the method http://purl.org/censi/2006/icpcov
    if (!nh_private_.getParam("do_compute_covariance", input_.do_compute_covariance))
      input_.do_compute_covariance = 0;

    // Checks that find_correspondences_tricks gives the right answer
    if (!nh_private_.getParam("debug_verify_tricks", input_.debug_verify_tricks))
      input_.debug_verify_tricks = 0;

    // If 1, the field 'true_alpha' (or 'alpha') in the first scan is used to compute the
    // incidence beta, and the factor (1/cos^2(beta)) used to weight the correspondence.");
    if (!nh_private_.getParam("use_ml_weights", input_.use_ml_weights))
      input_.use_ml_weights = 0;

    // If 1, the field 'readings_sigma' in the second scan is used to weight the
    // correspondence by 1/sigma^2
    if (!nh_private_.getParam("use_sigma_weights", input_.use_sigma_weights))
      input_.use_sigma_weights = 0;
  }

  void LaserTemplateMatcher::imuCallback(const sensor_msgs::Imu::ConstPtr &imu_msg)
  {
    boost::mutex::scoped_lock(mutex_);
    latest_imu_msg_ = *imu_msg;
    if (!received_imu_)
    {
      tf::quaternionMsgToTF(imu_msg->orientation, last_used_imu_orientation_);
      received_imu_ = true;
    }
  }

  void LaserTemplateMatcher::odomCallback(const nav_msgs::Odometry::ConstPtr &odom_msg)
  {
    boost::mutex::scoped_lock(mutex_);
    latest_odom_msg_ = *odom_msg;
    if (!received_odom_)
    {
      tf::poseMsgToTF(odom_msg->pose.pose, last_used_odom_pose_);
      received_odom_ = true;
    }
  }

  void LaserTemplateMatcher::velCallback(const geometry_msgs::Twist::ConstPtr &twist_msg)
  {
    boost::mutex::scoped_lock(mutex_);
    latest_vel_msg_ = *twist_msg;

    received_vel_ = true;
  }

  void LaserTemplateMatcher::velStmpCallback(const geometry_msgs::TwistStamped::ConstPtr &twist_msg)
  {
    boost::mutex::scoped_lock(mutex_);
    latest_vel_msg_ = twist_msg->twist;

    received_vel_ = true;
  }

  void LaserTemplateMatcher::cloudCallback(const PointCloudT::ConstPtr &cloud)
  {
    // **** if first scan, cache the tf from base to the scanner

    std_msgs::Header cloud_header = pcl_conversions::fromPCL(cloud->header);

    if (!initialized_)
    {
      // cache the static tf from base to laser
      // laser_model.header.frame_id = base_fixed_frame_;
      if (!getBaseLaserTransform(cloud_header.frame_id))
      {
        ROS_WARN("Skipping scan");
        return;
      }

      last_icp_time_ = cloud_header.stamp;
      initialized_ = true;
    }

    LDP curr_ldp_scan;
    PointCloudToLDP(cloud, curr_ldp_scan);
    processScan(curr_ldp_scan, cloud_header.stamp);
  }

  void LaserTemplateMatcher::scanCallback(const sensor_msgs::LaserScan::ConstPtr &scan_msg)
  {
    // **** if first scan, cache the tf from base to the scanner

    if (!initialized_)
    {
      createCache(scan_msg); // caches the sin and cos of all angles

      // cache the static transform between the base and laser
      if (!getBaseLaserTransform(scan_msg->header.frame_id))
      {
        ROS_WARN("Skipping scan");
        return;
      }

      last_icp_time_ = scan_msg->header.stamp;
      initialized_ = true;
    }

    LDP curr_ldp_scan;
    laserScanToLDP(scan_msg, curr_ldp_scan);
    processScan(curr_ldp_scan, scan_msg->header.stamp);
  }

  void LaserTemplateMatcher::estimateModelPoseCallback(const geometry_msgs::PoseStamped::ConstPtr &model_pose_msg)
  {
    boost::mutex::scoped_lock(mutex_);
    tf::StampedTransform msg_from_base;
    tf::Stamped<tf::Pose> model_from_msg;
    tf::poseStampedMsgToTF(*model_pose_msg, model_from_msg);

    try
    {
      tf_listener_.waitForTransform(base_frame_, model_pose_msg->header.frame_id, ros::Time(0), ros::Duration(tf_timeout_));
      tf_listener_.lookupTransform(base_frame_, model_pose_msg->header.frame_id, ros::Time(0), msg_from_base);
    }
    catch (const tf::TransformException &ex)
    {
      ROS_WARN("Could not get initial transform from %s to %s frame, %s", base_frame_.c_str(), model_pose_msg->header.frame_id.c_str(), ex.what());
      return;
    }
    last_base_in_fixed_ = (model_from_msg * msg_from_base).inverse();
    ROS_INFO("Successfully set new anchor point");
  }

  bool LaserTemplateMatcher::enableMatchingCallback(std_srvs::SetBool::Request &req,
                                                    std_srvs::SetBool::Response &res)
  {
    is_enabled_ = req.data;
    res.success = true;
    res.message = "LaserTemplateMatcher is now " + std::string(is_enabled_ ? "enabled" : "disabled");
    return true;
  }

  void LaserTemplateMatcher::processScan(LDP &curr_ldp_scan, const ros::Time &time)
  {
    laser_model.header.stamp = ros::Time::now();
    model_publisher_.publish(laser_model);

    if (!is_enabled_)
    {
      if (publish_tf_) // TODO: fix this, maybe on separate thread
      {
        tf::Transform last_fixed_in_base = last_base_in_fixed_.inverse();
        std::vector<tf::StampedTransform> tfs;
        tfs.push_back(tf::StampedTransform(last_fixed_in_base, time, base_frame_, base_fixed_frame_));
        tfs.push_back(tf::StampedTransform(base_from_laser_, time, base_fixed_frame_, laser_fixed_frame_));
        tf_broadcaster_.sendTransform(tfs);
      }
      return;
    }

    ros::WallTime start = ros::WallTime::now();

    // CSM is used in the following way:
    // The scans are always in the laser frame
    // The reference scan (prevLDPcan_) has a pose of [0, 0, 0]
    // The new scan (currLDPScan) has a pose equal to the movement
    // of the laser in the laser frame since the last scan
    // The computed correction is then propagated using the tf machinery

    input_.laser_sens = curr_ldp_scan;

    // **** estimated change since last scan

    // get the predicted offset of the scan base pose from the last scan base pose
    tf::Transform pred_last_base_offset = getPrediction(ros::Time(0));

    // calculate the predicted scan base pose by applying the predicted offset to the last scan base pose
    tf::Transform pred_base_in_fixed = last_base_in_fixed_ * pred_last_base_offset;

    // prev and last used interchangably
    // convert the predicted offset from the prev base frame to be in the prev laser frame
    // tf::Transform pred_laser = last_base_in_fixed_ * pred_last_base_offset * base_from_laser_;
    tf::Transform pred_laser = laser_from_base_ * last_base_in_fixed_ * base_from_laser_;

    input_.first_guess[0] = pred_laser.getOrigin().getX();
    input_.first_guess[1] = pred_laser.getOrigin().getY();
    input_.first_guess[2] = tf::getYaw(pred_laser.getRotation());

    // If they are non-Null, free covariance gsl matrices to avoid leaking memory
    if (output_.cov_x_m)
    {
      gsl_matrix_free(output_.cov_x_m);
      output_.cov_x_m = 0;
    }
    if (output_.dx_dy1_m)
    {
      gsl_matrix_free(output_.dx_dy1_m);
      output_.dx_dy1_m = 0;
    }
    if (output_.dx_dy2_m)
    {
      gsl_matrix_free(output_.dx_dy2_m);
      output_.dx_dy2_m = 0;
    }

    // *** scan match - using point to line icp from CSM

    sm_icp(&input_, &output_);
    // tf::Transform meas_base_offset;

    if (output_.valid)
    {

      // the measured offset of the scan from the prev in the prev laser frame
      tf::Transform meas_laser;
      createTfFromXYTheta(output_.x[0], output_.x[1], output_.x[2], meas_laser);

      // static double prev_scan_x, prev_scan_y, prev_scan_theta;
      // double ex = output_.x[0] - prev_scan_x;
      // double ey = output_.x[1] - prev_scan_y;
      // double ea = output_.x[2] - prev_scan_theta;
      // double en = sqrt(ex * ex + ey * ey);

      // ROS_WARN("Error X: %f, Error Y: %f, Error A: %f, |Error|: %f", ex, ey, ea, en);
      // prev_scan_x = output_.x[0];
      // prev_scan_y = output_.x[1];
      // prev_scan_theta = output_.x[2];

      // convert the measured offset from the prev laser frame to the prev base frame
      // meas_base_offset = base_from_laser_ * meas_laser_offset * laser_from_base_;

      // calculate the measured pose of the scan in the fixed frame
      last_base_in_fixed_ = base_from_laser_ * meas_laser * laser_from_base_; // last_base_in_fixed_ * meas_base_offset;
      tf::Transform last_fixed_in_base = last_base_in_fixed_.inverse();

      // **** publish

      Eigen::Matrix2f xy_cov = Eigen::Matrix2f::Zero();
      float yaw_cov = 0.0;
      if (input_.do_compute_covariance)
      {
        // get covariance from ICP
        xy_cov(0, 0) = gsl_matrix_get(output_.cov_x_m, 0, 0);
        xy_cov(0, 1) = gsl_matrix_get(output_.cov_x_m, 0, 1);
        xy_cov(1, 0) = gsl_matrix_get(output_.cov_x_m, 1, 0);
        xy_cov(1, 1) = gsl_matrix_get(output_.cov_x_m, 1, 1);
        yaw_cov = gsl_matrix_get(output_.cov_x_m, 2, 2);

        // rotate xy covariance from the last into odom frame
        auto rotation = getLaserRotation(last_base_in_fixed_);
        xy_cov = rotation * xy_cov * rotation.transpose();
      }
      else
      {
        xy_cov(0, 0) = position_covariance_[0];
        xy_cov(1, 1) = position_covariance_[1];
        yaw_cov = orientation_covariance_[2];
      }

      if (publish_pose_)
      {
        // unstamped Pose2D message
        geometry_msgs::Pose2D::Ptr pose_msg;
        pose_msg = boost::make_shared<geometry_msgs::Pose2D>();
        pose_msg->x = last_fixed_in_base.getOrigin().getX();
        pose_msg->y = last_fixed_in_base.getOrigin().getY();
        pose_msg->theta = tf::getYaw(last_fixed_in_base.getRotation());
        pose_publisher_.publish(pose_msg);
      }
      if (publish_pose_stamped_)
      {
        // stamped Pose message
        geometry_msgs::PoseStamped::Ptr pose_stamped_msg;
        pose_stamped_msg = boost::make_shared<geometry_msgs::PoseStamped>();

        pose_stamped_msg->header.stamp = time;
        pose_stamped_msg->header.frame_id = base_frame_;

        tf::poseTFToMsg(last_fixed_in_base, pose_stamped_msg->pose);

        pose_stamped_publisher_.publish(pose_stamped_msg);
      }
      if (publish_pose_with_covariance_)
      {
        // unstamped PoseWithCovariance message
        geometry_msgs::PoseWithCovariance::Ptr pose_with_covariance_msg;
        pose_with_covariance_msg = boost::make_shared<geometry_msgs::PoseWithCovariance>();
        tf::poseTFToMsg(last_fixed_in_base, pose_with_covariance_msg->pose);

        pose_with_covariance_msg->covariance = boost::assign::list_of(xy_cov(0, 0))(xy_cov(0, 1))(0)(0)(0)(0)(xy_cov(1, 0))(xy_cov(1, 1))(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(yaw_cov);

        pose_with_covariance_publisher_.publish(pose_with_covariance_msg);
      }
      if (publish_pose_with_covariance_stamped_)
      {
        // stamped Pose message
        geometry_msgs::PoseWithCovarianceStamped::Ptr pose_with_covariance_stamped_msg;
        pose_with_covariance_stamped_msg = boost::make_shared<geometry_msgs::PoseWithCovarianceStamped>();

        pose_with_covariance_stamped_msg->header.stamp = time;
        pose_with_covariance_stamped_msg->header.frame_id = base_frame_;

        tf::poseTFToMsg(last_fixed_in_base, pose_with_covariance_stamped_msg->pose.pose);

        pose_with_covariance_stamped_msg->pose.covariance = boost::assign::list_of(xy_cov(0, 0))(xy_cov(0, 1))(0)(0)(0)(0)(xy_cov(1, 0))(xy_cov(1, 1))(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(yaw_cov);

        pose_with_covariance_stamped_publisher_.publish(pose_with_covariance_stamped_msg);
      }

      if (publish_tf_)
      {
        std::vector<tf::StampedTransform> tfs;
        tfs.push_back(tf::StampedTransform(last_fixed_in_base, time, base_frame_, base_fixed_frame_));
        tfs.push_back(tf::StampedTransform(base_from_laser_, time, base_fixed_frame_, laser_fixed_frame_));
        tf_broadcaster_.sendTransform(tfs);
      }
    }
    else
    {
      // meas_base_offset.setIdentity();
      ROS_WARN("Error in scan matching");
    }

    // **** swap old and new

    last_icp_time_ = time;

    // **** statistics

    double dur = (ros::WallTime::now() - start).toSec() * 1e3;
    ROS_DEBUG("Scan matcher total duration: %.1f ms", dur);
  }

  void LaserTemplateMatcher::PointCloudToLDP(const PointCloudT::ConstPtr &cloud,
                                             LDP &ldp)
  {
    double max_d2 = cloud_res_ * cloud_res_;

    PointCloudT cloud_f;

    cloud_f.points.push_back(cloud->points[0]);

    for (unsigned int i = 1; i < cloud->points.size(); ++i)
    {
      const PointT &pa = cloud_f.points[cloud_f.points.size() - 1];
      const PointT &pb = cloud->points[i];

      double dx = pa.x - pb.x;
      double dy = pa.y - pb.y;
      double d2 = dx * dx + dy * dy;

      if (d2 > max_d2)
      {
        cloud_f.points.push_back(pb);
      }
    }

    unsigned int n = cloud_f.points.size();

    ldp = ld_alloc_new(n);

    for (unsigned int i = 0; i < n; i++)
    {
      // calculate position in laser frame
      if (is_nan(cloud_f.points[i].x) || is_nan(cloud_f.points[i].y))
      {
        ROS_WARN("Laser Scan Matcher: Cloud input contains NaN values. \
                Please use a filtered cloud input.");
      }
      else
      {
        double r = sqrt(cloud_f.points[i].x * cloud_f.points[i].x +
                        cloud_f.points[i].y * cloud_f.points[i].y);

        if (r > cloud_range_min_ && r < cloud_range_max_)
        {
          ldp->valid[i] = 1;
          ldp->readings[i] = r;
        }
        else
        {
          ldp->valid[i] = 0;
          ldp->readings[i] = -1; // for invalid range
        }
      }

      ldp->theta[i] = atan2(cloud_f.points[i].y, cloud_f.points[i].x);
      ldp->cluster[i] = -1;
    }

    ldp->min_theta = ldp->theta[0];
    ldp->max_theta = ldp->theta[n - 1];

    ldp->odometry[0] = 0.0;
    ldp->odometry[1] = 0.0;
    ldp->odometry[2] = 0.0;

    ldp->true_pose[0] = 0.0;
    ldp->true_pose[1] = 0.0;
    ldp->true_pose[2] = 0.0;
  }

  void LaserTemplateMatcher::laserScanToLDP(const sensor_msgs::LaserScan::ConstPtr &scan_msg,
                                            LDP &ldp)
  {
    unsigned int n = scan_msg->ranges.size();
    ldp = ld_alloc_new(n);

    for (unsigned int i = 0; i < n; i++)
    {
      // calculate position in laser frame

      double r = scan_msg->ranges[i];

      if (r > scan_msg->range_min && r < scan_msg->range_max)
      {
        // fill in laser scan data

        ldp->valid[i] = 1;
        ldp->readings[i] = r;
      }
      else
      {
        ldp->valid[i] = 0;
        ldp->readings[i] = -1; // for invalid range
      }

      ldp->theta[i] = scan_msg->angle_min + i * scan_msg->angle_increment;

      ldp->cluster[i] = -1;
    }

    ldp->min_theta = ldp->theta[0];
    ldp->max_theta = ldp->theta[n - 1];

    ldp->odometry[0] = 0.0;
    ldp->odometry[1] = 0.0;
    ldp->odometry[2] = 0.0;

    ldp->true_pose[0] = 0.0;
    ldp->true_pose[1] = 0.0;
    ldp->true_pose[2] = 0.0;
  }

  void LaserTemplateMatcher::createCache(const sensor_msgs::LaserScan::ConstPtr &scan_msg)
  {
    a_cos_.clear();
    a_sin_.clear();

    for (unsigned int i = 0; i < scan_msg->ranges.size(); ++i)
    {
      double angle = scan_msg->angle_min + i * scan_msg->angle_increment;
      a_cos_.push_back(cos(angle));
      a_sin_.push_back(sin(angle));
    }

    input_.min_reading = scan_msg->range_min;
    input_.max_reading = scan_msg->range_max;
  }

  bool LaserTemplateMatcher::getBaseLaserTransform(const std::string &frame_id)
  {
    tf::StampedTransform base_from_laser;
    try
    {
      tf_listener_.waitForTransform(base_frame_, frame_id, ros::Time(0), ros::Duration(tf_timeout_));
      tf_listener_.lookupTransform(base_frame_, frame_id, ros::Time(0), base_from_laser);
    }
    catch (const tf::TransformException &ex)
    {
      ROS_WARN("Could not get initial transform from base to laser frame, %s", ex.what());
      return false;
    }
    base_from_laser_ = base_from_laser;
    laser_from_base_ = base_from_laser_.inverse();

    return true;
  }

  // returns the predicted offset from the base pose of the last scan
  tf::Transform LaserTemplateMatcher::getPrediction(const ros::Time &stamp)
  {
    boost::mutex::scoped_lock(mutex_);

    // **** base case - no input available, use zero-motion model
    tf::Transform pred_last_base_offset = tf::Transform::getIdentity();

    // **** use velocity (for example from ab-filter)
    if (use_vel_)
    {
      double dt = (stamp - last_icp_time_).toSec();
      // NOTE: this assumes the velocity is in the base frame and that the base
      //       and laser frames share the same x,y and z axes
      double pr_ch_x = dt * latest_vel_msg_.linear.x;
      double pr_ch_y = dt * latest_vel_msg_.linear.y;
      double pr_ch_a = dt * latest_vel_msg_.angular.z;

      createTfFromXYTheta(pr_ch_x, pr_ch_y, pr_ch_a, pred_last_base_offset);
    }

    // **** use wheel odometry
    if (use_odom_ && received_odom_)
    {
      // NOTE: this assumes the odometry is in the base frame
      tf::Transform latest_odom_pose;
      tf::poseMsgToTF(latest_odom_msg_.pose.pose, latest_odom_pose);

      pred_last_base_offset = last_used_odom_pose_.inverse() * latest_odom_pose;

      last_used_odom_pose_ = latest_odom_pose;
    }

    // **** use imu
    if (use_imu_ && received_imu_)
    {
      // NOTE: this assumes the imu is in the base frame
      tf::Quaternion latest_imu_orientation;
      tf::quaternionMsgToTF(latest_imu_msg_.orientation, latest_imu_orientation);

      tf::Quaternion imu_orientation_offset = last_used_imu_orientation_.inverse() * latest_imu_orientation;
      pred_last_base_offset.setRotation(imu_orientation_offset);

      last_used_imu_orientation_ = latest_imu_orientation;
    }

    // **** use tf
    if (use_tf_)
    {
      tf::StampedTransform pred_last_base_offset_tf;
      try
      {
        tf_listener_.waitForTransform(base_frame_, last_icp_time_, base_frame_, stamp, base_fixed_frame_, ros::Duration(tf_timeout_));
        tf_listener_.lookupTransform(base_frame_, last_icp_time_, base_frame_, stamp, base_fixed_frame_, pred_last_base_offset_tf);
        pred_last_base_offset = pred_last_base_offset_tf;
      }
      catch (const tf::TransformException &ex)
      {
        ROS_WARN("Could not get base to fixed frame transform, %s", ex.what());
      }
    }

    return pred_last_base_offset;
  }

  void LaserTemplateMatcher::createTfFromXYTheta(
      double x, double y, double theta, tf::Transform &t)
  {
    t.setOrigin(tf::Vector3(x, y, 0.0));
    tf::Quaternion q;
    q.setRPY(0.0, 0.0, theta);
    t.setRotation(q);
  }

  Eigen::Matrix2f LaserTemplateMatcher::getLaserRotation(const tf::Transform &odom_pose) const
  {
    tf::Transform laser_in_fixed = odom_pose * laser_from_base_;
    tf::Matrix3x3 fixed_from_laser_rot(laser_in_fixed.getRotation());
    double r, p, y;
    fixed_from_laser_rot.getRPY(r, p, y);
    Eigen::Rotation2Df t(y);
    return t.toRotationMatrix();
  }

} // namespace scan_tools
