/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstring>
#include <folly/net/TcpInfo.h>
#include <folly/net/testing/MockNetOpsDispatcher.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

using namespace folly;
using namespace testing;

using us = std::chrono::microseconds;

// tests are only supported on Linux right now
#ifdef __linux__
#include <linux/sockios.h>
#include <sys/ioctl.h>

#include <folly/net/testing/TcpInfoTestUtil.h>

using namespace folly::test;

const auto kTestSiocoutqVal = 10U;
const auto kTestSiocinqVal = 100U;
const auto kTestUnknownCcName = "coolNewCCA"; // it's cool, new, and a CCA!

class TcpInfoTest : public Test {
 public:
  template <typename T1>
  void setupExpectCallTcpInfo(NetworkSocket& s, const T1& tInfo) {
    TcpInfoTestUtil::setupExpectCallTcpInfo(mockNetOpsDispatcher_, s, tInfo);
  }

  void setupExpectCallCcName(NetworkSocket& s, const std::string& ccName) {
    TcpInfoTestUtil::setupExpectCallCcName(mockNetOpsDispatcher_, s, ccName);
  }

  void setupExpectCallCcInfo(
      NetworkSocket& s, const folly::TcpInfo::tcp_cc_info& ccInfo) {
    TcpInfoTestUtil::setupExpectCallCcInfo(mockNetOpsDispatcher_, s, ccInfo);
  }

  struct ExpectCallMemInfoConfig {
    size_t siocoutq{0};
    size_t siocinq{0};
  };

  void setupExpectCallMemInfo(
      NetworkSocket& s, const ExpectCallMemInfoConfig& config) {
    EXPECT_CALL(mockIoctlDispatcher_, ioctl(s.toFd(), SIOCOUTQ, testing::_))
        .WillOnce(WithArgs<2>(Invoke([config](void* argp) {
          size_t* val = static_cast<size_t*>(argp);
          *val = config.siocoutq;
          return 0;
        })));
    EXPECT_CALL(mockIoctlDispatcher_, ioctl(s.toFd(), SIOCINQ, testing::_))
        .WillOnce(WithArgs<2>(Invoke([config](void* argp) {
          size_t* val = static_cast<size_t*>(argp);
          *val = config.siocinq;
          return 0;
        })));
  }

  static folly::TcpInfo::tcp_info getTestLatestTcpInfo() {
    folly::TcpInfo::tcp_info tInfo = {};
    tInfo.tcpi_state = 1;
    tInfo.tcpi_ca_state = 2;
    tInfo.tcpi_retransmits = 3;
    tInfo.tcpi_probes = 4;
    tInfo.tcpi_backoff = 5;
    tInfo.tcpi_options = 6;
    tInfo.tcpi_snd_wscale = 7;
    tInfo.tcpi_rcv_wscale = 8;
    tInfo.tcpi_delivery_rate_app_limited = 1;
    tInfo.tcpi_rto = 9;
    tInfo.tcpi_ato = 10;
    tInfo.tcpi_snd_mss = 11;
    tInfo.tcpi_rcv_mss = 12;
    tInfo.tcpi_unacked = 113; // 113 instead of 13 for packets in flight
    tInfo.tcpi_sacked = 14;
    tInfo.tcpi_lost = 15;
    tInfo.tcpi_retrans = 16;
    tInfo.tcpi_fackets = 17;
    tInfo.tcpi_last_data_sent = 18;
    tInfo.tcpi_last_ack_sent = 19;
    tInfo.tcpi_last_data_recv = 20;
    tInfo.tcpi_last_ack_recv = 21;
    tInfo.tcpi_pmtu = 22;
    tInfo.tcpi_rcv_ssthresh = 23;
    tInfo.tcpi_rtt = 24;
    tInfo.tcpi_rttvar = 25;
    tInfo.tcpi_snd_ssthresh = 26;
    tInfo.tcpi_snd_cwnd = 27;
    tInfo.tcpi_advmss = 28;
    tInfo.tcpi_reordering = 29;
    tInfo.tcpi_rcv_rtt = 30;
    tInfo.tcpi_rcv_space = 31;
    tInfo.tcpi_total_retrans = 32;
    tInfo.tcpi_pacing_rate = 33;
    tInfo.tcpi_max_pacing_rate = 34;
    tInfo.tcpi_bytes_acked = 35;
    tInfo.tcpi_bytes_received = 36;
    tInfo.tcpi_segs_out = 37;
    tInfo.tcpi_segs_in = 38;
    tInfo.tcpi_notsent_bytes = 39;
    tInfo.tcpi_min_rtt = 40;
    tInfo.tcpi_data_segs_in = 41;
    tInfo.tcpi_data_segs_out = 42;
    tInfo.tcpi_delivery_rate = 43;
    tInfo.tcpi_busy_time = 44;
    tInfo.tcpi_rwnd_limited = 45;
    tInfo.tcpi_sndbuf_limited = 46;
    tInfo.tcpi_delivered = 47;
    tInfo.tcpi_delivered_ce = 48;
    tInfo.tcpi_bytes_sent = 49;
    tInfo.tcpi_bytes_retrans = 50;
    tInfo.tcpi_dsack_dups = 51;
    tInfo.tcpi_reord_seen = 52;
    tInfo.tcpi_rcv_ooopack = 53;
    tInfo.tcpi_snd_wnd = 54;
    return tInfo;
  }

