//
//
// Copyright 2022 gRPC authors.
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
//
//

#include <grpc/grpc.h>
#include <grpc/impl/channel_arg_names.h>
#include <grpc/slice.h>
#include <grpc/slice_buffer.h>
#include <grpc/status.h>
#include <grpc/support/alloc.h>
#include <grpc/support/port_platform.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <tuple>

#include "absl/base/thread_annotations.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "src/core/channelz/channelz.h"
#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"
#include "src/core/ext/transport/chttp2/transport/frame_goaway.h"
#include "src/core/ext/transport/chttp2/transport/frame_ping.h"
#include "src/core/ext/transport/chttp2/transport/internal_channel_arg_names.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/endpoint_pair.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/slice/slice.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/lib/surface/completion_queue.h"
#include "src/core/server/server.h"
#include "src/core/util/crash.h"
#include "src/core/util/notification.h"
#include "src/core/util/orphanable.h"
#include "src/core/util/sync.h"
#include "src/core/util/useful.h"
#include "test/core/end2end/cq_verifier.h"
#include "test/core/test_util/test_config.h"

namespace grpc_core {
namespace {

void* Tag(intptr_t t) { return reinterpret_cast<void*>(t); }

class GracefulShutdownTest : public ::testing::Test {
 protected:
  GracefulShutdownTest() { SetupAndStart(); }

  ~GracefulShutdownTest() override { ShutdownAndDestroy(); }

  // Sets up the client and server
  void SetupAndStart() {
    ExecCtx exec_ctx;
    cq_ = grpc_completion_queue_create_for_next(nullptr);
    cqv_ = std::make_unique<CqVerifier>(cq_);
    grpc_arg server_args[] = {
        grpc_channel_arg_integer_create(
            const_cast<char*>(GRPC_ARG_HTTP2_BDP_PROBE), 0),
        grpc_channel_arg_integer_create(
            const_cast<char*>(GRPC_ARG_KEEPALIVE_TIME_MS), INT_MAX),
        grpc_channel_arg_integer_create(
            const_cast<char*>(GRPC_ARG_PING_TIMEOUT_MS), 2000)};
    grpc_channel_args server_channel_args = {GPR_ARRAY_SIZE(server_args),
                                             server_args};
    // Create server
    server_ = grpc_server_create(&server_channel_args, nullptr);
    auto* core_server = Server::FromC(server_);
    grpc_server_register_completion_queue(server_, cq_, nullptr);
    grpc_server_start(server_);
    fds_ = grpc_iomgr_create_endpoint_pair("fixture", nullptr);
    auto* transport = grpc_create_chttp2_transport(
        core_server->channel_args(), OrphanablePtr<grpc_endpoint>(fds_.server),
        false);
    grpc_endpoint_add_to_pollset(fds_.server, grpc_cq_pollset(cq_));
    CHECK(core_server->SetupTransport(transport, nullptr,
                                      core_server->channel_args()) ==
          absl::OkStatus());
    grpc_chttp2_transport_start_reading(transport, nullptr, nullptr, nullptr,
                                        nullptr);
    // Start polling on the client
    Notification client_poller_thread_started_notification;
    client_poll_thread_ = std::make_unique<std::thread>(
        [this, &client_poller_thread_started_notification]() {
          grpc_completion_queue* client_cq =
              grpc_completion_queue_create_for_next(nullptr);
          {
            ExecCtx exec_ctx;
            grpc_endpoint_add_to_pollset(fds_.client,
                                         grpc_cq_pollset(client_cq));
            grpc_endpoint_add_to_pollset(fds_.server,
                                         grpc_cq_pollset(client_cq));
          }
          client_poller_thread_started_notification.Notify();
          while (!shutdown_) {
            CHECK(grpc_completion_queue_next(
                      client_cq, grpc_timeout_milliseconds_to_deadline(10),
                      nullptr)
                      .type == GRPC_QUEUE_TIMEOUT);
          }
          grpc_completion_queue_destroy(client_cq);
        });
    client_poller_thread_started_notification.WaitForNotification();
    // Write connection prefix and settings frame
    constexpr char kPrefix[] =
        "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n\x00\x00\x00\x04\x00\x00\x00\x00\x00";
    Write(absl::string_view(kPrefix, sizeof(kPrefix) - 1));
    // Start reading on the client
    grpc_slice_buffer_init(&read_buffer_);
    GRPC_CLOSURE_INIT(&on_read_done_, OnReadDone, this, nullptr);
    GRPC_CLOSURE_INIT(&on_read_done_scheduler_, OnReadDoneScheduler, this,
                      nullptr);
    grpc_endpoint_read(fds_.client, &read_buffer_, &on_read_done_, false,
                       /*min_progress_size=*/1);
  }

