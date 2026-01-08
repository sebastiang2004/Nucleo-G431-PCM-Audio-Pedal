#include "dsp/app_dsp.h"

/* Include all DSP module headers */
#include "dsp/dsp_filters.h"
#include "dsp/dsp_distortion.h"
#include "dsp/dsp_delay.h"
#include "dsp/dsp_reverb.h"
#include <stdbool.h>

/* ============================================================================
 * DSP Core Module - Main Audio Processing Coordinator
 * 
 * This module orchestrates all DSP effects and provides the public API.
 * The actual DSP algorithms are implemented in separate modules:
 * - dsp_filters: DC blocker, HPF, compressor, limiter, cab-sim
 * - dsp_distortion: Guitar amp-style distortion
 * - dsp_delay: Echo/delay effect with feedback
 * - dsp_reverb: Algorithmic reverb with allpass diffusion
 * 
 * Signal Flow in ProcessFrame:
 * 1. DC blocking (remove offset)
 * 2. Clean HPF (tighten low end)
 * 3. Input gain staging
 * 4. Gentle compression (smooth dynamics)
 * 5. FX chain: Distortion -> Delay -> Reverb
 * 6. Makeup gain
 * 7. Master volume
 * 8. Peak limiter (output protection)
 * ============================================================================ */

/* Configuration Constants */
#define AUDIO_INPUT_GAIN_Q8            1536     /* 6x input gain */
#define AUDIO_MAKEUP_GAIN_Q8           1000     /* ~4x makeup gain */
#define CLEAN_HPF_ENABLE               1
#define CLEAN_HPF_R_Q15                32384    /* ~90Hz @ 48kHz */
#define CABSIM_ENABLE                  1
#define WET_HPF_ENABLE                 1
#define WET_HPF_R_Q15                  32004    /* ~180Hz @ 48kHz */
#define WET_LPF_A_Q15                  2048     /* Wet signal smoothing */

/* Static State Variables */
static volatile AppFxMode s_mode = APP_FX_MODE_BYPASS;
static volatile uint32_t s_button_last_ms = 0;
static volatile AppFxMask s_fx_mask = 0;
static volatile int32_t s_gain_q15 = 32768;  /* Master volume (Q15) */

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

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
 * @brief Apply gain in Q8 fixed-point format
 * @param x Input sample
 * @param gain_q8 Gain value (256 = 1.0x)
 * @return Scaled output
 */
static inline int32_t gain_s32_q8(int32_t x, int32_t gain_q8)
{
  return (int32_t)(((int64_t)x * (int64_t)gain_q8) >> 8);
}

/**
 * @brief Mix dry and wet signals
 * @param dry Dry (original) signal
 * @param wet Wet (processed) signal
 * @param mix_q15 Mix amount in Q15 (0=dry, 32768=wet)
 * @return Mixed output
 */
