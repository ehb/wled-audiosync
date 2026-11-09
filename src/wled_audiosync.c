// SPDX-License-Identifier: GPL-3.0-or-later
//
// wled-audiosync: analyses the audio a player is playing and sends it to WLED as
// "Audio Sync" packets (usermod audioreactive, protocol V2): volume, peak and 16
// frequency bands, 50 times a second, by UDP multicast. WLED's audio-reactive effects
// then follow the music without a microphone.
//
//   wled-audiosync --pipe /tmp/leadtap-sync.fifo [--lead-ms 180] [--target 239.0.0.1]
//                  [--port 11988] [--gain 1.0] [--dry] [-v]
// (--lead-ms 0..2000, --port 1..65535, --gain 0 < g <= 100; exit 2 for bad arguments or a
// path that is not a named pipe, 1 for a runtime error, so a supervisor can tell them apart)
//   wled-audiosync --selftest
//
// The analysis is modelled on WLED's pipeline (usermods/audioreactive/audio_reactive.cpp,
// WLED 16.0.1, read as a reference; the code is written independently, only the band table
// and a few constants are taken over): 512-point FFT at ~22 kHz, the
// same bin groups per band (fftCalc[0..15]), similar rise/fall smoothing and a square-root
// curve like its default scaling. WLED does not run its own gain control on received data,
// so this program does: each band and the volume follow their own slowly decaying maximum,
// the effects look the same at any volume. Not taken over: WLED's pink factors and
// high-band lift, which make up for a microphone.
//
// Input is a named pipe, 48 kHz, 16 bit, stereo (created here if missing); any process can
// feed it. The ALSA plugin alsa-leadtap is the intended source: its copy is silent for
// AC3/DTS and runs ahead of what sounds, so every packet is held back by --lead-ms, which
// must match the plugin's `lead` -- the light follows what is heard, not what is coming.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FFT_N       512     // FFT size after halving the rate, as in WLED
#define SRC_FRAMES  (FFT_N * 2)
#define BANDS       16
#define SEND_HZ     50      // WLED senders transmit every 20 ms
#define IDLE_HZ     5       // zero packets while nothing plays
#define WLED_BIN_HZ (22050.0 / FFT_N)  // WLED's bin width, ~43 Hz
#define IEC61937_PA ((int16_t)0xF872)
#define IEC61937_PB ((int16_t)0x4E1F)
#define IEC_HOLD_S  0.5     // stay silent this long after the last burst preamble
#define GATE_RMS    3.0e-4  // ~ -70 dBFS: below is silence
#define AGC_HALF_S  4.0     // half-life of the level maxima
#define BAND_TARGET 590.0f  // scale of FFT_Magnitude, in the range of a WLED band value
#define BAND_FULL   220.0f  // fftResult of a band at its recent maximum (sqrt curve)
#define PIPE_PERIOD 960     // 20 ms at 48 kHz: one analysis step per period
#define PIPE_IDLE_S 0.040   // no audio for this long counts as a silent period
#define DELAY_SLOTS 128     // 2.5 s of queued packets
#define EARLY_S     0.002   // a packet goes out at most this much before its time
#define MAX_LEAD_MS 2000    // as the plugin; the queue holds 2.5 s
#define VOL_TARGET  180.0f  // volume of a loud passage, WLED's mic range is ~0..255

// WLED Audio Sync V2, 44 bytes, little endian (as ESP32 and ARM).
typedef struct __attribute__((packed))
{
  char header[6];          // "00002"
  uint8_t reserved1[2];
  float sampleRaw;
  float sampleSmth;
  uint8_t samplePeak;
  uint8_t reserved2;
  uint8_t fftResult[BANDS];
  uint16_t reserved3;
  float FFT_Magnitude;
  float FFT_MajorPeak;
} sync_packet_t;
_Static_assert(sizeof(sync_packet_t) == 44, "V2 packet is 44 bytes");
_Static_assert(offsetof(sync_packet_t, sampleRaw) == 8, "offset sampleRaw");
_Static_assert(offsetof(sync_packet_t, samplePeak) == 16, "offset samplePeak");
_Static_assert(offsetof(sync_packet_t, fftResult) == 18, "offset fftResult");
_Static_assert(offsetof(sync_packet_t, FFT_Magnitude) == 36, "offset FFT_Magnitude");
_Static_assert(offsetof(sync_packet_t, FFT_MajorPeak) == 40, "offset FFT_MajorPeak");

