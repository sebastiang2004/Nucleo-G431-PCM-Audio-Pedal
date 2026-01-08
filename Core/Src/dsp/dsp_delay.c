#include "dsp/dsp_delay.h"
#include <string.h>

/* ============================================================================
 * DSP Delay Module Implementation
 * 
 * Digital delay/echo effect with character:
 * - Sample rate reduction for longer delay and warmth
 * - Feedback filtering to prevent harsh buildup
 * - 16-bit storage to conserve memory
 * ============================================================================ */

/* Configuration */
#define DELAY_LEN                      1024U    /**< Delay buffer length in samples */
#define DELAY_MASK                     (DELAY_LEN - 1U)  /**< Wraparound mask */
#define DELAY_FEEDBACK_Q15             16384    /**< Default feedback (0.50) */
#define DELAY_MIX_Q15                  11469    /**< Default mix (~0.35 wet) */
#define DELAY_DECIM                    8U       /**< Downsampling factor */
#define DELAY_FB_LPF_A_Q15             1024     /**< Feedback LPF coefficient */

/* Static State */
static volatile int32_t s_delay_mix_q15 = DELAY_MIX_Q15;
static volatile int32_t s_delay_feedback_q15 = DELAY_FEEDBACK_Q15;

static int16_t s_delay_buf_l[DELAY_LEN];
static int16_t s_delay_buf_r[DELAY_LEN];

static DelayState s_delay_l = {0, 0, 0, 0};
static DelayState s_delay_r = {0, 0, 0, 0};

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
 * @brief Convert S24 (int32) to S16 (int16) with saturation
 * 
 * Used when writing to delay buffer to save memory.
 * 
 * @param x Input sample in S24 format
 * @return Output sample in S16 format
 */
static inline int16_t s24_to_s16(int32_t x)
{
  x = clamp_s24(x);
  x >>= 8;  /* Convert 24-bit to 16-bit */
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return (int16_t)x;
}

/**
 * @brief Apply one-pole lowpass filter
 * 
 * Simple IIR filter: y[n] = y[n-1] + a * (x[n] - y[n-1])
 * Used to smooth feedback signal and remove harshness.
 * 
 * @param x Input sample in S24 format
 * @param st Pointer to filter state
 * @param a_q15 Filter coefficient in Q15 format
 * @return Filtered output in S24 format
 */
static inline int32_t onepole_lpf_s24(int32_t x, int32_t *st, int32_t a_q15)
{
  int32_t y = *st;
  y += (int32_t)(((int64_t)a_q15 * (int64_t)(x - y)) >> 15);
  y = clamp_s24(y);
  *st = y;
  return y;
}

/* Public API Implementation */

void DspDelay_Init(void)
{
  /* Clear delay buffers */
  memset(s_delay_buf_l, 0, sizeof(s_delay_buf_l));
  memset(s_delay_buf_r, 0, sizeof(s_delay_buf_r));

  /* Reset left channel state */
  s_delay_l.idx = 0;
  s_delay_l.phase = 0;
  s_delay_l.last_out_s24 = 0;
  s_delay_l.fb_lp_s24 = 0;

  /* Reset right channel state */
  s_delay_r.idx = 0;
  s_delay_r.phase = 0;
  s_delay_r.last_out_s24 = 0;
  s_delay_r.fb_lp_s24 = 0;
}

void DspDelay_SetMix(int32_t mix_q15)
{
  if (mix_q15 < 0) mix_q15 = 0;
  if (mix_q15 > 32768) mix_q15 = 32768;
  s_delay_mix_q15 = mix_q15;
}

int32_t DspDelay_GetMix(void)
{
  return s_delay_mix_q15;
}

void DspDelay_SetFeedback(int32_t feedback_q15)
{
  if (feedback_q15 < 0) feedback_q15 = 0;
  if (feedback_q15 > 32768) feedback_q15 = 32768;
  s_delay_feedback_q15 = feedback_q15;
}

int32_t DspDelay_GetFeedback(void)
{
  return s_delay_feedback_q15;
}

int32_t DspDelay_Process(int32_t x, int16_t *delay, DelayState *st)
{
  /* Downsampling technique: Update delay line only once every DECIM samples
   * This effectively reduces the sample rate, which:
   * 1. Increases delay time (delay length * DECIM)
   * 2. Creates natural high-frequency rolloff (warmer sound)
   * 3. Reduces CPU load (fewer writes to memory)
   * 
   * During other samples, we hold the last output value (zero-order hold).
   * This creates a "stepped" output that acts as additional filtering. */
  
  if (st->phase == 0)
  {
    /* Time to update delay line */
    uint32_t i = st->idx;
    
    /* Read delayed sample and convert to S24 */
    int32_t d = ((int32_t)delay[i]) << 8;

    /* Apply lowpass filter to feedback path
     * This prevents buildup of harsh high frequencies in the repeats
     * and creates a more natural, "analog tape" character */
    st->fb_lp_s24 = onepole_lpf_s24(d, &st->fb_lp_s24, DELAY_FB_LPF_A_Q15);
    
    /* Calculate feedback signal with user-adjustable amount */
    int32_t fb = (int32_t)(((int64_t)s_delay_feedback_q15 * (int64_t)st->fb_lp_s24) >> 15);

    /* Mix input with feedback and write to delay buffer
     * This creates the echo/repeat effect */
    delay[i] = s24_to_s16(clamp_s24(x + fb));
    
    /* Advance delay line position (circular buffer) */
    st->idx = (i + 1U) & DELAY_MASK;
    
    /* Store output for hold during other phases */
    st->last_out_s24 = d;
  }

  /* Advance downsampling phase counter */
  st->phase = (uint8_t)((st->phase + 1U) % (uint8_t)DELAY_DECIM);
  
  /* Return held output (zero-order hold interpolation) */
  return st->last_out_s24;
}

int16_t* DspDelay_GetBufferL(void)
{
  return s_delay_buf_l;
}

int16_t* DspDelay_GetBufferR(void)
{
  return s_delay_buf_r;
}

DelayState* DspDelay_GetStateL(void)
{
  return &s_delay_l;
}

DelayState* DspDelay_GetStateR(void)
{
  return &s_delay_r;
}
