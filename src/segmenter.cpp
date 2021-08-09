#include <ros/ros.h>

#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>

#include <fstream>
#include <memory>
#include <numeric>

using nvinfer1::ILogger;
using Severity = nvinfer1::ILogger::Severity;
using TrtRuntime = nvinfer1::IRuntime;
using TrtEngine = nvinfer1::ICudaEngine;
using TrtContext = nvinfer1::IExecutionContext;
using TrtNetworkDef = nvinfer1::INetworkDefinition;
using nvinfer1::NetworkDefinitionCreationFlag;

class Logger : public ILogger {
 public:
  explicit Logger(Severity severity = Severity::kINFO) : min_severity_(severity) {}

  void log(Severity severity, const char *msg) noexcept override {
    if (severity < min_severity_) {
      return;
    }

    switch (severity) {
      case Severity::kINTERNAL_ERROR:
        ROS_FATAL_STREAM(msg);
        break;
      case Severity::kERROR:
        ROS_ERROR_STREAM(msg);
        break;
      case Severity::kWARNING:
        ROS_WARN_STREAM(msg);
        break;
      case Severity::kINFO:
        ROS_INFO_STREAM(msg);
        break;
      case Severity::kVERBOSE:
      default:
        // ROS_INFO_STREAM(msg);
        ROS_DEBUG_STREAM(msg);
        break;
    }
  }

 private:
  Severity min_severity_;
};

std::ostream &operator<<(std::ostream &out, const nvinfer1::Dims &dims) {
  out << "[";
  for (int32_t i = 0; i < dims.nbDims - 1; ++i) {
    out << dims.d[i] << " x ";
  }
  out << dims.d[dims.nbDims - 1] << "]";
  return out;
}

template <typename T>
struct CudaMemoryHolder {
  explicit CudaMemoryHolder(const nvinfer1::Dims &dims) : dims(dims) {
    size =
        std::accumulate(dims.d, dims.d + dims.nbDims, 1, std::multiplies<int64_t>()) *
        sizeof(T);

    void *raw_memory = nullptr;
    auto error = cudaMalloc(&raw_memory, size);
    ROS_INFO_STREAM("allocated " << size << " bytes of memory for " << dims);
    if (error != cudaSuccess) {
      ROS_WARN_STREAM("Failed to malloc memory: " << cudaGetErrorString(error));
      return;
    }

    memory.reset(reinterpret_cast<T *>(raw_memory));
  }

  struct Deleter {
    void operator()(T *object) {
      if (object != nullptr) {
        cudaFree(object);
      }
    }
  };

  std::unique_ptr<T, Deleter> memory;
  size_t size;
  nvinfer1::Dims dims;
};

size_t getFileSize(std::istream &to_check) {
  to_check.seekg(0, std::istream::end);
  size_t size = to_check.tellg();
  to_check.seekg(0, std::ifstream::beg);
  return size;
}

cv::Mat makeDebugImg(const cv::Mat &classes,
                     double saturation = 0.85,
                     double luminance = 0.75) {
  double max_class = 0.0;
  double min_class = 0.0;
  cv::minMaxLoc(classes, &min_class, &max_class);
  ROS_INFO_STREAM("Min class: " << min_class << " Max class: " << max_class);

  if (max_class - min_class == 0.0) {
    ROS_WARN_STREAM("Min and max class are the same: " << max_class);
    return cv::Mat::zeros(classes.rows, classes.cols, CV_8UC3);
  }
  const double class_diff = max_class - min_class;

  cv::Mat new_image_hls(classes.rows, classes.cols, CV_32FC3);
  for (int r = 0; r < classes.rows; ++r) {
    for (int c = 0; c < classes.cols; ++c) {
      float *pixel = new_image_hls.ptr<float>(r, c);
      double ratio =
          static_cast<double>(classes.at<int32_t>(r, c) - min_class) / class_diff;
      pixel[0] = ratio * 360.0;
      pixel[1] = luminance;
      pixel[2] = saturation;
    }
  }

  cv::Mat new_image;
  cv::cvtColor(new_image_hls, new_image, cv::COLOR_HLS2BGR);
  cv::imshow("new image", new_image);
  cv::waitKey(0);
  return new_image;
}

void showStatistics(const std::vector<int32_t> &data) {
  std::map<int32_t, size_t> counts;
  std::vector<int32_t> unique_classes;
  for (const auto &class_id : data) {
    if (!counts.count(class_id)) {
      counts[class_id] = 0;
      unique_classes.push_back(class_id);
    }

    counts[class_id]++;
  }

  std::sort(unique_classes.begin(),
            unique_classes.end(),
            [&](const int32_t &lhs, const int32_t &rhs) {
              return counts[lhs] > counts[rhs];
            });

  double total = static_cast<double>(data.size());
  std::stringstream ss;
  ss << " Class pixel percentages:" << std::endl;
  for (const int32_t id : unique_classes) {
    ss << "  - " << id << ": " << static_cast<double>(counts[id]) / total * 100.0 << "%"
       << std::endl;
  }

  ROS_INFO_STREAM(ss.str());
}

