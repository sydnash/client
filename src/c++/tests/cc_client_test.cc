// Copyright 2022-2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <fstream>

#define TRITON_INFERENCE_SERVER_CLIENT_CLASS InferenceServerHttpClient
#include "grpc_client.h"
#include "gtest/gtest.h"
#include "http_client.cc"
#include "http_client.h"

namespace tc = triton::client;

namespace {

// This test must be run with a running Triton server,
// check L0_grpc in server repo for the setup.
template <typename ClientType>
class ClientTest : public ::testing::Test {
 public:
  ClientTest()
      : model_name_("onnx_int32_int32_int32"), shape_{1, 16}, dtype_("INT32")
  {
  }

  void SetUp() override
  {
    std::string url;
    if (std::is_same<ClientType, tc::InferenceServerGrpcClient>::value) {
      url = "localhost:8001";
    } else if (std::is_same<ClientType, tc::InferenceServerHttpClient>::value) {
      url = "localhost:8000";
    } else {
      ASSERT_TRUE(false) << "Unrecognized client class type '"
                         << typeid(ClientType).name() << "'";
    }
    auto err = ClientType::Create(&this->client_, url);
    ASSERT_TRUE(err.IsOk())
        << "failed to create GRPC client: " << err.Message();

    // Initialize 3 sets of inputs, each with 16 elements
    for (size_t i = 0; i < 3; ++i) {
      this->input_data_.emplace_back();
      for (size_t j = 0; j < 16; ++j) {
        this->input_data_.back().emplace_back(i * 16 + j);
      }
    }
  }

  tc::Error PrepareInputs(
      const std::vector<int32_t>& input_0, const std::vector<int32_t>& input_1,
      std::vector<tc::InferInput*>* inputs)
  {
    inputs->emplace_back();
    auto err = tc::InferInput::Create(
        &inputs->back(), "INPUT0", this->shape_, this->dtype_);
    if (!err.IsOk()) {
      return err;
    }
    err = inputs->back()->AppendRaw(
        reinterpret_cast<const uint8_t*>(input_0.data()),
        input_0.size() * sizeof(int32_t));
    if (!err.IsOk()) {
      return err;
    }
    inputs->emplace_back();
    err = tc::InferInput::Create(
        &inputs->back(), "INPUT1", this->shape_, this->dtype_);
    if (!err.IsOk()) {
      return err;
    }
    err = inputs->back()->AppendRaw(
        reinterpret_cast<const uint8_t*>(input_1.data()),
        input_1.size() * sizeof(int32_t));
    if (!err.IsOk()) {
      return err;
    }
    return tc::Error::Success;
  }

  void ValidateOutput(
      const std::vector<tc::InferResult*>& results,
      const std::vector<std::map<std::string, std::vector<int32_t>>>&
          expected_outputs)
  {
    ASSERT_EQ(results.size(), expected_outputs.size())
        << "unexpected number of results";
    for (size_t i = 0; i < results.size(); ++i) {
      ASSERT_TRUE(results[i]->RequestStatus().IsOk());
      for (const auto& expected : expected_outputs[i]) {
        const uint8_t* buf = nullptr;
        size_t byte_size = 0;
        auto err = results[i]->RawData(expected.first, &buf, &byte_size);
        ASSERT_TRUE(err.IsOk())
            << "failed to retrieve output '" << expected.first
            << "' for result " << i << ": " << err.Message();
        ASSERT_EQ(byte_size, (expected.second.size() * sizeof(int32_t)));
        EXPECT_EQ(memcmp(buf, expected.second.data(), byte_size), 0);
      }
    }
  }

  tc::Error LoadModel(
      const std::string& model_name, const std::string& config,
      const std::map<std::string, std::vector<char>>& files = {});

  std::string model_name_;
  std::unique_ptr<ClientType> client_;
  std::vector<std::vector<int32_t>> input_data_;
  std::vector<int64_t> shape_;
  std::string dtype_;
};

template <>
tc::Error
ClientTest<tc::InferenceServerGrpcClient>::LoadModel(
    const std::string& model_name, const std::string& config,
    const std::map<std::string, std::vector<char>>& files)
{
  return this->client_->LoadModel(model_name, tc::Headers(), config, files);
}

template <>
tc::Error
ClientTest<tc::InferenceServerHttpClient>::LoadModel(
    const std::string& model_name, const std::string& config,
    const std::map<std::string, std::vector<char>>& files)
{
  return this->client_->LoadModel(
      model_name, tc::Headers(), tc::Parameters(), config, files);
}

class HTTPTraceTest : public ::testing::Test {
 public:
  HTTPTraceTest() : model_name_("simple") {}

  void SetUp() override
  {
    std::string url;
    url = "localhost:8000";
    auto err = tc::InferenceServerHttpClient::Create(&client_, url);
    ASSERT_TRUE(err.IsOk())
        << "failed to create HTTP client: " << err.Message();
  }

  // Helper function to clear all the trace settings to initial state.
  void TearDown()
  {
    tc::Error err = tc::Error::Success;
    std::string response;

    std::map<std::string, std::vector<std::string>> clear_settings = {
        {"trace_level", {}},
        {"trace_rate", {}},
        {"trace_count", {}},
        {"log_frequency", {}}};

    err = client_->UpdateTraceSettings(&response, model_name_, clear_settings);
    ASSERT_TRUE(err.IsOk())
        << "unable to update trace settings: " << err.Message();
    err = client_->UpdateTraceSettings(&response, "", clear_settings);
    ASSERT_TRUE(err.IsOk())
        << "unable to update trace settings: " << err.Message();
  }

  // Helper function to make sure the trace setting is properly initialized /
  // reset before actually running the test case.
  void CheckServerInitialState()
  {
    tc::Error err = tc::Error::Success;
    std::string trace_settings;

    std::string initial_settings =
        "{\"trace_level\":[\"TIMESTAMPS\"],\"trace_rate\":\"1\",\"trace_"
        "count\":\"-1\",\"log_frequency\":\"0\",\"trace_file\":\"global_"
        "unittest.log\",\"trace_mode\":\"triton\"}";

    err = client_->GetTraceSettings(&trace_settings, model_name_);
    ASSERT_TRUE(err.IsOk())
        << "unable to get trace settings: " << err.Message();
    ASSERT_EQ(trace_settings, initial_settings)
        << "error: trace settings is not properly initialized for model'"
        << model_name_ << "'" << std::endl;

    err = client_->GetTraceSettings(&trace_settings, "");
    ASSERT_TRUE(err.IsOk())
        << "unable to get default trace settings: " << err.Message();
    ASSERT_EQ(trace_settings, initial_settings)
        << "error: default trace settings is not properly initialized"
        << std::endl;
  }

  std::string model_name_;
  std::unique_ptr<tc::InferenceServerHttpClient> client_;
};

class GRPCTraceTest : public ::testing::Test {
 public:
  GRPCTraceTest() : model_name_("simple") {}

  void SetUp() override
  {
    std::string url;
    url = "localhost:8001";
    auto err = tc::InferenceServerGrpcClient::Create(&this->client_, url);
    ASSERT_TRUE(err.IsOk())
        << "failed to create GRPC client: " << err.Message();
  }
  // Helper function to convert 'inference::TraceSettingResponse response' to a
  // string
  void ConvertResponse(
      const inference::TraceSettingResponse& response, std::string* str)
  {
    *str = response.DebugString();
    str->erase(std::remove(str->begin(), str->end(), ' '), str->end());
    str->erase(std::remove(str->begin(), str->end(), '\n'), str->end());
  }

  // Helper function to clear all the trace settings to initial state.
  void TearDown()
  {
    tc::Error err = tc::Error::Success;
    inference::TraceSettingResponse response;

    std::map<std::string, std::vector<std::string>> clear_settings = {
        {"trace_level", {}},
        {"trace_rate", {}},
        {"trace_count", {}},
        {"log_frequency", {}}};

    err = client_->UpdateTraceSettings(&response, model_name_, clear_settings);
    ASSERT_TRUE(err.IsOk())
        << "unable to update trace settings: " << err.Message();
    err = client_->UpdateTraceSettings(&response, "", clear_settings);
    ASSERT_TRUE(err.IsOk())
        << "unable to update trace settings: " << err.Message();
  }

