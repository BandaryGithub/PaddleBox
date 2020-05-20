/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#if defined _WIN32 || defined __APPLE__
#else
#define _LINUX
#endif

#include "paddle/fluid/framework/data_feed.h"
#ifdef _LINUX
#include <stdio_ext.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif
#include <utility>
#include "gflags/gflags.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"
#include "io/fs.h"
#include "io/shell.h"
#include "paddle/fluid/framework/feed_fetch_method.h"
#include "paddle/fluid/framework/feed_fetch_type.h"
#include "paddle/fluid/framework/fleet/box_wrapper.h"
#include "paddle/fluid/framework/fleet/fleet_wrapper.h"
#include "paddle/fluid/platform/timer.h"

namespace paddle {
namespace framework {
using platform::Timer;

void RecordCandidateList::ReSize(size_t length) {
  _mutex.lock();
  _capacity = length;
  CHECK(_capacity > 0);  // NOLINT
  _candidate_list.clear();
  _candidate_list.resize(_capacity);
  _full = false;
  _cur_size = 0;
  _total_size = 0;
  _mutex.unlock();
}

void RecordCandidateList::ReInit() {
  _mutex.lock();
  _full = false;
  _cur_size = 0;
  _total_size = 0;
  _mutex.unlock();
}

void RecordCandidateList::AddAndGet(const Record& record,
                                    RecordCandidate* result) {
  _mutex.lock();
  size_t index = 0;
  ++_total_size;
  auto fleet_ptr = FleetWrapper::GetInstance();
  if (!_full) {
    _candidate_list[_cur_size++] = record;
    _full = (_cur_size == _capacity);
  } else {
    CHECK(_cur_size == _capacity);
    index = fleet_ptr->LocalRandomEngine()() % _total_size;
    if (index < _capacity) {
      _candidate_list[index] = record;
    }
  }
  index = fleet_ptr->LocalRandomEngine()() % _cur_size;
  *result = _candidate_list[index];
  _mutex.unlock();
}

void DataFeed::AddFeedVar(Variable* var, const std::string& name) {
  CheckInit();
  for (size_t i = 0; i < use_slots_.size(); ++i) {
    if (name == use_slots_[i]) {
      if (var == nullptr) {
        feed_vec_[i] = nullptr;
      } else {
        feed_vec_[i] = var->GetMutable<LoDTensor>();
      }
    }
  }
}

bool DataFeed::SetFileList(const std::vector<std::string>& files) {
  std::unique_lock<std::mutex> lock(*mutex_for_pick_file_);
  CheckInit();
  // Do not set finish_set_filelist_ flag,
  // since a user may set file many times after init reader
  filelist_.assign(files.begin(), files.end());

  finish_set_filelist_ = true;
  return true;
}

void DataFeed::SetBatchSize(int batch_size) {
  PADDLE_ENFORCE_GT(batch_size, 0,
                    platform::errors::InvalidArgument(
                        "Batch size %d is illegal.", batch_size));
  default_batch_size_ = batch_size;
}

bool DataFeed::PickOneFile(std::string* filename) {
  PADDLE_ENFORCE_NOT_NULL(
      mutex_for_pick_file_,
      platform::errors::PreconditionNotMet(
          "You should call SetFileListMutex before PickOneFile"));
  PADDLE_ENFORCE_NOT_NULL(
      file_idx_, platform::errors::PreconditionNotMet(
                     "You should call SetFileListIndex before PickOneFile"));
  std::unique_lock<std::mutex> lock(*mutex_for_pick_file_);
  if (*file_idx_ == filelist_.size()) {
    VLOG(3) << "DataFeed::PickOneFile no more file to pick";
    return false;
  }
  VLOG(3) << "file_idx_=" << *file_idx_;
  *filename = filelist_[(*file_idx_)++];
  return true;
}

void DataFeed::CheckInit() {
  PADDLE_ENFORCE(finish_init_, "Initialization did not succeed.");
}

void DataFeed::CheckSetFileList() {
  PADDLE_ENFORCE(finish_set_filelist_, "Set filelist did not succeed.");
}

void DataFeed::CheckStart() {
  PADDLE_ENFORCE_EQ(finish_start_, true,
                    platform::errors::PreconditionNotMet(
                        "Datafeed has not started running yet."));
}

void DataFeed::AssignFeedVar(const Scope& scope) {
  CheckInit();
  for (size_t i = 0; i < use_slots_.size(); ++i) {
    feed_vec_[i] = scope.FindVar(use_slots_[i])->GetMutable<LoDTensor>();
  }
}

void DataFeed::CopyToFeedTensor(void* dst, const void* src, size_t size) {
  if (platform::is_cpu_place(this->place_)) {
    memcpy(dst, src, size);
  } else {
#ifdef PADDLE_WITH_CUDA
    cudaMemcpy(dst, src, size, cudaMemcpyHostToDevice);
#else
    PADDLE_THROW("Not supported GPU, Please compile WITH_GPU option");
#endif
  }
}

template <typename T>
void PrivateQueueDataFeed<T>::SetQueueSize(int queue_size) {
  PADDLE_ENFORCE(queue_size > 0, "Illegal queue size: %d.", queue_size);
  queue_size_ = queue_size;
  queue_ = paddle::framework::MakeChannel<T>();
  queue_->SetCapacity(queue_size);
}

template <typename T>
bool PrivateQueueDataFeed<T>::Start() {
  CheckSetFileList();
  read_thread_ = std::thread(&PrivateQueueDataFeed::ReadThread, this);
  read_thread_.detach();

  finish_start_ = true;
  return true;
}

template <typename T>
void PrivateQueueDataFeed<T>::ReadThread() {
#ifdef _LINUX
  std::string filename;
  while (PickOneFile(&filename)) {
    int err_no = 0;
    fp_ = fs_open_read(filename, &err_no, pipe_command_);
    __fsetlocking(&*fp_, FSETLOCKING_BYCALLER);
    T instance;
    while (ParseOneInstanceFromPipe(&instance)) {
      queue_->Put(instance);
    }
  }
  queue_->Close();
#endif
}

template <typename T>
int PrivateQueueDataFeed<T>::Next() {
#ifdef _LINUX
  CheckStart();
  int index = 0;
  T ins_vec;
  while (index < default_batch_size_) {
    T instance;
    if (!queue_->Get(instance)) {
      break;
    }
    AddInstanceToInsVec(&ins_vec, instance, index++);
  }
  batch_size_ = index;
  if (batch_size_ != 0) {
    PutToFeedVec(ins_vec);
  }
  return batch_size_;
#else
  return 0;
#endif
}

// explicit instantiation
template class PrivateQueueDataFeed<std::vector<MultiSlotType>>;

template <typename T>
InMemoryDataFeed<T>::InMemoryDataFeed() {
  this->file_idx_ = nullptr;
  this->mutex_for_pick_file_ = nullptr;
  this->fp_ = nullptr;
  this->thread_id_ = 0;
  this->thread_num_ = 1;
  this->parse_ins_id_ = false;
  this->parse_content_ = false;
  this->parse_logkey_ = false;
  this->enable_pv_merge_ = false;
  this->current_phase_ = 1;  // 1:join ;0:update
  this->input_channel_ = nullptr;
  this->output_channel_ = nullptr;
  this->consume_channel_ = nullptr;
}

template <typename T>
bool InMemoryDataFeed<T>::Start() {
#ifdef _LINUX
  this->CheckSetFileList();
  if (output_channel_->Size() == 0 && input_channel_->Size() != 0) {
    std::vector<T> data;
    input_channel_->Read(data);
    output_channel_->Write(std::move(data));
  }
#endif
  this->finish_start_ = true;
  return true;
}

template <typename T>
int InMemoryDataFeed<T>::Next() {
#ifdef _LINUX
  Timer next_timer;
  next_timer.Resume();
  this->CheckStart();
  CHECK(output_channel_ != nullptr);
  CHECK(consume_channel_ != nullptr);
  VLOG(3) << "output_channel_ size=" << output_channel_->Size()
          << ", consume_channel_ size=" << consume_channel_->Size()
          << ", thread_id=" << thread_id_;
  int index = 0;
  T instance;
  std::vector<T> ins_vec;
  // ins_vec.clear();
  ins_vec.reserve(this->default_batch_size_);
  platform::Timer lock_timer;
  while (index < this->default_batch_size_) {
    if (output_channel_->Size() == 0) {
      break;
    }
    output_channel_->GetUnlock(instance);
    lock_timer.Resume();
    ins_vec.push_back(instance);
    lock_timer.Pause();
    ++index;
    consume_channel_->PutUnlock(std::move(instance));
  }
  this->batch_size_ = index;
  VLOG(3) << "batch_size_=" << this->batch_size_
          << ", thread_id=" << thread_id_;
  next_timer.Pause();
  // VLOG(0) << "UPDATE_NEXT:" << next_timer.ElapsedUS();
  if (this->batch_size_ != 0) {
    PutToFeedVec(ins_vec);
  } else {
    VLOG(3) << "finish reading, output_channel_ size="
            << output_channel_->Size()
            << ", consume_channel_ size=" << consume_channel_->Size()
            << ", thread_id=" << thread_id_;
  }
  // VLOG(0) << "UPDATEPUSHBACK:" << lock_timer.ElapsedUS();
  return this->batch_size_;
#else
  return 0;
#endif
}

template <typename T>
void InMemoryDataFeed<T>::SetInputChannel(void* channel) {
  input_channel_ = static_cast<paddle::framework::ChannelObject<T>*>(channel);
}

template <typename T>
void InMemoryDataFeed<T>::SetInputPtrChannel(void* channel) {
  input_ptr_channel_ =
      static_cast<paddle::framework::ChannelObject<T*>*>(channel);
}

template <typename T>
void InMemoryDataFeed<T>::SetOutputChannel(void* channel) {
  output_channel_ = static_cast<paddle::framework::ChannelObject<T>*>(channel);
}

template <typename T>
void InMemoryDataFeed<T>::SetConsumeChannel(void* channel) {
  consume_channel_ = static_cast<paddle::framework::ChannelObject<T>*>(channel);
}

template <typename T>
void InMemoryDataFeed<T>::SetOutputPtrChannel(void* channel) {
  output_ptr_channel_ =
      static_cast<paddle::framework::ChannelObject<T*>*>(channel);
}

template <typename T>
void InMemoryDataFeed<T>::SetConsumePtrChannel(void* channel) {
  consume_ptr_channel_ =
      static_cast<paddle::framework::ChannelObject<T*>*>(channel);
}

template <typename T>
void InMemoryDataFeed<T>::SetInputPvChannel(void* channel) {
  input_pv_channel_ =
      static_cast<paddle::framework::ChannelObject<PvInstance>*>(channel);
}

template <typename T>
void InMemoryDataFeed<T>::SetOutputPvChannel(void* channel) {
  output_pv_channel_ =
      static_cast<paddle::framework::ChannelObject<PvInstance>*>(channel);
}

template <typename T>
void InMemoryDataFeed<T>::SetConsumePvChannel(void* channel) {
  consume_pv_channel_ =
      static_cast<paddle::framework::ChannelObject<PvInstance>*>(channel);
}

template <typename T>
void InMemoryDataFeed<T>::SetThreadId(int thread_id) {
  thread_id_ = thread_id;
}

template <typename T>
void InMemoryDataFeed<T>::SetThreadNum(int thread_num) {
  thread_num_ = thread_num;
}

template <typename T>
void InMemoryDataFeed<T>::SetParseContent(bool parse_content) {
  parse_content_ = parse_content;
}

template <typename T>
void InMemoryDataFeed<T>::SetParseLogKey(bool parse_logkey) {
  parse_logkey_ = parse_logkey;
}

template <typename T>
void InMemoryDataFeed<T>::SetEnablePvMerge(bool enable_pv_merge) {
  enable_pv_merge_ = enable_pv_merge;
}

template <typename T>
void InMemoryDataFeed<T>::SetCurrentPhase(int current_phase) {
  current_phase_ = current_phase;
}

template <typename T>
void InMemoryDataFeed<T>::SetParseInsId(bool parse_ins_id) {
  parse_ins_id_ = parse_ins_id;
}

template <typename T>
void InMemoryDataFeed<T>::LoadIntoMemory() {
#ifdef _LINUX
  VLOG(3) << "LoadIntoMemory() begin, thread_id=" << thread_id_;
  std::string filename;
  while (this->PickOneFile(&filename)) {
    VLOG(3) << "PickOneFile, filename=" << filename
            << ", thread_id=" << thread_id_;
#ifdef PADDLE_WITH_BOX_PS
    if (BoxWrapper::GetInstance()->UseAfsApi()) {
      this->fp_ = BoxWrapper::GetInstance()->afs_manager->GetFile(
          filename, this->pipe_command_);
    } else {
#endif
      int err_no = 0;
      this->fp_ = fs_open_read(filename, &err_no, this->pipe_command_);
#ifdef PADDLE_WITH_BOX_PS
    }
#endif
    CHECK(this->fp_ != nullptr);
    __fsetlocking(&*(this->fp_), FSETLOCKING_BYCALLER);
    paddle::framework::ChannelWriter<T> writer(input_channel_);
    T instance;
    platform::Timer timeline;
    timeline.Start();
    while (ParseOneInstanceFromPipe(&instance)) {
      writer << std::move(instance);
      instance = T();
    }
    writer.Flush();
    timeline.Pause();
    VLOG(3) << "LoadIntoMemory() read all lines, file=" << filename
            << ", cost time=" << timeline.ElapsedSec()
            << " seconds, thread_id=" << thread_id_;
  }
  VLOG(3) << "LoadIntoMemory() end, thread_id=" << thread_id_;
#endif
}

// explicit instantiation
template class InMemoryDataFeed<Record>;

void MultiSlotDataFeed::Init(
    const paddle::framework::DataFeedDesc& data_feed_desc) {
  finish_init_ = false;
  finish_set_filelist_ = false;
  finish_start_ = false;

  PADDLE_ENFORCE(data_feed_desc.has_multi_slot_desc(),
                 "Multi_slot_desc has not been set.");
  paddle::framework::MultiSlotDesc multi_slot_desc =
      data_feed_desc.multi_slot_desc();
  SetBatchSize(data_feed_desc.batch_size());
  // temporarily set queue size = batch size * 100
  SetQueueSize(data_feed_desc.batch_size() * 100);
  size_t all_slot_num = multi_slot_desc.slots_size();
  all_slots_.resize(all_slot_num);
  all_slots_type_.resize(all_slot_num);
  use_slots_index_.resize(all_slot_num);
  total_dims_without_inductive_.resize(all_slot_num);
  inductive_shape_index_.resize(all_slot_num);
  use_slots_.clear();
  use_slots_is_dense_.clear();
  for (size_t i = 0; i < all_slot_num; ++i) {
    const auto& slot = multi_slot_desc.slots(i);
    all_slots_[i] = slot.name();
    all_slots_type_[i] = slot.type();
    use_slots_index_[i] = slot.is_used() ? use_slots_.size() : -1;
    total_dims_without_inductive_[i] = 1;
    inductive_shape_index_[i] = -1;
    if (slot.is_used()) {
      use_slots_.push_back(all_slots_[i]);
      use_slots_is_dense_.push_back(slot.is_dense());
      std::vector<int> local_shape;
      if (slot.is_dense()) {
        for (int j = 0; j < slot.shape_size(); ++j) {
          if (slot.shape(j) > 0) {
            total_dims_without_inductive_[i] *= slot.shape(j);
          }
          if (slot.shape(j) == -1) {
            inductive_shape_index_[i] = j;
          }
        }
      }
      for (int j = 0; j < slot.shape_size(); ++j) {
        local_shape.push_back(slot.shape(j));
      }
      use_slots_shape_.push_back(local_shape);
    }
  }
  feed_vec_.resize(use_slots_.size());
  pipe_command_ = data_feed_desc.pipe_command();
  finish_init_ = true;
}

void MultiSlotDataFeed::ReadThread() {
#ifdef _LINUX
  std::string filename;
  while (PickOneFile(&filename)) {
    int err_no = 0;
    fp_ = fs_open_read(filename, &err_no, pipe_command_);
    CHECK(fp_ != nullptr);
    __fsetlocking(&*fp_, FSETLOCKING_BYCALLER);
    std::vector<MultiSlotType> instance;
    int ins_num = 0;
    while (ParseOneInstanceFromPipe(&instance)) {
      ins_num++;
      queue_->Put(instance);
    }
    VLOG(3) << "filename: " << filename << " inst num: " << ins_num;
  }
  queue_->Close();
#endif
}

bool MultiSlotDataFeed::CheckFile(const char* filename) {
#ifdef _LINUX
  CheckInit();  // get info of slots
  std::ifstream fin(filename);
  if (!fin.good()) {
    VLOG(1) << "error: open file<" << filename << "> fail";
    return false;
  }
  std::string line;
  int instance_cout = 0;
  std::string all_slots_alias = "";
  for (const auto& alias : all_slots_) {
    all_slots_alias += alias + " ";
  }
  std::string use_slots_alias = "";
  for (const auto& alias : use_slots_) {
    use_slots_alias += alias + " ";
  }
  VLOG(3) << "total slots num: " << all_slots_.size();
  VLOG(3) << "total slots alias: " << all_slots_alias;
  VLOG(3) << "used slots num: " << use_slots_.size();
  VLOG(3) << "used slots alias: " << use_slots_alias;
  while (getline(fin, line)) {
    ++instance_cout;
    const char* str = line.c_str();
    char* endptr = const_cast<char*>(str);
    int len = line.length();
    for (size_t i = 0; i < all_slots_.size(); ++i) {
      auto num = strtol(endptr, &endptr, 10);
      if (num < 0) {
        VLOG(0) << "error: the number of ids is a negative number: " << num;
        VLOG(0) << "please check line<" << instance_cout << "> in file<"
                << filename << ">";
        return false;
      } else if (num == 0) {
        VLOG(0)
            << "error: the number of ids can not be zero, you need "
               "padding it in data generator; or if there is something wrong"
               " with the data, please check if the data contains unresolvable "
               "characters.";
        VLOG(0) << "please check line<" << instance_cout << "> in file<"
                << filename << ">";
        return false;
      } else if (errno == ERANGE || num > INT_MAX) {
        VLOG(0) << "error: the number of ids greater than INT_MAX";
        VLOG(0) << "please check line<" << instance_cout << "> in file<"
                << filename << ">";
        return false;
      }
      if (all_slots_type_[i] == "float") {
        for (int i = 0; i < num; ++i) {
          strtof(endptr, &endptr);
          if (errno == ERANGE) {
            VLOG(0) << "error: the value is out of the range of "
                       "representable values for float";
            VLOG(0) << "please check line<" << instance_cout << "> in file<"
                    << filename << ">";
            return false;
          }
          if (i + 1 != num && endptr - str == len) {
            VLOG(0) << "error: there is a wrong with the number of ids.";
            VLOG(0) << "please check line<" << instance_cout << "> in file<"
                    << filename << ">";
            return false;
          }
        }
      } else if (all_slots_type_[i] == "uint64") {
        for (int i = 0; i < num; ++i) {
          strtoull(endptr, &endptr, 10);
          if (errno == ERANGE) {
            VLOG(0) << "error: the value is out of the range of "
                       "representable values for uint64_t";
            VLOG(0) << "please check line<" << instance_cout << "> in file<"
                    << filename << ">";
            return false;
          }
          if (i + 1 != num && endptr - str == len) {
            VLOG(0) << "error: there is a wrong with the number of ids.";
            VLOG(0) << "please check line<" << instance_cout << "> in file<"
                    << filename << ">";
            return false;
          }
        }
      } else {
        VLOG(0) << "error: this type<" << all_slots_type_[i]
                << "> is not supported";
        return false;
      }
    }
    // It may be added '\t' character to the end of the output of reduce
    // task when processes data by Hadoop(when the output of the reduce
    // task of Hadoop has only one field, it will add a '\t' at the end
    // of the line by default, and you can use this option to avoid it:
    // `-D mapred.textoutputformat.ignoreseparator=true`), which does
    // not affect the correctness of the data. Therefore, it should be
    // judged that the data is not normal when the end of each line of
    // data contains characters which are not spaces.
    while (endptr - str != len) {
      if (!isspace(*(endptr++))) {
        VLOG(0)
            << "error: there is some extra characters at the end of the line.";
        VLOG(0) << "please check line<" << instance_cout << "> in file<"
                << filename << ">";
        return false;
      }
    }
  }
  VLOG(3) << "instances cout: " << instance_cout;
  VLOG(3) << "The file format is correct";
#endif
  return true;
}

bool MultiSlotDataFeed::ParseOneInstanceFromPipe(
    std::vector<MultiSlotType>* instance) {
#ifdef _LINUX
  thread_local string::LineFileReader reader;

  if (!reader.getline(&*(fp_.get()))) {
    return false;
  } else {
    int use_slots_num = use_slots_.size();
    instance->resize(use_slots_num);

    const char* str = reader.get();
    std::string line = std::string(str);
    // VLOG(3) << line;
    char* endptr = const_cast<char*>(str);
    int pos = 0;
    for (size_t i = 0; i < use_slots_index_.size(); ++i) {
      int idx = use_slots_index_[i];
      int num = strtol(&str[pos], &endptr, 10);
      PADDLE_ENFORCE_NE(
          num, 0,
          platform::errors::InvalidArgument(
              "The number of ids can not be zero, you need padding "
              "it in data generator; or if there is something wrong with "
              "the data, please check if the data contains unresolvable "
              "characters.\nplease check this error line: %s",
              str));
      if (idx != -1) {
        (*instance)[idx].Init(all_slots_type_[i]);
        if ((*instance)[idx].GetType()[0] == 'f') {  // float
          for (int j = 0; j < num; ++j) {
            float feasign = strtof(endptr, &endptr);
            (*instance)[idx].AddValue(feasign);
          }
        } else if ((*instance)[idx].GetType()[0] == 'u') {  // uint64
          for (int j = 0; j < num; ++j) {
            uint64_t feasign = (uint64_t)strtoull(endptr, &endptr, 10);
            (*instance)[idx].AddValue(feasign);
          }
        }
        pos = endptr - str;
      } else {
        for (int j = 0; j <= num; ++j) {
          // pos = line.find_first_of(' ', pos + 1);
          while (line[pos + 1] != ' ') {
            pos++;
          }
        }
      }
    }
    return true;
  }
#else
  return true;
#endif
}

bool MultiSlotDataFeed::ParseOneInstance(std::vector<MultiSlotType>* instance) {
#ifdef _LINUX
  std::string line;
  if (getline(file_, line)) {
    int use_slots_num = use_slots_.size();
    instance->resize(use_slots_num);
    // parse line
    const char* str = line.c_str();
    char* endptr = const_cast<char*>(str);
    int pos = 0;
    for (size_t i = 0; i < use_slots_index_.size(); ++i) {
      int idx = use_slots_index_[i];
      int num = strtol(&str[pos], &endptr, 10);
      PADDLE_ENFORCE(
          num,
          "The number of ids can not be zero, you need padding "
          "it in data generator; or if there is something wrong with "
          "the data, please check if the data contains unresolvable "
          "characters.\nplease check this error line: %s",
          str);

      if (idx != -1) {
        (*instance)[idx].Init(all_slots_type_[i]);
        if ((*instance)[idx].GetType()[0] == 'f') {  // float
          for (int j = 0; j < num; ++j) {
            float feasign = strtof(endptr, &endptr);
            (*instance)[idx].AddValue(feasign);
          }
        } else if ((*instance)[idx].GetType()[0] == 'u') {  // uint64
          for (int j = 0; j < num; ++j) {
            uint64_t feasign = (uint64_t)strtoull(endptr, &endptr, 10);
            (*instance)[idx].AddValue(feasign);
          }
        }
        pos = endptr - str;
      } else {
        for (int j = 0; j <= num; ++j) {
          pos = line.find_first_of(' ', pos + 1);
        }
      }
    }
  } else {
    return false;
  }
#endif
  return false;
}

void MultiSlotDataFeed::AddInstanceToInsVec(
    std::vector<MultiSlotType>* ins_vec,
    const std::vector<MultiSlotType>& instance, int index) {
#ifdef _LINUX
  if (index == 0) {
    ins_vec->resize(instance.size());
    for (size_t i = 0; i < instance.size(); ++i) {
      (*ins_vec)[i].Init(instance[i].GetType());
      (*ins_vec)[i].InitOffset();
    }
  }

  for (size_t i = 0; i < instance.size(); ++i) {
    (*ins_vec)[i].AddIns(instance[i]);
  }
#endif
}

void MultiSlotDataFeed::PutToFeedVec(
    const std::vector<MultiSlotType>& ins_vec) {
#ifdef _LINUX
  for (size_t i = 0; i < use_slots_.size(); ++i) {
    if (feed_vec_[i] == nullptr) {
      continue;
    }
    const auto& type = ins_vec[i].GetType();
    const auto& offset = ins_vec[i].GetOffset();
    int total_instance = static_cast<int>(offset.back());

    if (type[0] == 'f') {  // float
      const auto& feasign = ins_vec[i].GetFloatData();
      float* tensor_ptr =
          feed_vec_[i]->mutable_data<float>({total_instance, 1}, this->place_);
      CopyToFeedTensor(tensor_ptr, &feasign[0], total_instance * sizeof(float));
    } else if (type[0] == 'u') {  // uint64
      // no uint64_t type in paddlepaddle
      const auto& feasign = ins_vec[i].GetUint64Data();
      int64_t* tensor_ptr = feed_vec_[i]->mutable_data<int64_t>(
          {total_instance, 1}, this->place_);
      CopyToFeedTensor(tensor_ptr, &feasign[0],
                       total_instance * sizeof(int64_t));
    }

    LoD data_lod{offset};
    feed_vec_[i]->set_lod(data_lod);
    if (use_slots_is_dense_[i]) {
      if (inductive_shape_index_[i] != -1) {
        use_slots_shape_[i][inductive_shape_index_[i]] =
            total_instance / total_dims_without_inductive_[i];
      }
      feed_vec_[i]->Resize(framework::make_ddim(use_slots_shape_[i]));
    }
  }
#endif
}

void MultiSlotInMemoryDataFeed::Init(
    const paddle::framework::DataFeedDesc& data_feed_desc) {
  finish_init_ = false;
  finish_set_filelist_ = false;
  finish_start_ = false;

  PADDLE_ENFORCE(data_feed_desc.has_multi_slot_desc(),
                 "Multi_slot_desc has not been set.");
  paddle::framework::MultiSlotDesc multi_slot_desc =
      data_feed_desc.multi_slot_desc();
  SetBatchSize(data_feed_desc.batch_size());
  size_t all_slot_num = multi_slot_desc.slots_size();
  all_slots_.resize(all_slot_num);
  all_slots_type_.resize(all_slot_num);
  use_slots_index_.resize(all_slot_num);
  total_dims_without_inductive_.resize(all_slot_num);
  inductive_shape_index_.resize(all_slot_num);
  use_slots_.clear();
  use_slots_is_dense_.clear();
  for (size_t i = 0; i < all_slot_num; ++i) {
    const auto& slot = multi_slot_desc.slots(i);
    all_slots_[i] = slot.name();
    all_slots_type_[i] = slot.type();
    use_slots_index_[i] = slot.is_used() ? use_slots_.size() : -1;
    total_dims_without_inductive_[i] = 1;
    inductive_shape_index_[i] = -1;
    if (slot.is_used()) {
      use_slots_.push_back(all_slots_[i]);
      use_slots_is_dense_.push_back(slot.is_dense());
      std::vector<int> local_shape;
      if (slot.is_dense()) {
        for (int j = 0; j < slot.shape_size(); ++j) {
          if (slot.shape(j) > 0) {
            total_dims_without_inductive_[i] *= slot.shape(j);
          }
          if (slot.shape(j) == -1) {
            inductive_shape_index_[i] = j;
          }
        }
      }
      for (int j = 0; j < slot.shape_size(); ++j) {
        local_shape.push_back(slot.shape(j));
      }
      use_slots_shape_.push_back(local_shape);
    }
  }
  feed_vec_.resize(use_slots_.size());
  const int kEstimatedFeasignNumPerSlot = 5;  // Magic Number
  for (size_t i = 0; i < all_slot_num; i++) {
    batch_float_feasigns_.push_back(std::vector<float>());
    batch_uint64_feasigns_.push_back(std::vector<uint64_t>());
    batch_float_feasigns_[i].reserve(default_batch_size_ *
                                     kEstimatedFeasignNumPerSlot);
    batch_uint64_feasigns_[i].reserve(default_batch_size_ *
                                      kEstimatedFeasignNumPerSlot);
    offset_.push_back(std::vector<size_t>());
    offset_[i].reserve(default_batch_size_ +
                       1);  // Each lod info will prepend a zero
  }
  visit_.resize(all_slot_num, false);
  offset.reserve(all_slot_num * (1 + default_batch_size_));
  offset_sum.reserve(all_slot_num * (1 + default_batch_size_));
  ins_vec.reserve(1 + default_batch_size_);
  pipe_command_ = data_feed_desc.pipe_command();
  finish_init_ = true;
  input_type_ = data_feed_desc.input_type();
}

void MultiSlotInMemoryDataFeed::GetMsgFromLogKey(const std::string& log_key,
                                                 uint64_t* search_id,
                                                 uint32_t* cmatch,
                                                 uint32_t* rank) {
  std::string searchid_str = log_key.substr(16, 16);
  *search_id = (uint64_t)strtoull(searchid_str.c_str(), NULL, 16);

  std::string cmatch_str = log_key.substr(11, 3);
  *cmatch = (uint32_t)strtoul(cmatch_str.c_str(), NULL, 16);

  std::string rank_str = log_key.substr(14, 2);
  *rank = (uint32_t)strtoul(rank_str.c_str(), NULL, 16);
}

bool MultiSlotInMemoryDataFeed::ParseOneInstanceFromPipe(Record* instance) {
#ifdef _LINUX
  thread_local string::LineFileReader reader;

  if (!reader.getline(&*(fp_.get()))) {
    return false;
  } else {
    const char* str = reader.get();
    std::string line = std::string(str);
    // VLOG(3) << line;
    char* endptr = const_cast<char*>(str);
    int pos = 0;
    if (parse_ins_id_) {
      int num = strtol(&str[pos], &endptr, 10);
      CHECK(num == 1);  // NOLINT
      pos = endptr - str + 1;
      size_t len = 0;
      while (str[pos + len] != ' ') {
        ++len;
      }
      instance->ins_id_ = std::string(str + pos, len);
      pos += len + 1;
      VLOG(3) << "ins_id " << instance->ins_id_;
    }
    if (parse_content_) {
      int num = strtol(&str[pos], &endptr, 10);
      CHECK(num == 1);  // NOLINT
      pos = endptr - str + 1;
      size_t len = 0;
      while (str[pos + len] != ' ') {
        ++len;
      }
      instance->content_ = std::string(str + pos, len);
      pos += len + 1;
      VLOG(3) << "content " << instance->content_;
    }
    if (parse_logkey_) {
      int num = strtol(&str[pos], &endptr, 10);
      CHECK(num == 1);  // NOLINT
      pos = endptr - str + 1;
      size_t len = 0;
      while (str[pos + len] != ' ') {
        ++len;
      }
      // parse_logkey
      std::string log_key = std::string(str + pos, len);
      uint64_t search_id;
      uint32_t cmatch;
      uint32_t rank;
      GetMsgFromLogKey(log_key, &search_id, &cmatch, &rank);

      instance->ins_id_ = log_key;
      instance->search_id = search_id;
      instance->cmatch = cmatch;
      instance->rank = rank;
      pos += len + 1;
    }
    instance->uint64_feasigns_.reserve(1000);
    instance->float_feasigns_.reserve(41);
    for (size_t i = 0; i < use_slots_index_.size(); ++i) {
      int idx = use_slots_index_[i];
      int num = strtol(&str[pos], &endptr, 10);
      PADDLE_ENFORCE(
          num,
          "The number of ids can not be zero, you need padding "
          "it in data generator; or if there is something wrong with "
          "the data, please check if the data contains unresolvable "
          "characters.\nplease check this error line: %s",
          str);
      if (idx != -1) {
        if (all_slots_type_[i][0] == 'f') {  // float
          for (int j = 0; j < num; ++j) {
            float feasign = strtof(endptr, &endptr);
            // if float feasign is equal to zero, ignore it
            // except when slot is dense
            if (fabs(feasign) < 1e-6 && !use_slots_is_dense_[i]) {
              continue;
            }
            FeatureKey f;
            f.float_feasign_ = feasign;
            instance->float_feasigns_.push_back(FeatureItem(f, idx));
          }
        } else if (all_slots_type_[i][0] == 'u') {  // uint64
          for (int j = 0; j < num; ++j) {
            uint64_t feasign = (uint64_t)strtoull(endptr, &endptr, 10);
            // if uint64 feasign is equal to zero, ignore it
            // except when slot is dense
            if (feasign == 0 && !use_slots_is_dense_[i]) {
              continue;
            }
            FeatureKey f;
            f.uint64_feasign_ = feasign;
            instance->uint64_feasigns_.push_back(FeatureItem(f, idx));
          }
        }
        pos = endptr - str;
      } else {
        for (int j = 0; j <= num; ++j) {
          // pos = line.find_first_of(' ', pos + 1);
          while (line[pos + 1] != ' ') {
            pos++;
          }
        }
      }
    }
    instance->float_feasigns_.shrink_to_fit();
    instance->uint64_feasigns_.shrink_to_fit();
    return true;
  }
#else
  return false;
#endif
}

bool MultiSlotInMemoryDataFeed::ParseOneInstance(Record* instance) {
#ifdef _LINUX
  std::string line;
  if (getline(file_, line)) {
    VLOG(3) << line;
    // parse line
    const char* str = line.c_str();
    char* endptr = const_cast<char*>(str);
    int pos = 0;
    for (size_t i = 0; i < use_slots_index_.size(); ++i) {
      int idx = use_slots_index_[i];
      int num = strtol(&str[pos], &endptr, 10);
      PADDLE_ENFORCE(
          num,
          "The number of ids can not be zero, you need padding "
          "it in data generator; or if there is something wrong with "
          "the data, please check if the data contains unresolvable "
          "characters.\nplease check this error line: %s",
          str);

      if (idx != -1) {
        if (all_slots_type_[i][0] == 'f') {  // float
          for (int j = 0; j < num; ++j) {
            float feasign = strtof(endptr, &endptr);
            if (fabs(feasign) < 1e-6) {
              continue;
            }
            FeatureKey f;
            f.float_feasign_ = feasign;
            instance->float_feasigns_.push_back(FeatureItem(f, idx));
          }
        } else if (all_slots_type_[i][0] == 'u') {  // uint64
          for (int j = 0; j < num; ++j) {
            uint64_t feasign = (uint64_t)strtoull(endptr, &endptr, 10);
            if (feasign == 0) {
              continue;
            }
            FeatureKey f;
            f.uint64_feasign_ = feasign;
            instance->uint64_feasigns_.push_back(FeatureItem(f, idx));
          }
        }
        pos = endptr - str;
      } else {
        for (int j = 0; j <= num; ++j) {
          pos = line.find_first_of(' ', pos + 1);
        }
      }
    }
    instance->float_feasigns_.shrink_to_fit();
    instance->uint64_feasigns_.shrink_to_fit();
    return true;
  } else {
    return false;
  }
#endif
  return false;
}

void MultiSlotInMemoryDataFeed::PutToFeedVec(
    const std::vector<Record>& ins_vec) {
#ifdef _LINUX
#if !defined(PADDLE_WITH_CUDA)
  for (size_t i = 0; i < batch_float_feasigns_.size(); ++i) {
    batch_float_feasigns_[i].clear();
    batch_uint64_feasigns_[i].clear();
    offset_[i].clear();
    offset_[i].push_back(0);
  }
  ins_content_vec_.clear();
  ins_content_vec_.reserve(ins_vec.size());
  ins_id_vec_.clear();
  ins_id_vec_.reserve(ins_vec.size());
  for (size_t i = 0; i < ins_vec.size(); ++i) {
    auto& r = ins_vec[i];
    ins_id_vec_.push_back(r.ins_id_);
    ins_content_vec_.push_back(r.content_);
    for (auto& item : r.float_feasigns_) {
      batch_float_feasigns_[item.slot()].push_back(item.sign().float_feasign_);
      visit_[item.slot()] = true;
    }
    for (auto& item : r.uint64_feasigns_) {
      batch_uint64_feasigns_[item.slot()].push_back(
          item.sign().uint64_feasign_);
      visit_[item.slot()] = true;
    }
    for (size_t j = 0; j < use_slots_.size(); ++j) {
      const auto& type = all_slots_type_[j];
      if (visit_[j]) {
        visit_[j] = false;
      } else {
        // fill slot value with default value 0
        if (type[0] == 'f') {  // float
          batch_float_feasigns_[j].push_back(0.0);
        } else if (type[0] == 'u') {  // uint64
          batch_uint64_feasigns_[j].push_back(0);
        }
      }
      // get offset of this ins in this slot
      if (type[0] == 'f') {  // float
        offset_[j].push_back(batch_float_feasigns_[j].size());
      } else if (type[0] == 'u') {  // uint64
        offset_[j].push_back(batch_uint64_feasigns_[j].size());
      }
    }
  }

  for (size_t i = 0; i < use_slots_.size(); ++i) {
    if (feed_vec_[i] == nullptr) {
      continue;
    }
    int total_instance = offset_[i].back();
    const auto& type = all_slots_type_[i];
    if (type[0] == 'f') {  // float
      float* feasign = batch_float_feasigns_[i].data();
      float* tensor_ptr =
          feed_vec_[i]->mutable_data<float>({total_instance, 1}, this->place_);
      CopyToFeedTensor(tensor_ptr, feasign, total_instance * sizeof(float));
    } else if (type[0] == 'u') {  // uint64
      // no uint64_t type in paddlepaddle
      uint64_t* feasign = batch_uint64_feasigns_[i].data();
      int64_t* tensor_ptr = feed_vec_[i]->mutable_data<int64_t>(
          {total_instance, 1}, this->place_);
      CopyToFeedTensor(tensor_ptr, feasign, total_instance * sizeof(int64_t));
    }
    auto& slot_offset = offset_[i];
    if (this->input_type_ == 0) {
      LoD data_lod{slot_offset};
      feed_vec_[i]->set_lod(data_lod);
    } else if (this->input_type_ == 1) {
      if (!use_slots_is_dense_[i]) {
        std::vector<size_t> tmp_offset;
        PADDLE_ENFORCE_EQ(slot_offset.size(), 2,
                          platform::errors::InvalidArgument(
                              "In batch reader, the sparse tensor lod size "
                              "must be 2, but received %d",
                              slot_offset.size()));
        const auto& max_size = slot_offset[1];
        tmp_offset.reserve(max_size + 1);
        for (unsigned int k = 0; k <= max_size; k++) {
          tmp_offset.emplace_back(k);
        }
        slot_offset = tmp_offset;
        LoD data_lod{slot_offset};
        feed_vec_[i]->set_lod(data_lod);
      }
    }
    if (use_slots_is_dense_[i]) {
      if (inductive_shape_index_[i] != -1) {
        use_slots_shape_[i][inductive_shape_index_[i]] =
            total_instance / total_dims_without_inductive_[i];
      }
      feed_vec_[i]->Resize(framework::make_ddim(use_slots_shape_[i]));
    }
  }
#else
  Timer timer;
  timer.Resume();
  paddle::platform::SetDeviceId(
      boost::get<platform::CUDAPlace>(this->GetPlace()).GetDeviceId());
  std::vector<size_t> ins_len(ins_vec.size(), 0);  // prefix sum of ins length
  size_t total_size = 0;
  for (size_t i = 0; i < ins_vec.size(); ++i) {
    auto& r = ins_vec[i];
    total_size += r.uint64_feasigns_.size() + r.float_feasigns_.size();
    ins_len[i] = total_size;
  }

  // fprintf(stderr, "totol size: %lu\n", total_size);
  auto cpu_buf = memory::AllocShared(platform::CPUPlace(),
                                     total_size * sizeof(FeatureItem));
  auto gpu_buf =
      memory::AllocShared(this->GetPlace(), total_size * sizeof(FeatureItem));
  FeatureItem* fea_list_cpu = reinterpret_cast<FeatureItem*>(cpu_buf->ptr());
  FeatureItem* fea_list_gpu = reinterpret_cast<FeatureItem*>(gpu_buf->ptr());

  size_t size_off = 0;
  for (size_t i = 0; i < ins_vec.size(); ++i) {
    auto& r = ins_vec[i];
    memcpy(fea_list_cpu + size_off, r.uint64_feasigns_.data(),
           sizeof(FeatureItem) * r.uint64_feasigns_.size());
    size_off += r.uint64_feasigns_.size();
    memcpy(fea_list_cpu + size_off, r.float_feasigns_.data(),
           sizeof(FeatureItem) * r.float_feasigns_.size());
    size_off += r.float_feasigns_.size();
  }

  size_t row_size = use_slots_.size();       // slot_number
  size_t col_size = 1 + ins_vec.size();      // 1 + batch_size
  size_t offset_size = row_size * col_size;  // length of lod array
  // printf("row_size: %d, col_size: %d\n", (int)row_size, (int)col_size);

  size_t uslot_num = 0;  // slot number of uint64_t type
  std::vector<size_t> index_map(
      row_size);  // make index map from origin slot id to actual slot id
  std::vector<char> slot_type(row_size, 'u');  // Record actual slot type

  for (size_t i = 0; i < row_size; ++i) {
    const auto& type = all_slots_type_[i];
    if (type[0] == 'u') {
      uslot_num++;
    }
  }
  for (size_t i = uslot_num; i < row_size; ++i) {
    slot_type[i] = 'f';
  }

  int u_slot_id = 0;
  for (size_t i = 0; i < use_slots_.size(); ++i) {
    const auto& type = all_slots_type_[i];
    if (type[0] == 'f') {  // float
      index_map[i] = uslot_num++;
    } else if (type[0] == 'u') {
      index_map[i] = u_slot_id++;
    }
  }

  offset.clear();
  offset_sum.clear();
  offset.resize(offset_size, 0);
  offset_sum.resize(offset_size, 0);

  // construct lod info
  int col = 1;
  for (size_t i = 0; i < total_size; ++i) {
    if (i >= ins_len[col - 1]) {
      col++;
    }
    offset[index_map[fea_list_cpu[i].slot()] * col_size + col]++;
  }

  // compute sum-lod info, for using kernel to make batch
  for (size_t i = 0; i < offset.size(); ++i) {
    int row = i / col_size;
    int col = i % col_size;
    if (col == 0) {
      continue;
    } else if (row == 0) {
      offset[i] += offset[i - 1];
      offset_sum[i] = offset[i];
    } else {
      offset_sum[i] = offset[i] + offset_sum[i - 1] + offset_sum[i - col_size] -
                      offset_sum[i - col_size - 1];
      offset[i] += offset[i - 1];
    }
  }

  Tensor offset_gpu;
  size_t* offset_gpu_data =
      reinterpret_cast<size_t*>(offset_gpu.mutable_data<int64_t>(
          {static_cast<int64_t>(offset_size), 1}, this->GetPlace()));
  cudaMemcpy(fea_list_gpu, fea_list_cpu, total_size * sizeof(FeatureItem),
             cudaMemcpyHostToDevice);
  cudaMemcpy(offset_gpu_data, offset_sum.data(), sizeof(size_t) * offset_size,
             cudaMemcpyHostToDevice);

  std::vector<void*> dest_cpu_p(row_size, nullptr);

  for (size_t i = 0; i < use_slots_.size(); ++i) {
    if (feed_vec_[i] == nullptr) {
      continue;
    }
    int total_instance = offset[(i + 1) * col_size - 1];
    const auto& type = all_slots_type_[i];
    if (type[0] == 'f') {  // float
      float* tensor_ptr =
          feed_vec_[i]->mutable_data<float>({total_instance, 1}, this->place_);
      dest_cpu_p[index_map[i]] = static_cast<void*>(tensor_ptr);
    } else if (type[0] == 'u') {
      // printf("slot[%d]: total feanum[%d]\n", (int)i, (int)total_instance);
      int64_t* tensor_ptr = feed_vec_[i]->mutable_data<int64_t>(
          {total_instance, 1}, this->place_);
      dest_cpu_p[index_map[i]] = static_cast<void*>(tensor_ptr);
    }
  }
  auto buf = memory::AllocShared(this->GetPlace(), row_size * sizeof(void*));
  auto type_buf =
      memory::AllocShared(this->GetPlace(), row_size * sizeof(char));
  void** dest_gpu_p = reinterpret_cast<void**>(buf->ptr());
  char* type_gpu_p = reinterpret_cast<char*>(type_buf->ptr());
  cudaMemcpy(dest_gpu_p, dest_cpu_p.data(), row_size * sizeof(void*),
             cudaMemcpyHostToDevice);
  cudaMemcpy(type_gpu_p, slot_type.data(), row_size * sizeof(char),
             cudaMemcpyHostToDevice);

  CopyForTensor(this->GetPlace(), fea_list_gpu, dest_gpu_p, offset_gpu_data,
                type_gpu_p, total_size, row_size, col_size);

  std::vector<size_t> lodinfo(col_size, 0);
  for (size_t i = 0; i < use_slots_.size(); ++i) {
    memcpy(lodinfo.data(), offset.data() + index_map[i] * col_size,
           col_size * sizeof(size_t));
    LoD lod{lodinfo};
    feed_vec_[i]->set_lod(lod);
    if (use_slots_is_dense_[i]) {
      if (inductive_shape_index_[i] != -1) {
        use_slots_shape_[i][inductive_shape_index_[i]] =
            offset[(index_map[i] + 1) * col_size - 1] /
            total_dims_without_inductive_[i];
      }
      feed_vec_[i]->Resize(framework::make_ddim(use_slots_shape_[i]));
    }
  }
  timer.Pause();
// VLOG(0) << "UPDATE_PUT:" << timer.ElapsedUS();
#endif
#endif
}

#if defined(PADDLE_WITH_CUDA) && !defined(_WIN32)
template <typename T>
void PrivateInstantDataFeed<T>::PutToFeedVec() {
  for (size_t i = 0; i < use_slots_.size(); ++i) {
    const auto& type = ins_vec_[i].GetType();
    const auto& offset = ins_vec_[i].GetOffset();
    int total_instance = static_cast<int>(offset.back());

    if (type[0] == 'f') {  // float
      const auto& feasign = ins_vec_[i].GetFloatData();
      float* tensor_ptr =
          feed_vec_[i]->mutable_data<float>({total_instance, 1}, this->place_);
      CopyToFeedTensor(tensor_ptr, &feasign[0], total_instance * sizeof(float));
    } else if (type[0] == 'u') {  // uint64
      // no uint64_t type in paddlepaddle
      const auto& feasign = ins_vec_[i].GetUint64Data();
      int64_t* tensor_ptr = feed_vec_[i]->mutable_data<int64_t>(
          {total_instance, 1}, this->place_);
      CopyToFeedTensor(tensor_ptr, &feasign[0],
                       total_instance * sizeof(int64_t));
    }

    LoD data_lod{offset};
    feed_vec_[i]->set_lod(data_lod);
    if (use_slots_is_dense_[i]) {
      int64_t total_dims = 1;
      for (const auto e : use_slots_shape_[i]) {
        total_dims *= e;
      }
      PADDLE_ENFORCE(
          total_dims == total_instance,
          "The actual data size of slot[%s] doesn't match its declaration",
          use_slots_[i].c_str());
      feed_vec_[i]->Resize(framework::make_ddim(use_slots_shape_[i]));
    }
  }
}

template <typename T>
int PrivateInstantDataFeed<T>::Next() {
  if (ParseOneMiniBatch()) {
    PutToFeedVec();
    return ins_vec_[0].GetBatchSize();
  }
  Postprocess();

  std::string filename;
  if (!PickOneFile(&filename)) {
    return -1;
  }
  if (!Preprocess(filename)) {
    return -1;
  }

  PADDLE_ENFORCE(true == ParseOneMiniBatch(), "Fail to parse mini-batch data");
  PutToFeedVec();
  return ins_vec_[0].GetBatchSize();
}

template <typename T>
void PrivateInstantDataFeed<T>::Init(const DataFeedDesc& data_feed_desc) {
  finish_init_ = false;
  finish_set_filelist_ = false;
  finish_start_ = false;

  PADDLE_ENFORCE(data_feed_desc.has_multi_slot_desc(),
                 "Multi_slot_desc has not been set.");
  paddle::framework::MultiSlotDesc multi_slot_desc =
      data_feed_desc.multi_slot_desc();
  SetBatchSize(data_feed_desc.batch_size());
  size_t all_slot_num = multi_slot_desc.slots_size();
  all_slots_.resize(all_slot_num);
  all_slots_type_.resize(all_slot_num);
  use_slots_index_.resize(all_slot_num);
  multi_inductive_shape_index_.resize(all_slot_num);
  use_slots_.clear();
  use_slots_is_dense_.clear();
  for (size_t i = 0; i < all_slot_num; ++i) {
    const auto& slot = multi_slot_desc.slots(i);
    all_slots_[i] = slot.name();
    all_slots_type_[i] = slot.type();
    use_slots_index_[i] = slot.is_used() ? use_slots_.size() : -1;
    if (slot.is_used()) {
      use_slots_.push_back(all_slots_[i]);
      use_slots_is_dense_.push_back(slot.is_dense());
      std::vector<int> local_shape;
      if (slot.is_dense()) {
        for (int j = 0; j < slot.shape_size(); ++j) {
          if (slot.shape(j) == -1) {
            multi_inductive_shape_index_[i].push_back(j);
          }
        }
      }
      for (int j = 0; j < slot.shape_size(); ++j) {
        local_shape.push_back(slot.shape(j));
      }
      use_slots_shape_.push_back(local_shape);
    }
  }
  feed_vec_.resize(use_slots_.size());
  ins_vec_.resize(use_slots_.size());

  finish_init_ = true;
}

template class PrivateInstantDataFeed<std::vector<MultiSlotType>>;

bool MultiSlotFileInstantDataFeed::Preprocess(const std::string& filename) {
  fd_ = open(filename.c_str(), O_RDONLY);
  PADDLE_ENFORCE(fd_ != -1, "Fail to open file: %s", filename.c_str());

  struct stat sb;
  fstat(fd_, &sb);
  end_ = static_cast<size_t>(sb.st_size);

  buffer_ =
      reinterpret_cast<char*>(mmap(NULL, end_, PROT_READ, MAP_PRIVATE, fd_, 0));
  PADDLE_ENFORCE(buffer_ != MAP_FAILED, strerror(errno));

  offset_ = 0;
  return true;
}

bool MultiSlotFileInstantDataFeed::Postprocess() {
  if (buffer_ != nullptr) {
    munmap(buffer_, end_);
    buffer_ = nullptr;
  }
  if (fd_ != -1) {
    close(fd_);
    fd_ = -1;
    end_ = 0;
    offset_ = 0;
  }
  return true;
}

bool MultiSlotFileInstantDataFeed::ParseOneMiniBatch() {
  if (offset_ == end_) {
    return false;
  }

  batch_size_ = 0;
  while (batch_size_ < default_batch_size_ && offset_ < end_) {
    for (size_t i = 0; i < use_slots_index_.size(); ++i) {
      int idx = use_slots_index_[i];
      char type = all_slots_type_[i][0];

      uint16_t num = *reinterpret_cast<uint16_t*>(buffer_ + offset_);
      PADDLE_ENFORCE(
          num,
          "The number of ids can not be zero, you need padding "
          "it in data generator; or if there is something wrong with "
          "the data, please check if the data contains unresolvable "
          "characters.");
      offset_ += sizeof(uint16_t);

      if (idx != -1) {
        int inductive_size = multi_inductive_shape_index_[i].size();
        if (UNLIKELY(batch_size_ == 0)) {
          ins_vec_[idx].Init(all_slots_type_[i], default_batch_size_ * num);
          ins_vec_[idx].InitOffset(default_batch_size_);
          uint64_t* inductive_shape =
              reinterpret_cast<uint64_t*>(buffer_ + offset_);
          for (int inductive_id = 0; inductive_id < inductive_size;
               ++inductive_id) {
            use_slots_shape_[i][multi_inductive_shape_index_[i][inductive_id]] =
                static_cast<int>(*(inductive_shape + inductive_id));
          }
        }
        num -= inductive_size;
        offset_ += sizeof(uint64_t) * inductive_size;

        if (type == 'f') {
          ins_vec_[idx].AppendValues(
              reinterpret_cast<float*>(buffer_ + offset_), num);
          offset_ += num * sizeof(float);
        } else if (type == 'u') {
          ins_vec_[idx].AppendValues(
              reinterpret_cast<uint64_t*>(buffer_ + offset_), num);
          offset_ += num * sizeof(uint64_t);
        }
      } else {
        if (type == 'f') {
          offset_ += num * sizeof(float);
        } else if (type == 'u') {
          offset_ += num * sizeof(uint64_t);
        }
      }
    }
    ++batch_size_;
    // OPTIMIZE: It is better to insert check codes between instances for format
    // checking
  }

  PADDLE_ENFORCE(batch_size_ == default_batch_size_ || offset_ == end_,
                 "offset_ != end_");
  return true;
}
#endif

bool PaddleBoxDataFeed::Start() {
#ifdef _LINUX
  VLOG(0) << "Begin Start";
  int phase = GetCurrentPhase();  // join: 1, update: 0
  this->CheckSetFileList();
  if (enable_pv_merge_ && phase == 1) {
    // join phase : input_pv_channel to output_pv_channel
    if (output_pv_channel_->Size() == 0 && input_pv_channel_->Size() != 0) {
      std::vector<PvInstance> data;
      input_pv_channel_->Read(data);
      output_pv_channel_->Write(std::move(data));
    }
  } else {
    // input_channel to output
    if (output_channel_->Size() == 0 && input_channel_->Size() != 0) {
      std::vector<Record> data;
      input_channel_->Read(data);
      output_channel_->Write(std::move(data));
    }
    if (output_ptr_channel_->Size() == 0 && input_ptr_channel_->Size() != 0) {
      std::vector<Record*> data;
      input_ptr_channel_->Read(data);
      output_ptr_channel_->Write(std::move(data));
      VLOG(0) << "output_ptr_channel size: " << output_ptr_channel_->Size();
    }
  }
  pv_vec.reserve(default_batch_size_);
#endif
  this->finish_start_ = true;
  return true;
}

int PaddleBoxDataFeed::Next() {
#ifdef _LINUX
  Timer next_timer;
  next_timer.Resume();
  int phase = GetCurrentPhase();  // join: 1, update: 0
  this->CheckStart();
  platform::Timer lock_timer;
  if (enable_pv_merge_ && phase == 1) {
    // join phase : output_pv_channel to consume_pv_channel
    CHECK(output_pv_channel_ != nullptr);
    CHECK(consume_pv_channel_ != nullptr);
    VLOG(3) << "output_pv_channel_ size=" << output_pv_channel_->Size()
            << ", consume_pv_channel_ size=" << consume_pv_channel_->Size()
            << ", thread_id=" << thread_id_;
    int index = 0;
    PvInstance pv_instance;
    // std::vector<PvInstance> pv_vec;
    // pv_vec.reserve(this->pv_batch_size_);
    pv_vec.clear();
    while (index < this->pv_batch_size_) {
      if (output_pv_channel_->Size() == 0) {
        break;
      }

      output_pv_channel_->GetUnlock(pv_instance);
      lock_timer.Resume();
      pv_vec.push_back(pv_instance);
      lock_timer.Pause();
      ++index;
      consume_pv_channel_->PutUnlock(std::move(pv_instance));
    }
    this->batch_size_ = index;
    VLOG(3) << "pv_batch_size_=" << this->batch_size_
            << ", thread_id=" << thread_id_;
    next_timer.Pause();
    // VLOG(0) << "JOIN_NEXT:" << next_timer.ElapsedUS();
    if (this->batch_size_ != 0) {
      PutToFeedVec(pv_vec);
    } else {
      VLOG(3) << "finish reading, output_pv_channel_ size="
              << output_pv_channel_->Size()
              << ", consume_pv_channel_ size=" << consume_pv_channel_->Size()
              << ", thread_id=" << thread_id_;
    }
    // VLOG(0) << "JOINPUSHBACK:" << lock_timer.ElapsedUS();
  } else {
    Timer next_timer;
    next_timer.Resume();
    this->CheckStart();
    CHECK(output_channel_ != nullptr);
    CHECK(consume_channel_ != nullptr);
    VLOG(3) << "output_channel_ size=" << output_channel_->Size()
            << ", consume_channel_ size=" << consume_channel_->Size()
            << ", thread_id=" << thread_id_;
    int index = 0;
    Record* instance;
    // std::vector<T> ins_vec;
    ins_vec.clear();
    // ins_vec.reserve(this->default_batch_size_);
    platform::Timer lock_timer;
    while (index < this->default_batch_size_) {
      if (output_ptr_channel_->Size() == 0) {
        break;
      }
      output_ptr_channel_->GetUnlock(instance);
      lock_timer.Resume();
      ins_vec.push_back(instance);
      lock_timer.Pause();
      ++index;
      consume_ptr_channel_->PutUnlock(std::move(instance));
    }
    this->batch_size_ = index;
    VLOG(3) << "batch_size_=" << this->batch_size_
            << ", thread_id=" << thread_id_;
    next_timer.Pause();
    // VLOG(0) << "UPDATE_NEXT:" << next_timer.ElapsedUS();
    if (this->batch_size_ != 0) {
      PutToFeedVec(ins_vec);
    } else {
      VLOG(3) << "finish reading, output_channel_ size="
              << output_channel_->Size()
              << ", consume_channel_ size=" << consume_channel_->Size()
              << ", thread_id=" << thread_id_;
    }
    // VLOG(0) << "UPDATEPUSHBACK:" << lock_timer.ElapsedUS();
    return this->batch_size_;
  }
  return this->batch_size_;
#else
  return 0;
#endif
}

void PaddleBoxDataFeed::Init(const DataFeedDesc& data_feed_desc) {
  MultiSlotInMemoryDataFeed::Init(data_feed_desc);
  rank_offset_name_ = data_feed_desc.rank_offset();
  pv_batch_size_ = data_feed_desc.pv_batch_size();
}

void PaddleBoxDataFeed::GetRankOffset(const std::vector<PvInstance>& pv_vec,
                                      int ins_number) {
  int index = 0;
  int max_rank = 3;  // the value is setting
  int row = ins_number;
  int col = max_rank * 2 + 1;
  int pv_num = pv_vec.size();

  std::vector<int> rank_offset_mat(row * col, -1);
  rank_offset_mat.shrink_to_fit();

  for (int i = 0; i < pv_num; i++) {
    auto pv_ins = pv_vec[i];
    int ad_num = pv_ins->ads.size();
    int index_start = index;
    for (int j = 0; j < ad_num; ++j) {
      auto ins = pv_ins->ads[j];
      int rank = -1;
      if ((ins->cmatch == 222 || ins->cmatch == 223) &&
          ins->rank <= static_cast<uint32_t>(max_rank) && ins->rank != 0) {
        rank = ins->rank;
      }

      rank_offset_mat[index * col] = rank;
      if (rank > 0) {
        for (int k = 0; k < ad_num; ++k) {
          auto cur_ins = pv_ins->ads[k];
          int fast_rank = -1;
          if ((cur_ins->cmatch == 222 || cur_ins->cmatch == 223) &&
              cur_ins->rank <= static_cast<uint32_t>(max_rank) &&
              cur_ins->rank != 0) {
            fast_rank = cur_ins->rank;
          }

          if (fast_rank > 0) {
            int m = fast_rank - 1;
            rank_offset_mat[index * col + 2 * m + 1] = cur_ins->rank;
            rank_offset_mat[index * col + 2 * m + 2] = index_start + k;
          }
        }
      }
      index += 1;
    }
  }

  int* rank_offset = rank_offset_mat.data();
  int* tensor_ptr = rank_offset_->mutable_data<int>({row, col}, this->place_);
  CopyToFeedTensor(tensor_ptr, rank_offset, row * col * sizeof(int));
}

void PaddleBoxDataFeed::AssignFeedVar(const Scope& scope) {
  MultiSlotInMemoryDataFeed::AssignFeedVar(scope);
  // set rank offset memory
  int phase = GetCurrentPhase();  // join: 1, update: 0
  if (enable_pv_merge_ && phase == 1) {
    rank_offset_ = scope.FindVar(rank_offset_name_)->GetMutable<LoDTensor>();
  }
}

void PaddleBoxDataFeed::PutToFeedVec(const std::vector<PvInstance>& pv_vec) {
#ifdef _LINUX
  Timer timer;
  timer.Resume();
  int ins_number = 0;
  std::vector<Record*> ins_vec;
  for (auto& pv : pv_vec) {
    ins_number += pv->ads.size();
    for (auto ins : pv->ads) {
      ins_vec.push_back(ins);
    }
  }
  GetRankOffset(pv_vec, ins_number);
  timer.Pause();
  // VLOG(0) << "JOIN_RANK:" << timer.ElapsedUS();
  PutToFeedVec(ins_vec);

#endif
}

int PaddleBoxDataFeed::GetCurrentPhase() {
#ifdef PADDLE_WITH_BOX_PS
  auto box_ptr = paddle::framework::BoxWrapper::GetInstance();
  return box_ptr->PassFlag();  // join: 1, update: 0
#else
  LOG(WARNING) << "It should be complied with BOX_PS...";
  return current_phase_;
#endif
}

void PaddleBoxDataFeed::PutToFeedVec(const std::vector<Record*>& ins_vec) {
#if defined(PADDLE_WITH_CUDA) && defined(_LINUX)
  ins_content_vec_.clear();
  ins_content_vec_.reserve(ins_vec.size());
  ins_id_vec_.clear();
  ins_id_vec_.reserve(ins_vec.size());
  for (size_t i = 0; i < ins_vec.size(); ++i) {
    auto& r = ins_vec[i];
    ins_id_vec_.push_back(r->ins_id_);
    ins_content_vec_.push_back(r->content_);
  }
  Timer timer;
  timer.Resume();

  Timer t1;
  Timer t2;
  Timer t3;
  Timer t4;
  t1.Resume();
  paddle::platform::SetDeviceId(
      boost::get<platform::CUDAPlace>(this->GetPlace()).GetDeviceId());
  std::vector<size_t> ins_len(ins_vec.size(), 0);  // prefix sum of ins length
  size_t total_size = 0;
  for (size_t i = 0; i < ins_vec.size(); ++i) {
    auto& r = ins_vec[i];
    total_size += r->uint64_feasigns_.size() + r->float_feasigns_.size();
    ins_len[i] = total_size;
  }

  // fprintf(stderr, "totol size: %lu\n", total_size);
  auto cpu_buf = memory::AllocShared(platform::CPUPlace(),
                                     total_size * sizeof(FeatureItem));
  auto gpu_buf =
      memory::AllocShared(this->GetPlace(), total_size * sizeof(FeatureItem));
  FeatureItem* fea_list_cpu = reinterpret_cast<FeatureItem*>(cpu_buf->ptr());
  FeatureItem* fea_list_gpu = reinterpret_cast<FeatureItem*>(gpu_buf->ptr());
  t1.Pause();

  Timer t21;
  Timer t22;
  Timer t23;
  Timer t24;
  t2.Resume();
  t21.Resume();
  size_t size_off = 0;
  for (size_t i = 0; i < ins_vec.size(); ++i) {
    auto& r = ins_vec[i];
    memcpy(fea_list_cpu + size_off, r->uint64_feasigns_.data(),
           sizeof(FeatureItem) * r->uint64_feasigns_.size());
    size_off += r->uint64_feasigns_.size();
    memcpy(fea_list_cpu + size_off, r->float_feasigns_.data(),
           sizeof(FeatureItem) * r->float_feasigns_.size());
    size_off += r->float_feasigns_.size();
  }
  t21.Pause();
  t22.Resume();

  size_t row_size = use_slots_.size();       // slot_number
  size_t col_size = 1 + ins_vec.size();      // 1 + batch_size
  size_t offset_size = row_size * col_size;  // length of lod array
  // printf("row_size: %d, col_size: %d\n", (int)row_size, (int)col_size);

  size_t uslot_num = 0;  // slot number of uint64_t type
  std::vector<size_t> index_map(
      row_size);  // make index map from origin slot id to actual slot id
  std::vector<char> slot_type(row_size, 'u');  // Record actual slot type

  for (size_t i = 0; i < row_size; ++i) {
    const auto& type = all_slots_type_[i];
    if (type[0] == 'u') {
      uslot_num++;
    }
  }
  for (size_t i = uslot_num; i < row_size; ++i) {
    slot_type[i] = 'f';
  }
  int u_slot_id = 0;
  for (size_t i = 0; i < use_slots_.size(); ++i) {
    const auto& type = all_slots_type_[i];
    if (type[0] == 'f') {  // float
      index_map[i] = uslot_num++;
    } else if (type[0] == 'u') {
      index_map[i] = u_slot_id++;
    }
  }

  t22.Pause();
  t23.Resume();
  // std::vector<size_t> offset(offset_size, 0);
  // std::vector<size_t> offset_sum(offset_size, 0);
  offset.clear();
  offset_sum.clear();
  offset.resize(offset_size, 0);
  offset_sum.resize(offset_size, 0);

  // construct lod info
  int col = 1;
  for (size_t i = 0; i < total_size; ++i) {
    if (i >= ins_len[col - 1]) {
      col++;
    }
    offset[index_map[fea_list_cpu[i].slot()] * col_size + col]++;
  }
  t23.Pause();
  t24.Resume();
  // compute sum-lod info, for using kernel to make batch
  for (size_t i = 0; i < offset.size(); ++i) {
    int row = i / col_size;
    int col = i % col_size;
    if (col == 0) {
      continue;
    } else if (row == 0) {
      offset[i] += offset[i - 1];
      offset_sum[i] = offset[i];
    } else {
      offset_sum[i] = offset[i] + offset_sum[i - 1] + offset_sum[i - col_size] -
                      offset_sum[i - col_size - 1];
      offset[i] += offset[i - 1];
    }
  }
  t24.Pause();
  t2.Pause();
  t3.Resume();
  Tensor offset_gpu;
  size_t* offset_gpu_data =
      reinterpret_cast<size_t*>(offset_gpu.mutable_data<int64_t>(
          {static_cast<int64_t>(offset_size), 1}, this->GetPlace()));
  cudaMemcpy(fea_list_gpu, fea_list_cpu, total_size * sizeof(FeatureItem),
             cudaMemcpyHostToDevice);
  cudaMemcpy(offset_gpu_data, offset_sum.data(), sizeof(size_t) * offset_size,
             cudaMemcpyHostToDevice);

  std::vector<void*> dest_cpu_p(row_size, nullptr);

  for (size_t i = 0; i < use_slots_.size(); ++i) {
    if (feed_vec_[i] == nullptr) {
      continue;
    }
    int total_instance = offset[(i + 1) * col_size - 1];
    const auto& type = all_slots_type_[i];
    if (type[0] == 'f') {  // float
      float* tensor_ptr =
          feed_vec_[i]->mutable_data<float>({total_instance, 1}, this->place_);
      dest_cpu_p[index_map[i]] = static_cast<void*>(tensor_ptr);
    } else if (type[0] == 'u') {
      // printf("slot[%d]: total feanum[%d]\n", (int)i, (int)total_instance);
      int64_t* tensor_ptr = feed_vec_[i]->mutable_data<int64_t>(
          {total_instance, 1}, this->place_);
      dest_cpu_p[index_map[i]] = static_cast<void*>(tensor_ptr);
    }
  }
  // fprintf(stderr, "after create tensor\n");
  auto buf = memory::AllocShared(this->GetPlace(), row_size * sizeof(void*));
  auto type_buf =
      memory::AllocShared(this->GetPlace(), row_size * sizeof(char));
  void** dest_gpu_p = reinterpret_cast<void**>(buf->ptr());
  char* type_gpu_p = reinterpret_cast<char*>(type_buf->ptr());
  cudaMemcpy(dest_gpu_p, dest_cpu_p.data(), row_size * sizeof(void*),
             cudaMemcpyHostToDevice);
  cudaMemcpy(type_gpu_p, slot_type.data(), row_size * sizeof(char),
             cudaMemcpyHostToDevice);

  t3.Pause();
  t4.Resume();
  CopyForTensor(this->GetPlace(), fea_list_gpu, dest_gpu_p, offset_gpu_data,
                type_gpu_p, total_size, row_size, col_size);

  std::vector<size_t> lodinfo(col_size, 0);
  for (size_t i = 0; i < use_slots_.size(); ++i) {
    memcpy(lodinfo.data(), offset.data() + index_map[i] * col_size,
           col_size * sizeof(size_t));
    LoD lod{lodinfo};
    feed_vec_[i]->set_lod(lod);
    if (use_slots_is_dense_[i]) {
      if (inductive_shape_index_[i] != -1) {
        use_slots_shape_[i][inductive_shape_index_[i]] =
            offset[(index_map[i] + 1) * col_size - 1] /
            total_dims_without_inductive_[i];
      }
      feed_vec_[i]->Resize(framework::make_ddim(use_slots_shape_[i]));
    }
  }
  t4.Pause();
  timer.Pause();
/*
  VLOG(0) << "JOIN_PUT:" << timer.ElapsedUS();
  VLOG(0) << "JOIN_PUT_PROFILE:" << t1.ElapsedUS() << ":" << t2.ElapsedUS()
          << ":" << t3.ElapsedUS() << ":" << t4.ElapsedUS();
  VLOG(0) << "JOIN_PUT_PROFILE_FINE:" << t21.ElapsedUS() << ":"
          << t22.ElapsedUS() << ":" << t23.ElapsedUS() << ":"
          << t24.ElapsedUS();
*/
#endif
}

}  // namespace framework
}  // namespace paddle
