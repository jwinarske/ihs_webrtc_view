// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

// Drives the presenter with real hardware-decoded frames and reports what
// actually reaches the ivi-homescreen platform-view interface.
//
// The synthetic check (presenter_test) proves the presenter honours the
// producer contract. This one answers a different question: given frames a
// real decoder produces, is what lands at ihs submit good enough, and does the
// IhsFrame/grant surface carry everything the compositor needs? It is a
// measurement tool as much as a test, so it prints the format, plane layout,
// pacing and hold depth rather than only passing or failing.
//
// The pipeline is the whole stack: a raw frame is encoded as H.264, sent over
// a loopback PeerConnection, decoded by the absorbed hardware decoder,
// delivered as a dma-buf through the sink registry, taken by the presenter and
// submitted here.
//
// This binary stands in for libihs_shared.so by exporting ihs_get_api, so the
// presenter's dlsym(RTLD_DEFAULT, ...) resolves to the recording
// implementation below.
//
// Needs a libwebrtc build with lw_enable_v4l2_codec=true and an H.264 decode
// device. Configure with:
//
//   cmake -S . -B build -DLIBWEBRTC_DIR=/path/to/libwebrtc \
//         -DLIBWEBRTC_LIB=/path/to/out/libwebrtc.so
//   cmake --build build --target presenter_live
//   LD_LIBRARY_PATH=$(dirname $LIBWEBRTC_LIB) ./build/native/presenter_live

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ihs/ihs.h"
#include "ihs/platform_view.h"
#include "lw_video_sink.h"

// libwebrtc: the C sink registry plus the C++ session API.
#include "c/lw_c_api.h"
#include "libwebrtc.h"
#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_video_frame.h"
#include "rtc_video_source.h"
#include "rtc_video_track.h"

namespace {

using namespace libwebrtc;
using Clock = std::chrono::steady_clock;

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kWantSubmits = 10;

// ---- what reached the compositor ----------------------------------------

struct SubmitRecord {
  Clock::time_point at;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fourcc = 0;
  uint64_t modifier = 0;
  uint32_t plane_count = 0;
  uint32_t stride[4] = {0, 0, 0, 0};
  uint32_t offset[4] = {0, 0, 0, 0};
  int fd[4] = {-1, -1, -1, -1};
  bool had_acquire_fence = false;
  bool got_release_fence = false;
};

std::mutex g_mu;
std::vector<SubmitRecord> g_submits;
std::atomic<int> g_released{0};
std::atomic<int> g_taken{0};
std::atomic<int> g_declined{0};
IhsPvFactory g_factory = nullptr;
void* g_factory_user = nullptr;
IhsPvGrant g_grant{};
std::atomic<uint32_t> g_negotiated_kind{0};
std::atomic<uint32_t> g_negotiated_sync{0};
// LW_PRESENTER_SYNC=explicit grants explicit sync and hands back an
// already-signalled release fence per submit, so the presenter can retire a
// buffer as soon as the compositor is done with it instead of falling back to
// a fixed depth window. Comparing the two runs shows how many decoder buffers
// the sync mode costs.
bool ExplicitSyncRequested() {
  const char* v = std::getenv("LW_PRESENTER_SYNC");
  return v != nullptr && std::strcmp(v, "explicit") == 0;
}

int FakeRegisterFactory(const char* /*view_type*/, IhsPvFactory factory,
                        void* user) {
  g_factory = factory;
  g_factory_user = user;
  return IHS_PV_OK;
}
void FakeUnregisterFactory(const char* /*view_type*/) { g_factory = nullptr; }

int FakeNegotiate(IhsPlatformView* /*view*/,
                  const IhsPvRequirements* /*requirements*/, IhsPvGrant* out) {
  if (out == nullptr) {
    return IHS_PV_ERR_INVALID;
  }
  out->struct_size = sizeof(IhsPvGrant);
  out->granted_kind = IHS_PV_KIND_TEXTURE_DMABUF_IMPORT;
  out->sync = ExplicitSyncRequested() ? IHS_PV_SYNC_EXPLICIT_PREFERRED
                                      : IHS_PV_SYNC_IMPLICIT;
  std::memset(&out->format, 0, sizeof(out->format));
  g_grant = *out;
  g_negotiated_kind = out->granted_kind;
  g_negotiated_sync = out->sync;
  return IHS_PV_OK;
}

int FakeSubmit(IhsPlatformView* /*view*/, const IhsFrame* frame,
               int acquire_fence_fd, int* out_release_fence_fd) {
  if (out_release_fence_fd != nullptr) {
    if (ExplicitSyncRequested()) {
      // Stand in for a fence the compositor has already signalled: an eventfd
      // created with a non-zero count is immediately readable, so the
      // presenter's bounded wait returns at once.
      *out_release_fence_fd = ::eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
    } else {
      *out_release_fence_fd = -1;  // release rides the callbacks
    }
  }
  if (frame != nullptr) {
    SubmitRecord r;
    r.at = Clock::now();
    r.width = frame->width;
    r.height = frame->height;
    r.fourcc = frame->format.fourcc;
    r.modifier = frame->format.modifier;
    r.plane_count = frame->plane_count;
    for (uint32_t i = 0; i < 4 && i < frame->plane_count; ++i) {
      r.stride[i] = frame->plane_stride[i];
      r.offset[i] = frame->plane_offset[i];
      r.fd[i] = frame->plane_fd[i];
    }
    r.had_acquire_fence = acquire_fence_fd >= 0;
    r.got_release_fence = false;
    std::lock_guard<std::mutex> lock(g_mu);
    g_submits.push_back(r);
  }
  if (acquire_fence_fd >= 0) {
    ::close(acquire_fence_fd);
  }
  return IHS_PV_OK;
}

int FakeQueryCaps(IhsPvCapabilities* /*out*/) { return IHS_PV_ERR_UNSUPPORTED; }
int FakeVulkanCtx(IhsVulkanContext* /*out*/) { return IHS_PV_ERR_UNSUPPORTED; }
int FakeEglCtx(IhsEglContext* /*out*/) { return IHS_PV_ERR_UNSUPPORTED; }
uint32_t FakeGrantPlaneId(IhsPlatformView* /*view*/) { return 0; }
int FakeGrantShmFd(IhsPlatformView* /*view*/, size_t* /*out_stride*/) {
  return -1;
}

const IhsPlatformViewApi g_pv_api = {
    sizeof(IhsPlatformViewApi),
    FakeQueryCaps,
    FakeVulkanCtx,
    FakeEglCtx,
    FakeRegisterFactory,
    FakeUnregisterFactory,
    FakeNegotiate,
    FakeGrantPlaneId,
    FakeGrantShmFd,
    FakeSubmit,
};
const IhsApi g_api = {
    sizeof(IhsApi), IHS_SHARED_ABI_VERSION, nullptr, nullptr, &g_pv_api,
    nullptr,
};

size_t SubmitCount() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_submits.size();
}

