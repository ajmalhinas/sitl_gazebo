/*
 * Copyright (C) 2012 Open Source Robotics Foundation
 * Copyright (C) 2017-2018 PX4 Pro Development Team
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
/**
 * @brief GPS Plugin
 *
 * This plugin publishes GPS and Groundtruth data to be used and propagated
 *
 * @author Amy Wagoner <arwagoner@gmail.com>
 * @author Nuno Marques <nuno.marques@dronesolutions.io>
 */

#include <gazebo_gps_plugin.h>

namespace gazebo {
GZ_REGISTER_MODEL_PLUGIN(GpsPlugin)

GpsPlugin::GpsPlugin() : ModelPlugin()
{ }

GpsPlugin::~GpsPlugin()
{
  event::Events::DisconnectWorldUpdateBegin(updateConnection_);
}

void GpsPlugin::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
{
  // Store the pointer to the model.
  model_ = _model;

  world_ = model_->GetWorld();
  last_time_ = world_->GetSimTime();
  last_gps_time_ = world_->GetSimTime();

  // I am using agrotech parks lat and long
  const char *env_lat = 7.347417;
  const char *env_lon = 81.739194;
  const char *env_alt = std::getenv("PX4_HOME_ALT");

  // Get noise param
  if (_sdf->HasElement("gpsNoise")) {
    getSdfParam<bool>(_sdf, "gpsNoise", gps_noise_, gps_noise_);
  } else {
    gps_noise_ = false;
  }

  if (env_lat) {
    gzmsg << "Home latitude is set to " << env_lat << ".\n";
    lat_home = std::stod(env_lat) * M_PI / 180.0;
  }
  if (env_lon) {
    gzmsg << "Home longitude is set to " << env_lon << ".\n";
    lon_home = std::stod(env_lon) * M_PI / 180.0;
  }
  if (env_alt) {
    gzmsg << "Home altitude is set to " << env_alt << ".\n";
    alt_home = std::stod(env_alt);
  }

  namespace_.clear();
  if (_sdf->HasElement("robotNamespace")) {
    namespace_ = _sdf->GetElement("robotNamespace")->Get<std::string>();
  } else {
    gzerr << "[gazebo_gps_plugin] Please specify a robotNamespace.\n";
  }

  node_handle_ = transport::NodePtr(new transport::Node());
  node_handle_->Init(namespace_);

  // Listen to the update event. This event is broadcast every simulation iteration.
  updateConnection_ = event::Events::ConnectWorldUpdateBegin(
      boost::bind(&GpsPlugin::OnUpdate, this, _1));

  gravity_W_ = world_->GetPhysicsEngine()->GetGravity();

  gps_pub_ = node_handle_->Advertise<gps_msgs::msgs::SITLGps>("~/" + model_->GetName() + "/gps", 10);
  gt_pub_ = node_handle_->Advertise<gps_msgs::msgs::Groundtruth>("~/" + model_->GetName() + "/groundtruth", 10);
}

void GpsPlugin::OnUpdate(const common::UpdateInfo&){
  common::Time current_time = world_->GetSimTime();
  double dt = (current_time - last_time_).Double();

  math::Pose T_W_I = model_->GetWorldPose();    // TODO(burrimi): Check tf
  math::Vector3& pos_W_I = T_W_I.pos;           // Use the models' world position for GPS and groundtruth

  // reproject position without noise into geographic coordinates
  auto latlon_gt = reproject(pos_W_I);

  math::Vector3 velocity_current_W = model_->GetWorldLinearVel();    // Use the models' world position for GPS velocity.

  math::Vector3 velocity_current_W_xy = velocity_current_W;
  velocity_current_W_xy.z = 0;

  // update noise parameters if gps_noise_ is set
  if (gps_noise_) {
    noise_gps_pos.x = gps_xy_noise_density * sqrt(dt) * randn_(rand_);
    noise_gps_pos.y = gps_xy_noise_density * sqrt(dt) * randn_(rand_);
    noise_gps_pos.z = gps_z_noise_density * sqrt(dt) * randn_(rand_);
    noise_gps_vel.x = gps_vxy_noise_density * sqrt(dt) * randn_(rand_);
    noise_gps_vel.y = gps_vxy_noise_density * sqrt(dt) * randn_(rand_);
    noise_gps_vel.z = gps_vz_noise_density * sqrt(dt) * randn_(rand_);
    random_walk_gps.x = gps_xy_random_walk * sqrt(dt) * randn_(rand_);
    random_walk_gps.y = gps_xy_random_walk * sqrt(dt) * randn_(rand_);
    random_walk_gps.z = gps_z_random_walk * sqrt(dt) * randn_(rand_);
  }
  else {
    noise_gps_pos.x = 0.0;
    noise_gps_pos.y = 0.0;
    noise_gps_pos.z = 0.0;
    noise_gps_vel.x = 0.0;
    noise_gps_vel.y = 0.0;
    noise_gps_vel.z = 0.0;
    random_walk_gps.x = 0.0;
    random_walk_gps.y = 0.0;
    random_walk_gps.z = 0.0;
  }

  // gps bias integration
  gps_bias.x += random_walk_gps.x * dt - gps_bias.x / gps_corellation_time;
  gps_bias.y += random_walk_gps.y * dt - gps_bias.y / gps_corellation_time;
  gps_bias.z += random_walk_gps.z * dt - gps_bias.z / gps_corellation_time;

  // reproject position with noise into geographic coordinates
  auto pos_with_noise = pos_W_I + noise_gps_pos + gps_bias;
  auto latlon = reproject(pos_with_noise);

  // standard deviation TODO: add a way of computing this
  std_xy = 1.0;
  std_z = 1.0;

  // fill SITLGps msg
  gps_msg.set_time(current_time.Double());
  gps_msg.set_latitude_deg(latlon.first * 180.0 / M_PI);
  gps_msg.set_longitude_deg(latlon.second * 180.0 / M_PI);
  gps_msg.set_altitude(pos_W_I.z + alt_home + noise_gps_pos.z + gps_bias.z);
  gps_msg.set_eph(std_xy);
  gps_msg.set_epv(std_z);
  gps_msg.set_velocity(velocity_current_W_xy.GetLength());
  gps_msg.set_velocity_east(velocity_current_W.x + noise_gps_vel.y);
  gps_msg.set_velocity_north(velocity_current_W.y + noise_gps_vel.x);
  gps_msg.set_velocity_up(velocity_current_W.z + noise_gps_vel.z);

  // add msg to buffer
  gps_delay_buffer.push(gps_msg);

  // apply GPS delay
  if ((current_time - last_gps_time_).Double() > gps_update_interval_) {
    last_gps_time_ = current_time;

    while (true) {
      gps_msg = gps_delay_buffer.front();
      double gps_current_delay = current_time.Double() - gps_delay_buffer.front().time();
      if (gps_delay_buffer.empty()) {
        // abort if buffer is empty already
        break;
      }
      // remove data that is too old or if buffer size is too large
      if (gps_current_delay > gps_delay) {
        gps_delay_buffer.pop();
        // remove data if buffer too large
      } else if (gps_delay_buffer.size() > gps_buffer_size_max) {
        gps_delay_buffer.pop();
      } else {
        // if we get here, we have good data, stop
        break;
      }
    }
    // publish SITLGps msg at 5hz
    gps_pub_->Publish(gps_msg);
  }

  // fill Groundtruth msg
  groundtruth_msg.set_time(current_time.Double());
  groundtruth_msg.set_latitude_rad(latlon_gt.first);
  groundtruth_msg.set_longitude_rad(latlon_gt.second);
  groundtruth_msg.set_altitude(-pos_W_I.z + alt_home);
  groundtruth_msg.set_velocity_east(velocity_current_W.x);
  groundtruth_msg.set_velocity_north(velocity_current_W.y);
  groundtruth_msg.set_velocity_up(velocity_current_W.z);

  // publish Groundtruth msg at full rate
  gt_pub_->Publish(groundtruth_msg);

  last_time_ = current_time;
}

std::pair<double, double> GpsPlugin::reproject(math::Vector3& pos)
{
  // reproject local position to gps coordinates
  double x_rad = pos.y / earth_radius;    // north
  double y_rad = pos.x / earth_radius;    // east
  double c = sqrt(x_rad * x_rad + y_rad * y_rad);
  double sin_c = sin(c);
  double cos_c = cos(c);
  double lat_rad, lon_rad;

  if (c != 0.0) {
    lat_rad = asin(cos_c * sin(lat_home) + (x_rad * sin_c * cos(lat_home)) / c);
    lon_rad = (lon_home + atan2(y_rad * sin_c, c * cos(lat_home) * cos_c - x_rad * sin(lat_home) * sin_c));
  } else {
    lat_rad = lat_home;
    lon_rad = lon_home;
  }

  return std::make_pair (lat_rad, lon_rad);
}
}
