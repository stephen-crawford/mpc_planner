#include <mpc-planner-jackal/ros1_planner.h>

#include <mpc-planner/data_preparation.h>

#include <mpc-planner-util/visuals.h>
#include <mpc-planner-util/parameters.h>
#include <mpc-planner-util/logging.h>
#include <mpc-planner-util/load_yaml.hpp>

#include <ros_tools/helpers.h>

using namespace MPCPlanner;

JackalPlanner::JackalPlanner(ros::NodeHandle &nh)
{

    LOG_INFO("Started Jackal Planner");

    // Initialize the configuration
    Configuration::getInstance().initialize(SYSTEM_CONFIG_PATH(__FILE__, "settings"));

    // Initialize the planner
    _planner = std::make_unique<Planner>();

    nh.getParam("jackal_planner/simulation", _simulation);
    LOG_VALUE("Simulation", _simulation);

    // Initialize the ROS interface
    initializeSubscribersAndPublishers(nh);

    if (_simulation)
        startEnvironment();

    _benchmarker = std::make_unique<RosTools::Benchmarker>("loop");

    // Start the control loop
    _timer = nh.createTimer(
        ros::Duration(1.0 / CONFIG["control_frequency"].as<double>()),
        &JackalPlanner::loop,
        this);

    LOG_DIVIDER();
}

void JackalPlanner::initializeSubscribersAndPublishers(ros::NodeHandle &nh)
{
    LOG_INFO("initializeSubscribersAndPublishers");

    _state_sub = nh.subscribe<nav_msgs::Odometry>(
        "/input/state", 5,
        boost::bind(&JackalPlanner::stateCallback, this, _1));

    _state_pose_sub = nh.subscribe<geometry_msgs::PoseStamped>(
        "/input/state_pose", 5,
        boost::bind(&JackalPlanner::statePoseCallback, this, _1));

    _goal_sub = nh.subscribe<geometry_msgs::PoseStamped>(
        "/input/goal", 1,
        boost::bind(&JackalPlanner::goalCallback, this, _1));

    _path_sub = nh.subscribe<nav_msgs::Path>(
        "/input/reference_path", 1,
        boost::bind(&JackalPlanner::pathCallback, this, _1));

    _obstacle_sim_sub = nh.subscribe<mpc_planner_msgs::obstacle_array>(
        "/input/obstacles_sim", 1,
        boost::bind(&JackalPlanner::obstacleSimCallback, this, _1));

    _obstacle_sub = nh.subscribe<derived_object_msgs::ObjectArray>(
        "/input/obstacles", 1,
        boost::bind(&JackalPlanner::obstacleCallback, this, _1));

    _bluetooth_sub = nh.subscribe<sensor_msgs::Joy>(
        "/input/bluetooth", 1,
        boost::bind(&JackalPlanner::bluetoothCallback, this, _1)); // Deadman switch

    _cmd_pub = nh.advertise<geometry_msgs::Twist>(
        "/output/command", 1);

    // Environment Reset
    if (_simulation)
    {
        _reset_simulation_pub = nh.advertise<std_msgs::Empty>("/lmpcc/reset_environment", 1);
        _reset_simulation_client = nh.serviceClient<std_srvs::Empty>("/gazebo/reset_world");
        _reset_ekf_client = nh.serviceClient<robot_localization::SetPose>("/set_pose");

        // Pedestrian simulator
        _ped_horizon_pub = nh.advertise<std_msgs::Int32>("/pedestrian_simulator/horizon", 1);
        _ped_integrator_step_pub = nh.advertise<std_msgs::Float32>("/pedestrian_simulator/integrator_step", 1);
        _ped_clock_frequency_pub = nh.advertise<std_msgs::Float32>("/pedestrian_simulator/clock_frequency", 1);
        _ped_start_client = nh.serviceClient<std_srvs::Empty>("/pedestrian_simulator/start");
    }

    // Roadmap reverse
    _reverse_roadmap_pub = nh.advertise<std_msgs::Empty>("/roadmap/reverse", 1);
}