// ---- loopback session ----------------------------------------------------

std::string ForceH264(const std::string& sdp) {
  std::istringstream in(sdp);
  std::vector<std::string> lines;
  std::vector<std::string> keep;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  for (const std::string& line : lines) {
    if (line.rfind("a=rtpmap:", 0) == 0 &&
        line.find(" H264/") != std::string::npos) {
      keep.push_back(line.substr(9, line.find(' ') - 9));
    }
  }
  if (keep.empty()) {
    return sdp;
  }
  std::string out;
  bool in_video = false;
  for (const std::string& line : lines) {
    if (line.rfind("m=", 0) == 0) {
      in_video = (line.rfind("m=video", 0) == 0);
    }
    if (in_video && line.rfind("m=video", 0) == 0) {
      std::istringstream ms(line);
      std::string tok;
      std::string hdr;
      for (int i = 0; i < 3 && (ms >> tok); ++i) {
        hdr += tok + " ";
      }
      out += hdr;
      for (size_t i = 0; i < keep.size(); ++i) {
        out += keep[i] + (i + 1 < keep.size() ? " " : "");
      }
      out += "\r\n";
      continue;
    }
    if (in_video &&
        (line.rfind("a=rtpmap:", 0) == 0 || line.rfind("a=fmtp:", 0) == 0 ||
         line.rfind("a=rtcp-fb:", 0) == 0)) {
      const size_t colon = line.find(':') + 1;
      const std::string pt = line.substr(colon, line.find(' ', colon) - colon);
      bool kept = false;
      for (const std::string& k : keep) {
        kept = kept || (k == pt);
      }
      if (!kept) {
        continue;
      }
    }
    out += line + "\r\n";
  }
  return out;
}

struct Candidate {
  std::string mid;
  int index = 0;
  std::string candidate;
};

class Peer : public RTCPeerConnectionObserver {
 public:
  scoped_refptr<RTCPeerConnection> pc;
  Peer* peer = nullptr;
  bool receives = false;
  lw_video_sink_token token = 0;
  std::atomic<bool> remote_set{false};

