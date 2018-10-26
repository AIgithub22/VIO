#include "keyframe.h"

KeyFrame::KeyFrame(double _header, Eigen::Vector3d _vio_T_w_i, Eigen::Matrix3d _vio_R_w_i, 
                   Eigen::Vector3d _cur_T_w_i, Eigen::Matrix3d _cur_R_w_i, 
                   cv::Mat &_image, const char *_brief_pattern_file)
:header{_header}, image{_image}, BRIEF_PATTERN_FILE(_brief_pattern_file)
{
    T_w_i = _cur_T_w_i;
    R_w_i = _cur_R_w_i;
    COL = image.cols;
    ROW = image.rows;
    use_retrive = false;
    is_looped = 0;
    has_loop = 0;
    update_loop_info = 0;
    vio_T_w_i = _vio_T_w_i;
    vio_R_w_i = _vio_R_w_i;
}

/*****************************************utility function************************************************/
bool inBorder(const cv::Point2f &pt, int COL, int ROW)
{
    const int BORDER_SIZE = 1;
    int img_x = cvRound(pt.x);
    int img_y = cvRound(pt.y);
    return BORDER_SIZE <= img_x && img_x < COL - BORDER_SIZE && BORDER_SIZE <= img_y && img_y < ROW - BORDER_SIZE;
}

/**
 * 根绝status的状态重新构造向量v
 */
template <typename Derived>
static void reduceVector(vector<Derived> &v, vector<uchar> status)
{
    int j = 0;
    for (int i = 0; i < int(v.size()); i++)
        if (status[i])
            v[j++] = v[i];
    v.resize(j);
}

/**
 * [KeyFrame::extractBrief 提取图像的brief描述子]
 * @param image [description]
 */
void KeyFrame::extractBrief(cv::Mat &image)
{
    BriefExtractor extractor(BRIEF_PATTERN_FILE);
    extractor(image, measurements, keypoints, descriptors);

    //！将tracking过程中的强角点和其描述子加入到window中
    //！question(window怎么称呼)
    int start = keypoints.size() - measurements.size();
    for(int i = 0; i< (int)measurements.size(); i++)
    {
        //！加入的只有强角点的Features和descriptors
        window_keypoints.push_back(keypoints[start + i]);
        window_descriptors.push_back(descriptors[start + i]);
    }
}
void KeyFrame::setExtrinsic(Eigen::Vector3d T, Eigen::Matrix3d R)
{
    qic = R;
    tic = T;
}

/**
 * [KeyFrame::buildKeyFrameFeatures 构建关键帧的Features]
 * @param estimator [description]
 * @param m_camera  [description]
 */
void KeyFrame::buildKeyFrameFeatures(Estimator &estimator, const camodocal::CameraPtr &m_camera)
{
    for (auto &it_per_id : estimator.f_manager.feature)
    {
        it_per_id.used_num = it_per_id.feature_per_frame.size();
        //if (!(it_per_id.used_num >= 2 && it_per_id.start_frame < WINDOW_SIZE - 2))
        //    continue;

        int frame_size = it_per_id.feature_per_frame.size();
        if(it_per_id.start_frame <= WINDOW_SIZE - 2 && it_per_id.start_frame + frame_size - 1 >= WINDOW_SIZE - 2)
        {
            //features current measurements
            Vector3d point = it_per_id.feature_per_frame[WINDOW_SIZE - 2 - it_per_id. start_frame].point;
            Vector2d point_uv;
            //！将3D Features投影到图像平面上
            m_camera->spaceToPlane(point, point_uv);
            measurements.push_back(cv::Point2f(point_uv.x(), point_uv.y()));
            pts_normalize.push_back(cv::Point2f(point.x()/point.z(), point.y()/point.z()));
            features_id.push_back(it_per_id.feature_id);
            //features 3D pos from first measurement and inverse depth
            Vector3d pts_i = it_per_id.feature_per_frame[0].point * it_per_id.estimated_depth;
            //！将3D Features从相机坐标系转到IMU坐标系
            point_clouds.push_back(estimator.Rs[it_per_id.start_frame] * (qic * pts_i + tic) + estimator.Ps[it_per_id.start_frame]);
        }
    }
}

