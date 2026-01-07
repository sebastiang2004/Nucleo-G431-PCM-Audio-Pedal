#include "app_dsp.h"

#include <stdbool.h>
#include <string.h>

/*
 * This file contains the "audio DSP" part of your project:
 * - DC blocker + gain staging + optional coloration
 * - FX blocks (distortion / delay / reverb)
 * - makeup gain + fast limiter
 *
 * It is intentionally HAL-independent so it is easy to read and change.
 */

/* ------------------------- Configuration (easy knobs) --------------------- */

/* Digital gain staging (Q8): 256=1.0x, 512=2.0x, 1024=4.0x, 2048=8.0x */
#define AUDIO_INPUT_GAIN_Q8            1536
#define AUDIO_MAKEUP_GAIN_Q8           1000

#define AUDIO_LIMITER_ENABLE           1
#define AUDIO_LIMITER_THRESH_S24       6500000
#define AUDIO_LIMITER_RELEASE_Q15      16

#define INPUT_COLOR_ENABLE             1
#define INPUT_COLOR_DRIVE_Q8           384

/* Clean-tone conditioning (always-on): helps electric guitar cleans.
 * - HPF removes handling rumble and tightens low end.
 * - Gentle compressor evens dynamics and adds sustain.
 */
#define CLEAN_HPF_ENABLE               1
/* 1st-order HPF coefficient R in Q15.
 * For fs=48kHz and fc~=90Hz: R ~= exp(-2*pi*fc/fs) ~= 0.9883 -> 32384.
 */
#define CLEAN_HPF_R_Q15                32384

#define CLEAN_COMP_ENABLE              1
/* Threshold in signed 24-bit counts (0..8388607). */
#define CLEAN_COMP_THRESH_S24          3000000
/* Ratio: 2 means 2:1 (gentle). Must be >= 1. */
#define CLEAN_COMP_RATIO               2
/* Envelope follower coefficients in Q15 (bigger=faster). */
#define CLEAN_COMP_ENV_ATTACK_Q15      4096
#define CLEAN_COMP_ENV_RELEASE_Q15     256
/* Gain smoothing (bigger=faster). */
#define CLEAN_COMP_GAIN_ATTACK_Q15     8192
#define CLEAN_COMP_GAIN_RELEASE_Q15    512

/* Guitar cab-sim (post-distortion): simple 2nd-order lowpass to remove fizz.
 * Designed for fs=48 kHz, fc~=5 kHz, Butterworth-ish (Q~0.707).
 */
#define CABSIM_ENABLE                  1

/* Reverb (fixed-point): feedback delay + allpass diffuser. */
#define REVERB_DELAY_LEN               2048U
#define REVERB_DELAY_MASK              (REVERB_DELAY_LEN - 1U)
#define REVERB_AP_LEN                  128U
#define REVERB_AP_MASK                 (REVERB_AP_LEN - 1U)

/* Split allpass buffer into two stages for more diffusion (warmer/less metallic). */
#define REVERB_AP1_LEN                 64U
#define REVERB_AP2_LEN                 64U
#define REVERB_AP1_MASK                (REVERB_AP1_LEN - 1U)
#define REVERB_AP2_MASK                (REVERB_AP2_LEN - 1U)

#define REVERB_FEEDBACK_Q15            22000   /* ~0.67 (tighter/less runaway) */
#define REVERB_DAMP_Q15                8192    /* stronger damping (less harsh) */
#define REVERB_AP_G_Q15                22938   /* 0.70 */
#define REVERB_MIX_Q15                 9830    /* ~0.30 wet (solo reverb mode) */

/* When stacking FX (ALL mode), keep space subtle so it stays punchy. */
#define REVERB_MIX_ALL_Q15             4915    /* ~0.15 wet */

/* Delay */
#define DELAY_LEN                      1024U
#define DELAY_MASK                     (DELAY_LEN - 1U)
#define DELAY_FEEDBACK_Q15             16384   /* 0.50 */
#define DELAY_MIX_Q15                  11469   /* ~0.35 wet (solo delay mode) */
#define DELAY_MIX_ALL_Q15              6554    /* legacy default; now scaled from delay_mix_q15 */

/* Make the delay feel less "robotic": operate the delay line at a lower
 * effective sample rate (zero-order hold), which increases delay time and
 * naturally rolls off highs.
 */
#define DELAY_DECIM                    8U

/* Gentle wet low-pass to remove harsh/metallic highs. */
#define WET_LPF_A_Q15                  2048    /* ~0.062 */

/* Delay feedback high-cut (bigger=faster/less dark). */
#define DELAY_FB_LPF_A_Q15             1024    /* ~0.031 */

