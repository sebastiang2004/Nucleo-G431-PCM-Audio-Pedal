#include "audio/app_audio.h"

#include "main.h"

#include <string.h>

#include "dsp/app_dsp.h"

/*
 * This file contains the "audio IO glue":
 * - I2S DMA buffers (RX from ADC, TX to DAC)
 * - RX callback: unpack -> AppDsp_ProcessFrame() -> push into ring
 * - TX callback: adaptive resample from ring -> pack into TX DMA buffer
 *
 * The resampler exists because I2S2 and I2S3 are independent masters, so tiny
 * clock mismatch is inevitable. Without this, you'd hear periodic glitches.
 */

/* Audio format:
 * - I2S left-justified (I2S_STANDARD_MSB)
 * - 24-bit sample in 32-bit slot
 * - HAL I2S uses a uint16_t DMA buffer; each 32-bit slot is 2 halfwords.
 */

#define AUDIO_CHANNELS                 2U
#define AUDIO_FRAMES_PER_HALF          64U
#define AUDIO_HALFWORDS_PER_SAMPLE32   2U
#define AUDIO_HALFWORDS_PER_FRAME      (AUDIO_CHANNELS * AUDIO_HALFWORDS_PER_SAMPLE32)
#define AUDIO_HALFWORDS_PER_HALF       (AUDIO_FRAMES_PER_HALF * AUDIO_HALFWORDS_PER_FRAME)
#define AUDIO_HALFWORDS_TOTAL          (2U * AUDIO_HALFWORDS_PER_HALF)

/* Size parameter for HAL_I2S_{Receive,Transmit}_DMA(): number of 24/32-bit data lengths */
#define I2S_DMA_SIZE_SAMPLE32_TOTAL    (AUDIO_HALFWORDS_TOTAL / AUDIO_HALFWORDS_PER_SAMPLE32)

static I2S_HandleTypeDef *s_rx_i2s = NULL;
static I2S_HandleTypeDef *s_tx_i2s = NULL;

static uint16_t s_i2s_rx_buf[AUDIO_HALFWORDS_TOTAL];
static uint16_t s_i2s_tx_buf[AUDIO_HALFWORDS_TOTAL];

/* Must be power-of-two for fast wrap. */
#define AUDIO_RING_FRAMES              256U
#define AUDIO_RING_MASK                (AUDIO_RING_FRAMES - 1U)

static int32_t s_ring_l[AUDIO_RING_FRAMES];
static int32_t s_ring_r[AUDIO_RING_FRAMES];
static volatile uint32_t s_ring_w = 0;       /* frame index */
static volatile uint32_t s_ring_r_q16 = 0;   /* Q16.16 frame index */
static volatile uint32_t s_ring_underrun = 0;
static volatile uint32_t s_ring_overflow = 0;

static volatile uint32_t s_audio_overrun_count = 0;
static volatile uint32_t s_audio_start_fail = 0;
static volatile uint32_t s_audio_runtime_fail = 0;
static volatile uint32_t s_audio_started = 0;
static volatile uint32_t s_audio_start_tx_status = 0;
static volatile uint32_t s_audio_start_rx_status = 0;

static inline uint32_t ring_fill_frames(uint32_t w, uint32_t r_int)
{
  return (w - r_int) & AUDIO_RING_MASK;
}

static inline int32_t clamp_s24(int32_t x)
{
  if (x > 8388607) return 8388607;
  if (x < -8388608) return -8388608;
  return x;
}

/* Extract signed 24-bit sample from left-justified 24-in-32 slot (two halfwords). */
static inline int32_t lj24in32_to_s24(const uint16_t *p)
{
  uint32_t w = ((uint32_t)p[0] << 16) | (uint32_t)p[1];
  return ((int32_t)w) >> 8;
}

/* Pack signed 24-bit sample into left-justified 24-in-32 slot (two halfwords). */
static inline void s24_to_lj24in32(uint16_t *p, int32_t s24)
{
  int32_t x = clamp_s24(s24);
  uint32_t w = (uint32_t)(x << 8);
  p[0] = (uint16_t)(w >> 16);
  p[1] = (uint16_t)(w & 0xFFFFU);
}

