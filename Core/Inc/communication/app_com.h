#ifndef APP_COM_H
#define APP_COM_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

void AppCom_Init(UART_HandleTypeDef *huart);
void AppCom_Poll(void);

/* Hook from HAL callbacks (main.c). */
void AppCom_OnUartRxCplt(UART_HandleTypeDef *huart);
void AppCom_OnUartRxEvent(UART_HandleTypeDef *huart, uint16_t size);
void AppCom_OnUartTxCplt(UART_HandleTypeDef *huart);
void AppCom_OnUartError(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* APP_COM_H */
