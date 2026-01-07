#ifndef APP_DSP_H
#define APP_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  APP_FX_MODE_BYPASS = 0,
  APP_FX_MODE_DISTORTION,
  APP_FX_MODE_REVERB,
  APP_FX_MODE_DELAY,
  APP_FX_MODE_ALL,
} AppFxMode;

typedef enum
{
  APP_FX_BIT_DISTORTION = (1u << 0),
  APP_FX_BIT_REVERB     = (1u << 1),
  APP_FX_BIT_DELAY      = (1u << 2),
} AppFxBit;

typedef uint32_t AppFxMask;

typedef enum
{
  APP_DSP_PARAM_DIST_DRIVE_Q8 = 0,
  APP_DSP_PARAM_DELAY_MIX_Q15,
  APP_DSP_PARAM_DELAY_FEEDBACK_Q15,
  APP_DSP_PARAM_REVERB_MIX_Q15,
  APP_DSP_PARAM_REVERB_FEEDBACK_Q15,
  APP_DSP_PARAM_REVERB_DAMP_Q15,
  APP_DSP_PARAM_GAIN_Q15,
} AppDspParamId;

void AppDsp_Init(void);

/* Debounced mode cycle for a single user action.
 * Pass a monotonically increasing millisecond tick (e.g. HAL_GetTick()).
 */
void AppDsp_OnButtonPress(uint32_t now_ms);

AppFxMode AppDsp_GetMode(void);

/* New control API: effect combinations + runtime parameters (for COM control). */
void AppDsp_SetFxMask(AppFxMask mask);
AppFxMask AppDsp_GetFxMask(void);

void AppDsp_SetParam(AppDspParamId id, int32_t value);
int32_t AppDsp_GetParam(AppDspParamId id);

/* In-place processing of one stereo frame.
 * Samples are signed 24-bit in int32_t (range: [-8388608, 8388607]).
 */
void AppDsp_ProcessFrame(int32_t *l_s24, int32_t *r_s24);

#ifdef __cplusplus
}
#endif

#endif /* APP_DSP_H */
