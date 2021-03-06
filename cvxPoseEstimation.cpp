//
//  cvxPoseEstimation.cpp
//  LoopClosure
//
//  Created by jimmy on 2016-03-31.
//  Copyright © 2016 jimmy. All rights reserved.
//

#include "cvxPoseEstimation.hpp"
#include <iostream>

using std::cout;
using std::endl;
using cv::Mat;

bool CvxPoseEstimation::estimateCameraPose(const cv::Mat & camera_matrix,
                                           const cv::Mat & dist_coeff,
                                           const vector<cv::Point2d> & im_pts,
                                           const vector<cv::Point3d> & wld_pts,
                                           cv::Mat & camera_pose)
{
    assert(im_pts.size() == wld_pts.size());
    
    Mat rvec;
    Mat tvec;
    bool is_solved = cv::solvePnPRansac(Mat(wld_pts), Mat(im_pts), camera_matrix, Mat(), rvec, tvec, false, 1000, 8.0);
    if (!is_solved) {
        printf("warning: solve PnP failed.\n");
        return false;
    }
    
    {
        // test re-projection error
        vector<cv::Point2d> projected_pts;
        int num = 0;
        cv::projectPoints(Mat(wld_pts), rvec, tvec, camera_matrix, Mat(), projected_pts);
        assert(im_pts.size() == projected_pts.size());
        for (int i = 0; i<projected_pts.size(); i++) {
            double error_reproj = cv::norm(im_pts[i] - projected_pts[i]);
            
            if (error_reproj > 10) {
            //    printf("reprojection error are %lf\n", error_reproj);
           //     cout<<"correct position   "<<im_pts[i]<<endl;
           //     cout<<"projected position "<<projected_pts[i]<<endl<<endl;
                num++;
            }
        }
        printf("bad projection (reprojection error > 10) number is %d, percentage %lf.\n", num, 1.0*num/projected_pts.size());
    }
    
    Mat rot;
    cv::Rodrigues(rvec,rot);
    
    assert(rot.type() == CV_64F);
    assert(tvec.type() == CV_64F);
    
    camera_pose = cv::Mat::eye(4, 4, CV_64F);
    for(int j = 0; j<3; j++) {
        for (int i = 0; i <3; i++) {
            camera_pose.at<double>(i,j) = rot.at<double>(i,j);
        }
    }
    
    camera_pose.at<double>(0, 3) = tvec.at<double>(0, 0);
    camera_pose.at<double>(1, 3) = tvec.at<double>(1, 0);
    camera_pose.at<double>(2, 3) = tvec.at<double>(2, 0);
    
    // camera to world coordinate
    camera_pose = camera_pose.inv();
    // cout<<"camera to world coordinate is "<<camera_pose<<endl;
    
    return true;
   
}

struct HypotheseLoss
{
    double loss_;
    Mat rvec_;        //rotation
    Mat tvec_;        //translation
    vector<unsigned int> inlier_indices_;
    
    HypotheseLoss()
    {
        loss_ = INT_MAX;
    }
    HypotheseLoss(const double loss)
    {
        loss_ = loss;
    }
    
    HypotheseLoss(const HypotheseLoss & other)
    {
        loss_ = other.loss_;
        rvec_ = other.rvec_;
        tvec_ = other.tvec_;
        inlier_indices_.clear();
        inlier_indices_.resize(other.inlier_indices_.size());
        for(int i = 0; i< other.inlier_indices_.size(); i++) {
            inlier_indices_[i] = other.inlier_indices_[i];
        }
    }
   
    bool operator < (const HypotheseLoss & other) const
    {
        return loss_ < other.loss_;
    }
    
    HypotheseLoss & operator = (const HypotheseLoss & other)
    {
        if(&other == this) {
            return *this;
        }
        loss_ = other.loss_;
        rvec_ = other.rvec_;
        tvec_ = other.tvec_;
        inlier_indices_.clear();
        inlier_indices_.resize(other.inlier_indices_.size());
        for(int i=0; i<other.inlier_indices_.size(); i++){
            inlier_indices_[i] = other.inlier_indices_[i];
        }
        
        return *this;
    }
    
};
    