/* Reverb modulation: small tap wobble reduces metallic ringing. */
#define REVERB_MOD_STEP_L              65536U  /* kept for experimentation */
#define REVERB_MOD_STEP_R              77824U  /* kept for experimentation */
/* Set to 0 to disable modulation (removes audible "wummmm" wobble). */
#define REVERB_MOD_AMP_SAMPLES         0

/* Wet-return conditioning: high-pass wet paths so lows stay tight and feedback
 * doesn't turn into a boomy wash.
 */
#define WET_HPF_ENABLE                 1
/* For fs=48kHz and fc~=180Hz: R ~= exp(-2*pi*fc/fs) ~= 0.9767 -> ~32004 */
#define WET_HPF_R_Q15                  32004

/* ------------------------------- Internals -------------------------------- */

static volatile AppFxMode s_mode = APP_FX_MODE_BYPASS;
static volatile uint32_t s_button_last_ms = 0;

static volatile AppFxMask s_fx_mask = 0;

/* Runtime parameters (defaults match previous compile-time constants). */
static volatile int32_t s_dist_drive_q8 = 40960; /* was const in distortion_process_s24 */
static volatile int32_t s_delay_mix_q15 = DELAY_MIX_Q15;
static volatile int32_t s_delay_mix_all_q15 = DELAY_MIX_ALL_Q15;
static volatile int32_t s_delay_feedback_q15 = DELAY_FEEDBACK_Q15;
static volatile int32_t s_reverb_mix_q15 = REVERB_MIX_Q15;
static volatile int32_t s_reverb_mix_all_q15 = REVERB_MIX_ALL_Q15;
static volatile int32_t s_reverb_feedback_q15 = REVERB_FEEDBACK_Q15;
static volatile int32_t s_reverb_damp_q15 = REVERB_DAMP_Q15;

/* Master output volume (Q15): 0=mute, 32768=unity. */
static volatile int32_t s_gain_q15 = 32768;

static inline int32_t clamp_s24(int32_t x)
{
  if (x > 8388607) return 8388607;
  if (x < -8388608) return -8388608;
  return x;
}

static inline int32_t gain_s32_q8(int32_t x, int32_t gain_q8)
{
  return (int32_t)(((int64_t)x * (int64_t)gain_q8) >> 8);
}

static inline int32_t q15_mul(int32_t a, int32_t b)
{
  return (int32_t)(((int64_t)a * b) >> 15);
}

static inline int32_t softclip_q15(int32_t xq15)
{
  /* Cubic soft clip: y = x - x^3/3 */
  int32_t x2 = q15_mul(xq15, xq15);
  int32_t x3 = q15_mul(x2, xq15);
  return xq15 - (x3 / 3);
}

static inline int32_t input_color_process_s24(int32_t x)
{
#if INPUT_COLOR_ENABLE
  int32_t y = gain_s32_q8(x, INPUT_COLOR_DRIVE_Q8);
  y = clamp_s24(y);
  int32_t yq15 = y >> 8;
  yq15 = softclip_q15(yq15);
  return clamp_s24(yq15 << 8);
#else
  return x;
#endif
}

typedef struct
{
  int32_t gain_q15;
} LimiterState;

static LimiterState s_limiter = {32768};

static inline int32_t abs_s32(int32_t x)
{
  return (x >= 0) ? x : -x;
}

static inline void limiter_process_s24(int32_t *l, int32_t *r)
{
#if AUDIO_LIMITER_ENABLE
  int32_t a = abs_s32(*l);
  int32_t b = abs_s32(*r);
  int32_t peak = (a > b) ? a : b;

  int32_t target = 32768;
  if (peak > AUDIO_LIMITER_THRESH_S24)
  {
    target = (int32_t)(((int64_t)AUDIO_LIMITER_THRESH_S24 << 15) / (int64_t)peak);
    if (target < 0) target = 0;
    if (target > 32768) target = 32768;
  }

  int32_t g = s_limiter.gain_q15;
  if (target < g)
  {
    g = target; /* instant attack */
  }
  else
  {
    g += (int32_t)(((int64_t)AUDIO_LIMITER_RELEASE_Q15 * (target - g)) >> 15);
    if (g > 32768) g = 32768;
  }
  s_limiter.gain_q15 = g;

  *l = clamp_s24((int32_t)(((int64_t)(*l) * g) >> 15));
  *r = clamp_s24((int32_t)(((int64_t)(*r) * g) >> 15));
#endif
}

static inline int16_t s24_to_s16(int32_t x)
{
  x = clamp_s24(x);
  x >>= 8;
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return (int16_t)x;
}

