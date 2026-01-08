#ifndef DSP_FILTERS_H
#define DSP_FILTERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DSP Filters Module
 * 
 * Provides various filtering and dynamic processing functions for audio:
 * - DC blocker: removes DC offset and subsonic content
 * - High-pass filters: removes low-frequency rumble
 * - Compressor: reduces dynamic range for smoother sound
 * - Limiter: prevents output clipping by fast gain reduction
 * - Cabinet simulator: lowpass filter emulating guitar speaker cabinet
 * ============================================================================ */

/* DC Blocker State
 * First-order highpass filter with very low cutoff (~20Hz at 48kHz) */
typedef struct
{
  int32_t x1;  /**< Previous input sample */
  int32_t y1;  /**< Previous output sample */
} DcBlockState;

/* Compressor State
 * Tracks envelope and applies dynamic gain reduction */
typedef struct
{
  int32_t env;        /**< Envelope follower state (peak detector) */
  int32_t gain_q15;   /**< Current gain value in Q15 format (0..32768) */
} CompState;

/* Biquad Filter State
 * Second-order IIR filter for cabinet simulation */
typedef struct
{
  int32_t x1;  /**< Input delayed by 1 sample */
  int32_t x2;  /**< Input delayed by 2 samples */
  int32_t y1;  /**< Output delayed by 1 sample */
  int32_t y2;  /**< Output delayed by 2 samples */
} BiquadState;

/* Limiter State
 * Fast peak limiter for output protection */
typedef struct
{
  int32_t gain_q15;  /**< Current gain reduction in Q15 format (0..32768) */
} LimiterState;

/**
 * @brief Initialize all filter states to zero
 * 
 * Resets DC blockers, HPFs, compressors, limiter, and cabinet sim filters
 * for both left and right channels.
 */
void DspFilters_Init(void);

/**
 * @brief Apply DC blocking filter to remove DC offset
 * 
 * Implements a first-order highpass filter with ~20Hz cutoff to remove
 * any DC component and subsonic content from the signal.
 * 
 * @param st Pointer to filter state structure
 * @param x Input sample in S24 format (signed 24-bit in int32)
 * @return Filtered output sample in S24 format
 */
int32_t DspFilters_DcBlock(DcBlockState *st, int32_t x);

/**
 * @brief Apply first-order highpass filter
 * 
 * Generic first-order HPF with configurable cutoff frequency.
 * Used for clean signal conditioning (~90Hz) and wet signal tightening (~180Hz).
 * 
 * @param st Pointer to filter state structure
 * @param x Input sample in S24 format
 * @param r_q15 Filter coefficient in Q15 format (determines cutoff frequency)
 * @return Filtered output sample in S24 format
 */
int32_t DspFilters_Hpf1(DcBlockState *st, int32_t x, int32_t r_q15);

/**
 * @brief Process one channel through gentle compressor
 * 
 * Implements a feedback compressor with:
 * - Envelope follower (peak detector with attack/release)
 * - Ratio control (2:1 typical for gentle compression)
 * - Gain smoothing to avoid pumping artifacts
 * 
 * @param st Pointer to compressor state
 * @param x_s24 Pointer to audio sample (modified in-place)
 */
void DspFilters_CompressOne(CompState *st, int32_t *x_s24);

/**
 * @brief Process stereo signal through dual-mono compressor
 * 
 * Applies independent compression to left and right channels.
 * Provides smooth dynamics and sustain for clean guitar tones.
 * 
 * @param l Pointer to left channel sample (modified in-place)
 * @param r Pointer to right channel sample (modified in-place)
 */
void DspFilters_CompressStereo(int32_t *l, int32_t *r);

/**
 * @brief Apply fast peak limiter for output protection
 * 
 * Prevents clipping by applying instant attack gain reduction when signal
 * exceeds threshold, with smooth release. Essential for maintaining
 * headroom and preventing distortion.
 * 
 * @param l Pointer to left channel sample (modified in-place)
 * @param r Pointer to right channel sample (modified in-place)
 */
void DspFilters_Limiter(int32_t *l, int32_t *r);

/**
 * @brief Apply cabinet simulator lowpass filter
 * 
 * Second-order Butterworth lowpass (~5kHz @ 48kHz sampling rate) that
 * emulates the frequency response of a guitar speaker cabinet,
 * removing harsh high-frequency content after distortion.
 * 
 * @param st Pointer to biquad filter state
 * @param x Input sample in S24 format
 * @return Filtered output sample in S24 format
 */
int32_t DspFilters_CabSim(BiquadState *st, int32_t x);

/**
 * @brief Apply simple one-pole lowpass filter
 * 
 * First-order IIR lowpass used for smoothing wet signals and
 * feedback paths in delay/reverb effects.
 * 
 * @param x Input sample in S24 format
 * @param st Pointer to filter state (previous output)
 * @param a_q15 Filter coefficient in Q15 format (higher = faster response)
 * @return Filtered output sample in S24 format
 */
int32_t DspFilters_OnePole(int32_t x, int32_t *st, int32_t a_q15);

/**
 * @brief Get pointer to left DC blocker state
 * @return Pointer to DC blocker state for left channel
 */
DcBlockState* DspFilters_GetDcL(void);

/**
 * @brief Get pointer to right DC blocker state
 * @return Pointer to DC blocker state for right channel
 */
DcBlockState* DspFilters_GetDcR(void);

/**
 * @brief Get pointer to left clean HPF state
 * @return Pointer to HPF state for left channel
 */
DcBlockState* DspFilters_GetCleanHpfL(void);

/**
 * @brief Get pointer to right clean HPF state
 * @return Pointer to HPF state for right channel
 */
DcBlockState* DspFilters_GetCleanHpfR(void);

/**
 * @brief Get pointer to left delay wet HPF state
 * @return Pointer to HPF state for left delay wet signal
 */
DcBlockState* DspFilters_GetWetHpfDelayL(void);

/**
 * @brief Get pointer to right delay wet HPF state
 * @return Pointer to HPF state for right delay wet signal
 */
DcBlockState* DspFilters_GetWetHpfDelayR(void);

/**
 * @brief Get pointer to left reverb wet HPF state
 * @return Pointer to HPF state for left reverb wet signal
 */
DcBlockState* DspFilters_GetWetHpfReverbL(void);

/**
 * @brief Get pointer to right reverb wet HPF state
 * @return Pointer to HPF state for right reverb wet signal
 */
DcBlockState* DspFilters_GetWetHpfReverbR(void);

/**
 * @brief Get pointer to left cabinet sim state
 * @return Pointer to biquad filter state for left channel
 */
BiquadState* DspFilters_GetCabL(void);

/**
 * @brief Get pointer to right cabinet sim state
 * @return Pointer to biquad filter state for right channel
 */
BiquadState* DspFilters_GetCabR(void);

/**
 * @brief Get pointer to left delay wet LPF state
 * @return Pointer to one-pole filter state
 */
int32_t* DspFilters_GetWetLpfDelayL(void);

/**
 * @brief Get pointer to right delay wet LPF state
 * @return Pointer to one-pole filter state
 */
int32_t* DspFilters_GetWetLpfDelayR(void);

/**
 * @brief Get pointer to left reverb wet LPF state
 * @return Pointer to one-pole filter state
 */
int32_t* DspFilters_GetWetLpfReverbL(void);

/**
 * @brief Get pointer to right reverb wet LPF state
 * @return Pointer to one-pole filter state
 */
int32_t* DspFilters_GetWetLpfReverbR(void);

#ifdef __cplusplus
}
#endif

#endif /* DSP_FILTERS_H */