  void OnSignalingState(RTCSignalingState) override {}
  void OnPeerConnectionState(RTCPeerConnectionState) override {}
  void OnIceGatheringState(RTCIceGatheringState) override {}
  void OnIceConnectionState(RTCIceConnectionState) override {}
  void OnIceCandidate(scoped_refptr<RTCIceCandidate> c) override {
    if (!c || !peer) {
      return;
    }
    Candidate cand{c->sdp_mid().std_string(), c->sdp_mline_index(),
                   c->candidate().std_string()};
    if (peer->remote_set) {
      peer->pc->AddCandidate(cand.mid.c_str(), cand.index,
                             cand.candidate.c_str());
    } else {
      std::lock_guard<std::mutex> lock(mu_);
      outbox_.push_back(std::move(cand));
    }
  }
  void FlushCandidatesTo(Peer* to) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const Candidate& c : outbox_) {
      to->pc->AddCandidate(c.mid.c_str(), c.index, c.candidate.c_str());
    }
    outbox_.clear();
  }
  void OnAddStream(scoped_refptr<RTCMediaStream>) override {}
  void OnRemoveStream(scoped_refptr<RTCMediaStream>) override {}
  void OnDataChannel(scoped_refptr<RTCDataChannel>) override {}
  void OnRenegotiationNeeded() override {}
  void OnTrack(scoped_refptr<RTCRtpTransceiver> transceiver) override {
    if (!receives || !transceiver || token == 0) {
      return;
    }
    scoped_refptr<RTCRtpReceiver> receiver = transceiver->receiver();
    if (!receiver) {
      return;
    }
    scoped_refptr<RTCMediaTrack> track = receiver->track();
    auto* video = dynamic_cast<RTCVideoTrack*>(track.get());
    if (video == nullptr) {
      return;
    }
    held_ = scoped_refptr<RTCVideoTrack>(video);  // keep the adapter alive
    const int rc = lw_video_track_bind_sink(
        reinterpret_cast<lw_video_track_t*>(video), token);
    std::printf("bound presenter sink to the decoded track (rc=%d)\n", rc);
  }
  void OnAddTrack(vector<scoped_refptr<RTCMediaStream>>,
                  scoped_refptr<RTCRtpReceiver>) override {}
  void OnRemoveTrack(scoped_refptr<RTCRtpReceiver>) override {}

 private:
  std::mutex mu_;
  std::vector<Candidate> outbox_;
  scoped_refptr<RTCVideoTrack> held_;
};

std::string g_sdp;
std::atomic<bool> g_sdp_done{false};
std::atomic<bool> g_set_done{false};

