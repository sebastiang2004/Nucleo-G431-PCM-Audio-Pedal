#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <stdint.h>
#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

void AppAudio_Init(I2S_HandleTypeDef *rx_i2s, I2S_HandleTypeDef *tx_i2s);
void AppAudio_Start(void);

uint8_t AppAudio_StartFailed(void);
uint8_t AppAudio_RuntimeFailed(void);

void AppAudio_OnRxHalfCplt(I2S_HandleTypeDef *hi2s);
void AppAudio_OnRxCplt(I2S_HandleTypeDef *hi2s);
void AppAudio_OnTxHalfCplt(I2S_HandleTypeDef *hi2s);
void AppAudio_OnTxCplt(I2S_HandleTypeDef *hi2s);
void AppAudio_OnError(I2S_HandleTypeDef *hi2s);

#ifdef __cplusplus
}
#endif

#endif /* APP_AUDIO_H */
