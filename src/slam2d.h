#ifndef __SLAM2D_H
#define __SLAM2D_H
#include "slam2d_pose_graph.h"

#include <iostream>
#include <Eigen/Eigen>
#include <sensor_msgs/MultiEchoLaserScan.h>
#include <sensor_msgs/LaserScan.h>
#include <nav_msgs/OccupancyGrid.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/visualization/cloud_viewer.h>
#include <opencv2/opencv.hpp>


using namespace std;
using namespace Eigen;
using namespace cv;


typedef pcl::PointXY PointType;

typedef struct
{
    double theta;
    Eigen::Vector2d t;

} state2d;

Eigen::Vector2d point2eigen(PointType p)
{
    Eigen::Vector2d pp;
    pp(0) = p.x;
    pp(1) = p.y;
    return pp;
}

PointType eigen2point(Eigen::Vector2d pp)
{
    PointType p;
    p.x = pp(0);
    p.y = pp(1);
    return p;
}

class slam2d
{
private:
    /* data */
public:
    slam2d(/* args */);
    ~slam2d();
    state2d state;
    state2d delta;
    double timestamp;
    nav_msgs::OccupancyGrid map2d;
    Mat cvmap2d;

    pcl::PointCloud<PointType> scan;
    pcl::PointCloud<PointType> scan_prev;

    pcl::PointCloud<PointType> scan_normal;
    bool cvmap_vis_enable = false;


    void readin_scan_data(const sensor_msgs::MultiEchoLaserScanConstPtr &msg);
    void readin_scan_data(const sensor_msgs::LaserScanConstPtr &msg);

    Vector2d world2map(Vector2d p);
    cv::Point2f world2map(cv::Point2f p);

    void update_scan_normal();
    void scan_match();
    void scan_map_match();
    VectorXd scan_map_match_cost(Vector3d pose);
    void update();
    void update_transform();
    void update_map();
    void cvmap2map();//convert cv map to map
};

slam2d::slam2d()
{
    state.t = Vector2d::Zero();
    state.theta = 0;
    map2d.header.frame_id = "odom";
    map2d.info.width = 2000;
    map2d.info.height = 2000;
    map2d.info.resolution = 0.1;
    map2d.info.origin.orientation.w = 1;
    map2d.info.origin.orientation.x = 0;
    map2d.info.origin.orientation.y = 0;
    map2d.info.origin.orientation.z = 0;
    map2d.info.origin.position.x = -0.5 * map2d.info.width * map2d.info.resolution;
    map2d.info.origin.position.y = -0.5 * map2d.info.height * map2d.info.resolution;
    map2d.info.origin.position.z = 0;
    map2d.data.resize(map2d.info.width * map2d.info.height);
    cvmap2d = Mat(map2d.info.width, map2d.info.height, CV_8SC1, -1);
    cvmap2map();
}

slam2d::~slam2d()
{
}

void slam2d::readin_scan_data(const sensor_msgs::MultiEchoLaserScanConstPtr &msg)
{
    timestamp = msg->header.stamp.toSec();
    scan.points.resize(msg->ranges.size());
    for (auto i = 0; i < msg->ranges.size(); i++)
    {
        float dist = msg->ranges[i].echoes[0]; //only first echo used for slam2d
        float theta = msg->angle_min + i * msg->angle_increment;
        scan.points[i].x = dist * cos(theta);
        scan.points[i].y = dist * sin(theta);
    }
    scan.width = scan.points.size();
    scan.height = 1;
    scan.is_dense = true;
}
void slam2d::readin_scan_data(const sensor_msgs::LaserScanConstPtr &msg)
{
    timestamp = msg->header.stamp.toSec();
    scan.points.resize(msg->ranges.size());
    for (auto i = 0; i < msg->ranges.size(); i++)
    {
        float dist = msg->ranges[i]; //only first echo used for slam2d
        float theta = msg->angle_min + i * msg->angle_increment;
        scan.points[i].x = dist * cos(theta);
        scan.points[i].y = dist * sin(theta);
    }
    scan.width = scan.points.size();
    scan.height = 1;
    scan.is_dense = true;
}