// WLED's band table for 22050 Hz without band-pass filter: inclusive bin ranges, damping.
static const int WLED_FROM[BANDS] = {1, 2, 3, 5, 7, 10, 13, 19, 26, 33, 44, 56, 70, 86, 104, 165};
static const int WLED_TO[BANDS] = {2, 3, 5, 7, 10, 13, 19, 26, 33, 44, 56, 70, 86, 104, 165, 215};
static const float WLED_DAMP[BANDS] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.88f, 0.70f};

typedef struct
{
  unsigned rate;            // source rate the bands were set up for
  int from[BANDS], to[BANDS];
  float window[FFT_N];
  float band_peak, vol_peak, decay;  // gain control
  float sum_peak;            // slowly decaying maximum of the band sums, for FFT_Magnitude
  float avg[BANDS];          // WLED's fftAvg (smoothing), here 0..1.5 per band
  float bpeak[BANDS];        // slowly decaying maximum per band
  float smth;                // smoothed volume
  float bass_avg;
  double last_peak_s, iec_until_s;
} analyzer_t;

typedef struct
{
  bool silent;               // send zeros
  bool iec;                  // IEC 61937 data seen
  float rms;
  int loudest_band;          // before gain and scaling, for the selftest
  sync_packet_t pkt;
} result_t;

static volatile sig_atomic_t stop_flag = 0;
static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static double now_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void analyzer_setup(analyzer_t* a, unsigned rate)
{
  memset(a, 0, sizeof(*a));
  a->rate = rate;
  // our bins are (rate / 2) / FFT_N wide; map WLED's bins by frequency
  double f = WLED_BIN_HZ / ((rate / 2.0) / FFT_N);
  for (int i = 0; i < BANDS; i++)
  {
    a->from[i] = (int)lround(WLED_FROM[i] * f);
    a->to[i] = (int)lround(WLED_TO[i] * f);
    if (a->from[i] < 1) a->from[i] = 1;
    if (a->to[i] < a->from[i]) a->to[i] = a->from[i];
    if (a->to[i] > FFT_N / 2 - 1) a->to[i] = FFT_N / 2 - 1;
  }
  for (int n = 0; n < FFT_N; n++)
    a->window[n] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * n / (FFT_N - 1));
  a->decay = (float)pow(0.5, 1.0 / (AGC_HALF_S * SEND_HZ));
  a->band_peak = 1.0f;
  a->sum_peak = 1.0f;
  a->vol_peak = 1.0e-3f;
}