  // Shuts down and destroys the client and server.
  void ShutdownAndDestroy() {
    shutdown_ = true;
    ExecCtx exec_ctx;
    {
      MutexLock lock(&ep_destroy_mu_);
      grpc_endpoint_destroy(fds_.client);
      fds_.client = nullptr;
    }
    ExecCtx::Get()->Flush();
    client_poll_thread_->join();
    CHECK(read_end_notification_.WaitForNotificationWithTimeout(
        absl::Seconds(5)));
    // Shutdown and destroy server
    grpc_server_shutdown_and_notify(server_, cq_, Tag(1000));
    cqv_->Expect(Tag(1000), true);
    cqv_->Verify();
    grpc_server_destroy(server_);
    cqv_.reset();
    grpc_completion_queue_destroy(cq_);
  }

  static void OnReadDone(void* arg, grpc_error_handle error) {
    GracefulShutdownTest* self = static_cast<GracefulShutdownTest*>(arg);
    if (error.ok()) {
      {
        MutexLock lock(&self->mu_);
        for (size_t i = 0; i < self->read_buffer_.count; ++i) {
          char* dump = grpc_dump_slice(self->read_buffer_.slices[i],
                                       GPR_DUMP_HEX | GPR_DUMP_ASCII);
          LOG(INFO) << "Read: " << dump;
          gpr_free(dump);
          absl::StrAppend(&self->read_bytes_,
                          StringViewFromSlice(self->read_buffer_.slices[i]));
        }
        self->read_cv_.SignalAll();
      }
      MutexLock lock(&self->ep_destroy_mu_);
      if (self->fds_.client != nullptr) {
        grpc_slice_buffer_reset_and_unref(&self->read_buffer_);
        grpc_endpoint_read(self->fds_.client, &self->read_buffer_,
                           &self->on_read_done_scheduler_, false,
                           /*min_progress_size=*/1);
        return;
      }
    }
    grpc_slice_buffer_destroy(&self->read_buffer_);
    self->read_end_notification_.Notify();
  }

  // Do async hop for OnReadDone() in case grpc_endpoint_read() invokes
  // us synchronously while we're holding the lock.
  static void OnReadDoneScheduler(void* arg, grpc_error_handle error) {
    GracefulShutdownTest* self = static_cast<GracefulShutdownTest*>(arg);
    ExecCtx::Run(DEBUG_LOCATION, &self->on_read_done_, std::move(error));
  }

  // Waits for \a bytes to show up in read_bytes_
  void WaitForReadBytes(absl::string_view bytes) {
    auto start_time = absl::Now();
    MutexLock lock(&mu_);
    while (true) {
      auto where = read_bytes_.find(std::string(bytes));
      if (where != std::string::npos) {
        read_bytes_ = read_bytes_.substr(where + bytes.size());
        break;
      }
      ASSERT_LT(absl::Now() - start_time, absl::Seconds(60))
          << "Timeout reading bytes";
      read_cv_.WaitWithTimeout(&mu_, absl::Seconds(5));
    }
  }

  std::string WaitForNBytes(size_t bytes) {
    auto start_time = absl::Now();
    MutexLock lock(&mu_);
    while (read_bytes_.size() < bytes) {
      EXPECT_LT(absl::Now() - start_time, absl::Seconds(60));
      read_cv_.WaitWithTimeout(&mu_, absl::Seconds(5));
    }
    std::string result = read_bytes_.substr(0, bytes);
    read_bytes_ = read_bytes_.substr(bytes);
    return result;
  }

  void WaitForClose() {
    ASSERT_TRUE(read_end_notification_.WaitForNotificationWithTimeout(
        absl::Minutes(1)));
  }