static inline int32_t mix_s24(int32_t dry, int32_t wet, int32_t mix_q15)
{
  /* mix_q15: 0=dry, 32768=wet */
  int32_t a = 32768 - mix_q15;
  return (int32_t)((((int64_t)dry * a) + ((int64_t)wet * mix_q15)) >> 15);
}

typedef struct
{
  int32_t x1;
  int32_t y1;
} DcBlockState;

static DcBlockState s_dc_l = {0, 0};
static DcBlockState s_dc_r = {0, 0};

static inline int32_t dc_block_s24(DcBlockState *st, int32_t x)
{
  const int32_t r_q15 = 32684; /* ~0.997 at 48 kHz (~20 Hz corner) */
  int32_t y = x - st->x1 + (int32_t)(((int64_t)r_q15 * st->y1) >> 15);
  st->x1 = x;
  st->y1 = y;
  return clamp_s24(y);
}

static inline int32_t hpf1_s24(DcBlockState *st, int32_t x, int32_t r_q15)
{
  /* 1st order HPF: y[n] = x[n] - x[n-1] + R*y[n-1] */
  int32_t y = x - st->x1 + (int32_t)(((int64_t)r_q15 * st->y1) >> 15);
  st->x1 = x;
  st->y1 = y;
  return clamp_s24(y);
}

static DcBlockState s_clean_hpf_l = {0, 0};
static DcBlockState s_clean_hpf_r = {0, 0};

static DcBlockState s_wet_hpf_delay_l = {0, 0};
static DcBlockState s_wet_hpf_delay_r = {0, 0};
static DcBlockState s_wet_hpf_reverb_l = {0, 0};
static DcBlockState s_wet_hpf_reverb_r = {0, 0};

typedef struct
{
  int32_t env;
  int32_t gain_q15;
} CompState;

static CompState s_comp_l = {0, 32768};
static CompState s_comp_r = {0, 32768};

static inline void clean_comp_process_one_s24(CompState *st, int32_t *x_s24)
{
#if CLEAN_COMP_ENABLE
  int32_t x = abs_s32(*x_s24);

  /* Envelope follower. */
  int32_t env = st->env;
  int32_t diff = x - env;
  int32_t k_env = (diff > 0) ? CLEAN_COMP_ENV_ATTACK_Q15 : CLEAN_COMP_ENV_RELEASE_Q15;
  env += (int32_t)(((int64_t)k_env * (int64_t)diff) >> 15);
  if (env < 0) env = 0;
  st->env = env;

  /* Compute target gain.
   * If env <= threshold: unity.
   * Else: output_env = thresh + (env - thresh)/ratio.
   * gain = output_env / env.
   */
  int32_t target_gain_q15 = 32768;
  if (env > CLEAN_COMP_THRESH_S24)
  {
    int32_t over = env - CLEAN_COMP_THRESH_S24;
    int32_t out_env = CLEAN_COMP_THRESH_S24 + (over / CLEAN_COMP_RATIO);
    target_gain_q15 = (int32_t)(((int64_t)out_env << 15) / (int64_t)env);
    if (target_gain_q15 < 0) target_gain_q15 = 0;
    if (target_gain_q15 > 32768) target_gain_q15 = 32768;
  }

  /* Smooth gain changes to avoid pumping. */
  int32_t g = st->gain_q15;
  int32_t gd = target_gain_q15 - g;
  int32_t k_g = (gd < 0) ? CLEAN_COMP_GAIN_ATTACK_Q15 : CLEAN_COMP_GAIN_RELEASE_Q15;
  g += (int32_t)(((int64_t)k_g * (int64_t)gd) >> 15);
  if (g < 0) g = 0;
  if (g > 32768) g = 32768;
  st->gain_q15 = g;

  *x_s24 = clamp_s24((int32_t)(((int64_t)(*x_s24) * (int64_t)g) >> 15));
#endif
}

static inline void clean_comp_process_s24(int32_t *l, int32_t *r)
{
#if CLEAN_COMP_ENABLE
  /* Dual-mono: compress each input independently (no cross-ducking). */
  clean_comp_process_one_s24(&s_comp_l, l);
  clean_comp_process_one_s24(&s_comp_r, r);
#endif
}

typedef struct
{
  int32_t hp_x1;
  int32_t hp_y1;
  int32_t lp_y1;
  int32_t os_x1;
} DistState;

static DistState s_dist_l = {0, 0, 0, 0};
static DistState s_dist_r = {0, 0, 0, 0};