bool Wait(std::atomic<bool>* flag, int timeout_ms = 10000) {
  for (int i = 0; i < timeout_ms / 10 && !*flag; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return flag->load();
}

// ---- counting relay between the registry and the presenter --------------
//
// Registering this instead of the presenter's own sink makes the producer side
// observable: how many frames the presenter takes, how many it has returned,
// and therefore how many decoder buffers it is holding at once. That hold
// depth is what decides whether a given decoder pool can feed it.

const LwVideoSinkV1* g_presenter_sink = nullptr;
void* g_presenter_user = nullptr;

struct RelayCtx {
  LwFrameRelease release;
  void* ctx;
};

void RelayRelease(void* p) {
  auto* relay = static_cast<RelayCtx*>(p);
  ++g_released;
  if (relay->release != nullptr) {
    relay->release(relay->ctx);
  }
  delete relay;
}

int RelayOnFrame(const LwDmabufDescriptor* desc, LwFrameRelease release,
                 void* ctx, void* /*user*/) {
  auto* relay = new RelayCtx{release, ctx};
  const int taken =
      g_presenter_sink->on_frame(desc, &RelayRelease, relay, g_presenter_user);
  if (taken != 0) {
    ++g_taken;
  } else {
    // Declined: the producer recycles the buffer itself, so drop the relay.
    ++g_declined;
    delete relay;
  }
  return taken;
}

void RelayOnFormat(const LwDmabufDescriptor* fmt, void* /*user*/) {
  if (g_presenter_sink->on_format != nullptr) {
    g_presenter_sink->on_format(fmt, g_presenter_user);
  }
}

void RelayOnEos(void* /*user*/) {
  if (g_presenter_sink->on_eos != nullptr) {
    g_presenter_sink->on_eos(g_presenter_user);
  }
}

LwVideoSinkV1 g_relay_sink{};

const char* FourccStr(uint32_t fourcc, char* buf) {
  std::memcpy(buf, &fourcc, 4);
  buf[4] = '\0';
  return buf;
}

void Report() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_submits.empty()) {
    std::printf("no frames reached the platform-view interface\n");
    return;
  }
  const SubmitRecord& f = g_submits.front();
  char fcc[5];
  std::printf("\n--- what reached ihs submit ---\n");
  std::printf("  grant           kind=%u sync=%u\n", g_negotiated_kind.load(),
              g_negotiated_sync.load());
  std::printf("  geometry        %ux%u\n", f.width, f.height);
  std::printf("  format          fourcc=%s modifier=0x%llx\n",
              FourccStr(f.fourcc, fcc),
              static_cast<unsigned long long>(f.modifier));
  std::printf("  planes          %u\n", f.plane_count);
  for (uint32_t i = 0; i < f.plane_count && i < 4; ++i) {
    std::printf("    plane %u       fd=%d stride=%u offset=%u\n", i, f.fd[i],
                f.stride[i], f.offset[i]);
  }
  std::printf("  sync mode       %s\n",
              ExplicitSyncRequested() ? "explicit (release fence per submit)"
                                      : "implicit (no release fence)");
  std::printf("  acquire fence   %s\n",
              f.had_acquire_fence ? "provided" : "none (-1, implicit)");

  // Cadence: how evenly the presenter paced them out.
  std::vector<double> gaps;
  for (size_t i = 1; i < g_submits.size(); ++i) {
    gaps.push_back(std::chrono::duration<double, std::milli>(
                       g_submits[i].at - g_submits[i - 1].at)
                       .count());
  }
  if (!gaps.empty()) {
    std::vector<double> sorted = gaps;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double g : gaps) {
      sum += g;
    }
    std::printf(
        "  submit cadence  n=%zu mean=%.1fms min=%.1f median=%.1f max=%.1f\n",
        gaps.size(), sum / static_cast<double>(gaps.size()), sorted.front(),
        sorted[sorted.size() / 2], sorted.back());
  }

  // How many distinct buffers the decoder pool had to supply.
  std::vector<int> fds;
  for (const SubmitRecord& r : g_submits) {
    if (std::find(fds.begin(), fds.end(), r.fd[0]) == fds.end()) {
      fds.push_back(r.fd[0]);
    }
  }
  std::printf("  distinct buffers %zu across %zu submits\n", fds.size(),
              g_submits.size());
  std::printf(
      "  producer         taken=%d released=%d still_held=%d declined=%d\n",
      g_taken.load(), g_released.load(), g_taken.load() - g_released.load(),
      g_declined.load());
}

}  // namespace

extern "C" __attribute__((visibility("default"))) const IhsApi* ihs_get_api(
    uint32_t /*requested_abi*/) {
  return &g_api;
}

extern "C" int ihs_webrtc_view_register(const char* view_type);
extern "C" const LwVideoSinkV1* ihs_webrtc_view_sink_for_view(int32_t view_id,
                                                              void** out_user);