  static folly::detail::tcp_info_legacy getTestLegacyTcpInfo() {
    folly::detail::tcp_info_legacy tInfo = {};
    tInfo.tcpi_state = 1;
    tInfo.tcpi_ca_state = 2;
    tInfo.tcpi_retransmits = 3;
    tInfo.tcpi_probes = 4;
    tInfo.tcpi_backoff = 5;
    tInfo.tcpi_options = 6;
    tInfo.tcpi_snd_wscale = 7;
    tInfo.tcpi_rcv_wscale = 8;
    tInfo.tcpi_rto = 9;
    tInfo.tcpi_ato = 10;
    tInfo.tcpi_snd_mss = 11;
    tInfo.tcpi_rcv_mss = 12;
    tInfo.tcpi_unacked = 113; // 113 instead of 13 for packets in flight
    tInfo.tcpi_sacked = 14;
    tInfo.tcpi_lost = 15;
    tInfo.tcpi_retrans = 16;
    tInfo.tcpi_fackets = 17;
    tInfo.tcpi_last_data_sent = 18;
    tInfo.tcpi_last_ack_sent = 19;
    tInfo.tcpi_last_data_recv = 20;
    tInfo.tcpi_last_ack_recv = 21;
    tInfo.tcpi_pmtu = 22;
    tInfo.tcpi_rcv_ssthresh = 23;
    tInfo.tcpi_rtt = 24;
    tInfo.tcpi_rttvar = 25;
    tInfo.tcpi_snd_ssthresh = 26;
    tInfo.tcpi_snd_cwnd = 27;
    tInfo.tcpi_advmss = 28;
    tInfo.tcpi_reordering = 29;
    tInfo.tcpi_rcv_rtt = 30;
    tInfo.tcpi_rcv_space = 31;
    tInfo.tcpi_total_retrans = 32;
    return tInfo;
  }

  static folly::TcpInfo::tcp_cc_info getTestBbrInfo() {
    folly::TcpInfo::tcp_cc_info ccInfo = {};
    ccInfo.bbr.bbr_bw_lo = 1;
    ccInfo.bbr.bbr_bw_hi = 2;
    ccInfo.bbr.bbr_min_rtt = 3;
    ccInfo.bbr.bbr_pacing_gain = 4;
    ccInfo.bbr.bbr_cwnd_gain = 5;
    return ccInfo;
  }

  static folly::TcpInfo::tcp_cc_info getTestVegasInfo() {
    folly::TcpInfo::tcp_cc_info ccInfo = {};
    ccInfo.vegas.tcpv_enabled = 6;
    ccInfo.vegas.tcpv_rttcnt = 7;
    ccInfo.vegas.tcpv_rtt = 8;
    ccInfo.vegas.tcpv_minrtt = 9;
    return ccInfo;
  }

  static folly::TcpInfo::tcp_cc_info getTestDctcpInfo() {
    folly::TcpInfo::tcp_cc_info ccInfo = {};
    ccInfo.dctcp.dctcp_enabled = 10;
    ccInfo.dctcp.dctcp_ce_state = 11;
    ccInfo.dctcp.dctcp_alpha = 12;
    ccInfo.dctcp.dctcp_ab_ecn = 13;
    ccInfo.dctcp.dctcp_ab_tot = 14;
    return ccInfo;
  }

  static void checkNoTcpInfo(const TcpInfo& wrappedTcpInfo) {
    // check a few accessors to make sure nothing available
    EXPECT_FALSE(wrappedTcpInfo.bytesRetransmitted().has_value());
    EXPECT_FALSE(wrappedTcpInfo.packetsRetransmitted().has_value());
    EXPECT_FALSE(wrappedTcpInfo.srtt().has_value());
    EXPECT_FALSE(wrappedTcpInfo.cwndInBytes().has_value());
    EXPECT_FALSE(wrappedTcpInfo.cwndInPackets().has_value());
    EXPECT_FALSE(
        wrappedTcpInfo
            .getFieldAsOptUInt64(&folly::TcpInfo::tcp_info::tcpi_total_retrans)
            .has_value());
    EXPECT_FALSE(
        wrappedTcpInfo
            .getFieldAsOptUInt64(&folly::TcpInfo::tcp_info::tcpi_snd_cwnd)
            .has_value());
  }

  static void checkTcpInfoAgainstLegacy(const TcpInfo& wrappedTcpInfo) {
    const auto& controlTcpInfo = getTestLegacyTcpInfo();

    EXPECT_FALSE(wrappedTcpInfo.minrtt());
    EXPECT_EQ(us(controlTcpInfo.tcpi_rtt), wrappedTcpInfo.srtt());

    EXPECT_FALSE(wrappedTcpInfo.bytesSent());
    EXPECT_FALSE(wrappedTcpInfo.bytesReceived());
    EXPECT_FALSE(wrappedTcpInfo.bytesRetransmitted());
    EXPECT_FALSE(wrappedTcpInfo.bytesAcked());

    EXPECT_FALSE(wrappedTcpInfo.packetsSent());
    EXPECT_FALSE(wrappedTcpInfo.packetsWithDataSent());
    EXPECT_FALSE(wrappedTcpInfo.packetsReceived());
    EXPECT_FALSE(wrappedTcpInfo.packetsWithDataReceived());
    EXPECT_EQ(
        controlTcpInfo.tcpi_total_retrans,
        wrappedTcpInfo.packetsRetransmitted());

    EXPECT_EQ(
        controlTcpInfo.tcpi_unacked + controlTcpInfo.tcpi_retrans -
            (controlTcpInfo.tcpi_sacked + controlTcpInfo.tcpi_lost),
        wrappedTcpInfo.packetsInFlight());
    EXPECT_NE(0U, wrappedTcpInfo.packetsInFlight());

    EXPECT_EQ(controlTcpInfo.tcpi_snd_cwnd, wrappedTcpInfo.cwndInPackets());
    EXPECT_EQ(
        controlTcpInfo.tcpi_snd_cwnd * controlTcpInfo.tcpi_snd_mss,
        wrappedTcpInfo.cwndInBytes());
    EXPECT_EQ(controlTcpInfo.tcpi_snd_ssthresh, wrappedTcpInfo.ssthresh());
    EXPECT_EQ(controlTcpInfo.tcpi_snd_mss, wrappedTcpInfo.mss());

    EXPECT_FALSE(wrappedTcpInfo.deliveryRateBitsPerSecond());
    EXPECT_FALSE(wrappedTcpInfo.deliveryRateBytesPerSecond());
    EXPECT_FALSE(wrappedTcpInfo.deliveryRateAppLimited());

    // try using getTcpInfoFieldAsOpt to get one of the older fields
    // this field _should_ be available in legacy
    EXPECT_EQ(
        controlTcpInfo.tcpi_snd_cwnd,
        wrappedTcpInfo.getFieldAsOptUInt64(
            &folly::TcpInfo::tcp_info::tcpi_snd_cwnd));

    // try using getTcpInfoFieldAsOpt to get one of the newer fields
    // this field should _not_ be available in legacy
    EXPECT_FALSE(
        wrappedTcpInfo
            .getFieldAsOptUInt64(&folly::TcpInfo::tcp_info::tcpi_delivery_rate)
            .hasValue());
  }

