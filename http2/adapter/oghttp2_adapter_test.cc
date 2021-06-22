#include "http2/adapter/oghttp2_adapter.h"

#include "http2/adapter/mock_http2_visitor.h"
#include "http2/adapter/test_frame_sequence.h"
#include "http2/adapter/test_utils.h"
#include "common/platform/api/quiche_test.h"
#include "common/platform/api/quiche_test_helpers.h"

namespace http2 {
namespace adapter {
namespace test {
namespace {

using testing::_;

enum FrameType {
  DATA,
  HEADERS,
  PRIORITY,
  RST_STREAM,
  SETTINGS,
  PUSH_PROMISE,
  PING,
  GOAWAY,
  WINDOW_UPDATE,
};

using spdy::SpdyFrameType;

class OgHttp2AdapterTest : public testing::Test {
 protected:
  void SetUp() override {
    OgHttp2Adapter::Options options{.perspective = Perspective::kServer};
    adapter_ = OgHttp2Adapter::Create(http2_visitor_, options);
  }

  DataSavingVisitor http2_visitor_;
  std::unique_ptr<OgHttp2Adapter> adapter_;
};

TEST_F(OgHttp2AdapterTest, IsServerSession) {
  EXPECT_TRUE(adapter_->IsServerSession());
}

TEST_F(OgHttp2AdapterTest, ProcessBytes) {
  testing::InSequence seq;
  EXPECT_CALL(http2_visitor_, OnFrameHeader(0, 0, 4, 0));
  EXPECT_CALL(http2_visitor_, OnSettingsStart());
  EXPECT_CALL(http2_visitor_, OnSettingsEnd());
  EXPECT_CALL(http2_visitor_, OnFrameHeader(0, 8, 6, 0));
  EXPECT_CALL(http2_visitor_, OnPing(17, false));
  adapter_->ProcessBytes(
      TestFrameSequence().ClientPreface().Ping(17).Serialize());
}

TEST(OgHttp2AdapterClientTest, ClientHandlesTrailers) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options{.perspective = Perspective::kClient};
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<const Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const char* kSentinel1 = "arbitrary pointer 1";
  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, nullptr, const_cast<char*>(kSentinel1));
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data, EqualsFrames({spdy::SpdyFrameType::SETTINGS,
                                  spdy::SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Data(1, "This is the response body.")
          .Headers(1, {{"final-status", "A-OK"}},
                   /*fin=*/true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 26, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 26));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the response body."));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "final-status", "A-OK"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::NO_ERROR));

  const ssize_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), stream_result);

  EXPECT_TRUE(adapter->session().want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({spdy::SpdyFrameType::SETTINGS}));
}

// TODO(birenroy): Validate headers and re-enable this test.
TEST(OgHttp2AdapterClientTest, DISABLED_ClientHandlesInvalidTrailers) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options{.perspective = Perspective::kClient};
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<const Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const char* kSentinel1 = "arbitrary pointer 1";
  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, nullptr, const_cast<char*>(kSentinel1));
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data, EqualsFrames({spdy::SpdyFrameType::SETTINGS,
                                  spdy::SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Data(1, "This is the response body.")
          .Headers(1, {{":bad-status", "9000"}},
                   /*fin=*/true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 26, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 26));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the response body."));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));

  // Bad status trailer will cause a PROTOCOL_ERROR. The header is never
  // delivered in an OnHeaderForStream callback.
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::PROTOCOL_ERROR));

  const ssize_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), stream_result);

  EXPECT_TRUE(adapter->session().want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({spdy::SpdyFrameType::SETTINGS,
                                            spdy::SpdyFrameType::RST_STREAM}));
}

TEST_F(OgHttp2AdapterTest, SubmitMetadata) {
  EXPECT_QUICHE_BUG(adapter_->SubmitMetadata(3, true), "Not implemented");
}

TEST_F(OgHttp2AdapterTest, GetSendWindowSize) {
  const int peer_window = adapter_->GetSendWindowSize();
  EXPECT_EQ(peer_window, kInitialFlowControlWindowSize);
}

