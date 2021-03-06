// Copyright 2020 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cmath>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/strings/substitute.h"
#include "mediapipe/calculators/tensor/image_to_tensor_utils.h"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/calculator_runner.h"
#include "mediapipe/framework/deps/file_path.h"
#include "mediapipe/framework/formats/image_format.pb.h"
#include "mediapipe/framework/formats/image_frame.h"
#include "mediapipe/framework/formats/image_frame_opencv.h"
#include "mediapipe/framework/formats/rect.pb.h"
#include "mediapipe/framework/formats/tensor.h"
#include "mediapipe/framework/port/gtest.h"
#include "mediapipe/framework/port/integral_types.h"
#include "mediapipe/framework/port/opencv_core_inc.h"
#include "mediapipe/framework/port/opencv_imgcodecs_inc.h"
#include "mediapipe/framework/port/opencv_imgproc_inc.h"
#include "mediapipe/framework/port/parse_text_proto.h"
#include "mediapipe/framework/port/status_matchers.h"

namespace mediapipe {
namespace {

cv::Mat GetRgb(absl::string_view path) {
  cv::Mat bgr = cv::imread(file::JoinPath("./", path));
  cv::Mat rgb;
  cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
  return rgb;
}

cv::Mat GetRgba(absl::string_view path) {
  cv::Mat bgr = cv::imread(file::JoinPath("./", path));
  cv::Mat rgb;
  cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGBA);
  return rgb;
}

// Image to tensor test template.
// No processing/assertions should be done after the function is invoked.
void RunTest(cv::Mat input, cv::Mat expected_result, float range_min,
             float range_max, int tensor_width, int tensor_height,
             bool keep_aspect, const mediapipe::NormalizedRect& roi) {
  auto graph_config = mediapipe::ParseTextProtoOrDie<CalculatorGraphConfig>(
      absl::Substitute(R"(
        input_stream: "input_image"
        input_stream: "roi"
        node {
          calculator: "ImageToTensorCalculator"
          input_stream: "IMAGE:input_image"
          input_stream: "NORM_RECT:roi"
          output_stream: "TENSORS:tensor"
          options {
            [mediapipe.ImageToTensorCalculatorOptions.ext] {
            output_tensor_width: $0
            output_tensor_height: $1
            keep_aspect_ratio: $4
            output_tensor_float_range {
                min: $2
                max: $3
              }
            }
          }
        }
        )",
                       /*$0=*/tensor_width,
                       /*$1=*/tensor_height,
                       /*$2=*/range_min,
                       /*$3=*/range_max,
                       /*$4=*/keep_aspect ? "true" : "false"));

  std::vector<Packet> output_packets;
  tool::AddVectorSink("tensor", &graph_config, &output_packets);

  // Run the graph.
  CalculatorGraph graph;
  MP_ASSERT_OK(graph.Initialize(graph_config));
  MP_ASSERT_OK(graph.StartRun({}));

  ImageFrame input_image(
      input.channels() == 4 ? ImageFormat::SRGBA : ImageFormat::SRGB,
      input.cols, input.rows, input.step, input.data, [](uint8*) {});
  MP_ASSERT_OK(graph.AddPacketToInputStream(
      "input_image",
      MakePacket<ImageFrame>(std::move(input_image)).At(Timestamp(0))));
  MP_ASSERT_OK(graph.AddPacketToInputStream(
      "roi",
      MakePacket<mediapipe::NormalizedRect>(std::move(roi)).At(Timestamp(0))));

  MP_ASSERT_OK(graph.WaitUntilIdle());
  ASSERT_THAT(output_packets, testing::SizeIs(1));

  // Get and process results.
  const std::vector<Tensor>& tensor_vec =
      output_packets[0].Get<std::vector<Tensor>>();
  ASSERT_THAT(tensor_vec, testing::SizeIs(1));

  const Tensor& tensor = tensor_vec[0];
  EXPECT_EQ(tensor.element_type(), Tensor::ElementType::kFloat32);

  auto view = tensor.GetCpuReadView();
  cv::Mat tensor_mat(tensor_height, tensor_width, CV_32FC3,
                     const_cast<float*>(view.buffer<float>()));
  cv::Mat result_rgb;
  auto transformation =
      GetValueRangeTransformation(range_min, range_max, 0.0f, 255.0f)
          .ValueOrDie();
  tensor_mat.convertTo(result_rgb, CV_8UC3, transformation.scale,
                       transformation.offset);

  cv::Mat diff;
  cv::absdiff(result_rgb, expected_result, diff);
  double max_val;
  cv::minMaxLoc(diff, nullptr, &max_val);
  // Expects the maximum absolute pixel-by-pixel difference is less than 5.
  EXPECT_LE(max_val, 5);

  // Fully close graph at end, otherwise calculator+tensors are destroyed
  // after calling WaitUntilDone().
  MP_ASSERT_OK(graph.CloseInputStream("input_image"));
  MP_ASSERT_OK(graph.CloseInputStream("roi"));
  MP_ASSERT_OK(graph.WaitUntilDone());
}