  static void checkTcpInfoAgainstLatest(const TcpInfo& wrappedTcpInfo) {
    const auto& controlTcpInfo = getTestLatestTcpInfo();

    EXPECT_EQ(us(controlTcpInfo.tcpi_min_rtt), wrappedTcpInfo.minrtt());
    EXPECT_EQ(us(controlTcpInfo.tcpi_rtt), wrappedTcpInfo.srtt());

    EXPECT_EQ(controlTcpInfo.tcpi_bytes_sent, wrappedTcpInfo.bytesSent());
    EXPECT_EQ(
        controlTcpInfo.tcpi_bytes_received, wrappedTcpInfo.bytesReceived());
    EXPECT_EQ(
        controlTcpInfo.tcpi_bytes_retrans, wrappedTcpInfo.bytesRetransmitted());
    EXPECT_EQ(controlTcpInfo.tcpi_bytes_acked, wrappedTcpInfo.bytesAcked());

    EXPECT_EQ(controlTcpInfo.tcpi_segs_out, wrappedTcpInfo.packetsSent());
    EXPECT_EQ(
        controlTcpInfo.tcpi_data_segs_out,
        wrappedTcpInfo.packetsWithDataSent());
    EXPECT_EQ(controlTcpInfo.tcpi_segs_in, wrappedTcpInfo.packetsReceived());
    EXPECT_EQ(
        controlTcpInfo.tcpi_data_segs_in,
        wrappedTcpInfo.packetsWithDataReceived());
    EXPECT_EQ(
        controlTcpInfo.tcpi_total_retrans,
        wrappedTcpInfo.packetsRetransmitted());

    EXPECT_EQ(
        controlTcpInfo.tcpi_unacked + controlTcpInfo.tcpi_retrans -
            (controlTcpInfo.tcpi_sacked + controlTcpInfo.tcpi_lost),
        wrappedTcpInfo.packetsInFlight());
    EXPECT_NE(0U, wrappedTcpInfo.packetsInFlight());

    EXPECT_EQ(controlTcpInfo.tcpi_delivered, wrappedTcpInfo.packetsDelivered());
    EXPECT_EQ(
        controlTcpInfo.tcpi_delivered_ce,
        wrappedTcpInfo.packetsDeliveredWithCEMarks());

    EXPECT_EQ(controlTcpInfo.tcpi_snd_cwnd, wrappedTcpInfo.cwndInPackets());
    EXPECT_EQ(
        controlTcpInfo.tcpi_snd_cwnd * controlTcpInfo.tcpi_snd_mss,
        wrappedTcpInfo.cwndInBytes());
    EXPECT_EQ(controlTcpInfo.tcpi_snd_ssthresh, wrappedTcpInfo.ssthresh());
    EXPECT_EQ(controlTcpInfo.tcpi_snd_mss, wrappedTcpInfo.mss());

    EXPECT_EQ(
        controlTcpInfo.tcpi_delivery_rate * 8,
        wrappedTcpInfo.deliveryRateBitsPerSecond());
    EXPECT_EQ(
        controlTcpInfo.tcpi_delivery_rate,
        wrappedTcpInfo.deliveryRateBytesPerSecond());
    EXPECT_EQ(
        controlTcpInfo.tcpi_delivery_rate_app_limited,
        wrappedTcpInfo.deliveryRateAppLimited());

    // try using getFieldAsOptUInt64 directly
    EXPECT_EQ(
        controlTcpInfo.tcpi_delivery_rate,
        wrappedTcpInfo.getFieldAsOptUInt64(
            &folly::TcpInfo::tcp_info::tcpi_delivery_rate));
  }

  static void checkNoCcNameType(const TcpInfo& wrappedTcpInfo) {
    EXPECT_FALSE(wrappedTcpInfo.ccNameRaw().has_value());
    EXPECT_FALSE(wrappedTcpInfo.ccNameEnum().has_value());
    EXPECT_FALSE(wrappedTcpInfo.ccNameEnumAsStr().has_value());
  }

  static void checkNoCcInfo(const TcpInfo& wrappedTcpInfo) {
    // should get false for all three types
    EXPECT_FALSE(wrappedTcpInfo.bbrCwndGain().has_value());
    EXPECT_FALSE(
        wrappedTcpInfo
            .getFieldAsOptUInt64(&folly::TcpInfo::tcp_bbr_info::bbr_pacing_gain)
            .has_value());
    EXPECT_FALSE(
        wrappedTcpInfo
            .getFieldAsOptUInt64(&folly::TcpInfo::tcpvegas_info::tcpv_rtt)
            .has_value());
    EXPECT_FALSE(
        wrappedTcpInfo
            .getFieldAsOptUInt64(&folly::TcpInfo::tcp_dctcp_info::dctcp_alpha)
            .has_value());
  }