bool KeyFrame::inAera(cv::Point2f pt, cv::Point2f center, float area_size)
{
    if(center.x < 0 || center.x > COL || center.y < 0 || center.y > ROW)
        return false;
    if(pt.x > center.x - area_size && pt.x < center.x + area_size &&
       pt.y > center.y - area_size && pt.y < center.y + area_size)
        return true;
    else
        return false;
}

/**
 * [KeyFrame::searchInAera description]
 * @param  center_cur        [强角点]
 * @param  area_size         [搜索范围]
 * @param  window_descriptor [强角点对应的描述子]
 * @param  descriptors_old   [闭环帧的描述子集合]
 * @param  keypoints_old     [闭环帧的关键点集合(强角点+Fast角点)]
 * @param  best_match        [良好的匹配点(闭环帧中)]
 * @return                   [description]
 */
bool KeyFrame::searchInAera(cv::Point2f center_cur, float area_size,
                            const BRIEF::bitset window_descriptor,
                            const std::vector<BRIEF::bitset> &descriptors_old,
                            const std::vector<cv::KeyPoint> &keypoints_old,
                            cv::Point2f &best_match)
{
    cv::Point2f best_pt;
    int bestDist = 128;
    int bestIndex = -1;
    for(int i = 0; i < (int)descriptors_old.size(); i++)
    {
        //！该强角点的搜索范围之内不存在匹配点
        if(!inAera(keypoints_old[i].pt, center_cur, area_size))
            continue;

        //！判断匹配之后的汉明距离是否满足要求
        int dis = HammingDis(window_descriptor, descriptors_old[i]);
        if(dis < bestDist)
        {
            bestDist = dis;
            bestIndex = i;
        }
    }

    //！良好的匹配点位置是在闭环帧中的
    if (bestIndex != -1)
    {
      best_match = keypoints_old[bestIndex].pt;
      return true;
    }
    else
      return false;
}

/**
 * [KeyFrame::FundmantalMatrixRANSAC description]
 * @param measurements_old      [闭环帧和匹配帧之间的良好匹配点(闭环帧中)]
 * @param measurements_old_norm [良好的匹配点在归一化平面的坐标]
 * @param m_camera              [description]
 */
void KeyFrame::FundmantalMatrixRANSAC(vector<cv::Point2f> &measurements_old,
                                      vector<cv::Point2f> &measurements_old_norm,
                                      const camodocal::CameraPtr &m_camera)
{
    if (measurements_old.size() >= 8)
    {
        measurements_old_norm.clear();

        vector<cv::Point2f> un_measurements(measurements_matched.size()), un_measurements_old(measurements_old.size());

        //！
        for (int i = 0; i < (int)measurements_matched.size(); i++)
        {
            //！将匹配帧中的良好匹配点反投影到归一化平面之后，在做一次指定内参的投影
            double FOCAL_LENGTH = 460.0;
            Eigen::Vector3d tmp_p;
            m_camera->liftProjective(Eigen::Vector2d(measurements_matched[i].x, measurements_matched[i].y), tmp_p);
            tmp_p.x() = FOCAL_LENGTH * tmp_p.x() / tmp_p.z() + COL / 2.0;
            tmp_p.y() = FOCAL_LENGTH * tmp_p.y() / tmp_p.z() + ROW / 2.0;
            un_measurements[i] = cv::Point2f(tmp_p.x(), tmp_p.y());

            //！将闭环帧中的良好匹配点反投影到归一化平面之后，在做一次指定内参的投影
            m_camera->liftProjective(Eigen::Vector2d(measurements_old[i].x, measurements_old[i].y), tmp_p);
            measurements_old_norm.push_back(cv::Point2f(tmp_p.x()/tmp_p.z(), tmp_p.y()/tmp_p.z()));
            tmp_p.x() = FOCAL_LENGTH * tmp_p.x() / tmp_p.z() + COL / 2.0;
            tmp_p.y() = FOCAL_LENGTH * tmp_p.y() / tmp_p.z() + ROW / 2.0;
            un_measurements_old[i] = cv::Point2f(tmp_p.x(), tmp_p.y());
        }
        
        //！根据匹配点之间的单应性关系，再做一次外点的剔除
        vector<uchar> status;
        cv::findFundamentalMat(un_measurements, un_measurements_old, cv::FM_RANSAC, 5.0, 0.99, status);
        reduceVector(measurements_old, status);
        reduceVector(measurements_old_norm, status);
        reduceVector(measurements_matched, status);
        reduceVector(features_id_matched, status);
        reduceVector(point_clouds_matched, status);
        
    }
}

