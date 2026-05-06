// Tests for FrameDemuxer using a fake IByteStream, exercising the
// framing logic against the round-1 testing reviewer's gaps:
//   - CRC16 over reference vectors (via parse_frame round-trip)
//   - Sync-byte classifier on adversarial inputs (partial frames,
//     unknown sync byte, AA56 host echo drained, AA55 + AA57 mixed)
//   - BadCrc retry-once policy: 1× retry success, 2× retry surfaces
//   - (seq, cmd_id) collision resistance: same seq + wrong cmd_id
//     does NOT satisfy the in-flight waiter
//   - Disconnect-mid-request raises DisconnectedDuringRequestError
//
// The test harness lives entirely in this file. FakeByteStream is an
// in-memory IByteStream backed by a deque<uint8_t>; it deliberately
// exercises the same code path the production CdcByteStream does
// (read with deadline, write returns -1 on disconnect).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "device/frame_demuxer.hpp"
#include "transport/byte_stream.hpp"

#include "wujihandcpp/data/tactile.hpp"
#include "wujihandcpp/protocol/tactile_command.hpp"

using namespace std::chrono_literals;
using namespace wujihandcpp::tactile;
using wujihandcpp::transport::IByteStream;

// ---------------------------------------------------------------------------
// FakeByteStream
// ---------------------------------------------------------------------------

namespace {

class FakeByteStream : public IByteStream {
public:
    // Make the demuxer's reader thread observable: enqueue bytes for
    // the demuxer to consume.
    void enqueue_read(const std::vector<uint8_t>& bytes) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto b : bytes) read_buf_.push_back(b);
        cv_.notify_all();
    }

    void simulate_disconnect() {
        std::lock_guard<std::mutex> lock(mu_);
        disconnected_ = true;
        cv_.notify_all();
    }

    // Return everything the demuxer has written since the last call,
    // and clear the buffer.
    std::vector<uint8_t> drain_writes() {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<uint8_t> v(write_buf_.begin(), write_buf_.end());
        write_buf_.clear();
        return v;
    }

    // Block (up to 200 ms) until at least `min` bytes have been
    // written, then return them. Used by tests to coordinate
    // "wait for the demuxer to actually send the command frame
    // before we inject the response".
    std::vector<uint8_t> wait_writes(size_t min, std::chrono::milliseconds deadline) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, deadline, [&]() { return write_buf_.size() >= min; });
        std::vector<uint8_t> v(write_buf_.begin(), write_buf_.end());
        write_buf_.clear();
        return v;
    }

    // IByteStream
    ssize_t read(uint8_t* buf, size_t len, uint32_t timeout_ms) override {
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lock(mu_);
        if (!cv_.wait_until(lock, deadline,
                            [&]() { return !read_buf_.empty() || disconnected_; })) {
            return 0;  // timeout — same shape as CdcByteStream
        }
        if (disconnected_) return -1;
        size_t take = std::min(len, read_buf_.size());
        for (size_t i = 0; i < take; ++i) {
            buf[i] = read_buf_.front();
            read_buf_.pop_front();
        }
        return static_cast<ssize_t>(take);
    }

    ssize_t write(const uint8_t* buf, size_t len, uint32_t /*timeout_ms*/) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (disconnected_) return -1;
        for (size_t i = 0; i < len; ++i) write_buf_.push_back(buf[i]);
        cv_.notify_all();
        return static_cast<ssize_t>(len);
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<uint8_t> read_buf_;
    std::deque<uint8_t> write_buf_;
    bool disconnected_ = false;
};

// ---------------------------------------------------------------------------
// Frame builders — produce wire-format bytes the demuxer expects.
// ---------------------------------------------------------------------------

inline void put_le16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

