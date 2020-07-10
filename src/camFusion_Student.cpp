
#include <iostream>
#include <algorithm>
#include <numeric>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "camFusion.hpp"
#include "dataStructures.h"

using namespace std;


// Create groups of Lidar points whose projection into the camera falls into the same bounding box
void clusterLidarWithROI(std::vector<BoundingBox> &boundingBoxes, std::vector<LidarPoint> &lidarPoints, float shrinkFactor, cv::Mat &P_rect_xx, cv::Mat &R_rect_xx, cv::Mat &RT)
{
    // loop over all Lidar points and associate them to a 2D bounding box
    cv::Mat X(4, 1, cv::DataType<double>::type);
    cv::Mat Y(3, 1, cv::DataType<double>::type);

    for (auto it1 = lidarPoints.begin(); it1 != lidarPoints.end(); ++it1)
    {
        // assemble vector for matrix-vector-multiplication
        X.at<double>(0, 0) = it1->x;
        X.at<double>(1, 0) = it1->y;
        X.at<double>(2, 0) = it1->z;
        X.at<double>(3, 0) = 1;

        // project Lidar point into camera
        Y = P_rect_xx * R_rect_xx * RT * X;
        cv::Point pt;
        pt.x = Y.at<double>(0, 0) / Y.at<double>(0, 2); // pixel coordinates
        pt.y = Y.at<double>(1, 0) / Y.at<double>(0, 2);

        vector<vector<BoundingBox>::iterator> enclosingBoxes; // pointers to all bounding boxes which enclose the current Lidar point
        for (vector<BoundingBox>::iterator it2 = boundingBoxes.begin(); it2 != boundingBoxes.end(); ++it2)
        {
            // shrink current bounding box slightly to avoid having too many outlier points around the edges
            cv::Rect smallerBox;
            smallerBox.x = (*it2).roi.x + shrinkFactor * (*it2).roi.width / 2.0;
            smallerBox.y = (*it2).roi.y + shrinkFactor * (*it2).roi.height / 2.0;
            smallerBox.width = (*it2).roi.width * (1 - shrinkFactor);
            smallerBox.height = (*it2).roi.height * (1 - shrinkFactor);

            // check wether point is within current bounding box
            if (smallerBox.contains(pt))
            {
                enclosingBoxes.push_back(it2);
            }

        } // eof loop over all bounding boxes

        // check wether point has been enclosed by one or by multiple boxes
        if (enclosingBoxes.size() == 1)
        { 
            // add Lidar point to bounding box
            enclosingBoxes[0]->lidarPoints.push_back(*it1);
        }

    } // eof loop over all Lidar points
}


void show3DObjects(std::vector<BoundingBox> &boundingBoxes, cv::Size worldSize, cv::Size imageSize, bool bWait)
{
    // create topview image
    cv::Mat topviewImg(imageSize, CV_8UC3, cv::Scalar(255, 255, 255));

    for(auto it1=boundingBoxes.begin(); it1!=boundingBoxes.end(); ++it1)
    {
        // create randomized color for current 3D object
        cv::RNG rng(it1->boxID);
        cv::Scalar currColor = cv::Scalar(rng.uniform(0,150), rng.uniform(0, 150), rng.uniform(0, 150));

        // plot Lidar points into top view image
        int top=1e8, left=1e8, bottom=0.0, right=0.0; 
        float xwmin=1e8, ywmin=1e8, ywmax=-1e8;
        for (auto it2 = it1->lidarPoints.begin(); it2 != it1->lidarPoints.end(); ++it2)
        {
            // world coordinates
            float xw = (*it2).x; // world position in m with x facing forward from sensor
            float yw = (*it2).y; // world position in m with y facing left from sensor
            xwmin = xwmin<xw ? xwmin : xw;
            ywmin = ywmin<yw ? ywmin : yw;
            ywmax = ywmax>yw ? ywmax : yw;

            // top-view coordinates
            int y = (-xw * imageSize.height / worldSize.height) + imageSize.height;
            int x = (-yw * imageSize.width / worldSize.width) + imageSize.width / 2;

            // find enclosing rectangle
            top = top<y ? top : y;
            left = left<x ? left : x;
            bottom = bottom>y ? bottom : y;
            right = right>x ? right : x;

            // draw individual point
            cv::circle(topviewImg, cv::Point(x, y), 4, currColor, -1);
        }

        // draw enclosing rectangle
        cv::rectangle(topviewImg, cv::Point(left, top), cv::Point(right, bottom),cv::Scalar(0,0,0), 2);

        // augment object with some key data
        char str1[200], str2[200];
        sprintf(str1, "id=%d, #pts=%d", it1->boxID, (int)it1->lidarPoints.size());
        putText(topviewImg, str1, cv::Point2f(left-250, bottom+50), cv::FONT_ITALIC, 2, currColor);
        sprintf(str2, "xmin=%2.2f m, yw=%2.2f m", xwmin, ywmax-ywmin);
        putText(topviewImg, str2, cv::Point2f(left-250, bottom+125), cv::FONT_ITALIC, 2, currColor);  
    }

    // plot distance markers
    float lineSpacing = 2.0; // gap between distance markers
    int nMarkers = floor(worldSize.height / lineSpacing);
    for (size_t i = 0; i < nMarkers; ++i)
    {
        int y = (-(i * lineSpacing) * imageSize.height / worldSize.height) + imageSize.height;
        cv::line(topviewImg, cv::Point(0, y), cv::Point(imageSize.width, y), cv::Scalar(255, 0, 0));
    }

    // display image
    string windowName = "3D Objects";
    cv::namedWindow(windowName, 1);
    cv::imshow(windowName, topviewImg);

    if(bWait)
    {
        cv::waitKey(0); // wait for key to be pressed
    }
}