void JackalPlanner::startEnvironment()
{
    LOG_INFO("Starting pedestrian simulator");
    for (int i = 0; i < 20; i++)
    {
        std_msgs::Int32 horizon_msg;
        horizon_msg.data = CONFIG["N"].as<int>();
        _ped_horizon_pub.publish(horizon_msg);

        std_msgs::Float32 integrator_step_msg;
        integrator_step_msg.data = CONFIG["integrator_step"].as<double>();
        _ped_integrator_step_pub.publish(integrator_step_msg);

        std_msgs::Float32 clock_frequency_msg;
        clock_frequency_msg.data = CONFIG["control_frequency"].as<double>();
        _ped_clock_frequency_pub.publish(clock_frequency_msg);

        std_srvs::Empty empty_msg;
        if (_ped_start_client.call(empty_msg))
            break;
        else
        {
            LOG_INFO_THROTTLE(3, "Waiting for pedestrian simulator to start");
            ros::Duration(1.0).sleep();
        }
    }
    _enable_output = true;
    LOG_INFO("Environment ready.");
}

void JackalPlanner::objectiveReached()
{
    if (_simulation)
    {
        if (_state.get("x") > 25.)
            reset();
    }
    else
    {
        bool reset_condition_forward_x = (_forward_x_experiment) && (_state.get("x") > 2.7);
        bool reset_condition_backward_x = (!_forward_x_experiment) && (_state.get("x") < -2.5);
        bool reset_condition = reset_condition_forward_x || reset_condition_backward_x;
        if (reset_condition)
        {
            reset();
            _forward_x_experiment = !_forward_x_experiment;
        }
    }
}

void JackalPlanner::loop(const ros::TimerEvent &event)
{
    (void)event;
    LOG_DEBUG("============= Loop =============");

    _benchmarker->start();

    objectiveReached();

    // Print the state
    _state.print();

    auto output = _planner->solveMPC(_state, _data);

    LOG_VALUE_DEBUG("Success", output.success);

    geometry_msgs::Twist cmd;
    if (_enable_output && output.success)
    {
        // Publish the command
        cmd.linear.x = _planner->getSolution(1, "v");  // = x1
        cmd.angular.z = _planner->getSolution(0, "w"); // = u0
        LOG_VALUE_DEBUG("Commanded v", cmd.linear.x);
        LOG_VALUE_DEBUG("Commanded w", cmd.angular.z);
    }
    else
    {
        cmd.linear.x = 0.0;
        cmd.angular.z = 0.0;
    }
    _cmd_pub.publish(cmd);
    _benchmarker->stop();

    _planner->visualize(_state, _data);
    visualize();

    LOG_DEBUG("============= End Loop =============");
}

void JackalPlanner::stateCallback(const nav_msgs::Odometry::ConstPtr &msg)
{
    _state.set("x", msg->pose.pose.position.x);
    _state.set("y", msg->pose.pose.position.y);
    _state.set("psi", RosTools::quaternionToAngle(msg->pose.pose.orientation));
    _state.set("v", std::sqrt(std::pow(msg->twist.twist.linear.x, 2.) + std::pow(msg->twist.twist.linear.y, 2.)));
}

void JackalPlanner::statePoseCallback(const geometry_msgs::PoseStamped::ConstPtr &msg)
{
    _state.set("x", msg->pose.position.x);
    _state.set("y", msg->pose.position.y);
    _state.set("psi", msg->pose.orientation.z);
    _state.set("v", msg->pose.position.z);
}

void JackalPlanner::goalCallback(const geometry_msgs::PoseStamped::ConstPtr &msg)
{
    LOG_DEBUG("Goal callback");
    _data.goal(0) = msg->pose.position.x;
    _data.goal(1) = msg->pose.position.y;
    _data.goal_received = true;
}