static int32_t s_reverb_delay_l[REVERB_DELAY_LEN];
static int32_t s_reverb_delay_r[REVERB_DELAY_LEN];
static int32_t s_reverb_ap_l[REVERB_AP_LEN];
static int32_t s_reverb_ap_r[REVERB_AP_LEN];
static uint32_t s_reverb_delay_idx_l = 0;
static uint32_t s_reverb_delay_idx_r = 0;
static uint32_t s_reverb_ap1_idx_l = 0;
static uint32_t s_reverb_ap1_idx_r = 0;
static uint32_t s_reverb_ap2_idx_l = 0;
static uint32_t s_reverb_ap2_idx_r = 0;
static int32_t s_reverb_lp_l = 0;
static int32_t s_reverb_lp_r = 0;
static uint32_t s_reverb_lfo_l = 0;
static uint32_t s_reverb_lfo_r = 0;

static int16_t s_delay_buf_l[DELAY_LEN];
static int16_t s_delay_buf_r[DELAY_LEN];

typedef struct
{
  uint32_t idx;
  uint8_t phase;
  int32_t last_out_s24;
  int32_t fb_lp_s24;
} DelayState;

static DelayState s_delay_l = {0, 0, 0, 0};
static DelayState s_delay_r = {0, 0, 0, 0};

static int32_t s_wet_lpf_delay_l = 0;
static int32_t s_wet_lpf_delay_r = 0;
static int32_t s_wet_lpf_reverb_l = 0;
static int32_t s_wet_lpf_reverb_r = 0;

static inline int32_t hard_tube_clip_s24(int32_t x)
{
  const int32_t thr_pos = 1200000;
  const int32_t thr_neg = 900000;

  if (x > thr_pos)
  {
    int32_t over = x - thr_pos;
    return thr_pos + (over >> 10);
  }
  if (x < -thr_neg)
  {
    int32_t over = x + thr_neg;
    return -thr_neg + (over >> 10);
  }
  return x;
}

static inline int32_t onepole_lpf_s24(int32_t x, int32_t *st, int32_t a_q15)
{
  int32_t y = *st;
  y += (int32_t)(((int64_t)a_q15 * (int64_t)(x - y)) >> 15);
  y = clamp_s24(y);
  *st = y;
  return y;
}

static inline int32_t allpass_process_s24_len(int32_t x, int32_t *buf, uint32_t *idx, uint32_t mask)
{
  uint32_t i = *idx;
  int32_t b = buf[i];

  int32_t y = b - (int32_t)(((int64_t)REVERB_AP_G_Q15 * (int64_t)x) >> 15);
  int32_t new_b = x + (int32_t)(((int64_t)REVERB_AP_G_Q15 * (int64_t)y) >> 15);
  buf[i] = clamp_s24(new_b);

  *idx = (i + 1U) & mask;
  return clamp_s24(y);
}

static inline int32_t triangle_lfo_offset_q8(uint32_t *phase, uint32_t step, int32_t amp_samples)
{
  *phase += step;
  uint8_t t = (uint8_t)(*phase >> 24);
  uint8_t tri = (t < 128) ? t : (uint8_t)(255 - t);

  /* Map tri 0..127 to signed [-amp..+amp] in Q8 (1/256 sample).
   * Using a fractional delay offset allows interpolation and removes the
   * stepping noise that sounds like a quiet "motor" or "prrrmmm".
   */
  int32_t off = (int32_t)tri - 64; /* -64..+63 */
  int32_t off_q8 = (off * (amp_samples * 256)) / 64;
  const int32_t lim = amp_samples * 256;
  if (off_q8 > lim) off_q8 = lim;
  if (off_q8 < -lim) off_q8 = -lim;
  return off_q8;
}

static inline int32_t reverb_process_s24(int32_t x,
                                        int32_t *delay,
                                        uint32_t *delay_idx,
                                        int32_t *lp,
                                        int32_t *ap_buf,
                                        uint32_t *ap1_idx,
                                        uint32_t *ap2_idx,
                                        uint32_t *lfo_phase,
                                        uint32_t lfo_step)
{
  uint32_t i = *delay_idx;
  int32_t mod_q8 = triangle_lfo_offset_q8(lfo_phase, lfo_step, REVERB_MOD_AMP_SAMPLES);
  int32_t mod_i = (mod_q8 >> 8);
  uint32_t frac = (uint32_t)(mod_q8 & 0xFF);

  /* Fractional tap read with linear interpolation to avoid stepping artifacts. */
  int32_t ri0 = (int32_t)i + mod_i;
  ri0 = (ri0 + (int32_t)REVERB_DELAY_LEN) & (int32_t)REVERB_DELAY_MASK;
  int32_t ri1 = (ri0 + 1) & (int32_t)REVERB_DELAY_MASK;

  int32_t d0 = delay[(uint32_t)ri0];
  int32_t d1 = delay[(uint32_t)ri1];
  int32_t d = d0 + (int32_t)(((int64_t)(d1 - d0) * (int64_t)frac) >> 8);

  int32_t lpv = *lp;
  lpv += (int32_t)(((int64_t)s_reverb_damp_q15 * (int64_t)(d - lpv)) >> 15);
  *lp = lpv;

  int32_t fb = (int32_t)(((int64_t)s_reverb_feedback_q15 * (int64_t)lpv) >> 15);
  delay[i] = clamp_s24(x + fb);
  *delay_idx = (i + 1U) & REVERB_DELAY_MASK;

  /* Two-stage diffusion. */
  int32_t y = allpass_process_s24_len(d, &ap_buf[0], ap1_idx, REVERB_AP1_MASK);
  y = allpass_process_s24_len(y, &ap_buf[REVERB_AP1_LEN], ap2_idx, REVERB_AP2_MASK);
  return y;
}

