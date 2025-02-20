#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/header.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include <tf2/transform_datatypes.h>
#include <Eigen/Dense>
#include <mutex>

using namespace Eigen;
using namespace std;

//-----------------------------
// Global variables
//-----------------------------
#define PI 3.14159265

bool enableFilter;
bool enableImu;
bool enableWheel;
bool enableLidar;

float lidarG;
float wheelG;
float imuG;

std::string filterFreq;

std::mutex mtx;

//-----------------------------
// LiDAR Odometry class
//-----------------------------
class AdaptiveFilter : public rclcpp::Node {

private:
    // Subscriber
    rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr subImu;
    rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr subWheelOdometry;
    rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr subLaserOdometry;

    // Publisher
    rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr pubFilteredOdometry;
    rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr pubIndLiDARMeasurement;

    // header
    std_msgs::msg::Header headerI;
    std_msgs::msg::Header headerW;
    std_msgs::msg::Header headerL;

    // TF 
    geometry_msgs::msg::TransformStamped filteredOdometryTrans;
    std::shared_ptr<tf2_ros::TransformBroadcaster> tfBroadcasterfiltered;

    // filtered odom
    nav_msgs::msg::Odometry filteredOdometry;
    nav_msgs::msg::Odometry indLiDAROdometry;

    // Measure
    Eigen::VectorXd imuMeasure, wheelMeasure, lidarMeasure, lidarMeasureL;

    // Measure Covariance
    Eigen::MatrixXd E_imu, E_wheel, E_lidar, E_lidarL, E_pred;

    // States and covariances
    Eigen::VectorXd X, V;
    Eigen::MatrixXd P, PV;

    // pose and velocities
    Eigen::VectorXd pose, velocities;

    // Times
    double imuTimeLast;
    double wheelTimeLast;
    double lidarTimeLast;

    double imuTimeCurrent;
    double wheelTimeCurrent;
    double lidarTimeCurrent;

    double imu_dt;
    double wheel_dt;
    double lidar_dt;

    // imu varibles
    struct bias {
        double x;
        double y;
        double z;
    } bias_linear_acceleration, bias_angular_velocity;

    // number of state or measure vectors
    int N_STATES = 12;
    int N_IMU = 9; 
    int N_WHEEL = 2; 
    int N_LIDAR = 6;
    
    // boolean
    bool imuActivated;
    bool wheelActivated;
    bool lidarActivated;
    bool imuNew;
    bool wheelNew;
    bool lidarNew;
    bool velComp;

    // adaptive covariance
    double nCorner, nSurf; 
    double Gx, Gy, Gz, Gphi, Gtheta, Gpsi;
    float l_min;

public:
    AdaptiveFilter(const std::string &node_name) : Node(node_name) {
        // Subscriber
        subImu = this->create_subscription<sensor_msgs::msg::Imu>(
            "/imu", 50, std::bind(&AdaptiveFilter::imuHandler, this, std::placeholders::_1));
        subWheelOdometry = this->create_subscription<nav_msgs::msg::Odometry>(
            "/odom", 5, std::bind(&AdaptiveFilter::wheelOdometryHandler, this, std::placeholders::_1));
        subLaserOdometry = this->create_subscription<nav_msgs::msg::Odometry>(
            "/odom_rf2o", 5, std::bind(&AdaptiveFilter::laserOdometryHandler, this, std::placeholders::_1));
        
        // Publisher
        pubFilteredOdometry = this->create_publisher<nav_msgs::msg::Odometry>("/ekf_loam/filter_odom_to_init", 5);
        pubIndLiDARMeasurement = this->create_publisher<nav_msgs::msg::Odometry>("/indirect_lidar_measurement", 5);

        // TF Broadcaster
        tfBroadcasterfiltered = std::make_shared<tf2_ros::TransformBroadcaster>(this);

        // Initialization
        allocateMemory();
        initialization();
    }