bool JackalPlanner::isPathTheSame(const nav_msgs::Path::ConstPtr &msg)
{
    // Check if the path is the same
    if (_data.reference_path.x.size() != msg->poses.size())
        return false;

    // Check up to the first two points
    int num_points = std::min(2, (int)_data.reference_path.x.size());
    for (int i = 0; i < num_points; i++)
    {
        if (!_data.reference_path.pointInPath(i, msg->poses[i].pose.position.x, msg->poses[i].pose.position.y))
            return false;
    }
    return true;
}

void JackalPlanner::pathCallback(const nav_msgs::Path::ConstPtr &msg)
{
    LOG_DEBUG("Path callback");

    if (isPathTheSame(msg))
        return;

    _data.reference_path.clear();

    for (auto &pose : msg->poses)
    {
        _data.reference_path.x.push_back(pose.pose.position.x);
        _data.reference_path.y.push_back(pose.pose.position.y);
    }
    _data.reference_path.psi.push_back(0.0);
    _planner->onDataReceived(_data, "reference_path");
}

void JackalPlanner::obstacleSimCallback(const mpc_planner_msgs::obstacle_array::ConstPtr &msg)
{
    _data.dynamic_obstacles.clear();

    for (auto &obstacle : msg->obstacles)
    {
        // Save the obstacle
        _data.dynamic_obstacles.emplace_back(
            obstacle.id,
            Eigen::Vector2d(obstacle.pose.position.x, obstacle.pose.position.y),
            RosTools::quaternionToAngle(obstacle.pose),
            CONFIG["obstacle_radius"].as<double>());
        auto &dynamic_obstacle = _data.dynamic_obstacles.back();

        if (obstacle.probabilities.size() == 0) // No Predictions!
            continue;

        // Save the prediction
        if (obstacle.probabilities.size() == 1) // One mode
        {
            const auto &mode = obstacle.gaussians[0];
            for (size_t k = 0; k < mode.mean.poses.size(); k++)
            {
                dynamic_obstacle.prediction.steps.emplace_back(
                    Eigen::Vector2d(mode.mean.poses[k].pose.position.x, mode.mean.poses[k].pose.position.y),
                    RosTools::quaternionToAngle(mode.mean.poses[k].pose.orientation),
                    mode.major_semiaxis[k],
                    mode.minor_semiaxis[k]);
            }

            if (mode.major_semiaxis.back() == 0. || !CONFIG["probabilistic"]["enable"].as<bool>())
                dynamic_obstacle.prediction.type = PredictionType::DETERMINISTIC;
            else
                dynamic_obstacle.prediction.type = PredictionType::GAUSSIAN;
        }
        else
        {
            ROSTOOLS_ASSERT(false, "Multiple modes not yet supported");
        }
    }
    ensureObstacleSize(_data.dynamic_obstacles, _state);
    _planner->onDataReceived(_data, "dynamic obstacles");
}

void JackalPlanner::obstacleCallback(const derived_object_msgs::ObjectArray::ConstPtr &msg)
{
    _data.dynamic_obstacles.clear();

    for (auto &object : msg->objects)
    {
        // Eigen::Vector2d position = Eigen::Vector2d(object.pose.position.x, object.pose.position.y);
        // Eigen::Vector2d velocity = Eigen::Vector2d(-object.twist.linear.y, object.twist.linear.x);

        double object_angle = RosTools::quaternionToAngle(object.pose.orientation) +
                              std::atan2(object.twist.linear.y, object.twist.linear.x) +
                              M_PI_2;

        // Save the obstacle
        _data.dynamic_obstacles.emplace_back(
            object.id,
            Eigen::Vector2d(object.pose.position.x, object.pose.position.y),
            object_angle,
            object.shape.dimensions[1]);
        auto &dynamic_obstacle = _data.dynamic_obstacles.back();

        // Read the orientation of the obstacle from the velocity estimation!
        dynamic_obstacle.angle = object_angle;

        // std::cout << object.id << ": " << object_angle << std::endl;

        geometry_msgs::Twist global_twist = object.twist;
        Eigen::Matrix2d rot_matrix = RosTools::rotationMatrixFromHeading(-RosTools::quaternionToAngle(object.pose.orientation));
        Eigen::Vector2d twist_out = rot_matrix * Eigen::Vector2d(global_twist.linear.x, global_twist.linear.y);

        // Eigen::Vector2d velocity = Eigen::Vector2d(object.twist.linear.x, object.twist.linear.y);

        // Make a constant velocity prediction
        dynamic_obstacle.prediction = getConstantVelocityPrediction(
            dynamic_obstacle.position,
            twist_out,
            CONFIG["integrator_step"].as<double>(),
            CONFIG["N"].as<int>());
    }
    ensureObstacleSize(_data.dynamic_obstacles, _state);

    _planner->onDataReceived(_data, "dynamic obstacles");
}