  static void checkCcFieldsAgainstBbr(const TcpInfo& wrappedTcpInfo) {
    EXPECT_EQ("BBR", wrappedTcpInfo.ccNameEnumAsStr());
    EXPECT_EQ(TcpInfo::CongestionControlName::BBR, wrappedTcpInfo.ccNameEnum());

    const auto controlCcInfo = getTestBbrInfo().bbr;
    EXPECT_EQ(
        controlCcInfo.bbr_pacing_gain,
        wrappedTcpInfo.getFieldAsOptUInt64(
            &folly::TcpInfo::tcp_bbr_info::bbr_pacing_gain));
    const uint64_t bbrBwBytesPerSecond =
        (uint64_t(controlCcInfo.bbr_bw_hi) << 32) + controlCcInfo.bbr_bw_lo;
    EXPECT_EQ(bbrBwBytesPerSecond, wrappedTcpInfo.bbrBwBytesPerSecond());
    EXPECT_EQ(bbrBwBytesPerSecond * 8, wrappedTcpInfo.bbrBwBitsPerSecond());
    EXPECT_EQ(us(controlCcInfo.bbr_min_rtt), wrappedTcpInfo.bbrMinrtt());
    EXPECT_EQ(controlCcInfo.bbr_pacing_gain, wrappedTcpInfo.bbrPacingGain());
    EXPECT_EQ(controlCcInfo.bbr_cwnd_gain, wrappedTcpInfo.bbrCwndGain());

    // try using getFieldAsOptUInt64 directly to get one of the BBR fields
    EXPECT_EQ(
        controlCcInfo.bbr_pacing_gain,
        wrappedTcpInfo.getFieldAsOptUInt64(
            &folly::TcpInfo::tcp_bbr_info::bbr_pacing_gain));

    // no CC info for the other types
    EXPECT_FALSE(wrappedTcpInfo.getFieldAsOptUInt64(
        &folly::TcpInfo::tcpvegas_info::tcpv_rtt));
    EXPECT_FALSE(wrappedTcpInfo.getFieldAsOptUInt64(
        &folly::TcpInfo::tcp_dctcp_info::dctcp_alpha));
  }

  static void checkCcFieldsAgainstVegas(const TcpInfo& wrappedTcpInfo) {
    EXPECT_EQ("VEGAS", wrappedTcpInfo.ccNameEnumAsStr());
    EXPECT_EQ(
        TcpInfo::CongestionControlName::VEGAS, wrappedTcpInfo.ccNameEnum());

    const auto controlCcInfo = getTestVegasInfo().vegas;
    EXPECT_EQ(
        controlCcInfo.tcpv_rtt,
        wrappedTcpInfo.getFieldAsOptUInt64(
            &folly::TcpInfo::tcpvegas_info::tcpv_rtt));

    // no CC info for the other types
    EXPECT_FALSE(wrappedTcpInfo.bbrCwndGain().has_value());
    EXPECT_FALSE(
        wrappedTcpInfo
            .getFieldAsOptUInt64(&folly::TcpInfo::tcp_bbr_info::bbr_pacing_gain)
            .has_value());
    EXPECT_FALSE(
        wrappedTcpInfo
            .getFieldAsOptUInt64(&folly::TcpInfo::tcp_dctcp_info::dctcp_alpha)
            .has_value());
  }

  static void checkCcFieldsAgainstDctcp(
      const TcpInfo& wrappedTcpInfo,
      const TcpInfo::CongestionControlName dctcpType) {
    switch (dctcpType) {
      case TcpInfo::CongestionControlName::DCTCP:
        EXPECT_EQ("DCTCP", wrappedTcpInfo.ccNameEnumAsStr());
        break;
      case TcpInfo::CongestionControlName::DCTCP_RENO:
        EXPECT_EQ("DCTCP_RENO", wrappedTcpInfo.ccNameEnumAsStr());
        break;
      case TcpInfo::CongestionControlName::DCTCP_CUBIC:
        EXPECT_EQ("DCTCP_CUBIC", wrappedTcpInfo.ccNameEnumAsStr());
        break;
      case TcpInfo::CongestionControlName::UNKNOWN:
      case TcpInfo::CongestionControlName::RENO:
      case TcpInfo::CongestionControlName::CUBIC:
      case TcpInfo::CongestionControlName::BIC:
      case TcpInfo::CongestionControlName::BBR:
      case TcpInfo::CongestionControlName::VEGAS:
      case TcpInfo::CongestionControlName::NumCcTypes:
        FAIL();
    }

    EXPECT_EQ(dctcpType, wrappedTcpInfo.ccNameEnum());

    const auto controlCcInfo = getTestDctcpInfo().dctcp;
    EXPECT_EQ(
        controlCcInfo.dctcp_alpha,
        wrappedTcpInfo.getFieldAsOptUInt64(
            &folly::TcpInfo::tcp_dctcp_info::dctcp_alpha));
    EXPECT_EQ(
        controlCcInfo.dctcp_enabled,
        wrappedTcpInfo.getFieldAsOptUInt64(
            &folly::TcpInfo::tcp_dctcp_info::dctcp_enabled));

    // no CC info for the other types
    EXPECT_FALSE(wrappedTcpInfo.bbrCwndGain().has_value());
    EXPECT_FALSE(
        wrappedTcpInfo
            .getFieldAsOptUInt64(&folly::TcpInfo::tcp_bbr_info::bbr_pacing_gain)
            .has_value());
    EXPECT_FALSE(
        wrappedTcpInfo
            .getFieldAsOptUInt64(&folly::TcpInfo::tcpvegas_info::tcpv_rtt)
            .has_value());
  }

  static void checkNoMemoryInfo(const TcpInfo& wrappedTcpInfo) {
    EXPECT_FALSE(wrappedTcpInfo.sendBufInUseBytes().has_value()); // siocoutq
    EXPECT_FALSE(wrappedTcpInfo.recvBufInUseBytes().has_value()); // siocinq
  }

 protected:
  StrictMock<folly::netops::test::MockDispatcher> mockNetOpsDispatcher_;
  StrictMock<TcpInfoTestUtil::MockIoctlDispatcher> mockIoctlDispatcher_;
};