    //------------------
    // Auxliar functions
    //------------------
    void allocateMemory() {
        imuMeasure.resize(N_IMU);
        wheelMeasure.resize(N_WHEEL);
        lidarMeasure.resize(N_LIDAR);
        lidarMeasureL.resize(N_LIDAR);

        E_imu.resize(N_IMU,N_IMU);
        E_wheel.resize(N_WHEEL,N_WHEEL);
        E_lidar.resize(N_LIDAR,N_LIDAR);
        E_lidarL.resize(N_LIDAR,N_LIDAR);
        E_pred.resize(N_STATES,N_STATES);

        X.resize(N_STATES);
        P.resize(N_STATES,N_STATES);

        V.resize(N_STATES);
        PV.resize(N_STATES,N_STATES);
    }

    void initialization() {
        // times
        imuTimeLast = 0;
        lidarTimeLast = 0;
        wheelTimeLast = 0;

        imuTimeCurrent = 0;
        lidarTimeCurrent = 0;
        wheelTimeCurrent = 0;

        // auxliar 
        bias_linear_acceleration.x = 0.0001;
        bias_linear_acceleration.y = 0.0001;
        bias_linear_acceleration.z = 0.0001;

        bias_angular_velocity.x = 0.00000001;
        bias_angular_velocity.y = 0.00000001;
        bias_angular_velocity.z = 0.00000001;

        wheel_dt = 0.05;
        lidar_dt = 0.1;

        // boolean
        imuActivated = false;
        lidarActivated = false;
        wheelActivated = false;

        imuNew = false;
        wheelNew = false;
        lidarNew = false;

        velComp = false;

        // matrices and vectors
        imuMeasure = Eigen::VectorXd::Zero(N_IMU);
        wheelMeasure = Eigen::VectorXd::Zero(N_WHEEL);
        lidarMeasure = Eigen::VectorXd::Zero(N_LIDAR);
        lidarMeasureL = Eigen::VectorXd::Zero(N_LIDAR);
        
        E_imu = Eigen::MatrixXd::Zero(N_IMU,N_IMU);
        E_lidar = Eigen::MatrixXd::Zero(N_LIDAR,N_LIDAR);
        E_lidarL = Eigen::MatrixXd::Zero(N_LIDAR,N_LIDAR);
        E_wheel = Eigen::MatrixXd::Zero(N_WHEEL,N_WHEEL);
        E_pred = Eigen::MatrixXd::Zero(N_STATES,N_STATES);

        // state initial
        X = Eigen::VectorXd::Zero(N_STATES);
        P = Eigen::MatrixXd::Zero(N_STATES,N_STATES);
        V = Eigen::VectorXd::Zero(N_STATES);

        // covariance initial
        P(0,0) = 0.1;   // x
        P(1,1) = 0.1;   // y
        P(2,2) = 0.1;   // z
        P(3,3) = 0.1;   // roll
        P(4,4) = 0.1;   // pitch
        P(5,5) = 0.1;   // yaw
        P(6,6) = 0.1;   // vx
        P(7,7) = 0.1;   // vy
        P(8,8) = 0.1;   // vz
        P(9,9) = 0.1;   // wx
        P(10,10) = 0.1;   // wy
        P(11,11) = 0.1;   // wz

        // Fixed prediction covariance
        E_pred.block(6,6,6,6) = 0.01*P.block(6,6,6,6);

        // adptive covariance constants
        nCorner = 500.0; // 7000
        nSurf = 5000;    // 5400
        
        Gz = 0.0048;    // x [m]
        Gx = 0.0022;    // y [m]
        Gy = 0.0016;    // z [m]
        Gpsi = 0.0044;  // phi [rad]
        Gphi = 0.0052;  // theta [rad]
        Gtheta = 0.005; // psi [rad]

        l_min = 0.005;
    }

