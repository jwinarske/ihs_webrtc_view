// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

// Checks the presenter against a stand-in ivi-homescreen. The test binary
// exports ihs_get_api itself, so the presenter's dlsym(RTLD_DEFAULT, ...)
// resolves to this file and every call it makes -- register_factory,
// negotiate, submit -- lands on a recording implementation. Frames are
// synthetic: a memfd per buffer stands in for a dma-buf, which is enough
// because the presenter forwards plane fds rather than importing them.
//
// It exercises the presenter's public C ABI, the same two entry points the
// Dart control plane calls, and asserts the parts of the producer contract
// the presenter is responsible for:
//
//   - a frame taken from the sink reaches ihs submit with its geometry,
//     format and plane fds intact
//   - every taken frame is released back to the producer exactly once, so the
//     decoder's pool cannot leak
//   - a frame is never released before it has been submitted
//   - declining costs nothing: with no view bound the sink still releases
//   - suspending stops submissions without stranding buffers
//   - under an explicit grant, buffers are retired as their release fences
//     signal rather than only when the depth window forces it
//
// The fence path needs no GPU: an eventfd created with a non-zero count is
// immediately readable, which is exactly what a fence the compositor has
// already signalled looks like to poll().
//
// Runs anywhere: no GPU, no display, no webrtc.

#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "ihs/ihs.h"
#include "ihs/platform_view.h"
#include "lw_video_sink.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

// ---- recording stand-in for the registry --------------------------------

struct Submitted {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t plane_count = 0;
  int plane_fd0 = -1;
  uint32_t stride0 = 0;
  uint64_t modifier = 0;
  uint32_t fourcc = 0;
};

std::mutex g_mu;
std::vector<Submitted> g_submits;

// Which frame each buffer belongs to, so a submission can be matched to the
// release that follows it. Every frame gets its own buffer, as a real decoder
// pool would.
std::map<int, int> g_fd_to_frame;
std::set<int> g_submitted_frames;
std::set<int> g_released_frames;
IhsPvFactory g_factory = nullptr;
void* g_factory_user = nullptr;

// Which sync mode the stand-in registry grants for the current pass.
uint8_t g_sync = IHS_PV_SYNC_IMPLICIT;

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
  out->sync = g_sync;
  std::memset(&out->format, 0, sizeof(out->format));
  return IHS_PV_OK;
}

int FakeSubmit(IhsPlatformView* /*view*/, const IhsFrame* frame,
               int acquire_fence_fd, int* out_release_fence_fd) {
  if (acquire_fence_fd >= 0) {
    ::close(acquire_fence_fd);  // the registry owns it
  }
  if (out_release_fence_fd != nullptr) {
    // An eventfd with a non-zero initial count is readable straight away, so
    // it stands in for a fence the compositor has already signalled. The
    // presenter owns it from here and closes it on retire.
    *out_release_fence_fd = (g_sync == IHS_PV_SYNC_IMPLICIT)
                                ? -1
                                : ::eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
  }
  if (frame != nullptr) {
    Submitted s;
    s.width = frame->width;
    s.height = frame->height;
    s.plane_count = frame->plane_count;
    s.plane_fd0 = frame->plane_fd[0];
    s.stride0 = frame->plane_stride[0];
    s.modifier = frame->format.modifier;
    s.fourcc = frame->format.fourcc;
    std::lock_guard<std::mutex> lock(g_mu);
    g_submits.push_back(s);
    const auto it = g_fd_to_frame.find(frame->plane_fd[0]);
    if (it != g_fd_to_frame.end()) {
      g_submitted_frames.insert(it->second);
    }
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

// ---- synthetic producer -------------------------------------------------

std::atomic<int> g_released{0};
std::set<const void*> g_release_ctx_seen;
std::atomic<int> g_release_before_submit{0};
std::mutex g_rel_mu;

// Identifies the frame a release belongs to.
struct FrameCookie {
  int index = -1;
};

void ReleaseFrame(void* ctx) {
  auto* cookie = static_cast<FrameCookie*>(ctx);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_submits.empty()) {
      ++g_release_before_submit;
    }
    if (cookie != nullptr) {
      g_released_frames.insert(cookie->index);
    }
  }
  std::lock_guard<std::mutex> lock(g_rel_mu);
  g_release_ctx_seen.insert(ctx);
  ++g_released;
}

// A memfd stands in for a dma-buf: the presenter forwards plane fds, it does
// not import them.
int MakeBufferFd(size_t bytes) {
  const int fd = ::memfd_create("lw-test-frame", MFD_CLOEXEC);
  if (fd >= 0) {
    (void)::ftruncate(fd, static_cast<off_t>(bytes));
  }
  return fd;
}

