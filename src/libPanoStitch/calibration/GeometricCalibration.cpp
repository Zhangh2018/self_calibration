#include <iostream>
#include <unordered_map>
#include <random>
//#define BOOST_FILESYSTEM_NO_DEPRECATED
//#include <boost/filesystem.hpp>
#include "opencv2/opencv.hpp"
#include "opencv2/stitching/detail/matchers.hpp"
#include "ceres/ceres.h"
#include "ceres/rotation.h"

#include "GeometricCalibration.h"
#include "Camera.h"
#include "SystemUtil.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

using namespace surround360;

static std::string FLAGS_frames = "";
static int64_t FLAGS_pass_count = 2;
static double FLAGS_outlier_factor = 5;
static bool FLAGS_robust = true;
static bool FLAGS_force_in_front = true;
static double FLAGS_debug_matches_overlap = 1;
static double FLAGS_debug_error_scale = 0;
static bool FLAGS_lock_positions = true;
static bool FLAGS_shared_distortion = true;
static double FLAGS_perturb_positions = 0;
static double FLAGS_perturb_rotations = 0;
static double FLAGS_perturb_principals = 0;
static bool FLAGS_discard_outside_fov = false;
static bool FLAGS_save_debug_images = false;

// is the stem expected to be the camera id? i.e. the path is of the form:
//   <frame index>/ ... /<camera id>.<extension>
//   e.g. 1/cam2.bmp or 000000/isp_out/cam14.png
// or is the stem expected to be the frame index? i.e. the path is of the form:
//   .../<camera id>/<frame index>.<extension>
//   e.g. 1/cam2/000123.bmp or rgb/cam14/000123.png
const bool kStemIsCameraId = false; // stem is frame index

std::unordered_map<std::string, int> cameraIdToIndex;
std::unordered_map<std::string, int> cameraGroupToIndex;

Camera makeCamera(
	const Camera& camera,
	const Camera::Vector3& position,
	const Camera::Vector3& rotation,
	const Camera::Vector2& principal,
	const Camera::Real& focal,
	const Camera::Vector2& distortion) {
	Camera result = camera;
	result.position = position;
	result.setRotation(rotation);
	result.principal = principal;
	result.setScalarFocal(focal);
	result.distortion = distortion;

	return result;
}

struct ReprojectionFunctor {
	static ceres::CostFunction* addResidual(
		ceres::Problem& problem,
		Camera::Vector3& position,
		Camera::Vector3& rotation,
		Camera::Vector2& principal,
		Camera::Real& focal,
		Camera::Vector2& distortion,
		Camera::Vector3& world,
		const Camera& camera,
		const Camera::Vector2& pixel,
		bool robust = false) {
		auto* cost = new CostFunction(new ReprojectionFunctor(camera, pixel));
		auto* loss = robust ? new ceres::HuberLoss(1.0) : nullptr;
		problem.AddResidualBlock(
			cost,
			loss,
			position.data(),
			rotation.data(),
			principal.data(),
			&focal,
			distortion.data(),
			world.data());
		return cost;
	}

	bool operator()(
		double const* const position,
		double const* const rotation,
		double const* const principal,
		double const* const focal,
		double const* const distortion,
		double const* const world,
		double* residuals) const {
		// create a camera using parameters
		// TODO: maybe compute modified cameras once per iteration using
		//   vector<IterationCallback> Solver::Options::callbacks?
		Camera modified = makeCamera(
			camera,
			Eigen::Map<const Camera::Vector3>(position),
			Eigen::Map<const Camera::Vector3>(rotation),
			Eigen::Map<const Camera::Vector2>(principal),
			*focal,
			Eigen::Map<const Camera::Vector2>(distortion));
		// transform world with that camera and compare to pixel
		Eigen::Map<const Camera::Vector3> w(world);
		Eigen::Map<Camera::Vector2> r(residuals);
		r = modified.pixel(w) - pixel;

		return true;
	}

private:
	using CostFunction = ceres::NumericDiffCostFunction<
		ReprojectionFunctor,
		ceres::CENTRAL,
		2, // residuals
		3, // position
		3, // rotation
		2, // principal
		1, // focal
		2, // distortion
		3>; // world

