#ifndef DSP_DISTORTION_H
#define DSP_DISTORTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DSP Distortion Module
 * 
 * Implements guitar amplifier-style distortion effect with:
 * - Highpass filtering to remove subsonic content before clipping
 * - Adjustable drive (gain) control
 * - Tube-style asymmetric hard clipping
 * - 2x oversampling to reduce aliasing artifacts
 * - Post-distortion lowpass filtering for warmth
 * ============================================================================ */

/**
 * Distortion State Structure
 * 
 * Maintains filter states for highpass, lowpass, and oversampling
 */
typedef struct
{
  int32_t hp_x1;   /**< Highpass filter: previous input */
  int32_t hp_y1;   /**< Highpass filter: previous output */
  int32_t lp_y1;   /**< Lowpass filter: previous output */
  int32_t os_x1;   /**< Oversampling: previous input sample */
} DistState;

/**
 * @brief Initialize distortion effect
 * 
 * Resets all filter states and clears internal buffers.
 * Must be called before first use.
 */
void DspDistortion_Init(void);

/**
 * @brief Set distortion drive amount
 * 
 * Controls how much the signal is amplified before clipping.
 * Higher drive values produce more aggressive distortion.
 * 
 * @param drive_q8 Drive amount in Q8 fixed-point format
 *                  (256 = 1.0x, 512 = 2.0x, etc.)
 *                  Typical range: 256..131072 (1x to 512x)
 */
void DspDistortion_SetDrive(int32_t drive_q8);

/**
 * @brief Get current distortion drive setting
 * 
 * @return Current drive amount in Q8 format
 */
int32_t DspDistortion_GetDrive(void);

/**
 * @brief Process one channel of audio through distortion
 * 
 * Signal chain:
 * 1. Highpass filter (~150Hz) removes DC and rumble
 * 2. Apply drive gain
 * 3. 2x oversampling interpolation
 * 4. Asymmetric hard clipping (tube-style)
 * 5. Lowpass filter for warmth
 * 6. Level adjustment
 * 
 * @param st Pointer to distortion state for this channel
 * @param x Input sample in S24 format (signed 24-bit in int32)
 * @return Processed output sample in S24 format
 */
int32_t DspDistortion_Process(DistState *st, int32_t x);

/**
 * @brief Get pointer to left channel distortion state
 * @return Pointer to left channel state
 */
DistState* DspDistortion_GetStateL(void);

/**
 * @brief Get pointer to right channel distortion state
 * @return Pointer to right channel state
 */
DistState* DspDistortion_GetStateR(void);

#ifdef __cplusplus
}
#endif

#endif /* DSP_DISTORTION_H */
