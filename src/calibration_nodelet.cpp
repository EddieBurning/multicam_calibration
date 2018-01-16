/* -*-c++-*--------------------------------------------------------------------
 * 2017 Bernd Pfrommer bernd.pfrommer@gmail.com
 *      Kartik Mohta
 */
#include "multicam_calibration/calibration_nodelet.h"
#include "multicam_calibration/camera_extrinsics.h"
#include "multicam_calibration/get_init_pose.h"
#include <memory>
#include <ctime>
#include <chrono>
#include <std_msgs/UInt32.h>
#include <sstream>
#include <boost/range/irange.hpp>

namespace multicam_calibration {
  using boost::irange;

  static CameraExtrinsics get_kalibr_style_transform(const ros::NodeHandle &nh,
                                                     const std::string &field) {
    CameraExtrinsics T;
    XmlRpc::XmlRpcValue lines;
    if (!nh.getParam(field, lines)) {
      throw (std::runtime_error("cannot find transform " + field));
    }
    if (lines.size() != 4 || lines.getType() != XmlRpc::XmlRpcValue::TypeArray) {
      throw (std::runtime_error("invalid transform " + field));
    }
    for (int i = 0; i < lines.size(); i++) {
      if (lines.size() != 4 || lines.getType() != XmlRpc::XmlRpcValue::TypeArray) {
        throw (std::runtime_error("bad line for transform " + field));
      }
      for (int j = 0; j < lines[i].size(); j++) {
        if (lines[i][j].getType() != XmlRpc::XmlRpcValue::TypeDouble) {
          throw (std::runtime_error("bad value for transform " + field));
        } else {
          T(i, j) = static_cast<double>(lines[i][j]);
        }
      }
    }
    return (T);
  }

  void CalibrationNodelet::onInit() {
    cameras_ready_ = false;
    calibrator_.reset(new Calibrator());
    ros::NodeHandle nh = getPrivateNodeHandle();
    try {
      parseCameras();
    } catch (const std::runtime_error &e) {
      ROS_ERROR_STREAM("error parsing cameras: " << e.what());
      ros::shutdown();
    }
    nh.getParam("use_approximate_sync", use_approximate_sync_);
    ROS_INFO_STREAM((use_approximate_sync_ ? "" : "not ") <<  "using approximate sync");
    detector_.reset(new MultiCamApriltagDetector(nh));
    nh.param<int>("skip_num_frames", skipFrames_, 1);
    bool fixIntrinsics;
    nh.param<bool>("fix_intrinsics", fixIntrinsics, false);
    calibrator_->setFixIntrinsics(fixIntrinsics);
    
    image_transport::ImageTransport it(nh);
    for (const auto &camdata : cameras_) {
      if (cameras_.size() > 1) {
        std::shared_ptr<image_transport::SubscriberFilter> p(new image_transport::SubscriberFilter());
        sub_.push_back(p);
        sub_.back()->subscribe(it, camdata.rostopic, 1);
      } else {
        singleCamSub_ = nh.subscribe(camdata.rostopic, 1,
                                     &CalibrationNodelet::callback1, this);
      }
      imagePub_.push_back(nh.advertise<ImageMsg>(camdata.name + "/detected_image", 1));
      tagCountPub_.push_back(nh.advertise<std_msgs::UInt32>(camdata.name + "/num_detected_tags", 1));
    }
    calibrationService_ = nh.advertiseService("calibration", &CalibrationNodelet::calibrate, this);
    subscribe();
    std::string filename;
    if (nh.getParam("corners_file", filename)) {
      if (!readPointsFromFile(filename)) {
        ROS_ERROR_STREAM("file not found or bad: " << filename);
      } else {
        if (!worldPoints_.empty()) {
          ROS_INFO_STREAM("read " << worldPoints_[0].size() << " frames from " << filename);
          CalibrationCmd::Request rq;
          CalibrationCmd::Response rsp;
          calibrate(rq, rsp);
          ros::shutdown();
        }
      }
    }
  }

  static void bombout(const std::string &param, const std::string &cam) {
    throw (std::runtime_error("cannot find " + param + " for cam " + cam));
  }