  void WaitForGoaway(uint32_t last_stream_id, uint32_t error_code,
                     grpc_slice message) {
    grpc_slice_buffer buffer;
    grpc_slice_buffer_init(&buffer);
    grpc_chttp2_goaway_append(last_stream_id, error_code, message, &buffer,
                              &http2_ztrace_collector_);
    std::string expected_bytes;
    for (size_t i = 0; i < buffer.count; ++i) {
      absl::StrAppend(&expected_bytes, StringViewFromSlice(buffer.slices[i]));
    }
    grpc_slice_buffer_destroy(&buffer);
    WaitForReadBytes(expected_bytes);
  }

  uint64_t WaitForPing() {
    grpc_slice ping_slice = grpc_chttp2_ping_create(0, 0);
    auto whole_ping = StringViewFromSlice(ping_slice);
    CHECK(whole_ping.size() == 9 + 8);
    WaitForReadBytes(whole_ping.substr(0, 9));
    std::string ping = WaitForNBytes(8);
    return (static_cast<uint64_t>(static_cast<uint8_t>(ping[0])) << 56) |
           (static_cast<uint64_t>(static_cast<uint8_t>(ping[1])) << 48) |
           (static_cast<uint64_t>(static_cast<uint8_t>(ping[2])) << 40) |
           (static_cast<uint64_t>(static_cast<uint8_t>(ping[3])) << 32) |
           (static_cast<uint64_t>(static_cast<uint8_t>(ping[4])) << 24) |
           (static_cast<uint64_t>(static_cast<uint8_t>(ping[5])) << 16) |
           (static_cast<uint64_t>(static_cast<uint8_t>(ping[6])) << 8) |
           (static_cast<uint64_t>(static_cast<uint8_t>(ping[7])));
  }

  void SendPingAck(uint64_t opaque_data) {
    grpc_slice ping_slice = grpc_chttp2_ping_create(1, opaque_data);
    Write(StringViewFromSlice(ping_slice));
  }

  // This is a blocking call. It waits for the write callback to be invoked
  // before returning. (In other words, do not call this from a thread that
  // should not be blocked, for example, a polling thread.)
  void Write(absl::string_view bytes) {
    ExecCtx exec_ctx;
    grpc_slice slice =
        StaticSlice::FromStaticBuffer(bytes.data(), bytes.size()).TakeCSlice();
    grpc_slice_buffer buffer;
    grpc_slice_buffer_init(&buffer);
    grpc_slice_buffer_add(&buffer, slice);
    WriteBuffer(&buffer);
    grpc_slice_buffer_destroy(&buffer);
  }

  void WriteBuffer(grpc_slice_buffer* buffer) {
    Notification on_write_done_notification_;
    GRPC_CLOSURE_INIT(&on_write_done_, OnWriteDone,
                      &on_write_done_notification_, nullptr);
    grpc_endpoint_write(
        fds_.client, buffer, &on_write_done_,
        grpc_event_engine::experimental::EventEngine::Endpoint::WriteArgs());
    ExecCtx::Get()->Flush();
    CHECK(on_write_done_notification_.WaitForNotificationWithTimeout(
        absl::Seconds(5)));
  }

  static void OnWriteDone(void* arg, grpc_error_handle error) {
    if (!error.ok()) {
      Crash(absl::StrCat("Write failed: ", error.ToString()));
    }
    Notification* on_write_done_notification_ = static_cast<Notification*>(arg);
    on_write_done_notification_->Notify();
  }

  // Held when destroying fds_.client so we know not to start another read.
  Mutex ep_destroy_mu_;