    MatrixXd adaptive_covariance(double fCorner, double fSurf) {
        Eigen::MatrixXd Q(6,6);
        double cov_x, cov_y, cov_z, cov_phi, cov_psi, cov_theta;
        
        // heuristic
        cov_x     = (nCorner - min(fCorner,nCorner))/nCorner + l_min;
        cov_y     = (nCorner - min(fCorner,nCorner))/nCorner + l_min;
        cov_psi = (nCorner - min(fCorner,nCorner))/nCorner + l_min;
        cov_z     = (nSurf - min(fSurf,nSurf))/nSurf + l_min;
        cov_phi   = (nSurf - min(fSurf,nSurf))/nSurf + l_min;
        cov_theta   = (nSurf - min(fSurf,nSurf))/nSurf + l_min;
        
        Q = MatrixXd::Zero(6,6);
        float b = lidarG/1.0;
        float c = lidarG/1.0;
        Q(0,0) = b*Gx*cov_x;
        Q(1,1) = c*Gy*cov_y;
        Q(2,2) = b*Gz*cov_z;
        Q(3,3) = c*Gphi*cov_phi;
        Q(4,4) = b*Gtheta*cov_theta;
        Q(5,5) = c*Gpsi*cov_psi;

        return Q;
    }

    //-----------------
    // predict function
    //-----------------
    void prediction_stage(double dt) {
        Eigen::MatrixXd F(N_STATES,N_STATES);

        // jacobian's computation
        F = jacobian_state(X, dt);

        // Priori state and covariance estimated
        X = f_prediction_model(X, dt);

        // Priori covariance
        P = F*P*F.transpose() + E_pred;
    }

    //-----------------
    // correction stage
    //-----------------
    void correction_wheel_stage(double dt) {
        Eigen::VectorXd Y(N_WHEEL), hx(N_WHEEL);
        Eigen::MatrixXd H(N_WHEEL,N_STATES), K(N_STATES,N_WHEEL), E(N_WHEEL,N_WHEEL), S(N_WHEEL,N_WHEEL);

        // measure model of wheel odometry (only foward linear velocity)
        hx(0) = X(6);
        hx(1) = X(11);
        // measurement
        Y = wheelMeasure;

        // Jacobian of hx with respect to the states
        H = Eigen::MatrixXd::Zero(N_WHEEL,N_STATES);
        H(0,6) = 1; 
        H(1,11) = 1;

        // covariance matrices
        E << E_wheel;

        // Kalman's gain
        S = H*P*H.transpose() + E;
        K = P*H.transpose()*S.inverse();

        // correction
        X = X + K*(Y - hx);
        P = P - K*H*P;
    }

    void correction_imu_stage(double dt) {
        Eigen::Matrix3d S, E;
        Eigen::Vector3d Y, hx;
        Eigen::MatrixXd H(3,N_STATES), K(N_STATES,3);

        // measure model
        hx = X.block(3,0,3,1);
        // wheel measurement
        Y = imuMeasure.block(6,0,3,1);

        // Jacobian of hx with respect to the states
        H = Eigen::MatrixXd::Zero(3,N_STATES);
        H.block(0,3,3,3) = Eigen::MatrixXd::Identity(3,3);

        // covariance matrices
        E = E_imu.block(6,6,3,3);

        // Kalman's gain
        S = H*P*H.transpose() + E;
        K = P*H.transpose()*S.inverse();

        // correction
        X = X + K*(Y - hx);
        P = P - K*H*P;
    }

    void correction_lidar_stage(double dt) {
        Eigen::MatrixXd K(N_STATES,N_LIDAR), S(N_LIDAR,N_LIDAR), G(N_LIDAR,N_LIDAR), Gl(N_LIDAR,N_LIDAR), Q(N_LIDAR,N_LIDAR);
        Eigen::VectorXd Y(N_LIDAR), hx(N_LIDAR);
        Eigen::MatrixXd H(N_LIDAR,N_STATES); 

        // measure model
        hx = X.block(6,0,6,1);
        // wheel measurement
        Y = indirect_lidar_measurement(lidarMeasure, lidarMeasureL, dt);

        // Jacobian of hx with respect to the states
        H = Eigen::MatrixXd::Zero(N_LIDAR,N_STATES);
        H.block(0,6,6,6) = Eigen::MatrixXd::Identity(N_LIDAR,N_LIDAR);

        // Error propagation
        G = jacobian_lidar_measurement(lidarMeasure, lidarMeasureL, dt);
        Gl = jacobian_lidar_measurementL(lidarMeasure, lidarMeasureL, dt);

        Q =  G*E_lidar*G.transpose() + Gl*E_lidarL*Gl.transpose();

        // data save 
        publish_indirect_lidar_measurement(Y, Q);        

        // Kalman's gain
        S = H*P*H.transpose() + Q;
        K = P*H.transpose()*S.inverse();

        // correction
        X = X + K*(Y - hx);
        P = P - K*H*P;

        // last measurement
        lidarMeasureL = lidarMeasure;
        E_lidarL = E_lidar;
    }
    