// in-place iterative radix-2 FFT
static void fft(float* re, float* im, int n)
{
  for (int i = 1, j = 0; i < n; i++)
  {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j)
    {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1)
  {
    float ang = -2.0f * (float)M_PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len)
    {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; k++)
      {
        int a = i + k, b = i + k + len / 2;
        float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr; im[b] = im[a] - xi;
        re[a] += xr; im[a] += xi;
        float t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}

static void zero_packet(sync_packet_t* p)
{
  memset(p, 0, sizeof(*p));
  memcpy(p->header, "00002", 6);
  p->FFT_MajorPeak = 1.0f;  // WLED's floor
}

// One analysis step over SRC_FRAMES interleaved stereo frames that are playing now.
static void analyze(analyzer_t* a, const int16_t* frames, double t, float gain, result_t* r)
{
  memset(r, 0, sizeof(*r));
  zero_packet(&r->pkt);
  r->loudest_band = -1;
  // IEC 61937 (AC3/DTS passthrough): compressed data, nothing to analyse
  for (int n = 0; n < SRC_FRAMES; n++)
    if (frames[2 * n] == IEC61937_PA && frames[2 * n + 1] == IEC61937_PB)
      a->iec_until_s = t + IEC_HOLD_S;
  r->iec = t < a->iec_until_s;

  float re[FFT_N], im[FFT_N];
  double sum2 = 0;
  for (int k = 0; k < FFT_N; k++)
  {
    const int16_t* f = frames + 4 * k;  // two stereo frames -> one mono sample at half rate
    float x = ((float)f[0] + f[1] + f[2] + f[3]) / (4.0f * 32768.0f);
    sum2 += (double)x * x;
    re[k] = x * a->window[k] * 32768.0f;  // WLED works on raw sample units
    im[k] = 0.0f;
  }
  r->rms = (float)sqrt(sum2 / FFT_N);
  a->band_peak *= a->decay;
  a->sum_peak *= a->decay;
  a->vol_peak *= a->decay;
  if (r->iec || r->rms < GATE_RMS)
  {
    r->silent = true;
    for (int i = 0; i < BANDS; i++)
    {
      a->avg[i] *= 0.8f;
      a->bpeak[i] *= a->decay;  // keep forgetting old maxima through pauses too
    }
    a->smth *= 0.8f;
    return;
  }

  fft(re, im, FFT_N);
  float mag[FFT_N / 2];
  int major = 1;
  for (int i = 0; i < FFT_N / 2; i++)
  {
    mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]);
    if (i >= 1 && mag[i] > mag[major]) major = i;
  }

  float calc[BANDS], loudest = -1, loudest_sum = -1;
  for (int i = 0; i < BANDS; i++)
  {
    float s = 0;
    for (int b = a->from[i]; b <= a->to[i]; b++) s += mag[b];
    // fftAddAvg as in WLED: the scaled sum divided by the number of bins (an average, not a
    // sum, so wide treble bands do not outweigh narrow bass bands), then the band's damping
    float scaled = s * 0.0625f * WLED_DAMP[i];
    if (scaled > loudest_sum) loudest_sum = scaled;
    calc[i] = s * 0.0625f / (float)(a->to[i] - a->from[i] + 1) * WLED_DAMP[i];
    if (calc[i] > loudest) { loudest = calc[i]; r->loudest_band = i; }
    a->bpeak[i] *= a->decay;
    if (calc[i] > a->bpeak[i]) a->bpeak[i] = calc[i];
  }
  if (loudest > a->band_peak) a->band_peak = loudest;
  if (loudest_sum > a->sum_peak) a->sum_peak = loudest_sum;
  float g = BAND_TARGET / a->sum_peak;  // for FFT_Magnitude (scale unchanged by the averaging)

  // Each band against its own slowly decaying maximum, but never against less than a
  // tenth of the loudest band: nearly empty bands are not blown up into noise. WLED's
  // pink factors and its extra lift of the high bands make up for a microphone that
  // hears little treble; a digital signal has the full treble, with them bands 14/15
  // stuck at 255 (measured on speech). So: one curve for all bands.
  float floor_peak = 0.1f * a->band_peak;
  for (int i = 0; i < BANDS; i++)
  {
    float ref = a->bpeak[i] > floor_peak ? a->bpeak[i] : floor_peak;
    float v = gain * calc[i] / ref;  // 0..1 at the band's recent maximum
    if (v > 1.5f) v = 1.5f;
    // WLED: rise fast, fall slower (decayTime default 1400 ms)
    a->avg[i] = v > a->avg[i] ? v * 0.75f + 0.25f * a->avg[i] : v * 0.17f + 0.83f * a->avg[i];
    float x = BAND_FULL * sqrtf(a->avg[i]);  // square root like WLED's default scaling
    r->pkt.fftResult[i] = (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
  }

  if (r->rms > a->vol_peak) a->vol_peak = r->rms;
  float vol = gain * VOL_TARGET * r->rms / a->vol_peak;
  if (vol > 255.0f) vol = 255.0f;
  a->smth = a->smth * 0.8f + vol * 0.2f;
  r->pkt.sampleRaw = vol;
  r->pkt.sampleSmth = a->smth;

  // beat: the bass bands jump well above their recent average
  float bass = (r->pkt.fftResult[0] + r->pkt.fftResult[1]) / 2.0f;
  if (bass > 80.0f && bass > a->bass_avg * 1.4f && t - a->last_peak_s > 0.1)
  {
    r->pkt.samplePeak = 1;
    a->last_peak_s = t;
  }
  a->bass_avg = a->bass_avg * 0.9f + bass * 0.1f;

  r->pkt.FFT_Magnitude = mag[major] * 0.0625f * g;
  float hz = major * (a->rate / 2.0f) / FFT_N;
  r->pkt.FFT_MajorPeak = hz < 1.0f ? 1.0f : hz > 11025.0f ? 11025.0f : hz;
}