  static CameraExtrinsics get_transform(const ros::NodeHandle &nh, const std::string &field,
                                        const CameraExtrinsics &def) {
    CameraExtrinsics T(def);
    try {
      T = get_kalibr_style_transform(nh, field);
    } catch (std::runtime_error &e) {
    }
    return (T);
  }

  void CalibrationNodelet::parseCameras() {
    ros::NodeHandle nh = getPrivateNodeHandle();

    int cam_index = 0; 
    while (true) {
      std::string cam = "cam" + std::to_string(cam_index);
      XmlRpc::XmlRpcValue lines;
      if (!nh.getParam(cam, lines)) break; // no more cameras!
      ROS_INFO_STREAM("Parsing " << cam);
      CalibrationData calibData;
      calibData.name = cam;
      CameraIntrinsics &ci = calibData.intrinsics;
      if (!nh.getParam(cam + "/camera_model",
                       ci.camera_model)) { bombout("camera_model", cam); }
      if (!nh.getParam(cam + "/distortion_model",
                       ci.distortion_model)) { bombout("distortion_model", cam); }
      if (!nh.getParam(cam + "/distortion_coeffs",
                       ci.distortion_coeffs)) { bombout("distortion_coeffs", cam); }
      if (!nh.getParam(cam + "/intrinsics",  ci.intrinsics)) { bombout("intrinsics", cam); }
      if (!nh.getParam(cam + "/resolution",  ci.resolution)) { bombout("resolution", cam); }
      if (!nh.getParam(cam + "/rostopic",  calibData.rostopic)) { bombout("rostopic", cam); }
      calibData.T_cam_imu = get_transform(nh, cam + "/T_cam_imu", zeros());
      calibData.T_cn_cnm1 = get_transform(nh, cam + "/T_cn_cnm1", identity());
      cameras_.push_back(calibData);
      worldPoints_.push_back(CamWorldPoints());
      imagePoints_.push_back(CamImagePoints());
      calibrator_->addCamera(cam, calibData);
      ROS_INFO_STREAM("added camera: " << cam);
      cam_index++;
    }
    ROS_INFO_STREAM("Found " << cameras_.size() << " cameras! Moving on...");
    cameras_ready_ = true;
    T_imu_body_ = get_transform(nh, "T_imu_body", zeros());
  }

  static std::string make_filename(const std::string &base) {
    using std::chrono::system_clock;
    std::time_t tt = system_clock::to_time_t(system_clock::now());
    struct std::tm * ptm = std::localtime(&tt);
    std::stringstream ss;
    ss << std::put_time(ptm, "%F-%H-%M-%S");
    return (base + "-" + ss.str() + ".yaml");
  }
  
  bool CalibrationNodelet::calibrate(CalibrationCmd::Request& req,
                                     CalibrationCmd::Response &res) {
    calibrator_->runCalibration();
    CalibDataVec results = calibrator_->getCalibrationResults();
    if (results.empty()) {
      ROS_ERROR("empty calibration results, no file written!");
      return (false);
    }
    std::string baseName, linkName, calibDir;
    ros::NodeHandle nh = getPrivateNodeHandle();
    nh.param<std::string>("output_filename", baseName, "calibration_output");
    nh.param<std::string>("latest_link_name", linkName, "latest.yaml");
    nh.param<std::string>("calib_dir", calibDir, "calib");

    std::string fname = make_filename(baseName);
    std::string fullname = calibDir + "/" + fname;
    
    ROS_INFO_STREAM("writing calibration to " << fullname);
    std::ofstream of(fullname);
    writeCalibration(of, results);
    writeCalibration(std::cout, results);
    // test with poses from optimizer
    testCalibration();
    // test with poses computed from homography
    homographyTest(results);
    std::string cmd = "ln -sf " + fname + " " + calibDir + "/" + linkName;
    if (std::system(NULL) && std::system(cmd.c_str())) {
      ROS_ERROR_STREAM("link command failed: " << cmd);
    }
    return (true);
  }

