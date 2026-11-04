// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <cstring>

#ifdef _WIN32
  // lib includes
  #include <windows.h>
#endif

// local includes
#include "../../steady/SteadyProbe.h"

TEST(SteadyProbeTest, CanonicalAuthenticatedReplyAndFiniteReplayWindow) {
  steady_probe_session state {};
  std::array<unsigned char, 64> request {}, reply {};
  for (unsigned sequence = 0; sequence < STEADY_PROBE_SAMPLES; ++sequence) {
    ASSERT_TRUE(steady_probe_request(request.data(), 7, sequence, 123));
    ASSERT_TRUE(steady_probe_reply_once(&state, reply.data(), request.data(), request.size(), true, 456, 789, 10000000));
    ASSERT_TRUE(steady_probe_valid(reply.data(), reply.size(), 2));
    ASSERT_EQ(steady_probe_get64(reply.data() + 24), 123);
    ASSERT_EQ(steady_probe_get64(reply.data() + 32), 456);
    ASSERT_EQ(steady_probe_get64(reply.data() + 40), 789);
    ASSERT_FALSE(steady_probe_reply_once(&state, reply.data(), request.data(), request.size(), true, 456, 789, 10000000));
  }
  ASSERT_FALSE(steady_probe_request(request.data(), 7, STEADY_PROBE_SAMPLES, 123));
  ASSERT_TRUE(steady_probe_request(request.data(), 8, 0, 123));
  ASSERT_FALSE(steady_probe_reply_once(&state, reply.data(), request.data(), request.size(), true, 456, 789, 10000000));
  ASSERT_EQ(state.generation, 7);
}

TEST(SteadyProbeTest, InvalidUnauthenticatedRequestsCannotMutateOutputOrOwnership) {
  steady_probe_session state {};
  std::array<unsigned char, 64> request {}, reply, sentinel;
  sentinel.fill(0xA5);
  ASSERT_TRUE(steady_probe_request(request.data(), 7, 0, 123));
  reply = sentinel;
  ASSERT_FALSE(steady_probe_reply_once(&state, reply.data(), request.data(), request.size(), false, 456, 789, 10000000));
  ASSERT_EQ(reply, sentinel);
  ASSERT_EQ(state.generation, 0);
  for (std::size_t size = 0; size < request.size(); ++size) {
    ASSERT_FALSE(steady_probe_reply_once(&state, reply.data(), request.data(), size, true, 456, 789, 10000000));
    ASSERT_EQ(reply, sentinel);
  }
  ASSERT_FALSE(steady_probe_reply_once(&state, reply.data(), request.data(), request.size(), true, 456, 455, 10000000));
  ASSERT_EQ(reply, sentinel);
}

TEST(SteadyProbeTest, VersionRequiresAnExactUniqueSdpAttribute) {
  ASSERT_EQ(steady_probe_sdp_version(nullptr), 0);
  ASSERT_EQ(steady_probe_sdp_version("v=0\r\na=x-steady-input.probeVersion:1\r\n"), 1);
  ASSERT_EQ(steady_probe_sdp_version("a=x-steady-input.probeVersion:1junk\n"), 0);
  ASSERT_EQ(steady_probe_sdp_version("a=x-steady-input.probeVersion:2\n"), 0);
  ASSERT_EQ(steady_probe_sdp_version("a=x-steady-input.probeVersion:1\na=x-steady-input.probeVersion:1\n"), 0);
}