// --- selftest -----------------------------------------------------------------

#define CHECK(name, cond, ...)                                  \
  do {                                                          \
    if (cond) printf("ok   %s\n", name);                        \
    else { printf("FAIL %s: ", name); printf(__VA_ARGS__); printf("\n"); fails++; } \
  } while (0)

// Collects bytes from the nonblocking pipe into whole periods: pipes hand out any byte count.
// 1 = period complete, 0 = not yet, -1 = error.
static int pipe_fill(int fd, void* buf, size_t* have, size_t want)
{
  while (*have < want)
  {
    ssize_t r = read(fd, (char*)buf + *have, want - *have);
    if (r > 0)
      *have += (size_t)r;
    else if (r == 0 || errno == EAGAIN)
      return 0;
    else if (errno != EINTR)
      return -1;
  }
  *have = 0;
  return 1;
}

// Input side of the pipe: whole periods of audio, and a silent period of its own when nothing
// has arrived for PIPE_IDLE_S. Bytes of an incomplete period stay where they are across the
// idle period (a producer may stop in the middle of a sample and go on later); the silent
// period is a separate buffer, so the sample boundaries of later audio do not move.
typedef struct
{
  int16_t buf[PIPE_PERIOD * 2];   // the period being assembled
  int16_t zeros[PIPE_PERIOD * 2];
  size_t have;                    // bytes of buf
  double activity;                // last time audio arrived or an idle period was produced
} pipein_t;

// 1: *out is the next period (audio, or silence after idle); 0: nothing yet; -1: read error.
static int pipein_step(pipein_t* in, int fd, double now, const int16_t** out)
{
  size_t before = in->have;
  int r = pipe_fill(fd, in->buf, &in->have, sizeof in->buf);
  if (r < 0) return -1;
  if (r == 1)
  {
    in->activity = now;
    *out = in->buf;
    return 1;
  }
  if (in->have != before) in->activity = now;
  if (now - in->activity >= PIPE_IDLE_S)
  {
    in->activity = now;
    *out = in->zeros;
    return 1;
  }
  return 0;
}