  typedef std::pair<double, unsigned int> Stat;
  static double avg(const Stat &s) {
    return (s.second > 0 ? s.first/s.second : 0.0);
  }
  void CalibrationNodelet::testCalibration() {
    Calibrator::Residuals res;
    calibrator_->testCalibration(&res);
    if (res.size() == 0) {
      ROS_ERROR("no residuals found for test!");
      return;
    }
    Stat totErr;
    Stat maxErr;
    unsigned int  maxErrCam(0);
    std::vector<Stat>   cameraStats(res[0].size(), Stat(0.0, 0));
    std::vector<std::ofstream> resFiles;
    for (const auto cam_idx : irange(0ul, res[0].size())) {
      resFiles.push_back(std::ofstream("residuals_cam" + std::to_string(cam_idx) + ".txt"));
    }
    for (const auto fnum : irange(0ul, res.size())) {
      for (const auto cam_idx : irange(0ul, res[fnum].size())) {
        for (const auto res_idx : irange(0ul, res[fnum][cam_idx].size())) {
          Calibrator::Vec2d v = res[fnum][cam_idx][res_idx];
          double err = v(0)*v(0) + v(1)*v(1);
          totErr.first += err;
          totErr.second++;
          cameraStats[cam_idx].first += err;
          cameraStats[cam_idx].second++;
          resFiles[cam_idx] << v(0) << " " << v(1) << std::endl;
          if (err > maxErr.first) {
            maxErr.first  = err;
            maxErr.second = fnum;
            maxErrCam     = cam_idx;
          }
        }
      }
    }
    ROS_INFO_STREAM("----------------- reprojection errors: ---------------");
    ROS_INFO_STREAM(  "total error:     " << std::sqrt(avg(totErr)) << " px");
    for (const auto cam_idx: irange(0ul, cameraStats.size())) {
      ROS_INFO_STREAM("avg error cam " << cam_idx << ": " <<
                      std::sqrt(avg(cameraStats[cam_idx])) << " px");
    }
    ROS_INFO_STREAM("max error: " <<  std::sqrt(maxErr.first)
                    << " px at frame: " << maxErr.second << " for cam: " << maxErrCam);
    
  }

  void CalibrationNodelet::homographyTest(const CalibDataVec &results) const {
    if (worldPoints_.empty()) {
      ROS_WARN("no world points to test calib!");
      return;
    }
    
    if (results.size() != imagePoints_.size() || results.size() != worldPoints_.size()) {
      ROS_WARN("number of cameras does not match number of points!");
      return;
    }
    struct CamErr {
      double errSum {0};
      int    cnt {0};
    };
    ROS_INFO_STREAM("-------------- simple homography test ---------");
    // want at least 4 tags visible, or else the homography is not very stable
    const unsigned int MIN_NUM_POINTS(4*4);
    std::ofstream df("debug.txt");
    std::vector<CamErr> cam_error(worldPoints_.size());
    for (unsigned int frame = 0; frame < worldPoints_[0].size(); frame++) {
      for (unsigned int cam_idx = 0; cam_idx < worldPoints_.size(); cam_idx++) {
        const auto &wpts = worldPoints_[cam_idx][frame];
        const auto &ipts = imagePoints_[cam_idx][frame];
        const CameraIntrinsics &ci = results[cam_idx].intrinsics;
        if (wpts.size() >= MIN_NUM_POINTS) { // found first camera that has observed points!
          // get transform T_cnm1_world from homography.
          CameraExtrinsics T_c1_world =
            get_init_pose::get_init_pose(wpts, ipts, ci.intrinsics, ci.distortion_model, ci.distortion_coeffs);
          CameraExtrinsics T_c2_c1 = identity();
          for (unsigned int cam2_idx = cam_idx; cam2_idx < results.size(); cam2_idx++) {
            CamErr &ce = cam_error[cam2_idx];
            const CalibrationData &cam2 = results[cam2_idx];
            T_c2_c1 = (cam2_idx == cam_idx) ? identity() : cam2.T_cn_cnm1 * T_c2_c1; // forward the chain
            CameraExtrinsics T_c2_world = T_c2_c1 * T_c1_world;
            const auto &wpts2 = worldPoints_[cam2_idx][frame];
            const auto &ipts2 = imagePoints_[cam2_idx][frame];
            if (wpts2.size() >= MIN_NUM_POINTS) {
              // take the world points observed by the second camera,
              // and transform them according to T_cam_world into cam2 frame.
              // Then project them into cam2 and see how they line
              // up with image points.
              FrameImagePoints ipts2proj;
              const CameraIntrinsics &ci2 = cam2.intrinsics;
              get_init_pose::project_points(wpts2, T_c2_world, ci2.intrinsics,
                                            ci2.distortion_model, ci2.distortion_coeffs,
                                            &ipts2proj);
              double e(0);
              for (unsigned int i = 0; i < ipts2.size(); i++) {
                double dx = (ipts2[i].x - ipts2proj[i].x);
                double dy = (ipts2[i].y - ipts2proj[i].y);
                e += dx*dx + dy*dy;
              }
              double epp = sqrt(e/(double)ipts2.size()); // per pixel error
              ce.errSum += e;
              ce.cnt    += ipts2.size();
              if (epp > 10.0) {
                ROS_WARN_STREAM("frame " << frame << " tested between cams " << cam_idx
                                << " and " << cam2_idx << " has high pixel err: " << epp);
                
                df << frame << " " << cam2_idx << " " << wpts2.size();
                for (const auto &wp2 : wpts2) {
                  df << " " << wp2.x << " " << wp2.y << " " << wp2.z;
                }
                for (const auto &ip2 : ipts2) {
                  df << " " << ip2.x << " " << ip2.y;
                }
                for (const auto &ip2 : ipts2proj) {
                  df << " " << ip2.x << " " << ip2.y;
                }
                df << std::endl;
              }
            }
          }
          break;  // we are done
        }
      }
    }
    for (unsigned int cam_idx = 0; cam_idx < cam_error.size(); cam_idx++) {
      const CamErr &ce = cam_error[cam_idx];
      double perPointErr(0);
      if (ce.cnt >= 0)  {
        perPointErr = std::sqrt(ce.errSum / (double)ce.cnt);
      }
      ROS_INFO_STREAM("camera: " << cam_idx << " points: " << ce.cnt << " reproj err: " << perPointErr);
    }
  }

