// Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "paddle/fluid/imperative/all_reduce.h"
#include "paddle/fluid/imperative/layer.h"
#include "paddle/fluid/imperative/variable_wrapper.h"
#include "paddle/fluid/memory/memory.h"

namespace paddle {
namespace imperative {

struct Bucket {
  framework::Tensor contents;

  std::vector<size_t> offset_;
  std::vector<size_t> length_;
  // Global indices of participating variables in the bucket
  std::vector<size_t> variable_indices_;

  // Number of params that haven't been ready
  size_t pending = -1;
  bool is_sparse_ = false;

  // external message of bucket
  framework::proto::VarType::Type dtype;
};

struct VariableIndex {
  // record the index in buckets_
  size_t bucket_index;
  size_t variable_index;
};

class Reducer {
 public:
  explicit Reducer(
      const std::vector<std::shared_ptr<imperative::VarBase>>& vars,
      const std::vector<std::vector<size_t>>& bucket_indices);

  virtual ~Reducer() {}

  void initialize_buckets(
      const std::vector<std::vector<size_t>>& bucket_indices);

  void initialize_grad_space(const Bucket& bucket);

  void Print_Data();

  void add_dist_hook(VariableWrapper* var_warpper);

  void mark_variable_ready(const VariableIndex& var_index,
                           VariableWrapper* var_warpper);

  void mark_bucket_ready(size_t bucket_index);

  void finalize_backward();
  // Reducer Singleton
  static std::shared_ptr<Reducer> SetInstance(
      const std::vector<std::shared_ptr<imperative::VarBase>>& vars,
      const std::vector<std::vector<size_t>>& bucket_indices) {
    if (NULL == s_instance_) {
      s_instance_.reset(new paddle::imperative::Reducer(vars, bucket_indices));
    }
    return s_instance_;
  }

  static std::shared_ptr<Reducer> GetInstance() {
    PADDLE_ENFORCE_EQ(
        s_instance_ != NULL, true,
        platform::errors::InvalidArgument("Reducer is not initialized."));
    return s_instance_;
  }

 protected:
  std::vector<std::shared_ptr<imperative::VarBase>> vars_;
  std::vector<std::vector<size_t>> bucket_indices_;

 private:
  static std::shared_ptr<Reducer> s_instance_;
  std::vector<Bucket> buckets_;
  size_t next_bucket_ = 0;
  std::unordered_map<std::string, VariableIndex> varname2index_;
  platform::Place place_;
  std::unique_ptr<paddle::platform::CUDADeviceContext> dev_ctx_;
};

std::vector<std::vector<size_t>> assign_bucket_by_size(
    const std::vector<std::shared_ptr<imperative::VarBase>>& tensors,
    const std::vector<size_t>& bucket_size_limits);

}  // namespace imperative
}  // namespace paddle
