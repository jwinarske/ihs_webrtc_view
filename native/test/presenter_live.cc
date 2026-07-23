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

// libwebrtc, through its flat C ABI only -- the same surface the presenter and
// the Dart control plane use. Speaking C++ here coupled this tool to the
// library's vtable layout, which is what a define mismatch turns into a crash.
#include "c/lw_c_api.h"

namespace {

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

// A peer, driven entirely through the C ABI.
struct CPeer {
  lw_pc_t* pc = nullptr;
  CPeer* peer = nullptr;
  bool receives = false;
  lw_video_sink_token token = 0;
  lw_video_track_t* remote_track = nullptr;
  std::atomic<bool> remote_set{false};

  void Send(const Candidate& c) {
    if (peer->remote_set) {
      lw_pc_add_ice_candidate(peer->pc, c.mid.c_str(), c.index,
                              c.candidate.c_str());
    } else {
      std::lock_guard<std::mutex> lock(mu);
      outbox.push_back(c);
    }
  }

  void FlushCandidates() {
    std::lock_guard<std::mutex> lock(mu);
    for (const Candidate& c : outbox) {
      lw_pc_add_ice_candidate(peer->pc, c.mid.c_str(), c.index,
                              c.candidate.c_str());
    }
    outbox.clear();
  }

  std::mutex mu;
  std::vector<Candidate> outbox;
};

void OnIceCandidate(char* candidate, char* mid, int mline_index, void* user) {
  Candidate cand{mid != nullptr ? mid : "", mline_index,
                 candidate != nullptr ? candidate : ""};
  lw_string_free(candidate);
  lw_string_free(mid);
  static_cast<CPeer*>(user)->Send(cand);
}

void OnTrack(lw_transceiver_t* transceiver, void* user) {
  auto* self = static_cast<CPeer*>(user);
  lw_receiver_t* receiver = lw_transceiver_receiver(transceiver);
  lw_release(transceiver);
  if (receiver == nullptr || !self->receives) {
    lw_release(receiver);
    return;
  }
  self->remote_track = lw_receiver_video_track(receiver);
  lw_release(receiver);
  if (self->remote_track == nullptr) {
    return;
  }
  const int rc = lw_video_track_bind_sink(self->remote_track, self->token);
  std::printf("bound presenter sink to the decoded track (rc=%d)\n", rc);
}

// SDP callbacks take ownership of their strings.
struct SdpResult {
  std::string sdp;
  std::atomic<bool> done{false};
};

void OnSdp(char* sdp, char* type, void* user) {
  auto* result = static_cast<SdpResult*>(user);
  result->sdp = sdp != nullptr ? sdp : "";
  lw_string_free(sdp);
  lw_string_free(type);
  result->done = true;
}

void OnSdpFailure(char* error, void* user) {
  std::printf("  sdp failure: %s\n", error != nullptr ? error : "?");
  lw_string_free(error);
  static_cast<SdpResult*>(user)->done = true;
}

void OnSetDone(void* user) { *static_cast<std::atomic<bool>*>(user) = true; }

void OnSetFailure(char* error, void* user) {
  std::printf("  set description failure: %s\n",
              error != nullptr ? error : "?");
  lw_string_free(error);
  *static_cast<std::atomic<bool>*>(user) = true;
}

SdpResult g_offer;
SdpResult g_answer;
std::atomic<bool> g_set_done_flag{false};

std::string g_sdp;

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
  if (lw_initialize() == 0) {
    std::printf("RESULT: FAIL (initialize)\n");
    return 1;
  }
  lw_factory_t* factory = lw_factory_create();
  if (factory == nullptr || lw_factory_initialize(factory) == 0) {
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

  CPeer sender;
  CPeer receiver;
  receiver.receives = true;
  receiver.token = token;
  sender.peer = &receiver;
  receiver.peer = &sender;
  sender.pc = lw_pc_create(factory);
  receiver.pc = lw_pc_create(factory);
  if (sender.pc == nullptr || receiver.pc == nullptr) {
    std::printf("RESULT: FAIL (peer connection)\n");
    return 1;
  }

  LwPcObserver observer{};
  observer.size = sizeof(observer);
  observer.on_ice_candidate = OnIceCandidate;
  observer.on_track = OnTrack;
  if (lw_pc_set_observer(sender.pc, &observer, &sender) != 0 ||
      lw_pc_set_observer(receiver.pc, &observer, &receiver) != 0) {
    std::printf("RESULT: FAIL (observer)\n");
    return 1;
  }

  lw_video_source_t* source =
      lw_factory_create_video_source(factory, "live-source");
  lw_video_track_t* track =
      lw_factory_create_video_track(factory, source, "live-video");
  const char* stream_ids[] = {"live-stream"};
  lw_sender_t* rtp_sender = lw_pc_add_track(sender.pc, track, stream_ids, 1);
  if (source == nullptr || track == nullptr || rtp_sender == nullptr) {
    std::printf("RESULT: FAIL (local video)\n");
    return 1;
  }

  lw_pc_create_offer(sender.pc, OnSdp, OnSdpFailure, &g_offer);
  if (!Wait(&g_offer.done) || g_offer.sdp.empty()) {
    std::printf("RESULT: FAIL (no offer)\n");
    return 1;
  }
  g_sdp = ForceH264(g_offer.sdp);
  if (g_sdp.find("H264") == std::string::npos) {
    std::printf("RESULT: FAIL (no H.264 offer)\n");
    return 1;
  }

  auto set_description = [](lw_pc_t* pc, bool local, const std::string& sdp,
                            const char* type) {
    g_set_done_flag = false;
    if (local) {
      lw_pc_set_local_description(pc, sdp.c_str(), type, OnSetDone,
                                  OnSetFailure, &g_set_done_flag);
    } else {
      lw_pc_set_remote_description(pc, sdp.c_str(), type, OnSetDone,
                                   OnSetFailure, &g_set_done_flag);
    }
    return Wait(&g_set_done_flag);
  };

  set_description(sender.pc, true, g_sdp, "offer");
  set_description(receiver.pc, false, g_sdp, "offer");
  receiver.remote_set = true;
  sender.FlushCandidates();

  lw_pc_create_answer(receiver.pc, OnSdp, OnSdpFailure, &g_answer);
  if (!Wait(&g_answer.done) || g_answer.sdp.empty()) {
    std::printf("RESULT: FAIL (no answer)\n");
    return 1;
  }
  set_description(receiver.pc, true, g_answer.sdp, "answer");
  set_description(sender.pc, false, g_answer.sdp, "answer");
  sender.remote_set = true;
  receiver.FlushCandidates();

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
    if (lw_video_source_push_i420(source, kWidth, kHeight, i420.data(),
                                  i420.size()) != 0) {
      std::printf("RESULT: FAIL (push frame)\n");
      return 1;
    }
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
  if (receiver.remote_track != nullptr) {
    lw_video_track_unbind_sink(receiver.remote_track);
    lw_release(receiver.remote_track);
  }
  lw_video_sink_unregister(token);
  lw_pc_remove_observer(sender.pc);
  lw_pc_remove_observer(receiver.pc);
  lw_pc_close(sender.pc);
  lw_pc_close(receiver.pc);
  lw_release(rtp_sender);
  lw_release(track);
  lw_release(source);
  lw_release(sender.pc);
  lw_release(receiver.pc);
  lw_release(factory);
  lw_terminate();
  return pass ? 0 : 1;
}