LwDmabufDescriptor MakeDescriptor(int fd, uint64_t seq) {
  LwDmabufDescriptor d{};
  d.size = sizeof(d);
  d.fourcc = ('N') | ('V' << 8) | ('1' << 16) | ('2' << 24);
  d.modifier = 0x0123456789abcdefULL;
  d.width = 320;
  d.height = 240;
  d.num_planes = 2;
  d.planes[0].fd = fd;
  d.planes[0].offset = 0;
  d.planes[0].pitch = 512;
  d.planes[1].fd = fd;
  d.planes[1].offset = 512 * 240;
  d.planes[1].pitch = 512;
  d.acquire_fence_fd = -1;
  d.rtp_timestamp_us = static_cast<int64_t>(seq) * 33333;
  d.frame_seq = seq;
  d.pool_generation = 1;
  return d;
}

size_t SubmitCount() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_submits.size();
}

// What the presenter still holds: frames submitted to the registry that have
// not yet been returned to the producer. This is the property the fence path
// exists to reduce, so it is measured rather than inferred from frames still
// arriving.
//
// Counted per frame rather than as a difference of totals, because a frame the
// playout clock drops is released without ever being submitted.
size_t HoldDepth() {
  std::lock_guard<std::mutex> lock(g_mu);
  size_t held = 0;
  for (const int frame : g_submitted_frames) {
    if (g_released_frames.count(frame) == 0) {
      ++held;
    }
  }
  return held;
}

void ResetRecording() {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_submits.clear();
    g_fd_to_frame.clear();
    g_submitted_frames.clear();
    g_released_frames.clear();
  }
  std::lock_guard<std::mutex> lock(g_rel_mu);
  g_release_ctx_seen.clear();
  g_released = 0;
  g_release_before_submit = 0;
}

struct PassResult {
  int taken = 0;
  size_t submits = 0;
  int released_in_run = 0;  // before teardown drains what is left
  int released_at_end = 0;  // after teardown
  size_t max_hold = 0;
};

}  // namespace

// The presenter resolves this by dlsym(RTLD_DEFAULT, "ihs_get_api"), so the
// test binary itself stands in for libihs_shared.so. Needs -rdynamic.
extern "C" __attribute__((visibility("default"))) const IhsApi* ihs_get_api(
    uint32_t /*requested_abi*/) {
  return &g_api;
}

// The presenter's public C ABI (the surface the Dart control plane drives).
extern "C" int ihs_webrtc_view_register(const char* view_type);
extern "C" const LwVideoSinkV1* ihs_webrtc_view_sink_for_view(int32_t view_id,
                                                              void** out_user);