	ReprojectionFunctor(const Camera& camera, const Camera::Vector2& pixel) :
		camera(camera),
		pixel(pixel) {
	}

	const Camera& camera;
	const Camera::Vector2 pixel;
};

struct TriangulationFunctor {
	static ceres::CostFunction* addResidual(
		ceres::Problem& problem,
		Camera::Vector3& world,
		const Camera& camera,
		const Camera::Vector2& pixel,
		const bool robust = false) {
		auto* cost = new CostFunction(new TriangulationFunctor(camera, pixel));
		auto* loss = robust ? new ceres::HuberLoss(1.0) : nullptr;
		problem.AddResidualBlock(
			cost,
			loss,
			world.data());
		return cost;
	}

	bool operator()(
		double const* const world,
		double* residuals) const {
		Eigen::Map<const Camera::Vector3> w(world);
		Eigen::Map<Camera::Vector2> r(residuals);

		// transform world with camera and compare to pixel
		r = camera.pixel(w) - pixel;

		return true;
	}

private:
	using CostFunction = ceres::NumericDiffCostFunction<
		TriangulationFunctor,
		ceres::CENTRAL,
		2, // residuals
		3>; // world

	TriangulationFunctor(const Camera& camera, const Camera::Vector2& pixel) :
		camera(camera),
		pixel(pixel) {
	}

	const Camera& camera;
	const Camera::Vector2 pixel;
};

using Observations = std::vector<std::pair<const Camera&, Camera::Vector2>>;

Camera::Vector3 averageAtDistance(
	const Observations& observations,
	const Camera::Real distance) {
	Camera::Vector3 sum = Camera::Vector3::Zero();
	for (const auto& obs : observations) {
		sum += obs.first.rig(obs.second).pointAt(distance);
	}
	return sum / observations.size();
}

Camera::Vector3 triangulateNonlinear(
	const Observations& observations,
	const bool forceInFront) {
	ceres::Solver::Options options;

	// initial value is average of distant points
	const Camera::Real kInitialDistance = 1000; // not hugely important
	Camera::Vector3 world = averageAtDistance(observations, kInitialDistance);
	ceres::Problem problem;
	for (const auto& obs : observations) {
		TriangulationFunctor::addResidual(problem, world, obs.first, obs.second);
	}

	ceres::Solver::Summary summary;
	ceres::Solve(options, &problem, &summary);

	if (forceInFront) {
		for (const auto& obs : observations) {
			if (obs.first.isBehind(world)) {
				return averageAtDistance(observations, Camera::kNearInfinity);
			}
		}
	}

	return world;
}

double calcPercentile(std::vector<double> values, double percentile = 0.5) {
	if (values.empty()) {
		return NAN;
	}
	CHECK_LT(percentile, 1);
	size_t index(percentile * values.size());
	std::nth_element(values.begin(), values.begin() + index, values.end());
	return values[index];
}

Camera::Vector2 reprojectionError(
	const ceres::Problem& problem,
	ceres::ResidualBlockId id) {
	auto cost = problem.GetCostFunctionForResidualBlock(id);
	std::vector<double*> parameterBlocks;
	problem.GetParameterBlocksForResidualBlock(id, &parameterBlocks);
	Camera::Vector2 residual;
	cost->Evaluate(parameterBlocks.data(), residual.data(), nullptr);
	return residual;
}

std::vector<double> getReprojectionErrorNorms(const ceres::Problem& problem) {
	std::vector<double> result;
	std::vector<ceres::ResidualBlockId> ids;
	problem.GetResidualBlocks(&ids);
	for (auto& id : ids) {
		result.push_back(reprojectionError(problem, id).norm());
	}
	return result;
}

// remove if residual error is more than threshold
void removeOutliers(ceres::Problem& problem, double threshold) {
	std::vector<ceres::ResidualBlockId> ids;
	problem.GetResidualBlocks(&ids);
	for (auto & id : ids) {
		if (reprojectionError(problem, id).norm() > threshold) {
			problem.RemoveResidualBlock(id);
		}
	}
}





