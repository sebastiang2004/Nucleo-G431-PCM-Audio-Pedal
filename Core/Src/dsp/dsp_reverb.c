#include "dsp/dsp_reverb.h"
#include <string.h>

/* ============================================================================
 * DSP Reverb Module Implementation
 * 
 * Algorithmic reverb based on feedback delay network with allpass diffusion.
 * Creates spacious ambience and room simulation.
 * ============================================================================ */

/* Configuration */
#define REVERB_DELAY_LEN               2048U
#define REVERB_DELAY_MASK              (REVERB_DELAY_LEN - 1U)
#define REVERB_AP_LEN                  128U
#define REVERB_AP1_LEN                 64U
#define REVERB_AP2_LEN                 64U
#define REVERB_AP1_MASK                (REVERB_AP1_LEN - 1U)
#define REVERB_AP2_MASK                (REVERB_AP2_LEN - 1U)

#define REVERB_FEEDBACK_Q15            22000   /* 0.67 feedback */
#define REVERB_DAMP_Q15                8192    /* Damping strength */
#define REVERB_AP_G_Q15                22938   /* 0.70 allpass gain */
#define REVERB_MIX_Q15                 9830    /* 0.30 wet mix */
#define REVERB_MOD_AMP_SAMPLES         0       /* Modulation disabled */

/* Static State */
static volatile int32_t s_reverb_mix_q15 = REVERB_MIX_Q15;
static volatile int32_t s_reverb_feedback_q15 = REVERB_FEEDBACK_Q15;
static volatile int32_t s_reverb_damp_q15 = REVERB_DAMP_Q15;

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

/* Helper Functions */

/**
 * @brief Clamp value to signed 24-bit range
 */
static inline int32_t clamp_s24(int32_t x)
{
  if (x > 8388607) return 8388607;
  if (x < -8388608) return -8388608;
  return x;
}

/**
 * @brief Generate triangle LFO with fractional offset
 * 
 * Creates smooth pitch modulation for chorus-like shimmer.
 * Returns offset in Q8 format (1/256 sample precision).
 */
static inline int32_t triangle_lfo_offset_q8(uint32_t *phase, uint32_t step, int32_t amp_samples)
{
  *phase += step;
  uint8_t t = (uint8_t)(*phase >> 24);
  uint8_t tri = (t < 128) ? t : (uint8_t)(255 - t);

  /* Map 0..127 triangle to signed offset [-amp..+amp] in Q8 */
  int32_t off = (int32_t)tri - 64;
  int32_t off_q8 = (off * (amp_samples * 256)) / 64;
  const int32_t lim = amp_samples * 256;
  if (off_q8 > lim) off_q8 = lim;
  if (off_q8 < -lim) off_q8 = -lim;
  return off_q8;
}

/**
 * @brief Process allpass filter section
 * 
 * Allpass filters preserve amplitude but change phase relationships,
 * creating dense diffusion that sounds like natural reflections.
 * 
 * Transfer function: H(z) = (-g + z^-N) / (1 - g*z^-N)
 */
static inline int32_t allpass_process_s24_len(int32_t x, int32_t *buf, uint32_t *idx, uint32_t mask)
{
  uint32_t i = *idx;
  int32_t b = buf[i];  /* Read delayed sample */

  /* Allpass calculation */
  int32_t y = b - (int32_t)(((int64_t)REVERB_AP_G_Q15 * (int64_t)x) >> 15);
  int32_t new_b = x + (int32_t)(((int64_t)REVERB_AP_G_Q15 * (int64_t)y) >> 15);
  buf[i] = clamp_s24(new_b);

  *idx = (i + 1U) & mask;  /* Advance circular buffer */
  return clamp_s24(y);
}

/* Public API Implementation */

void DspReverb_Init(void)
{
  /* Clear delay buffers */
  memset(s_reverb_delay_l, 0, sizeof(s_reverb_delay_l));
  memset(s_reverb_delay_r, 0, sizeof(s_reverb_delay_r));
  memset(s_reverb_ap_l, 0, sizeof(s_reverb_ap_l));
  memset(s_reverb_ap_r, 0, sizeof(s_reverb_ap_r));

  /* Reset indices */
  s_reverb_delay_idx_l = 0;
  s_reverb_delay_idx_r = 0;
  s_reverb_ap1_idx_l = 0;
  s_reverb_ap1_idx_r = 0;
  s_reverb_ap2_idx_l = 0;
  s_reverb_ap2_idx_r = 0;

  /* Reset filters and LFOs */
  s_reverb_lp_l = 0;
  s_reverb_lp_r = 0;
  s_reverb_lfo_l = 0;
  s_reverb_lfo_r = 0;
}