/**
 * [KeyFrame::searchByDes 在闭环帧中寻找良好匹配点]
 * @param measurements_old      [description]
 * @param measurements_old_norm [description]
 * @param descriptors_old       [闭环帧的描述子]
 * @param keypoints_old         [闭环帧的关键点]
 * @param m_camera              [description]
 */
void KeyFrame::searchByDes(std::vector<cv::Point2f> &measurements_old, 
                           std::vector<cv::Point2f> &measurements_old_norm,
                           const std::vector<BRIEF::bitset> &descriptors_old,
                           const std::vector<cv::KeyPoint> &keypoints_old,
                           const camodocal::CameraPtr &m_camera)
{
    //ROS_INFO("loop_match before cur %d %d, old %d", (int)window_descriptors.size(), (int)measurements.size(), (int)descriptors_old.size());
    std::vector<uchar> status;
    for(int i = 0; i < (int)window_descriptors.size(); i++)
    {
        cv::Point2f pt(0.f, 0.f);
        if (searchInAera(measurements[i], 200, window_descriptors[i], descriptors_old, keypoints_old, pt))
          status.push_back(1);
        else
          status.push_back(0);
        measurements_old.push_back(pt);
    }

    //！依据status的重新构造匹配的keypoints和featuresid
    measurements_matched = measurements;
    features_id_matched = features_id;
    point_clouds_matched = point_clouds;
    reduceVector(measurements_old, status);
    reduceVector(measurements_matched, status);
    reduceVector(features_id_matched, status);
    reduceVector(point_clouds_matched, status);
}

/**
 * [KeyFrame::PnPRANSAC description]
 * @param measurements_old      [description]
 * @param measurements_old_norm [description]
 * @param PnP_T_old             [description]
 * @param PnP_R_old             [description]
 */