  // Helper function to make sure the trace setting is properly initialized /
  // reset before actually running the test case.
  void CheckServerInitialState()
  {
    tc::Error err = tc::Error::Success;
    inference::TraceSettingResponse response;
    std::string trace_settings;

    std::string initial_settings =
        "settings{key:\"log_frequency\"value{value:\"0\"}}settings{key:\"trace_"
        "count\"value{value:\"-1\"}}settings{key:\"trace_file\"value{value:"
        "\"global_unittest.log\"}}settings{key:\"trace_level\"value{value:"
        "\"TIMESTAMPS\"}}settings{key:\"trace_mode\"value{value:\"triton\"}}"
        "settings{key:\"trace_rate\"value{value:\"1\"}}";
    err = client_->GetTraceSettings(&response, model_name_);
    ASSERT_TRUE(err.IsOk())
        << "unable to get trace settings: " << err.Message();
    EXPECT_NO_FATAL_FAILURE(ConvertResponse(response, &trace_settings));
    ASSERT_EQ(trace_settings, initial_settings)
        << "error: trace settings is not properly initialized for model'"
        << model_name_ << "'" << std::endl;

    err = client_->GetTraceSettings(&response, "");
    ASSERT_TRUE(err.IsOk())
        << "unable to get default trace settings: " << err.Message();
    EXPECT_NO_FATAL_FAILURE(ConvertResponse(response, &trace_settings));
    ASSERT_EQ(trace_settings, initial_settings)
        << "error: default trace settings is not properly initialized"
        << std::endl;
  }

  std::string model_name_;
  std::unique_ptr<tc::InferenceServerGrpcClient> client_;
};


TYPED_TEST_SUITE_P(ClientTest);

TYPED_TEST_P(ClientTest, InferMulti)
{
  tc::Error err = tc::Error::Success;
  // Create 3 sets of 'options', 'inputs', 'outputs', technically
  // only InferInput can not be reused for requests that are sent
  // concurrently, here use distinct objects for all 'options',
  // 'inputs', and 'outputs' for simplicity.
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    // Not swap
    options.back().model_version_ = "1";

    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);

    expected_outputs.emplace_back();
    {
      auto& expected = expected_outputs.back()["OUTPUT0"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] + input_1[i]);
      }
    }
    {
      auto& expected = expected_outputs.back()["OUTPUT1"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] - input_1[i]);
      }
    }
  }

  std::vector<tc::InferResult*> results;
  err = this->client_->InferMulti(&results, options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, InferMultiDifferentOutputs)
{
  tc::Error err = tc::Error::Success;
  // Create 3 sets of 'options', 'inputs', 'outputs', technically
  // only InferInput can not be reused for requests that are sent
  // concurrently, here use distinct objects for all 'options',
  // 'inputs', and 'outputs' for simplicity.
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    // Not swap
    options.back().model_version_ = "1";

    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    // Explicitly request different output for different request
    // 0: request 0
    // 1: request 1
    // 2: no request (both will be returned)
    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    expected_outputs.emplace_back();
    if (i != 1) {
      if (i != 2) {
        err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
        ASSERT_TRUE(err.IsOk())
            << "failed to create inference output: " << err.Message();
        outputs.back().emplace_back(output);
      }

      auto& expected = expected_outputs.back()["OUTPUT0"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] + input_1[i]);
      }
    }
    if (i != 0) {
      if (i != 2) {
        err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
        ASSERT_TRUE(err.IsOk())
            << "failed to create inference output: " << err.Message();
        outputs.back().emplace_back(output);
      }

      auto& expected = expected_outputs.back()["OUTPUT1"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] - input_1[i]);
      }
    }
  }

  std::vector<tc::InferResult*> results;
  err = this->client_->InferMulti(&results, options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, InferMultiDifferentOptions)
{
  tc::Error err = tc::Error::Success;
  // Create 3 sets of 'options', 'inputs', 'outputs', technically
  // only InferInput can not be reused for requests that are sent
  // concurrently, here use distinct objects for all 'options',
  // 'inputs', and 'outputs' for simplicity.
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    // output will be different based on version
    // v1 : not swap
    // v2 : swap
    // v3 : swap
    size_t version = (i % 3) + 1;
    options.back().model_version_ = std::to_string(version);

    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);

    expected_outputs.emplace_back();
    {
      auto& expected =
          expected_outputs.back()[version == 1 ? "OUTPUT0" : "OUTPUT1"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] + input_1[i]);
      }
    }
    {
      auto& expected =
          expected_outputs.back()[version == 1 ? "OUTPUT1" : "OUTPUT0"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] - input_1[i]);
      }
    }
  }

  std::vector<tc::InferResult*> results;
  err = this->client_->InferMulti(&results, options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, InferMultiOneOption)
{
  // Create only 1 sets of 'options'.
  tc::Error err = tc::Error::Success;
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  options.emplace_back(this->model_name_);
  // Not swap
  options.back().model_version_ = "1";
  for (size_t i = 0; i < 3; ++i) {
    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);

    expected_outputs.emplace_back();
    {
      auto& expected = expected_outputs.back()["OUTPUT0"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] + input_1[i]);
      }
    }
    {
      auto& expected = expected_outputs.back()["OUTPUT1"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] - input_1[i]);
      }
    }
  }

  std::vector<tc::InferResult*> results;
  err = this->client_->InferMulti(&results, options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, InferMultiOneOutput)
{
  // Create only 1 sets of 'outputs', but combine with different 'options'
  tc::Error err = tc::Error::Success;
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    // output will be different based on version
    // v1 : not swap
    // v2 : swap
    // v3 : swap
    size_t version = (i % 3) + 1;
    options.back().model_version_ = std::to_string(version);

    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);

    expected_outputs.emplace_back();
    {
      auto& expected = expected_outputs.back()["OUTPUT0"];
      if (version == 1) {
        for (size_t i = 0; i < 16; ++i) {
          expected.emplace_back(input_0[i] + input_1[i]);
        }
      } else {
        for (size_t i = 0; i < 16; ++i) {
          expected.emplace_back(input_0[i] - input_1[i]);
        }
      }
    }
  }

  std::vector<tc::InferResult*> results;
  err = this->client_->InferMulti(&results, options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, InferMultiNoOutput)
{
  // Not specifying 'outputs' at all, but combine with different 'options'
  tc::Error err = tc::Error::Success;
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    // output will be different based on version
    // v1 : not swap
    // v2 : swap
    // v3 : swap
    size_t version = (i % 3) + 1;
    options.back().model_version_ = std::to_string(version);

    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    expected_outputs.emplace_back();
    {
      auto& expected =
          expected_outputs.back()[version == 1 ? "OUTPUT0" : "OUTPUT1"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] + input_1[i]);
      }
    }
    {
      auto& expected =
          expected_outputs.back()[version == 1 ? "OUTPUT1" : "OUTPUT0"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] - input_1[i]);
      }
    }
  }

  std::vector<tc::InferResult*> results;
  err = this->client_->InferMulti(&results, options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, InferMultiMismatchOptions)
{
  // Create mismatch number of 'options'.
  tc::Error err = tc::Error::Success;
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  options.emplace_back(this->model_name_);
  options.emplace_back(this->model_name_);
  for (size_t i = 0; i < 3; ++i) {
    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);
  }

  std::vector<tc::InferResult*> results;
  err = this->client_->InferMulti(&results, options, inputs, outputs);
  ASSERT_FALSE(err.IsOk()) << "Expect InferMulti() to fail";
}

TYPED_TEST_P(ClientTest, InferMultiMismatchOutputs)
{
  // Create mismatch number of 'outputs'.
  tc::Error err = tc::Error::Success;
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    if (i != 2) {
      tc::InferRequestedOutput* output;
      outputs.emplace_back();
      err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
      ASSERT_TRUE(err.IsOk())
          << "failed to create inference output: " << err.Message();
      outputs.back().emplace_back(output);
      err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
      ASSERT_TRUE(err.IsOk())
          << "failed to create inference output: " << err.Message();
      outputs.back().emplace_back(output);
    }
  }

  std::vector<tc::InferResult*> results;
  err = this->client_->InferMulti(&results, options, inputs, outputs);
  ASSERT_FALSE(err.IsOk()) << "Expect InferMulti() to fail";
}