static inline void ring_push_frames_s24(int32_t l, int32_t r)
{
  /* IMPORTANT:
   * This function is called at audio rate (64 frames per half-buffer).
   * Disabling global IRQs here can starve UART RX interrupts and make COM
   * commands (FXMASK/PSET) feel slow or get corrupted.
   *
   * We therefore avoid global IRQ masking and instead rely on:
   * - 32-bit aligned writes being atomic on Cortex-M
   * - write samples first, then publish s_ring_w last
   */

  uint32_t w = s_ring_w;
  uint32_t r_int = s_ring_r_q16 >> 16;
  uint32_t fill = ring_fill_frames(w, r_int);

  /* Ensure at least 2 frames of headroom to allow interpolation at the read side. */
  if (fill >= (AUDIO_RING_FRAMES - 2U))
  {
    /* Best-effort drop: may race with TX callback but is safe. */
    s_ring_r_q16 += (1U << 16);
    s_ring_overflow++;
  }

  s_ring_l[w] = l;
  s_ring_r[w] = r;
  __DMB();
  s_ring_w = (w + 1U) & AUDIO_RING_MASK;
}

static void tx_fill_half(uint32_t half_index)
{
  uint32_t base = half_index ? AUDIO_HALFWORDS_PER_HALF : 0U;
  uint16_t *tx = &s_i2s_tx_buf[base];

  /* Target fill around half the ring. */
  const int32_t target = (int32_t)(AUDIO_RING_FRAMES / 2U);
  const int32_t step_base_q16 = (1 << 16);
  const int32_t step_limit = 128; /* +/-0.20% */

  static int32_t fill_err_filt = 0;

  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t w_snapshot = s_ring_w;
  uint32_t r_q16 = s_ring_r_q16;
  if (!primask)
  {
    __enable_irq();
  }

  uint32_t r_int = r_q16 >> 16;
  int32_t fill = (int32_t)ring_fill_frames(w_snapshot, r_int);

  int32_t err = (fill - target);
  fill_err_filt += (err - fill_err_filt) >> 4;

  int32_t step_q16 = step_base_q16 + fill_err_filt;
  if (step_q16 < (step_base_q16 - step_limit)) step_q16 = (step_base_q16 - step_limit);
  if (step_q16 > (step_base_q16 + step_limit)) step_q16 = (step_base_q16 + step_limit);

  for (uint32_t frame = 0; frame < AUDIO_FRAMES_PER_HALF; frame++)
  {
    uint32_t idx0 = (r_q16 >> 16) & AUDIO_RING_MASK;
    uint32_t idx1 = (idx0 + 1U) & AUDIO_RING_MASK;
    uint32_t frac = r_q16 & 0xFFFFU;

    /* Need at least 2 frames buffered for interpolation.
     * Use the initial write-pointer snapshot to avoid per-frame IRQ masking.
     * This is conservative (writer may advance after snapshot) and ensures we
     * never read beyond what was available when the fill started.
     */
    uint32_t have = ring_fill_frames(w_snapshot, (r_q16 >> 16));

    int32_t l_out = 0;
    int32_t r_out = 0;
    if (have >= 2U)
    {
      int32_t l0 = s_ring_l[idx0];
      int32_t l1 = s_ring_l[idx1];
      int32_t r0s = s_ring_r[idx0];
      int32_t r1s = s_ring_r[idx1];

      l_out = l0 + (int32_t)(((int64_t)(l1 - l0) * (int64_t)frac) >> 16);
      r_out = r0s + (int32_t)(((int64_t)(r1s - r0s) * (int64_t)frac) >> 16);
    }
    else
    {
      s_ring_underrun++;
    }

    uint32_t o = frame * AUDIO_HALFWORDS_PER_FRAME;
    s24_to_lj24in32(&tx[o + 0], l_out);
    s24_to_lj24in32(&tx[o + 2], r_out);

    r_q16 += (uint32_t)step_q16;
  }

  uint32_t primask3 = __get_PRIMASK();
  __disable_irq();
  s_ring_r_q16 = r_q16;
  if (!primask3)
  {
    __enable_irq();
  }
}