void showStatistics(const cv::Mat &classes) {
  std::map<int32_t, size_t> counts;
  std::vector<int32_t> unique_classes;
  for (int r = 0; r < classes.rows; ++r) {
    for (int c = 0; c < classes.cols; ++c) {
      int32_t class_id = classes.at<int32_t>(r, c);
      if (!counts.count(class_id)) {
        counts[class_id] = 0;
        unique_classes.push_back(class_id);
      }

      counts[class_id]++;
    }
  }

  double total = static_cast<double>(classes.rows * classes.cols);
  std::sort(unique_classes.begin(),
            unique_classes.end(),
            [&](const int32_t &lhs, const int32_t &rhs) {
              return counts[lhs] > counts[rhs];
            });

  std::stringstream ss;
  ss << " Class pixel percentages:" << std::endl;
  for (const int32_t id : unique_classes) {
    ss << "  - " << id << ": " << static_cast<double>(counts[id]) / total * 100.0 << "%"
       << std::endl;
  }

  ROS_INFO_STREAM(ss.str());
}

std::unique_ptr<TrtEngine> deserializeEngine(TrtRuntime &runtime,
                                             const std::string &engine_path) {
  std::ifstream engine_file(engine_path, std::ios::binary);
  if (engine_file.fail()) {
    ROS_INFO_STREAM("Engine file: " << engine_path << " not found!");
    return nullptr;
  }

  const size_t engine_size = getFileSize(engine_file);
  std::vector<char> engine_data(engine_size);
  engine_file.read(engine_data.data(), engine_size);

  std::unique_ptr<TrtEngine> engine(
      runtime.deserializeCudaEngine(engine_data.data(), engine_size));
  if (!engine) {
    ROS_FATAL_STREAM("Engine creation failed");
    return nullptr;
  }

  return engine;
}

std::unique_ptr<TrtEngine> buildEngineFromOnnx(TrtRuntime &runtime,
                                               Logger &logger,
                                               const std::string &model_path,
                                               const std::string &engine_path) {
  std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(logger));
  const auto network_flags =
      1u << static_cast<uint32_t>(NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
  std::unique_ptr<TrtNetworkDef> network(builder->createNetworkV2(network_flags));
  builder->setMaxBatchSize(1);

  std::unique_ptr<nvonnxparser::IParser> parser(
      nvonnxparser::createParser(*network, logger));
  parser->parseFromFile(model_path.c_str(), static_cast<int>(ILogger::Severity::kINFO));
  for (int i = 0; i < parser->getNbErrors(); ++i) {
    ROS_ERROR_STREAM("Parser Error #" << i << ": " << parser->getError(i)->desc());
  }

  std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
  // TODO(nathan) check this (32 MB right now)
  config->setMaxWorkspaceSize(32 << 20);

  std::unique_ptr<nvinfer1::IHostMemory> memory(
      builder->buildSerializedNetwork(*network, *config));
  if (!memory) {
    ROS_FATAL_STREAM("Failed to build network");
    return nullptr;
  }

  std::ofstream fout(engine_path, std::ios::binary);
  fout.write(reinterpret_cast<char *>(memory->data()), memory->size());

  std::unique_ptr<TrtEngine> engine(
      runtime.deserializeCudaEngine(memory->data(), memory->size()));
  if (!engine) {
    ROS_FATAL_STREAM("Engine creation failed");
    return nullptr;
  }
  return engine;
}