TYPED_TEST_P(ClientTest, AsyncInferMulti)
{
  tc::Error err = tc::Error::Success;
  // Create 3 sets of 'options', 'inputs', 'outputs', technically
  // only InferInput can not be reused for requests that are sent
  // concurrently, here use distinct objects for all 'options',
  // 'inputs', and 'outputs' for simplicity.
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    // Not swap
    options.back().model_version_ = "1";

    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);

    expected_outputs.emplace_back();
    {
      auto& expected = expected_outputs.back()["OUTPUT0"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] + input_1[i]);
      }
    }
    {
      auto& expected = expected_outputs.back()["OUTPUT1"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] - input_1[i]);
      }
    }
  }

  std::vector<tc::InferResult*> results;
  std::condition_variable cv;
  std::mutex mu;
  err = this->client_->AsyncInferMulti(
      [&results, &cv, &mu](std::vector<tc::InferResult*> res) {
        {
          std::lock_guard<std::mutex> lk(mu);
          results.swap(res);
        }
        cv.notify_one();
      },
      options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  std::unique_lock<std::mutex> lk(mu);
  cv.wait(lk, [this, &results] { return !results.empty(); });
  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, AsyncInferMultiDifferentOutputs)
{
  tc::Error err = tc::Error::Success;
  // Create 3 sets of 'options', 'inputs', 'outputs', technically
  // only InferInput can not be reused for requests that are sent
  // concurrently, here use distinct objects for all 'options',
  // 'inputs', and 'outputs' for simplicity.
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    // Not swap
    options.back().model_version_ = "1";

    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    // Explicitly request different output for different request
    // 0: request 0
    // 1: request 1
    // 2: no request (both will be returned)
    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    expected_outputs.emplace_back();
    if (i != 1) {
      if (i != 2) {
        err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
        ASSERT_TRUE(err.IsOk())
            << "failed to create inference output: " << err.Message();
        outputs.back().emplace_back(output);
      }

      auto& expected = expected_outputs.back()["OUTPUT0"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] + input_1[i]);
      }
    }
    if (i != 0) {
      if (i != 2) {
        err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
        ASSERT_TRUE(err.IsOk())
            << "failed to create inference output: " << err.Message();
        outputs.back().emplace_back(output);
      }

      auto& expected = expected_outputs.back()["OUTPUT1"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] - input_1[i]);
      }
    }
  }

  std::vector<tc::InferResult*> results;
  std::condition_variable cv;
  std::mutex mu;
  err = this->client_->AsyncInferMulti(
      [&results, &cv, &mu](std::vector<tc::InferResult*> res) {
        {
          std::lock_guard<std::mutex> lk(mu);
          results.swap(res);
        }
        cv.notify_one();
      },
      options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  std::unique_lock<std::mutex> lk(mu);
  cv.wait(lk, [this, &results] { return !results.empty(); });
  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, AsyncInferMultiDifferentOptions)
{
  tc::Error err = tc::Error::Success;
  // Create 3 sets of 'options', 'inputs', 'outputs', technically
  // only InferInput can not be reused for requests that are sent
  // concurrently, here use distinct objects for all 'options',
  // 'inputs', and 'outputs' for simplicity.
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    // output will be different based on version
    // v1 : not swap
    // v2 : swap
    // v3 : swap
    size_t version = (i % 3) + 1;
    options.back().model_version_ = std::to_string(version);

    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);

    expected_outputs.emplace_back();
    {
      auto& expected =
          expected_outputs.back()[version == 1 ? "OUTPUT0" : "OUTPUT1"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] + input_1[i]);
      }
    }
    {
      auto& expected =
          expected_outputs.back()[version == 1 ? "OUTPUT1" : "OUTPUT0"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] - input_1[i]);
      }
    }
  }

  std::vector<tc::InferResult*> results;
  std::condition_variable cv;
  std::mutex mu;
  err = this->client_->AsyncInferMulti(
      [&results, &cv, &mu](std::vector<tc::InferResult*> res) {
        {
          std::lock_guard<std::mutex> lk(mu);
          results.swap(res);
        }
        cv.notify_one();
      },
      options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  std::unique_lock<std::mutex> lk(mu);
  cv.wait(lk, [this, &results] { return !results.empty(); });
  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, AsyncInferMultiOneOption)
{
  // Create only 1 sets of 'options'.
  tc::Error err = tc::Error::Success;
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  options.emplace_back(this->model_name_);
  // Not swap
  options.back().model_version_ = "1";
  for (size_t i = 0; i < 3; ++i) {
    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);

    expected_outputs.emplace_back();
    {
      auto& expected = expected_outputs.back()["OUTPUT0"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] + input_1[i]);
      }
    }
    {
      auto& expected = expected_outputs.back()["OUTPUT1"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] - input_1[i]);
      }
    }
  }

  std::vector<tc::InferResult*> results;
  std::condition_variable cv;
  std::mutex mu;
  err = this->client_->AsyncInferMulti(
      [&results, &cv, &mu](std::vector<tc::InferResult*> res) {
        {
          std::lock_guard<std::mutex> lk(mu);
          results.swap(res);
        }
        cv.notify_one();
      },
      options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  std::unique_lock<std::mutex> lk(mu);
  cv.wait(lk, [this, &results] { return !results.empty(); });
  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, AsyncInferMultiOneOutput)
{
  // Create only 1 sets of 'outputs', but combine with different 'options'
  tc::Error err = tc::Error::Success;
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    // output will be different based on version
    // v1 : not swap
    // v2 : swap
    // v3 : swap
    size_t version = (i % 3) + 1;
    options.back().model_version_ = std::to_string(version);

    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);

    expected_outputs.emplace_back();
    {
      auto& expected = expected_outputs.back()["OUTPUT0"];
      if (version == 1) {
        for (size_t i = 0; i < 16; ++i) {
          expected.emplace_back(input_0[i] + input_1[i]);
        }
      } else {
        for (size_t i = 0; i < 16; ++i) {
          expected.emplace_back(input_0[i] - input_1[i]);
        }
      }
    }
  }

  std::vector<tc::InferResult*> results;
  std::condition_variable cv;
  std::mutex mu;
  err = this->client_->AsyncInferMulti(
      [&results, &cv, &mu](std::vector<tc::InferResult*> res) {
        {
          std::lock_guard<std::mutex> lk(mu);
          results.swap(res);
        }
        cv.notify_one();
      },
      options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  std::unique_lock<std::mutex> lk(mu);
  cv.wait(lk, [this, &results] { return !results.empty(); });
  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, AsyncInferMultiNoOutput)
{
  // Not specifying 'outputs' at all, but combine with different 'options'
  tc::Error err = tc::Error::Success;
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    // output will be different based on version
    // v1 : not swap
    // v2 : swap
    // v3 : swap
    size_t version = (i % 3) + 1;
    options.back().model_version_ = std::to_string(version);

    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    expected_outputs.emplace_back();
    {
      auto& expected =
          expected_outputs.back()[version == 1 ? "OUTPUT0" : "OUTPUT1"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] + input_1[i]);
      }
    }
    {
      auto& expected =
          expected_outputs.back()[version == 1 ? "OUTPUT1" : "OUTPUT0"];
      for (size_t i = 0; i < 16; ++i) {
        expected.emplace_back(input_0[i] - input_1[i]);
      }
    }
  }

  std::vector<tc::InferResult*> results;
  std::condition_variable cv;
  std::mutex mu;
  err = this->client_->AsyncInferMulti(
      [&results, &cv, &mu](std::vector<tc::InferResult*> res) {
        {
          std::lock_guard<std::mutex> lk(mu);
          results.swap(res);
        }
        cv.notify_one();
      },
      options, inputs, outputs);
  ASSERT_TRUE(err.IsOk()) << "failed to perform multiple inferences: "
                          << err.Message();

  std::unique_lock<std::mutex> lk(mu);
  cv.wait(lk, [this, &results] { return !results.empty(); });
  EXPECT_NO_FATAL_FAILURE(this->ValidateOutput(results, expected_outputs));
}