    //---------
    // Models
    //---------
    VectorXd f_prediction_model(VectorXd x, double dt) { 
        // state: {x, y, z, roll, pitch, yaw, vx, vy, vz, wx, wy, wz}
        //        {         (world)         }{        (body)        }
        Eigen::Matrix3d R, Rx, Ry, Rz, J;
        Eigen::VectorXd xp(N_STATES);
        Eigen::MatrixXd A(6,6);    

        // Rotation matrix
        Rx = Eigen::AngleAxisd(x(3), Eigen::Vector3d::UnitX());
        Ry = Eigen::AngleAxisd(x(4), Eigen::Vector3d::UnitY());
        Rz = Eigen::AngleAxisd(x(5), Eigen::Vector3d::UnitZ());
        R = Rz*Ry*Rx;
        
        // Jacobian matrix
        J << 1.0, sin(x(3))*tan(x(4)), cos(x(3))*tan(x(4)),
             0.0, cos(x(3)), -sin(x(3)),
             0.0, sin(x(3))/cos(x(4)), cos(x(3))/cos(x(4));
        
        // model
        A = Eigen::MatrixXd::Identity(6,6);
        A.block(0,0,3,3) = R;
        A.block(3,3,3,3) = J;

        xp.block(0,0,6,1) = x.block(0,0,6,1) + A*x.block(6,0,6,1)*dt;
        xp.block(6,0,6,1) = x.block(6,0,6,1);

        return xp;
    }

    VectorXd indirect_lidar_measurement(VectorXd u, VectorXd ul, double dt) {
        Eigen::Matrix3d R, Rx, Ry, Rz, J;
        Eigen::VectorXd up(N_LIDAR), u_diff(N_LIDAR);  
        Eigen::MatrixXd A(N_LIDAR,N_LIDAR);  

        // Rotation matrix
        Rx = Eigen::AngleAxisd(ul(3), Eigen::Vector3d::UnitX());
        Ry = Eigen::AngleAxisd(ul(4), Eigen::Vector3d::UnitY());
        Rz = Eigen::AngleAxisd(ul(5), Eigen::Vector3d::UnitZ());
        R = Rz*Ry*Rx;
        
        // Jacobian matrix
        J << 1.0, sin(ul(3))*tan(ul(4)), cos(ul(3))*tan(ul(4)),
             0.0, cos(ul(3)), -sin(ul(3)),
             0.0, sin(ul(3))/cos(ul(4)), cos(ul(3))/cos(ul(4));
        
        // model
        u_diff.block(0,0,3,1) = (u.block(0,0,3,1) - ul.block(0,0,3,1));
        u_diff(3) = atan2(sin(u(3) - ul(3)),cos(u(3) - ul(3)));
        u_diff(4) = atan2(sin(u(4) - ul(4)),cos(u(4) - ul(4)));
        u_diff(5) = atan2(sin(u(5) - ul(5)),cos(u(5) - ul(5)));

        A = Eigen::MatrixXd::Zero(N_LIDAR,N_LIDAR);
        A.block(0,0,3,3) = R.transpose();
        A.block(3,3,3,3) = J.inverse();

        up = A*u_diff/dt;

        return up;

    }