void buildCameraIndexMaps(const Camera::Rig& rig) {
  for (int i = 0; i < rig.size(); ++i) {
    cameraIdToIndex[rig[i].id] = i;
    cameraGroupToIndex[rig[i].group] = i; // last camera in group wins
  }
}

std::string getCameraIdFromPath(const std::string& image) {
  if (kStemIsCameraId) {
    return image;
  }
  return image;
}


int getFrameIndexFromPath(const std::string& image) {
  if (kStemIsCameraId) {
    return std::stoi(image);
  }
  return  std::stoi(image);
}

int getCameraIndex(const std::string& image) {
	return cameraIdToIndex.at(getCameraIdFromPath(image));
}



// create a string that adheres to the format of an image path
std::string makeArtificialPath(int frame, const std::string id) {
  return std::to_string(frame) + "/" + id;
}

cv::Mat loadImage(const std::string& path) {
	return cv::imread(path);
}



template <typename V>
void perturb(V& v, Camera::Real amount) {
  for (int i = 0; i < v.size(); ++i)
    v[i] += amount * 2 * (std::rand() / double(RAND_MAX) - 0.5);
}

void perturbCameras(
    std::vector<Camera>& cameras,
    const double pos,
    const double rot,
    const double principal) {
  for (auto& camera : cameras) {
    if (&camera != &cameras[0]) {
      perturb(camera.position, pos);
      auto rotation = camera.getRotation();
      perturb(rotation, rot);
      camera.setRotation(rotation);
    }
    perturb(camera.principal, principal);
  }
}

bool Overlap::isIntraFrame() const {
	return getFrameIndexFromPath(images[0]) == getFrameIndexFromPath(images[1]);
}

Overlap& findOrAddOverlap(
    std::vector<Overlap>& overlaps,
    const std::string& i0,
    const std::string& i1) {
  for (auto& overlap : overlaps) {
    if (overlap.images[0] == i0 && overlap.images[1] == i1) {
      return overlap;
    }
  }
  overlaps.emplace_back(i0, i1);
  return overlaps.back();
}

Camera::Vector3 triangulate(const Observations& observations) {
  return triangulateNonlinear(observations, FLAGS_force_in_front);
}

// a trace is a world coordinate and a list of observations that reference it
struct Trace {
  Camera::Vector3 position;

  std::vector<std::pair<std::string, int>> references;

  void add(const std::string& image, const int index) {
    references.emplace_back(image, index);
  }

  // inherit trace's references
  void inherit(Trace& trace, KeypointMap& keypointMap, int index) {
    for (const auto& ref : trace.references) {
     keypointMap[ref.first][ref.second].index = index;
    }
    references.insert(
      references.end(),
      trace.references.begin(),
      trace.references.end());
    trace.references.clear();
  }
};

// return reprojection RMSE for each match in overlap
// returns NaN if one observation is outside the other camera's fov
std::vector<Camera::Real> reprojectionErrors(
    const Overlap& overlap,
    const KeypointMap& keypointMap,
    const std::vector<Trace>& traces,
    const std::vector<Camera>& cameras) {
  const std::reference_wrapper<const Camera> cams[2] = {
    cameras[getCameraIndex(overlap.images[0])],
    cameras[getCameraIndex(overlap.images[1])] };
  const std::reference_wrapper<const std::vector<Keypoint>> keypoints[2] = {
    keypointMap.at(overlap.images[0]),
    keypointMap.at(overlap.images[1]) };

  std::vector<Camera::Real> result;
  for (const auto& match : overlap.matches) {
    // TODO: if sees is not a good idea, this can be a much simpler loop
    bool visible = true;
    Camera::Vector2 pixels[2];
    for (int i = 0; i < 2; ++i) {
      pixels[i] = keypoints[i].get()[match[i]].position;
      if (FLAGS_discard_outside_fov) {
        if (!cams[1 - i].get().sees(cams[i].get().rigNearInfinity(pixels[i]))) {
          visible = false;
          break;
        }
      }
    }
    if (!visible) {
      result.emplace_back(NAN);
    } else {
      int trace = keypoints[0].get()[match[0]].index;
      CHECK_EQ(trace, keypoints[1].get()[match[1]].index);
      Camera::Vector3 rig = trace < 0
        ? triangulate({{ cams[0], pixels[0] }, { cams[1], pixels[1] }})
        : traces[trace].position;
      Camera::Real squaredNorm = 0;
      for (int i = 0; i < 2; ++i) {
        squaredNorm += (pixels[i] - cams[i].get().pixel(rig)).squaredNorm();
      }
      result.emplace_back(sqrt(squaredNorm / 2));
    }
  }

  return result;
}