TYPED_TEST_P(ClientTest, AsyncInferMultiMismatchOptions)
{
  // Create mismatch number of 'options'.
  tc::Error err = tc::Error::Success;
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  options.emplace_back(this->model_name_);
  options.emplace_back(this->model_name_);
  for (size_t i = 0; i < 3; ++i) {
    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    tc::InferRequestedOutput* output;
    outputs.emplace_back();
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);
    err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
    ASSERT_TRUE(err.IsOk())
        << "failed to create inference output: " << err.Message();
    outputs.back().emplace_back(output);
  }

  std::vector<tc::InferResult*> results;
  std::condition_variable cv;
  std::mutex mu;
  err = this->client_->AsyncInferMulti(
      [&results, &cv, &mu](std::vector<tc::InferResult*> res) {
        {
          std::lock_guard<std::mutex> lk(mu);
          results.swap(res);
        }
        cv.notify_one();
      },
      options, inputs, outputs);
  ASSERT_FALSE(err.IsOk()) << "Expect AsyncInferMulti() to fail";
}

TYPED_TEST_P(ClientTest, AsyncInferMultiMismatchOutputs)
{
  // Create mismatch number of 'outputs'.
  tc::Error err = tc::Error::Success;
  std::vector<tc::InferOptions> options;
  std::vector<std::vector<tc::InferInput*>> inputs;
  std::vector<std::vector<const tc::InferRequestedOutput*>> outputs;

  std::vector<std::map<std::string, std::vector<int32_t>>> expected_outputs;
  for (size_t i = 0; i < 3; ++i) {
    options.emplace_back(this->model_name_);
    const auto& input_0 = this->input_data_[i % this->input_data_.size()];
    const auto& input_1 = this->input_data_[(i + 1) % this->input_data_.size()];
    inputs.emplace_back();
    err = this->PrepareInputs(input_0, input_1, &inputs.back());

    if (i != 2) {
      tc::InferRequestedOutput* output;
      outputs.emplace_back();
      err = tc::InferRequestedOutput::Create(&output, "OUTPUT0");
      ASSERT_TRUE(err.IsOk())
          << "failed to create inference output: " << err.Message();
      outputs.back().emplace_back(output);
      err = tc::InferRequestedOutput::Create(&output, "OUTPUT1");
      ASSERT_TRUE(err.IsOk())
          << "failed to create inference output: " << err.Message();
      outputs.back().emplace_back(output);
    }
  }

  std::vector<tc::InferResult*> results;
  std::condition_variable cv;
  std::mutex mu;
  err = this->client_->AsyncInferMulti(
      [&results, &cv, &mu](std::vector<tc::InferResult*> res) {
        {
          std::lock_guard<std::mutex> lk(mu);
          results.swap(res);
        }
        cv.notify_one();
      },
      options, inputs, outputs);
  ASSERT_FALSE(err.IsOk()) << "Expect AsyncInferMulti() to fail";
}

TYPED_TEST_P(ClientTest, LoadWithFileOverride)
{
  std::vector<char> content;
  {
    std::string path("unit_test_models/onnx_int32_int32_int32/3/model.onnx");
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
      ASSERT_TRUE(false) << "failed to open file for testing";
    }

    in.seekg(0, std::ios::end);
    content = std::vector<char>(in.tellg());
    in.seekg(0, std::ios::beg);
    in.read(content.data(), content.size());
    in.close();
  }

  std::string config("{\"backend\":\"onnxruntime\"}");
  std::string model_name("onnx_int32_int32_int32");
  std::string override_name("override_model");
  std::vector<std::pair<std::string, bool>> expected_version_ready{
      {"1", false}, {"3", true}};
  std::vector<std::pair<std::string, bool>> expected_override_version_ready{
      {"1", true}, {"3", false}};

  tc::Error err = tc::Error::Success;
  for (const auto& vr : expected_version_ready) {
    bool ready = false;
    err = this->client_->IsModelReady(&ready, model_name, vr.first);
    ASSERT_TRUE(err.IsOk())
        << "failed to get version readiness: " << err.Message();
    ASSERT_EQ(ready, vr.second) << "expect model " << model_name << " version "
                                << vr.first << " readiness: " << vr.second;
  }

  // Request to load the model with override file, should fail
  // without providing override config. The config requirement
  // serves as an reminder that the existing model directory will
  // not be used.
  err = this->LoadModel(
      model_name, std::string(), {{"file:1/model.onnx", content}});
  ASSERT_FALSE(err.IsOk()) << "Expect LoadModel() to fail";
  // Sanity check that the model is unchanged
  for (const auto& vr : expected_version_ready) {
    bool ready = false;
    err = this->client_->IsModelReady(&ready, model_name, vr.first);
    ASSERT_TRUE(err.IsOk())
        << "failed to get version readiness: " << err.Message();
    ASSERT_EQ(ready, vr.second) << "expect model " << model_name << " version "
                                << vr.first << " readiness: " << vr.second;
  }

  // Request to load the model with override file and config in
  // a different name
  err =
      this->LoadModel(override_name, config, {{"file:1/model.onnx", content}});
  ASSERT_TRUE(err.IsOk()) << "Expect LoadModel() succeed: " << err.Message();
  // Sanity check that the model with original name is unchanged
  for (const auto& vr : expected_version_ready) {
    bool ready = false;
    err = this->client_->IsModelReady(&ready, model_name, vr.first);
    ASSERT_TRUE(err.IsOk())
        << "failed to get version readiness: " << err.Message();
    ASSERT_EQ(ready, vr.second) << "expect model " << model_name << " version "
                                << vr.first << " readiness: " << vr.second;
  }

  // Check override model readiness
  for (const auto& vr : expected_override_version_ready) {
    bool ready = false;
    err = this->client_->IsModelReady(&ready, override_name, vr.first);
    ASSERT_TRUE(err.IsOk())
        << "failed to get version readiness: " << err.Message();
    ASSERT_EQ(ready, vr.second)
        << "expect model " << override_name << " version " << vr.first
        << " readiness: " << vr.second;
  }

  // Request to load the model with override file and config in
  // original name
  err = this->LoadModel(model_name, config, {{"file:1/model.onnx", content}});
  ASSERT_TRUE(err.IsOk()) << "Expect LoadModel() succeed: " << err.Message();
  // check that the model with original name is changed
  for (const auto& vr : expected_override_version_ready) {
    bool ready = false;
    err = this->client_->IsModelReady(&ready, model_name, vr.first);
    ASSERT_TRUE(err.IsOk())
        << "failed to get version readiness: " << err.Message();
    ASSERT_EQ(ready, vr.second) << "expect model " << model_name << " version "
                                << vr.first << " readiness: " << vr.second;
  }

  // Sanity check readiness of the different named model
  for (const auto& vr : expected_override_version_ready) {
    bool ready = false;
    err = this->client_->IsModelReady(&ready, override_name, vr.first);
    ASSERT_TRUE(err.IsOk())
        << "failed to get version readiness: " << err.Message();
    ASSERT_EQ(ready, vr.second)
        << "expect model " << override_name << " version " << vr.first
        << " readiness: " << vr.second;
  }
}

TYPED_TEST_P(ClientTest, LoadWithConfigOverride)
{
  // Request to load the model with override config
  std::string model_name("onnx_int32_int32_int32");
  std::vector<std::pair<std::string, bool>> original_version_ready{
      {"2", true}, {"3", true}};
  std::vector<std::pair<std::string, bool>> expected_version_ready{
      {"2", true}, {"3", false}};
  tc::Error err = tc::Error::Success;

  // Send the config with wrong format
  std::string config(
      "\"parameters\": {\"config\": {{\"backend\":\"onnxruntime\", "
      "\"version_policy\":{\"specific\":{\"versions\":[2]}}}}}");

  err = this->LoadModel(model_name, config);
  ASSERT_FALSE(err.IsOk()) << "Expect LoadModel() to fail";

  // The model should not be changed after a failed LoadModel request
  for (const auto& vr : original_version_ready) {
    bool ready = false;
    err = this->client_->IsModelReady(&ready, model_name, vr.first);
    ASSERT_TRUE(err.IsOk())
        << "failed to get version readiness: " << err.Message();
    ASSERT_EQ(ready, vr.second) << "expect model " << model_name << " version "
                                << vr.first << " readiness: " << vr.second;
  }

  // Send the config with correct format
  config =
      "{\"backend\":\"onnxruntime\", "
      "\"version_policy\":{\"specific\":{\"versions\":[2]}}}";
  err = this->LoadModel(model_name, config);

  // The model should be changed after a successful LoadModel request
  for (const auto& vr : expected_version_ready) {
    bool ready = false;
    err = this->client_->IsModelReady(&ready, model_name, vr.first);
    ASSERT_TRUE(err.IsOk())
        << "failed to get version readiness: " << err.Message();
    ASSERT_EQ(ready, vr.second) << "expect model " << model_name << " version "
                                << vr.first << " readiness: " << vr.second;
  }
}