void KeyFrame::PnPRANSAC(vector<cv::Point2f> &measurements_old,
                         std::vector<cv::Point2f> &measurements_old_norm, 
                         Eigen::Vector3d &PnP_T_old, Eigen::Matrix3d &PnP_R_old)
{
    cv::Mat r, rvec, t, D, tmp_r;
    cv::Mat K = (cv::Mat_<double>(3, 3) << 1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0);
    Matrix3d R_inital;
    Vector3d P_inital;
    //！从IMU坐标系转到相机坐标系
    Matrix3d R_w_c = vio_R_w_i * qic;
    Vector3d T_w_c = vio_T_w_i + vio_R_w_i * tic;

    R_inital = R_w_c.inverse();
    P_inital = -(R_inital * T_w_c);
    
    cv::eigen2cv(R_inital, tmp_r);
    cv::Rodrigues(tmp_r, rvec);
    cv::eigen2cv(P_inital, t);

    vector<cv::Point3f> pts_3_vector;
    for(auto &it: point_clouds_matched)
        pts_3_vector.push_back(cv::Point3f((float)it.x(),(float)it.y(),(float)it.z()));

    //！利用PnP算法，求解闭环帧和匹配帧之间的位姿关系
    cv::Mat inliers;
    TicToc t_pnp_ransac;
    if (CV_MAJOR_VERSION < 3)
        solvePnPRansac(pts_3_vector, measurements_old_norm, K, D, rvec, t, true, 100, 10.0 / 460.0, 100, inliers);
    else
    {
        if (CV_MINOR_VERSION < 2)
            solvePnPRansac(pts_3_vector, measurements_old_norm, K, D, rvec, t, true, 100, sqrt(10.0 / 460.0), 0.99, inliers);
        else
            solvePnPRansac(pts_3_vector, measurements_old_norm, K, D, rvec, t, true, 100, 10.0 / 460.0, 0.99, inliers);

    }
    ROS_DEBUG("t_pnp_ransac %f ms", t_pnp_ransac.toc());

    //！提取PnP求解过程中的内点
    std::vector<uchar> status;
    for (int i = 0; i < (int)measurements_old_norm.size(); i++)
        status.push_back(0);

    for( int i = 0; i < inliers.rows; i++)
    {
        int n = inliers.at<int>(i);
        status[n] = 1;
    } 

    //！将求得的相对位姿再转到IMU坐标系下
    cv::Rodrigues(rvec, r);
    Matrix3d R_pnp, R_w_c_old;
    cv::cv2eigen(r, R_pnp);
    R_w_c_old = R_pnp.transpose();
    Vector3d T_pnp, T_w_c_old;
    cv::cv2eigen(t, T_pnp);
    T_w_c_old = R_w_c_old * (-T_pnp);

    PnP_R_old = R_w_c_old * qic.transpose();
    PnP_T_old = T_w_c_old - PnP_R_old * tic;   

    reduceVector(measurements_old, status);
    reduceVector(measurements_old_norm, status);
    reduceVector(measurements_matched, status);
    reduceVector(features_id_matched, status);
    reduceVector(point_clouds_matched, status);


}

/**
 * [KeyFrame::findConnectionWithOldFrame 计算闭环帧与当前帧的位姿关系和匹配关系]
 * @param  old_kf                [闭环帧]
 * @param  measurements_old      [闭环帧中良好的匹配点]
 * @param  measurements_old_norm [闭环帧中良好的匹配点]
 * @param  PnP_T_old             [description]
 * @param  PnP_R_old             [description]
 * @param  m_camera              [description]
 * @return                       [description]
 */
bool KeyFrame::findConnectionWithOldFrame(const KeyFrame* old_kf,
                                          std::vector<cv::Point2f> &measurements_old, std::vector<cv::Point2f> &measurements_old_norm,
                                          Eigen::Vector3d &PnP_T_old, Eigen::Matrix3d &PnP_R_old,
                                          const camodocal::CameraPtr &m_camera)
{
    TicToc t_match;
    searchByDes(measurements_old, measurements_old_norm, old_kf->descriptors, old_kf->keypoints, m_camera);
    FundmantalMatrixRANSAC(measurements_old, measurements_old_norm, m_camera);
    if ((int)measurements_old_norm.size() > MIN_LOOP_NUM)
    {
        PnPRANSAC(measurements_old, measurements_old_norm, PnP_T_old, PnP_R_old);
    }
    ROS_DEBUG("loop final use num %d %lf---------------", (int)measurements_old.size(), t_match.toc());
    return true;
}

void KeyFrame::updatePose(const Eigen::Vector3d &_T_w_i, const Eigen::Matrix3d &_R_w_i)
{
    unique_lock<mutex> lock(mMutexPose);
    T_w_i = _T_w_i;
    R_w_i = _R_w_i;
}

void KeyFrame::updateOriginPose(const Eigen::Vector3d &_T_w_i, const Eigen::Matrix3d &_R_w_i)
{
    unique_lock<mutex> lock(mMutexPose);
    vio_T_w_i = _T_w_i;
    vio_R_w_i = _R_w_i;
}

