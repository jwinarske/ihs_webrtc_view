// webrtc_session — a minimal native WebRTC receive session over the libwebrtc
// flat C ABI, exposing just what the Dart control plane needs: start a session
// (connect + answer + ICE on a background thread) and poll for the decoded
// video-track handle. All the signaling-thread callback work stays in C, so
// Dart never has to service native-thread callbacks; it polls a pointer.
//
// The track handle returned is the session's persistent VideoTrackImpl wrapper
// (kept alive for the session), so the platform-view binding can register a
// native sink against it with lw_video_track_bind_sink.
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "c/lw_c_api.h"

typedef struct {
  int sock;
  pthread_mutex_t send_m;
  pthread_t thread;
  lw_factory_t* factory;
  lw_pc_t* pc;
  lw_video_track_t* video_track;  // persistent wrapper; keeps the sink live
  volatile int ice_state;
  volatile int running;
} Session;

static Session g_s;

static void send_framed(const char* header, const char* payload, int len) {
  pthread_mutex_lock(&g_s.send_m);
  (void)!write(g_s.sock, header, strlen(header));
  if (len > 0) (void)!write(g_s.sock, payload, len);
  pthread_mutex_unlock(&g_s.send_m);
}

// ---- observer callbacks (signaling thread) ----
// The payloads below are owned by the callback: read them, then hand them
// back with lw_string_free.
static void on_ice_candidate(char* candidate, char* sdp_mid, int mline,
                             void* user) {
  (void)user;
  if (candidate != NULL) {
    char hdr[64];
    int n = (int)strlen(candidate);
    snprintf(hdr, sizeof hdr, "CAND %d %d\n", mline, n);
    send_framed(hdr, candidate, n);
  }
  lw_string_free(candidate);
  lw_string_free(sdp_mid);
}
static void on_ice_connection_state(int state, void* user) {
  (void)user;
  g_s.ice_state = state;
  fprintf(stderr, "[session] ice_connection_state=%d\n", state);
}
static void on_track(lw_transceiver_t* transceiver, void* user) {
  (void)user;
  lw_receiver_t* rx = lw_transceiver_receiver(transceiver);
  lw_video_track_t* vt = rx ? lw_receiver_video_track(rx) : NULL;
  fprintf(stderr, "[session] OnTrack: video_track=%s\n", vt ? "yes" : "no");
  // Publish the persistent wrapper for the Dart side to bind a sink to.
  g_s.video_track = vt;  // released in stop()
  if (rx) lw_release((void*)rx);
  lw_release((void*)transceiver);
}

// ---- SDP async chain ----
static void on_fail(char* err, void* user) {
  (void)user;
  fprintf(stderr, "[session] sdp error: %s\n", err ? err : "?");
  lw_string_free(err);
}
static void on_answer_created(char* sdp, char* type, void* user) {
  (void)user;
  if (sdp != NULL) {
    lw_pc_set_local_description(g_s.pc, sdp, "answer", NULL, on_fail, NULL);
    int n = (int)strlen(sdp);
    char hdr[64];
    snprintf(hdr, sizeof hdr, "ANSWER %d\n", n);
    send_framed(hdr, sdp, n);
    fprintf(stderr, "[session] answer sent (%d bytes)\n", n);
  }
  lw_string_free(sdp);
  lw_string_free(type);
}
static void on_remote_offer_set(void* user) {
  (void)user;
  lw_pc_create_answer(g_s.pc, on_answer_created, on_fail, NULL);
}

// ---- framed reader ----
static int read_line(int fd, char* out, int max) {
  int i = 0;
  while (i < max - 1) {
    char c;
    int r = read(fd, &c, 1);
    if (r <= 0) return -1;
    if (c == '\n') break;
    out[i++] = c;
  }
  out[i] = 0;
  return i;
}
static int read_n(int fd, char* buf, int n) {
  int got = 0;
  while (got < n) {
    int r = read(fd, buf + got, n - got);
    if (r <= 0) return -1;
    got += r;
  }
  return got;
}