TEST_F(HTTPTraceTest, HTTPUpdateTraceSettings)
{
  // Update model and global trace settings in order, and expect the global
  // trace settings will only reflect to the model setting fields that haven't
  // been specified.
  tc::Error err = tc::Error::Success;
  std::string trace_settings;

  EXPECT_NO_FATAL_FAILURE(this->TearDown());
  EXPECT_NO_FATAL_FAILURE(this->CheckServerInitialState());

  std::string expected_first_model_settings =
      "{\"trace_level\":[\"TIMESTAMPS\"],\"trace_rate\":\"1\",\"trace_count\":"
      "\"-1\",\"log_frequency\":\"0\",\"trace_file\":\"global_unittest.log\","
      "\"trace_"
      "mode\":\"triton\"}";
  std::string expected_second_model_settings =
      "{\"trace_level\":[\"TIMESTAMPS\",\"TENSORS\"],\"trace_rate\":\"1\","
      "\"trace_count\":\"-1\",\"log_frequency\":\"0\",\"trace_file\":\"global_"
      "unittest."
      "log\",\"trace_mode\":\"triton\"}";
  std::string expected_global_settings =
      "{\"trace_level\":[\"TIMESTAMPS\",\"TENSORS\"],\"trace_rate\":\"1\","
      "\"trace_count\":\"-1\",\"log_frequency\":\"0\",\"trace_file\":\"global_"
      "unittest."
      "log\",\"trace_mode\":\"triton\"}";

  std::map<std::string, std::vector<std::string>> model_update_settings = {
      {"trace_file", {"model.log"}}};
  std::map<std::string, std::vector<std::string>> global_update_settings = {
      {"trace_level", {"TIMESTAMPS", "TENSORS"}}};

  err = this->client_->UpdateTraceSettings(
      &trace_settings, this->model_name_, model_update_settings);
  ASSERT_FALSE(err.IsOk()) << "update disabled settings: trace_file";
  ASSERT_TRUE(
      err.Message() ==
      "trace file location can not be updated through network protocol")
      << "error: Unexpected error message: " << err.Message();

  err = this->client_->GetTraceSettings(&trace_settings, this->model_name_);
  ASSERT_TRUE(err.IsOk()) << "unable to get trace settings: " << err.Message();
  ASSERT_EQ(trace_settings, expected_first_model_settings)
      << "error: Unexpected updated model trace settings" << std::endl;
  // Note that 'trace_level' may be mismatch due to the order of the levels
  // listed, currently we assume the order is the same for simplicity. But the
  // order shouldn't be enforced and this checking needs to be improved when
  // this kind of failure is reported
  err = this->client_->UpdateTraceSettings(
      &trace_settings, "", global_update_settings);
  ASSERT_EQ(trace_settings, expected_global_settings)
      << "error: Unexpected updated global trace settings" << std::endl;

  err = this->client_->GetTraceSettings(&trace_settings, this->model_name_);
  ASSERT_TRUE(err.IsOk()) << "unable to get trace settings: " << err.Message();
  ASSERT_EQ(trace_settings, expected_second_model_settings)
      << "error: Unexpected model trace settings after global update"
      << std::endl;
}

TEST_F(HTTPTraceTest, HTTPClearTraceSettings)
{
  // Clear global and model trace settings in order, and expect the default /
  // global trace settings are propagated properly.
  tc::Error err = tc::Error::Success;
  std::string trace_settings;

  EXPECT_NO_FATAL_FAILURE(this->TearDown());
  EXPECT_NO_FATAL_FAILURE(this->CheckServerInitialState());

  // First set up the model / global trace setting that: model 'simple' has
  // 'trace_rate' and 'log_frequency' specified global has 'trace_level',
  // 'trace_count' and 'trace_rate' specified
  std::map<std::string, std::vector<std::string>> model_update_settings = {
      {"trace_rate", {"12"}}, {"log_frequency", {"34"}}};
  std::map<std::string, std::vector<std::string>> global_update_settings = {
      {"trace_rate", {"56"}},
      {"trace_count", {"78"}},
      {"trace_level", {"OFF"}}};
  err = this->client_->UpdateTraceSettings(
      &trace_settings, "", global_update_settings);
  ASSERT_TRUE(err.IsOk()) << "unable to update trace settings: "
                          << err.Message();
  err = this->client_->UpdateTraceSettings(
      &trace_settings, this->model_name_, model_update_settings);
  ASSERT_TRUE(err.IsOk()) << "unable to update trace settings: "
                          << err.Message();

  std::string expected_global_settings =
      "{\"trace_level\":[\"OFF\"],\"trace_rate\":\"1\",\"trace_count\":\"-1\","
      "\"log_frequency\":\"0\",\"trace_file\":\"global_unittest.log\",\"trace_"
      "mode\":\"triton\"}";
  std::string expected_first_model_settings =
      "{\"trace_level\":[\"OFF\"],\"trace_rate\":\"12\",\"trace_count\":\"-1\","
      "\"log_frequency\":\"34\",\"trace_file\":\"global_unittest.log\",\"trace_"
      "mode\":\"triton\"}";
  std::string expected_second_model_settings =
      "{\"trace_level\":[\"OFF\"],\"trace_rate\":\"1\",\"trace_count\":\"-1\","
      "\"log_frequency\":\"34\",\"trace_file\":\"global_unittest.log\",\"trace_"
      "mode\":\"triton\"}";
  std::map<std::string, std::vector<std::string>> global_clear_settings = {
      {"trace_rate", {}}, {"trace_count", {}}};
  std::map<std::string, std::vector<std::string>> model_clear_settings = {
      {"trace_rate", {}}, {"trace_level", {}}};

  // Clear global
  err = this->client_->UpdateTraceSettings(
      &trace_settings, "", global_clear_settings);
  ASSERT_TRUE(err.IsOk()) << "unable to update trace settings: "
                          << err.Message();
  ASSERT_EQ(trace_settings, expected_global_settings)
      << "error: Unexpected updated global trace settings" << std::endl;
  err = this->client_->GetTraceSettings(&trace_settings, this->model_name_);
  ASSERT_TRUE(err.IsOk()) << "unable to get trace settings: " << err.Message();
  ASSERT_EQ(trace_settings, expected_first_model_settings)
      << "error: Unexpected model trace settings after global clear"
      << std::endl;

  // Clear model
  err = this->client_->UpdateTraceSettings(
      &trace_settings, this->model_name_, model_clear_settings);
  ASSERT_TRUE(err.IsOk()) << "unable to update trace settings: "
                          << err.Message();
  ASSERT_EQ(trace_settings, expected_second_model_settings)
      << "error: Unexpected model trace settings after model clear"
      << std::endl;
  err = this->client_->GetTraceSettings(&trace_settings, "");
  ASSERT_TRUE(err.IsOk()) << "unable to get trace settings: " << err.Message();
  ASSERT_EQ(trace_settings, expected_global_settings)
      << "error: Unexpected global trace settings after model clear"
      << std::endl;
}