void removeOutliers(
    std::vector<Overlap>& overlaps,
    const KeypointMap& keypointMap,
    const std::vector<Trace>& traces,
    const std::vector<Camera>& cameras,
    const Camera::Real factor) {
  int total = 0;
  int invisible = 0;
  int outliers = 0;

  for (Overlap& overlap : overlaps) {
    /*if (overlap.isIntraFrame()) */{
      auto errors = reprojectionErrors(overlap, keypointMap, traces, cameras);
      CHECK_EQ(errors.size(), overlap.matches.size());
      // compute threshold as factor x median of non-nan reprojection errors
      std::vector<Camera::Real> numbers;
      for (const auto& error : errors) {
        if (!std::isnan(error)) {
          numbers.emplace_back(error);
        }
      }
      auto threshold = factor * calcPercentile(numbers);
      // defrag the good matches to the front of overlap.matches
      int inliers = 0;
      for (int i = 0; i < errors.size(); ++i) {
        if (!std::isnan(errors[i]) && errors[i] < threshold) {
          overlap.matches[inliers] = overlap.matches[i];
          ++inliers;
        }
      }

      total += errors.size();
      invisible += errors.size() - numbers.size();
      outliers += numbers.size() - inliers;

      overlap.matches.resize(inliers);
      overlap.matches.shrink_to_fit();
    }
  }

  LOG(INFO)
    << "removeOutliers: total: " << total << " "
    << "invisible: " << invisible << " "
    << "outliers: " << outliers << std::endl;
}

void triangulateTraces(
    std::vector<Trace>& traces,
    const KeypointMap& keypointMap,
    const std::vector<Camera>& cameras) {
  for (Trace& trace : traces) {
    if (!trace.references.empty()) {
      Observations observations;
      for (const auto& ref : trace.references) {
        const auto& keypoint = keypointMap.at(ref.first)[ref.second];
        const auto& camera = cameras[getCameraIndex(ref.first)];
        observations.emplace_back(camera, keypoint.position);
      }
      trace.position = triangulate(observations);
    }
  }
}

// debugging only: ensure that referenced keypoints refer back to the trace
void validateTraces(
    const std::vector<Trace>& traces,
    const KeypointMap& keypointMap) {
  for (int i = 0; i < traces.size(); ++i) {
    for (const auto& ref : traces[i].references) {
      const auto& keypoint = keypointMap.at(ref.first)[ref.second];
      CHECK_EQ(i, keypoint.index) << "keypoint must reference trace";
    }
  }
}

// create a trace for each match, i.e. do not connect the matches up
std::vector<Trace> disconnectedTraces(
    KeypointMap& keypointMap,
    const std::vector<Overlap>& overlaps) {
  std::vector<Trace> result;
  for (const auto& overlap : overlaps) {
    for (int m = 0; m < overlap.matches.size(); ++m) {
      result.emplace_back();
      const auto& match = overlap.matches[m];
      result.back().add(overlap.images[0], match[0]);
      result.back().add(overlap.images[1], match[1]);
    }
  }
  return result;
}