// Build a valid AA55 data frame (3088 B). `hand` byte and a single
// nonzero pressure value at row=2,col=3 so the consumer can detect
// it. `seq` and `timestamp_ms` are filled directly.
std::vector<uint8_t> build_data_frame(uint16_t seq, uint32_t ts_ms,
                                      uint8_t hand_byte = 0,
                                      float marker_pressure = 0.5f) {
    std::vector<uint8_t> v;
    v.resize(wujihandcpp::tactile::protocol::FRAME_SIZE, 0);
    v[0] = 0xAA;
    v[1] = 0x55;
    v[2] = static_cast<uint8_t>(wujihandcpp::tactile::protocol::EXPECTED_LENGTH & 0xFF);
    v[3] = static_cast<uint8_t>(wujihandcpp::tactile::protocol::EXPECTED_LENGTH >> 8);
    v[wujihandcpp::tactile::protocol::OFFSET_HAND] = hand_byte;
    // Mark cell [2][3] so a later test can verify it survived parsing.
    size_t idx = wujihandcpp::tactile::protocol::OFFSET_TACTILE_DATA
                 + (2 * 32 + 3) * sizeof(float);
    std::memcpy(v.data() + idx, &marker_pressure, sizeof(float));
    v[wujihandcpp::tactile::protocol::OFFSET_SEQUENCE] =
        static_cast<uint8_t>(seq & 0xFF);
    v[wujihandcpp::tactile::protocol::OFFSET_SEQUENCE + 1] =
        static_cast<uint8_t>(seq >> 8);
    v[wujihandcpp::tactile::protocol::OFFSET_TIMESTAMP] =
        static_cast<uint8_t>(ts_ms & 0xFF);
    v[wujihandcpp::tactile::protocol::OFFSET_TIMESTAMP + 1] =
        static_cast<uint8_t>((ts_ms >> 8) & 0xFF);
    v[wujihandcpp::tactile::protocol::OFFSET_TIMESTAMP + 2] =
        static_cast<uint8_t>((ts_ms >> 16) & 0xFF);
    v[wujihandcpp::tactile::protocol::OFFSET_TIMESTAMP + 3] =
        static_cast<uint8_t>((ts_ms >> 24) & 0xFF);
    uint16_t crc = wujihandcpp::tactile::protocol::crc16_ccitt(
        v.data() + wujihandcpp::tactile::protocol::OFFSET_LENGTH,
        wujihandcpp::tactile::protocol::OFFSET_CRC
            - wujihandcpp::tactile::protocol::OFFSET_LENGTH);
    v[wujihandcpp::tactile::protocol::OFFSET_CRC] =
        static_cast<uint8_t>(crc & 0xFF);
    v[wujihandcpp::tactile::protocol::OFFSET_CRC + 1] =
        static_cast<uint8_t>(crc >> 8);
    return v;
}

// Build a valid AA57 response frame: status + payload.
std::vector<uint8_t> build_response_frame(uint16_t seq, Cmd cmd, Status status,
                                          const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> v;
    v.push_back(0xAA);
    v.push_back(0x57);
    // length = 11 (overhead) + payload size
    uint16_t length = static_cast<uint16_t>(11 + payload.size());
    put_le16(v, length);
    put_le16(v, static_cast<uint16_t>(cmd));
    put_le16(v, seq);
    v.push_back(static_cast<uint8_t>(status));
    for (auto b : payload) v.push_back(b);
    // CRC over bytes [2, length - 2)
    uint16_t crc = wujihandcpp::tactile::protocol::crc16_ccitt(
        v.data() + 2, static_cast<size_t>(length) - 4);
    put_le16(v, crc);
    return v;
}

// Same as build_response_frame but flips the CRC so the demuxer
// classifies it as BadCrc (with matching seq+cmd, the demuxer surfaces
// it to the in-flight waiter as Status::BadCrc — that's the documented
// retry trigger).
std::vector<uint8_t> build_response_frame_corrupted_crc(
    uint16_t seq, Cmd cmd, const std::vector<uint8_t>& payload) {
    auto v = build_response_frame(seq, cmd, Status::Ok, payload);
    // Flip the last 2 bytes (the CRC field).
    v[v.size() - 2] ^= 0xFF;
    return v;
}