  static std::string vec2str(const std::vector<double> v) {
    std::stringstream ss;
    ss << "[";
    if (v.size() > 0) {
      ss << v[0];
    }
    for (unsigned int i = 1; i < v.size(); i++) {
      ss << ", " << v[i];
    }
    ss << "]";
    return (ss.str());
  }

  static void print_tf(std::ostream &os, const std::string &p, const CameraExtrinsics &T) {
    os << std::fixed << std::setprecision(11);
    const int w = 14;
    for (unsigned int i = 0; i < 4; i++) {
      os << p << "- [" << std::setw(w) << T(i,0) << ", " << std::setw(w) << T(i,1) << ", " << std::setw(w) << T(i,2) << ", " << std::setw(w) << T(i,3) << "]" << std::endl;
    }
  }
  
  void CalibrationNodelet::writeCalibration(std::ostream &os, const CalibDataVec &results) {
    for (unsigned int cam_idx = 0; cam_idx < results.size(); cam_idx++) {
      const CalibrationData &cd = results[cam_idx];
      os << cd.name << ":" << std::endl;
      if (isNonZero(cd.T_cam_imu)) {
        os << "  T_cam_imu:" << std::endl;
        print_tf(os, "  ", cd.T_cam_imu);
      }
      if (cam_idx > 0) {
        os << "  T_cn_cnm1:" << std::endl;
        print_tf(os, "  ", cd.T_cn_cnm1);
      }
      const CameraIntrinsics &ci = cd.intrinsics;
      os << "  camera_model: " << ci.camera_model << std::endl;
      os << "  intrinsics: " << vec2str(ci.intrinsics) << std::endl;
      os << "  distortion_model: " << ci.distortion_model << std::endl;
      os << "  distortion_coeffs: " << vec2str(ci.distortion_coeffs) << std::endl;
      os << "  resolution: [" << ci.resolution[0] << ", " << ci.resolution[1] << "]" << std::endl;
      os << "  rostopic: " << cd.rostopic << std::endl;
    }
    if (isNonZero(T_imu_body_)) {
      os << "T_imu_body:" << std::endl;
      print_tf(os, "  ", T_imu_body_);
    }
  }