std::vector<Trace> assembleTraces(
    KeypointMap& keypointMap,
    const std::vector<Overlap>& overlaps) {
  // mark all keypoints as unreferenced
  for (auto& keypoints : keypointMap) {
    for (auto& keypoint : keypoints.second) {
      keypoint.index = -1;
    }
  }
  std::vector<Trace> result;
  for (const auto& overlap : overlaps) {
    const std::reference_wrapper<std::vector<Keypoint>> keypoints[2] = {
      keypointMap[overlap.images[0]],
      keypointMap[overlap.images[1]] };
    for (int m = 0; m < overlap.matches.size(); ++m) {
      const auto& match = overlap.matches[m];
      const std::reference_wrapper<int> indexes[2] = {
        keypoints[0].get()[match[0]].index,
        keypoints[1].get()[match[1]].index };
      if (indexes[0] < 0 && indexes[1] < 0) {
        result.emplace_back();
        for (int i = 0; i < 2; ++i) {
          indexes[i].get() = result.size() - 1;
          result[indexes[i]].add(overlap.images[i], match[i]);
        }
      } else if (indexes[0] < 0) {
        indexes[0].get() = indexes[1];
        result[indexes[0]].add(overlap.images[0], match[0]);
      } else if (indexes[1] < 0) {
        indexes[1].get() = indexes[0];
        result[indexes[1]].add(overlap.images[1], match[1]);
      } else if (indexes[0] != indexes[1]) {
        auto& trace = result[indexes[1]];
        result[indexes[0]].inherit(trace, keypointMap, indexes[0]);
      }
    }
  }

  LOG(INFO) << "found " << result.size() << " traces";

  return result;
}

cv::Mat blend(const cv::Mat& mat0, const cv::Mat& mat1) {
  if (mat0.empty()) {
    return 0.5 * mat1;
  }
  cv::Mat result;
  cv::addWeighted(mat0, 0.5, mat1, 0.5, 0, result);
  return result;
}

cv::Mat projectImageBetweenCamerasNearest(
    const Camera& dst,
    const Camera& src,
    const cv::Mat& srcImage) {
  cv::Mat result(cv::Size(dst.resolution.x(), dst.resolution.y()), CV_8UC3);
  for (int y = 0; y < result.rows; ++y) {
    for (int x = 0; x < result.cols; ++x) {
      Camera::Vector2 dstPixel(x + 0.5, y + 0.5);
      auto rig = dst.rigNearInfinity(dstPixel);
      cv::Vec3b color(0, 0, 0);
      if (src.sees(rig)) {
        if (srcImage.empty()) {
          color = cv::Vec3b(255, 255, 255);
        } else {
          auto srcPixel = src.pixel(rig);
          color = srcImage.at<cv::Vec3b>(srcPixel.y(), srcPixel.x());
        }
      }
      result.at<cv::Vec3b>(y, x) = color;
    }
  }
  return result;
}

// draw a line that starts out red, ends up green
void drawRedGreenLine(
    cv::Mat& dst,
    const Camera::Vector2& r,
    const Camera::Vector2& g,
    const Camera::Vector2& m) {
  const cv::Scalar red(0, 0, 255);
  const cv::Scalar green(0, 255, 0);
  cv::line(dst, cv::Point2f(r.x(), r.y()), cv::Point2f(m.x(), m.y()), red, 2);
  cv::line(dst, cv::Point2f(g.x(), g.y()), cv::Point2f(m.x(), m.y()), green, 2);
}

cv::Mat renderOverlap(
    const Overlap& overlap,
    const KeypointMap& keypointMap,
    const std::vector<Trace>& traces,
    const std::vector<Camera>& cameras) {
  // transform image 1 into image 0's space and overlay the two
  const std::string& image0 = overlap.images[0];
  const std::string& image1 = overlap.images[1];
  const Camera& camera0 = cameras[getCameraIndex(image0)];
  const Camera& camera1 = cameras[getCameraIndex(image1)];
  const std::vector<Keypoint>& keypoints0 = keypointMap.at(image0);
  const std::vector<Keypoint>& keypoints1 = keypointMap.at(image1);
  cv::Mat result = blend(
    loadImage(image0),
    projectImageBetweenCamerasNearest(
      camera0, camera1, loadImage(image1)));
  for (const auto& match : overlap.matches) {
    Camera::Vector2 p0 = keypoints0[match[0]].position;
    Camera::Vector2 p1 = keypoints1[match[1]].position;
    int trace = keypoints0[match[0]].index;
    CHECK_EQ(trace, keypoints1[match[1]].index);
    Camera::Vector3 rig = trace < 0
      ? triangulate({{ camera0, p0 }, { camera1, p1 }})
      : traces[trace].position;
    drawRedGreenLine(
      result,
      p0, // p0 in red
      camera0.pixel(camera1.rigNearInfinity(p1)), // transformed p1 in green
      camera0.pixel(rig)); // via transformed triangulation
  }

  return result;
}