void JackalPlanner::bluetoothCallback(const sensor_msgs::Joy::ConstPtr &msg)
{
    if (msg->axes[2] < -0.9 && !_enable_output)
        LOG_INFO("Planning enabled (deadman switch pressed)");
    else if (msg->axes[2] > -0.9 && _enable_output)
        LOG_INFO("Deadmanswitch enabled (deadman switch released)");

    _enable_output = msg->axes[2] < -0.9;
}

void JackalPlanner::visualize()
{
    auto &publisher = VISUALS.getPublisher("angle");
    auto &line = publisher.getNewLine();

    line.addLine(Eigen::Vector2d(_state.get("x"), _state.get("y")),
                 Eigen::Vector2d(_state.get("x") + 1.0 * std::cos(_state.get("psi")), _state.get("y") + 1.0 * std::sin(_state.get("psi"))));
    publisher.publish();
}

void JackalPlanner::reset()
{
    LOG_INFO("Resetting");

    if (_simulation)
    {
        // Reset the environment
        for (int j = 0; j < 1; j++)
        {
            // ActuateBrake(5.0);
            ros::Duration(1.0 / CONFIG["control_frequency"].as<double>()).sleep();
        }

        _reset_simulation_client.call(_reset_msg);
        _reset_ekf_client.call(_reset_pose_msg);
        _reset_simulation_pub.publish(std_msgs::Empty());
    }
    else
    {
        std_msgs::Empty empty_msg;
        _reverse_roadmap_pub.publish(empty_msg);
    }

    _planner->reset(_state, _data);
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, ros::this_node::getName());

    ros::NodeHandle nh;
    auto jackal_planner = std::make_shared<JackalPlanner>(nh);
    VISUALS.init(&nh);

    ros::spin();

    return 0;
}

/*
// These are tests
Solver solver;

solver.setParameter(0, "acceleration", 3.0);
solver.setParameter(0, "angular_velocity", 4.0);
assert(solver.getParameter(0, "angular_velocity") == 4.0);
assert(solver.getParameter(0, "acceleration") == 3.0);

solver.setXinit("x", 10.0);
solver.setXinit("y", 20.0);
assert(solver._params.xinit[0] == 10.0);
assert(solver._params.xinit[1] == 20.0);

// Test run for the solver
solver.setXinit("x", 0.0);
solver.setXinit("y", 0.0);
solver.setXinit("v", 1.0);

for (int k = 0; k < solver.N; k++)
{
solver.setParameter(k, "acceleration", 1.0);
solver.setParameter(k, "angular_velocity", 3.0);
assert(solver.getParameter(k, "acceleration") == 1.0);
assert(solver.getParameter(k, "angular_velocity") == 3.0);
}

int exitflag = solver.solve();
std::cout << "Exit flag: " << exitflag << std::endl;
assert(exitflag == 1);

// Read the solution
std::cout << solver.getOutput(0, "a") << std::endl;
std::cout << solver.getOutput(0, "w") << std::endl;
std::cout << solver.getOutput(2, "x") << std::endl;
std::cout << solver.getOutput(2, "y") << std::endl;
*/