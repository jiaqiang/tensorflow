/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifdef INTEL_MKL
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/grappler/devices.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/optimizers/remapper.h"
#include "tensorflow/core/grappler/utils/grappler_test.h"
#include "tensorflow/core/platform/test.h"

#define REGISTER_TEST_FLOAT32(TEST) REGISTER_TEST(TEST, DT_FLOAT, Float32Input);

#ifdef ENABLE_INTEL_MKL_BFLOAT16
#define REGISTER_TEST_BFLOAT16(TEST) \
  REGISTER_TEST(TEST, DT_BFLOAT16, BFloat16Input);

#define REGISTER_TEST_ALL_TYPES(TEST) \
  REGISTER_TEST_FLOAT32(TEST);        \
  REGISTER_TEST_BFLOAT16(TEST);
#else
#define REGISTER_TEST_ALL_TYPES(TEST) REGISTER_TEST_FLOAT32(TEST);
#endif  // ENABLE_INTEL_MKL_BFLOAT16

namespace tensorflow {
namespace grappler {

class MklRemapperTest : public GrapplerTest {
 public:
  const string kAddNOp = "AddN";
  const string kAddOp = "Add";
  const string kAddV2Op = "AddV2";

 protected:
  void FuseConv2DWithBiasAndAddNOrAdd(const string& data_format, bool has_relu,
                                      string add_op, bool add_with_bcast) {
    using ::tensorflow::ops::Placeholder;

    tensorflow::Scope s = tensorflow::Scope::NewRootScope();

    auto input_shape = (data_format == "NHWC")
                           ? ops::Placeholder::Shape({8, 32, 32, 3})
                           : ops::Placeholder::Shape({8, 3, 32, 32});
    auto input_shape_addn = ops::Placeholder::Shape({});
    if (data_format == "NHWC") {
      if (add_with_bcast)
        input_shape_addn = ops::Placeholder::Shape({128});
      else
        input_shape_addn = ops::Placeholder::Shape({8, 32, 32, 128});
    } else {
      if (add_with_bcast)
        input_shape_addn = ops::Placeholder::Shape({32});
      else
        input_shape_addn = ops::Placeholder::Shape({8, 128, 32, 32});
    }
    auto filter_shape = ops::Placeholder::Shape({1, 1, 3, 128});
    auto bias_shape = ops::Placeholder::Shape({128});

    auto input = Placeholder(s.WithOpName("input"), DT_FLOAT, input_shape);
    auto input_addn =
        Placeholder(s.WithOpName("input_addn"), DT_FLOAT, input_shape_addn);
    auto filter = Placeholder(s.WithOpName("filter"), DT_FLOAT, filter_shape);
    auto bias = Placeholder(s.WithOpName("bias"), DT_FLOAT, bias_shape);

    std::vector<int> strides = {1, 1, 1, 1};
    auto conv =
        ops::Conv2D(s.WithOpName("conv"), input, filter, strides, "SAME",
                    ops::Conv2D::Attrs().DataFormat(data_format));
    auto bias_add = ops::BiasAdd(s.WithOpName("bias_add"), conv, bias,
                                 ops::BiasAdd::Attrs().DataFormat(data_format));
    if (add_op == kAddNOp) {
      auto addn = ops::AddN(s.WithOpName(add_op),
                            std::initializer_list<Input>{input_addn, bias_add});
      if (has_relu) {
        auto relu = ops::Relu(s.WithOpName("relu"), addn);
        ops::Identity(s.WithOpName("fetch"), relu);
      } else {
        ops::Identity(s.WithOpName("fetch"), addn);
      }
    } else if (add_op == kAddV2Op) {
      auto add = ops::AddV2(s.WithOpName(add_op), input_addn, bias_add);
      if (has_relu) {
        auto relu = ops::Relu(s.WithOpName("relu"), add);
        ops::Identity(s.WithOpName("fetch"), relu);
      } else {
        ops::Identity(s.WithOpName("fetch"), add);
      }
    } else {
      auto add = ops::Add(s.WithOpName(add_op), input_addn, bias_add);
      if (has_relu) {
        auto relu = ops::Relu(s.WithOpName("relu"), add);
        ops::Identity(s.WithOpName("fetch"), relu);
      } else {
        ops::Identity(s.WithOpName("fetch"), add);
      }
    }
    auto input_tensor = GenerateRandomTensor<DT_FLOAT>(
        TensorShape(input_shape.shape_.dim_sizes()));
    auto input_addn_tensor = GenerateRandomTensor<DT_FLOAT>(
        TensorShape(input_shape_addn.shape_.dim_sizes()));
    auto filter_tensor = GenerateRandomTensor<DT_FLOAT>(
        TensorShape(filter_shape.shape_.dim_sizes()));
    auto bias_tensor = GenerateRandomTensor<DT_FLOAT>(
        TensorShape(bias_shape.shape_.dim_sizes()));

    GrapplerItem item;
    item.fetch = {"fetch"};
    item.feed = {{"input", input_tensor},
                 {"filter", filter_tensor},
                 {"bias", bias_tensor},
                 {"input_addn", input_addn_tensor}};
    TF_CHECK_OK(s.ToGraphDef(&item.graph));

    // Place all nodes on CPU.
    for (int i = 0; i < item.graph.node_size(); ++i) {
      item.graph.mutable_node(i)->set_device("/device:CPU:0");
    }

    // Set Rewriter config to AGGRESSIVE so that we can use Placeholder shape
    // to test that Add with both inputs having same shape get fused with
    // Conv2D. Setting this config to AGGRESSIVE is not required for the feature
    // though.
    Remapper optimizer(RewriterConfig::AGGRESSIVE);
    GraphDef output;
    TF_CHECK_OK(optimizer.Optimize(nullptr, item, &output));

    bool check_fusion = !add_with_bcast;
    int found = 0;
    for (const NodeDef& node : output.node()) {
      auto fetch_node_name = has_relu ? "relu" : add_op;
      if (node.name() == fetch_node_name) {
        if (check_fusion) {
          EXPECT_EQ("_FusedConv2D", node.op());
          EXPECT_EQ("input", node.input(0));
          EXPECT_EQ("filter", node.input(1));

          EXPECT_EQ(2, node.attr().at("num_args").i());
          EXPECT_EQ("bias", node.input(2));
          EXPECT_EQ("input_addn", node.input(3));

          const auto fused_ops = node.attr().at("fused_ops").list().s();
          if (has_relu) {
            EXPECT_EQ(3, fused_ops.size());
            EXPECT_EQ("BiasAdd", fused_ops[0]);
            EXPECT_EQ("Add", fused_ops[1]);
            EXPECT_EQ("Relu", fused_ops[2]);
          } else {
            EXPECT_EQ(2, fused_ops.size());
            EXPECT_EQ("BiasAdd", fused_ops[0]);
            EXPECT_EQ("Add", fused_ops[1]);
          }
        } else {
          if (has_relu) {
            EXPECT_EQ(node.op(), "Relu");
            ASSERT_EQ(node.input_size(), 1);
            EXPECT_EQ(node.input(0), add_op);
          } else {
            EXPECT_EQ(node.op(), add_op);
            ASSERT_EQ(node.input_size(), 2);
          }
        }
        found++;
      }
    }
    EXPECT_EQ(1, found);

    auto tensors_expected = EvaluateNodes(item.graph, item.fetch, item.feed);
    auto tensors = EvaluateNodes(output, item.fetch, item.feed);
    EXPECT_EQ(1, tensors_expected.size());
    EXPECT_EQ(1, tensors.size());
    test::ExpectTensorNear<float>(tensors_expected[0], tensors[0], 1e-6);
  }
};

#define CREATE_CONV2DFUSION_TEST(data_format, addop, relu, bcast)                    \
  TEST_F(                                                                            \
      MklRemapperTest,                                                               \
      FuseConv2DWithBiasAnd##addop##_##data_format##_relu##relu##_addbcast##bcast) { \
    const bool kShouldFuseRelu = relu;                                               \
    const bool kIsAddWithBcast = bcast;                                              \
    FuseConv2DWithBiasAndAddNOrAdd(#data_format, relu, #addop, bcast);               \
  }

#define CREATE_CONV2DFUSION_ADD_NOBCAST_TEST(addop)    \
  CREATE_CONV2DFUSION_TEST(NHWC, addop, false, false); \
  CREATE_CONV2DFUSION_TEST(NHWC, addop, true, false);  \
  CREATE_CONV2DFUSION_TEST(NCHW, addop, false, false); \
  CREATE_CONV2DFUSION_TEST(NCHW, addop, true, false);

CREATE_CONV2DFUSION_ADD_NOBCAST_TEST(AddN);

#define CREATE_CONV2DFUSION_ADD_BCAST_TEST(addop)      \
  CREATE_CONV2DFUSION_TEST(NHWC, addop, false, false); \
  CREATE_CONV2DFUSION_TEST(NHWC, addop, true, false);  \
  CREATE_CONV2DFUSION_TEST(NCHW, addop, false, false); \
  CREATE_CONV2DFUSION_TEST(NCHW, addop, true, false);  \
  CREATE_CONV2DFUSION_TEST(NHWC, addop, false, true);  \
  CREATE_CONV2DFUSION_TEST(NHWC, addop, true, true);   \
  CREATE_CONV2DFUSION_TEST(NCHW, addop, false, true);  \
  CREATE_CONV2DFUSION_TEST(NCHW, addop, true, true);

CREATE_CONV2DFUSION_ADD_BCAST_TEST(Add);
CREATE_CONV2DFUSION_ADD_BCAST_TEST(AddV2);

#undef CREATE_CONV2DFUSION_ADD_NOBCAST_TEST
#undef CREATE_CONV2DFUSION_ADD_BCAST_TEST
#undef CREATE_CONV2DFUSION_TEST

#define REGISTER_TEST(NAME, T, INPUT)                                         \
  TEST_F(MklRemapperTest, NAME##_##T) {                                       \
    using ::tensorflow::ops::Placeholder;                                     \
                                                                              \
    for (const string& activation : {"Relu", "Relu6", "Elu", "None"}) {       \
      tensorflow::Scope s = tensorflow::Scope::NewRootScope();                \
                                                                              \
      auto input_shape = Placeholder::Shape({8, 32, 32, 3});                  \
      auto filter_shape = Placeholder::Shape({1, 1, 3, 1});                   \
      auto bias_shape = Placeholder::Shape({3});                              \
                                                                              \
      auto input = Placeholder(s.WithOpName("input"), DT_FLOAT, input_shape); \
      auto filter =                                                           \
          Placeholder(s.WithOpName("filter"), DT_FLOAT, filter_shape);        \
      auto bias = Placeholder(s.WithOpName("bias"), DT_FLOAT, bias_shape);    \
                                                                              \
      std::vector<int> strides = {1, 1, 1, 1};                                \
      auto conv = ops::DepthwiseConv2dNative(s.WithOpName("depthwise_conv"),  \
                                             input, filter, strides, "SAME"); \
      auto bias_add = ops::BiasAdd(s.WithOpName("bias_add"), conv, bias);     \
                                                                              \
      ops::Identity fetch = [&]() -> ops::Identity {                          \
        auto activate = s.WithOpName("activation");                           \
        auto fetch = s.WithOpName("fetch");                                   \
                                                                              \
        if (activation == "Relu") {                                           \
          return ops::Identity(fetch, ops::Relu(activate, bias_add));         \
        } else if (activation == "Relu6") {                                   \
          return ops::Identity(fetch, ops::Relu6(activate, bias_add));        \
        } else if (activation == "Elu") {                                     \
          return ops::Identity(fetch, ops::Elu(activate, bias_add));          \
        }                                                                     \
                                                                              \
        DCHECK(activation == "None");                                         \
        return ops::Identity(fetch, bias_add);                                \
      }();                                                                    \
                                                                              \
      auto input_t = GenerateRandomTensor<DT_FLOAT>({8, 32, 32, 3});          \
      auto filter_t = GenerateRandomTensor<DT_FLOAT>({1, 1, 3, 1});           \
      auto bias_t = GenerateRandomTensor<DT_FLOAT>({3});                      \
                                                                              \
      GrapplerItem item;                                                      \
      item.fetch = {"fetch"};                                                 \
      item.feed = {                                                           \
          {"input", input_t}, {"filter", filter_t}, {"bias", bias_t}};        \
      TF_CHECK_OK(s.ToGraphDef(&item.graph));                                 \
                                                                              \
      for (int i = 0; i < item.graph.node_size(); ++i) {                      \
        item.graph.mutable_node(i)->set_device("/device:CPU:0");              \
      }                                                                       \
                                                                              \
      Remapper optimizer(RewriterConfig::ON);                                 \
      GraphDef output;                                                        \
      TF_CHECK_OK(optimizer.Optimize(nullptr, item, &output));                \
                                                                              \
      int found = 0;                                                          \
      for (const NodeDef& node : output.node()) {                             \
        if (node.name() != "bias_add" && node.name() != "activation")         \
          continue;                                                           \
                                                                              \
        EXPECT_EQ(node.op(), "_FusedDepthwiseConv2dNative");                  \
        ASSERT_EQ(node.input_size(), 3);                                      \
        EXPECT_EQ(node.input(0), "input");                                    \
        EXPECT_EQ(node.input(1), "filter");                                   \
                                                                              \
        EXPECT_EQ(node.attr().at("num_args").i(), 1);                         \
        EXPECT_EQ(node.input(2), "bias");                                     \
                                                                              \
        const auto fused_ops = node.attr().at("fused_ops").list().s();        \
        if (node.name() == "bias_add") {                                      \
          ASSERT_EQ(fused_ops.size(), 1);                                     \
          EXPECT_EQ(fused_ops[0], "BiasAdd");                                 \
          found++;                                                            \
        }                                                                     \
        if (node.name() == "activation") {                                    \
          ASSERT_EQ(fused_ops.size(), 2);                                     \
          EXPECT_EQ(fused_ops[0], "BiasAdd");                                 \
          EXPECT_EQ(fused_ops[1], activation);                                \
          found++;                                                            \
        }                                                                     \
      }                                                                       \
      EXPECT_EQ(found, 1);                                                    \
                                                                              \
      auto tensors_expected =                                                 \
          EvaluateNodes(item.graph, item.fetch, item.feed);                   \
      ASSERT_EQ(tensors_expected.size(), 1);                                  \
      auto tensors = EvaluateNodes(output, item.fetch, item.feed);            \
      ASSERT_EQ(tensors.size(), 1);                                           \
      test::ExpectTensorNear<float>(tensors[0], tensors_expected[0], 1e-6);   \
    }                                                                         \
  }
REGISTER_TEST_ALL_TYPES(FuseDepthwiseConv2DWithBiasAndActivation);
#undef REGISTER_TEST

class MklFuseMatMulWithBiasAddGrad : public MklRemapperTest {
 public:
  void VerifyFused(bool ta, bool tb) {
    using ::tensorflow::ops::Placeholder;
    int m = 2;
    int k = 3;
    int n = 4;

    tensorflow::Scope s = tensorflow::Scope::NewRootScope();

    auto input_shape = ops::Placeholder::Shape({m, k});
    if (ta) input_shape = ops::Placeholder::Shape({k, m});
    auto weight_shape = ops::Placeholder::Shape({k, n});
    if (tb) weight_shape = ops::Placeholder::Shape({n, k});

    auto input = Placeholder(s.WithOpName("input"), DT_FLOAT, input_shape);
    auto weight = Placeholder(s.WithOpName("weight"), DT_FLOAT, weight_shape);

    auto matmul =
        ops::MatMul(s.WithOpName("matmul"), input, weight,
                    ops::MatMul::Attrs().TransposeA(ta).TransposeB(tb));
    auto bias_add = ops::BiasAddGrad(s.WithOpName("bias_add_grad"), matmul);
    Output matmul_grad_input;
    Output matmul_grad_filter;
    if (!ta && !tb) {
      matmul_grad_input =
          ops::MatMul(s.WithOpName("matmul_grad_input"), matmul, weight,
                      ops::MatMul::Attrs().TransposeA(false).TransposeB(true));
      matmul_grad_filter =
          ops::MatMul(s.WithOpName("matmul_grad_filter"), input, matmul,
                      ops::MatMul::Attrs().TransposeA(true).TransposeB(false));
    } else if (!ta && tb) {
      matmul_grad_input =
          ops::MatMul(s.WithOpName("matmul_grad_input"), matmul, weight,
                      ops::MatMul::Attrs().TransposeA(false).TransposeB(false));
      matmul_grad_filter =
          ops::MatMul(s.WithOpName("matmul_grad_filter"), matmul, input,
                      ops::MatMul::Attrs().TransposeA(true).TransposeB(false));
    } else if (ta && !tb) {
      matmul_grad_input =
          ops::MatMul(s.WithOpName("matmul_grad_input"), weight, matmul,
                      ops::MatMul::Attrs().TransposeA(false).TransposeB(true));
      matmul_grad_filter =
          ops::MatMul(s.WithOpName("matmul_grad_filter"), input, matmul,
                      ops::MatMul::Attrs().TransposeA(false).TransposeB(false));
    } else {
      matmul_grad_input =
          ops::MatMul(s.WithOpName("matmul_grad_input"), weight, matmul,
                      ops::MatMul::Attrs().TransposeA(true).TransposeB(true));
      matmul_grad_filter =
          ops::MatMul(s.WithOpName("matmul_grad_filter"), matmul, input,
                      ops::MatMul::Attrs().TransposeA(true).TransposeB(true));
    }
    auto fetch_matmul =
        ops::Identity(s.WithOpName("fetch_m"), matmul_grad_filter);
    auto fetch_bias = ops::Identity(s.WithOpName("fetch_b"), bias_add);

    auto input_t = GenerateRandomTensor<DT_FLOAT>({m, k});
    if (ta) input_t = GenerateRandomTensor<DT_FLOAT>({k, m});
    auto weight_t = GenerateRandomTensor<DT_FLOAT>({k, n});
    if (tb) weight_t = GenerateRandomTensor<DT_FLOAT>({n, k});

    GrapplerItem item;
    item.fetch = {"fetch_m", "fetch_b"};
    item.feed = {{"input", input_t}, {"weight", weight_t}};
    TF_CHECK_OK(s.ToGraphDef(&item.graph));

    // Place all nodes on CPU.
    for (int i = 0; i < item.graph.node_size(); ++i) {
      item.graph.mutable_node(i)->set_device("/device:CPU:0");
    }

    Remapper optimizer(RewriterConfig::ON);
    GraphDef output;
    TF_CHECK_OK(optimizer.Optimize(nullptr, item, &output));

    int found = 0;
    for (const NodeDef& node : output.node()) {
      if (node.name() == "matmul_grad_filter") {
        EXPECT_EQ("_FusedMatMulGrad", node.op());
        EXPECT_EQ("input", node.input(0));
        EXPECT_EQ("matmul", node.input(1));

        const auto fused_ops = node.attr().at("fused_ops").list().s();
        EXPECT_EQ(1, fused_ops.size());
        EXPECT_EQ("BiasAddGrad", fused_ops[0]);
        found++;
      }
    }
    EXPECT_EQ(1, found);

    auto tensors_expected = EvaluateNodes(item.graph, item.fetch, item.feed);
    auto tensors = EvaluateNodes(output, item.fetch, item.feed);
    EXPECT_EQ(2, tensors_expected.size());
    EXPECT_EQ(2, tensors.size());
    test::ExpectTensorNear<float>(tensors_expected[0], tensors[0], 1e-6);
    test::ExpectTensorNear<float>(tensors_expected[1], tensors[1], 1e-6);
  }
};

TEST_F(MklFuseMatMulWithBiasAddGrad, a0b0) {
  bool traspose_a = false;
  bool traspose_b = false;
  this->VerifyFused(traspose_a, traspose_b);
}

TEST_F(MklFuseMatMulWithBiasAddGrad, a0b1) {
  bool traspose_a = false;
  bool traspose_b = true;
  this->VerifyFused(traspose_a, traspose_b);
}

TEST_F(MklFuseMatMulWithBiasAddGrad, a1b0) {
  bool traspose_a = true;
  bool traspose_b = false;
  this->VerifyFused(traspose_a, traspose_b);
}

TEST_F(MklFuseMatMulWithBiasAddGrad, a1b1) {
  bool traspose_a = true;
  bool traspose_b = true;
  this->VerifyFused(traspose_a, traspose_b);
}

TEST_F(MklFuseMatMulWithBiasAddGrad, negative0) {
  using ::tensorflow::ops::Placeholder;
  tensorflow::Scope s = tensorflow::Scope::NewRootScope();

  int m = 2, k = 3, n = 4;

  auto input_shape = ops::Placeholder::Shape({m, k});
  auto weight_shape = ops::Placeholder::Shape({k, n});

  auto input = Placeholder(s.WithOpName("input"), DT_FLOAT, input_shape);
  auto weight = Placeholder(s.WithOpName("weight"), DT_FLOAT, weight_shape);

  auto matmul =
      ops::MatMul(s.WithOpName("matmul"), input, weight,
                  ops::MatMul::Attrs().TransposeA(false).TransposeB(false));
  auto matmul1 =
      ops::MatMul(s.WithOpName("matmul1"), weight, input,
                  ops::MatMul::Attrs().TransposeA(true).TransposeB(true));
  auto bias_add = ops::BiasAddGrad(s.WithOpName("bias_add_grad"), matmul);
  Output matmul_grad_input;
  Output matmul_grad_filter;
  matmul_grad_input =
      ops::MatMul(s.WithOpName("matmul_grad_input"), matmul, weight,
                  ops::MatMul::Attrs().TransposeA(false).TransposeB(true));
  matmul_grad_filter =
      ops::MatMul(s.WithOpName("matmul_grad_filter"), input, matmul,
                  ops::MatMul::Attrs().TransposeA(true).TransposeB(false));
  auto fetch_matmul =
      ops::Identity(s.WithOpName("fetch_m"), matmul_grad_filter);
  auto fetch_bias = ops::Identity(s.WithOpName("fetch_b"), bias_add);

  auto input_t = GenerateRandomTensor<DT_FLOAT>({m, k});
  auto weight_t = GenerateRandomTensor<DT_FLOAT>({k, n});

  GrapplerItem item;
  item.fetch = {"fetch_m", "fetch_b"};
  item.feed = {{"input", input_t}, {"weight", weight_t}};
  TF_CHECK_OK(s.ToGraphDef(&item.graph));

  // Place all nodes on CPU.
  for (int i = 0; i < item.graph.node_size(); ++i) {
    item.graph.mutable_node(i)->set_device("/device:CPU:0");
  }

  Remapper optimizer(RewriterConfig::ON);
  GraphDef output;
  TF_CHECK_OK(optimizer.Optimize(nullptr, item, &output));

  for (const NodeDef& node : output.node()) {
    if (node.name() == "matmul_grad_filter") {
      EXPECT_EQ("MatMul", node.op());
    }
  }
}

TEST_F(MklFuseMatMulWithBiasAddGrad, negative1) {
  using ::tensorflow::ops::Placeholder;
  tensorflow::Scope s = tensorflow::Scope::NewRootScope();

  int m = 2, k = 3, n = 4;

  auto input_shape = ops::Placeholder::Shape({m, k});
  auto weight_shape = ops::Placeholder::Shape({k, n});

  auto input = Placeholder(s.WithOpName("input"), DT_FLOAT, input_shape);
  auto weight = Placeholder(s.WithOpName("weight"), DT_FLOAT, weight_shape);

  auto matmul =
      ops::MatMul(s.WithOpName("matmul"), input, weight,
                  ops::MatMul::Attrs().TransposeA(false).TransposeB(false));
  auto bias_add = ops::BiasAddGrad(s.WithOpName("bias_add_grad"), matmul);
  auto relu = ops::Relu(s.WithOpName("relu"), matmul);
  Output matmul_grad_input;
  Output matmul_grad_filter;
  matmul_grad_input =
      ops::MatMul(s.WithOpName("matmul_grad_input"), matmul, weight,
                  ops::MatMul::Attrs().TransposeA(false).TransposeB(true));
  matmul_grad_filter =
      ops::MatMul(s.WithOpName("matmul_grad_filter"), input, matmul,
                  ops::MatMul::Attrs().TransposeA(true).TransposeB(false));
  auto fetch_matmul =
      ops::Identity(s.WithOpName("fetch_m"), matmul_grad_filter);
  auto fetch_bias = ops::Identity(s.WithOpName("fetch_b"), bias_add);

  auto input_t = GenerateRandomTensor<DT_FLOAT>({m, k});
  auto weight_t = GenerateRandomTensor<DT_FLOAT>({k, n});

  GrapplerItem item;
  item.fetch = {"fetch_m", "fetch_b"};
  item.feed = {{"input", input_t}, {"weight", weight_t}};
  TF_CHECK_OK(s.ToGraphDef(&item.graph));

  // Place all nodes on CPU.
  for (int i = 0; i < item.graph.node_size(); ++i) {
    item.graph.mutable_node(i)->set_device("/device:CPU:0");
  }

  Remapper optimizer(RewriterConfig::ON);
  GraphDef output;
  TF_CHECK_OK(optimizer.Optimize(nullptr, item, &output));

  for (const NodeDef& node : output.node()) {
    if (node.name() == "matmul_grad_filter") {
      EXPECT_EQ("MatMul", node.op());
    }
  }
}

TEST_F(MklFuseMatMulWithBiasAddGrad, negative2) {
  using ::tensorflow::ops::Placeholder;
  tensorflow::Scope s = tensorflow::Scope::NewRootScope();

  int m = 2, k = 3, n = 4;

  auto input_shape = ops::Placeholder::Shape({m, k});
  auto weight_shape = ops::Placeholder::Shape({k, n});

  auto input = Placeholder(s.WithOpName("input"), DT_FLOAT, input_shape);
  auto weight = Placeholder(s.WithOpName("weight"), DT_FLOAT, weight_shape);

  auto matmul =
      ops::MatMul(s.WithOpName("matmul"), input, weight,
                  ops::MatMul::Attrs().TransposeA(false).TransposeB(false));
  auto bias_add = ops::BiasAddGrad(s.WithOpName("bias_add_grad"), matmul);
  Output matmul_grad_input;
  Output matmul_grad_filter;
  matmul_grad_filter =
      ops::MatMul(s.WithOpName("matmul_grad_filter"), input, matmul,
                  ops::MatMul::Attrs().TransposeA(true).TransposeB(false));
  auto fetch_matmul =
      ops::Identity(s.WithOpName("fetch_m"), matmul_grad_filter);
  auto fetch_bias = ops::Identity(s.WithOpName("fetch_b"), bias_add);

  auto input_t = GenerateRandomTensor<DT_FLOAT>({m, k});
  auto weight_t = GenerateRandomTensor<DT_FLOAT>({k, n});

  GrapplerItem item;
  item.fetch = {"fetch_m", "fetch_b"};
  item.feed = {{"input", input_t}, {"weight", weight_t}};
  TF_CHECK_OK(s.ToGraphDef(&item.graph));

  // Place all nodes on CPU.
  for (int i = 0; i < item.graph.node_size(); ++i) {
    item.graph.mutable_node(i)->set_device("/device:CPU:0");
  }

  Remapper optimizer(RewriterConfig::ON);
  GraphDef output;
  TF_CHECK_OK(optimizer.Optimize(nullptr, item, &output));

  for (const NodeDef& node : output.node()) {
    if (node.name() == "matmul_grad_filter") {
      EXPECT_EQ("MatMul", node.op());
    }
  }
}

}  // namespace grappler
}  // namespace tensorflow
#endif  // INTEL_MKL