TEST_F(GRPCTraceTest, GRPCUpdateTraceSettings)
{
  // Update model and global trace settings in order, and expect the global
  // trace settings will only reflect to the model setting fields that haven't
  // been specified.
  tc::Error err = tc::Error::Success;
  inference::TraceSettingResponse response;
  std::string trace_settings;

  EXPECT_NO_FATAL_FAILURE(this->TearDown());
  EXPECT_NO_FATAL_FAILURE(this->CheckServerInitialState());

  std::string expected_first_model_settings =
      "settings{key:\"log_frequency\"value{value:\"0\"}}settings{key:\"trace_"
      "count\"value{value:\"-1\"}}settings{key:\"trace_file\"value{value:"
      "\"global_unittest.log\"}}settings{key:\"trace_level\"value{value:"
      "\"TIMESTAMPS\"}}"
      "settings{key:\"trace_mode\"value{value:\"triton\"}}"
      "settings{key:\"trace_rate\"value{value:\"1\"}}";
  std::string expected_second_model_settings =
      "settings{key:\"log_frequency\"value{value:\"0\"}}settings{key:\"trace_"
      "count\"value{value:\"-1\"}}settings{key:\"trace_file\"value{value:"
      "\"global_unittest.log\"}}settings{key:\"trace_level\"value{value:"
      "\"TIMESTAMPS\"value:\"TENSORS\"}}settings{key:\"trace_mode\"value{value:"
      "\"triton\"}}"
      "settings{key:\"trace_rate\"value{value:\"1\"}}";
  std::string expected_global_settings =
      "settings{key:\"log_frequency\"value{value:\"0\"}}settings{key:\"trace_"
      "count\"value{value:\"-1\"}}settings{key:\"trace_file\"value{value:"
      "\"global_unittest.log\"}}settings{key:\"trace_level\"value{value:"
      "\"TIMESTAMPS\"value:\"TENSORS\"}}settings{key:\"trace_mode\"value{value:"
      "\"triton\"}}"
      "settings{key:\"trace_rate\"value{value:\"1\"}}";

  std::map<std::string, std::vector<std::string>> model_update_settings = {
      {"trace_file", {"model.log"}}};
  std::map<std::string, std::vector<std::string>> global_update_settings = {
      {"trace_level", {"TIMESTAMPS", "TENSORS"}}};


  err = this->client_->UpdateTraceSettings(
      &response, this->model_name_, model_update_settings);
  ASSERT_FALSE(err.IsOk()) << "update disabled settings: trace_file";
  ASSERT_TRUE(
      err.Message() ==
      "trace file location can not be updated through network protocol")
      << "error: Unexpected error message: " << err.Message();

  err = this->client_->GetTraceSettings(&response, this->model_name_);
  ASSERT_TRUE(err.IsOk()) << "unable to get trace settings: " << err.Message();
  EXPECT_NO_FATAL_FAILURE(ConvertResponse(response, &trace_settings));
  ASSERT_EQ(trace_settings, expected_first_model_settings)
      << "error: Unexpected updated model trace settings" << std::endl;
  // Note that 'trace_level' may be mismatch due to the order of the levels
  // listed, currently we assume the order is the same for simplicity. But the
  // order shouldn't be enforced and this checking needs to be improved when
  // this kind of failure is reported
  err =
      this->client_->UpdateTraceSettings(&response, "", global_update_settings);
  ASSERT_TRUE(err.IsOk()) << "unable to update trace settings: "
                          << err.Message();
  EXPECT_NO_FATAL_FAILURE(ConvertResponse(response, &trace_settings));
  ASSERT_EQ(trace_settings, expected_global_settings)
      << "error: Unexpected updated global trace settings" << std::endl;

  err = this->client_->GetTraceSettings(&response, this->model_name_);
  ASSERT_TRUE(err.IsOk()) << "unable to get trace settings: " << err.Message();
  EXPECT_NO_FATAL_FAILURE(ConvertResponse(response, &trace_settings));
  ASSERT_EQ(trace_settings, expected_second_model_settings)
      << "error: Unexpected model trace settings after global update"
      << std::endl;
}

TEST_F(GRPCTraceTest, GRPCClearTraceSettings)
{
  // Clear global and model trace settings in order, and expect the default /
  // global trace settings are propagated properly.
  tc::Error err = tc::Error::Success;
  inference::TraceSettingResponse response;
  std::string trace_settings;

  EXPECT_NO_FATAL_FAILURE(this->TearDown());
  EXPECT_NO_FATAL_FAILURE(this->CheckServerInitialState());

  // First set up the model / global trace setting that: model 'simple' has
  // 'trace_rate' and 'log_frequency' specified global has 'trace_level',
  // 'trace_count' and 'trace_rate' specified
  std::map<std::string, std::vector<std::string>> model_update_settings = {
      {"trace_rate", {"12"}}, {"log_frequency", {"34"}}};
  std::map<std::string, std::vector<std::string>> global_update_settings = {
      {"trace_rate", {"56"}},
      {"trace_count", {"78"}},
      {"trace_level", {"OFF"}}};
  err =
      this->client_->UpdateTraceSettings(&response, "", global_update_settings);
  ASSERT_TRUE(err.IsOk()) << "unable to update trace settings: "
                          << err.Message();
  err = this->client_->UpdateTraceSettings(
      &response, this->model_name_, model_update_settings);
  ASSERT_TRUE(err.IsOk()) << "unable to update trace settings: "
                          << err.Message();

  std::string expected_global_settings =
      "settings{key:\"log_frequency\"value{value:\"0\"}}settings{key:\"trace_"
      "count\"value{value:\"-1\"}}settings{key:\"trace_file\"value{value:"
      "\"global_unittest.log\"}}settings{key:\"trace_level\"value{value:"
      "\"OFF\"}}settings{key:\"trace_mode\"value{value:\"triton\"}}"
      "settings{key:\"trace_rate\"value{value:\"1\"}}";
  std::string expected_first_model_settings =
      "settings{key:\"log_frequency\"value{value:\"34\"}}settings{key:\"trace_"
      "count\"value{value:\"-1\"}}settings{key:\"trace_file\"value{value:"
      "\"global_unittest.log\"}}settings{key:\"trace_level\"value{value:"
      "\"OFF\"}}settings{key:\"trace_mode\"value{value:\"triton\"}}"
      "settings{key:\"trace_rate\"value{value:\"12\"}}";
  std::string expected_second_model_settings =
      "settings{key:\"log_frequency\"value{value:\"34\"}}settings{key:\"trace_"
      "count\"value{value:\"-1\"}}settings{key:\"trace_file\"value{value:"
      "\"global_unittest.log\"}}settings{key:\"trace_level\"value{value:"
      "\"OFF\"}}settings{key:\"trace_mode\"value{value:\"triton\"}}"
      "settings{key:\"trace_rate\"value{value:\"1\"}}";
  std::map<std::string, std::vector<std::string>> global_clear_settings = {
      {"trace_rate", {}}, {"trace_count", {}}};
  std::map<std::string, std::vector<std::string>> model_clear_settings = {
      {"trace_rate", {}}, {"trace_level", {}}};

  // Clear global
  err =
      this->client_->UpdateTraceSettings(&response, "", global_clear_settings);
  ASSERT_TRUE(err.IsOk()) << "unable to update trace settings: "
                          << err.Message();
  EXPECT_NO_FATAL_FAILURE(ConvertResponse(response, &trace_settings));
  ASSERT_EQ(trace_settings, expected_global_settings)
      << "error: Unexpected updated global trace settings" << std::endl;
  err = client_->GetTraceSettings(&response, this->model_name_);
  ASSERT_TRUE(err.IsOk()) << "unable to get trace settings: " << err.Message();
  EXPECT_NO_FATAL_FAILURE(ConvertResponse(response, &trace_settings));
  ASSERT_EQ(trace_settings, expected_first_model_settings)
      << "error: Unexpected model trace settings after global clear"
      << std::endl;

  // Clear model
  err = this->client_->UpdateTraceSettings(
      &response, this->model_name_, model_clear_settings);
  ASSERT_TRUE(err.IsOk()) << "unable to update trace settings: "
                          << err.Message();
  EXPECT_NO_FATAL_FAILURE(ConvertResponse(response, &trace_settings));
  ASSERT_EQ(trace_settings, expected_second_model_settings)
      << "error: Unexpected model trace settings after model clear"
      << std::endl;
  err = client_->GetTraceSettings(&response, "");
  ASSERT_TRUE(err.IsOk()) << "unable to get trace settings: " << err.Message();
  EXPECT_NO_FATAL_FAILURE(ConvertResponse(response, &trace_settings));
  ASSERT_EQ(trace_settings, expected_global_settings)
      << "error: Unexpected global trace settings after model clear"
      << std::endl;
}

class TestHttpInferRequest : public tc::HttpInferRequest {
 public:
  tc::Error ConvertBinaryInputsToJSON(
      tc::InferInput& input,
      triton::common::TritonJson::Value& data_json) const override
  {
    return tc::HttpInferRequest::ConvertBinaryInputsToJSON(input, data_json);
  }

  tc::Error ConvertBinaryInputToJSON(
      const uint8_t* buf, const size_t element_count,
      const std::string& datatype,
      triton::common::TritonJson::Value& data_json) const override
  {
    return tc::HttpInferRequest::ConvertBinaryInputToJSON(
        buf, element_count, datatype, data_json);
  }
};

class HTTPJSONDataTest : public ::testing::Test {};