cv::Mat renderReprojections(
    const std::string& image,
    const Camera& camera,
    const std::vector<Keypoint>& keypoints,
    const std::vector<Trace>& traces,
    const Camera::Real scale) {
  cv::Mat result = 0.5f * loadImage(image);
  const cv::Scalar green(0, 255, 0);
  const cv::Scalar red(0, 0, 255);
  for (const auto& keypoint : keypoints) {
    if (keypoint.index >= 0) {
      // draw red line from image keypoint to reprojected world point
      // then continue in green in the same direction but scale x as far
      Camera::Vector2 proj = camera.pixel(traces[keypoint.index].position);
      drawRedGreenLine(
        result,
        keypoint.position,
        proj + scale * (proj - keypoint.position),
        proj);
    }
  }

  return result;
}

std::string getReprojectionReport(const ceres::Problem& problem) {
  auto norms = getReprojectionErrorNorms(problem);
  double total = 0;
  double totalSq = 0;
  for (auto norm : norms) {
    total += norm;
    totalSq += norm * norm;
  }

  std::ostringstream result;
  result
    << "reprojections " << norms.size() << " "
    << "RMSE " << sqrt(totalSq / norms.size()) << " "
    << "average " << total / norms.size() << " "
    << "median " << calcPercentile(norms, 0.5) << " "
    << "90% " << calcPercentile(norms, 0.9) << " "
    << "99% " << calcPercentile(norms, 0.99) << " ";

  result << "worst 3: ";
  std::sort(norms.begin(), norms.end());
  for (int i = norms.size() - 3; i < norms.size(); ++i) {
    result << norms[i] << " ";
  }

  return result.str();
}

double acosClamp(double x) {
  return std::acos(min(max(-1.0, x), 1.0));
}

std::string getCameraRmseReport(
    const std::vector<Camera>& cameras,
    const std::vector<Camera>& groundTruth) {
  Camera::Real position = 0;
  Camera::Real rotation = 0;
  Camera::Real principal = 0;
  Camera::Real distortion = 0;
  Camera::Real focal = 0;

  for (int i = 0; i < cameras.size(); ++i) {
    {
      auto before = groundTruth[i].position;
      auto after = cameras[i].position;
      position += (after - before).squaredNorm();
    }
    for (int v = 0; v < 3; ++v) {
      auto before = groundTruth[i].rotation.row(v);
      auto after = cameras[i].rotation.row(v);
      rotation += (after - before).squaredNorm();
    }
    {
      auto before = groundTruth[i].principal;
      auto after = cameras[i].principal;
      principal += (after - before).squaredNorm();
    }
    {
      auto before = groundTruth[i].distortion;
      auto after = cameras[i].distortion;
      distortion += (after - before).squaredNorm();
    }
    {
      auto before = groundTruth[i].focal;
      auto after = cameras[i].focal;
      focal += (after - before).squaredNorm();
    }
  }

  Camera::Real angle = 0;
  int angleCount = 0;
  for (int i = 0; i < cameras.size(); ++i) {
    for (int j = 0; j < i; ++j) {
      for (int v = 2; v < 3; ++v) {
        // angle between camera i and j
        auto before = acosClamp(
          groundTruth[i].rotation.row(v).dot(
            groundTruth[j].rotation.row(v)));
        if (before > 1) {
          continue; // only count angles less than a radian
        }
        auto after = acosClamp(
          cameras[i].rotation.row(v).dot(
            cameras[j].rotation.row(v)));
        angle += (after - before) * (after - before);
        ++angleCount;
      }
    }
  }

  // average
  position /= cameras.size();
  rotation /= 3 * cameras.size();
  principal /= cameras.size();
  distortion /= cameras.size();
  focal /= cameras.size();
  angle /= angleCount;

  std::ostringstream result;
  result << "RMSEs: "
    << "Pos " << sqrt(position) << " "
    << "Rot " << sqrt(rotation) << " "
    << "Principal " << sqrt(principal) << " "
    << "Distortion " << sqrt(distortion) << " "
    << "Focal " << sqrt(focal) << " "
    << "Angle " << sqrt(angle) << " ";

  return result.str();
}

