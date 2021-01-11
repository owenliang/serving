/* Copyright 2017 Google Inc. All Rights Reserved.

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

#include "tensorflow_serving/servables/tensorflow/util.h"

#include <atomic>

#include "absl/strings/str_format.h"
#include "google/protobuf/wrappers.pb.h"
#include "tensorflow/cc/saved_model/signature_constants.h"
#include "tensorflow/core/example/example.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/monitoring/counter.h"
#include "tensorflow/core/lib/monitoring/sampler.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/cord.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/path.h"
#include "tensorflow/core/platform/threadpool_options.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow_serving/apis/input.pb.h"
#include "tensorflow_serving/apis/internal/serialized_input.pb.h"
#include "tensorflow_serving/apis/model.pb.h"
#include "tensorflow_serving/resources/resource_values.h"

namespace tensorflow {
namespace serving {
namespace {

// Constants used in the resource estimation heuristic.
static constexpr double kResourceEstimateRAMMultiplier = 1.2;
static constexpr int kResourceEstimateRAMPadBytes = 0;

auto* example_counts = monitoring::Sampler<1>::New(
    {"/tensorflow/serving/request_example_counts",
     "The number of tensorflow.Examples per request.", "model"},
    // It's 15 buckets with the last bucket being 2^14 to DBL_MAX;
    // so the limits are [1, 2, 4, 8, ..., 16 * 1024, DBL_MAX].
    monitoring::Buckets::Exponential(1, 2, 15));

auto* example_count_total = monitoring::Counter<1>::New(
    "/tensorflow/serving/request_example_count_total",
    "The total number of tensorflow.Examples.", "model");

auto* runtime_latency = monitoring::Sampler<3>::New(
    {
        "/tensorflow/serving/runtime_latency",
        "Distribution of wall time (in microseconds) for Tensorflow runtime.",
        "model_name",
        "API",
        "runtime",
    },  // Scale of 10, power of 1.8 with bucket count 33 (~20 minutes).
    monitoring::Buckets::Exponential(10, 1.8, 33));

auto* model_call_count = monitoring::Counter<2>::New(
    "/tensorflow/serving/model_call_count",
    "The call counter of each model-version", "model_nane", "model_version");

// Returns the number of examples in the Input.
int NumInputExamples(const internal::SerializedInput& input) {
  switch (input.kind_case()) {
    case Input::KindCase::kExampleList:
      return input.example_list().examples_size();
    case Input::KindCase::kExampleListWithContext:
      return input.example_list_with_context().examples_size();
    default:
      break;
  }
  return 0;
}

std::atomic<bool> signature_method_check{true};

// Returns all the descendants, both directories and files, recursively under
// 'dirname'. The paths returned are all prefixed with 'dirname'.
Status GetAllDescendants(const string& dirname, FileProbingEnv* env,
                         std::vector<string>* const descendants) {
  descendants->clear();
  // Make sure that dirname exists;
  TF_RETURN_IF_ERROR(env->FileExists(dirname));
  std::deque<string> dir_q;      // Queue for the BFS
  std::vector<string> dir_list;  // List of all dirs discovered
  dir_q.push_back(dirname);
  // Do a BFS on the directory to discover all immediate children.
  while (!dir_q.empty()) {
    const string dir = dir_q.front();
    dir_q.pop_front();
    std::vector<string> children;
    // GetChildren might fail if we don't have appropriate permissions.
    TF_RETURN_IF_ERROR(env->GetChildren(dir, &children));
    for (const string& child : children) {
      const string child_path = io::JoinPath(dir, child);
      descendants->push_back(child_path);
      // If the child is a directory add it to the queue.
      if (env->IsDirectory(child_path).ok()) {
        dir_q.push_back(child_path);
      }
    }
  }
  return Status::OK();
}

}  // namespace

namespace internal {

monitoring::Sampler<1>* GetExampleCounts() { return example_counts; }

monitoring::Counter<1>* GetExampleCountTotal() { return example_count_total; }

}  // namespace internal

void SetSignatureMethodNameCheckFeature(bool v) { signature_method_check = v; }

bool GetSignatureMethodNameCheckFeature() { return signature_method_check; }

void RecordRequestExampleCount(const string& model_name, size_t count) {
  example_counts->GetCell(model_name)->Add(count);
  example_count_total->GetCell(model_name)->IncrementBy(count);
}

Status InputToSerializedExampleTensor(const Input& input, Tensor* examples) {
  internal::SerializedInput serialized_input;
  // There's a reason we serialize and then parse 'input' in this way:
  // 'example_list' and 'example_list_with_context' are lazily parsed
  // fields, which means they are lazily deserialized the very first
  // time they are accessed. So if we access them here for counting the
  // num_examples, then we'll pay a heavy cost of deserialization.
  //
  // SerializedInput proto has been created to prevent this, but at the same
  // time get the count of num_examples as well.
  bool parse_serialized_input_ok = false;
#if defined(PLATFORM_GOOGLE)
  // Benchmark ('BM_InputToSerializedExample') can help measure the effect of
  // changes in the future.
  parse_serialized_input_ok =
      serialized_input.ParseFromCord(input.SerializeAsCord());
#else
  parse_serialized_input_ok =
      serialized_input.ParseFromString(input.SerializeAsString());
#endif
  if (!parse_serialized_input_ok) {
    return errors::Internal("Error parsing serialized input.");
  }

  const int64 num_examples = NumInputExamples(serialized_input);
  if (num_examples == 0) {
    return errors::InvalidArgument("Input is empty.");
  }
  *examples = Tensor(DT_STRING, TensorShape({num_examples}));
  switch (serialized_input.kind_case()) {
    case Input::KindCase::KIND_NOT_SET:
      break;

    case Input::KindCase::kExampleList: {
      auto input_vec = examples->vec<tstring>();
      int input_vec_index = 0;
      for (const auto& entry : serialized_input.example_list().examples()) {
        input_vec(input_vec_index++) = entry;
      }
      break;
    }

    case Input::KindCase::kExampleListWithContext: {
      const auto& context =
          serialized_input.example_list_with_context().context();
      auto input_vec = examples->vec<tstring>();
      int input_vec_index = 0;
      for (const auto& entry :
           serialized_input.example_list_with_context().examples()) {
        tstring& input_str = input_vec(input_vec_index++);
        input_str.resize_uninitialized(context.size() + entry.size());
        // 'input_str_ptr' now points to the beginning of input_str.
        char* input_str_ptr = &input_str[0];
#if defined(PLATFORM_GOOGLE)
        // When absl::Cord OSS is fully shipped and protobuf open-source suports
        // Cord, we can get rid of marco above and unify code path.
        context.CopyToArray(input_str_ptr);
        entry.CopyToArray(input_str_ptr + context.size());
#else
        memcpy(input_str_ptr, &context[0], context.size());
        memcpy(input_str_ptr + context.size(), &entry[0], entry.size());
#endif
      }
    } break;

    default:
      return errors::Unimplemented(
          "Input with kind ", serialized_input.kind_case(), " not supported.");
  }
  return Status::OK();
}

Status PerformOneShotTensorComputation(
    const RunOptions& run_options, const Input& input,
    const string& input_tensor_name,
    const std::vector<string>& output_tensor_names, Session* session,
    std::vector<Tensor>* outputs, int* num_input_examples,
    const thread::ThreadPoolOptions& thread_pool_options,
    int64* runtime_latency) {
  // Setup the input Tensor to be a vector of string containing the serialized
  // tensorflow.Example.
  Tensor input_tensor;
  TF_RETURN_IF_ERROR(InputToSerializedExampleTensor(input, &input_tensor));
  *num_input_examples = input_tensor.dim_size(0);

  const uint64 start_microseconds = EnvTime::NowMicros();
  RunMetadata run_metadata;
  TF_RETURN_IF_ERROR(session->Run(
      run_options, {{input_tensor_name, input_tensor}}, output_tensor_names, {},
      outputs, &run_metadata, thread_pool_options));
  const uint64 end_microseconds = EnvTime::NowMicros();
  if (runtime_latency != nullptr) {
    *runtime_latency = end_microseconds - start_microseconds;
  }
  return Status::OK();
}

Status PerformOneShotTensorComputation(
    const RunOptions& run_options, const Input& input,
    const std::set<string>& input_tensor_names,
    const std::vector<string>& output_tensor_names, Session* session,
    std::vector<Tensor>* outputs, int* num_input_examples,
    const thread::ThreadPoolOptions& thread_pool_options) {
  // Setup the input Tensor to be a vector of string containing the serialized
  // tensorflow.Example.
  Tensor input_tensor;
  TF_RETURN_IF_ERROR(InputToSerializedExampleTensor(input, &input_tensor));
  *num_input_examples = input_tensor.dim_size(0);

  std::vector<std::pair<string, Tensor>> inputs;
  inputs.reserve(input_tensor_names.size());
  for (const auto& name : input_tensor_names) {
    inputs.emplace_back(name, input_tensor);
  }

  RunMetadata run_metadata;
  return session->Run(run_options, inputs, output_tensor_names, {}, outputs,
                      &run_metadata, thread_pool_options);
}

void MakeModelSpec(const string& model_name,
                   const absl::optional<string>& signature_name,
                   const absl::optional<int64>& version,
                   ModelSpec* model_spec) {
  model_spec->Clear();
  model_spec->set_name(model_name);
  if (signature_name) {
    model_spec->set_signature_name(signature_name->empty()
                                       ? kDefaultServingSignatureDefKey
                                       : *signature_name);
  }
  if (version) {
    model_spec->mutable_version()->set_value(*version);
  }
}

Status GetModelDiskSize(const string& path, FileProbingEnv* env,
                        uint64* total_file_size) {
  if (env == nullptr) {
    return errors::Internal("FileProbingEnv not set");
  }

  std::vector<string> descendants;
  TF_RETURN_IF_ERROR(GetAllDescendants(path, env, &descendants));
  *total_file_size = 0;
  for (const string& descendant : descendants) {
    if (!(env->IsDirectory(descendant).ok())) {
      uint64 file_size;
      TF_RETURN_IF_ERROR(env->GetFileSize(descendant, &file_size));
      *total_file_size += file_size;
    }
  }
  return Status::OK();
}

Status EstimateResourceFromPathUsingDiskState(const string& path,
                                              FileProbingEnv* env,
                                              ResourceAllocation* estimate) {
  uint64 total_file_size = 0;
  TF_RETURN_IF_ERROR(GetModelDiskSize(path, env, &total_file_size));

  const uint64 ram_requirement =
      total_file_size * kResourceEstimateRAMMultiplier +
      kResourceEstimateRAMPadBytes;

  ResourceAllocation::Entry* ram_entry = estimate->add_resource_quantities();
  Resource* ram_resource = ram_entry->mutable_resource();
  ram_resource->set_device(device_types::kMain);
  ram_resource->set_kind(resource_kinds::kRamBytes);
  ram_entry->set_quantity(ram_requirement);

  return Status::OK();
}

void RecordRuntimeLatency(const string& model_name, const string& api,
                          const string& runtime, int64 latency_usec) {
  runtime_latency->GetCell(model_name, api, runtime)->Add(latency_usec);
}

void RecordModelCall(const string& model_name, int64 model_version) {
  std::string model_version_s = absl::StrFormat("%lld", model_version);
  model_call_count->GetCell(model_name, model_version_s)->IncrementBy(1);
}

}  // namespace serving
}  // namespace tensorflow