cv::Point2f slam2d::world2map(cv::Point2f p)
{
    cv::Point2f m;
    m.x = p.x / map2d.info.resolution;
    m.y = p.y / map2d.info.resolution;
    m.x += map2d.info.width * 0.5;
    m.y += map2d.info.height * 0.5;
    return m;
}

Vector2d slam2d::world2map(Vector2d p)
{
    Vector2d m;
    m = p / map2d.info.resolution;
    m(0) += map2d.info.width * 0.5;
    m(1) += map2d.info.height * 0.5;
    return m;
}
void slam2d::update_scan_normal()
{
    //compute normal of scan
    scan_normal.points.resize(scan.points.size());
    for (auto i = 1; i < scan.points.size(); i++)
    {
        PointType p1 = scan.points[i - 1];
        PointType p2 = scan.points[i];

        Eigen::Vector2d delta(p1.x - p2.x, p1.y - p2.y);
        Eigen::Vector2d normal(-delta(1), delta(0));
        normal /= normal.norm();
        scan_normal.points[i].x = normal(0);
        scan_normal.points[i].y = normal(1);
    }
}
void slam2d::scan_match()
{
    double pose[3] = {0};
    if (scan.points.size() && scan_prev.points.size())
    {

        Problem problem;
        //solve delta with ceres constraints
        pcl::KdTreeFLANN<PointType> kdtree;
        kdtree.setInputCloud(scan.makeShared());
        int K = 2; // K nearest neighbor search
        std::vector<int> index(K);
        std::vector<float> distance(K);
        //1. project scan_prev to scan

        Eigen::Matrix2d R;
        R(0, 0) = cos(delta.theta); R(0, 1) = -sin(delta.theta);
        R(1, 0) = sin(delta.theta); R(1, 1) = cos(delta.theta);
        Eigen::Vector2d dt = delta.t;
        //find nearest neighur
        for (int i = 0; i < scan_prev.points.size(); i++)
        {
            PointType search_point = scan_prev.points[i];
            //project search_point to current frame
            PointType search_point_predict = eigen2point(R * point2eigen(search_point) + dt);
            if (kdtree.nearestKSearch(search_point_predict, K, index, distance) == K)
            {
                //add constraints
                Eigen::Vector2d p = point2eigen(search_point);
                Eigen::Vector2d p1 = point2eigen(scan.points[index[0]]);
                Eigen::Vector2d p2 = point2eigen(scan.points[index[1]]);
                ceres::CostFunction *cost_function = lidar_edge_error::Create(p, p1, p2);
                problem.AddResidualBlock(cost_function,
                                         new CauchyLoss(0.5),
                                         pose);
            }
        }

        ceres::Solver::Options options;
        options.linear_solver_type = ceres::DENSE_SCHUR;
        options.minimizer_progress_to_stdout = false;

        ceres::Solver::Summary summary;
        ceres::Solve(options, &problem, &summary);
        std::cout << summary.FullReport() << "\n";

        printf("result: %lf, %lf, %lf\n", pose[0], pose[1], pose[2]);

        delta.theta = pose[0];
        delta.t(0) = pose[1];
        delta.t(1) = pose[2];

    }
}

VectorXd slam2d::scan_map_match_cost(Vector3d pose)
{
    VectorXd residual = VectorXd::Zero(scan.points.size());
    Eigen::Matrix2d R;
    R(0, 0) = cos(state.theta); R(0, 1) = -sin(state.theta);
    R(1, 0) = sin(state.theta); R(1, 1) =  cos(state.theta);
    //printf("cols: %d, rows: %d\n", cvmap2d.cols, cvmap2d.rows);
    for (int i = 0; i < scan.points.size(); i++)
    {
        Vector2d p = point2eigen(scan.points[i]);
        Vector2d pp = world2map(R * p + state.t);
        //cout << "pp: " << pp.transpose() << endl;
        if ((pp(0) <= 0) || (pp(0) >= cvmap2d.cols) || (pp(1) <= 0) || (pp(1) >= cvmap2d.rows))
        {
            residual[i] = 0;
            //printf("res:%f\n", residual[i]);
        } else 
        {
            //get value from map
            double x = pp(0);
            double y = pp(1);
            int x2 = ceil(x);
            int x1 = x2 - 1;
            int y2 = ceil(y);
            int y1 = y2 - 1;
            float p11 = cvmap2d.at<int8_t>(y1 * cvmap2d.cols + x1);
            float p12 = cvmap2d.at<int8_t>(y1 * cvmap2d.cols + x2);
            float p21 = cvmap2d.at<int8_t>(y2 * cvmap2d.cols + x1);
            float p22 = cvmap2d.at<int8_t>(y2 * cvmap2d.cols + x2);
            float p1 = (x - x1) * p12 + (x2 - x) * p11;
            float p2 = (x - x1) * p22 + (x2 - x) * p21;
            float ppp = (y - y1) * p2 + (y2 - y) * p1;
            residual[i] = 100 - ppp;
            //printf("i: %d, res:%f\n", i, residual[i]);
        }
    }
    return residual;
}