// associate a given bounding box with the keypoints it contains
void clusterKptMatchesWithROI(BoundingBox &boundingBox, std::vector<cv::KeyPoint> &kptsPrev,
                        std::vector<cv::KeyPoint> &kptsCurr, std::vector<cv::DMatch> &kptMatches) {

    // Iterate through the keypoint matches and check if the corresponding 
    // trainId pt in kptsCurr falls in the ROI of the BB
    std::vector<cv::DMatch> kptsRoi;
    std::vector<double> accDistances;
    for (auto eachMatch : kptMatches) {
        
        cv::KeyPoint kpt = kptsCurr[eachMatch.trainIdx];

        if (boundingBox.roi.contains(cv::Point(kpt.pt.x, kpt.pt.y))) {
            kptsRoi.push_back(eachMatch);
            accDistances.push_back(eachMatch.distance);
        }
    
    }
    long  meanDist = std::accumulate(accDistances.begin(), accDistances.end(), 0)/accDistances.size();
    double threshold = 0.7*meanDist;

    for (auto eachRoiMatch : kptsRoi) {
        if (eachRoiMatch.distance < threshold) {
            boundingBox.kptMatches.push_back(eachRoiMatch);
        }
    }

}


// Compute time-to-collision (TTC) based on keypoint correspondences in successive images
void computeTTCCamera(std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, 
                      std::vector<cv::DMatch> kptMatches, double frameRate, double &TTC, cv::Mat *visImg)
{
        vector<double> distRatios; // stores the distance ratios for all keypoints between curr. and prev. frame
        for (auto it1 = kptMatches.begin(); it1 != kptMatches.end() - 1; ++it1)
        { // outer kpt. loop

            // get current keypoint and its matched partner in the prev. frame
            cv::KeyPoint kpOuterCurr = kptsCurr.at(it1->trainIdx);
            cv::KeyPoint kpOuterPrev = kptsPrev.at(it1->queryIdx);

            for (auto it2 = kptMatches.begin() + 1; it2 != kptMatches.end(); ++it2)
            { // inner kpt.-loop

                double minDist = 100.0; // min. required distance

                // get next keypoint and its matched partner in the prev. frame
                cv::KeyPoint kpInnerCurr = kptsCurr.at(it2->trainIdx);
                cv::KeyPoint kpInnerPrev = kptsPrev.at(it2->queryIdx);

                // compute distances and distance ratios
                double distCurr = cv::norm(kpOuterCurr.pt - kpInnerCurr.pt);
                double distPrev = cv::norm(kpOuterPrev.pt - kpInnerPrev.pt);

                if (distPrev > std::numeric_limits<double>::epsilon() && distCurr >= minDist)
                { // avoid division by zero

                    double distRatio = distCurr / distPrev;
                    distRatios.push_back(distRatio);
                }
            } // eof inner loop over all matched kpts
    }     // eof outer loop over all matched kpts

    // only continue if list of distance ratios is not empty
    if (distRatios.size() == 0)
    {
        TTC = NAN;
        return;
    }

    // compute camera-based TTC from distance ratios
	std::sort(distRatios.begin(), distRatios.end()); 
    double medianDistRatio;
  	if (distRatios.size()%2 == 0) {
    	medianDistRatio = distRatios[distRatios.size()/2+1];
    } else {
    	medianDistRatio = distRatios[distRatios.size()/2];
    }
    double dT = 1 / frameRate;
    TTC = -dT / (1 - medianDistRatio);
}


