#include "dsp/dsp_filters.h"
#include <stddef.h>

/* ============================================================================
 * DSP Filters Module Implementation
 * 
 * This module provides signal conditioning and dynamic processing:
 * - Removes DC offset and subsonic rumble
 * - Applies gentle compression for smooth dynamics
 * - Protects output with fast peak limiting
 * - Simulates guitar cabinet frequency response
 * ============================================================================ */

/* Configuration Constants */
#define AUDIO_LIMITER_ENABLE           1
#define AUDIO_LIMITER_THRESH_S24       6500000  /**< Limiter threshold (signed 24-bit) */
#define AUDIO_LIMITER_RELEASE_Q15      16       /**< Limiter release speed in Q15 */

#define CLEAN_COMP_ENABLE              1
#define CLEAN_COMP_THRESH_S24          3000000  /**< Compressor threshold */
#define CLEAN_COMP_RATIO               2        /**< Compression ratio (2:1) */
#define CLEAN_COMP_ENV_ATTACK_Q15      4096     /**< Envelope attack coefficient */
#define CLEAN_COMP_ENV_RELEASE_Q15     256      /**< Envelope release coefficient */
#define CLEAN_COMP_GAIN_ATTACK_Q15     8192     /**< Gain smoothing attack */
#define CLEAN_COMP_GAIN_RELEASE_Q15    512      /**< Gain smoothing release */

/* Cabinet simulator biquad coefficients (Q28 format)
 * Butterworth lowpass ~5kHz @ fs=48kHz */
#define CAB_B0_Q28   ((int32_t)19407624)   /* ~0.0723 */
#define CAB_B1_Q28   ((int32_t)38815248)   /* ~0.1446 */
#define CAB_B2_Q28   ((int32_t)19407624)   /* ~0.0723 */
#define CAB_A1_Q28   ((int32_t)-297323915) /* ~-1.108 */
#define CAB_A2_Q28   ((int32_t)106689359)  /* ~0.3976 */

/* Static State Variables */
static LimiterState s_limiter = {32768};

static DcBlockState s_dc_l = {0, 0};
static DcBlockState s_dc_r = {0, 0};

static DcBlockState s_clean_hpf_l = {0, 0};
static DcBlockState s_clean_hpf_r = {0, 0};

static DcBlockState s_wet_hpf_delay_l = {0, 0};
static DcBlockState s_wet_hpf_delay_r = {0, 0};
static DcBlockState s_wet_hpf_reverb_l = {0, 0};
static DcBlockState s_wet_hpf_reverb_r = {0, 0};

static CompState s_comp_l = {0, 32768};
static CompState s_comp_r = {0, 32768};

static BiquadState s_cab_l = {0, 0, 0, 0};
static BiquadState s_cab_r = {0, 0, 0, 0};

static int32_t s_wet_lpf_delay_l = 0;
static int32_t s_wet_lpf_delay_r = 0;
static int32_t s_wet_lpf_reverb_l = 0;
static int32_t s_wet_lpf_reverb_r = 0;

/* Helper Functions */

/**
 * @brief Clamp value to signed 24-bit range
 * @param x Input value
 * @return Clamped value in range [-8388608, 8388607]
 */
static inline int32_t clamp_s24(int32_t x)
{
  if (x > 8388607) return 8388607;
  if (x < -8388608) return -8388608;
  return x;
}

/**
 * @brief Get absolute value of signed 32-bit integer
 * @param x Input value
 * @return Absolute value
 */
static inline int32_t abs_s32(int32_t x)
{
  return (x >= 0) ? x : -x;
}

/* Public API Implementation */

void DspFilters_Init(void)
{
  /* Reset DC blockers */
  s_dc_l.x1 = s_dc_l.y1 = 0;
  s_dc_r.x1 = s_dc_r.y1 = 0;

  /* Reset clean HPFs */
  s_clean_hpf_l.x1 = s_clean_hpf_l.y1 = 0;
  s_clean_hpf_r.x1 = s_clean_hpf_r.y1 = 0;

  /* Reset wet signal HPFs */
  s_wet_hpf_delay_l.x1 = s_wet_hpf_delay_l.y1 = 0;
  s_wet_hpf_delay_r.x1 = s_wet_hpf_delay_r.y1 = 0;
  s_wet_hpf_reverb_l.x1 = s_wet_hpf_reverb_l.y1 = 0;
  s_wet_hpf_reverb_r.x1 = s_wet_hpf_reverb_r.y1 = 0;

  /* Reset compressors */
  s_comp_l.env = 0;
  s_comp_l.gain_q15 = 32768;
  s_comp_r.env = 0;
  s_comp_r.gain_q15 = 32768;

  /* Reset cabinet sim */
  s_cab_l.x1 = s_cab_l.x2 = s_cab_l.y1 = s_cab_l.y2 = 0;
  s_cab_r.x1 = s_cab_r.x2 = s_cab_r.y1 = s_cab_r.y2 = 0;

  /* Reset wet signal LPFs */
  s_wet_lpf_delay_l = 0;
  s_wet_lpf_delay_r = 0;
  s_wet_lpf_reverb_l = 0;
  s_wet_lpf_reverb_r = 0;

  /* Reset limiter */
  s_limiter.gain_q15 = 32768;
}

