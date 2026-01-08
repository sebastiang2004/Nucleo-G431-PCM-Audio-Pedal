#include "dsp/dsp_distortion.h"

/* ============================================================================
 * DSP Distortion Module Implementation
 * 
 * Guitar amp-style distortion with tube-like characteristics:
 * - Asymmetric clipping mimics vacuum tube behavior
 * - Oversampling reduces aliasing (digital artifacts)
 * - Post-filtering adds warmth and removes harshness
 * ============================================================================ */

/* Static State */
static volatile int32_t s_dist_drive_q8 = 40960; /**< Default drive: 160x (heavy distortion) */

static DistState s_dist_l = {0, 0, 0, 0};
static DistState s_dist_r = {0, 0, 0, 0};

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
 * @brief Asymmetric hard clipping (tube-style)
 * 
 * Emulates vacuum tube distortion characteristics:
 * - Asymmetric thresholds (softer on negative peaks)
 * - Hard clipping reduces headroom above threshold
 * - Creates rich harmonics typical of tube amps
 * 
 * @param x Input sample in S24 format
 * @return Clipped output sample in S24 format
 */
static inline int32_t hard_tube_clip_s24(int32_t x)
{
  const int32_t thr_pos = 1200000;  /**< Positive clipping threshold */
  const int32_t thr_neg = 900000;   /**< Negative clipping threshold (asymmetric) */

  if (x > thr_pos)
  {
    /* Gentle compression above positive threshold */
    int32_t over = x - thr_pos;
    return thr_pos + (over >> 10);  /* Divide overshoot by 1024 */
  }
  if (x < -thr_neg)
  {
    /* Gentle compression below negative threshold */
    int32_t over = x + thr_neg;
    return -thr_neg + (over >> 10);
  }
  
  return x; /* Linear region (no clipping) */
}

/* Public API Implementation */

void DspDistortion_Init(void)
{
  /* Reset left channel state */
  s_dist_l.hp_x1 = 0;
  s_dist_l.hp_y1 = 0;
  s_dist_l.lp_y1 = 0;
  s_dist_l.os_x1 = 0;

  /* Reset right channel state */
  s_dist_r.hp_x1 = 0;
  s_dist_r.hp_y1 = 0;
  s_dist_r.lp_y1 = 0;
  s_dist_r.os_x1 = 0;
}

void DspDistortion_SetDrive(int32_t drive_q8)
{
  /* Clamp to reasonable range: 1x to 512x */
  if (drive_q8 < 0) drive_q8 = 0;
  if (drive_q8 > 131072) drive_q8 = 131072;
  s_dist_drive_q8 = drive_q8;
}

int32_t DspDistortion_GetDrive(void)
{
  return s_dist_drive_q8;
}

int32_t DspDistortion_Process(DistState *st, int32_t x)
{
  /* Step 1: Highpass filter to remove DC and subsonic content
   * Prevents "flubby" low-end and tightens distortion character
   * Corner frequency ~150Hz @ 48kHz sampling rate */
  const int32_t hp_r_q15 = 32113; /* R coefficient in Q15 format */
  
  int32_t hp_y = x - st->hp_x1 + (int32_t)(((int64_t)hp_r_q15 * st->hp_y1) >> 15);
  st->hp_x1 = x;
  st->hp_y1 = hp_y;

  /* Step 2: Apply drive gain
   * Multiplies signal amplitude before clipping to control distortion amount */
  int32_t d24 = (int32_t)(((int64_t)hp_y * s_dist_drive_q8) >> 8);

  /* Step 3: 2x oversampling to reduce aliasing
   * Interpolates between current and previous sample, effectively
   * doubling the sample rate for the clipping operation */
  int32_t d24_mid = (d24 + st->os_x1) >> 1;  /* Linear interpolation */
  st->os_x1 = d24;

  /* Step 4: Apply hard clipping at 2x rate
   * Process both original and interpolated sample */
  int32_t y0 = hard_tube_clip_s24(clamp_s24(d24));
  int32_t y1 = hard_tube_clip_s24(clamp_s24(d24_mid));
  
  /* Downsample back to original rate by averaging */
  int32_t y24 = (y0 + y1) >> 1;

  /* Step 5: Lowpass filter for warmth
   * Removes harsh high-frequency artifacts created by clipping
   * Creates smoother, more "analog" character */
  const int32_t lp_a_q15 = 12000; /* Filter coefficient (moderate cutoff) */
  st->lp_y1 += (int32_t)(((int64_t)lp_a_q15 * (y24 - st->lp_y1)) >> 15);
  y24 = st->lp_y1;

  /* Step 6: Output level adjustment
   * Compensate for gain added by clipping to maintain consistent volume */
  const int32_t level_q8 = 256; /* Unity gain (no adjustment currently) */
  y24 = (int32_t)(((int64_t)y24 * level_q8) >> 8);
  
  return clamp_s24(y24);
}

DistState* DspDistortion_GetStateL(void)
{
  return &s_dist_l;
}

DistState* DspDistortion_GetStateR(void)
{
  return &s_dist_r;
}