TEST_F(HTTPJSONDataTest, ConvertBinaryInputsToJSON)
{
  // This tests the HttpInferRequest::ConvertBinaryInputsToJSON() function,
  // which basically cycles through all the inputs that added to an InferInput
  // via InferInput::AppendRaw(). This test confirms that an InferInput with two
  // calls to InferInput::AppendRaw() correctly has all contents from the
  // AppendRaw() calls correctly converted into a flattened JSON array.

  TestHttpInferRequest test_http_infer_request{};

  tc::InferInput* input{};
  tc::InferInput::Create(&input, "INPUT", {1, 2, 2}, "INT32");
  int32_t input_raw_1[4] = {1, 3, 5, 7};
  size_t input_raw_byte_size_1 = sizeof(input_raw_1);
  int32_t input_raw_2[4] = {2, 4, 6, 8};
  size_t input_raw_byte_size_2 = sizeof(input_raw_2);
  input->AppendRaw(
      reinterpret_cast<uint8_t*>(input_raw_1), input_raw_byte_size_1);
  input->AppendRaw(
      reinterpret_cast<uint8_t*>(input_raw_2), input_raw_byte_size_2);
  triton::common::TritonJson::Value data_json(
      triton::common::TritonJson::ValueType::ARRAY);

  tc::Error err{
      test_http_infer_request.ConvertBinaryInputsToJSON(*input, data_json)};

  EXPECT_TRUE(err.IsOk());
  EXPECT_TRUE(data_json.ArraySize() == 8);
  int64_t value{0};
  data_json.IndexAsInt(0, &value);
  EXPECT_TRUE(value == 1);
  data_json.IndexAsInt(1, &value);
  EXPECT_TRUE(value == 3);
  data_json.IndexAsInt(2, &value);
  EXPECT_TRUE(value == 5);
  data_json.IndexAsInt(3, &value);
  EXPECT_TRUE(value == 7);
  data_json.IndexAsInt(4, &value);
  EXPECT_TRUE(value == 2);
  data_json.IndexAsInt(5, &value);
  EXPECT_TRUE(value == 4);
  data_json.IndexAsInt(6, &value);
  EXPECT_TRUE(value == 6);
  data_json.IndexAsInt(7, &value);
  EXPECT_TRUE(value == 8);
}

TEST_F(HTTPJSONDataTest, ConvertBinaryInputToJSON)
{
  // This tests the HttpInferRequest::ConvertBinaryInputToJSON() function,
  // which converts one binary buffer into a corresponding JSON array of
  // specified data type. This test tests each valid and invalid data type.

  TestHttpInferRequest test_http_infer_request{};
  tc::Error err{};

  const uint8_t* buf{nullptr};
  size_t element_count{2};
  std::string datatype{""};
  triton::common::TritonJson::Value data_json{};

  // BOOL
  datatype = "BOOL";
  std::array<bool, 2> bool_array({false, true});
  buf = reinterpret_cast<const uint8_t*>(bool_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == bool_array.size());
  bool bool_value{false};
  data_json.IndexAsBool(0, &bool_value);
  EXPECT_TRUE(bool_value == bool_array[0]);
  data_json.IndexAsBool(1, &bool_value);
  EXPECT_TRUE(bool_value == bool_array[1]);

  // UINT8
  datatype = "UINT8";
  std::array<uint8_t, 2> uint8_array({1, UINT8_MAX});
  buf = reinterpret_cast<const uint8_t*>(uint8_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == uint8_array.size());
  uint64_t uint8_value{0};
  data_json.IndexAsUInt(0, &uint8_value);
  EXPECT_TRUE(uint8_value == uint8_array[0]);
  data_json.IndexAsUInt(1, &uint8_value);
  EXPECT_TRUE(uint8_value == uint8_array[1]);

  // UINT16
  datatype = "UINT16";
  std::array<uint16_t, 2> uint16_array({1, UINT16_MAX});
  buf = reinterpret_cast<const uint8_t*>(uint16_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == uint16_array.size());
  uint64_t uint16_value{0};
  data_json.IndexAsUInt(0, &uint16_value);
  EXPECT_TRUE(uint16_value == uint16_array[0]);
  data_json.IndexAsUInt(1, &uint16_value);
  EXPECT_TRUE(uint16_value == uint16_array[1]);

  // UINT32
  datatype = "UINT32";
  std::array<uint32_t, 2> uint32_array({1, UINT32_MAX});
  buf = reinterpret_cast<const uint8_t*>(uint32_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == uint32_array.size());
  uint64_t uint32_value{0};
  data_json.IndexAsUInt(0, &uint32_value);
  EXPECT_TRUE(uint32_value == uint32_array[0]);
  data_json.IndexAsUInt(1, &uint32_value);
  EXPECT_TRUE(uint32_value == uint32_array[1]);

  // UINT64
  datatype = "UINT64";
  std::array<uint64_t, 2> uint64_array({1, UINT64_MAX});
  buf = reinterpret_cast<const uint8_t*>(uint64_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == uint64_array.size());
  uint64_t uint64_value{0};
  data_json.IndexAsUInt(0, &uint64_value);
  EXPECT_TRUE(uint64_value == uint64_array[0]);
  data_json.IndexAsUInt(1, &uint64_value);
  EXPECT_TRUE(uint64_value == uint64_array[1]);

  // INT8
  datatype = "INT8";
  std::array<int8_t, 2> int8_array({INT8_MIN, INT8_MAX});
  buf = reinterpret_cast<const uint8_t*>(int8_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == int8_array.size());
  int64_t int8_value{0};
  data_json.IndexAsInt(0, &int8_value);
  EXPECT_TRUE(int8_value == int8_array[0]);
  data_json.IndexAsInt(1, &int8_value);
  EXPECT_TRUE(int8_value == int8_array[1]);

  // INT16
  datatype = "INT16";
  std::array<int16_t, 2> int16_array({INT16_MIN, INT16_MAX});
  buf = reinterpret_cast<const uint8_t*>(int16_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == int16_array.size());
  int64_t int16_value{0};
  data_json.IndexAsInt(0, &int16_value);
  EXPECT_TRUE(int16_value == int16_array[0]);
  data_json.IndexAsInt(1, &int16_value);
  EXPECT_TRUE(int16_value == int16_array[1]);

  // INT32
  datatype = "INT32";
  std::array<int32_t, 2> int32_array({INT32_MIN, INT32_MAX});
  buf = reinterpret_cast<const uint8_t*>(int32_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == int32_array.size());
  int64_t int32_value{0};
  data_json.IndexAsInt(0, &int32_value);
  EXPECT_TRUE(int32_value == int32_array[0]);
  data_json.IndexAsInt(1, &int32_value);
  EXPECT_TRUE(int32_value == int32_array[1]);

  // INT64
  datatype = "INT64";
  std::array<int64_t, 2> int64_array({INT64_MIN, INT64_MAX});
  buf = reinterpret_cast<const uint8_t*>(int64_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == int64_array.size());
  int64_t int64_value{0};
  data_json.IndexAsInt(0, &int64_value);
  EXPECT_TRUE(int64_value == int64_array[0]);
  data_json.IndexAsInt(1, &int64_value);
  EXPECT_TRUE(int64_value == int64_array[1]);

  // FP16 - invalid data type
  datatype = "FP16";

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == false);

  // FP32
  datatype = "FP32";
  std::array<float, 2> fp32_array({-1000.0, 1000.0});
  buf = reinterpret_cast<const uint8_t*>(fp32_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == fp32_array.size());
  double fp32_value{0.0};
  data_json.IndexAsDouble(0, &fp32_value);
  EXPECT_NEAR(fp32_value, fp32_array[0], 1.0);
  data_json.IndexAsDouble(1, &fp32_value);
  EXPECT_NEAR(fp32_value, fp32_array[1], 1.0);

  // FP64
  datatype = "FP64";
  std::array<double, 2> fp64_array({-1000.0, 1000.0});
  buf = reinterpret_cast<const uint8_t*>(fp64_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == fp64_array.size());
  double fp64_value{0.0};
  data_json.IndexAsDouble(0, &fp64_value);
  EXPECT_NEAR(fp64_value, fp64_array[0], 1.0);
  data_json.IndexAsDouble(1, &fp64_value);
  EXPECT_NEAR(fp64_value, fp64_array[1], 1.0);

  // BYTES
  datatype = "BYTES";
  std::array<uint8_t, 12> bytes_array(
      {2, 0, 0, 0, 1, INT8_MAX, 2, 0, 0, 0, 2, INT8_MAX});
  buf = reinterpret_cast<const uint8_t*>(bytes_array.data());
  data_json = triton::common::TritonJson::Value(
      triton::common::TritonJson::ValueType::ARRAY);

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(data_json.ArraySize() == 2);
  const char* bytes_value{nullptr};
  size_t bytes_len{0};
  data_json.IndexAsString(0, &bytes_value, &bytes_len);
  EXPECT_TRUE(bytes_len == 2);
  EXPECT_TRUE(bytes_value[0] == bytes_array[4]);
  EXPECT_TRUE(bytes_value[1] == bytes_array[5]);
  data_json.IndexAsString(1, &bytes_value, &bytes_len);
  EXPECT_TRUE(bytes_len == 2);
  EXPECT_TRUE(bytes_value[0] == bytes_array[10]);
  EXPECT_TRUE(bytes_value[1] == bytes_array[11]);

  // BF16 - invalid data type
  datatype = "BF16";

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == false);

  // invaliddatatype - invalid data type
  datatype = "invaliddatatype";

  err = test_http_infer_request.ConvertBinaryInputToJSON(
      buf, element_count, datatype, data_json);

  EXPECT_TRUE(err.IsOk() == false);
}