TEST_F(TcpInfoTest, LegacyStruct) {
  NetworkSocket s(0);
  setupExpectCallTcpInfo(s, getTestLegacyTcpInfo());

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = false;
  options.getMemInfo = false;
  auto wrappedTcpInfoExpect =
      TcpInfo::initFromFd(s, options, mockNetOpsDispatcher_);
  ASSERT_TRUE(wrappedTcpInfoExpect.hasValue());
  const auto& wrappedTcpInfo = wrappedTcpInfoExpect.value();

  checkTcpInfoAgainstLegacy(wrappedTcpInfo);

  // no CC name/type or info; no memory info
  checkNoCcNameType(wrappedTcpInfo);
  checkNoCcInfo(wrappedTcpInfo);
  checkNoMemoryInfo(wrappedTcpInfo);
}

TEST_F(TcpInfoTest, LatestStruct) {
  NetworkSocket s(0);
  setupExpectCallTcpInfo(s, getTestLatestTcpInfo());

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = false;
  options.getMemInfo = false;
  auto wrappedTcpInfoExpect =
      TcpInfo::initFromFd(s, options, mockNetOpsDispatcher_);
  ASSERT_TRUE(wrappedTcpInfoExpect.hasValue());
  const auto& wrappedTcpInfo = wrappedTcpInfoExpect.value();

  checkTcpInfoAgainstLatest(wrappedTcpInfo);

  // no CC name/type or info; no memory info
  checkNoCcNameType(wrappedTcpInfo);
  checkNoCcInfo(wrappedTcpInfo);
  checkNoMemoryInfo(wrappedTcpInfo);
}

TEST_F(TcpInfoTest, ConstructorWithLatestTcpInfo) {
  TcpInfo wrappedTcpInfo{getTestLatestTcpInfo()};
  checkTcpInfoAgainstLatest(wrappedTcpInfo);
}

TEST_F(TcpInfoTest, LatestStructWithCcInfo) {
  NetworkSocket s(0);
  setupExpectCallTcpInfo(s, getTestLatestTcpInfo());
  setupExpectCallCcName(s, "bbr");
  setupExpectCallCcInfo(s, getTestBbrInfo());

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = true;
  options.getMemInfo = false;
  auto wrappedTcpInfoExpect = TcpInfo::initFromFd(
      s, options, mockNetOpsDispatcher_, mockIoctlDispatcher_);
  ASSERT_TRUE(wrappedTcpInfoExpect.hasValue());
  const auto& wrappedTcpInfo = wrappedTcpInfoExpect.value();

  checkTcpInfoAgainstLatest(wrappedTcpInfo);
  checkCcFieldsAgainstBbr(wrappedTcpInfo);

  // no memory info
  checkNoMemoryInfo(wrappedTcpInfo);
}

TEST_F(TcpInfoTest, LatestStructWithMemInfo) {
  NetworkSocket s(0);
  setupExpectCallTcpInfo(s, getTestLatestTcpInfo());
  setupExpectCallMemInfo(
      s,
      ExpectCallMemInfoConfig{
          .siocoutq = kTestSiocoutqVal, .siocinq = kTestSiocinqVal});

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = false;
  options.getMemInfo = true;
  auto wrappedTcpInfoExpect = TcpInfo::initFromFd(
      s, options, mockNetOpsDispatcher_, mockIoctlDispatcher_);
  ASSERT_TRUE(wrappedTcpInfoExpect.hasValue());
  const auto& wrappedTcpInfo = wrappedTcpInfoExpect.value();

  checkTcpInfoAgainstLatest(wrappedTcpInfo);
  EXPECT_EQ(kTestSiocoutqVal, wrappedTcpInfo.sendBufInUseBytes()); // siocoutq
  EXPECT_EQ(kTestSiocinqVal, wrappedTcpInfo.recvBufInUseBytes()); // siocinq

  // no CC name/type or info
  checkNoCcNameType(wrappedTcpInfo);
  checkNoCcInfo(wrappedTcpInfo);
}

TEST_F(TcpInfoTest, LatestStructWithCcInfoAndMemInfo) {
  NetworkSocket s(0);
  setupExpectCallTcpInfo(s, getTestLatestTcpInfo());
  setupExpectCallCcName(s, "bbr");
  setupExpectCallCcInfo(s, getTestBbrInfo());
  setupExpectCallMemInfo(
      s,
      ExpectCallMemInfoConfig{
          .siocoutq = kTestSiocoutqVal, .siocinq = kTestSiocinqVal});

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = true;
  options.getMemInfo = true;
  auto wrappedTcpInfoExpect = TcpInfo::initFromFd(
      s, options, mockNetOpsDispatcher_, mockIoctlDispatcher_);
  ASSERT_TRUE(wrappedTcpInfoExpect.hasValue());
  const auto& wrappedTcpInfo = wrappedTcpInfoExpect.value();

  checkTcpInfoAgainstLatest(wrappedTcpInfo);
  checkCcFieldsAgainstBbr(wrappedTcpInfo);
  EXPECT_EQ(kTestSiocoutqVal, wrappedTcpInfo.sendBufInUseBytes()); // siocoutq
  EXPECT_EQ(kTestSiocinqVal, wrappedTcpInfo.recvBufInUseBytes()); // siocinq
}