static inline int32_t mix_s24(int32_t dry, int32_t wet, int32_t mix_q15)
{
  int32_t a = 32768 - mix_q15;
  return (int32_t)((((int64_t)dry * a) + ((int64_t)wet * mix_q15)) >> 15);
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

void AppDsp_Init(void)
{
  /* Initialize all DSP modules */
  DspFilters_Init();
  DspDistortion_Init();
  DspDelay_Init();
  DspReverb_Init();
  
  /* Reset mode and parameters */
  s_mode = APP_FX_MODE_BYPASS;
  s_button_last_ms = 0;
  s_fx_mask = 0;
  s_gain_q15 = 32768;  /* Unity gain */
}

void AppDsp_OnButtonPress(uint32_t now_ms)
{
  /* Debounce: ignore button presses within 300ms of each other
   * Prevents accidental double-triggers from mechanical bounce */
  if ((now_ms - s_button_last_ms) < 300U)
  {
    return;
  }
  s_button_last_ms = now_ms;

  /* Cycle through effect modes:
   * BYPASS -> DISTORTION -> REVERB -> DELAY -> ALL -> BYPASS */
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

  /* Update FX mask to match legacy mode behavior */
  if (s_mode == APP_FX_MODE_BYPASS) 
    s_fx_mask = 0;
  else if (s_mode == APP_FX_MODE_DISTORTION) 
    s_fx_mask = APP_FX_BIT_DISTORTION;
  else if (s_mode == APP_FX_MODE_REVERB) 
    s_fx_mask = APP_FX_BIT_REVERB;
  else if (s_mode == APP_FX_MODE_DELAY) 
    s_fx_mask = APP_FX_BIT_DELAY;
  else if (s_mode == APP_FX_MODE_ALL) 
    s_fx_mask = (APP_FX_BIT_DISTORTION | APP_FX_BIT_REVERB | APP_FX_BIT_DELAY);
}

AppFxMode AppDsp_GetMode(void)
{
  return (AppFxMode)s_mode;
}

void AppDsp_SetFxMask(AppFxMask mask)
{
  /* Sanitize mask: only allow defined FX bits */
  mask &= (AppFxMask)(APP_FX_BIT_DISTORTION | APP_FX_BIT_REVERB | APP_FX_BIT_DELAY);
  s_fx_mask = mask;

  /* Update legacy mode for consistent status/LED behavior */
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

void AppDsp_SetParam(AppDspParamId id, int32_t value)
{
  /* Route parameter changes to appropriate DSP module */
  switch (id)
  {
    case APP_DSP_PARAM_DIST_DRIVE_Q8:
      DspDistortion_SetDrive(value);
      break;
    case APP_DSP_PARAM_DELAY_MIX_Q15:
      DspDelay_SetMix(value);
      break;
    case APP_DSP_PARAM_DELAY_FEEDBACK_Q15:
      DspDelay_SetFeedback(value);
      break;
    case APP_DSP_PARAM_REVERB_MIX_Q15:
      DspReverb_SetMix(value);
      break;
    case APP_DSP_PARAM_REVERB_FEEDBACK_Q15:
      DspReverb_SetFeedback(value);
      break;
    case APP_DSP_PARAM_REVERB_DAMP_Q15:
      DspReverb_SetDamp(value);
      break;
    case APP_DSP_PARAM_GAIN_Q15:
      /* Master volume: allow up to 2.0x gain */
      if (value < 0) value = 0;
      if (value > 65536) value = 65536;
      s_gain_q15 = value;
      break;
    default:
      break;
  }
}

int32_t AppDsp_GetParam(AppDspParamId id)
{
  /* Route parameter queries to appropriate DSP module */
  switch (id)
  {
    case APP_DSP_PARAM_DIST_DRIVE_Q8:
      return DspDistortion_GetDrive();
    case APP_DSP_PARAM_DELAY_MIX_Q15:
      return DspDelay_GetMix();
    case APP_DSP_PARAM_DELAY_FEEDBACK_Q15:
      return DspDelay_GetFeedback();
    case APP_DSP_PARAM_REVERB_MIX_Q15:
      return DspReverb_GetMix();
    case APP_DSP_PARAM_REVERB_FEEDBACK_Q15:
      return DspReverb_GetFeedback();
    case APP_DSP_PARAM_REVERB_DAMP_Q15:
      return DspReverb_GetDamp();
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

  /* Determine which effects are enabled */
  AppFxMask mask = (AppFxMask)s_fx_mask;
  bool has_dist = (mask & APP_FX_BIT_DISTORTION) != 0;
  bool has_rev  = (mask & APP_FX_BIT_REVERB) != 0;
  bool has_del  = (mask & APP_FX_BIT_DELAY) != 0;
  uint32_t fx_count = (has_dist ? 1u : 0u) + (has_rev ? 1u : 0u) + (has_del ? 1u : 0u);

  /* ========== Pre-Processing ========== */
  
  /* Remove DC offset and subsonic content */
  l = DspFilters_DcBlock(DspFilters_GetDcL(), l);
  r = DspFilters_DcBlock(DspFilters_GetDcR(), r);

#if CLEAN_HPF_ENABLE
  /* Tighten low end for cleaner guitar tone */
  l = DspFilters_Hpf1(DspFilters_GetCleanHpfL(), l, CLEAN_HPF_R_Q15);
  r = DspFilters_Hpf1(DspFilters_GetCleanHpfR(), r, CLEAN_HPF_R_Q15);
#endif

  /* Boost input signal for better processing headroom */
  l = gain_s32_q8(l, AUDIO_INPUT_GAIN_Q8);
  r = gain_s32_q8(r, AUDIO_INPUT_GAIN_Q8);

  /* Apply gentle compression for smoother dynamics */
  DspFilters_CompressStereo(&l, &r);

  /* ========== Effects Chain ========== */
  
  /* DISTORTION: Guitar amp-style overdrive with cabinet sim */
  if (has_dist)
  {
    l = DspDistortion_Process(DspDistortion_GetStateL(), clamp_s24(l));
    r = DspDistortion_Process(DspDistortion_GetStateR(), clamp_s24(r));

#if CABSIM_ENABLE
    /* Simulate speaker cabinet frequency response */
    l = DspFilters_CabSim(DspFilters_GetCabL(), l);
    r = DspFilters_CabSim(DspFilters_GetCabR(), r);
#endif
  }

  /* DELAY: Echo effect with feedback */
  if (has_del)
  {
    int32_t wl = DspDelay_Process(clamp_s24(l), DspDelay_GetBufferL(), DspDelay_GetStateL());
    int32_t wr = DspDelay_Process(clamp_s24(r), DspDelay_GetBufferR(), DspDelay_GetStateR());
    
#if WET_HPF_ENABLE
    /* Remove low-frequency mud from delay repeats */
    wl = DspFilters_Hpf1(DspFilters_GetWetHpfDelayL(), wl, WET_HPF_R_Q15);
    wr = DspFilters_Hpf1(DspFilters_GetWetHpfDelayR(), wr, WET_HPF_R_Q15);
#endif
    
    /* Smooth delay output to reduce digital harshness */
    wl = DspFilters_OnePole(wl, DspFilters_GetWetLpfDelayL(), WET_LPF_A_Q15);
    wr = DspFilters_OnePole(wr, DspFilters_GetWetLpfDelayR(), WET_LPF_A_Q15);

    /* Mix delay with dry signal (reduce mix when stacking multiple FX) */
    int32_t mix = DspDelay_GetMix();
    if (fx_count > 1u)
    {
      mix = (mix * 3) / 4;  /* 75% of original mix in stacked mode */
    }
    l = mix_s24(clamp_s24(l), wl, mix);
    r = mix_s24(clamp_s24(r), wr, mix);
  }

  /* REVERB: Room ambience and space */
  if (has_rev)
  {
    int32_t wl = DspReverb_Process(clamp_s24(l), 
                                   DspReverb_GetDelayL(), 
                                   DspReverb_GetDelayIdxL(), 
                                   DspReverb_GetLpL(), 
                                   DspReverb_GetAllpassL(), 
                                   DspReverb_GetAp1IdxL(), 
                                   DspReverb_GetAp2IdxL(), 
                                   DspReverb_GetLfoL(), 
                                   65536U);
    
    int32_t wr = DspReverb_Process(clamp_s24(r), 
                                   DspReverb_GetDelayR(), 
                                   DspReverb_GetDelayIdxR(), 
                                   DspReverb_GetLpR(), 
                                   DspReverb_GetAllpassR(), 
                                   DspReverb_GetAp1IdxR(), 
                                   DspReverb_GetAp2IdxR(), 
                                   DspReverb_GetLfoR(), 
                                   77824U);
    
#if WET_HPF_ENABLE
    /* Keep reverb tight by filtering lows */
    wl = DspFilters_Hpf1(DspFilters_GetWetHpfReverbL(), wl, WET_HPF_R_Q15);
    wr = DspFilters_Hpf1(DspFilters_GetWetHpfReverbR(), wr, WET_HPF_R_Q15);
#endif
    
    /* Smooth reverb output */
    wl = DspFilters_OnePole(wl, DspFilters_GetWetLpfReverbL(), WET_LPF_A_Q15);
    wr = DspFilters_OnePole(wr, DspFilters_GetWetLpfReverbR(), WET_LPF_A_Q15);
    
    /* Use less reverb when stacking effects to keep clarity */
    int32_t mix = (fx_count > 1u) ? (DspReverb_GetMix() / 2) : DspReverb_GetMix();
    l = mix_s24(clamp_s24(l), wl, mix);
    r = mix_s24(clamp_s24(r), wr, mix);
  }

  /* ========== Post-Processing ========== */
  
  /* Makeup gain to compensate for processing losses */
  int32_t makeup_q8 = AUDIO_MAKEUP_GAIN_Q8;
  if (mask != 0)
  {
    makeup_q8 = (makeup_q8 * 3) / 4;  /* Reduce slightly when FX active */
  }
  l = gain_s32_q8(l, makeup_q8);
  r = gain_s32_q8(r, makeup_q8);

  /* Apply master volume control */
  l = clamp_s24((int32_t)(((int64_t)l * (int64_t)s_gain_q15) >> 15));
  r = clamp_s24((int32_t)(((int64_t)r * (int64_t)s_gain_q15) >> 15));

  /* Final protection: fast peak limiter prevents clipping */
  DspFilters_Limiter(&l, &r);

  /* Write processed samples back */
  *l_s24 = clamp_s24(l);
  *r_s24 = clamp_s24(r);
}