    //----------
    // Jacobians
    //----------
    MatrixXd jacobian_state(VectorXd x, double dt) {
        Eigen::MatrixXd J(N_STATES,N_STATES);
        Eigen::VectorXd f0(N_STATES), f1(N_STATES), x_plus(N_STATES);

        f0 = f_prediction_model(x, dt);

        double delta = 0.0001;
        for (size_t i = 0; i < N_STATES; i++){
            x_plus = x;
            x_plus(i) = x_plus(i) + delta;

            f1 = f_prediction_model(x_plus, dt);
           
            J.block(0,i,N_STATES,1) = (f1 - f0)/delta;       
            J(3,i) = sin(f1(3) - f0(3))/delta;
            J(4,i) = sin(f1(4) - f0(4))/delta;
            J(5,i) = sin(f1(5) - f0(5))/delta; 
        }

        return J;
    }

    MatrixXd jacobian_lidar_measurement(VectorXd u, VectorXd ul, double dt) { 
        Eigen::MatrixXd J(N_LIDAR,N_LIDAR);
        Eigen::VectorXd f0(N_LIDAR), f1(N_LIDAR), u_plus(N_LIDAR);

        f0 = indirect_lidar_measurement(u, ul, dt);

        double delta = 0.0000001;
        for (size_t i = 0; i < N_LIDAR; i++){
            u_plus = u;
            u_plus(i) = u_plus(i) + delta;

            f1 = indirect_lidar_measurement(u_plus, ul, dt);
           
            J.block(0,i,N_LIDAR,1) = (f1 - f0)/delta;       
            J(3,i) = sin(f1(3) - f0(3))/delta;
            J(4,i) = sin(f1(4) - f0(4))/delta;
            J(5,i) = sin(f1(5) - f0(5))/delta; 
        }

        return J;
    }

    MatrixXd jacobian_lidar_measurementL(VectorXd u, VectorXd ul, double dt) { 
        Eigen::MatrixXd J(N_LIDAR,N_LIDAR);
        Eigen::VectorXd f0(N_LIDAR), f1(N_LIDAR), ul_plus(N_LIDAR);

        f0 = indirect_lidar_measurement(u, ul, dt);

        double delta = 0.0000001;
        for (size_t i = 0; i < N_LIDAR; i++){
            ul_plus = ul;
            ul_plus(i) = ul_plus(i) + delta;

            f1 = indirect_lidar_measurement(u, ul_plus, dt);
           
            J.block(0,i,N_LIDAR,1) = (f1 - f0)/delta;       
            J(3,i) = sin(f1(3) - f0(3))/delta;
            J(4,i) = sin(f1(4) - f0(4))/delta;
            J(5,i) = sin(f1(5) - f0(5))/delta; 
        }

        return J;
    }