  void CalibrationNodelet::subscribe() {
    switch (sub_.size()) {
    case 0:
      break;
    case 2:
      if (use_approximate_sync_) {
        approx_sync2_.reset(new ApproxTimeSynchronizer2(
                              ApproxSyncPolicy2(60/*q size*/), *(sub_[0]), *(sub_[1])));
        approx_sync2_->registerCallback(&CalibrationNodelet::callback2, this);
      } else {
        sync2_.reset(new ExactSynchronizer2(*(sub_[0]), *(sub_[1]), 2));
        sync2_->registerCallback(&CalibrationNodelet::callback2, this);
      }
      break;
    case 3:
      if (use_approximate_sync_) {
        approx_sync3_.reset(new ApproxTimeSynchronizer3(
                              ApproxSyncPolicy3(60/*q size*/), *(sub_[0]), *(sub_[1]), *(sub_[2])));
        approx_sync3_->registerCallback(&CalibrationNodelet::callback3, this);
      } else {
        sync3_.reset(new ExactSynchronizer3(*(sub_[0]), *(sub_[1]), *(sub_[2]), 2));
        sync3_->registerCallback(&CalibrationNodelet::callback3, this);
      }
      break;
    case 4:
      if (use_approximate_sync_) {
        approx_sync4_.reset(new ApproxTimeSynchronizer4(
                              ApproxSyncPolicy4(60/*q size*/),
                              *(sub_[0]), *(sub_[1]), *(sub_[2]), *(sub_[3])));
        approx_sync4_->registerCallback(&CalibrationNodelet::callback4, this);
      } else {
        ROS_ERROR("No exact sync beyond 3 cameras, right now");
      }
      break;
    default:
      ROS_ERROR("invalid number of subscribers!");
    }
  }

  bool CalibrationNodelet::guessCameraPose(const CamWorldPoints &wp, const CamImagePoints &ip, CameraExtrinsics *T_0_w) const {
    CameraExtrinsics T_n_0 = identity();
    bool poseFound(false);
    for (unsigned int cam_idx = 0; cam_idx < cameras_.size(); cam_idx++) {
      const CalibrationData &cd = cameras_[cam_idx];
      T_n_0 = cd.T_cn_cnm1 * T_n_0; // chain forward from cam0 to current camera
      if (!wp[cam_idx].empty()) {
        const CameraIntrinsics &ci = cd.intrinsics;
        CameraExtrinsics T_n_w = get_init_pose::get_init_pose(wp[cam_idx], ip[cam_idx], ci.intrinsics,
                                                              ci.distortion_model, ci.distortion_coeffs);
        //std::cout << cam_idx << " init pose: " << std::endl << T_n_w << std::endl;
        if (!poseFound) {
          // first camera with valid pose found
          *T_0_w = T_n_0.inverse() * T_n_w;
          poseFound = true;
        } else {
          // XXX issue warning if error is too big!
          CameraExtrinsics T_0_w_test = T_n_0.inverse() * T_n_w;
          CameraExtrinsics T_err = T_0_w_test.inverse() * (*T_0_w);
          double rot_err = 1.5 - 0.5 *(T_err(0,0) + T_err(1,1) + T_err(2,2));
          if (rot_err > 0.25) {
            ROS_WARN_STREAM("init tf for camera " << cam_idx << " is off from your initial .yaml file!");
            ROS_WARN_STREAM("expected T_n_0 from " << cam_idx << " to cam 0 is roughly:");
            ROS_WARN_STREAM(T_n_w * T_0_w->inverse());
          }
          //std::cout << "T_err: " << std::endl << T_err << std::endl;
          //double trans_err = T_err.block<3,1>(0,3).norm();//std::abs(T_err(3,0)) +std::abs(T_err(3,1)) +std::abs(T_err(3,2))
          //std::cout << "rot error: " << rot_err << " trans error: " << trans_err <<std::endl;
        }
      }
    }
    return (poseFound);
  }
  