static int selftest_pipe(void)
{
  int fails = 0, p[2];
  if (pipe2(p, O_NONBLOCK) != 0) return 1;
  int16_t in[8] = {1, 2, 3, 4, 5, 6, 7, 8}, out[4];
  size_t have = 0;
  CHECK("pipe: empty is not a period", pipe_fill(p[0], out, &have, sizeof out) == 0 && have == 0, "have %zu", have);
  if (write(p[1], in, 5) != 5) fails++;  // cuts a sample in half
  CHECK("pipe: partial is kept", pipe_fill(p[0], out, &have, sizeof out) == 0 && have == 5, "have %zu", have);
  if (write(p[1], (char*)in + 5, 11) != 11) fails++;
  int r = pipe_fill(p[0], out, &have, sizeof out);
  CHECK("pipe: period assembled in order", r == 1 && have == 0 && out[0] == 1 && out[3] == 4, "r %d first %d", r, out[0]);
  r = pipe_fill(p[0], out, &have, sizeof out);
  CHECK("pipe: next period", r == 1 && out[0] == 5 && out[3] == 8, "r %d first %d", r, out[0]);
  // idle in the middle of a sample: the incomplete input survives, later audio stays aligned
  static pipein_t pi;
  const int16_t* per = NULL;
  memset(&pi, 0, sizeof pi);
  int16_t big[PIPE_PERIOD * 2 + 4];
  for (size_t i = 0; i < sizeof big / sizeof big[0]; i++) big[i] = (int16_t)(i + 1);
  if (write(p[1], big, 5) != 5) fails++;  // 2.5 samples
  CHECK("idle: partial period is not a period", pipein_step(&pi, p[0], 0.0, &per) == 0 && pi.have == 5, "have %zu", pi.have);
  r = pipein_step(&pi, p[0], 0.1, &per);
  int allzero = r == 1;
  for (size_t i = 0; allzero && i < PIPE_PERIOD * 2; i++) allzero = per[i] == 0;
  CHECK("idle: a silent period is produced after the idle time", allzero && pi.have == 5, "r %d have %zu", r, pi.have);
  if (write(p[1], (char*)big + 5, sizeof big - 5) != (ssize_t)sizeof big - 5) fails++;
  r = pipein_step(&pi, p[0], 0.11, &per);
  CHECK("idle: audio after the idle time keeps its sample boundaries", r == 1 && per[0] == 1 && per[1] == 2 && per[PIPE_PERIOD * 2 - 1] == PIPE_PERIOD * 2,
        "r %d first %d second %d last %d", r, per[0], per[1], per[PIPE_PERIOD * 2 - 1]);
  r = pipein_step(&pi, p[0], 0.111, &per);
  CHECK("idle: the rest of the data is next, after the 4 extra values", r == 0 && pi.have == 4 * sizeof(int16_t), "r %d have %zu", r, pi.have);
  close(p[0]);
  close(p[1]);
  return fails;
}

static bool parse_uint(const char* s, unsigned long lo, unsigned long hi, unsigned long* out);
static bool parse_gain(const char* s, float* out);