void KeyFrame::getPose(Eigen::Vector3d &_T_w_i, Eigen::Matrix3d &_R_w_i)
{
    unique_lock<mutex> lock(mMutexPose);
    _T_w_i = T_w_i;
    _R_w_i = R_w_i;
}

void KeyFrame::getOriginPose(Eigen::Vector3d &_T_w_i, Eigen::Matrix3d &_R_w_i)
{
    unique_lock<mutex> lock(mMutexPose);
    _T_w_i = vio_T_w_i;
    _R_w_i = vio_R_w_i;
}

void KeyFrame::updateLoopConnection(Vector3d relative_t, Quaterniond relative_q, double relative_yaw)
{
    has_loop = 1;
    update_loop_info = 1;
    unique_lock<mutex> lock(mLoopInfo);
    Eigen::Matrix<double, 8, 1> connected_info;
    connected_info <<relative_t.x(), relative_t.y(), relative_t.z(),
                     relative_q.w(), relative_q.x(), relative_q.y(), relative_q.z(),
                     relative_yaw;
    loop_info = connected_info;
}

Eigen::Vector3d KeyFrame::getLoopRelativeT()
{
    assert(update_loop_info == 1);
    unique_lock<mutex> lock(mLoopInfo);
    return Eigen::Vector3d(loop_info(0), loop_info(1), loop_info(2));
}

double KeyFrame::getLoopRelativeYaw()
{
    assert(update_loop_info == 1);
    unique_lock<mutex> lock(mLoopInfo);
    return loop_info(7);
}

void KeyFrame::detectLoop(int index)
{
    has_loop = true;
    loop_index = index;
}

void KeyFrame::removeLoop()
{
    has_loop = false;
    update_loop_info = 0;
}

/**
 * [KeyFrame::HammingDis 求取一对描述子的汉明距离]
 * @param  a [description]
 * @param  b [description]
 * @return   [description]
 */
int KeyFrame::HammingDis(const BRIEF::bitset &a, const BRIEF::bitset &b)
{
    BRIEF::bitset xor_of_bitset = a ^ b;
    int dis = xor_of_bitset.count();
    return dis;
}

BriefExtractor::BriefExtractor(const std::string &pattern_file)
{
  // The DVision::BRIEF extractor computes a random pattern by default when
  // the object is created.
  // We load the pattern that we used to build the vocabulary, to make
  // the descriptors compatible with the predefined vocabulary
  
  // loads the pattern
  cv::FileStorage fs(pattern_file.c_str(), cv::FileStorage::READ);
  if(!fs.isOpened()) throw string("Could not open file ") + pattern_file;
  
  vector<int> x1, y1, x2, y2;
  fs["x1"] >> x1;
  fs["x2"] >> x2;
  fs["y1"] >> y1;
  fs["y2"] >> y2;
  
  m_brief.importPairs(x1, y1, x2, y2);
}

/**
 * [BriefExtractor::operator 提取Fast角点和Brief描述子]
 * @param descriptors [最终得到的描述子]
 * @param window_pts  [tracking中的强角点]
 * @param keys        [所有的KeyPints]
 */
void BriefExtractor::operator() (const cv::Mat &im, const std::vector<cv::Point2f> window_pts,
                                 vector<cv::KeyPoint> &keys, vector<BRIEF::bitset> &descriptors) const
{
  // extract FAST keypoints with opencv
  const int fast_th = 20; // corner detector response threshold
  cv::FAST(im, keys, fast_th, true);

  //! 将tracking中的强角点也加入到keypoints当中
  for(int i = 0; i < (int)window_pts.size(); i++)
  {
      cv::KeyPoint key;
      key.pt = window_pts[i];
      keys.push_back(key);
  }
  // compute their BRIEF descriptor
  m_brief.compute(im, keys, descriptors);
}