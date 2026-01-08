#include "error/app_error.h"
#include "stm32g4xx_hal.h"

void AppError_BlinkForever(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  __disable_irq();
  while (1)
  {
    HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
    for (volatile uint32_t i = 0; i < 200000U; i++)
    {
      __NOP();
    }
  }
}