bool CvxPoseEstimation::preemptiveRANSAC(const vector<cv::Point2d> & img_pts,
                                         const vector<cv::Point3d> & wld_pts,
                                         const cv::Mat & camera_matrix,
                                         const cv::Mat & dist_coeff,
                                         const PreemptiveRANSACParameter & param,
                                         cv::Mat & camera_pose)
{
    assert(img_pts.size() == wld_pts.size());
    assert(img_pts.size() > 500);
    
    const int num_iteration = 2048;
    int K = 1024;
    const int N = (int)img_pts.size();
    const int B = 500;
    
    vector<std::pair<Mat, Mat> > rt_candidate;
    for (int i = 0; i<num_iteration; i++) {
        
        int k1 = 0;
        int k2 = 0;
        int k3 = 0;
        int k4 = 0;
        
        do{
            k1 = rand()%N;
            k2 = rand()%N;
            k3 = rand()%N;
            k4 = rand()%N;
        }while (k1 == k2 || k1 == k3 || k1 == k4 ||
                k2 == k3 || k2 == k4 || k3 == k4);
        
        vector<cv::Point2d> sampled_img_pts;
        vector<cv::Point3d> sampled_wld_pts;
        
        sampled_img_pts.push_back(img_pts[k1]);
        sampled_img_pts.push_back(img_pts[k2]);
        sampled_img_pts.push_back(img_pts[k3]);
        
        sampled_wld_pts.push_back(wld_pts[k1]);
        sampled_wld_pts.push_back(wld_pts[k2]);
        sampled_wld_pts.push_back(wld_pts[k3]);
        
        Mat rvec;
        Mat tvec;
        bool is_solved = cv::solvePnP(Mat(sampled_wld_pts), Mat(sampled_img_pts), camera_matrix, dist_coeff, rvec, tvec, false, CV_EPNP);
        if (is_solved) {
            rt_candidate.push_back(std::make_pair(rvec, tvec));
        }
        if (rt_candidate.size() > K) {
            printf("initialization repeat %d times\n", i);
            break;
        }
    }
    printf("init camera parameter number is %lu\n", rt_candidate.size());
    
    K = (int)rt_candidate.size();
    
    
    vector<HypotheseLoss> losses;
    for (int i = 0; i<rt_candidate.size(); i++) {
        HypotheseLoss hyp(0.0);
        hyp.rvec_ = rt_candidate[i].first;
        hyp.tvec_ = rt_candidate[i].second;
        losses.push_back(hyp);
    }
    
    double reproj_threshold = param.reproj_threshold;
    while (losses.size() > 1) {
        // sample random set
        vector<cv::Point2d> sampled_img_pts;
        vector<cv::Point3d> sampled_wld_pts;
        for (int i =0; i<B; i++) {
            int index = rand()%N;
            sampled_img_pts.push_back(img_pts[index]);
            sampled_wld_pts.push_back(wld_pts[index]);
        }
        
        // count outliers
        for (int i = 0; i<losses.size(); i++) {
            // evaluate the accuracy by check reprojection error
            vector<cv::Point2d> projected_pts;
            cv::projectPoints(sampled_wld_pts, losses[i].rvec_, losses[i].tvec_, camera_matrix, dist_coeff, projected_pts);
            for (int j = 0; j<projected_pts.size(); j++) {
                cv::Point2d dif = projected_pts[j] - sampled_img_pts[j];
                double dis = cv::norm(dif);
                if (dis > reproj_threshold) {
                    losses[i].loss_ += 1.0;
                }
                else  {
                    losses[i].inlier_indices_.push_back(j);
                }
            }
        }
        
        std::sort(losses.begin(), losses.end());
        losses.resize(losses.size()/2);
        
        
        for (int j = 0; j<losses.size(); j++) {
            printf("after: loss is %lf\n", losses[j].loss_);
        }
        printf("\n\n");
        
        // refine by inliers
        for (int i = 0; i<losses.size(); i++) {
            // number of inliers is larger than minimum configure
            if (losses[i].inlier_indices_.size() > 4) {
                vector<cv::Point2d> inlier_img_pts;
                vector<cv::Point3d> inlier_wld_pts;
                for (int j = 0; j < losses[i].inlier_indices_.size(); j++) {
                    int index = losses[i].inlier_indices_[j];
                    inlier_img_pts.push_back(sampled_img_pts[index]);
                    inlier_wld_pts.push_back(sampled_wld_pts[index]);
                }
                
                Mat rvec = losses[i].rvec_;
                Mat tvec = losses[i].tvec_;
                bool is_solved = cv::solvePnP(Mat(inlier_wld_pts), Mat(inlier_img_pts), camera_matrix, dist_coeff, rvec, tvec, true, CV_EPNP);  // CV_ITERATIVE   CV_EPNP
                if (is_solved) {
                    losses[i].rvec_ = rvec;
                    losses[i].tvec_ = tvec;
                }
            }
        }        
    }
    
    assert(losses.size() == 1);
    
    // change to camera to world transformation
    Mat rot;
    cv::Rodrigues(losses.front().rvec_, rot);
    Mat tvec = losses.front().tvec_;
    camera_pose = cv::Mat::eye(4, 4, CV_64F);
    for (int j = 0; j<3; j++) {
        for (int i = 0; i<3; i++) {
            camera_pose.at<double>(i, j) = rot.at<double>(i, j);
        }
    }
    camera_pose.at<double>(0, 3) = tvec.at<double>(0, 0);
    camera_pose.at<double>(1, 3) = tvec.at<double>(1, 0);
    camera_pose.at<double>(2, 3) = tvec.at<double>(2, 0);
    
    // camere to world coordinate
    camera_pose = camera_pose.inv();
    
    return true;
}