TEST_F(TcpInfoTest, LatestStructWithCcInfoAndMemInfoUnknownCc) {
  NetworkSocket s(0);
  setupExpectCallTcpInfo(s, getTestLatestTcpInfo());
  setupExpectCallCcName(s, kTestUnknownCcName);
  setupExpectCallMemInfo(
      s,
      ExpectCallMemInfoConfig{
          .siocoutq = kTestSiocoutqVal, .siocinq = kTestSiocinqVal});

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = true;
  options.getMemInfo = true;
  auto wrappedTcpInfoExpect = TcpInfo::initFromFd(
      s, options, mockNetOpsDispatcher_, mockIoctlDispatcher_);
  ASSERT_TRUE(wrappedTcpInfoExpect.hasValue());
  const auto& wrappedTcpInfo = wrappedTcpInfoExpect.value();

  checkTcpInfoAgainstLatest(wrappedTcpInfo);
  EXPECT_EQ(kTestSiocoutqVal, wrappedTcpInfo.sendBufInUseBytes()); // siocoutq
  EXPECT_EQ(kTestSiocinqVal, wrappedTcpInfo.recvBufInUseBytes()); // siocinq

  // the CC name and enum should be set, but there should be no other CC info
  EXPECT_EQ(kTestUnknownCcName, wrappedTcpInfo.ccNameRaw());
  EXPECT_EQ("UNKNOWN", wrappedTcpInfo.ccNameEnumAsStr());
  EXPECT_EQ(
      TcpInfo::CongestionControlName::UNKNOWN, wrappedTcpInfo.ccNameEnum());
  checkNoCcInfo(wrappedTcpInfo);
}

TEST_F(TcpInfoTest, FailUninitializedSocket) {
  NetworkSocket s;

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = true;
  options.getMemInfo = true;
  auto wrappedTcpInfoExpect = TcpInfo::initFromFd(
      s, options, mockNetOpsDispatcher_, mockIoctlDispatcher_);
  ASSERT_FALSE(wrappedTcpInfoExpect.hasValue()); // complete failure
}

TEST_F(TcpInfoTest, FailTcpInfo) {
  NetworkSocket s(0);

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = true;
  options.getMemInfo = true;
  EXPECT_CALL(mockNetOpsDispatcher_, getsockopt(s, IPPROTO_TCP, TCP_INFO, _, _))
      .WillOnce(Return(-1));
  auto wrappedTcpInfoExpect = TcpInfo::initFromFd(
      s, options, mockNetOpsDispatcher_, mockIoctlDispatcher_);
  ASSERT_FALSE(wrappedTcpInfoExpect.hasValue()); // complete failure
}

TEST_F(TcpInfoTest, FailCcName) {
  NetworkSocket s(0);
  setupExpectCallTcpInfo(s, getTestLatestTcpInfo());
  setupExpectCallMemInfo(
      s,
      ExpectCallMemInfoConfig{
          .siocoutq = kTestSiocoutqVal, .siocinq = kTestSiocinqVal});

  // ensure that we try to fetch TCP name via TCP_CONGESTION
  // return -1 when trying to get CC name to mimic socket error state
  EXPECT_CALL(
      mockNetOpsDispatcher_,
      getsockopt(
          s,
          IPPROTO_TCP,
          TCP_CONGESTION,
          NotNull(),
          Pointee(Eq(TcpInfo::kLinuxTcpCaNameMax))))
      .WillOnce(Return(-1));

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = true;
  options.getMemInfo = true;
  auto wrappedTcpInfoExpect = TcpInfo::initFromFd(
      s, options, mockNetOpsDispatcher_, mockIoctlDispatcher_);
  ASSERT_TRUE(wrappedTcpInfoExpect.hasValue());
  const auto& wrappedTcpInfo = wrappedTcpInfoExpect.value();

  checkTcpInfoAgainstLatest(wrappedTcpInfo);
  EXPECT_EQ(kTestSiocoutqVal, wrappedTcpInfo.sendBufInUseBytes()); // siocoutq
  EXPECT_EQ(kTestSiocinqVal, wrappedTcpInfo.recvBufInUseBytes()); // siocinq

  // no CC name/type or info due to failed lookup for TCP_CONGESTION
  checkNoCcNameType(wrappedTcpInfo);
  checkNoCcInfo(wrappedTcpInfo);
}

TEST_F(TcpInfoTest, FailCcInfo) {
  NetworkSocket s(0);
  setupExpectCallTcpInfo(s, getTestLatestTcpInfo());
  setupExpectCallCcName(s, "bbr");
  setupExpectCallMemInfo(
      s,
      ExpectCallMemInfoConfig{
          .siocoutq = kTestSiocoutqVal, .siocinq = kTestSiocinqVal});

  // ensure that we try to fetch TCP info via TCP_CC_INFO
  // return -1 when trying to get CC name to mimic socket error state
  EXPECT_CALL(
      mockNetOpsDispatcher_,
      getsockopt(
          s,
          IPPROTO_TCP,
          TCP_CC_INFO,
          NotNull(),
          Pointee(Eq(sizeof(TcpInfo::tcp_cc_info)))))
      .WillOnce(Return(-1));

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = true;
  options.getMemInfo = true;
  auto wrappedTcpInfoExpect = TcpInfo::initFromFd(
      s, options, mockNetOpsDispatcher_, mockIoctlDispatcher_);
  ASSERT_TRUE(wrappedTcpInfoExpect.hasValue());
  const auto& wrappedTcpInfo = wrappedTcpInfoExpect.value();

  checkTcpInfoAgainstLatest(wrappedTcpInfo);
  EXPECT_EQ(kTestSiocoutqVal, wrappedTcpInfo.sendBufInUseBytes()); // siocoutq
  EXPECT_EQ(kTestSiocinqVal, wrappedTcpInfo.recvBufInUseBytes()); // siocinq

  // the CC name and enum should be set, but there should be no other CC info,
  // despite how this is BBR (and thus triggered a TCP_CC_INFO lookup) given
  // that the TCP_CC_INFO lookup failed
  EXPECT_EQ("BBR", wrappedTcpInfo.ccNameEnumAsStr());
  EXPECT_EQ(TcpInfo::CongestionControlName::BBR, wrappedTcpInfo.ccNameEnum());
  checkNoCcInfo(wrappedTcpInfo);
}

