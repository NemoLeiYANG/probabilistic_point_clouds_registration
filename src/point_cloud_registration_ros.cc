#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <ros/ros.h>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#include <pcl/common/transforms.h>
#include <pcl/io/pcd_io.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/visualization/cloud_viewer.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <ros/console.h>
#include <pcl/filters/filter.h>
#include "point_cloud_registration/point_cloud_registration.h"

typedef pcl::PointXYZ PointType;

int main(int argc, char** argv) {
  std::string node_name = "aslam_map_merging";
  ros::init(argc, argv, node_name, ros::init_options::AnonymousName);

  int dim_neighborhood;
  ros::param::param<int>("~dim_neighborhood", dim_neighborhood, 10);
  ROS_INFO("The dimension of neighborhood: %d", dim_neighborhood);

  bool use_gaussian;
  double dof;

  ros::param::param<bool>("~use_gaussian", use_gaussian, false);
  if (use_gaussian) {
    ROS_INFO("Using gaussian model");
    dof = std::numeric_limits<double>::infinity();
  } else {
    ros::param::param<double>("~dof", dof, 5);
    ROS_INFO("Degree of freedom of t-distribution: %f", dof);
  }
  double radius;
  ros::param::param<double>("~radius", radius, 3);
  ROS_INFO("Radius of the neighborhood search: %f", radius);

  ROS_INFO("Loading sparse point cloud");
  std::string sparse_file_name;
  pcl::PointCloud<PointType>::Ptr sparse_cloud =
      boost::make_shared<pcl::PointCloud<PointType>>();
  if (ros::param::get("~sparse_file_name", sparse_file_name) == false ||
      pcl::io::loadPCDFile<PointType>(sparse_file_name, *sparse_cloud) == -1) {
    ROS_INFO("Could not load sparse cloud, closing...");
    exit(1);
  } else {
    ROS_INFO("Using file %s as sparse point cloud", sparse_file_name.c_str());
    std::vector<int> tmp_indices;
    pcl::removeNaNFromPointCloud(*sparse_cloud, *sparse_cloud, tmp_indices);
    ROS_INFO("Removed %d NaN points from sparse cloud", tmp_indices.size());
  }

  ROS_INFO("Loading dense point cloud");
  pcl::PointCloud<PointType>::Ptr dense_cloud =
      boost::make_shared<pcl::PointCloud<PointType>>();
  std::string dense_file_name;
  if (ros::param::get("~dense_file_name", dense_file_name) == false ||
      pcl::io::loadPCDFile<PointType>(dense_file_name, *dense_cloud) == -1) {
    ROS_INFO("Could not load dense cloud, closing...");
    exit(1);
  } else {
    ROS_INFO("Using file %s as dense point cloud", dense_file_name.c_str());
    std::vector<int> tmp_indices;
    pcl::removeNaNFromPointCloud(*dense_cloud, *dense_cloud, tmp_indices);
    ROS_INFO("Removed %d NaN points from dense cloud", tmp_indices.size());
  }

  pcl::KdTreeFLANN<PointType> kdtree;
  kdtree.setInputCloud(dense_cloud);
  std::vector<std::vector<int>> correspondences(
      sparse_cloud->size());
  std::vector<float> distances(dim_neighborhood);
  for (std::size_t i = 0; i < sparse_cloud->size(); i++) {
    std::vector<int> results;
    kdtree.radiusSearch(*sparse_cloud, i, radius, results, distances,
                        dim_neighborhood);
    ROS_DEBUG("Found %d correspondences", results.size());
    correspondences[i] = results;
  }

  point_cloud_registration::PointCloudRegistration registration(
      *sparse_cloud, *dense_cloud, correspondences, dof);

  ceres::Solver::Options options;
  options.linear_solver_type = ceres::DENSE_QR;
  options.minimizer_progress_to_stdout = true;
  options.max_num_iterations = std::numeric_limits<int>::max();
  ceres::Solver::Summary summary;
  registration.Solve(&options, &summary);
  ROS_INFO_STREAM(summary.FullReport());

  std::unique_ptr<Eigen::Quaternion<double>> estimated_rot =
      registration.rotation();
  std::unique_ptr<Eigen::Vector3d> estimated_translation =
      registration.translation();
  Eigen::Affine3d estimated_transform = Eigen::Affine3d::Identity();
  estimated_transform.rotate(*estimated_rot);
  estimated_transform.pretranslate(Eigen::Vector3d(*estimated_translation));
  pcl::PointCloud<PointType>::Ptr aligned_sparse(
      new pcl::PointCloud<PointType>());
  pcl::transformPointCloud(*sparse_cloud, *aligned_sparse, estimated_transform);

  ROS_INFO("Estimated trans: [%f, %f, %f]", (*estimated_translation)[0],
           (*estimated_translation)[1], (*estimated_translation)[2]);
  ROS_INFO("Estimated rot: [%f, %f, %f, %f]", estimated_rot->x(),
           estimated_rot->y(), estimated_rot->z(), estimated_rot->w());

  std::string aligned_file_name = "aligned_" + sparse_file_name;
  pcl::io::savePCDFile(aligned_file_name, *aligned_sparse, true);

  pcl::visualization::PCLVisualizer viewer;
  viewer.initCameraParameters();
  int v0(0);
  viewer.createViewPort(0, 0.0, 1.0, 1.0, v0);
  viewer.setBackgroundColor(0, 0, 0, v0);
  pcl::visualization::PointCloudColorHandlerCustom<PointType>
      aligned_sparse_red(aligned_sparse, 255, 0, 0);
  pcl::visualization::PointCloudColorHandlerCustom<PointType> dense_blue(
      dense_cloud, 0, 0, 255);
  viewer.addPointCloud<PointType>(aligned_sparse, aligned_sparse_red,
                                  "aligned_sparse", v0);
  viewer.addPointCloud<PointType>(dense_cloud, dense_blue, "dense", v0);

  while (!viewer.wasStopped()) {
    viewer.spinOnce(100);
    boost::this_thread::sleep(boost::posix_time::microseconds(100000));
  }
  return 0;
}