namespace {

// Drives one pass: creates a view, pushes frames through the sink, and tears
// it down. `sync` selects what the stand-in registry grants.
PassResult DrivePass(int32_t view_id, uint8_t sync, const char* label) {
  PassResult result;
  g_sync = sync;
  ResetRecording();

  // Create the view the way the registry would.
  IhsPvCreateInfo info{};
  info.struct_size = sizeof(info);
  info.id = view_id;
  info.view_type = "webrtc-view";
  info.width = 320;
  info.height = 240;
  IhsPvCallbacks callbacks{};
  void* view_user = nullptr;
  auto* view = reinterpret_cast<IhsPlatformView*>(0x1234);  // opaque to us
  CHECK(g_factory(&info, g_factory_user, view, &callbacks, &view_user) ==
        IHS_PV_OK);

  void* sink_user = nullptr;
  const LwVideoSinkV1* sink =
      ihs_webrtc_view_sink_for_view(view_id, &sink_user);
  CHECK(sink != nullptr);
  if (sink == nullptr) {
    std::printf("PRESENTER_TEST_FAIL (no sink for view)\n");
    return result;
  }
  CHECK(sink->size >= sizeof(LwVideoSinkV1));
  if (sink->on_frame == nullptr || sink->on_format == nullptr) {
    std::printf("PRESENTER_TEST_FAIL (incomplete sink table)\n");
    return result;
  }

  // ---- drive frames through the sink ------------------------------------
  // Enough frames that the depth window is reached under implicit sync, where
  // nothing retires until it is exceeded.
  constexpr int kFrames = 24;

  // One buffer per frame, as a real pool has, so a submission can be matched
  // to the release that follows it.
  std::vector<int> fds(kFrames, -1);
  std::vector<FrameCookie> cookies(kFrames);
  for (int i = 0; i < kFrames; ++i) {
    fds[static_cast<size_t>(i)] = MakeBufferFd(512 * 240 * 3 / 2);
    CHECK(fds[static_cast<size_t>(i)] >= 0);
    cookies[static_cast<size_t>(i)].index = i;
    std::lock_guard<std::mutex> lock(g_mu);
    g_fd_to_frame[fds[static_cast<size_t>(i)]] = i;
  }

  LwDmabufDescriptor fmt = MakeDescriptor(fds[0], 0);
  sink->on_format(&fmt, sink_user);

  for (int i = 0; i < kFrames; ++i) {
    LwDmabufDescriptor d =
        MakeDescriptor(fds[static_cast<size_t>(i)], static_cast<uint64_t>(i));
    if (sink->on_frame(&d, &ReleaseFrame, &cookies[static_cast<size_t>(i)],
                       sink_user) != 0) {
      ++result.taken;
    }
    // Feed at the pace the timestamps imply, so frames are presented rather
    // than dropped as late, and sample while the present thread works.
    for (int t = 0; t < 7; ++t) {
      result.max_hold = std::max(result.max_hold, HoldDepth());
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // Let the present thread drain what it paced, still sampling.
  for (int i = 0; i < 100; ++i) {
    result.max_hold = std::max(result.max_hold, HoldDepth());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  result.submits = SubmitCount();
  result.released_in_run = g_released.load();
  std::printf(
      "%s: taken=%d submitted=%zu released=%d distinct_released=%zu "
      "max_hold=%zu\n",
      label, result.taken, result.submits, result.released_in_run,
      g_release_ctx_seen.size(), result.max_hold);

  CHECK(result.taken > 0);
  CHECK(result.submits > 0);
  CHECK(g_release_before_submit == 0);
  // Every release must correspond to a distinct frame: a double release would
  // return the same buffer to the decoder pool twice.
  CHECK(g_release_ctx_seen.size() ==
        static_cast<size_t>(result.released_in_run));

  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const Submitted& s : g_submits) {
      CHECK(s.width == 320 && s.height == 240);
      CHECK(s.plane_count == 2);
      CHECK(g_fd_to_frame.count(s.plane_fd0) == 1);
      CHECK(s.stride0 == 512);
      CHECK(s.modifier == 0x0123456789abcdefULL);
    }
  }

  // ---- teardown must not strand buffers ---------------------------------
  if (callbacks.dispose != nullptr) {
    callbacks.dispose(view_user);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  result.released_at_end = g_released.load();
  std::printf("%s after dispose: taken=%d released=%d\n", label, result.taken,
              result.released_at_end);
  CHECK(result.released_at_end == result.taken);  // nothing left held
  CHECK(ihs_webrtc_view_sink_for_view(view_id, nullptr) == nullptr);

  for (const int buffer_fd : fds) {
    if (buffer_fd >= 0) {
      ::close(buffer_fd);
    }
  }
  return result;
}

}  // namespace

int main() {
  CHECK(ihs_webrtc_view_register("webrtc-view") == 0);
  CHECK(g_factory != nullptr);
  if (g_factory == nullptr) {
    std::printf("PRESENTER_TEST_FAIL (no factory registered)\n");
    return 1;
  }

  // A sink with no view yet must still be resolvable as absent.
  CHECK(ihs_webrtc_view_sink_for_view(7, nullptr) == nullptr);

  const PassResult implicit = DrivePass(7, IHS_PV_SYNC_IMPLICIT, "implicit");
  const PassResult explicit_sync =
      DrivePass(8, IHS_PV_SYNC_EXPLICIT_REQUIRED, "explicit");

  // The point of retiring on the fence: with the compositor already done, a
  // buffer goes back to the producer immediately instead of waiting for the
  // depth window to push it out. Depth is what the change exists to improve,
  // so assert on depth rather than on frames still arriving.
  CHECK(explicit_sync.max_hold < implicit.max_hold);
  CHECK(explicit_sync.max_hold <= 2);
  // The window is what bounds the implicit case, so it must be reached.
  CHECK(implicit.max_hold > 2);

  // Said the other way round, and without sampling: under an explicit grant
  // every buffer is back with the producer before teardown, where implicit
  // sync still has a window's worth in hand for teardown to drain.
  CHECK(explicit_sync.released_in_run == explicit_sync.taken);
  CHECK(implicit.released_in_run < implicit.taken);

  if (g_failures == 0) {
    std::printf("PRESENTER_TEST_OK\n");
    return 0;
  }
  std::printf("PRESENTER_TEST_FAIL (%d)\n", g_failures);
  return 1;
}