  static bool get_intrinsics_csv_data(const std::string &filename,
                                      const unsigned int camera_id,
                                      std::vector<int> *frame_nums,
                                      CamWorldPoints &world_points,
                                      CamImagePoints &img_pts,
                                      int skip_factor = 0)
  {
    std::ifstream in(filename);
    if(!in.is_open())
      return false;

    std::string line;

    int frame_num;
    int prev_frame_num = -1;
    unsigned int frame_count = 0;
    frame_nums->clear();
    while(getline(in, line))
      {
        float world_x, world_y, cam_x, cam_y;
        unsigned int cam_idx;
        if(sscanf(line.c_str(), "%d, intrinsics, %u, %f, %f, %f, %f", &frame_num,
                  &cam_idx, &world_x, &world_y, &cam_x, &cam_y) != 6)
          continue;
        if(cam_idx != camera_id)
          continue;
        if(frame_num % (skip_factor + 1) != 0)
          continue;

        bool new_frame = false;
        if(frame_num != prev_frame_num)
          {
            prev_frame_num = frame_num;
            new_frame = true;
            ++frame_count;
          }

        if(new_frame) {
          world_points.push_back(FrameWorldPoints());
          frame_nums->push_back(frame_num);
        }

        world_points[frame_count - 1].emplace_back(world_x, world_y, 0);

        if(new_frame)
          img_pts.push_back(FrameImagePoints());

        img_pts[frame_count - 1].emplace_back(cam_x, cam_y);
      }
    in.close();
    printf("read %u frames from corners file for cam %u\n", frame_count, camera_id);
    return true;
  }

  bool CalibrationNodelet::readPointsFromFile(const std::string &fname) {

    std::set<int> fnum_set;
    for (unsigned int camid = 0; camid < cameras_.size(); camid++) {
      std::vector<int> fnums;
      CamWorldPoints wp;
      CamImagePoints ip;
      if (!get_intrinsics_csv_data(fname, camid, &fnums, wp, ip)) {
        return (false);
      }
      std::copy(fnums.begin(), fnums.end(), std::inserter(fnum_set, fnum_set.end()));
      ROS_INFO_STREAM("camid " << camid << " read frames: " << fnums.size());
    }
    ROS_INFO_STREAM("total number of frames: " << fnum_set.size());
    for (unsigned int camid = 0; camid < cameras_.size(); camid++) {
      std::vector<int> fnums;
      CamWorldPoints wp;
      CamImagePoints ip;
      if (!get_intrinsics_csv_data(fname, camid, &fnums, wp, ip)) {
        return (false);
      }
      worldPoints_[camid].resize(fnum_set.size());
      imagePoints_[camid].resize(fnum_set.size());
      for (unsigned int i = 0; i < fnums.size(); i++) {
        int fnum = fnums[i];
        int frank = std::distance(fnum_set.begin(), fnum_set.find(fnum));
        worldPoints_[camid][frank] = wp[i];
        imagePoints_[camid][frank] = ip[i];
      }
    }

    
    for (unsigned int fnum = 0; fnum < worldPoints_[0].size(); fnum++) {
      CamWorldPoints wp;
      CamImagePoints ip;
      for (unsigned int camid = 0; camid < cameras_.size(); camid++) {
        wp.push_back(worldPoints_[camid][fnum]);
        ip.push_back(imagePoints_[camid][fnum]);
      }
      CameraExtrinsics cam0Pose;
      if (!guessCameraPose(wp, ip, &cam0Pose)) {
        ROS_WARN_STREAM("no detections found, skipping frame " << fnum);
        continue;
      }
      calibrator_->addPoints(frameNum_, wp, ip, cam0Pose);
    }
    return (true);
  }