static inline int32_t delay_process_s24(int32_t x, int16_t *delay, DelayState *st)
{
  /* Update delay at a lower effective sample rate to increase delay time and
   * naturally roll off highs (warmer, less metallic).
   */
  if (st->phase == 0)
  {
    uint32_t i = st->idx;
    int32_t d = ((int32_t)delay[i]) << 8;

    /* Low-pass the feedback signal to avoid robotic high-frequency ringing. */
    st->fb_lp_s24 = onepole_lpf_s24(d, &st->fb_lp_s24, DELAY_FB_LPF_A_Q15);
    int32_t fb = (int32_t)(((int64_t)s_delay_feedback_q15 * (int64_t)st->fb_lp_s24) >> 15);

    delay[i] = s24_to_s16(clamp_s24(x + fb));
    st->idx = (i + 1U) & DELAY_MASK;
    st->last_out_s24 = d;
  }

  st->phase = (uint8_t)((st->phase + 1U) % (uint8_t)DELAY_DECIM);
  return st->last_out_s24;
}

static inline int32_t distortion_process_s24(DistState *st, int32_t x)
{
  const int32_t hp_r_q15 = 32113; /* ~150 Hz corner */
  int32_t hp_y = x - st->hp_x1 + (int32_t)(((int64_t)hp_r_q15 * st->hp_y1) >> 15);
  st->hp_x1 = x;
  st->hp_y1 = hp_y;

  int32_t d24 = (int32_t)(((int64_t)hp_y * s_dist_drive_q8) >> 8);

  int32_t d24_mid = (d24 + st->os_x1) >> 1;
  st->os_x1 = d24;

  int32_t y0 = hard_tube_clip_s24(clamp_s24(d24));
  int32_t y1 = hard_tube_clip_s24(clamp_s24(d24_mid));
  int32_t y24 = (y0 + y1) >> 1;

  const int32_t lp_a_q15 = 12000;
  st->lp_y1 += (int32_t)(((int64_t)lp_a_q15 * (y24 - st->lp_y1)) >> 15);
  y24 = st->lp_y1;

  const int32_t level_q8 = 256;
  y24 = (int32_t)(((int64_t)y24 * level_q8) >> 8);
  return clamp_s24(y24);
}

typedef struct
{
  int32_t x1;
  int32_t x2;
  int32_t y1;
  int32_t y2;
} BiquadState;

static BiquadState s_cab_l = {0, 0, 0, 0};
static BiquadState s_cab_r = {0, 0, 0, 0};

/* Q28 biquad coefficients for lowpass @ ~5kHz, fs=48kHz.
 * Difference equation:
 * y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
 */
#define CAB_B0_Q28   ((int32_t)19407624)   /* ~0.0723 */
#define CAB_B1_Q28   ((int32_t)38815248)   /* ~0.1446 */
#define CAB_B2_Q28   ((int32_t)19407624)   /* ~0.0723 */
#define CAB_A1_Q28   ((int32_t)-297323915) /* ~-1.108 */
#define CAB_A2_Q28   ((int32_t)106689359)  /* ~0.3976 */

static inline int32_t cab_lpf_process_s24(BiquadState *st, int32_t x)
{
  int64_t acc = 0;
  acc += (int64_t)CAB_B0_Q28 * (int64_t)x;
  acc += (int64_t)CAB_B1_Q28 * (int64_t)st->x1;
  acc += (int64_t)CAB_B2_Q28 * (int64_t)st->x2;
  acc -= (int64_t)CAB_A1_Q28 * (int64_t)st->y1;
  acc -= (int64_t)CAB_A2_Q28 * (int64_t)st->y2;

  int32_t y = (int32_t)(acc >> 28);
  y = clamp_s24(y);

  st->x2 = st->x1;
  st->x1 = x;
  st->y2 = st->y1;
  st->y1 = y;
  return y;
}