static void process_rx_half(uint32_t half_index)
{
  uint32_t base = half_index ? AUDIO_HALFWORDS_PER_HALF : 0U;
  const uint16_t *rx = &s_i2s_rx_buf[base];

  for (uint32_t frame = 0; frame < AUDIO_FRAMES_PER_HALF; frame++)
  {
    uint32_t o = frame * AUDIO_HALFWORDS_PER_FRAME;

    int32_t l = lj24in32_to_s24(&rx[o + 0]);
    int32_t r = lj24in32_to_s24(&rx[o + 2]);

    AppDsp_ProcessFrame(&l, &r);
    ring_push_frames_s24(l, r);
  }
}

void AppAudio_Init(I2S_HandleTypeDef *rx_i2s, I2S_HandleTypeDef *tx_i2s)
{
  s_rx_i2s = rx_i2s;
  s_tx_i2s = tx_i2s;
}

void AppAudio_Start(void)
{
  if ((s_rx_i2s == NULL) || (s_tx_i2s == NULL))
  {
    s_audio_start_fail = 1;
    s_audio_runtime_fail = 1;
    s_audio_started = 0;
    return;
  }

  memset(s_i2s_rx_buf, 0, sizeof(s_i2s_rx_buf));
  memset(s_i2s_tx_buf, 0, sizeof(s_i2s_tx_buf));

  s_ring_w = 0;
  s_ring_r_q16 = 0;
  s_ring_underrun = 0;
  s_ring_overflow = 0;
  s_audio_runtime_fail = 0;

  /* TX first so DAC sees continuous clocks/data; buffer is initially zeros. */
  s_audio_start_tx_status = (uint32_t)HAL_I2S_Transmit_DMA(s_tx_i2s, s_i2s_tx_buf, (uint16_t)I2S_DMA_SIZE_SAMPLE32_TOTAL);
  s_audio_start_rx_status = (uint32_t)HAL_I2S_Receive_DMA(s_rx_i2s, s_i2s_rx_buf, (uint16_t)I2S_DMA_SIZE_SAMPLE32_TOTAL);

  if ((s_audio_start_tx_status != (uint32_t)HAL_OK) || (s_audio_start_rx_status != (uint32_t)HAL_OK))
  {
    s_audio_start_fail = 1;
    s_audio_runtime_fail = 1;
    s_audio_started = 0;
    return;
  }

  s_audio_start_fail = 0;
  s_audio_started = 1;
}

uint8_t AppAudio_StartFailed(void)
{
  return (uint8_t)(s_audio_start_fail ? 1U : 0U);
}

uint8_t AppAudio_RuntimeFailed(void)
{
  return (uint8_t)(s_audio_runtime_fail ? 1U : 0U);
}

void AppAudio_OnRxHalfCplt(I2S_HandleTypeDef *hi2s)
{
  if (hi2s == s_rx_i2s)
  {
    process_rx_half(0U);
  }
}

void AppAudio_OnRxCplt(I2S_HandleTypeDef *hi2s)
{
  if (hi2s == s_rx_i2s)
  {
    process_rx_half(1U);
  }
}

void AppAudio_OnTxHalfCplt(I2S_HandleTypeDef *hi2s)
{
  if (hi2s == s_tx_i2s)
  {
    tx_fill_half(0U);
  }
}

void AppAudio_OnTxCplt(I2S_HandleTypeDef *hi2s)
{
  if (hi2s == s_tx_i2s)
  {
    tx_fill_half(1U);
  }
}

void AppAudio_OnError(I2S_HandleTypeDef *hi2s)
{
  (void)hi2s;
  s_audio_overrun_count++;
  s_audio_runtime_fail = 1;
}