  grpc_endpoint_pair fds_;
  grpc_server* server_ = nullptr;
  grpc_completion_queue* cq_ = nullptr;
  std::unique_ptr<CqVerifier> cqv_;
  std::unique_ptr<std::thread> client_poll_thread_;
  std::atomic<bool> shutdown_{false};
  grpc_closure on_read_done_;
  grpc_closure on_read_done_scheduler_;
  Mutex mu_;
  CondVar read_cv_;
  Notification read_end_notification_;
  grpc_slice_buffer read_buffer_;
  std::string read_bytes_ ABSL_GUARDED_BY(mu_);
  grpc_closure on_write_done_;
  Http2ZTraceCollector http2_ztrace_collector_;
};

TEST_F(GracefulShutdownTest, GracefulGoaway) {
  // Initiate shutdown on the server
  grpc_server_shutdown_and_notify(server_, cq_, Tag(1));
  // Wait for first goaway
  WaitForGoaway((1u << 31) - 1, /*error_code=*/0,
                /*message=*/
                Slice::FromStaticString("Server shutdown").TakeCSlice());
  // Wait for the ping
  uint64_t ping_id = WaitForPing();
  // Reply to the ping
  SendPingAck(ping_id);
  // Wait for final goaway
  WaitForGoaway(0, /*error_code=*/0,
                /*message=*/
                Slice::FromStaticString("Server shutdown").TakeCSlice());
  // The shutdown should successfully complete.
  cqv_->Expect(Tag(1), true);
  cqv_->Verify();
}

TEST_F(GracefulShutdownTest, RequestStartedBeforeFinalGoaway) {
  grpc_call_error error;
  grpc_call* s;
  grpc_call_details call_details;
  grpc_metadata_array request_metadata_recv;
  grpc_call_details_init(&call_details);
  grpc_metadata_array_init(&request_metadata_recv);
  error = grpc_server_request_call(server_, &s, &call_details,
                                   &request_metadata_recv, cq_, cq_, Tag(100));
  CHECK_EQ(error, GRPC_CALL_OK);
  // Initiate shutdown on the server
  grpc_server_shutdown_and_notify(server_, cq_, Tag(1));
  // Wait for first goaway
  WaitForGoaway((1u << 31) - 1, /*error_code=*/0,
                /*message=*/
                Slice::FromStaticString("Server shutdown").TakeCSlice());
  // Wait for the ping
  uint64_t ping_id = WaitForPing();
  // Start a request
  constexpr char kRequestFrame[] =
      "\x00\x00\xbe\x01\x05\x00\x00\x00\x01"
      "\x10\x05:path\x08/foo/bar"
      "\x10\x07:scheme\x04http"
      "\x10\x07:method\x04POST"
      "\x10\x0a:authority\x09localhost"
      "\x10\x0c"
      "content-type\x10"
      "application/grpc"
      "\x10\x14grpc-accept-encoding\x15identity,deflate,gzip"
      "\x10\x02te\x08trailers"
      "\x10\x0auser-agent\x17grpc-c/0.12.0.0 (linux)";
  Write(absl::string_view(kRequestFrame, sizeof(kRequestFrame) - 1));
  // Reply to the ping
  SendPingAck(ping_id);
  // Wait for final goaway with last stream ID 1 to show that the HTTP2
  // transport accepted the stream.
  WaitForGoaway(1, /*error_code=*/0,
                /*message=*/
                Slice::FromStaticString("Server shutdown").TakeCSlice());
  // TODO(yashykt): The surface layer automatically cancels calls received after
  // shutdown has been called. Once that is fixed, this should be a success.
  cqv_->Expect(Tag(100), false);
  // The shutdown should successfully complete.
  cqv_->Expect(Tag(1), true);
  cqv_->Verify();
  grpc_metadata_array_destroy(&request_metadata_recv);
  grpc_call_details_destroy(&call_details);
}

TEST_F(GracefulShutdownTest, RequestStartedAfterFinalGoawayIsIgnored) {
  // Start a request before shutdown to make sure that the connection stays
  // alive.
  grpc_call_error error;
  grpc_call* s;
  grpc_call_details call_details;
  grpc_metadata_array request_metadata_recv;
  grpc_call_details_init(&call_details);
  grpc_metadata_array_init(&request_metadata_recv);
  error = grpc_server_request_call(server_, &s, &call_details,
                                   &request_metadata_recv, cq_, cq_, Tag(100));
  CHECK_EQ(error, GRPC_CALL_OK);
  // Send the request from the client.
  constexpr char kRequestFrame[] =
      "\x00\x00\xbe\x01\x05\x00\x00\x00\x01"
      "\x10\x05:path\x08/foo/bar"
      "\x10\x07:scheme\x04http"
      "\x10\x07:method\x04POST"
      "\x10\x0a:authority\x09localhost"
      "\x10\x0c"
      "content-type\x10"
      "application/grpc"
      "\x10\x14grpc-accept-encoding\x15identity,deflate,gzip"
      "\x10\x02te\x08trailers"
      "\x10\x0auser-agent\x17grpc-c/0.12.0.0 (linux)";
  Write(absl::string_view(kRequestFrame, sizeof(kRequestFrame) - 1));
  cqv_->Expect(Tag(100), true);
  cqv_->Verify();

  // Initiate shutdown on the server
  grpc_server_shutdown_and_notify(server_, cq_, Tag(1));
  // Wait for first goaway
  WaitForGoaway((1u << 31) - 1, /*error_code=*/0,
                /*message=*/
                Slice::FromStaticString("Server shutdown").TakeCSlice());
  // Wait for the ping
  uint64_t ping_id = WaitForPing();
  // Reply to the ping
  SendPingAck(ping_id);
  // Wait for final goaway
  WaitForGoaway(1, /*error_code=*/0,
                /*message=*/
                Slice::FromStaticString("Server shutdown").TakeCSlice());

  // Send another request from the client which should be ignored.
  constexpr char kNewRequestFrame[] =
      "\x00\x00\xbe\x01\x05\x00\x00\x00\x03"
      "\x10\x05:path\x08/foo/bar"
      "\x10\x07:scheme\x04http"
      "\x10\x07:method\x04POST"
      "\x10\x0a:authority\x09localhost"
      "\x10\x0c"
      "content-type\x10"
      "application/grpc"
      "\x10\x14grpc-accept-encoding\x15identity,deflate,gzip"
      "\x10\x02te\x08trailers"
      "\x10\x0auser-agent\x17grpc-c/0.12.0.0 (linux)";
  Write(absl::string_view(kNewRequestFrame, sizeof(kNewRequestFrame) - 1));

  // Finish the accepted request.
  grpc_op ops[3];
  grpc_op* op;
  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_SEND_STATUS_FROM_SERVER;
  op->data.send_status_from_server.trailing_metadata_count = 0;
  op->data.send_status_from_server.status = GRPC_STATUS_UNIMPLEMENTED;
  grpc_slice status_details = grpc_slice_from_static_string("xyz");
  op->data.send_status_from_server.status_details = &status_details;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_RECV_CLOSE_ON_SERVER;
  int was_cancelled = 2;
  op->data.recv_close_on_server.cancelled = &was_cancelled;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  error = grpc_call_start_batch(s, ops, static_cast<size_t>(op - ops), Tag(101),
                                nullptr);
  CHECK_EQ(error, GRPC_CALL_OK);
  cqv_->Expect(Tag(101), true);
  // The shutdown should successfully complete.
  cqv_->Expect(Tag(1), true);
  cqv_->Verify();
  grpc_call_unref(s);
  grpc_metadata_array_destroy(&request_metadata_recv);
  grpc_call_details_destroy(&call_details);
}

// Make sure that the graceful goaway eventually makes progress even if a client
// does not respond to the ping.
TEST_F(GracefulShutdownTest, UnresponsiveClient) {
  absl::Time initial_time = absl::Now();
  // Initiate shutdown on the server
  grpc_server_shutdown_and_notify(server_, cq_, Tag(1));
  // Wait for first goaway
  WaitForGoaway((1u << 31) - 1, /*error_code=*/0,
                /*message=*/
                Slice::FromStaticString("Server shutdown").TakeCSlice());
  // Wait for the ping
  std::ignore = WaitForPing();
  // Wait for final goaway without sending a ping ACK.
  WaitForClose();
  EXPECT_GE(absl::Now() - initial_time,
            absl::Seconds(2) -
                absl::Seconds(
                    1) /* clock skew between threads due to time caching */);
  // The shutdown should successfully complete.
  cqv_->Expect(Tag(1), true);
  cqv_->Verify();
}

// Test that servers send a GOAWAY with the last stream ID even when the
// transport is disconnected without letting Graceful GOAWAY complete
// successfully.
TEST_F(GracefulShutdownTest, GoawayReceivedOnServerDisconnect) {
  // Initiate shutdown on the server and immediately disconnect.
  grpc_server_shutdown_and_notify(server_, cq_, Tag(1));
  grpc_server_cancel_all_calls(server_);
  // Wait for final goaway.
  WaitForGoaway(/*last_stream_id=*/0, /*error_code=*/2,
                grpc_slice_from_static_string("Cancelling all calls"));
  // The shutdown should successfully complete.
  cqv_->Expect(Tag(1), true);
  cqv_->Verify();
}

}  // namespace
}  // namespace grpc_core

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc_init();
  int result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}