/* ------------------------------- Public API ------------------------------- */

void AppDsp_Init(void)
{
  s_mode = APP_FX_MODE_BYPASS;
  s_button_last_ms = 0;
  s_fx_mask = 0;

  s_dist_l.hp_x1 = s_dist_l.hp_y1 = s_dist_l.lp_y1 = s_dist_l.os_x1 = 0;
  s_dist_r.hp_x1 = s_dist_r.hp_y1 = s_dist_r.lp_y1 = s_dist_r.os_x1 = 0;

  memset(s_reverb_delay_l, 0, sizeof(s_reverb_delay_l));
  memset(s_reverb_delay_r, 0, sizeof(s_reverb_delay_r));
  memset(s_reverb_ap_l, 0, sizeof(s_reverb_ap_l));
  memset(s_reverb_ap_r, 0, sizeof(s_reverb_ap_r));
  s_reverb_delay_idx_l = 0;
  s_reverb_delay_idx_r = 0;
  s_reverb_ap1_idx_l = 0;
  s_reverb_ap1_idx_r = 0;
  s_reverb_ap2_idx_l = 0;
  s_reverb_ap2_idx_r = 0;
  s_reverb_lp_l = 0;
  s_reverb_lp_r = 0;
  s_reverb_lfo_l = 0;
  s_reverb_lfo_r = 0;

  memset(s_delay_buf_l, 0, sizeof(s_delay_buf_l));
  memset(s_delay_buf_r, 0, sizeof(s_delay_buf_r));
  s_delay_l.idx = 0;
  s_delay_l.phase = 0;
  s_delay_l.last_out_s24 = 0;
  s_delay_l.fb_lp_s24 = 0;
  s_delay_r.idx = 0;
  s_delay_r.phase = 0;
  s_delay_r.last_out_s24 = 0;
  s_delay_r.fb_lp_s24 = 0;

  s_wet_lpf_delay_l = 0;
  s_wet_lpf_delay_r = 0;
  s_wet_lpf_reverb_l = 0;
  s_wet_lpf_reverb_r = 0;

  s_limiter.gain_q15 = 32768;

  s_dc_l.x1 = s_dc_l.y1 = 0;
  s_dc_r.x1 = s_dc_r.y1 = 0;

  s_clean_hpf_l.x1 = s_clean_hpf_l.y1 = 0;
  s_clean_hpf_r.x1 = s_clean_hpf_r.y1 = 0;

  s_wet_hpf_delay_l.x1 = s_wet_hpf_delay_l.y1 = 0;
  s_wet_hpf_delay_r.x1 = s_wet_hpf_delay_r.y1 = 0;
  s_wet_hpf_reverb_l.x1 = s_wet_hpf_reverb_l.y1 = 0;
  s_wet_hpf_reverb_r.x1 = s_wet_hpf_reverb_r.y1 = 0;

  s_comp_l.env = 0;
  s_comp_l.gain_q15 = 32768;
  s_comp_r.env = 0;
  s_comp_r.gain_q15 = 32768;

  s_cab_l.x1 = s_cab_l.x2 = s_cab_l.y1 = s_cab_l.y2 = 0;
  s_cab_r.x1 = s_cab_r.x2 = s_cab_r.y1 = s_cab_r.y2 = 0;
}

void AppDsp_OnButtonPress(uint32_t now_ms)
{
  /* Debounce: ignore edges within 300 ms. */
  if ((now_ms - s_button_last_ms) < 300U)
  {
    return;
  }
  s_button_last_ms = now_ms;

  switch (s_mode)
  {
    case APP_FX_MODE_BYPASS:
      s_mode = APP_FX_MODE_DISTORTION;
      break;
    case APP_FX_MODE_DISTORTION:
      s_mode = APP_FX_MODE_REVERB;
      break;
    case APP_FX_MODE_REVERB:
      s_mode = APP_FX_MODE_DELAY;
      break;
    case APP_FX_MODE_DELAY:
      s_mode = APP_FX_MODE_ALL;
      break;
    default:
      s_mode = APP_FX_MODE_BYPASS;
      break;
  }

  /* Keep mask consistent with legacy mode cycling. */
  if (s_mode == APP_FX_MODE_BYPASS) s_fx_mask = 0;
  else if (s_mode == APP_FX_MODE_DISTORTION) s_fx_mask = APP_FX_BIT_DISTORTION;
  else if (s_mode == APP_FX_MODE_REVERB) s_fx_mask = APP_FX_BIT_REVERB;
  else if (s_mode == APP_FX_MODE_DELAY) s_fx_mask = APP_FX_BIT_DELAY;
  else if (s_mode == APP_FX_MODE_ALL) s_fx_mask = (APP_FX_BIT_DISTORTION | APP_FX_BIT_REVERB | APP_FX_BIT_DELAY);
}