class TestInferResultHttp : public tc::InferResultHttp {
 public:
  TestInferResultHttp() {}

  tc::Error ConvertJSONOutputToBinary(
      triton::common::TritonJson::Value& data_json, const std::string& datatype,
      const uint8_t** buf, size_t* buf_size) const override
  {
    return tc::InferResultHttp::ConvertJSONOutputToBinary(
        data_json, datatype, buf, buf_size);
  }
};

TEST_F(HTTPJSONDataTest, ConvertJSONOutputToBinary)
{
  // This tests the InferResultHttp::ConvertJSONOutputToBinary() function,
  // which converts one JSON array into a binary buffer of specified data type.
  // This test tests each valid and invalid data type.

  TestInferResultHttp test_infer_result_http{};
  tc::Error err{};

  triton::common::TritonJson::Value data_json{};
  std::string datatype{""};
  const uint8_t* buf{nullptr};
  size_t buf_size{0};

  // BOOL
  datatype = "BOOL";
  data_json.Parse(R"([false, true])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == sizeof(bool) * 2);
  EXPECT_TRUE(reinterpret_cast<const bool*>(buf)[0] == false);
  EXPECT_TRUE(reinterpret_cast<const bool*>(buf)[1] == true);

  // UINT8
  datatype = "UINT8";
  data_json.Parse(R"([1, 255])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == sizeof(uint8_t) * 2);
  EXPECT_TRUE(reinterpret_cast<const uint8_t*>(buf)[0] == 1);
  EXPECT_TRUE(reinterpret_cast<const uint8_t*>(buf)[1] == 255);

  // UINT16
  datatype = "UINT16";
  data_json.Parse(R"([1, 65535])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == sizeof(uint16_t) * 2);
  EXPECT_TRUE(reinterpret_cast<const uint16_t*>(buf)[0] == 1);
  EXPECT_TRUE(reinterpret_cast<const uint16_t*>(buf)[1] == 65535);

  // UINT32
  datatype = "UINT32";
  data_json.Parse(R"([1, 4294967295])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == sizeof(uint32_t) * 2);
  EXPECT_TRUE(reinterpret_cast<const uint32_t*>(buf)[0] == 1);
  EXPECT_TRUE(reinterpret_cast<const uint32_t*>(buf)[1] == 4294967295);

  // UINT64
  datatype = "UINT64";
  data_json.Parse(R"([1, 18446744073709551615])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == sizeof(uint64_t) * 2);
  EXPECT_TRUE(reinterpret_cast<const uint64_t*>(buf)[0] == 1);
  EXPECT_TRUE(
      reinterpret_cast<const uint64_t*>(buf)[1] == 18446744073709551615ULL);

  // INT8
  datatype = "INT8";
  data_json.Parse(R"([-128, 127])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == sizeof(int8_t) * 2);
  EXPECT_TRUE(reinterpret_cast<const int8_t*>(buf)[0] == -128);
  EXPECT_TRUE(reinterpret_cast<const int8_t*>(buf)[1] == 127);

  // INT16
  datatype = "INT16";
  data_json.Parse(R"([-32768, 32767])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == sizeof(int16_t) * 2);
  EXPECT_TRUE(reinterpret_cast<const int16_t*>(buf)[0] == -32768);
  EXPECT_TRUE(reinterpret_cast<const int16_t*>(buf)[1] == 32767);

  // INT32
  datatype = "INT32";
  data_json.Parse(R"([-2147483648, 2147483647])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == sizeof(int32_t) * 2);
  EXPECT_TRUE(reinterpret_cast<const int32_t*>(buf)[0] == -2147483648);
  EXPECT_TRUE(reinterpret_cast<const int32_t*>(buf)[1] == 2147483647);

  // INT64
  datatype = "INT64";
  data_json.Parse(R"([-9223372036854775808, 9223372036854775807])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == sizeof(int64_t) * 2);
  EXPECT_TRUE(
      reinterpret_cast<const int64_t*>(buf)[0] == -9223372036854775807L - 1);
  EXPECT_TRUE(
      reinterpret_cast<const int64_t*>(buf)[1] == 9223372036854775807LL);

  // FP16 - invalid data type
  datatype = "FP16";

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == false);

  // FP32
  datatype = "FP32";
  data_json.Parse(R"([-1000.0, 1000.0])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == sizeof(float) * 2);
  EXPECT_NEAR(reinterpret_cast<const float*>(buf)[0], -1000.0, 1.0);
  EXPECT_NEAR(reinterpret_cast<const float*>(buf)[1], 1000.0, 1.0);

  // FP64
  datatype = "FP64";
  data_json.Parse(R"([-1000.0, 1000.0])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == sizeof(double) * 2);
  EXPECT_NEAR(reinterpret_cast<const double*>(buf)[0], -1000.0, 1.0);
  EXPECT_NEAR(reinterpret_cast<const double*>(buf)[1], 1000.0, 1.0);

  // BYTES
  datatype = "BYTES";
  data_json.Parse(R"(["\u0001\u007F", "\u0002\u007F"])");

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == true);
  EXPECT_TRUE(buf_size == 12);
  EXPECT_TRUE(*reinterpret_cast<const uint32_t*>(buf) == 2);
  EXPECT_TRUE(reinterpret_cast<const uint8_t*>(buf)[4] == 1);
  EXPECT_TRUE(reinterpret_cast<const uint8_t*>(buf)[5] == 127);
  EXPECT_TRUE(*reinterpret_cast<const uint32_t*>(buf + 6) == 2);
  EXPECT_TRUE(reinterpret_cast<const uint8_t*>(buf)[10] == 2);
  EXPECT_TRUE(reinterpret_cast<const uint8_t*>(buf)[11] == 127);

  // BF16 - invalid data type
  datatype = "BF16";

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == false);

  // invaliddatatype - invalid data type
  datatype = "invaliddatatype";

  err = test_infer_result_http.ConvertJSONOutputToBinary(
      data_json, datatype, &buf, &buf_size);

  EXPECT_TRUE(err.IsOk() == false);
}

REGISTER_TYPED_TEST_SUITE_P(
    ClientTest, InferMulti, InferMultiDifferentOutputs,
    InferMultiDifferentOptions, InferMultiOneOption, InferMultiOneOutput,
    InferMultiNoOutput, InferMultiMismatchOptions, InferMultiMismatchOutputs,
    AsyncInferMulti, AsyncInferMultiDifferentOutputs,
    AsyncInferMultiDifferentOptions, AsyncInferMultiOneOption,
    AsyncInferMultiOneOutput, AsyncInferMultiNoOutput,
    AsyncInferMultiMismatchOptions, AsyncInferMultiMismatchOutputs,
    LoadWithFileOverride, LoadWithConfigOverride);

INSTANTIATE_TYPED_TEST_SUITE_P(GRPC, ClientTest, tc::InferenceServerGrpcClient);
INSTANTIATE_TYPED_TEST_SUITE_P(HTTP, ClientTest, tc::InferenceServerHttpClient);

}  // namespace

int
main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