TEST(ImageToTensorCalculatorTest, MediumSubRectKeepAspect) {
  mediapipe::NormalizedRect roi;
  roi.set_x_center(0.65f);
  roi.set_y_center(0.4f);
  roi.set_width(0.5f);
  roi.set_height(0.5f);
  roi.set_rotation(0);
  RunTest(
      GetRgb("/mediapipe/calculators/"
             "tensor/testdata/image_to_tensor/input.jpg"),
      GetRgb("/mediapipe/calculators/"
             "tensor/testdata/image_to_tensor/medium_sub_rect_keep_aspect.png"),
      /*range_min=*/0.0f,
      /*range_max=*/1.0f,
      /*tensor_width=*/256, /*tensor_height=*/256, /*keep_aspect=*/true, roi);
}

TEST(ImageToTensorCalculatorTest, MediumSubRectKeepAspectWithRotation) {
  mediapipe::NormalizedRect roi;
  roi.set_x_center(0.65f);
  roi.set_y_center(0.4f);
  roi.set_width(0.5f);
  roi.set_height(0.5f);
  roi.set_rotation(M_PI * 90.0f / 180.0f);
  RunTest(GetRgb("/mediapipe/calculators/"
                 "tensor/testdata/image_to_tensor/input.jpg"),
          GetRgb("/mediapipe/calculators/"
                 "tensor/testdata/image_to_tensor/"
                 "medium_sub_rect_keep_aspect_with_rotation.png"),
          /*range_min=*/0.0f, /*range_max=*/1.0f,
          /*tensor_width=*/256, /*tensor_height=*/256, /*keep_aspect=*/true,
          roi);
}

TEST(ImageToTensorCalculatorTest, MediumSubRectWithRotation) {
  mediapipe::NormalizedRect roi;
  roi.set_x_center(0.65f);
  roi.set_y_center(0.4f);
  roi.set_width(0.5f);
  roi.set_height(0.5f);
  roi.set_rotation(M_PI * -45.0f / 180.0f);
  RunTest(
      GetRgb("/mediapipe/calculators/"
             "tensor/testdata/image_to_tensor/input.jpg"),
      GetRgb(
          "/mediapipe/calculators/"
          "tensor/testdata/image_to_tensor/medium_sub_rect_with_rotation.png"),
      /*range_min=*/-1.0f,
      /*range_max=*/1.0f,
      /*tensor_width=*/256, /*tensor_height=*/256, /*keep_aspect=*/false, roi);
}

TEST(ImageToTensorCalculatorTest, LargeSubRect) {
  mediapipe::NormalizedRect roi;
  roi.set_x_center(0.5f);
  roi.set_y_center(0.5f);
  roi.set_width(1.5f);
  roi.set_height(1.1f);
  roi.set_rotation(0);
  RunTest(GetRgb("/mediapipe/calculators/"
                 "tensor/testdata/image_to_tensor/input.jpg"),
          GetRgb("/mediapipe/calculators/"
                 "tensor/testdata/image_to_tensor/large_sub_rect.png"),
          /*range_min=*/0.0f,
          /*range_max=*/1.0f,
          /*tensor_width=*/128, /*tensor_height=*/128, /*keep_aspect=*/false,
          roi);
}

TEST(ImageToTensorCalculatorTest, LargeSubRectKeepAspect) {
  mediapipe::NormalizedRect roi;
  roi.set_x_center(0.5f);
  roi.set_y_center(0.5f);
  roi.set_width(1.5f);
  roi.set_height(1.1f);
  roi.set_rotation(0);
  RunTest(
      GetRgb("/mediapipe/calculators/"
             "tensor/testdata/image_to_tensor/input.jpg"),
      GetRgb("/mediapipe/calculators/"
             "tensor/testdata/image_to_tensor/large_sub_rect_keep_aspect.png"),
      /*range_min=*/0.0f,
      /*range_max=*/1.0f,
      /*tensor_width=*/128, /*tensor_height=*/128, /*keep_aspect=*/true, roi);
}

TEST(ImageToTensorCalculatorTest, LargeSubRectKeepAspectWithRotation) {
  mediapipe::NormalizedRect roi;
  roi.set_x_center(0.5f);
  roi.set_y_center(0.5f);
  roi.set_width(1.5f);
  roi.set_height(1.1f);
  roi.set_rotation(M_PI * -15.0f / 180.0f);
  RunTest(GetRgba("/mediapipe/calculators/"
                  "tensor/testdata/image_to_tensor/input.jpg"),
          GetRgb("/mediapipe/calculators/"
                 "tensor/testdata/image_to_tensor/"
                 "large_sub_rect_keep_aspect_with_rotation.png"),
          /*range_min=*/0.0f,
          /*range_max=*/1.0f,
          /*tensor_width=*/128, /*tensor_height=*/128, /*keep_aspect=*/true,
          roi);
}

TEST(ImageToTensorCalculatorTest, NoOpExceptRange) {
  mediapipe::NormalizedRect roi;
  roi.set_x_center(0.5f);
  roi.set_y_center(0.5f);
  roi.set_width(1.0f);
  roi.set_height(1.0f);
  roi.set_rotation(0);
  RunTest(GetRgba("/mediapipe/calculators/"
                  "tensor/testdata/image_to_tensor/input.jpg"),
          GetRgb("/mediapipe/calculators/"
                 "tensor/testdata/image_to_tensor/noop_except_range.png"),
          /*range_min=*/0.0f,
          /*range_max=*/1.0f,
          /*tensor_width=*/64, /*tensor_height=*/128, /*keep_aspect=*/true,
          roi);
}

}  // namespace
}  // namespace mediapipe
