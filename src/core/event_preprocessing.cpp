// iKalibr: Unified Targetless Spatiotemporal Calibration Framework
// Copyright 2024, the School of Geodesy and Geomatics (SGG), Wuhan University, China
// https://github.com/Unsigned-Long/iKalibr.git
// Author: Shuolong Chen (shlchen@whu.edu.cn)
// GitHub: https://github.com/Unsigned-Long
//  ORCID: 0000-0002-5283-9057
// Purpose: See .h/.hpp file.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * The names of its contributors can not be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "core/event_preprocessing.h"
#include "veta/camera/pinhole.h"
#include "opencv2/imgproc.hpp"
#include "util/status.hpp"
#include "opencv2/calib3d.hpp"
#include "opengv/sac/Ransac.hpp"
#include "config/configor.h"
#include "filesystem"
#include "cereal/types/tuple.hpp"
#include "cereal/types/list.hpp"
#include "cereal/types/utility.hpp"
#include "factor/data_correspondence.h"

#include <opencv2/highgui.hpp>

namespace {
bool IKALIBR_UNIQUE_NAME(_2_) = ns_ikalibr::_1_(__FILE__);
}

namespace ns_ikalibr {
/**
 * ActiveEventSurface
 */
ActiveEventSurface::ActiveEventSurface(const ns_veta::PinholeIntrinsic::Ptr &intri,
                                       double filterThd)
    : FILTER_THD(filterThd),
      _intri(intri),
      _undistoMap(VisualUndistortionMap::Create(_intri)),
      _eventImgMat(cv::Size(_intri->imgWidth, _intri->imgHeight), CV_8UC3, cv::Scalar(0, 0, 0)) {
    _sae[0] = Eigen::MatrixXd::Zero(_intri->imgWidth, _intri->imgHeight);
    _sae[1] = Eigen::MatrixXd::Zero(_intri->imgWidth, _intri->imgHeight);
    _saeLatest[0] = Eigen::MatrixXd::Zero(_intri->imgWidth, _intri->imgHeight);
    _saeLatest[1] = Eigen::MatrixXd::Zero(_intri->imgWidth, _intri->imgHeight);
    _timeLatest = 0.0;
}

ActiveEventSurface::Ptr ActiveEventSurface::Create(const ns_veta::PinholeIntrinsicPtr &intri,
                                                   double filterThd) {
    return std::make_shared<ActiveEventSurface>(intri, filterThd);
}

void ActiveEventSurface::GrabEvent(const Event::Ptr &event, bool drawEventMat) {
    const bool ep = event->GetPolarity();
    const std::uint16_t ex = event->GetPos()(0), ey = event->GetPos()(1);
    const double et = event->GetTimestamp();

    // update Surface of Active Events
    const int pol = ep ? 1 : 0;
    const int polInv = !ep ? 1 : 0;
    double &tLast = _saeLatest[pol](ex, ey);
    double &tLastInv = _saeLatest[polInv](ex, ey);

    if ((et > tLast + FILTER_THD) || (tLastInv > tLast)) {
        tLast = et;
        _sae[pol](ex, ey) = et;
    } else {
        tLast = et;
    }
    _timeLatest = et;

    if (drawEventMat) {
        // draw image
        _eventImgMat.at<cv::Vec3b>(cv::Point2d(ex, ey)) =
            ep ? cv::Vec3b(255, 0, 0) : cv::Vec3b(0, 0, 255);
    }
}

void ActiveEventSurface::GrabEvent(const EventArray::Ptr &events, bool drawEventMat) {
    for (const auto &event : events->GetEvents()) {
        GrabEvent(event, drawEventMat);
    }
}

cv::Mat ActiveEventSurface::GetEventImgMat(bool resetMat, bool undistoMat) {
    auto mat = _eventImgMat.clone();
    if (resetMat) {
        _eventImgMat =
            cv::Mat(cv::Size(_intri->imgWidth, _intri->imgHeight), CV_8UC3, cv::Scalar(0, 0, 0));
    }
    if (undistoMat) {
        return _undistoMap->RemoveDistortion(mat);
    } else {
        return mat;
    }
}

cv::Mat ActiveEventSurface::TimeSurface(bool ignorePolarity,
                                        bool undistoMat,
                                        int medianBlurKernelSize,
                                        double decaySec) {
    // create exponential-decayed Time Surface map
    const auto imgSize = cv::Size(_intri->imgWidth, _intri->imgHeight);
    cv::Mat timeSurfaceMap = cv::Mat::zeros(imgSize, CV_64F);

    for (int y = 0; y < imgSize.height; ++y) {
        for (int x = 0; x < imgSize.width; ++x) {
            const double mostRecentStampAtCoord = std::max(_sae[1](x, y), _sae[0](x, y));

            const double dt = _timeLatest - mostRecentStampAtCoord;
            double expVal = std::exp(-dt / decaySec);

            if (!ignorePolarity) {
                double polarity = _sae[1](x, y) > _sae[0](x, y) ? 1.0 : -1.0;
                expVal *= polarity;
            }
            timeSurfaceMap.at<double>(y, x) = expVal;
        }
    }

    if (!ignorePolarity) {
        timeSurfaceMap = 255.0 * (timeSurfaceMap + 1.0) / 2.0;
    } else {
        timeSurfaceMap = 255.0 * timeSurfaceMap;
    }
    timeSurfaceMap.convertTo(timeSurfaceMap, CV_8U);

    if (medianBlurKernelSize > 0) {
        cv::medianBlur(timeSurfaceMap, timeSurfaceMap, 2 * medianBlurKernelSize + 1);
    }

    if (undistoMat) {
        return _undistoMap->RemoveDistortion(timeSurfaceMap);
    } else {
        return timeSurfaceMap;
    }
}

std::pair<cv::Mat, cv::Mat> ActiveEventSurface::RawTimeSurface(bool ignorePolarity,
                                                               bool undistoMat) {
    // create exponential-decayed Time Surface map
    const auto imgSize = cv::Size(_intri->imgWidth, _intri->imgHeight);
    cv::Mat timeSurfaceMap = cv::Mat::zeros(imgSize, CV_64FC1);
    cv::Mat polarityMap = cv::Mat::zeros(imgSize, CV_8UC1);

    for (int y = 0; y < imgSize.height; ++y) {
        for (int x = 0; x < imgSize.width; ++x) {
            double mostRecentStampAtCoord = std::max(_sae[1](x, y), _sae[0](x, y));
            double polarity = _sae[1](x, y) > _sae[0](x, y) ? 1.0 : -1.0;
            polarityMap.at<uchar>(y, x) = polarity;
            if (!ignorePolarity) {
                mostRecentStampAtCoord *= polarity;
            }
            timeSurfaceMap.at<double>(y, x) = mostRecentStampAtCoord;
        }
    }
    if (undistoMat) {
        return {_undistoMap->RemoveDistortion(timeSurfaceMap),
                _undistoMap->RemoveDistortion(polarityMap)};
    } else {
        return {timeSurfaceMap, polarityMap};
    }
}

double ActiveEventSurface::GetTimeLatest() const { return _timeLatest; }

EventArray::Ptr EventNormFlow::NormFlowPack::ActiveEvents(double dt) const {
    std::list<Event::Ptr> events;
    const int rows = rawTimeSurfaceMap.rows;
    const int cols = rawTimeSurfaceMap.cols;
    for (int ey = 0; ey < rows; ey++) {
        for (int ex = 0; ex < cols; ex++) {
            const auto &et = rawTimeSurfaceMap.at<double>(ey, ex);
            // whether this pixel is assigned
            if (et < 1E-3 || timestamp - et > dt) {
                continue;
            }
            const auto &ep = polarityMap.at<uchar>(ey, ex);
            events.push_back(Event::Create(et, {ex, ey}, ep));
        }
    }
    if (events.size() > 0) {
        std::vector<Event::Ptr> eventsVec(events.cbegin(), events.cend());
        return EventArray::Create(eventsVec.back()->GetTimestamp(), eventsVec);
    } else {
        return nullptr;
    }
}

EventArray::Ptr EventNormFlow::NormFlowPack::NormFlowEvents() const {
    std::list<Event::Ptr> events;
    const int rows = rawTimeSurfaceMap.rows;
    const int cols = rawTimeSurfaceMap.cols;
    for (int ey = 0; ey < rows; ey++) {
        for (int ex = 0; ex < cols; ex++) {
            const auto &et = rawTimeSurfaceMap.at<double>(ey, ex);
            // whether this pixel is assigned
            if (et < 1E-3) {
                continue;
            }
            if (inliersOccupy.at<uchar>(ey, ex) == 0) {
                continue;
            }
            const auto &ep = polarityMap.at<uchar>(ey, ex);
            events.push_back(Event::Create(et, {ex, ey}, ep));
        }
    }
    if (events.size() > 0) {
        std::vector<Event::Ptr> eventsVec(events.cbegin(), events.cend());
        return EventArray::Create(eventsVec.back()->GetTimestamp(), eventsVec);
    } else {
        return nullptr;
    }
}

cv::Mat EventNormFlow::NormFlowPack::Visualization(double dt) const {
    cv::Mat m1;
    cv::hconcat(nfSeedsImg, nfsImg, m1);

    cv::Mat actEventMat(nfSeedsImg.size(), CV_8UC3, cv::Scalar(0, 0, 0));
    for (const auto &event : this->ActiveEvents(dt)->GetEvents()) {
        auto ex = event->GetPos()(0), ey = event->GetPos()(1);
        auto ep = event->GetPolarity();
        actEventMat.at<cv::Vec3b>(cv::Point2d(ex, ey)) =
            ep ? cv::Vec3b(255, 0, 0) : cv::Vec3b(0, 0, 255);
    }

    cv::Mat nfEventMat(nfSeedsImg.size(), CV_8UC3, cv::Scalar(0, 0, 0));
    for (const auto &event : this->NormFlowEvents()->GetEvents()) {
        auto ex = event->GetPos()(0), ey = event->GetPos()(1);
        auto ep = event->GetPolarity();
        nfEventMat.at<cv::Vec3b>(cv::Point2d(ex, ey)) =
            ep ? cv::Vec3b(255, 0, 0) : cv::Vec3b(0, 0, 255);
    }

    cv::Mat m2;
    cv::hconcat(actEventMat, nfEventMat, m2);

    cv::Mat m3;
    cv::vconcat(m1, m2, m3);

    return m3;
}

EventNormFlow::NormFlowPack::Ptr EventNormFlow::ExtractNormFlows(double decaySec,
                                                                 int winSize,
                                                                 int neighborDist,
                                                                 double goodRatioThd,
                                                                 double timeDistEventToPlaneThd,
                                                                 int ransacMaxIter) const {
    // CV_64FC1
    auto [rtsMat, pMat] = _sea->RawTimeSurface(true, true /*todo: undistorted*/);
    // CV_8UC1
    auto tsImg = _sea->TimeSurface(true, true /*todo: undistorted*/, 0, decaySec);
    const double timeLast = _sea->GetTimeLatest();
    cv::Mat mask;
    cv::inRange(rtsMat, std::max(1E-3, timeLast - 1.5 * decaySec), timeLast, mask);

    cv::cvtColor(tsImg, tsImg, cv::COLOR_GRAY2BGR);
    auto tsImgNfs = tsImg.clone();

    const int ws = winSize;
    const int subTravSize = std::max(winSize, neighborDist);
    const int winSampleCount = (2 * ws + 1) * (2 * ws + 1);
    const int winSampleCountThd = winSampleCount * goodRatioThd;
    const int rows = mask.rows;
    const int cols = mask.cols;
    cv::Mat occupy = cv::Mat::zeros(rows, cols, CV_8UC1);
    cv::Mat inliersOccupy = cv::Mat::zeros(rows, cols, CV_8UC1);
    std::list<NormFlow::Ptr> nfs;
#define OUTPUT_PLANE_FIT 0
#if OUTPUT_PLANE_FIT
    std::list<std::pair<Eigen::Vector3d, std::list<std::tuple<double, double, double>>>> drawData;
#endif
    for (int y = subTravSize; y < mask.rows - subTravSize; y++) {
        for (int x = subTravSize; x < mask.cols - subTravSize; x++) {
            if (mask.at<uchar>(y /*row*/, x /*col*/) != 255) {
                continue;
            }
            // for this window, obtain the values [x, y, timestamp]
            std::vector<std::tuple<int, int, double>> inRangeData;
            double timeCen = 0.0;
            inRangeData.reserve(winSampleCount);
            bool jumpCurPixel = false;
            for (int dy = -subTravSize; dy <= subTravSize; ++dy) {
                for (int dx = -subTravSize; dx <= subTravSize; ++dx) {
                    int nx = x + dx;
                    int ny = y + dy;

                    // this pixel is in neighbor range
                    if (std::abs(dx) <= neighborDist && std::abs(dy) <= neighborDist) {
                        if (occupy.at<uchar>(ny /*row*/, nx /*col*/) == 255) {
                            // this pixl has been occupied, thus the current pixel would not be
                            // considered in norm flow estimation
                            jumpCurPixel = true;
                            break;
                        }
                    }

                    // this pixel is not considered in the window
                    if (std::abs(dx) > ws || std::abs(dy) > ws) {
                        continue;
                    }

                    // in window but not involved in norm flow estimation
                    if (mask.at<uchar>(ny /*row*/, nx /*col*/) != 255) {
                        continue;
                    }

                    double timestamp = rtsMat.at<double>(ny /*row*/, nx /*col*/);
                    inRangeData.emplace_back(nx, ny, timestamp);
                    if (nx == x && ny == y) {
                        timeCen = timestamp;
                    }
                }
                if (jumpCurPixel) {
                    break;
                }
            }
            // data in this window is sufficient
            if (jumpCurPixel || static_cast<int>(inRangeData.size()) < winSampleCountThd) {
                continue;
            }
            /**
             * drawing
             */
            tsImg.at<cv::Vec3b>(y, x) = cv::Vec3b(0, 0, 255);  // selected but not verified
            occupy.at<uchar>(y /*row*/, x /*col*/) = 255;

            // try fit planes using ransac
            auto centeredInRangeData = Centralization(inRangeData);
            opengv::sac::Ransac<EventLocalPlaneSacProblem> ransac;
            std::shared_ptr<EventLocalPlaneSacProblem> probPtr(
                new EventLocalPlaneSacProblem(centeredInRangeData));
            ransac.sac_model_ = probPtr;
            // the point to plane threshold in temporal domain
            ransac.threshold_ = timeDistEventToPlaneThd;
            ransac.max_iterations_ = ransacMaxIter;
            auto res = ransac.computeModel();

            if (!res || ransac.inliers_.size() / (double)inRangeData.size() < goodRatioThd) {
                continue;
            }
            // success
            Eigen::Vector3d abc;
            probPtr->optimizeModelCoefficients(ransac.inliers_, ransac.model_coefficients_, abc);

            // 'abd' is the params we are interested in
            const double dtdx = -abc(0), dtdy = -abc(1);
            Eigen::Vector2d nf = 1.0 / (dtdx * dtdx + dtdy * dtdy) * Eigen::Vector2d(dtdx, dtdy);

            if (nf.squaredNorm() > 4E3 * 4E3) {
                // the fitted plane is orthogonal to the t-axis, todo: a better way?
                continue;
            }

            nfs.push_back(NormFlow::Create(timeCen, Eigen::Vector2i{x, y}, nf));

            for (int idx : ransac.inliers_) {
                const auto &[ex, ey, et] = inRangeData.at(idx);
                inliersOccupy.at<uchar>(ey /*row*/, ex /*col*/) = 255;
            }

            /**
             * drawing
             */
            tsImg.at<cv::Vec3b>(y, x) = cv::Vec3b(0, 255, 0);  // selected and verified
            DrawLineOnCVMat(tsImgNfs, Eigen::Vector2d{x, y} + 0.01 * nf, {x, y});

#if OUTPUT_PLANE_FIT
            std::list<std::tuple<double, double, double>> centeredInliers;
            for (int idx : ransac.inliers_) {
                centeredInliers.push_back(centeredInRangeData.at(idx));
            }
            drawData.push_back({abc, centeredInliers});
#endif
        }
    }
#if OUTPUT_PLANE_FIT
    auto path = Configor::DataStream::DebugPath;
    static int count = 0;
    if (std::filesystem::exists(path)) {
        auto filename = path + "/event_local_planes" + std::to_string(count++) + ".yaml";
        std::ofstream ofs(filename);
        cereal::YAMLOutputArchive ar(ofs);
        ar(cereal::make_nvp("event_local_planes", drawData));
    }
#endif

    auto pack = std::make_shared<NormFlowPack>();
    pack->nfs = nfs;
    pack->inliersOccupy = inliersOccupy;
    pack->polarityMap = pMat;
    pack->rawTimeSurfaceMap = rtsMat;
    pack->timestamp = _sea->GetTimeLatest();
    // for visualization
    pack->nfsImg = tsImgNfs;
    pack->nfSeedsImg = tsImg;
    return pack;
}

std::vector<std::tuple<double, double, double>> EventNormFlow::Centralization(
    const std::vector<std::tuple<int, int, double>> &inRangeData) {
    double mean1 = 0.0, mean2 = 0.0, mean3 = 0.0;

    for (const auto &t : inRangeData) {
        mean1 += std::get<0>(t);
        mean2 += std::get<1>(t);
        mean3 += std::get<2>(t);
    }

    size_t n = inRangeData.size();
    mean1 /= n;
    mean2 /= n;
    mean3 /= n;

    std::vector<std::tuple<double, double, double>> centeredInRangeData;
    centeredInRangeData.resize(inRangeData.size());

    for (size_t i = 0; i < inRangeData.size(); ++i) {
        centeredInRangeData[i] = std::make_tuple(std::get<0>(inRangeData[i]) - mean1,
                                                 std::get<1>(inRangeData[i]) - mean2,
                                                 std::get<2>(inRangeData[i]) - mean3);
    }

    return centeredInRangeData;
}

/**
 * EventLocalPlaneSacProblem
 */
int EventLocalPlaneSacProblem::getSampleSize() const { return 3; }

double EventLocalPlaneSacProblem::PointToPlaneDistance(
    double x, double y, double t, double A, double B, double C) {
    // double numerator = std::abs(A * x + B * y + t + C);
    // double denominator = std::sqrt(A * A + B * B + 1);
    // return numerator / denominator;
    double tPred = -(A * x + B * y + C);
    return std::abs(t - tPred);
}

bool EventLocalPlaneSacProblem::computeModelCoefficients(const std::vector<int> &indices,
                                                         model_t &outModel) const {
    Eigen::MatrixXd M(indices.size(), 3);
    Eigen::VectorXd b(indices.size());

    for (int i = 0; i < static_cast<int>(indices.size()); ++i) {
        const auto &[x, y, t] = _data.at(indices.at(i));
        M(i, 0) = x;
        M(i, 1) = y;
        M(i, 2) = 1;
        b(i) = -t;
    }
    outModel = (M.transpose() * M).ldlt().solve(M.transpose() * b);
    return true;
}

void EventLocalPlaneSacProblem::getSelectedDistancesToModel(const model_t &model,
                                                            const std::vector<int> &indices,
                                                            std::vector<double> &scores) const {
    scores.resize(indices.size());
    for (int i = 0; i < static_cast<int>(indices.size()); ++i) {
        const auto &[x, y, t] = _data.at(indices.at(i));
        scores.at(i) = PointToPlaneDistance(x, y, t, model(0), model(1), model(2));
    }
}

void EventLocalPlaneSacProblem::optimizeModelCoefficients(const std::vector<int> &inliers,
                                                          const model_t &model,
                                                          model_t &optimized_model) {
    computeModelCoefficients(inliers, optimized_model);
}

/**
 * EventLineTracking::EventLine
 */
std::uint32_t EventLineTracking::EventLine::idCounter = 0;

EventLineTracking::EventLine::EventLine(const NormFlowPtr &nf)
    : id(idCounter++),
      timestamp(nf->timestamp),
      activity(1.0) {
    param.head<2>() = nf->nfDir;
    param.tail<1>() = nf->p.cast<double>().transpose() * nf->nfDir;
}

EventLineTracking::EventLine::Ptr EventLineTracking::EventLine::Create(const NormFlowPtr &nf) {
    return std::make_shared<EventLine>(nf);
}

double EventLineTracking::EventLine::PointToLine(const Eigen::Vector2d &p) const {
    return (p.transpose() * param.head<2>() - param.tail<1>())(0);
}

double EventLineTracking::EventLine::DirectionDifferenceCos(const Eigen::Vector2d &nfDir) const {
    return (param.head<2>().transpose() * nfDir)(0);
}

void EventLineTracking::EventLine::Normalize() { param /= param.head<2>().norm(); }

/**
 * EventLineTracking::LineParamUpdate
 */
EventLineTracking::LineParamUpdate::LineParamUpdate(double x, double y)
    : x(x),
      y(y),
      xx(x * x),
      yy(y * y),
      xy(x * y) {}

EventLineTracking::LineParamUpdate::Ptr EventLineTracking::LineParamUpdate::Create(double x,
                                                                                   double y) {
    return std::make_shared<LineParamUpdate>(x, y);
}

void EventLineTracking::LineParamUpdate::Update(double x, double y, double delta) {
    this->xy = delta * this->xy + x * y;
    this->yy = delta * this->yy + y * y;
    this->xx = delta * this->xx + x * x;
    this->x = delta * this->x + x;
    this->y = delta * this->y + y;
}

std::array<Eigen::Vector3d, 2> EventLineTracking::LineParamUpdate::ObtainLine(double omega) const {
    const double a = omega * (yy - xx) + x * x - y * y;
    const double a2 = a * a;

    const double b = 2.0 * (omega * xy - x * y);
    const double b2 = b * b;

    const double beta = std::sqrt(a2 / (a2 + b2));

    const double gamma1 = std::sqrt((1.0 - beta) * 0.5);
    const double gamma2 = std::sqrt((1.0 + beta) * 0.5);

    constexpr double cospi4 = M_SQRT2 * 0.5;
    // cos, sin (norm dir)
    std::array<std::pair<double, double>, 2> solutions;
    {
        // case 1
        double cos = gamma2, sin = gamma1;
        if (-b / a /* tan(2 * theta) */ > 0) {
            if (cos < cospi4) {
                sin *= -1.0;
            }
        } else {
            if (cos > cospi4) {
                sin *= -1.0;
            }
        }
        solutions[0] = {cos, sin};
    }
    {
        // case 2
        double cos = gamma1, sin = gamma2;
        if (-b / a /* tan(2 * theta) */ > 0) {
            if (cos < cospi4) {
                sin *= -1.0;
            }
        } else {
            if (cos > cospi4) {
                sin *= -1.0;
            }
        }
        solutions[1] = {cos, sin};
    }
    std::array<Eigen::Vector3d, 2> results;
    for (size_t i = 0; i < 2; ++i) {
        const auto &[cos, sin] = solutions[i];
        const double rho = (x * cos + y * sin) / omega;
        results[i] = Eigen::Vector3d(cos, sin, rho);
    }
    return results;
}

/**
 * EventLineTracking
 */
void EventLineTracking::TrackingUsingNormFlow(const EventNormFlow::NormFlowPack::Ptr &nfPack) {
    // sort norm flow data
    nfPack->nfs.sort([&](const NormFlow::Ptr &a, const NormFlow::Ptr &b) {
        return a->timestamp < b->timestamp;
    });

#define SHOW_DETAILS 0

    std::list<EventLine::Ptr> lines;
    std::map<EventLine::Ptr, std::list<NormFlow::Ptr>> lineWithNFs;
    std::map<EventLine::Ptr, LineParamUpdate::Ptr> lineParamUpdate;

    double lastTime = nfPack->nfs.front()->timestamp;
    for (const auto &nf : nfPack->nfs) {
        // compute the delta time, update last time stamp
        double dt = nf->timestamp - lastTime;
        const double decay = std::exp(-nf->nfNorm * dt);
        lastTime = nf->timestamp;

        // spdlog::info("p: ({}, {}), t: {:.5f} (sec), dt: {:.3f}, nf: ({:.3f}, {:.3f})", nf->p(0),
        //              nf->p(1), nf->timestamp, dt, nf->nf(0), nf->nf(1));

        EventLine::Ptr ml = nullptr;
        for (auto &el : lines) {
            // apply the decay
            el->activity *= decay;
            el->timestamp = nf->timestamp;

            // association
            const double dist = std::abs(el->PointToLine(nf->p.cast<double>()));
            const double dirDiffCos = el->DirectionDifferenceCos(nf->nfDir);
            // too far
            if (dist > P2L_DISTANCE_THD) {
#if SHOW_DETAILS
                spdlog::warn("distance too far!!! Jump it!!!");
#endif
                continue;
            }
            // not same direction
            if (dirDiffCos < P2L_ORIENTATION_THD) {
#if SHOW_DETAILS
                spdlog::warn("orientation difference too large!!! Jump it!!!");
#endif
                continue;
            }
            // find the line whose activity is the largest, i.e., matched (associated) line
            if (ml == nullptr) {
                ml = el;
            } else {
                if (ml->activity < el->activity) {
                    ml = el;
                }
            }
#if SHOW_DETAILS
            spdlog::info("associate line!!!");
            auto mat = nfPack->nfSeedsImg.clone();
            DrawLine(mat, el);
            DrawKeypointOnCVMat(mat, nf->p.cast<double>(), true, cv::Scalar(255, 0, 0));
            cv::imshow("Associate To Event Line", mat);
            cv::waitKey(0);
#endif
        }

        // initialize a new line
        if (ml == nullptr) {
            if (lines.size() >= MAX_LINE_COUNT) {
                // find the one with the lowest activity
                auto i = std::min_element(lines.begin(), lines.end(),
                                          [](const EventLine::Ptr &el1, const EventLine::Ptr &el2) {
                                              return el1->activity < el2->activity;
                                          });
                lines.erase(i);
            }
            auto newLine = EventLine::Create(nf);
            lines.push_back(newLine);
            lineWithNFs[newLine].push_back(nf);
            lineParamUpdate[newLine] = LineParamUpdate::Create(nf->p(0), nf->p(1));
#if SHOW_DETAILS
            // visualization
            auto mat = nfPack->nfSeedsImg.clone();
            DrawLine(mat, newLine);
            // yellow, new associated norm flow
            DrawKeypointOnCVMat(mat, nf->p.cast<double>(), true, cv::Scalar(0, 255, 255));
            cv::imshow("Newly Initialized Event Line", mat);
            cv::waitKey(0);
#endif
            continue;
        }
        spdlog::info("update line using associated norm flow (blue old, yellow new)");
        spdlog::info("line id: {}, associated nfs count: {}, activity: {:.3f}", ml->id,
                     lineWithNFs[ml].size(), ml->activity);

        // update activity of this matched line
        ml->activity += 1.0;

        // add current norm flow to nf container of this matched line
        auto &curLineWithNFs = lineWithNFs[ml];
        curLineWithNFs.push_back(nf);

        // update implicit line parameters
        auto &lParam = lineParamUpdate[ml];
        lParam->Update(nf->p(0), nf->p(1), decay);

        // cos, sin, rho, two possible solutions (candidates)
        auto res = lParam->ObtainLine(ml->activity);

        // choose the best match line
        Eigen::Vector3d nl;
        if (std::abs(ml->param(0) /*old cos*/ - res.at(0)(0) /*new cos 1*/) <
            std::abs(ml->param(0) /*old cos*/ - res.at(1)(0) /*new cos 2*/)) {
            nl = res.at(0);
        } else {
            nl = res.at(1);
        }

        // visualization
        auto mat = nfPack->nfsImg.clone();
        DrawLine(mat, ml, cv::Scalar(255, 0, 0));
        for (const auto &oldNF : curLineWithNFs) {
            // blue, old associated norm flow
            DrawKeypointOnCVMat(mat, oldNF->p.cast<double>(), true, cv::Scalar(255, 0, 0));
        }
        for (const auto &r : res) {
            DrawLine(mat, r(0), r(1), -r(2), cv::Scalar(0, 0, 255));
        }
        DrawLine(mat, nl(0), nl(1), -nl(2), cv::Scalar(0, 255, 255));
        // yellow, new associated norm flow
        DrawKeypointOnCVMat(mat, nf->p.cast<double>(), true, cv::Scalar(0, 255, 255));
        cv::imshow("Newly Associated Event Norm Flow", mat);
        cv::waitKey(0);

        // update
        ml->param = nl;
    }
}

void EventLineTracking::DrawLine(cv::Mat &m, const EventLine::Ptr &el, const cv::Scalar &color) {
    const float a = el->param(0), b = el->param(1), c = -el->param(2);
    DrawLine(m, a, b, c, color);
}

void EventLineTracking::DrawLine(
    cv::Mat &m, const double a, const double b, const double c, const cv::Scalar &color) {
    const int width = m.cols, height = m.rows;
    cv::Point2f p1, p2;
    if (b == 0) {
        p1.x = 0;
        p1.y = -c / a;
        p2.x = width;
        p2.y = -c / a;
    } else if (a == 0) {
        p1.x = -c / b;
        p1.y = 0;
        p2.x = -c / b;
        p2.y = height;
    } else {
        p1.x = -c / a;
        p1.y = 0;
        p2.x = -(c + b * height) / a;
        p2.y = height;
    }
    DrawLineOnCVMat(m, p1, p2, color);
}

}  // namespace ns_ikalibr