Mat CvxPoseEstimation::rotationToEularAngle(const cv::Mat & rot)
{
    assert(rot.rows == 3 && rot.cols == 3);
    assert(rot.type() == CV_64FC1);
    
    // https://d3cw3dd2w32x2b.cloudfront.net/wp-content/uploads/2012/07/euler-angles.pdf
    double m00 = rot.at<double>(0, 0);
    double m01 = rot.at<double>(0, 1);
    double m02 = rot.at<double>(0, 2);
    double m10 = rot.at<double>(1, 0);
    double m11 = rot.at<double>(1, 1);
    double m12 = rot.at<double>(1, 2);
    double m20 = rot.at<double>(2, 0);
    double m21 = rot.at<double>(2, 1);
    double m22 = rot.at<double>(2, 2);
    double theta1 = atan2(m12, m22);
    double c2 = sqrt(m00 * m00 + m01 * m01);
    double theta2 = atan2(-m02, c2);
    double s1 = sin(theta1);
    double c1 = cos(theta1);
    double theta3 = atan2(s1*m20 - c1 * m10, c1*m11 - s1*m21);
    
    double scale = 180.0/3.14159;
    theta1 *= scale;
    theta2 *= scale;
    theta3 *= scale;
    
    Mat eular_angle = cv::Mat::zeros(3, 1, CV_64FC1);
    eular_angle.at<double>(0, 0) = theta1;
    eular_angle.at<double>(1, 0) = theta2;
    eular_angle.at<double>(2, 0) = theta3;
    return eular_angle;
    //printf("Eular angle %lf %lf %lf\n", theta1, theta2, theta3);    
}