TEST_F(TcpInfoTest, FailSiocoutq) {
  NetworkSocket s(0);
  setupExpectCallTcpInfo(s, getTestLatestTcpInfo());
  setupExpectCallCcName(s, "bbr");
  setupExpectCallCcInfo(s, getTestBbrInfo());

  EXPECT_CALL(mockIoctlDispatcher_, ioctl(s.toFd(), SIOCOUTQ, testing::_))
      .WillOnce(Return(-1));
  EXPECT_CALL(mockIoctlDispatcher_, ioctl(s.toFd(), SIOCINQ, testing::_))
      .WillOnce(WithArgs<2>(Invoke([toSet = kTestSiocinqVal](void* argp) {
        size_t* val = static_cast<size_t*>(argp);
        *val = toSet;
        return 0;
      })));

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = true;
  options.getMemInfo = true;
  auto wrappedTcpInfoExpect = TcpInfo::initFromFd(
      s, options, mockNetOpsDispatcher_, mockIoctlDispatcher_);
  ASSERT_TRUE(wrappedTcpInfoExpect.hasValue());
  const auto& wrappedTcpInfo = wrappedTcpInfoExpect.value();

  checkTcpInfoAgainstLatest(wrappedTcpInfo);
  checkCcFieldsAgainstBbr(wrappedTcpInfo);

  // should have SIOCINQ, but no SIOCOUTQ given failure during lookup
  EXPECT_FALSE(wrappedTcpInfo.sendBufInUseBytes().has_value()); // siocoutq
  EXPECT_EQ(kTestSiocinqVal, wrappedTcpInfo.recvBufInUseBytes()); // siocinq
}

TEST_F(TcpInfoTest, FailSiocinq) {
  NetworkSocket s(0);
  setupExpectCallTcpInfo(s, getTestLatestTcpInfo());
  setupExpectCallCcName(s, "bbr");
  setupExpectCallCcInfo(s, getTestBbrInfo());

  EXPECT_CALL(mockIoctlDispatcher_, ioctl(s.toFd(), SIOCOUTQ, testing::_))
      .WillOnce(WithArgs<2>(Invoke([toSet = kTestSiocoutqVal](void* argp) {
        size_t* val = static_cast<size_t*>(argp);
        *val = toSet;
        return 0;
      })));
  EXPECT_CALL(mockIoctlDispatcher_, ioctl(s.toFd(), SIOCINQ, testing::_))
      .WillOnce(Return(-1));

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = true;
  options.getMemInfo = true;
  auto wrappedTcpInfoExpect = TcpInfo::initFromFd(
      s, options, mockNetOpsDispatcher_, mockIoctlDispatcher_);
  ASSERT_TRUE(wrappedTcpInfoExpect.hasValue());
  const auto& wrappedTcpInfo = wrappedTcpInfoExpect.value();

  checkTcpInfoAgainstLatest(wrappedTcpInfo);
  checkCcFieldsAgainstBbr(wrappedTcpInfo);

  // should have SIOCOUTQ, but no SIOCINQ given failure during lookup
  EXPECT_EQ(kTestSiocoutqVal, wrappedTcpInfo.sendBufInUseBytes()); // siocoutq
  EXPECT_FALSE(wrappedTcpInfo.recvBufInUseBytes().has_value()); // siocinq
}

struct TcpInfoTestCcParamRawStrAndEnum {
  TcpInfoTestCcParamRawStrAndEnum(
      std::string ccNameRaw, TcpInfo::CongestionControlName ccNameEnum)
      : ccNameRaw(std::move(ccNameRaw)), ccNameEnum(ccNameEnum) {}
  const std::string ccNameRaw; // raw name returned by kernel
  const TcpInfo::CongestionControlName ccNameEnum; // expected
};

class TcpInfoTestCcParam
    : public TcpInfoTest,
      public testing::WithParamInterface<TcpInfoTestCcParamRawStrAndEnum> {
 public:
  static std::vector<TcpInfoTestCcParamRawStrAndEnum> getTestingValues() {
    return std::vector<TcpInfoTestCcParamRawStrAndEnum>{
        {kTestUnknownCcName, TcpInfo::CongestionControlName::UNKNOWN},
        {"cubic", TcpInfo::CongestionControlName::CUBIC},
        {"bic", TcpInfo::CongestionControlName::BIC},
        {"dctcp", TcpInfo::CongestionControlName::DCTCP},
        {"dctcp_reno", TcpInfo::CongestionControlName::DCTCP_RENO},
        {"bbr", TcpInfo::CongestionControlName::BBR},
        {"reno", TcpInfo::CongestionControlName::RENO},
        {"dctcp_cubic", TcpInfo::CongestionControlName::DCTCP_CUBIC},
        {"vegas", TcpInfo::CongestionControlName::VEGAS}};
  }
};

INSTANTIATE_TEST_SUITE_P(
    CcParamTests,
    TcpInfoTestCcParam,
    ::testing::ValuesIn(TcpInfoTestCcParam::getTestingValues()));