static int selftest(void)
{
  static int16_t buf[SRC_FRAMES * 2];
  analyzer_t a;
  result_t r;
  int fails = 0;

  {
    const unsigned rate = 48000;
    // tone -> WLED band (86-129 Hz, 818-1120 Hz, 4479-7106 Hz). WLED's neighbouring bands
    // share their edge bins (ranges are inclusive), so a pure tone may also come out one
    // band off in WLED itself: +-1 is right, further off is a mapping error.
    struct { float hz; int band; } tones[] = {{110, 1}, {950, 7}, {5500, 14}};
    for (unsigned k = 0; k < sizeof(tones) / sizeof(tones[0]); k++)
    {
      analyzer_setup(&a, rate);
      for (int n = 0; n < SRC_FRAMES; n++)
      {
        int16_t v = (int16_t)(12000 * sinf(2.0f * (float)M_PI * tones[k].hz * n / rate));
        buf[2 * n] = buf[2 * n + 1] = v;
      }
      for (int i = 0; i < 20; i++) analyze(&a, buf, i / 50.0, 1.0f, &r);
      char name[80];
      snprintf(name, sizeof name, "%u Hz: %.0f Hz tone lands in band %d (+-1)", rate, tones[k].hz, tones[k].band);
      CHECK(name, abs(r.loudest_band - tones[k].band) <= 1, "loudest band %d", r.loudest_band);
      snprintf(name, sizeof name, "%u Hz: %.0f Hz tone gives a strong band and volume", rate, tones[k].hz);
      CHECK(name, r.pkt.fftResult[r.loudest_band] > 120 && r.pkt.sampleSmth > 100,
            "band %u, smth %.0f", r.pkt.fftResult[r.loudest_band], r.pkt.sampleSmth);
      int far = 0;  // nothing loud far away from the tone
      for (int i = 0; i < BANDS; i++)
        if (abs(i - tones[k].band) > 3 && r.pkt.fftResult[i] > 60) far++;
      snprintf(name, sizeof name, "%u Hz: %.0f Hz tone leaves distant bands quiet", rate, tones[k].hz);
      CHECK(name, far == 0, "%d distant bands above 60", far);
      float hz = r.pkt.FFT_MajorPeak;
      snprintf(name, sizeof name, "%u Hz: major peak near %.0f Hz", rate, tones[k].hz);
      CHECK(name, fabsf(hz - tones[k].hz) < 60.0f, "%.0f Hz", hz);
    }
    analyzer_setup(&a, rate);
    memset(buf, 0, sizeof buf);
    analyze(&a, buf, 0, 1.0f, &r);
    CHECK("silence -> zero packet", r.silent && r.pkt.sampleSmth == 0 && r.pkt.fftResult[3] == 0,
          "silent %d", r.silent);
  }
  // IEC 61937 preamble in otherwise loud audio -> silent, and for a while after
  analyzer_setup(&a, 48000);
  for (int n = 0; n < SRC_FRAMES; n++)
    buf[2 * n] = buf[2 * n + 1] = (int16_t)(12000 * sinf(2.0f * (float)M_PI * 950 * n / 48000));
  buf[200] = IEC61937_PA;
  buf[201] = IEC61937_PB;
  analyze(&a, buf, 10.0, 1.0f, &r);
  CHECK("IEC 61937 burst -> silent", r.silent && r.iec, "silent %d iec %d", r.silent, r.iec);
  buf[200] = buf[201] = 0;
  analyze(&a, buf, 10.2, 1.0f, &r);
  CHECK("IEC hold: still silent 0.2 s later", r.silent, "silent %d", r.silent);
  analyze(&a, buf, 11.0, 1.0f, &r);
  CHECK("IEC hold over after 1 s", !r.silent, "silent %d", r.silent);
  // gain control: a quiet passage after loud ones still moves (peak decays)
  analyzer_setup(&a, 48000);
  for (int i = 0; i < 50; i++) analyze(&a, buf, i / 50.0, 1.0f, &r);  // loud 950 Hz
  for (int n = 0; n < SRC_FRAMES * 2; n++) buf[n] /= 20;               // -26 dB
  for (int i = 0; i < 50 * 20; i++) analyze(&a, buf, 1 + i / 50.0, 1.0f, &r);
  CHECK("gain control: -26 dB recovers within 20 s", r.pkt.sampleSmth > 100 && r.pkt.fftResult[7] > 100,
        "smth %.0f band7 %u", r.pkt.sampleSmth, r.pkt.fftResult[7]);
  {
    float g = 0;
    unsigned long n = 0;
    CHECK("arguments: gain 2.5 accepted", parse_gain("2.5", &g) && g == 2.5f, "g %f", g);
    CHECK("arguments: gain 100 accepted", parse_gain("100", &g), "g %f", g);
    const char* bad_gain[] = {"0", "-1", "100.5", "nan", "inf", "1e999", "", "abc", "1x", " 1"};
    int accepted = 0;
    for (unsigned i = 0; i < sizeof bad_gain / sizeof bad_gain[0]; i++)
      if (parse_gain(bad_gain[i], &g)) accepted++;
    CHECK("arguments: bad gains are refused", accepted == 0, "%d accepted", accepted);
    const char* bad_num[] = {"", "-1", "+5", "12x", " 7", "99999999999999999999", "70000"};
    accepted = 0;
    for (unsigned i = 0; i < sizeof bad_num / sizeof bad_num[0]; i++)
      if (parse_uint(bad_num[i], 1, 65535, &n)) accepted++;
    CHECK("arguments: bad integers are refused", accepted == 0 && parse_uint("11988", 1, 65535, &n) && n == 11988,
          "%d accepted", accepted);
  }
  fails += selftest_pipe();
  // packet header
  sync_packet_t p;
  zero_packet(&p);
  CHECK("header 00002 + NUL", memcmp(p.header, "00002\0", 6) == 0, "header");
  printf(fails ? "\n%d FAILED\n" : "\nALL OK\n", fails);
  return fails ? 1 : 0;
}