AppFxMode AppDsp_GetMode(void)
{
  return (AppFxMode)s_mode;
}

void AppDsp_SetFxMask(AppFxMask mask)
{
  mask &= (AppFxMask)(APP_FX_BIT_DISTORTION | APP_FX_BIT_REVERB | APP_FX_BIT_DELAY);
  s_fx_mask = mask;

  /* Update legacy mode for existing status/LED timing logic. */
  if (mask == 0)
  {
    s_mode = APP_FX_MODE_BYPASS;
  }
  else if (mask == APP_FX_BIT_DISTORTION)
  {
    s_mode = APP_FX_MODE_DISTORTION;
  }
  else if (mask == APP_FX_BIT_REVERB)
  {
    s_mode = APP_FX_MODE_REVERB;
  }
  else if (mask == APP_FX_BIT_DELAY)
  {
    s_mode = APP_FX_MODE_DELAY;
  }
  else
  {
    s_mode = APP_FX_MODE_ALL;
  }
}

AppFxMask AppDsp_GetFxMask(void)
{
  return (AppFxMask)s_fx_mask;
}

static inline int32_t clamp_q15(int32_t x)
{
  if (x < 0) return 0;
  if (x > 32768) return 32768;
  return x;
}

static inline int32_t clamp_gain_q15(int32_t x)
{
  /* Allow master gain > 1.0x for extra output volume.
   * 32768 = 1.0x, 65536 = 2.0x.
   */
  if (x < 0) return 0;
  if (x > 65536) return 65536;
  return x;
}

void AppDsp_SetParam(AppDspParamId id, int32_t value)
{
  switch (id)
  {
    case APP_DSP_PARAM_DIST_DRIVE_Q8:
      if (value < 0) value = 0;
      if (value > 131072) value = 131072;
      s_dist_drive_q8 = value;
      break;
    case APP_DSP_PARAM_DELAY_MIX_Q15:
      s_delay_mix_q15 = clamp_q15(value);
      break;
    case APP_DSP_PARAM_DELAY_FEEDBACK_Q15:
      s_delay_feedback_q15 = clamp_q15(value);
      break;
    case APP_DSP_PARAM_REVERB_MIX_Q15:
      s_reverb_mix_q15 = clamp_q15(value);
      break;
    case APP_DSP_PARAM_REVERB_FEEDBACK_Q15:
      s_reverb_feedback_q15 = clamp_q15(value);
      break;
    case APP_DSP_PARAM_REVERB_DAMP_Q15:
      s_reverb_damp_q15 = clamp_q15(value);
      break;
    case APP_DSP_PARAM_GAIN_Q15:
      s_gain_q15 = clamp_gain_q15(value);
      break;
    default:
      break;
  }
}

int32_t AppDsp_GetParam(AppDspParamId id)
{
  switch (id)
  {
    case APP_DSP_PARAM_DIST_DRIVE_Q8:
      return s_dist_drive_q8;
    case APP_DSP_PARAM_DELAY_MIX_Q15:
      return s_delay_mix_q15;
    case APP_DSP_PARAM_DELAY_FEEDBACK_Q15:
      return s_delay_feedback_q15;
    case APP_DSP_PARAM_REVERB_MIX_Q15:
      return s_reverb_mix_q15;
    case APP_DSP_PARAM_REVERB_FEEDBACK_Q15:
      return s_reverb_feedback_q15;
    case APP_DSP_PARAM_REVERB_DAMP_Q15:
      return s_reverb_damp_q15;
    case APP_DSP_PARAM_GAIN_Q15:
      return s_gain_q15;
    default:
      return 0;
  }
}