    //----------
    // callbacks
    //----------
    void imuHandler(const sensor_msgs::msg::Imu::SharedPtr imuIn) {
        double timeL = this->get_clock()->now().seconds();

        // time
        if (imuActivated){
            imuTimeLast = imuTimeCurrent;
            imuTimeCurrent = imuIn->header.stamp.sec + imuIn->header.stamp.nanosec * 1e-9;
        } else {
            imuTimeCurrent = imuIn->header.stamp.sec + imuIn->header.stamp.nanosec * 1e-9;
            imuTimeLast = imuTimeCurrent + 0.01;
            imuActivated = true;
        }       

        // roll, pitch and yaw 
        double roll, pitch, yaw;
        geometry_msgs::msg::Quaternion orientation = imuIn->orientation;
        tf2::Matrix3x3(tf2::Quaternion(orientation.x, orientation.y, orientation.z, orientation.w)).getRPY(roll, pitch, yaw);

        // measure
        imuMeasure.block(0,0,3,1) << imuIn->linear_acceleration.x, imuIn->linear_acceleration.y, imuIn->linear_acceleration.z;
        imuMeasure.block(3,0,3,1) << imuIn->angular_velocity.x, imuIn->angular_velocity.y, imuIn->angular_velocity.z; 
        imuMeasure.block(6,0,3,1) << roll, pitch, yaw;

        // covariance
        E_imu.block(0,0,3,3) << imuIn->linear_acceleration_covariance[0], imuIn->linear_acceleration_covariance[1], imuIn->linear_acceleration_covariance[2],
                                imuIn->linear_acceleration_covariance[3], imuIn->linear_acceleration_covariance[4], imuIn->linear_acceleration_covariance[5],
                                imuIn->linear_acceleration_covariance[6], imuIn->linear_acceleration_covariance[7], imuIn->linear_acceleration_covariance[8];
        E_imu.block(3,3,3,3) << imuIn->angular_velocity_covariance[0], imuIn->angular_velocity_covariance[1], imuIn->angular_velocity_covariance[2],
                                imuIn->angular_velocity_covariance[3], imuIn->angular_velocity_covariance[4], imuIn->angular_velocity_covariance[5],
                                imuIn->angular_velocity_covariance[6], imuIn->angular_velocity_covariance[7], imuIn->angular_velocity_covariance[8];
        E_imu.block(6,6,3,3) << imuIn->orientation_covariance[0], imuIn->orientation_covariance[1], imuIn->orientation_covariance[2],
                                imuIn->orientation_covariance[3], imuIn->orientation_covariance[4], imuIn->orientation_covariance[5],
                                imuIn->orientation_covariance[6], imuIn->orientation_covariance[7], imuIn->orientation_covariance[8];

        E_imu.block(6,6,3,3) = imuG*E_imu.block(6,6,3,3);

        // time
        imu_dt = imuTimeCurrent - imuTimeLast;
        imu_dt = 0.01;

        // header
        double timediff = this->get_clock()->now().seconds() - timeL + imuTimeCurrent;
        headerI = imuIn->header;
        headerI.stamp = rclcpp::Time(static_cast<int64_t>(timediff * 1e9));

        imuNew = true;
    }

    void wheelOdometryHandler(const nav_msgs::msg::Odometry::SharedPtr wheelOdometry) {
        double timeL = this->get_clock()->now().seconds();

        // time
        if (wheelActivated){
            wheelTimeLast = wheelTimeCurrent;
            wheelTimeCurrent = wheelOdometry->header.stamp.sec + wheelOdometry->header.stamp.nanosec * 1e-9;
        } else {
            wheelTimeCurrent = wheelOdometry->header.stamp.sec + wheelOdometry->header.stamp.nanosec * 1e-9;
            wheelTimeLast = wheelTimeCurrent + 0.05;
            wheelActivated = true;
        } 

        // measure
        wheelMeasure << 1.0*wheelOdometry->twist.twist.linear.x, wheelOdometry->twist.twist.angular.z;

        // covariance
        E_wheel(0,0) = wheelG*wheelOdometry->twist.covariance[0];
        E_wheel(1,1) = 100*wheelOdometry->twist.covariance[35];

        // time
        wheel_dt = wheelTimeCurrent - wheelTimeLast;
        wheel_dt = 0.05;

        // header
        double timediff = this->get_clock()->now().seconds() - timeL + wheelTimeCurrent;
        headerW = wheelOdometry->header;
        headerW.stamp = rclcpp::Time(static_cast<int64_t>(timediff * 1e9));


        // new measure
        wheelNew = true;
    }

