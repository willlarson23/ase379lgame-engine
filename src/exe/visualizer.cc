// Author: Tucker Haydon

#include <cstdlib>
#include <vector>
#include <string>
#include <map>
#include <string>
#include <memory>
#include <thread>
#include <csignal>
#include <utility>
#include <sstream>

#include <Eigen/Dense>
#include <ros/ros.h>

#include "yaml-cpp/yaml.h"
#include "map3d.h"

#include "trajectory_warden.h"
#include "trajectory.h"
#include "trajectory_subscriber_node.h"
#include "trajectory_publisher_node.h"
#include "trajectory_dispatcher.h"

#include "quad_state_warden.h"
#include "quad_state.h"
#include "quad_state_subscriber_node.h"
#include "quad_state_dispatcher.h"
#include "quad_state_guard.h"

#include "mediation_layer.h"
#include "view_manager.h"

#include "balloon_watchdog.h"
#include "quad_state_watchdog.h"

using namespace game_engine;

namespace { 
  // Signal variable and handler
  volatile std::sig_atomic_t kill_program;
  void SigIntHandler(int sig) {
    kill_program = 1;
  }
}


int main(int argc, char** argv) {
  // Configure sigint handler
  std::signal(SIGINT, SigIntHandler);

  // Start ROS
  ros::init(argc, argv, "visualizer", ros::init_options::NoSigintHandler);
  ros::NodeHandle nh("/game_engine/");

  // YAML config
  std::string map_file_path;
  if(false == nh.getParam("map_file_path", map_file_path)) {
    std::cerr << "Required parameter not found on server: map_file_path" << std::endl;
    std::exit(EXIT_FAILURE);
  }

  const YAML::Node node = YAML::LoadFile(map_file_path);
  const Map3D map = node["map"].as<Map3D>();

  std::map<std::string, std::string> quad_state_topics;
  if(false == nh.getParam("quad_state_topics", quad_state_topics)) {
    std::cerr << "Required parameter not found on server: quad_state_topics" << std::endl;
    std::exit(EXIT_FAILURE);
  }

  std::vector<std::string> quad_names;
  for(const auto& kv: quad_state_topics) {
    quad_names.push_back(kv.first);
  }

  // Parse the initial quad positions
  std::map<std::string, std::string> initial_quad_positions_string;
  if(false == nh.getParam("initial_quad_positions", initial_quad_positions_string)) {
    std::cerr << "Required parameter not found on server: initial_quad_positions" << std::endl;
    std::exit(EXIT_FAILURE);
  }
  std::map<
    std::string, 
    Eigen::Vector3d, 
    std::less<std::string>, 
    Eigen::aligned_allocator<std::pair<const std::string, Eigen::Vector3d>>> initial_quad_positions;
  for(const auto& kv: initial_quad_positions_string) {
    const std::string& quad_name = kv.first;
    const std::string& quad_position_string = kv.second;
    std::stringstream ss(quad_position_string);
    double x,y,z;
    ss >> x >> y >> z;
    initial_quad_positions[quad_name] = Eigen::Vector3d(x,y,z);
  }

  // Initialize the QuadStateWarden. The QuadStateWarden enables safe, multi-threaded
  // access to quadcopter state data. Internal components that require access to
  // state data should request access through QuadStateWarden.
  auto quad_state_warden = std::make_shared<QuadStateWarden>();
  for(const auto& kv: quad_state_topics) {
    const std::string& quad_name = kv.first;  
    quad_state_warden->Register(quad_name);

    const Eigen::Vector3d& initial_quad_position = initial_quad_positions[quad_name];
    quad_state_warden->Write(quad_name, QuadState(
          (Eigen::Matrix<double, 13, 1>() <<
          initial_quad_position(0), initial_quad_position(1), initial_quad_position(2),
          0,0,0,
          1,0,0,0,
          0,0,0
          ).finished()));
  }

  // For every quad, subscribe to its corresponding state topic
  std::vector<std::shared_ptr<QuadStateSubscriberNode>> state_subscribers;
  for(const auto& kv: quad_state_topics) {
    const std::string& quad_name = kv.first;  
    const std::string& topic = kv.second;  
    state_subscribers.push_back(
        std::make_shared<QuadStateSubscriberNode>(
            topic, 
            quad_name, 
            quad_state_warden));
  }


  // Create quad state guards that will be accessed by the mediation layer
  std::unordered_map<std::string, std::shared_ptr<QuadStateGuard>> quad_state_guards;
  for(const auto& kv: quad_state_topics) {
    const std::string& quad_name = kv.first;
    const Eigen::Vector3d& initial_quad_position = initial_quad_positions[quad_name];
    quad_state_guards[quad_name] 
      = std::make_shared<QuadStateGuard>(QuadState(
            (Eigen::Matrix<double, 13, 1>() <<
          initial_quad_position(0), initial_quad_position(1), initial_quad_position(2),
          0,0,0,
          1,0,0,0,
          0,0,0
          ).finished()));
  }

  // The quad state dispatcher pipes data from the state warden to any state
  // guards
  auto quad_state_dispatcher = std::make_shared<QuadStateDispatcher>();
  std::thread quad_state_dispatcher_thread([&](){
      quad_state_dispatcher->Run(quad_state_warden, quad_state_guards);
      });

  // Views
  std::string quad_mesh_file_path;
  if(false == nh.getParam("quad_mesh_file_path", quad_mesh_file_path)) {
    std::cerr << "Required parameter not found on server: quad_mesh_file_path" << std::endl;
    std::exit(EXIT_FAILURE);
  }

  std::string balloon_mesh_file_path;
  if(false == nh.getParam("balloon_mesh_file_path", balloon_mesh_file_path)) {
    std::cerr << "Required parameter not found on server: balloon_mesh_file_path" << std::endl;
    std::exit(EXIT_FAILURE);
  }

  std::vector<double> blue_balloon_position_vector;
  if(false == nh.getParam("blue_balloon_position", blue_balloon_position_vector)) {
    std::cerr << "Required parameter not found on server: blue_balloon_position" << std::endl;
    std::exit(EXIT_FAILURE);
  }
  const Eigen::Vector3d blue_balloon_position(
      blue_balloon_position_vector[0],
      blue_balloon_position_vector[1],
      blue_balloon_position_vector[2]);

  std::vector<double> red_balloon_position_vector;
  if(false == nh.getParam("red_balloon_position", red_balloon_position_vector)) {
    std::cerr << "Required parameter not found on server: red_balloon_position" << std::endl;
    std::exit(EXIT_FAILURE);
  }
  const Eigen::Vector3d red_balloon_position(
      red_balloon_position_vector[0],
      red_balloon_position_vector[1],
      red_balloon_position_vector[2]);

  std::map<std::string, std::string> team_assignments;
  if(false == nh.getParam("team_assignments", team_assignments)) {
    std::cerr << "Required parameter not found on server: team_assignments" << std::endl;
    std::exit(EXIT_FAILURE);
  }

  ViewManager::QuadViewOptions quad_view_options;
  quad_view_options.quad_mesh_file_path = quad_mesh_file_path;
  for(const auto& kv: team_assignments) {
    const std::string& color = kv.second;
    const std::string& quad_name = kv.first;
    quad_view_options.quads.push_back(std::make_pair<>(color, quad_state_guards[quad_name]));
  }

  ViewManager::BalloonViewOptions balloon_view_options;
  balloon_view_options.balloon_mesh_file_path = balloon_mesh_file_path;
  balloon_view_options.balloons.push_back(std::make_pair("red", red_balloon_position));
  balloon_view_options.balloons.push_back(std::make_pair("blue", blue_balloon_position));

  ViewManager::EnvironmentViewOptions environment_view_options;
  environment_view_options.map = map;

  auto view_manager = std::make_shared<ViewManager>();
  std::thread view_manager_thread(
      [&]() {
        view_manager->Run(
            quad_view_options,
            balloon_view_options,
            environment_view_options);
      });


  // Kill program thread. This thread sleeps for a second and then checks if the
  // 'kill_program' variable has been set. If it has, it shuts ros down and
  // sends stop signals to any other threads that might be running.
  std::thread kill_thread(
      [&]() {
        while(true) {
          if(true == kill_program) {
            break;
          } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
          }
        }
        ros::shutdown();

        view_manager->Stop();
        quad_state_dispatcher->Stop();
        quad_state_warden->Stop();
      });

  // Spin for ros subscribers
  ros::spin();

  // Wait for program termination via ctl-c
  kill_thread.join();

  // Wait for other threads to die
  quad_state_dispatcher_thread.join();
  view_manager_thread.join();

  return EXIT_SUCCESS;
}

