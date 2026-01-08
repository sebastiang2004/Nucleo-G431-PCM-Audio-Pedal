#ifndef DSP_REVERB_H
#define DSP_REVERB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DSP Reverb Module
 * 
 * Implements algorithmic reverb effect using:
 * - Feedback delay network for room simulation
 * - Allpass filters for diffusion (density)
 * - Damping filter to control brightness
 * - Optional LFO modulation for chorus-like shimmer
 * - Adjustable mix, feedback, and damping
 * ============================================================================ */

/**
 * @brief Initialize reverb effect
 * 
 * Clears all delay lines, allpass buffers, and filter states.
 * Must be called before first use.
 */
void DspReverb_Init(void);

/**
 * @brief Set reverb mix (wet/dry balance)
 * 
 * Controls how much reverb is mixed with the dry signal.
 * 
 * @param mix_q15 Mix amount in Q15 format (0..32768)
 *                0 = fully dry (no reverb)
 *                16384 = 50/50 mix
 *                32768 = fully wet (reverb only)
 */
void DspReverb_SetMix(int32_t mix_q15);

/**
 * @brief Get current reverb mix setting
 * @return Mix amount in Q15 format
 */
int32_t DspReverb_GetMix(void);

/**
 * @brief Set reverb feedback amount
 * 
 * Controls decay time of the reverb tail.
 * Higher feedback creates longer reverb.
 * 
 * @param feedback_q15 Feedback amount in Q15 format (0..32768)
 *                     ~16384 = short room
 *                     ~22000 = medium hall
 *                     ~28000 = large cathedral
 */
void DspReverb_SetFeedback(int32_t feedback_q15);

/**
 * @brief Get current reverb feedback setting
 * @return Feedback amount in Q15 format
 */
int32_t DspReverb_GetFeedback(void);

/**
 * @brief Set reverb damping (high-frequency absorption)
 * 
 * Controls how quickly high frequencies decay.
 * Emulates material absorption in a real room.
 * 
 * @param damp_q15 Damping amount in Q15 format (0..32768)
 *                 Small values = bright, metallic reverb
 *                 Large values = darker, warmer reverb
 */
void DspReverb_SetDamp(int32_t damp_q15);

/**
 * @brief Get current reverb damping setting
 * @return Damping amount in Q15 format
 */
int32_t DspReverb_GetDamp(void);

/**
 * @brief Process one channel through reverb effect
 * 
 * Signal flow:
 * 1. Read from feedback delay line (with optional modulation)
 * 2. Apply damping lowpass filter
 * 3. Scale by feedback amount
 * 4. Sum with input and write back to delay
 * 5. Pass through 2-stage allpass diffusion network
 * 
 * @param x Input sample in S24 format (signed 24-bit in int32)
 * @param delay Pointer to delay buffer array
 * @param delay_idx Pointer to current delay line position
 * @param lp Pointer to damping filter state
 * @param ap_buf Pointer to allpass buffer array
 * @param ap1_idx Pointer to allpass stage 1 position
 * @param ap2_idx Pointer to allpass stage 2 position
 * @param lfo_phase Pointer to LFO phase accumulator
 * @param lfo_step LFO phase increment per sample
 * @return Reverb output sample in S24 format
 */
int32_t DspReverb_Process(int32_t x,
                          int32_t *delay,
                          uint32_t *delay_idx,
                          int32_t *lp,
                          int32_t *ap_buf,
                          uint32_t *ap1_idx,
                          uint32_t *ap2_idx,
                          uint32_t *lfo_phase,
                          uint32_t lfo_step);

/**
 * @brief Get pointer to left channel delay buffer
 * @return Pointer to left delay buffer
 */
int32_t* DspReverb_GetDelayL(void);

/**
 * @brief Get pointer to right channel delay buffer
 * @return Pointer to right delay buffer
 */
int32_t* DspReverb_GetDelayR(void);

/**
 * @brief Get pointer to left channel allpass buffer
 * @return Pointer to left allpass buffer
 */
int32_t* DspReverb_GetAllpassL(void);

/**
 * @brief Get pointer to right channel allpass buffer
 * @return Pointer to right allpass buffer
 */
int32_t* DspReverb_GetAllpassR(void);

/**
 * @brief Get pointer to left channel delay index
 * @return Pointer to left delay index
 */
uint32_t* DspReverb_GetDelayIdxL(void);

/**
 * @brief Get pointer to right channel delay index
 * @return Pointer to right delay index
 */
uint32_t* DspReverb_GetDelayIdxR(void);

/**
 * @brief Get pointer to left channel allpass stage 1 index
 * @return Pointer to left AP1 index
 */
uint32_t* DspReverb_GetAp1IdxL(void);

/**
 * @brief Get pointer to right channel allpass stage 1 index
 * @return Pointer to right AP1 index
 */
uint32_t* DspReverb_GetAp1IdxR(void);

/**
 * @brief Get pointer to left channel allpass stage 2 index
 * @return Pointer to left AP2 index
 */
uint32_t* DspReverb_GetAp2IdxL(void);

/**
 * @brief Get pointer to right channel allpass stage 2 index
 * @return Pointer to right AP2 index
 */
uint32_t* DspReverb_GetAp2IdxR(void);

/**
 * @brief Get pointer to left channel lowpass state
 * @return Pointer to left LP state
 */
int32_t* DspReverb_GetLpL(void);

/**
 * @brief Get pointer to right channel lowpass state
 * @return Pointer to right LP state
 */
int32_t* DspReverb_GetLpR(void);

/**
 * @brief Get pointer to left channel LFO phase
 * @return Pointer to left LFO phase
 */
uint32_t* DspReverb_GetLfoL(void);

/**
 * @brief Get pointer to right channel LFO phase
 * @return Pointer to right LFO phase
 */
uint32_t* DspReverb_GetLfoR(void);

#ifdef __cplusplus
}
#endif

#endif /* DSP_REVERB_H */