int main(int argc, char *argv[]) {
  ros::init(argc, argv, "test_node");

  ros::NodeHandle nh("~");

  std::string model_path;
  nh.getParam("model_path", model_path);

  std::string engine_path;
  nh.getParam("engine_path", engine_path);

  int width;
  nh.getParam("image_width", width);

  int height;
  nh.getParam("image_height", height);

  std::string input_file = "input.png";
  nh.getParam("input_file", input_file);

  std::string output_file = "output.png";
  nh.getParam("output_file", output_file);

  cv::Mat file_img = cv::imread(input_file);
  if (file_img.empty()) {
    ROS_FATAL_STREAM("Image not found: " << input_file);
    return 1;
  }
  cv::Mat img(file_img.rows, file_img.cols, CV_8UC3);
  ROS_INFO_STREAM("Image: " << file_img.rows << " x " << file_img.cols << " x "
                            << file_img.channels());
  if (file_img.channels() == 3) {
    cv::cvtColor(file_img, img, cv::COLOR_BGR2RGB);
  } else {
    cv::cvtColor(file_img, img, cv::COLOR_BGRA2RGB);
  }
  cv::Mat infer_img;
  cv::resize(img, infer_img, cv::Size(width, height));

  cv::Mat img_float;
  infer_img.convertTo(img_float, CV_32FC3);
  ROS_INFO_STREAM("Image: " << img_float.rows << " x " << img_float.cols << " x "
                            << img_float.channels());

  ROS_INFO("Finished conversion");

  std::vector<float> mean{0.485f, 0.456f, 0.406f};
  std::vector<float> stddev{0.229f, 0.224f, 0.225f};

  for (int r = 0; r < img_float.rows; ++r) {
    for (int c = 0; c < img_float.cols; ++c) {
      float *pixel = img_float.ptr<float>(r, c);
      for (int channel = 0; channel < 3; ++channel) {
        pixel[channel] =
            ((pixel[channel] / 255.0) - mean.at(channel)) / stddev.at(channel);
      }
    }
  }

  ROS_INFO("Loading model");
  Logger logger(Severity::kINFO);
  std::unique_ptr<TrtRuntime> trt_runtime(nvinfer1::createInferRuntime(logger));
  std::unique_ptr<TrtEngine> engine = deserializeEngine(*trt_runtime, engine_path);
  if (!engine) {
    ROS_WARN("TRT engine not found! Rebuilding.");
    engine = buildEngineFromOnnx(*trt_runtime, logger, model_path, engine_path);
  }

  if (!engine) {
    ROS_FATAL_STREAM("Something went horribly wrong!");
    return 1;
  }

  std::unique_ptr<TrtContext> context(engine->createExecutionContext());
  if (!context) {
    ROS_FATAL_STREAM("Failed to create execution context");
    return 1;
  }

  int32_t n_bindings = engine->getNbBindings();
  for (int32_t i = 0; i < n_bindings; ++i) {
    ROS_INFO_STREAM("Binding " << i << ": " << engine->getBindingName(i));
  }

  auto input_idx = engine->getBindingIndex("input.1");
  if (input_idx == -1) {
    ROS_FATAL_STREAM("Failed to get input index");
    return 1;
  }

  nvinfer1::Dims4 input_dims{1, 3, height, width};
  context->setBindingDimensions(input_idx, input_dims);

  auto output_idx = engine->getBindingIndex("4464");
  if (output_idx == -1) {
    ROS_FATAL_STREAM("Failed to get input index");
    return 1;
  }

  auto output_dims = context->getBindingDimensions(output_idx);

  if (engine->getBindingDataType(input_idx) != nvinfer1::DataType::kFLOAT) {
    ROS_FATAL_STREAM("Input type doesn't match expected: "
                     << static_cast<int32_t>(engine->getBindingDataType(input_idx))
                     << " != " << static_cast<int32_t>(nvinfer1::DataType::kFLOAT));
  }

  if (engine->getBindingDataType(output_idx) != nvinfer1::DataType::kINT32) {
    ROS_FATAL_STREAM("Output type doesn't match expected: "
                     << static_cast<int32_t>(engine->getBindingDataType(output_idx))
                     << " != " << static_cast<int32_t>(nvinfer1::DataType::kINT32));
  }

  CudaMemoryHolder<float> input(input_dims);
  CudaMemoryHolder<int32_t> output(output_dims);

  ROS_INFO("Starting Inference");

  cudaStream_t stream;
  if (cudaStreamCreate(&stream) != cudaSuccess) {
    ROS_FATAL_STREAM("Creating cuda stream failed!");
    return 1;
  }

  if (cudaMemcpyAsync(input.memory.get(),
                      img_float.data,
                      input.size,
                      cudaMemcpyHostToDevice,
                      stream) != cudaSuccess) {
    ROS_FATAL_STREAM("Failed to copy image to gpu!");
    return 1;
  }

  void *bindings[] = {input.memory.get(), output.memory.get()};
  bool status = context->enqueueV2(bindings, stream, nullptr);
  if (!status) {
    ROS_FATAL_STREAM("Inference failed!");
    return 1;
  }

  cudaStreamSynchronize(stream);
  auto error = cudaGetLastError();
  if (error != cudaSuccess) {
    ROS_FATAL_STREAM("inference failed! " << cudaGetErrorString(error));
    return 1;
  }

  cv::Mat classes(img_float.rows, img_float.cols, CV_32S);
  std::vector<int32_t> data(img_float.rows * img_float.cols);
  error = cudaMemcpyAsync(data.data(),
                          output.memory.get(),
                          data.size() * sizeof(int32_t),
                          // classes.step[0] * classes.rows,
                          cudaMemcpyDeviceToHost,
                          stream);
  if (error != cudaSuccess) {
    ROS_FATAL_STREAM("Copying output failed: " << cudaGetErrorString(error));
    return 1;
  }

  cudaStreamSynchronize(stream);

  ROS_INFO("Finished inference");

  // cv::Mat color_classes = makeDebugImg(classes);
  // cv::imwrite(output_file, color_classes);
  showStatistics(data);

  return 0;
}
