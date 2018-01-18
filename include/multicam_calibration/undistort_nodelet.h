/* -*-c++-*--------------------------------------------------------------------
 * 2017 Bernd Pfrommer bernd.pfrommer@gmail.com
 */

#ifndef MULTICAM_CALIBRATION_UNDISTORT_NODELET_H
#define MULTICAM_CALIBRATION_UNDISTORT_NODELET_H

#include <image_transport/image_transport.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <nodelet/nodelet.h>
#include <ros/ros.h>
#include <opencv2/core/core.hpp>


namespace multicam_calibration {
  using sensor_msgs::ImageConstPtr;
  using sensor_msgs::CameraInfoConstPtr;
  class UndistortNodelet : public nodelet::Nodelet {
  public:
    void onInit() override;
  private:
    void imageCallback(const ImageConstPtr &img);
    void cameraInfoCallback(const CameraInfoConstPtr &camInfo);
    // ---------- variables

    ros::Subscriber             cameraInfoSub_;
    ros::Publisher              cameraInfoPub_;
    sensor_msgs::CameraInfo     cameraInfo_;
    image_transport::Subscriber imageSub_;
    image_transport::Publisher  imagePub_;

    cv::Mat mapx_;
    cv::Mat mapy_;
  };
}
#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(multicam_calibration::UndistortNodelet, nodelet::Nodelet)

#endif