template <typename T>
double* parameterBlock(T& t) {
  return t.data();
}

template <>
double* parameterBlock(Camera::Real& t) {
  return &t;
}

template <>
double* parameterBlock(Trace& t) {
  return t.position.data();
}

template <typename T>
void lockParameter(
    ceres::Problem& problem,
    T& param,
    const bool lock = true) {
  if (lock) {
    problem.SetParameterBlockConstant(parameterBlock(param));
  } else {
    problem.SetParameterBlockVariable(parameterBlock(param));
  }
}

template <typename T>
void lockParameters(
    ceres::Problem& problem,
    std::vector<T>& params,
    const bool lock = true) {
  for (auto& param : params) {
    lockParameter(problem, param, lock);
  }
}

void showMatches(
    const std::vector<Camera>& cameras,
    const KeypointMap& keypointMap,
    const std::vector<Overlap>& overlaps,
    const std::vector<Trace>& traces,
    const std::string& debugDir,
    const int pass) {
  // visualization for debugging
  for (const auto& overlap : overlaps) {
    const int idx0 = getCameraIndex(overlap.images[0]);
    const int idx1 = getCameraIndex(overlap.images[1]);
    if (cameras[idx0].overlap(cameras[idx1]) > FLAGS_debug_matches_overlap) {
      cv::Mat image = renderOverlap(overlap, keypointMap, traces, cameras);
      cv::resize(image, image, cv::Size(), 0.5, 0.5);

      if (FLAGS_save_debug_images) {
        std::string filename = debugDir + "/" +
          "pass" + std::to_string(pass) + "_" +
          getCameraIdFromPath(overlap.images[0]) + "-" +
          getCameraIdFromPath(overlap.images[1]) + ".png";
        imwrite(filename, image);
      } else {
        cv::imshow("overlap", image);
        cv::waitKey();
      }
    }
  }
}

void showReprojections(
    const std::vector<Camera>& cameras,
    const KeypointMap& keypointMap,
    const std::vector<Trace>& traces,
    const Camera::Real scale) {
  for (const auto& entry : keypointMap) {
    const auto& image = entry.first;
    const auto& keypoints = entry.second;
    const auto& camera = cameras[getCameraIndex(image)];
    cv::Mat render = renderReprojections(image, camera, keypoints, traces, scale);
    cv::resize(render, render, cv::Size(), 0.5, 0.5);
    cv::imshow("reprojections", render);
    cv::waitKey();
  }
}

void solve(
    ceres::Problem& problem,
    std::vector<Camera::Vector3>& positions,
    std::vector<Camera::Vector3>& rotations,
    std::vector<Trace>& traces) {
  ceres::Solver::Options options;
  options.use_inner_iterations = true;
  options.max_num_iterations = 500;
  options.minimizer_progress_to_stdout = false;
  ceres::Solver::Summary summary;

  // lock camera 0 pose
  problem.SetParameterBlockConstant(positions[0].data());
  problem.SetParameterBlockConstant(rotations[0].data());

  LOG(INFO) << getReprojectionReport(problem) << std::endl;
  Solve(options, &problem, &summary);
  LOG(INFO) << summary.BriefReport();
  LOG(INFO) << getReprojectionReport(problem) << std::endl;
}

