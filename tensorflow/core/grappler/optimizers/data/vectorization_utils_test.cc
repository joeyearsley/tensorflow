/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/grappler/optimizers/data/vectorization_utils.h"

#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/grappler/optimizers/data/function_utils.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/tools/graph_transforms/transform_utils.h"

namespace tensorflow {
namespace grappler {
namespace vectorization_utils {
namespace {

NodeDef* AddCastNode(const string& name, const std::vector<string>& inputs,
                     DataType src, DataType dst, bool truncate,
                     FunctionDef* fn) {
  NodeDef* node = function_utils::AddNode(name, "Cast", inputs, {}, fn);
  graph_transforms::SetNodeAttr("SrcT", src, node);
  graph_transforms::SetNodeAttr("DstT", dst, node);
  graph_transforms::SetNodeAttr("Truncate", truncate, node);
  return node;
}

NodeDef* AddUnstackNode(const string& name, const std::vector<string>& inputs,
                        DataType t, int axis, int num, FunctionDef* fn) {
  NodeDef* node = function_utils::AddNode(name, "Unpack", inputs, {}, fn);
  graph_transforms::SetNodeAttr("T", t, node);
  graph_transforms::SetNodeAttr("axis", axis, node);
  graph_transforms::SetNodeAttr("num", num, node);
  return node;
}

NodeDef* AddMapDefunNode(const string& name, const std::vector<string>& inputs,
                         const std::vector<DataType>& t_arguments,
                         const std::vector<DataType>& output_types,
                         const std::vector<TensorShape>& output_shapes,
                         const string& function_name, FunctionDef* fn) {
  NameAttrList func;
  func.set_name(function_name);
  NodeDef* node = function_utils::AddNode(name, "MapDefun", inputs, {}, fn);
  graph_transforms::SetNodeAttr("Targuments", t_arguments, node);
  graph_transforms::SetNodeAttr("output_types", output_types, node);
  graph_transforms::SetNodeAttr("output_shapes", output_shapes, node);
  graph_transforms::SetNodeAttr("f", func, node);
  return node;
}

// TODO(rachelim): Use FunctionDefHelper::Create instead
FunctionDef CreateFunction(
    StringPiece name, const std::vector<std::pair<string, DataType>>& inputs,
    const std::vector<std::pair<string, DataType>>& outputs,
    const std::map<string, string>& rets) {
  FunctionDef func;
  auto* signature = func.mutable_signature();
  signature->set_name(string(name));
  for (const auto& x : inputs) {
    auto* arg_def = signature->add_input_arg();
    arg_def->set_name(x.first);
    arg_def->set_type(x.second);
  }
  for (const auto& x : outputs) {
    auto* arg_def = signature->add_output_arg();
    arg_def->set_name(x.first);
    arg_def->set_type(x.second);
  }
  for (const auto& x : rets) {
    (*func.mutable_ret())[x.first] = x.second;
  }

  return func;
}

TEST(FunctionDefInputDescTest, ConstructedCorrectly) {}

// Before:
//
//                 +------+   +------+
// +---------------+ Arg0 +---+ Arg1 +--------+
// |               +---+--+   +---+--+        |
// |                   |          |           |
// |               +---v--+   +---v--+        |
// |   +-----------+ Arg0 +---+ Arg1 +----+   |
// |   |           +---+--+   +---+--+    |   |
// |   |               |          |       |   |
// |   | MapDefun  +---v--+   +---v--+    |   |
// |   +-----------+ Ret0 +---+ Ret1 +----+   |
// |               +---+--+   +---+--+        |
// |                   |          |           |
// |               +---v--+   +---v--+        |
// +---------------+ Ret0 +---+ Ret1 +--------+
//                 +------+   +------+
//
//
//  After:
//
//                 +------+   +------+
// +---------------+ Arg0 +---+ Arg1 +--------+
// |               +---+--+   +---+--+        |
// |                   |          |           |
// |                   |          |           |
// |                   |          |           |
// |               +---v--+   +---v--+        |
// +---------------+ Ret0 +---+ Ret1 +--------+
//                 +------+   +------+
//
TEST(VectorizeMapDefunTest, VectorizeDefunNoOps) {
  FunctionDef inner =
      CreateFunction("inner_function", {{"arg0", DT_INT32}, {"arg1", DT_INT32}},
                     {{"ret0", DT_INT32}, {"ret1", DT_INT32}},
                     {{"ret0", "arg0"}, {"ret1", "arg1"}});
  FunctionDef outer = CreateFunction(
      "outer_function", {{"ret0", DT_INT32}, {"ret1", DT_INT32}},
      {{"mapdefun", DT_INT32}, {"mapdefun_0", DT_INT32}},
      {{"mapdefun", "MapDefun:output:0"}, {"mapdefun_0", "MapDefun:output:1"}});

  NodeDef* map_defun = AddMapDefunNode(
      "MapDefun", {"ret0", "ret1"}, {DT_INT32, DT_INT32}, {DT_INT32, DT_INT32},
      {{}, {}}, inner.signature().name(), &outer);
  CHECK_NOTNULL(map_defun);

  VectorizeMapDefun(&outer, &inner, map_defun);
  EXPECT_TRUE(!function_utils::ContainsFunctionNodeWithOp("MapDefun", outer));
  EXPECT_EQ(outer.ret().at("mapdefun"), "ret0");
  EXPECT_EQ(outer.ret().at("mapdefun_0"), "ret1");
}

// Before:
//
//                 +------+   +------+
// +---------------+ Arg0 +---+ Arg1 +--------+
// |               +---+--+   +---+--+        |
// |                   |          |           |
// |               +---v--+   +---v--+        |
// |   +-----------+ Arg0 +---+ Arg1 +----+   |
// |   |           +---+--+   +---+--+    |   |
// |   |               |          |       |   |
// |   |   +------+    |      +---v--+    |   |
// |   |   |Const |    |      | Op0  |    |   |
// |   |   +---v--+    |      +---+--+    |   |
// |   |       |       |          |       |   |
// |   |       |   +---v--+   +---v--+    |   |
// |   |       +---| XOp1 |   | XOp2 |    |   |
// |   |           +---+--+   +---+--+    |   |
// |   |               |          |       |   |
// |   | MapDefun  +---v--+   +---v--+    |   |
// |   +-----------+ Ret0 +---+ Ret1 +----+   |
// |               +---+--+   +---+--+        |
// |                   |          |           |
// |               +---v--+   +---v--+        |
// +---------------+ Ret0 +---+ Ret1 +--------+
//                 +------+   +------+
//
//   where XOp1 and XOp2 are not convertible.
//
// After:
//
// No change because the ops are not convertible.
//
TEST(VectorizeMapDefunTest, VectorizeDefunUnconvertible) {
  FunctionDef inner =
      CreateFunction("inner_function", {{"arg0", DT_INT32}, {"arg1", DT_INT32}},
                     {{"ret0", DT_INT32}, {"ret1", DT_INT32}},
                     {{"ret0", "XOp1:output:0"}, {"ret1", "XOp2:output:0"}});
  NodeDef* x_op1 =
      function_utils::AddNode("XOp1", "XOp1", {"const", "arg0"}, {}, &inner);
  CHECK_NOTNULL(x_op1);

  NodeDef* x_op2 = function_utils::AddNode("XOp2", "XOp2", {"op1"}, {}, &inner);
  CHECK_NOTNULL(x_op2);

  FunctionDef outer = CreateFunction(
      "outer_function", {{"x", DT_INT32}, {"y", DT_INT32}},
      {{"mapdefun", DT_INT32}, {"mapdefun_0", DT_INT32}},
      {{"mapdefun", "MapDefun:output:0"}, {"mapdefun_0", "MapDefun:output:1"}});

  NodeDef* map_defun = AddMapDefunNode(
      "MapDefun", {"x", "y"}, {DT_INT32, DT_INT32}, {DT_INT32, DT_INT32},
      {{}, {}}, inner.signature().name(), &outer);
  CHECK_NOTNULL(map_defun);

  FunctionDef outer_copy(outer);
  FunctionDef inner_copy(inner);
  VectorizeMapDefun(&outer, &inner, map_defun);
  // They should be unchanged
  EXPECT_TRUE(FunctionDefsEqual(outer_copy, outer));
  EXPECT_TRUE(FunctionDefsEqual(inner_copy, inner));
}

// Before:
//
//
//                 +------+
// +---------------+ Arg0 +---------+
// |               +---+--+         |
// |                   |            |
// |               +---v--+         |
// |   +-----------+ Arg0 +-----+   |
// |   |           +---+--+     |   |
// |   |               |        |   |
// |   |               |        |   |
// |   |           +---v--+     |   |
// |   |           | Cast |     |   |
// |   |           +---+--+     |   |
// |   |               |        |   |
// |   | MapDefun  +---v--+     |   |
// |   +-----------+ Ret0 +-----+   |
// |               +---+--+         |
// |                   |            |
// |               +---v--+         |
// +---------------+ Ret0 +---------+
//                 +------+
//
//
//  After:
//
//                 +------+
// +---------------+ Arg0 +---------+
// |               +---+--+         |
// |                   |            |
// |               +---v--+         |
// |               | Cast |         |
// |               +---+--+         |
// |                   |            |
// |               +---v--+         |
// +---------------+ Ret0 +---------+
//                 +------+
//
TEST(VectorizeMapDefunTest, VectorizeDefunSimpleCast) {
  FunctionDef inner =
      CreateFunction("inner_function", {{"arg0", DT_INT32}},
                     {{"ret0", DT_INT64}}, {{"ret0", "Cast:y:0"}});
  NodeDef* cast_op =
      AddCastNode("Cast", {"arg0"}, DT_INT32, DT_INT64, false, &inner);
  CHECK_NOTNULL(cast_op);

  FunctionDef outer = CreateFunction("outer_function", {{"x", DT_INT32}},
                                     {{"mapdefun", DT_INT64}},
                                     {{"mapdefun", "MapDefun:output:0"}});

  NodeDef* map_defun =
      AddMapDefunNode("MapDefun", {"x"}, {DT_INT32}, {DT_INT64}, {{}},
                      inner.signature().name(), &outer);
  CHECK_NOTNULL(map_defun);

  VectorizeMapDefun(&outer, &inner, map_defun);
  EXPECT_TRUE(!function_utils::ContainsFunctionNodeWithOp("MapDefun", outer));
  const NodeDef& cast_node =
      outer.node_def(function_utils::FindFunctionNodeWithOp("Cast", outer));
  EXPECT_EQ(cast_node.input(0), "x");
  EXPECT_EQ(outer.ret().at("mapdefun"),
            strings::StrCat(cast_node.name(), ":y:0"));
  EXPECT_EQ(outer.node_def_size(), 1);
}

// Before:
//
//                 +------+
// +---------------+ Arg0 +-------------------+
// |               +---+--+                   |
// |                   |                      |
// |               +---v--+                   |
// |   +-----------+ Arg0 +---------------+   |
// |   |           +---+--+               |   |
// |   |               |                  |   |
// |   |               |                  |   |
// |   |           +---v--+               |   |
// |   |           | Cast |               |   |
// |   |           +---+--+               |   |
// |   |               |                  |   |
// |   |               +----------+       |   |
// |   |               |          |       |   |
// |   | MapDefun  +---v--+   +---v--+    |   |
// |   +-----------+ Ret0 +---+ Ret1 +----+   |
// |               +---+--+   +---+--+        |
// |                   |          |           |
// |               +---v--+   +---v--+        |
// +---------------+ Ret0 +---+ Ret1 +--------+
//                 +------+   +------+
//
//
//  After:
//
//                 +------+
// +---------------+ Arg0 +-------------------+
// |               +---+--+                   |
// |                   |                      |
// |                   |                      |
// |               +---v--+                   |
// |               | Cast |                   |
// |               +---+--+                   |
// |                   |                      |
// |                   +----------+           |
// |                   |          |           |
// |               +---v--+   +---v--+        |
// +---------------+ Ret0 +---+ Ret1 +--------+
//                 +------+   +------+
//
TEST(VectorizeMapDefunTest, VectorizeDefunCastUsedTwice) {
  // Tests that behavior is correct when an output is used more than once.
  FunctionDef inner =
      CreateFunction("inner_function", {{"arg0", DT_INT32}},
                     {{"ret0", DT_INT64}, {"ret1", DT_INT64}},
                     {{"ret0", "Cast:y:0"}, {"ret1", "Cast:y:0"}});
  NodeDef* cast_op =
      AddCastNode("Cast", {"arg0"}, DT_INT32, DT_INT64, false, &inner);
  CHECK_NOTNULL(cast_op);

  FunctionDef outer = CreateFunction(
      "outer_function", {{"x", DT_INT32}},
      {{"mapdefun", DT_INT64}, {"mapdefun_0", DT_INT64}},
      {{"mapdefun", "MapDefun:output:0"}, {"mapdefun_0", "MapDefun:output:1"}});

  NodeDef* map_defun =
      AddMapDefunNode("MapDefun", {"x"}, {DT_INT32}, {DT_INT64, DT_INT64},
                      {{}, {}}, inner.signature().name(), &outer);
  CHECK_NOTNULL(map_defun);

  VectorizeMapDefun(&outer, &inner, map_defun);
  EXPECT_TRUE(!function_utils::ContainsFunctionNodeWithOp("MapDefun", outer));
  const NodeDef& cast_node =
      outer.node_def(function_utils::FindFunctionNodeWithOp("Cast", outer));
  EXPECT_EQ(cast_node.input(0), "x");
  EXPECT_EQ(outer.ret().at("mapdefun"),
            strings::StrCat(cast_node.name(), ":y:0"));
  EXPECT_EQ(outer.ret().at("mapdefun_0"),
            strings::StrCat(cast_node.name(), ":y:0"));
  EXPECT_EQ(outer.node_def_size(), 1);
}

// Before:
//
//                        +------+
// +----------------------+ Arg0 +----------------------+
// |                      +---+--+                      |
// |                          |                         |
// |                      +---v--+                      |
// |   +------------------+ Arg0 +------------------+   |
// |   |                  +---+--+                  |   |
// |   |                      |                     |   |
// |   |                      |                     |   |
// |   |                  +---v---+ num=3           |   |
// |   |                  |Unstack| axis=0          |   |
// |   |                  ++--+--++                 |   |
// |   |                   |  |  |                  |   |
// |   |              +----+  |  +-------+          |   |
// |   |              |       |          |          |   |
// |   | MapDefun +---v--+  +-v----+  +--v---+      |   |
// |   +----------+ Ret0 +--+ Ret1 +--+ Ret2 +------+   |
// |              +---+--+  +--+---+  +--+---+          |
// |                  |        |         |              |
// |              +---v--+  +--v---+  +--v---+          |
// +--------------+ Ret0 +--+ Ret1 +--+ Ret2 +----------+
//                +------+  +------+  +------+
//
//
//  After:
//
//                        +------+
// +----------------------+ Arg0 +----------------------+
// |                      +---+--+                      |
// |                          |                         |
// |                          |                         |
// |                          |                         |
// |                      +---v---+ num=3               |
// |                      |Unstack| axis=1              |
// |                      ++--+--++                     |
// |                       |  |  |                      |
// |                  +----+  |  +-------+              |
// |                  |       |          |              |
// |                  |       |          |              |
// |              +---v--+  +-v----+  +--v---+          |
// +--------------+ Ret0 +--+ Ret1 +--+ Ret2 +----------+
//                +------+  +------+  +------+
//
TEST(VectorizeMapDefunTest, VectorizeDefunOpWithMultipleOutputs) {
  FunctionDef inner = CreateFunction(
      "inner_function", {{"arg0", DT_INT32}},
      {{"ret0", DT_INT32}, {"ret1", DT_INT32}, {"ret2", DT_INT32}},
      {{"ret0", "MyUnstack:output:0"},
       {"ret1", "MyUnstack:output:1"},
       {"ret2", "MyUnstack:output:2"}});
  NodeDef* unstack_op =
      AddUnstackNode("MyUnstack", {"arg0"}, DT_INT32, 0, 3, &inner);
  CHECK_NOTNULL(unstack_op);

  FunctionDef outer = CreateFunction("outer_function", {{"x", DT_INT32}},
                                     {{"mapdefun", DT_INT32},
                                      {"mapdefun_0", DT_INT32},
                                      {"mapdefun_1", DT_INT32}},
                                     {{"mapdefun", "MapDefun:output:0"},
                                      {"mapdefun_0", "MapDefun:output:1"},
                                      {"mapdefun_1", "MapDefun:output:2"}});

  NodeDef* map_defun = AddMapDefunNode(
      "MapDefun", {"x"}, {DT_INT32}, {DT_INT32, DT_INT32, DT_INT32},
      {{1}, {1}, {1}}, inner.signature().name(), &outer);
  CHECK_NOTNULL(map_defun);

  VectorizeMapDefun(&outer, &inner, map_defun);
  EXPECT_TRUE(!function_utils::ContainsFunctionNodeWithOp("MapDefun", outer));
  const NodeDef& unpack_node =
      outer.node_def(function_utils::FindFunctionNodeWithOp("Unpack", outer));
  EXPECT_EQ(unpack_node.input(0), "x");
  EXPECT_EQ(unpack_node.attr().at("axis").i(), 1);
  EXPECT_EQ(unpack_node.attr().at("T").type(), DT_INT32);
  EXPECT_EQ(unpack_node.attr().at("num").i(), 3);
  EXPECT_EQ(outer.ret().at("mapdefun"),
            strings::StrCat(unpack_node.name(), ":output:0"));
  EXPECT_EQ(outer.ret().at("mapdefun_0"),
            strings::StrCat(unpack_node.name(), ":output:1"));
  EXPECT_EQ(outer.ret().at("mapdefun_1"),
            strings::StrCat(unpack_node.name(), ":output:2"));
  EXPECT_EQ(outer.node_def_size(), 1);
}

// Before:
//
//                        +------+
// +----------------------+ Arg0 +----------------------+
// |                      +---+--+                      |
// |                          |                         |
// |                      +---v--+                      |
// |   +------------------+ Arg0 +------------------+   |
// |   |                  +---+--+                  |   |
// |   |                      |                     |   |
// |   |                  +---+--+                  |   |
// |   |                  | Cast |                  |   |
// |   |                  +---+--+                  |   |
// |   |                      |                     |   |
// |   |                  +---v---+ num=3           |   |
// |   |                  |Unstack| axis=0          |   |
// |   |                  ++--+--++                 |   |
// |   |                   |  |  |                  |   |
// |   |              +----+  |  +-------+          |   |
// |   |              |       |          |          |   |
// |   | MapDefun +---v--+  +-v----+  +--v---+      |   |
// |   +----------+ Ret0 +--+ Ret1 +--+ Ret2 +------+   |
// |              +---+--+  +--+---+  +--+---+          |
// |                  |        |         |              |
// |              +---v--+  +--v---+  +--v---+          |
// +--------------+ Ret0 +--+ Ret1 +--+ Ret2 +----------+
//                +------+  +------+  +------+
//
//
//  After:
//
//                        +------+
// +----------------------+ Arg0 +----------------------+
// |                      +---+--+                      |
// |                          |                         |
// |                      +---+--+                      |
// |                      | Cast |                      |
// |                      +---+--+                      |
// |                          |                         |
// |                      +---v---+ num=3               |
// |                      |Unstack| axis=1              |
// |                      ++--+--++                     |
// |                       |  |  |                      |
// |                  +----+  |  +-------+              |
// |                  |       |          |              |
// |                  |       |          |              |
// |              +---v--+  +-v----+  +--v---+          |
// +--------------+ Ret0 +--+ Ret1 +--+ Ret2 +----------+
//                +------+  +------+  +------+
//
TEST(VectorizeMapDefunTest, VectorizeDefunChainedConvertibleOps) {
  FunctionDef inner = CreateFunction(
      "inner_function", {{"arg0", DT_INT32}},
      {{"ret0", DT_INT32}, {"ret1", DT_INT32}, {"ret2", DT_INT32}},
      {{"ret0", "MyUnstack:output:0"},
       {"ret1", "MyUnstack:output:1"},
       {"ret2", "MyUnstack:output:2"}});
  NodeDef* cast_op =
      AddCastNode("Cast", {"arg0"}, DT_INT32, DT_INT64, false, &inner);
  CHECK_NOTNULL(cast_op);
  NodeDef* unstack_op =
      AddUnstackNode("MyUnstack", {"Cast:y:0"}, DT_INT32, 0, 3, &inner);
  CHECK_NOTNULL(unstack_op);

  FunctionDef outer = CreateFunction("outer_function", {{"x", DT_INT32}},
                                     {{"mapdefun", DT_INT32},
                                      {"mapdefun_0", DT_INT32},
                                      {"mapdefun_1", DT_INT32}},
                                     {{"mapdefun", "MapDefun:output:0"},
                                      {"mapdefun_0", "MapDefun:output:1"},
                                      {"mapdefun_1", "MapDefun:output:2"}});

  NodeDef* map_defun = AddMapDefunNode(
      "MapDefun", {"x"}, {DT_INT32}, {DT_INT32, DT_INT32, DT_INT32},
      {{1}, {1}, {1}}, inner.signature().name(), &outer);
  CHECK_NOTNULL(map_defun);

  VectorizeMapDefun(&outer, &inner, map_defun);
  EXPECT_TRUE(!function_utils::ContainsFunctionNodeWithOp("MapDefun", outer));
  const NodeDef& cast_node =
      outer.node_def(function_utils::FindFunctionNodeWithOp("Cast", outer));
  EXPECT_EQ(cast_node.input(0), "x");
  const NodeDef& unpack_node =
      outer.node_def(function_utils::FindFunctionNodeWithOp("Unpack", outer));
  EXPECT_EQ(unpack_node.input(0), strings::StrCat(cast_node.name(), ":y:0"));
  EXPECT_EQ(unpack_node.attr().at("axis").i(), 1);
  EXPECT_EQ(unpack_node.attr().at("T").type(), DT_INT32);
  EXPECT_EQ(unpack_node.attr().at("num").i(), 3);

  EXPECT_EQ(outer.ret().at("mapdefun"),
            strings::StrCat(unpack_node.name(), ":output:0"));
  EXPECT_EQ(outer.ret().at("mapdefun_0"),
            strings::StrCat(unpack_node.name(), ":output:1"));
  EXPECT_EQ(outer.ret().at("mapdefun_1"),
            strings::StrCat(unpack_node.name(), ":output:2"));
  EXPECT_EQ(outer.node_def_size(), 2);
}

// Before:
//
//
//                 +------+
// +---------------+ Arg0 +---------+
// |               +---+--+         |
// |                   |            |
// |               +---v--+         |
// |   +-----------+ Arg0 +-----+   |
// |   |           +---+--+     |   |
// |   |     +---------+        |   |
// |   | +---v--+      |        |   |
// |   | |Print |      |        |   |
// |   | +---+--+      |        |   |
// |   |     :     +---v--+     |   |
// |   |     ::::::> Cast |     |   |
// |   |           +---+--+     |   |
// |   |               |        |   |
// |   | MapDefun  +---v--+     |   |
// |   +-----------+ Ret0 +-----+   |
// |               +---+--+         |
// |                   |            |
// |               +---v--+         |
// +---------------+ Ret0 +---------+
//                 +------+
//
//
//  After:
//
//  No change because we don't deal with control inputs for now.
//
TEST(VectorizeMapDefunTest, VectorizeDefunWithControlInputs) {
  FunctionDef inner =
      CreateFunction("inner_function", {{"arg0", DT_INT32}},
                     {{"ret0", DT_INT64}}, {{"ret0", "Cast:y:0"}});
  // The attrs aren't relevant
  NodeDef* print_op =
      function_utils::AddNode("Print", "Print", {"arg0", "arg0"}, {}, &inner);
  CHECK_NOTNULL(print_op);
  NodeDef* cast_op = AddCastNode("Cast", {"arg0", "^Print"}, DT_INT32, DT_INT64,
                                 false, &inner);
  CHECK_NOTNULL(cast_op);

  FunctionDef outer = CreateFunction("outer_function", {{"x", DT_INT32}},
                                     {{"mapdefun", DT_INT64}},
                                     {{"mapdefun", "MapDefun:output:0"}});

  NodeDef* map_defun =
      AddMapDefunNode("MapDefun", {"x"}, {DT_INT32}, {DT_INT64}, {{}},
                      inner.signature().name(), &outer);
  CHECK_NOTNULL(map_defun);

  FunctionDef outer_copy(outer);
  FunctionDef inner_copy(inner);
  VectorizeMapDefun(&outer, &inner, map_defun);
  // They should be unchanged
  EXPECT_TRUE(FunctionDefsEqual(outer_copy, outer));
}

// TODO(rachelim): More test cases when we get around to implementing them:
// [] A badly defined converter, e.g. doesn't produce nodes that have the
//    same number of outputs/inputs as the nodes to be converted
// [] Converter where the 'converted' form has multiple nodes.
// [] Case with dependent nodes, e.g. ops with const inputs that are
//    broadcasted.
// [] Python-side tests to actually run the functions to make sure
//    they work.

}  // namespace
}  // namespace vectorization_utils
}  // namespace grappler
}  // namespace tensorflow