int32_t DspFilters_DcBlock(DcBlockState *st, int32_t x)
{
  /* First-order highpass with R ~= 0.997 gives ~20Hz cutoff @ 48kHz
   * Transfer function: H(z) = (1 - z^-1) / (1 - R*z^-1)
   * Difference equation: y[n] = x[n] - x[n-1] + R*y[n-1] */
  const int32_t r_q15 = 32684; /* 0.997 in Q15 format */
  
  int32_t y = x - st->x1 + (int32_t)(((int64_t)r_q15 * st->y1) >> 15);
  st->x1 = x;
  st->y1 = y;
  return clamp_s24(y);
}

int32_t DspFilters_Hpf1(DcBlockState *st, int32_t x, int32_t r_q15)
{
  /* Generic first-order highpass filter
   * r_q15 determines cutoff frequency:
   * - ~32384 (0.9883) gives ~90Hz @ 48kHz (clean HPF)
   * - ~32004 (0.9767) gives ~180Hz @ 48kHz (wet HPF) */
  
  int32_t y = x - st->x1 + (int32_t)(((int64_t)r_q15 * st->y1) >> 15);
  st->x1 = x;
  st->y1 = y;
  return clamp_s24(y);
}

void DspFilters_CompressOne(CompState *st, int32_t *x_s24)
{
#if CLEAN_COMP_ENABLE
  int32_t x = abs_s32(*x_s24);

  /* Envelope follower: tracks peak amplitude with asymmetric attack/release
   * Fast attack captures transients, slow release avoids pumping */
  int32_t env = st->env;
  int32_t diff = x - env;
  int32_t k_env = (diff > 0) ? CLEAN_COMP_ENV_ATTACK_Q15 : CLEAN_COMP_ENV_RELEASE_Q15;
  env += (int32_t)(((int64_t)k_env * (int64_t)diff) >> 15);
  if (env < 0) env = 0;
  st->env = env;

  /* Calculate target gain based on compression ratio
   * If envelope exceeds threshold:
   *   output_level = threshold + (envelope - threshold) / ratio
   *   gain = output_level / envelope
   * This reduces dynamic range above threshold */
  int32_t target_gain_q15 = 32768; /* Unity gain by default */
  if (env > CLEAN_COMP_THRESH_S24)
  {
    int32_t over = env - CLEAN_COMP_THRESH_S24;
    int32_t out_env = CLEAN_COMP_THRESH_S24 + (over / CLEAN_COMP_RATIO);
    target_gain_q15 = (int32_t)(((int64_t)out_env << 15) / (int64_t)env);
    if (target_gain_q15 < 0) target_gain_q15 = 0;
    if (target_gain_q15 > 32768) target_gain_q15 = 32768;
  }

  /* Smooth gain changes to prevent audible pumping artifacts
   * Fast attack on gain reduction, slow release on gain increase */
  int32_t g = st->gain_q15;
  int32_t gd = target_gain_q15 - g;
  int32_t k_g = (gd < 0) ? CLEAN_COMP_GAIN_ATTACK_Q15 : CLEAN_COMP_GAIN_RELEASE_Q15;
  g += (int32_t)(((int64_t)k_g * (int64_t)gd) >> 15);
  if (g < 0) g = 0;
  if (g > 32768) g = 32768;
  st->gain_q15 = g;

  /* Apply gain to signal */
  *x_s24 = clamp_s24((int32_t)(((int64_t)(*x_s24) * (int64_t)g) >> 15));
#endif
}

void DspFilters_CompressStereo(int32_t *l, int32_t *r)
{
#if CLEAN_COMP_ENABLE
  /* Process each channel independently (dual-mono)
   * No stereo linking or cross-channel ducking */
  DspFilters_CompressOne(&s_comp_l, l);
  DspFilters_CompressOne(&s_comp_r, r);
#endif
}