static void* session_loop(void* arg) {
  (void)arg;
  char header[128];
  char* payload = malloc(1 << 16);
  while (g_s.running) {
    struct timeval tv = {1, 0};
    setsockopt(g_s.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int hn = read_line(g_s.sock, header, sizeof header);
    if (hn <= 0) continue;
    char kind[16];
    int a = 0, b = 0;
    if (sscanf(header, "%15s", kind) != 1) continue;
    if (strcmp(kind, "OFFER") == 0) {
      sscanf(header, "%*s %d", &b);
      if (read_n(g_s.sock, payload, b) == b) {
        payload[b] = 0;
        lw_pc_set_remote_description(g_s.pc, payload, "offer",
                                     on_remote_offer_set, on_fail, NULL);
      }
    } else if (strcmp(kind, "CAND") == 0) {
      sscanf(header, "%*s %d %d", &a, &b);
      if (read_n(g_s.sock, payload, b) == b) {
        payload[b] = 0;
        lw_pc_add_ice_candidate(g_s.pc, "", a, payload);
      }
    }
  }
  free(payload);
  return NULL;
}

// ---- public C API (for Dart FFI) ----

// Starts a receive session: initializes libwebrtc, connects the TCP signaling
// socket to host:port, and runs the answer/ICE loop on a background thread.
// Returns 0 on success, negative on error.
int webrtc_session_start(const char* host, int port) {
  if (g_s.running) return 0;
  if (!lw_initialize()) return -1;
  g_s.factory = lw_factory_create();
  if (!g_s.factory || !lw_factory_initialize(g_s.factory)) return -2;
  g_s.pc = lw_pc_create(g_s.factory);
  if (!g_s.pc) return -3;
  pthread_mutex_init(&g_s.send_m, NULL);

  LwPcObserver obs;
  memset(&obs, 0, sizeof obs);
  obs.size = sizeof obs;
  obs.on_ice_candidate = on_ice_candidate;
  obs.on_ice_connection_state = on_ice_connection_state;
  obs.on_track = on_track;
  lw_pc_set_observer(g_s.pc, &obs, NULL);

  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char portstr[16];
  snprintf(portstr, sizeof portstr, "%d", port);
  if (getaddrinfo(host, portstr, &hints, &res) != 0) return -4;
  g_s.sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (connect(g_s.sock, res->ai_addr, res->ai_addrlen) != 0) {
    freeaddrinfo(res);
    return -5;
  }
  freeaddrinfo(res);
  fprintf(stderr, "[session] connected to %s:%d\n", host, port);

  g_s.running = 1;
  if (pthread_create(&g_s.thread, NULL, session_loop, NULL) != 0) {
    g_s.running = 0;
    return -6;
  }
  return 0;
}

// Returns the decoded video-track handle as an address (0 until OnTrack fires).
// Stable for the session; hand it to the platform-view binding.
// The id webrtc gave the remote track. Owned by the caller (lw_string_free),
// NULL before the track arrives. A consumer that resolves the track by id --
// as a platform view does -- needs this rather than the handle.
char* webrtc_session_track_id(void) {
  return g_s.video_track != NULL ? lw_video_track_id(g_s.video_track) : NULL;
}

uintptr_t webrtc_session_track(void) {
  return (uintptr_t)g_s.video_track;
}

int webrtc_session_ice_state(void) { return g_s.ice_state; }

void webrtc_session_stop(void) {
  if (!g_s.running) return;
  g_s.running = 0;
  pthread_join(g_s.thread, NULL);
  if (g_s.video_track) lw_release((void*)g_s.video_track);
  lw_pc_remove_observer(g_s.pc);
  lw_pc_close(g_s.pc);
  lw_release((void*)g_s.pc);
  lw_release((void*)g_s.factory);
  lw_terminate();
  close(g_s.sock);
  memset(&g_s, 0, sizeof g_s);
}