    void laserOdometryHandler(const nav_msgs::msg::Odometry::SharedPtr laserOdometry) {
        double timeL = this->get_clock()->now().seconds();

        if (lidarActivated){
            lidarTimeLast = lidarTimeCurrent;
            lidarTimeCurrent = laserOdometry->header.stamp.sec + laserOdometry->header.stamp.nanosec * 1e-9;
        } else {
            lidarTimeCurrent = laserOdometry->header.stamp.sec + laserOdometry->header.stamp.nanosec * 1e-9;
            lidarTimeLast = lidarTimeCurrent + 0.1;
            lidarActivated = true;
        }  
        
        // roll, pitch and yaw 
        double roll, pitch, yaw;
        geometry_msgs::msg::Quaternion orientation = laserOdometry->pose.pose.orientation;
        tf2::Matrix3x3(tf2::Quaternion(orientation.x, orientation.y, orientation.z, orientation.w)).getRPY(roll, pitch, yaw);

        lidarMeasure.block(0,0,3,1) << laserOdometry->pose.pose.position.x, laserOdometry->pose.pose.position.y, laserOdometry->pose.pose.position.z;
        lidarMeasure.block(3,0,3,1) << roll, pitch, yaw;    

        // covariance
        double corner = double(laserOdometry->twist.twist.linear.x);
        double surf = double(laserOdometry->twist.twist.angular.x); 

        E_lidar = adaptive_covariance(corner, surf);

        // time
        lidar_dt = lidarTimeCurrent - lidarTimeLast;
        lidar_dt = 0.1;

        // header
        double timediff = this->get_clock()->now().seconds() - timeL + lidarTimeCurrent;
        headerL = laserOdometry->header;
        headerL.stamp = rclcpp::Time(static_cast<int64_t>(timediff * 1e9));

        
        //New measure
        lidarNew = true;
    }

    //----------
    // publisher
    //----------
    void publish_odom(char model) {
        switch(model) {
            case 'i':
                filteredOdometry.header = headerI;
                break;
            case 'w':
                filteredOdometry.header = headerW;
                break;
            case 'l':
                filteredOdometry.header = headerL;
        }

        filteredOdometry.header.frame_id = "chassis_init";
        filteredOdometry.child_frame_id = "ekf_odom_frame";

        // geometry_msgs::msg::Quaternion geoQuat = tf2::toMsg(tf2::Quaternion(X(3), X(4), X(5)));
        
        // Create quaternion from roll, pitch, yaw
        tf2::Quaternion q;
        q.setRPY(X(3), X(4), X(5));
        geometry_msgs::msg::Quaternion geoQuat = tf2::toMsg(q);

        // pose
        filteredOdometry.pose.pose.orientation.x = geoQuat.x;
        filteredOdometry.pose.pose.orientation.y = geoQuat.y;
        filteredOdometry.pose.pose.orientation.z = geoQuat.z;
        filteredOdometry.pose.pose.orientation.w = geoQuat.w;
        filteredOdometry.pose.pose.position.x = X(0); 
        filteredOdometry.pose.pose.position.y = X(1);
        filteredOdometry.pose.pose.position.z = X(2);

        // pose convariance
        int k = 0;
        for (int i = 0; i < 6; i++){
            for (int j = 0; j < 6; j++){
                filteredOdometry.pose.covariance[k] = P(i,j);
                k++;
            }
        }      

        // twist
        filteredOdometry.twist.twist.linear.x = X(6);
        filteredOdometry.twist.twist.linear.y = X(7);
        filteredOdometry.twist.twist.linear.z = X(8);
        filteredOdometry.twist.twist.angular.x = X(9);
        filteredOdometry.twist.twist.angular.y = X(10);
        filteredOdometry.twist.twist.angular.z = X(11);

        // twist convariance
        k = 0;
        for (int i = 6; i < 12; i++){
            for (int j = 6; j < 12; j++){
                filteredOdometry.twist.covariance[k] = P(i,j);
                k++;
            }
        } 

        pubFilteredOdometry->publish(filteredOdometry);
    }

    void publish_indirect_lidar_measurement(VectorXd y, MatrixXd Pi) {
        indLiDAROdometry.header = headerL;
        indLiDAROdometry.header.frame_id = "chassis_init";
        indLiDAROdometry.child_frame_id = "ind_lidar_frame";

        // twist
        indLiDAROdometry.twist.twist.linear.x = y(0);
        indLiDAROdometry.twist.twist.linear.y = y(1);
        indLiDAROdometry.twist.twist.linear.z = y(2);
        indLiDAROdometry.twist.twist.angular.x = y(3);
        indLiDAROdometry.twist.twist.angular.y = y(4);
        indLiDAROdometry.twist.twist.angular.z = y(5);

        // twist convariance
        int k = 0;
        for (int i = 0; i < 6; i++){
            for (int j = 0; j < 6; j++){
                indLiDAROdometry.twist.covariance[k] = Pi(i,j);
                k++;
            }
        } 

        pubIndLiDARMeasurement->publish(indLiDAROdometry);
    }