// --- Named pipe input (copy from the ALSA plugin alsa-leadtap) ------------------

// Exit codes: 0 stopped by a signal, 1 runtime or system error (a supervisor restarts), 2 the
// arguments or the path are wrong (restarting does not help).
static int run_pipe(const char* path, int sock, const struct sockaddr_in* dst, float gain, bool dry,
                    bool verbose, unsigned lead_ms)
{
  analyzer_t an;
  analyzer_setup(&an, 48000);
  static int16_t frames[SRC_FRAMES * 2];
  static pipein_t in;
  static struct
  {
    double due;
    sync_packet_t pkt;
  } q[DELAY_SLOTS];
  unsigned qh = 0, qt = 0;  // next to send, next free
  double last_send = 0, last_print = 0, last_err = -1e9;
  unsigned long err_count = 0;

  if (mkfifo(path, 0666) != 0 && errno != EEXIST)
  {
    fprintf(stderr, "wled-audiosync: mkfifo %s: %s\n", path, strerror(errno));
    return 1;
  }
  // read-write: the pipe then never reports end-of-file while the player is stopped
  int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
  {
    fprintf(stderr, "wled-audiosync: open %s: %s\n", path, strerror(errno));
    return 1;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode))
  {
    fprintf(stderr, "wled-audiosync: %s is not a named pipe\n", path);
    close(fd);
    return 2;
  }
  printf("wled-audiosync: reading %s, holding packets back %u ms\n", path, lead_ms);
  fflush(stdout);
  in.activity = now_s();

  int rc = 0;
  while (!stop_flag)
  {
    // sleep until audio arrives, but not past the idle time or the next packet's time
    double t = now_s(), wake = in.activity + PIPE_IDLE_S;
    if (qh != qt && q[qh].due - EARLY_S < wake) wake = q[qh].due - EARLY_S;
    int timeout_ms = wake <= t ? 0 : (int)ceil((wake - t) * 1000.0);
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int ready = poll(&pfd, 1, timeout_ms);
    if (ready < 0 && errno != EINTR)
    {
      fprintf(stderr, "wled-audiosync: poll: %s\n", strerror(errno));
      rc = 1;
      break;
    }
    t = now_s();
    const int16_t* per = NULL;
    int got = ready < 0 ? 0 : pipein_step(&in, fd, t, &per);
    if (got < 0)
    {
      fprintf(stderr, "wled-audiosync: read: %s\n", strerror(errno));
      rc = 1;
      break;
    }
    if (got == 1)
    {
      // slide the analysis window by what arrived (or by silence while idle)
      memmove(frames, frames + 2 * PIPE_PERIOD, (size_t)(SRC_FRAMES - PIPE_PERIOD) * 4);
      memcpy(frames + 2 * (SRC_FRAMES - PIPE_PERIOD), per, sizeof in.buf);
      result_t r;
      analyze(&an, frames, t, gain, &r);
      // while silent only a few packets a second, enough to keep WLED's effects at rest
      if (!(r.silent && t - last_send < 1.0 / IDLE_HZ))
      {
        last_send = t;
        if ((qt + 1) % DELAY_SLOTS != qh)
        {
          q[qt].due = t + lead_ms / 1000.0;
          q[qt].pkt = r.pkt;
          qt = (qt + 1) % DELAY_SLOTS;
        }
        if ((dry || verbose) && t - last_print >= 0.2)
        {
          last_print = t;
          printf("%s vol %5.1f smth %5.1f peak %u major %6.0f Hz bands", r.silent ? "quiet" : "music",
                 r.pkt.sampleRaw, r.pkt.sampleSmth, r.pkt.samplePeak, r.pkt.FFT_MajorPeak);
          for (int i = 0; i < BANDS; i++) printf(" %3u", r.pkt.fftResult[i]);
          printf("\n");
          fflush(stdout);
        }
      }
    }
    // send what is due (at most EARLY_S early: the loop wakes for the packet's time)
    while (qh != qt && q[qh].due <= t + EARLY_S)
    {
      if (!dry && sendto(sock, &q[qh].pkt, sizeof q[qh].pkt, 0, (const struct sockaddr*)dst, sizeof *dst) < 0)
      {
        // a lost network is not fatal (it may come back), but it is said, at most every 10 s
        err_count++;
        if (t - last_err >= 10.0)
        {
          fprintf(stderr, "wled-audiosync: send: %s (%lu failed so far)\n", strerror(errno), err_count);
          last_err = t;
        }
      }
      qh = (qh + 1) % DELAY_SLOTS;
    }
  }
  close(fd);
  return rc;
}