int main() {
  constexpr int32_t kViewId = 1;
  setenv("LW_V4L2", "1", 1);  // opt the hardware decoder in

  // ---- stand up the presenter behind a stand-in registry -----------------
  if (ihs_webrtc_view_register("webrtc-view") != 0 || g_factory == nullptr) {
    std::printf("RESULT: FAIL (presenter did not register)\n");
    return 1;
  }
  IhsPvCreateInfo info{};
  info.struct_size = sizeof(info);
  info.id = kViewId;
  info.view_type = "webrtc-view";
  info.width = kWidth;
  info.height = kHeight;
  IhsPvCallbacks callbacks{};
  void* view_user = nullptr;
  auto* view = reinterpret_cast<IhsPlatformView*>(0x1234);
  if (g_factory(&info, g_factory_user, view, &callbacks, &view_user) !=
      IHS_PV_OK) {
    std::printf("RESULT: FAIL (view creation)\n");
    return 1;
  }
  void* sink_user = nullptr;
  const LwVideoSinkV1* sink =
      ihs_webrtc_view_sink_for_view(kViewId, &sink_user);
  if (sink == nullptr) {
    std::printf("RESULT: FAIL (no sink for view)\n");
    return 1;
  }

  // ---- bring up the session and hand the presenter's sink to the track ---
  if (!LibWebRTC::Initialize()) {
    std::printf("RESULT: FAIL (initialize)\n");
    return 1;
  }
  scoped_refptr<RTCPeerConnectionFactory> factory =
      LibWebRTC::CreateRTCPeerConnectionFactory();
  if (!factory || !factory->Initialize()) {
    std::printf("RESULT: FAIL (factory)\n");
    return 1;
  }
  g_presenter_sink = sink;
  g_presenter_user = sink_user;
  g_relay_sink.size = sizeof(g_relay_sink);
  g_relay_sink.on_frame = RelayOnFrame;
  g_relay_sink.on_format = RelayOnFormat;
  g_relay_sink.on_eos = RelayOnEos;
  const lw_video_sink_token token =
      lw_video_sink_register(&g_relay_sink, nullptr);
  if (token == 0) {
    std::printf("RESULT: FAIL (sink register)\n");
    return 1;
  }

  RTCConfiguration config;
  scoped_refptr<RTCMediaConstraints> constraints =
      RTCMediaConstraints::Create();
  Peer sender;
  Peer receiver;
  receiver.receives = true;
  receiver.token = token;
  sender.peer = &receiver;
  receiver.peer = &sender;
  sender.pc = factory->Create(config, constraints);
  receiver.pc = factory->Create(config, constraints);
  sender.pc->RegisterRTCPeerConnectionObserver(&sender);
  receiver.pc->RegisterRTCPeerConnectionObserver(&receiver);

  scoped_refptr<RTCVideoSource> source =
      factory->CreateCustomVideoSource("live-source", constraints);
  scoped_refptr<RTCVideoTrack> track =
      factory->CreateVideoTrack(source, "live-video");
  std::vector<string> stream_ids;
  stream_ids.push_back(string("live-stream"));
  sender.pc->AddTrack(track, vector<string>(stream_ids));

  g_sdp_done = false;
  sender.pc->CreateOffer(
      [](const string& sdp, const string&) {
        g_sdp = ForceH264(sdp.std_string());
        g_sdp_done = true;
      },
      [](const char*) { g_sdp_done = true; }, constraints);
  if (!Wait(&g_sdp_done) || g_sdp.find("H264") == std::string::npos) {
    std::printf("RESULT: FAIL (no H.264 offer)\n");
    return 1;
  }
  auto set_description = [&](const scoped_refptr<RTCPeerConnection>& pc,
                             bool local, const std::string& sdp,
                             const char* type) {
    g_set_done = false;
    if (local) {
      pc->SetLocalDescription(
          sdp.c_str(), type, [] { g_set_done = true; },
          [](const char*) { g_set_done = true; });
    } else {
      pc->SetRemoteDescription(
          sdp.c_str(), type, [] { g_set_done = true; },
          [](const char*) { g_set_done = true; });
    }
    return Wait(&g_set_done);
  };
  set_description(sender.pc, true, g_sdp, "offer");
  set_description(receiver.pc, false, g_sdp, "offer");
  receiver.remote_set = true;
  sender.FlushCandidatesTo(&receiver);

  std::string answer;
  g_sdp_done = false;
  receiver.pc->CreateAnswer(
      [&answer](const string& sdp, const string&) {
        answer = sdp.std_string();
        g_sdp_done = true;
      },
      [](const char*) { g_sdp_done = true; }, constraints);
  if (!Wait(&g_sdp_done) || answer.empty()) {
    std::printf("RESULT: FAIL (no answer)\n");
    return 1;
  }
  set_description(receiver.pc, true, answer, "answer");
  set_description(sender.pc, false, answer, "answer");
  sender.remote_set = true;
  receiver.FlushCandidatesTo(&sender);

  // ---- feed frames until enough have reached the compositor --------------
  std::vector<uint8_t> i420(static_cast<size_t>(kWidth) * kHeight * 3 / 2);
  for (int n = 0; n < 400 && SubmitCount() < kWantSubmits; ++n) {
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        i420[static_cast<size_t>(y) * kWidth + x] =
            static_cast<uint8_t>(x + y + n * 3);
      }
    }
    std::memset(i420.data() + static_cast<size_t>(kWidth) * kHeight, 128,
                static_cast<size_t>(kWidth) * kHeight / 2);
    source->OnCapturedFrame(RTCVideoFrame::Create(
        kWidth, kHeight, i420.data(), static_cast<int>(i420.size())));
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  Report();

  const size_t submits = SubmitCount();
  const bool pass = submits >= static_cast<size_t>(kWantSubmits) / 2;
  std::printf("\nRESULT: %s (%zu frames reached the platform view)\n",
              pass ? "PASS" : "FAIL", submits);

  if (callbacks.dispose != nullptr) {
    callbacks.dispose(view_user);
  }
  lw_video_sink_unregister(token);
  sender.pc->Close();
  receiver.pc->Close();
  LibWebRTC::Terminate();
  return pass ? 0 : 1;
}