  void CalibrationNodelet::process(const std::vector<ImageConstPtr> &msg_vec) {
    if ((skipCount_++ % skipFrames_) != 0) {
      return;
    }
    CamWorldPoints wp;
    CamImagePoints ip;
    std::vector<apriltag_ros::ApriltagVec> detected_tags =
      detector_->process(msg_vec, &wp, &ip);
    ROS_ASSERT(msg_vec.size() == detected_tags.size());
    // Insert newly detected points into existing set.
    // At this point one could add the data points to an
    // incremental solver.
    CameraExtrinsics cam0Pose;
    if (!guessCameraPose(wp, ip, &cam0Pose)) {
      ROS_WARN("no detections found, skipping frame!");
      return;
    }
    calibrator_->addPoints(frameNum_, wp, ip, cam0Pose);

    // add new frames to each camera
    for (unsigned int cam_idx = 0; cam_idx < wp.size(); cam_idx++) {
      if (cam_idx < wp.size()) {
        worldPoints_[cam_idx].push_back(wp[cam_idx]);
      }
      if (cam_idx < ip.size()) {
        imagePoints_[cam_idx].push_back(ip[cam_idx]);
      }
    }
    CalibDataVec::iterator cam = cameras_.begin();
    for (const auto &tags : detected_tags) {
      cam->tagCount += tags.size();
      ++cam;
      if (cam == cameras_.end()) {
        break;
      }
    }
    publishDebugImages(msg_vec, detected_tags);
    publishTagCounts();
    frameNum_++;
#if 0    
    if (frameNum_ > 1) {
      CalibrationCmd::Request req;
      req.calibration = 0;
      CalibrationCmd::Response res;
      calibrate(req, res);
    }
#endif    
  }

  void CalibrationNodelet::publishDebugImages(const std::vector<ImageMsg::ConstPtr> &msg_vec,
                                              const std::vector<apriltag_ros::ApriltagVec> &detected_tags) {
    for (int i = 0; i < (int)detected_tags.size(); i++) {
      if (imagePub_[i].getNumSubscribers() > 0) {
        cv_bridge::CvImageConstPtr const cv_ptr = cv_bridge::toCvShare(
          msg_vec[i], sensor_msgs::image_encodings::MONO8);
        const cv::Mat gray = cv_ptr->image;
        if (gray.rows == 0) {
          ROS_ERROR("cannot decode image, not MONO8!");
          continue;
        }
        cv::Mat img;
        cv::cvtColor(gray, img, CV_GRAY2BGR);
        apriltag_ros::DrawApriltags(img, detected_tags[i]);
        cv_bridge::CvImage cv_img(msg_vec[i]->header, sensor_msgs::image_encodings::BGR8, img);
        imagePub_[i].publish(cv_img.toImageMsg());
      }
    }
  }
  
  void CalibrationNodelet::publishTagCounts() {
    for (unsigned int i = 0; i < cameras_.size(); i++) {
      const CalibrationData &cd = cameras_[i];
      if (tagCountPub_[i].getNumSubscribers() > 0) {
        std_msgs::UInt32 msg;
        msg.data = cd.tagCount;
        tagCountPub_[i].publish(msg);
      }
    }
  }

  
  void CalibrationNodelet::callback1(ImageConstPtr const &img0) {
    std::vector<ImageMsg::ConstPtr> msg_vec = {img0};
    process(msg_vec);
  }

  void CalibrationNodelet::callback2(ImageConstPtr const &img0, ImageConstPtr const &img1) {
    std::vector<ImageMsg::ConstPtr> msg_vec = {img0, img1};
    process(msg_vec);
  }

  void CalibrationNodelet::callback3(ImageConstPtr const &img0, ImageConstPtr const &img1,
                                     ImageConstPtr const &img2) {
    std::vector<ImageMsg::ConstPtr> msg_vec = {img0, img1, img2};
    process(msg_vec);
  }
  void CalibrationNodelet::callback4(ImageConstPtr const &img0, ImageConstPtr const &img1,
                                     ImageConstPtr const &img2, ImageConstPtr const &img3) {
    std::vector<ImageMsg::ConstPtr> msg_vec = {img0, img1, img2, img3};
    process(msg_vec);
  }

  void CalibrationNodelet::callback5(ImageConstPtr const &img0, ImageConstPtr const &img1,
                                     ImageConstPtr const &img2, ImageConstPtr const &img3,
                                     ImageConstPtr const &img4) {
    std::vector<ImageMsg::ConstPtr> msg_vec = {img0, img1, img2, img3, img4};
    process(msg_vec);
  }

  void CalibrationNodelet::callback6(ImageConstPtr const &img0, ImageConstPtr const &img1,
                                     ImageConstPtr const &img2, ImageConstPtr const &img3,
                                     ImageConstPtr const &img4, ImageConstPtr const &img5) {
    std::vector<ImageMsg::ConstPtr> msg_vec = {img0, img1, img2, img3, img4, img5};
    process(msg_vec);
  }

}