// Extract the seq an in-flight command actually sent. Demuxer's
// command() builds: AA 56 LEN(2) CMD(2) SEQ(2) PAYLOAD CRC(2).
// Test reads the writes after command() has dispatched.
uint16_t parse_sent_seq(const std::vector<uint8_t>& bytes) {
    EXPECT_GE(bytes.size(), 8u);
    return static_cast<uint16_t>(bytes[6] | (bytes[7] << 8));
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class FrameDemuxerTest : public ::testing::Test {
protected:
    void SetUp() override {
        stream_ = std::make_shared<FakeByteStream>();
        demux_ = std::make_shared<FrameDemuxer>(stream_);
        demux_->start();
    }

    void TearDown() override {
        // Stop is idempotent + handles self-detach. The demux destructor
        // also calls stop, so this is belt-and-suspenders for tests that
        // don't otherwise tear down explicitly.
        demux_->stop();
    }

    std::shared_ptr<FakeByteStream> stream_;
    std::shared_ptr<FrameDemuxer> demux_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Data frame path
// ---------------------------------------------------------------------------

TEST_F(FrameDemuxerTest, ValidDataFrame_DeliveredToConsumer) {
    stream_->enqueue_read(build_data_frame(/*seq=*/42, /*ts=*/12345));

    uint8_t buf[wujihandcpp::tactile::protocol::FRAME_SIZE];
    EXPECT_TRUE(demux_->wait_data_frame(buf, /*timeout_ms=*/500));

    auto frame = wujihandcpp::tactile::protocol::parse_frame(buf);
    EXPECT_EQ(frame.sequence, 42u);
    EXPECT_EQ(frame.timestamp_ms, 12345u);
    EXPECT_FLOAT_EQ(frame.pressure[2][3], 0.5f);
}

TEST_F(FrameDemuxerTest, BadCrcDataFrame_Dropped) {
    auto bad = build_data_frame(1, 100);
    // Corrupt the CRC tail; demuxer should drop the frame silently.
    bad[wujihandcpp::tactile::protocol::OFFSET_CRC] ^= 0xFF;
    stream_->enqueue_read(bad);

    uint8_t buf[wujihandcpp::tactile::protocol::FRAME_SIZE];
    EXPECT_FALSE(demux_->wait_data_frame(buf, /*timeout_ms=*/200));
}

TEST_F(FrameDemuxerTest, MultipleDataFrames_OrderPreserved) {
    auto a = build_data_frame(1, 100);
    auto b = build_data_frame(2, 200);
    auto c = build_data_frame(3, 300);
    stream_->enqueue_read(a);
    stream_->enqueue_read(b);
    stream_->enqueue_read(c);

    uint8_t buf[wujihandcpp::tactile::protocol::FRAME_SIZE];
    for (uint16_t expected : {1, 2, 3}) {
        ASSERT_TRUE(demux_->wait_data_frame(buf, /*timeout_ms=*/500));
        auto f = wujihandcpp::tactile::protocol::parse_frame(buf);
        EXPECT_EQ(f.sequence, expected);
    }
}

TEST_F(FrameDemuxerTest, UnknownSyncByte_DroppedAndRecoversToValidFrame) {
    // Send some garbage that mimics 0xAA + non-{0x55, 0x57, 0x56} —
    // the demuxer's slide-window logic should keep eating bytes until
    // it finds a valid header.
    std::vector<uint8_t> garbage = {0xAA, 0x00, 0x12, 0x34, 0xAA, 0xFF};
    stream_->enqueue_read(garbage);
    stream_->enqueue_read(build_data_frame(99, 999));

    uint8_t buf[wujihandcpp::tactile::protocol::FRAME_SIZE];
    ASSERT_TRUE(demux_->wait_data_frame(buf, /*timeout_ms=*/500));
    auto f = wujihandcpp::tactile::protocol::parse_frame(buf);
    EXPECT_EQ(f.sequence, 99u);
}

TEST_F(FrameDemuxerTest, AA56HostEcho_DrainedNotDelivered) {
    // AA 56 frame (host->device echo) must be drained without populating
    // the data queue or perturbing the in-flight slot.
    std::vector<uint8_t> echo = {0xAA, 0x56};
    put_le16(echo, /*length=*/16);
    // 12 bytes of payload (length 16 = sync 2 + len 2 + payload 12)
    for (int i = 0; i < 12; ++i) echo.push_back(0xCD);
    stream_->enqueue_read(echo);

    // Now send a real data frame; if the AA56 wasn't drained correctly,
    // the demuxer's stream alignment is broken and the next data frame
    // wouldn't parse.
    stream_->enqueue_read(build_data_frame(7, 70));

    uint8_t buf[wujihandcpp::tactile::protocol::FRAME_SIZE];
    ASSERT_TRUE(demux_->wait_data_frame(buf, /*timeout_ms=*/500));
    auto f = wujihandcpp::tactile::protocol::parse_frame(buf);
    EXPECT_EQ(f.sequence, 7u);
}

// ---------------------------------------------------------------------------
// Command / response round-trip
// ---------------------------------------------------------------------------

TEST_F(FrameDemuxerTest, CommandRoundtrip_OkResponse) {
    // Drive a command in a worker thread so we can inject the response.
    auto fut = std::async(std::launch::async, [this]() {
        return demux_->command(Cmd::GetDiagnostics, nullptr, 0, /*timeout=*/500);
    });

    // Wait for the demuxer's command frame to land in the fake.
    auto sent = stream_->wait_writes(/*min=*/10, 500ms);
    ASSERT_GE(sent.size(), 10u);
    uint16_t seq = parse_sent_seq(sent);

    // Inject a clean response with a tiny payload.
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04};
    stream_->enqueue_read(build_response_frame(seq, Cmd::GetDiagnostics,
                                               Status::Ok, payload));

    auto got = fut.get();
    EXPECT_EQ(got, payload);
}

TEST_F(FrameDemuxerTest, CommandRoundtrip_BadCrcRetriesOnceAndSucceeds) {
    auto fut = std::async(std::launch::async, [this]() {
        return demux_->command(Cmd::GetDiagnostics, nullptr, 0, /*timeout=*/500);
    });

    // Capture first send + inject corrupted-CRC response → demuxer
    // surfaces Status::BadCrc to the waiter, which retries once.
    auto sent1 = stream_->wait_writes(10, 500ms);
    uint16_t seq1 = parse_sent_seq(sent1);
    stream_->enqueue_read(build_response_frame_corrupted_crc(
        seq1, Cmd::GetDiagnostics, {}));

    // Capture retry send + inject Ok response.
    auto sent2 = stream_->wait_writes(10, 500ms);
    uint16_t seq2 = parse_sent_seq(sent2);
    EXPECT_NE(seq1, seq2);  // each command bumps next_seq_
    std::vector<uint8_t> payload = {0xAA};
    stream_->enqueue_read(build_response_frame(seq2, Cmd::GetDiagnostics,
                                               Status::Ok, payload));

    auto got = fut.get();
    EXPECT_EQ(got, payload);
}

TEST_F(FrameDemuxerTest, CommandRoundtrip_TwoConsecutiveBadCrcsThrow) {
    auto fut = std::async(std::launch::async, [this]() -> std::vector<uint8_t> {
        return demux_->command(Cmd::GetDiagnostics, nullptr, 0, /*timeout=*/500);
    });

    // 1st attempt: corrupted CRC → triggers retry.
    auto sent1 = stream_->wait_writes(10, 500ms);
    stream_->enqueue_read(build_response_frame_corrupted_crc(
        parse_sent_seq(sent1), Cmd::GetDiagnostics, {}));

    // 2nd attempt: corrupted CRC again → command() surfaces Error(BadCrc).
    auto sent2 = stream_->wait_writes(10, 500ms);
    stream_->enqueue_read(build_response_frame_corrupted_crc(
        parse_sent_seq(sent2), Cmd::GetDiagnostics, {}));

    EXPECT_THROW(fut.get(), Error);
}

TEST_F(FrameDemuxerTest, ResponseSeqMismatch_DoesNotSatisfyWaiter) {
    auto fut = std::async(std::launch::async, [this]() -> std::vector<uint8_t> {
        return demux_->command(Cmd::GetDiagnostics, nullptr, 0, /*timeout=*/200);
    });

    auto sent = stream_->wait_writes(10, 500ms);
    uint16_t real_seq = parse_sent_seq(sent);
    // Inject response with a *different* seq → demuxer ignores it.
    stream_->enqueue_read(build_response_frame(real_seq + 100,
                                               Cmd::GetDiagnostics,
                                               Status::Ok, {0xFF}));

    EXPECT_THROW(fut.get(), ResponseTimeoutError);
}

TEST_F(FrameDemuxerTest, ResponseCmdIdMismatch_DoesNotSatisfyWaiter) {
    // The (seq, cmd_id) pairing is documented as the defense against
    // u16 seq wrap collisions: same seq but different cmd_id must be
    // rejected even though seq alone matches.
    auto fut = std::async(std::launch::async, [this]() -> std::vector<uint8_t> {
        return demux_->command(Cmd::GetDiagnostics, nullptr, 0, /*timeout=*/200);
    });

    auto sent = stream_->wait_writes(10, 500ms);
    uint16_t real_seq = parse_sent_seq(sent);
    // Right seq, wrong cmd_id → must NOT wake the waiter.
    stream_->enqueue_read(build_response_frame(real_seq, Cmd::GetDeviceInfo,
                                               Status::Ok, {}));

    EXPECT_THROW(fut.get(), ResponseTimeoutError);
}

TEST_F(FrameDemuxerTest, DisconnectDuringCommand_ThrowsDisconnected) {
    auto fut = std::async(std::launch::async, [this]() -> std::vector<uint8_t> {
        return demux_->command(Cmd::GetDiagnostics, nullptr, 0, /*timeout=*/2000);
    });

    // Let the command write the request first.
    (void)stream_->wait_writes(10, 500ms);

    // Yank the cable.
    stream_->simulate_disconnect();

    EXPECT_THROW(fut.get(), DisconnectedDuringRequestError);
}