void refine(
    std::vector<Camera>& cameras,
    KeypointMap keypointMap,
    std::vector<Overlap> overlaps,
    const int pass,
    const std::string& debugDir) {

  // remove outlier matches
  std::vector<Trace> traces = disconnectedTraces(keypointMap, overlaps);
  triangulateTraces(traces, keypointMap, cameras);
  removeOutliers(overlaps, keypointMap, traces, cameras, FLAGS_outlier_factor);

  // assemble and remove outlier traces
  traces = assembleTraces(keypointMap, overlaps);
  triangulateTraces(traces, keypointMap, cameras);
  removeOutliers(overlaps, keypointMap, traces, cameras, FLAGS_outlier_factor);

  // final triangulation
  traces = assembleTraces(keypointMap, overlaps);
  triangulateTraces(traces, keypointMap, cameras);

  // visualization for debugging
  showMatches(
    cameras,
    keypointMap,
    overlaps,
    traces,
    debugDir,
    pass);

  // read camera parameters from cameras
  std::vector<Camera::Vector3> positions;
  std::vector<Camera::Vector3> rotations;
  std::vector<Camera::Vector2> principals;
  std::vector<Camera::Real> focals;
  std::vector<Camera::Vector2> distortions;
  for (auto& camera : cameras) {
    positions.push_back(camera.position);
    rotations.push_back(camera.getRotation());
    principals.push_back(camera.principal);
    focals.push_back(camera.getScalarFocal());
    distortions.push_back(camera.distortion);
  }

  // create the problem: add a residual for each observation
  ceres::Problem problem;
  for (const Trace& trace : traces) {
    for (const auto& ref : trace.references) {
      const std::string& image = ref.first;
      const auto& keypoint = keypointMap[image][ref.second];
      const int camera = getCameraIndex(image);
      const int group = cameraGroupToIndex[cameras[camera].group];
      ReprojectionFunctor::addResidual(
        problem,
        positions[camera],
        rotations[camera],
        principals[camera],
        focals[camera],
        distortions[FLAGS_shared_distortion ? group : camera],
        traces[keypoint.index].position,
        cameras[camera],
        keypoint.position,
        FLAGS_robust);
    }
  }

  if (pass == 0) {
    // first pass: lock position, focal and distortion
    lockParameters(problem, positions);
    lockParameters(problem, rotations);
   if (FLAGS_shared_distortion) {
      for (const auto& mapping : cameraGroupToIndex) {
        lockParameter(problem, distortions[mapping.second]);
      }
    } else {
      lockParameters(problem, distortions);
    }
  } else {
    if (FLAGS_lock_positions) {
      lockParameters(problem, positions);
    }
	lockParameters(problem, focals);
	lockParameters(problem, principals);


  }

  solve(problem, positions, rotations, traces);

  // write optimized camera parameters back into cameras
  for (int i = 0; i < cameras.size(); ++i) {
    const int group = cameraGroupToIndex[cameras[i].group];
    cameras[i] = makeCamera(
      cameras[i],
      positions[i],
      rotations[i],
      principals[i],
      focals[i],
      distortions[FLAGS_shared_distortion ? group : i]);
  }

  // visualization for debugging
  if (FLAGS_debug_error_scale && pass == FLAGS_pass_count - 1) {
    showReprojections(cameras, keypointMap, traces, FLAGS_debug_error_scale);
  }
}

int calibrate(const Camera::Rig& rig,
	KeypointMap keypointMap,
	std::vector<Overlap> overlaps,
	const std::string& debugDir,
	Camera::Rig& rigCalibrated) {

	buildCameraIndexMaps(rig);
	rigCalibrated = rig;

	perturbCameras(
		rigCalibrated,
		FLAGS_perturb_positions,
		FLAGS_perturb_rotations,
		FLAGS_perturb_principals);



	LOG(INFO) << getCameraRmseReport(rigCalibrated, rig) << std::endl;
	for (int pass = 0; pass < FLAGS_pass_count; ++pass) {
		refine(rigCalibrated, keypointMap, overlaps, pass, debugDir);
		std::cout
			<< "pass " << pass << ": "
			<< getCameraRmseReport(rigCalibrated, rig) << std::endl;
	}

	return 0;
}