void CvxPoseEstimation::poseDistance(const cv::Mat & src_pose,
                                     const cv::Mat & dst_pose,
                                     double & angle_distance,
                                     double & euclidean_disance)
{
    // http://chrischoy.github.io/research/measuring-rotation/
    assert(src_pose.type() == CV_64F);
    assert(dst_pose.type() == CV_64F);
    
    Mat src_R = src_pose(cv::Rect(0, 0, 3, 3));
    Mat dst_R = dst_pose(cv::Rect(0, 0, 3, 3));
    
    double scale = 180.0/3.14159;
    
    Mat q1 = CvxPoseEstimation::rotationToQuaternion(src_R);
    Mat q2 = CvxPoseEstimation::rotationToQuaternion(dst_R);
    double val_dot =fabs(q1.dot(q2));
    
    
    // double dot = r1.dot(r2);
    // angle_distance = acos(dot) * scale;
    angle_distance = 2.0 * acos(val_dot) *scale;

    
    euclidean_disance = 0.0;
    double dx = src_pose.at<double>(0, 3) - dst_pose.at<double>(0, 3);
    double dy = src_pose.at<double>(1, 3) - dst_pose.at<double>(1, 3);
    double dz = src_pose.at<double>(2, 3) - dst_pose.at<double>(2, 3);
    euclidean_disance += dx * dx;
    euclidean_disance += dy * dy;
    euclidean_disance += dz * dz;
    euclidean_disance = sqrt(euclidean_disance);
}

Mat CvxPoseEstimation::rotationToQuaternion(const cv::Mat & rot)
{
    assert(rot.type() == CV_64FC1);
    assert(rot.rows == 3 && rot.cols == 3);
    
    Mat ret = cv::Mat::zeros(4, 1, CV_64FC1);
    
    float r11 = rot.at<double>(0, 0);
    float r12 = rot.at<double>(0, 1);
    float r13 = rot.at<double>(0, 2);
    float r21 = rot.at<double>(1, 0);
    float r22 = rot.at<double>(1, 1);
    float r23 = rot.at<double>(1, 2);
    float r31 = rot.at<double>(2, 0);
    float r32 = rot.at<double>(2, 1);
    float r33 = rot.at<double>(2, 2);
    
    float q0 = ( r11 + r22 + r33 + 1.0f) / 4.0f;
    float q1 = ( r11 - r22 - r33 + 1.0f) / 4.0f;
    float q2 = (-r11 + r22 - r33 + 1.0f) / 4.0f;
    float q3 = (-r11 - r22 + r33 + 1.0f) / 4.0f;
    if(q0 < 0.0f) q0 = 0.0f;
    if(q1 < 0.0f) q1 = 0.0f;
    if(q2 < 0.0f) q2 = 0.0f;
    if(q3 < 0.0f) q3 = 0.0f;
    q0 = sqrt(q0);
    q1 = sqrt(q1);
    q2 = sqrt(q2);
    q3 = sqrt(q3);
    if(q0 >= q1 && q0 >= q2 && q0 >= q3) {
        q0 *= +1.0f;
        q1 *= CvxPoseEstimation::SIGN(r32 - r23);
        q2 *= CvxPoseEstimation::SIGN(r13 - r31);
        q3 *= CvxPoseEstimation::SIGN(r21 - r12);
    } else if(q1 >= q0 && q1 >= q2 && q1 >= q3) {
        q0 *= CvxPoseEstimation::SIGN(r32 - r23);
        q1 *= +1.0f;
        q2 *= CvxPoseEstimation::SIGN(r21 + r12);
        q3 *= CvxPoseEstimation::SIGN(r13 + r31);
    } else if(q2 >= q0 && q2 >= q1 && q2 >= q3) {
        q0 *= CvxPoseEstimation::SIGN(r13 - r31);
        q1 *= CvxPoseEstimation::SIGN(r21 + r12);
        q2 *= +1.0f;
        q3 *= CvxPoseEstimation::SIGN(r32 + r23);
    } else if(q3 >= q0 && q3 >= q1 && q3 >= q2) {
        q0 *= CvxPoseEstimation::SIGN(r21 - r12);
        q1 *= CvxPoseEstimation::SIGN(r31 + r13);
        q2 *= CvxPoseEstimation::SIGN(r32 + r23);
        q3 *= +1.0f;
    } else {
        
        printf("Error: rotation matrix quaternion.\n");
        assert(0);
    }
    float r = CvxPoseEstimation::NORM(q0, q1, q2, q3);
    q0 /= r;
    q1 /= r;
    q2 /= r;
    q3 /= r;
    
    ret.at<double>(0, 0) = q0;
    ret.at<double>(1, 0) = q1;
    ret.at<double>(2, 0) = q2;
    ret.at<double>(3, 0) = q3;
    return ret;
    
    
}