TEST_F(OgHttp2AdapterTest, MarkDataConsumedForStream) {
  EXPECT_QUICHE_BUG(adapter_->MarkDataConsumedForStream(1, 11),
                    "Stream 1 not found");
}

TEST_F(OgHttp2AdapterTest, TestSerialize) {
  EXPECT_TRUE(adapter_->session().want_read());
  EXPECT_FALSE(adapter_->session().want_write());

  adapter_->SubmitSettings(
      {{HEADER_TABLE_SIZE, 128}, {MAX_FRAME_SIZE, 128 << 10}});
  EXPECT_TRUE(adapter_->session().want_write());

  adapter_->SubmitPriorityForStream(3, 1, 255, true);
  adapter_->SubmitRst(3, Http2ErrorCode::CANCEL);
  adapter_->SubmitPing(42);
  adapter_->SubmitGoAway(13, Http2ErrorCode::NO_ERROR, "");
  adapter_->SubmitWindowUpdate(3, 127);
  EXPECT_TRUE(adapter_->session().want_write());

  int result = adapter_->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(
      http2_visitor_.data(),
      EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::PRIORITY,
                    SpdyFrameType::RST_STREAM, SpdyFrameType::PING,
                    SpdyFrameType::GOAWAY, SpdyFrameType::WINDOW_UPDATE}));
  EXPECT_FALSE(adapter_->session().want_write());
}

TEST_F(OgHttp2AdapterTest, TestPartialSerialize) {
  EXPECT_FALSE(adapter_->session().want_write());

  adapter_->SubmitSettings(
      {{HEADER_TABLE_SIZE, 128}, {MAX_FRAME_SIZE, 128 << 10}});
  adapter_->SubmitGoAway(13, Http2ErrorCode::NO_ERROR, "And don't come back!");
  adapter_->SubmitPing(42);
  EXPECT_TRUE(adapter_->session().want_write());

  http2_visitor_.set_send_limit(20);
  int result = adapter_->Send();
  EXPECT_EQ(0, result);
  EXPECT_TRUE(adapter_->session().want_write());
  result = adapter_->Send();
  EXPECT_EQ(0, result);
  EXPECT_TRUE(adapter_->session().want_write());
  result = adapter_->Send();
  EXPECT_EQ(0, result);
  EXPECT_FALSE(adapter_->session().want_write());
  EXPECT_THAT(http2_visitor_.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY,
                            SpdyFrameType::PING}));
}

TEST(OgHttp2AdapterServerTest, ServerSendsInvalidTrailers) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options{.perspective = Perspective::kServer};
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->session().want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const ssize_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), result);

  const absl::string_view kBody = "This is an example response body.";

  // The body source must indicate that the end of the body is not the end of
  // the stream.
  auto body1 =
      absl::make_unique<TestDataFrameSource>(visitor, kBody, /*has_fin=*/false);
  int submit_result = adapter->SubmitResponse(
      1, ToHeaders({{":status", "200"}, {"x-comment", "Sure, sounds good."}}),
      std::move(body1));
  EXPECT_EQ(submit_result, 0);
  EXPECT_TRUE(adapter->session().want_write());
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::NO_ERROR));
  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames(
                  {spdy::SpdyFrameType::SETTINGS, spdy::SpdyFrameType::SETTINGS,
                   spdy::SpdyFrameType::HEADERS, spdy::SpdyFrameType::DATA}));
  EXPECT_THAT(visitor.data(), testing::HasSubstr(kBody));
  visitor.Clear();
  EXPECT_FALSE(adapter->session().want_write());

  // The body source has been exhausted by the call to Send() above.
  int trailer_result =
      adapter->SubmitTrailer(1, ToHeaders({{":final-status", "a-ok"}}));
  ASSERT_EQ(trailer_result, 0);
  EXPECT_TRUE(adapter->session().want_write());

  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({spdy::SpdyFrameType::HEADERS}));
}

}  // namespace
}  // namespace test
}  // namespace adapter
}  // namespace http2