// --- main ---------------------------------------------------------------------

// Strict numbers: the whole argument, no sign or blanks tricks, within the range.
static bool parse_uint(const char* s, unsigned long lo, unsigned long hi, unsigned long* out)
{
  if (!*s || *s < '0' || *s > '9') return false;
  char* end;
  errno = 0;
  unsigned long v = strtoul(s, &end, 10);
  if (errno || *end || v < lo || v > hi) return false;
  *out = v;
  return true;
}

static bool parse_gain(const char* s, float* out)
{
  if (!((*s >= '0' && *s <= '9') || *s == '.')) return false;  // strtod would skip blanks
  char* end;
  errno = 0;
  double v = strtod(s, &end);
  if (errno || end == s || *end || !isfinite(v) || v <= 0.0 || v > 100.0) return false;
  *out = (float)v;
  return true;
}

int main(int argc, char** argv)
{
  const char* target = "239.0.0.1";
  int port = 11988;
  float gain = 1.0f;
  bool dry = false, verbose = false;
  const char* pipe_path = NULL;
  unsigned lead_ms = 180;
  bool usage = false, bad_value = false;
  unsigned long n;
  for (int i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--selftest")) return selftest();
    else if (!strcmp(argv[i], "--dry")) dry = true;
    else if (!strcmp(argv[i], "-v")) verbose = true;
    else if (!strcmp(argv[i], "--target") && i + 1 < argc) target = argv[++i];
    else if (!strcmp(argv[i], "--port") && i + 1 < argc)
    {
      if (parse_uint(argv[++i], 1, 65535, &n)) port = (int)n;
      else { fprintf(stderr, "wled-audiosync: --port must be 1..65535\n"); bad_value = true; }
    }
    else if (!strcmp(argv[i], "--gain") && i + 1 < argc)
    {
      if (!parse_gain(argv[++i], &gain))
      {
        fprintf(stderr, "wled-audiosync: --gain must be a number with 0 < g <= 100\n");
        bad_value = true;
      }
    }
    else if (!strcmp(argv[i], "--pipe") && i + 1 < argc) pipe_path = argv[++i];
    else if (!strcmp(argv[i], "--lead-ms") && i + 1 < argc)
    {
      if (parse_uint(argv[++i], 0, MAX_LEAD_MS, &n)) lead_ms = (unsigned)n;
      else { fprintf(stderr, "wled-audiosync: --lead-ms must be 0..%d\n", MAX_LEAD_MS); bad_value = true; }
    }
    else usage = true;
  }
  if (bad_value) return 2;
  if (usage || !pipe_path)
  {
    fprintf(stderr,
            "usage: %s --pipe path [--lead-ms n] [--target ip] [--port n] [--gain g] [--dry] [-v]\n"
            "       %s --selftest\n",
            argv[0], argv[0]);
    return 2;
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (sock < 0 || inet_pton(AF_INET, target, &dst.sin_addr) != 1)
  {
    fprintf(stderr, "wled-audiosync: bad target %s\n", target);
    return 2;
  }
  unsigned char ttl = 1;
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
  printf("wled-audiosync: %s -> %s:%d%s\n", pipe_path, target, port, dry ? " (dry)" : "");
  fflush(stdout);
  return run_pipe(pipe_path, sock, &dst, gain, dry, verbose, lead_ms);
}
