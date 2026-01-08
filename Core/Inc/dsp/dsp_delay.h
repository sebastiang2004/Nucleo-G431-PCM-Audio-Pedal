#ifndef DSP_DELAY_H
#define DSP_DELAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DSP Delay Module
 * 
 * Implements echo/delay effect with:
 * - Adjustable feedback for multiple repeats
 * - Adjustable wet/dry mix
 * - Downsampling for longer delay time and warmer character
 * - Feedback lowpass filtering to reduce harshness
 * - Wet signal conditioning (HPF + LPF)
 * ============================================================================ */

/**
 * Delay State Structure
 * 
 * Maintains delay line position and filtering states
 */
typedef struct
{
  uint32_t idx;          /**< Current position in delay buffer */
  uint8_t phase;         /**< Downsampling phase counter (0..DECIM-1) */
  int32_t last_out_s24;  /**< Held output sample during downsampling */
  int32_t fb_lp_s24;     /**< Feedback lowpass filter state */
} DelayState;

/**
 * @brief Initialize delay effect
 * 
 * Clears delay buffers and resets all state variables.
 * Must be called before first use.
 */
void DspDelay_Init(void);

/**
 * @brief Set delay mix (wet/dry balance)
 * 
 * Controls how much delay signal is mixed with the dry signal.
 * 
 * @param mix_q15 Mix amount in Q15 format (0..32768)
 *                0 = fully dry (no delay)
 *                16384 = 50/50 mix
 *                32768 = fully wet (delay only)
 */
void DspDelay_SetMix(int32_t mix_q15);

/**
 * @brief Get current delay mix setting
 * @return Mix amount in Q15 format
 */
int32_t DspDelay_GetMix(void);

/**
 * @brief Set delay feedback amount
 * 
 * Controls how much of the delayed signal is fed back into the delay line.
 * Higher feedback creates more repeats.
 * 
 * @param feedback_q15 Feedback amount in Q15 format (0..32768)
 *                     0 = single echo
 *                     16384 = moderate repeats
 *                     32768 = infinite feedback (self-oscillation)
 */
void DspDelay_SetFeedback(int32_t feedback_q15);

/**
 * @brief Get current delay feedback setting
 * @return Feedback amount in Q15 format
 */
int32_t DspDelay_GetFeedback(void);

/**
 * @brief Process one channel through delay effect
 * 
 * Signal flow:
 * 1. Read from delay buffer (with zero-order hold downsampling)
 * 2. Apply feedback lowpass filter
 * 3. Mix input with filtered feedback
 * 4. Write to delay buffer
 * 
 * The downsampling creates longer delay time and naturally rolls off
 * highs, giving a warmer, less "digital" character.
 * 
 * @param x Input sample in S24 format (signed 24-bit in int32)
 * @param delay Pointer to delay buffer (array of int16_t)
 * @param st Pointer to delay state for this channel
 * @return Delayed output sample in S24 format
 */
int32_t DspDelay_Process(int32_t x, int16_t *delay, DelayState *st);

/**
 * @brief Get pointer to left channel delay buffer
 * @return Pointer to left delay buffer
 */
int16_t* DspDelay_GetBufferL(void);

/**
 * @brief Get pointer to right channel delay buffer
 * @return Pointer to right delay buffer
 */
int16_t* DspDelay_GetBufferR(void);

/**
 * @brief Get pointer to left channel delay state
 * @return Pointer to left channel state
 */
DelayState* DspDelay_GetStateL(void);

/**
 * @brief Get pointer to right channel delay state
 * @return Pointer to right channel state
 */
DelayState* DspDelay_GetStateR(void);

#ifdef __cplusplus
}
#endif

#endif /* DSP_DELAY_H */