void DspReverb_SetMix(int32_t mix_q15)
{
  if (mix_q15 < 0) mix_q15 = 0;
  if (mix_q15 > 32768) mix_q15 = 32768;
  s_reverb_mix_q15 = mix_q15;
}

int32_t DspReverb_GetMix(void)
{
  return s_reverb_mix_q15;
}

void DspReverb_SetFeedback(int32_t feedback_q15)
{
  if (feedback_q15 < 0) feedback_q15 = 0;
  if (feedback_q15 > 32768) feedback_q15 = 32768;
  s_reverb_feedback_q15 = feedback_q15;
}

int32_t DspReverb_GetFeedback(void)
{
  return s_reverb_feedback_q15;
}

void DspReverb_SetDamp(int32_t damp_q15)
{
  if (damp_q15 < 0) damp_q15 = 0;
  if (damp_q15 > 32768) damp_q15 = 32768;
  s_reverb_damp_q15 = damp_q15;
}

int32_t DspReverb_GetDamp(void)
{
  return s_reverb_damp_q15;
}

int32_t DspReverb_Process(int32_t x,
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
  
  /* Optional modulation for shimmer (currently disabled) */
  int32_t mod_q8 = triangle_lfo_offset_q8(lfo_phase, lfo_step, REVERB_MOD_AMP_SAMPLES);
  int32_t mod_i = (mod_q8 >> 8);
  uint32_t frac = (uint32_t)(mod_q8 & 0xFF);

  /* Read from delay line with linear interpolation */
  int32_t ri0 = (int32_t)i + mod_i;
  ri0 = (ri0 + (int32_t)REVERB_DELAY_LEN) & (int32_t)REVERB_DELAY_MASK;
  int32_t ri1 = (ri0 + 1) & (int32_t)REVERB_DELAY_MASK;

  int32_t d0 = delay[(uint32_t)ri0];
  int32_t d1 = delay[(uint32_t)ri1];
  int32_t d = d0 + (int32_t)(((int64_t)(d1 - d0) * (int64_t)frac) >> 8);

  /* Damping filter: removes high frequencies to simulate absorption */
  int32_t lpv = *lp;
  lpv += (int32_t)(((int64_t)s_reverb_damp_q15 * (int64_t)(d - lpv)) >> 15);
  *lp = lpv;

  /* Feedback with decay control */
  int32_t fb = (int32_t)(((int64_t)s_reverb_feedback_q15 * (int64_t)lpv) >> 15);
  delay[i] = clamp_s24(x + fb);
  *delay_idx = (i + 1U) & REVERB_DELAY_MASK;

  /* Two-stage allpass diffusion for dense, natural sound */
  int32_t y = allpass_process_s24_len(d, &ap_buf[0], ap1_idx, REVERB_AP1_MASK);
  y = allpass_process_s24_len(y, &ap_buf[REVERB_AP1_LEN], ap2_idx, REVERB_AP2_MASK);
  
  return y;
}

/* State Accessor Functions */
int32_t* DspReverb_GetDelayL(void) { return s_reverb_delay_l; }
int32_t* DspReverb_GetDelayR(void) { return s_reverb_delay_r; }
int32_t* DspReverb_GetAllpassL(void) { return s_reverb_ap_l; }
int32_t* DspReverb_GetAllpassR(void) { return s_reverb_ap_r; }
uint32_t* DspReverb_GetDelayIdxL(void) { return &s_reverb_delay_idx_l; }
uint32_t* DspReverb_GetDelayIdxR(void) { return &s_reverb_delay_idx_r; }
uint32_t* DspReverb_GetAp1IdxL(void) { return &s_reverb_ap1_idx_l; }
uint32_t* DspReverb_GetAp1IdxR(void) { return &s_reverb_ap1_idx_r; }
uint32_t* DspReverb_GetAp2IdxL(void) { return &s_reverb_ap2_idx_l; }
uint32_t* DspReverb_GetAp2IdxR(void) { return &s_reverb_ap2_idx_r; }
int32_t* DspReverb_GetLpL(void) { return &s_reverb_lp_l; }
int32_t* DspReverb_GetLpR(void) { return &s_reverb_lp_r; }
uint32_t* DspReverb_GetLfoL(void) { return &s_reverb_lfo_l; }
uint32_t* DspReverb_GetLfoR(void) { return &s_reverb_lfo_r; }