void DspFilters_Limiter(int32_t *l, int32_t *r)
{
#if AUDIO_LIMITER_ENABLE
  /* Find peak amplitude across both channels */
  int32_t a = abs_s32(*l);
  int32_t b = abs_s32(*r);
  int32_t peak = (a > b) ? a : b;

  /* Calculate target gain to bring peak down to threshold
   * If peak exceeds threshold, reduce gain proportionally */
  int32_t target = 32768; /* Unity gain */
  if (peak > AUDIO_LIMITER_THRESH_S24)
  {
    /* Compute gain reduction: target = threshold / peak */
    target = (int32_t)(((int64_t)AUDIO_LIMITER_THRESH_S24 << 15) / (int64_t)peak);
    if (target < 0) target = 0;
    if (target > 32768) target = 32768;
  }

  /* Apply instant attack (no smoothing) for transient peaks
   * Use smooth release to avoid pumping artifacts */
  int32_t g = s_limiter.gain_q15;
  if (target < g)
  {
    g = target; /* Instant attack - immediately reduce gain */
  }
  else
  {
    /* Smooth release - gradually restore gain */
    g += (int32_t)(((int64_t)AUDIO_LIMITER_RELEASE_Q15 * (target - g)) >> 15);
    if (g > 32768) g = 32768;
  }
  s_limiter.gain_q15 = g;

  /* Apply gain reduction to both channels */
  *l = clamp_s24((int32_t)(((int64_t)(*l) * g) >> 15));
  *r = clamp_s24((int32_t)(((int64_t)(*r) * g) >> 15));
#endif
}

int32_t DspFilters_CabSim(BiquadState *st, int32_t x)
{
  /* Second-order IIR (biquad) lowpass filter
   * Emulates guitar cabinet frequency response by rolling off highs above ~5kHz
   * Difference equation:
   *   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
   * 
   * Coefficients are in Q28 format for precision with 24-bit audio */
  
  int64_t acc = 0;
  acc += (int64_t)CAB_B0_Q28 * (int64_t)x;
  acc += (int64_t)CAB_B1_Q28 * (int64_t)st->x1;
  acc += (int64_t)CAB_B2_Q28 * (int64_t)st->x2;
  acc -= (int64_t)CAB_A1_Q28 * (int64_t)st->y1;
  acc -= (int64_t)CAB_A2_Q28 * (int64_t)st->y2;

  int32_t y = (int32_t)(acc >> 28);
  y = clamp_s24(y);

  /* Update delay line */
  st->x2 = st->x1;
  st->x1 = x;
  st->y2 = st->y1;
  st->y1 = y;
  
  return y;
}

int32_t DspFilters_OnePole(int32_t x, int32_t *st, int32_t a_q15)
{
  /* Simple first-order IIR lowpass filter
   * y[n] = y[n-1] + a * (x[n] - y[n-1])
   * where a_q15 determines cutoff frequency:
   * - Small a (slow) = lower cutoff
   * - Large a (fast) = higher cutoff */
  
  int32_t y = *st;
  y += (int32_t)(((int64_t)a_q15 * (int64_t)(x - y)) >> 15);
  y = clamp_s24(y);
  *st = y;
  return y;
}

/* State Accessor Functions */
DcBlockState* DspFilters_GetDcL(void) { return &s_dc_l; }
DcBlockState* DspFilters_GetDcR(void) { return &s_dc_r; }
DcBlockState* DspFilters_GetCleanHpfL(void) { return &s_clean_hpf_l; }
DcBlockState* DspFilters_GetCleanHpfR(void) { return &s_clean_hpf_r; }
DcBlockState* DspFilters_GetWetHpfDelayL(void) { return &s_wet_hpf_delay_l; }
DcBlockState* DspFilters_GetWetHpfDelayR(void) { return &s_wet_hpf_delay_r; }
DcBlockState* DspFilters_GetWetHpfReverbL(void) { return &s_wet_hpf_reverb_l; }
DcBlockState* DspFilters_GetWetHpfReverbR(void) { return &s_wet_hpf_reverb_r; }
BiquadState* DspFilters_GetCabL(void) { return &s_cab_l; }
BiquadState* DspFilters_GetCabR(void) { return &s_cab_r; }
int32_t* DspFilters_GetWetLpfDelayL(void) { return &s_wet_lpf_delay_l; }
int32_t* DspFilters_GetWetLpfDelayR(void) { return &s_wet_lpf_delay_r; }
int32_t* DspFilters_GetWetLpfReverbL(void) { return &s_wet_lpf_reverb_l; }
int32_t* DspFilters_GetWetLpfReverbR(void) { return &s_wet_lpf_reverb_r; }