void computeTTCLidar(std::vector<LidarPoint> &lidarPointsPrev,
                     std::vector<LidarPoint> &lidarPointsCurr, double frameRate, double &TTC)
{
    // auxiliary variables
    // double dT = 0.1;        
    // time between two measurements in seconds
    
    double laneWidth = 4.0; // assumed width of the ego lane

    // find closest distance to Lidar points within ego lane
    double minXPrev = 1e9, minXCurr = 1e9;
    double minXPrevSize = 0;
    double minXCurrSize = 0;
    
    if (!lidarPointsCurr.size() || !lidarPointsPrev.size()) {
        TTC = NAN;
        return;
    }

    for (auto it = lidarPointsPrev.begin(); it != lidarPointsPrev.end(); ++it)
    {	
      	if (std::abs(it->y) <= (laneWidth/2.0)) {
        	minXPrev = minXPrev > it->x ? it->x : minXPrev;
            minXPrevSize++;
            // minXPrev = minXPrev/minXPrevSize;
        }
     }
     minXPrev = minXPrev = minXPrev/minXPrevSize;

    for (auto it = lidarPointsCurr.begin(); it != lidarPointsCurr.end(); ++it)
    {	
      	if (std::abs(it->y) <= (laneWidth/2.0)) {
        	minXCurr = minXCurr > it->x ? it->x : minXCurr;
            minXCurrSize++;
        }
    }
    minXCurr = minXCurr/minXCurrSize;
    minXPrev = minXPrev/minXPrevSize;
    // compute TTC from both measurements
    TTC = minXCurr * (1/frameRate) / (minXPrev - minXCurr);
}


void matchBoundingBoxes(std::vector<cv::DMatch> &matches, std::map<int, int> &bbBestMatches, DataFrame &prevFrame, DataFrame &currFrame) {
    // For each of the matches between the frames
    // for (auto &each_match : matches)  {
    //     // For every frame in the bounding box for current frame
    //     for (auto &each_box : currFrame.boundingBoxes) {
    //         if (bbBestMatches.find(each_box.boxID) == bbBestMatches.end()) {
    //             if (each_box.roi.contains(currFrame.keypoints[each_match.trainIdx].pt)) {
    //                 for (auto &each_prev_box : prevFrame.boundingBoxes) {
    //                     if (each_prev_box.roi.contains(prevFrame.keypoints[each_match.queryIdx].pt)) {
    //                         bbBestMatches[each_box.boxID] = each_prev_box.boxID; 
    //                     }
    //                 }
    //             }
    //         }
    //     }
    // }  
    int prevSize = prevFrame.boundingBoxes.size();
    int currSize = currFrame.boundingBoxes.size();

    int matchMatrix[currSize][prevSize] = { };

    // For every match, create a currID -> prevID matrix where
    // matrix[currID][prevID] represents number of keypoints matches
    // for that particular pair.
    for (auto eachMatch : matches) {
        // std::map<int, int> queryIds;
        int currID = -1;
        int prevID = -1;
        
        for (auto eachCurrBox : currFrame.boundingBoxes) {
            if (eachCurrBox.roi.contains(currFrame.keypoints[eachMatch.trainIdx].pt)) {
                currID = eachCurrBox.boxID;
            }
        }

        for (auto eachPrevBox : prevFrame.boundingBoxes) {
            if (eachPrevBox.roi.contains(prevFrame.keypoints[eachMatch.queryIdx].pt)) {
                // bbBestMatches[each_prev_box.boxID] =. -1;
                // if (queryIds.find(eachMatch.queryIdx) != queryIds.end()) {
                    //  = eachPrevBox.boxID;
                // }
                prevID = eachPrevBox.boxID;
            }
        }


        if ((currID != -1) && (prevID != -1)) {
            matchMatrix[currID][prevID]++;
        }

    }

    for (int i = 0; i < currSize; i++) {
        int maxCount = INT_MIN;
        int index = 0;
        for (int j = 0; j < prevSize; j++) {
            if (matchMatrix[i][j] > maxCount) {
                maxCount = matchMatrix[i][j];
                index = j;
            }
        }
        bbBestMatches[i] = index;
    }



}