    //----------
    // runs
    //----------
    void run() {
        // rate
        rclcpp::Rate r(200);        

        double t_last = this->get_clock()->now().seconds();
        double t_now;
        double dt_now;

        while (rclcpp::ok()) {
            // Prediction
            if (enableFilter){
                // prediction stage
                t_now = this->get_clock()->now().seconds();
                dt_now = t_now - t_last;
                t_last = t_now;

                prediction_stage(dt_now);
                
                // publish state
                if (filterFreq == "p"){
                    publish_odom('p');
                }
            }

            // Correction IMU
            if (enableFilter && enableImu && imuActivated && imuNew){
                // correction stage
                correction_imu_stage(imu_dt);

                // publish state
                if (filterFreq == "i"){
                    publish_odom('i');
                }

                // control variable
                imuNew = false;
            }

            // Correction wheel
            if (enableFilter && enableWheel && wheelActivated && wheelNew){                
                // correction stage
                correction_wheel_stage(wheel_dt);

                if (filterFreq == "w"){
                    publish_odom('w');
                }                

                // control variable
                wheelNew = false;
            }

            // Correction LiDAR
            if (enableFilter && enableLidar && lidarActivated && lidarNew){                
                // correction stage
                correction_lidar_stage(lidar_dt);

                // publish state
                if (filterFreq == "l"){
                    publish_odom('l');
                }

                // control variable
                lidarNew = false;
            }
            
            rclcpp::spin_some(this->get_node_base_interface());
            r.sleep();        
        }
    }
};


//-----------------------------
// Main 
//-----------------------------
int main(int argc, char** argv) {
    rclcpp::init(argc, argv);

    //Parameters init:    
    auto nh_ = rclcpp::Node::make_shared("adaptive_filter");
    try {
        nh_->declare_parameter("/ekf_loam/enableFilter", true);
        nh_->declare_parameter("/adaptive_filter/enableImu", true);
        nh_->declare_parameter("/adaptive_filter/enableWheel", true);
        nh_->declare_parameter("/adaptive_filter/enableLidar", true);
        nh_->declare_parameter("/adaptive_filter/filterFreq", std::string("l"));

        nh_->declare_parameter("/adaptive_filter/lidarG", float(1000));
        nh_->declare_parameter("/adaptive_filter/wheelG", float(0.05));
        nh_->declare_parameter("/adaptive_filter/imuG", float(0.1));

        nh_->get_parameter("/ekf_loam/enableFilter", enableFilter);
        nh_->get_parameter("/adaptive_filter/enableImu", enableImu);
        nh_->get_parameter("/adaptive_filter/enableWheel", enableWheel);
        nh_->get_parameter("/adaptive_filter/enableLidar", enableLidar);
        nh_->get_parameter("/adaptive_filter/filterFreq", filterFreq);

        nh_->get_parameter("/adaptive_filter/lidarG", lidarG);
        nh_->get_parameter("/adaptive_filter/wheelG", wheelG);
        nh_->get_parameter("/adaptive_filter/imuG", imuG);
    } catch (int e) {
        RCLCPP_INFO(nh_->get_logger(), "Exception occurred when importing parameters in Adaptive Filter Node. Exception Nr. %d", e);
    }

    std::string node_name = "adaptive_filter";
    if (argc > 1) {
        node_name = argv[1];
    }

    auto af = std::make_shared<AdaptiveFilter>(node_name);

    if (enableFilter) {
        RCLCPP_INFO(rclcpp::get_logger(node_name), "Adaptive Filter Started.");
        // runs
        af->run();
    } else {
        RCLCPP_INFO(rclcpp::get_logger(node_name), "Adaptive Filter Stopped.");
    }

    rclcpp::spin(af);
    rclcpp::shutdown();
    return 0;
}