void slam2d::scan_map_match()
{
    float eps = 1e-2;
    Vector3d pose(state.theta, state.t(0), state.t(1));
    VectorXd r = scan_map_match_cost(pose);
    MatrixXd H = MatrixXd::Zero(scan.points.size(), 3);
    for(int i = 0; i < 3; i++)
    {
        Vector3d pose1 = pose;
        pose1(i) += eps;
        VectorXd r1 = scan_map_match_cost(pose1);
        Vector3d pose2 = pose;
        pose2(i) -= eps;
        VectorXd r2 = scan_map_match_cost(pose2);
        H.col(i) = (r1 - r2 ) / (2 * eps);
        //cout << "r1: " << r1.transpose() << endl;
        //cout << "r2: " << r2.transpose() << endl;
    }
    cout << "H: " << H.transpose() << endl;
    Vector3d dx = (H.transpose() * H).ldlt().solve(H.transpose() * r);
    cout << "dx: " << dx.transpose() << endl;
}

void slam2d::update_transform()
{

    Eigen::Matrix2d dR;
    dR(0, 0) = cos(delta.theta); dR(0, 1) = -sin(delta.theta);
    dR(1, 0) = sin(delta.theta); dR(1, 1) = cos(delta.theta);


    Eigen::Vector2d dt_inv = -dR.transpose() * delta.t;
    Eigen::Matrix2d dR_inv = dR.transpose();
    

    Eigen::Matrix2d R;
    R(0, 0) = cos(state.theta); R(0, 1) = -sin(state.theta);
    R(1, 0) = sin(state.theta); R(1, 1) =  cos(state.theta);
    state.theta += (-delta.theta);
    state.t += R * dt_inv;
}

void slam2d::update()
{
    static int cnt = 0;
    if (scan.points.size() && scan_prev.points.size())
    {
        scan_match();
        update_transform();
        //scan_map_match();
        update_map();
    }

    if (scan.points.size())
    {
        scan_prev = scan;
    }
    cnt++;
}

void slam2d::update_map()
{
    //update map with scan and state
    cv::Point2f tt;
    tt.x = state.t(0);
    tt.y = state.t(1);
    cv::Point2f origin = world2map(tt);
    Eigen::Matrix2d R;
    R(0, 0) = cos(state.theta); R(0, 1) = -sin(state.theta);
    R(1, 0) = sin(state.theta); R(1, 1) =  cos(state.theta);
    for (int i = 0; i < scan.points.size(); i++)
    {
        PointType p = scan.points[i];
        float dist = sqrtf(p.x * p.x + p.y * p.y);
        if (dist > 30) continue;
        Eigen::Vector2d pp = world2map(R * point2eigen(p) + state.t);
        cv:Point2f ppp(pp(0), pp(1));
        
        cv::line(cvmap2d, origin, ppp, 100, 1, 4, 0);
        cv::circle(cvmap2d, ppp, 1, 0, 1, 4, 0);
    }
    cvmap2map();
}

void slam2d::cvmap2map()
{
    for (int i = 0; i < cvmap2d.rows; i++)
    {
        for(int j = 0; j < cvmap2d.cols; j++)
        {
            map2d.data[i * map2d.info.width + j] = cvmap2d.at<int8_t>(i, j);
        }
    }
    if (cvmap_vis_enable)
    {
        imshow("cvmap2d", cvmap2d);
        waitKey(2);
    }
}
#endif