void AppDsp_ProcessFrame(int32_t *l_s24, int32_t *r_s24)
{
  int32_t l = *l_s24;
  int32_t r = *r_s24;

  AppFxMask mask = (AppFxMask)s_fx_mask;
  bool has_dist = (mask & APP_FX_BIT_DISTORTION) != 0;
  bool has_rev  = (mask & APP_FX_BIT_REVERB) != 0;
  bool has_del  = (mask & APP_FX_BIT_DELAY) != 0;
  uint32_t fx_count = (has_dist ? 1u : 0u) + (has_rev ? 1u : 0u) + (has_del ? 1u : 0u);

  /* Remove DC/subsonic before any gain. */
  l = dc_block_s24(&s_dc_l, l);
  r = dc_block_s24(&s_dc_r, r);

#if CLEAN_HPF_ENABLE
  /* Clean rumble removal (tightens low end for guitar cleans). */
  l = hpf1_s24(&s_clean_hpf_l, l, CLEAN_HPF_R_Q15);
  r = hpf1_s24(&s_clean_hpf_r, r, CLEAN_HPF_R_Q15);
#endif

  /* Gain staging: lift instrument level first. */
  l = gain_s32_q8(l, AUDIO_INPUT_GAIN_Q8);
  r = gain_s32_q8(r, AUDIO_INPUT_GAIN_Q8);

  /* Gentle dual-mono compressor for smoother clean dynamics. */
  clean_comp_process_s24(&l, &r);

  /* Subtle always-on coloration. */
  l = input_color_process_s24(l);
  r = input_color_process_s24(r);

  /* FX chain order: Distortion -> Delay -> Reverb.
   * This keeps cab-sim right after distortion and keeps space FX last.
   */
  if (has_dist)
  {
    l = distortion_process_s24(&s_dist_l, clamp_s24(l));
    r = distortion_process_s24(&s_dist_r, clamp_s24(r));

#if CABSIM_ENABLE
    l = cab_lpf_process_s24(&s_cab_l, l);
    r = cab_lpf_process_s24(&s_cab_r, r);
#endif
  }

  if (has_del)
  {
    int32_t wl = delay_process_s24(clamp_s24(l), s_delay_buf_l, &s_delay_l);
    int32_t wr = delay_process_s24(clamp_s24(r), s_delay_buf_r, &s_delay_r);
#if WET_HPF_ENABLE
    wl = hpf1_s24(&s_wet_hpf_delay_l, wl, WET_HPF_R_Q15);
    wr = hpf1_s24(&s_wet_hpf_delay_r, wr, WET_HPF_R_Q15);
#endif
    wl = onepole_lpf_s24(wl, &s_wet_lpf_delay_l, WET_LPF_A_Q15);
    wr = onepole_lpf_s24(wr, &s_wet_lpf_delay_r, WET_LPF_A_Q15);

    /* Let the UI knob actually control delay depth even when multiple FX are
     * enabled. Keep a small attenuation in stacked mode so it doesn't swamp.
     */
    int32_t mix = s_delay_mix_q15;
    if (fx_count > 1u)
    {
      mix = (mix * 3) / 4; /* 0.75x */
    }
    l = mix_s24(clamp_s24(l), wl, mix);
    r = mix_s24(clamp_s24(r), wr, mix);
  }

  if (has_rev)
  {
    int32_t wl = reverb_process_s24(clamp_s24(l), s_reverb_delay_l, &s_reverb_delay_idx_l, &s_reverb_lp_l, s_reverb_ap_l, &s_reverb_ap1_idx_l, &s_reverb_ap2_idx_l, &s_reverb_lfo_l, REVERB_MOD_STEP_L);
    int32_t wr = reverb_process_s24(clamp_s24(r), s_reverb_delay_r, &s_reverb_delay_idx_r, &s_reverb_lp_r, s_reverb_ap_r, &s_reverb_ap1_idx_r, &s_reverb_ap2_idx_r, &s_reverb_lfo_r, REVERB_MOD_STEP_R);
#if WET_HPF_ENABLE
    wl = hpf1_s24(&s_wet_hpf_reverb_l, wl, WET_HPF_R_Q15);
    wr = hpf1_s24(&s_wet_hpf_reverb_r, wr, WET_HPF_R_Q15);
#endif
    wl = onepole_lpf_s24(wl, &s_wet_lpf_reverb_l, WET_LPF_A_Q15);
    wr = onepole_lpf_s24(wr, &s_wet_lpf_reverb_r, WET_LPF_A_Q15);
    int32_t mix = (fx_count > 1u) ? s_reverb_mix_all_q15 : s_reverb_mix_q15;
    l = mix_s24(clamp_s24(l), wl, mix);
    r = mix_s24(clamp_s24(r), wr, mix);
  }

  /* Makeup gain for overall loudness. */
  int32_t makeup_q8 = AUDIO_MAKEUP_GAIN_Q8;
  if (mask != 0)
  {
    makeup_q8 = (makeup_q8 * 3) / 4;
  }
  l = gain_s32_q8(l, makeup_q8);
  r = gain_s32_q8(r, makeup_q8);

  /* Master volume control (unity by default). */
  l = clamp_s24((int32_t)(((int64_t)l * (int64_t)s_gain_q15) >> 15));
  r = clamp_s24((int32_t)(((int64_t)r * (int64_t)s_gain_q15) >> 15));

  /* Final protection against transient overload. */
  limiter_process_s24(&l, &r);

  *l_s24 = clamp_s24(l);
  *r_s24 = clamp_s24(r);
}