TEST_P(TcpInfoTestCcParam, FetchAllAndCheck) {
  const auto testParams = GetParam();

  NetworkSocket s(0);
  setupExpectCallTcpInfo(s, getTestLatestTcpInfo());
  setupExpectCallMemInfo(
      s,
      ExpectCallMemInfoConfig{
          .siocoutq = kTestSiocoutqVal, .siocinq = kTestSiocinqVal});

  // setup CC specifics for the test
  setupExpectCallCcName(s, testParams.ccNameRaw);
  switch (testParams.ccNameEnum) {
    case TcpInfo::CongestionControlName::UNKNOWN:
    case TcpInfo::CongestionControlName::RENO:
    case TcpInfo::CongestionControlName::CUBIC:
    case TcpInfo::CongestionControlName::BIC:
      break; // CC_INFO not supported
    case TcpInfo::CongestionControlName::BBR:
      setupExpectCallCcInfo(s, getTestBbrInfo());
      break;
    case TcpInfo::CongestionControlName::VEGAS:
      setupExpectCallCcInfo(s, getTestVegasInfo());
      break;
    case TcpInfo::CongestionControlName::DCTCP:
    case TcpInfo::CongestionControlName::DCTCP_RENO:
    case TcpInfo::CongestionControlName::DCTCP_CUBIC:
      setupExpectCallCcInfo(s, getTestDctcpInfo());
      break;
    case TcpInfo::CongestionControlName::NumCcTypes:
      FAIL();
  }

  TcpInfo::LookupOptions options = {};
  options.getCcInfo = true;
  options.getMemInfo = true;
  auto wrappedTcpInfoExpect = TcpInfo::initFromFd(
      s, options, mockNetOpsDispatcher_, mockIoctlDispatcher_);
  ASSERT_TRUE(wrappedTcpInfoExpect.hasValue());
  const auto& wrappedTcpInfo = wrappedTcpInfoExpect.value();

  checkTcpInfoAgainstLatest(wrappedTcpInfo);
  EXPECT_EQ(kTestSiocoutqVal, wrappedTcpInfo.sendBufInUseBytes()); // siocoutq
  EXPECT_EQ(kTestSiocinqVal, wrappedTcpInfo.recvBufInUseBytes()); // siocinq

  // check CC information
  EXPECT_EQ(testParams.ccNameRaw, wrappedTcpInfo.ccNameRaw());
  EXPECT_EQ(testParams.ccNameEnum, wrappedTcpInfo.ccNameEnum());
  switch (testParams.ccNameEnum) {
    case TcpInfo::CongestionControlName::UNKNOWN:
      EXPECT_EQ("UNKNOWN", wrappedTcpInfo.ccNameEnumAsStr());
      break;
    case TcpInfo::CongestionControlName::RENO:
      EXPECT_EQ("RENO", wrappedTcpInfo.ccNameEnumAsStr());
      break;
    case TcpInfo::CongestionControlName::CUBIC:
      EXPECT_EQ("CUBIC", wrappedTcpInfo.ccNameEnumAsStr());
      break;
    case TcpInfo::CongestionControlName::BIC:
      EXPECT_EQ("BIC", wrappedTcpInfo.ccNameEnumAsStr());
      break;
    case TcpInfo::CongestionControlName::BBR:
      EXPECT_EQ("BBR", wrappedTcpInfo.ccNameEnumAsStr());
      break;
    case TcpInfo::CongestionControlName::VEGAS:
      EXPECT_EQ("VEGAS", wrappedTcpInfo.ccNameEnumAsStr());
      break;
    case TcpInfo::CongestionControlName::DCTCP:
      EXPECT_EQ("DCTCP", wrappedTcpInfo.ccNameEnumAsStr());
      break;
    case TcpInfo::CongestionControlName::DCTCP_RENO:
      EXPECT_EQ("DCTCP_RENO", wrappedTcpInfo.ccNameEnumAsStr());
      break;
    case TcpInfo::CongestionControlName::DCTCP_CUBIC:
      EXPECT_EQ("DCTCP_CUBIC", wrappedTcpInfo.ccNameEnumAsStr());
      break;
    case TcpInfo::CongestionControlName::NumCcTypes:
      FAIL();
  }

  switch (testParams.ccNameEnum) {
    case TcpInfo::CongestionControlName::UNKNOWN:
    case TcpInfo::CongestionControlName::RENO:
    case TcpInfo::CongestionControlName::CUBIC:
    case TcpInfo::CongestionControlName::BIC:
      checkNoCcInfo(wrappedTcpInfo); // should be no extra CC_INFO
      break;
    case TcpInfo::CongestionControlName::BBR:
      checkCcFieldsAgainstBbr(wrappedTcpInfo);
      break;
    case TcpInfo::CongestionControlName::VEGAS:
      checkCcFieldsAgainstVegas(wrappedTcpInfo);
      break;
    case TcpInfo::CongestionControlName::DCTCP:
    case TcpInfo::CongestionControlName::DCTCP_RENO:
    case TcpInfo::CongestionControlName::DCTCP_CUBIC:
      checkCcFieldsAgainstDctcp(wrappedTcpInfo, testParams.ccNameEnum);
      break;
    case TcpInfo::CongestionControlName::NumCcTypes:
      FAIL();
  }
}

#endif // #ifdef __linux__

#ifdef __APPLE__
TEST(TcpInfoAppleTest, CwndIsReportedInBytes) {
  folly::TcpInfo::tcp_info tInfo = {};
  tInfo.tcpi_maxseg = 1448;
  tInfo.tcpi_snd_cwnd = 14480;
  TcpInfo wrappedTcpInfo{tInfo};
  EXPECT_EQ(14480U, wrappedTcpInfo.cwndInBytes());
  EXPECT_EQ(10U, wrappedTcpInfo.cwndInPackets());
}

TEST(TcpInfoAppleTest, CwndInPacketsCountsPartialSegment) {
  folly::TcpInfo::tcp_info tInfo = {};
  tInfo.tcpi_maxseg = 1448;
  tInfo.tcpi_snd_cwnd = 15927;
  TcpInfo wrappedTcpInfo{tInfo};
  EXPECT_EQ(15927U, wrappedTcpInfo.cwndInBytes());
  EXPECT_EQ(11U, wrappedTcpInfo.cwndInPackets());
}

TEST(TcpInfoAppleTest, CwndInPacketsUnavailableWithoutMss) {
  folly::TcpInfo::tcp_info tInfo = {};
  tInfo.tcpi_snd_cwnd = 14480;
  TcpInfo wrappedTcpInfo{tInfo};
  EXPECT_EQ(14480U, wrappedTcpInfo.cwndInBytes());
  EXPECT_FALSE(wrappedTcpInfo.cwndInPackets().has_value());
}
#endif // #ifdef __APPLE__
