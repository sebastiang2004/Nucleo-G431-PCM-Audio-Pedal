#include "communication/app_com.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/app_dsp.h"

/* TX is interrupt-driven to avoid stalling the MCU when the host sends a lot
 * of commands (PSET/FXMASK). Replies are enqueued into a ring buffer and
 * drained via HAL_UART_Transmit_IT(). */

#ifndef APP_COM_TX_RING_SIZE
#define APP_COM_TX_RING_SIZE 512u
#endif

/* Simple, line-based ASCII protocol over UART.
 * Commands (\n terminated):
 *   PING                       -> PONG
 *   STATUS                     -> STATUS FXMASK=<n> ...
 *   FXMASK <n>                 -> OK FXMASK <n>
 *   PSET <param> <value>       -> OK PSET <param> <value>
 *
 * Params:
 *   dist_drive_q8       (0..131072)
 *   gain_q15            (0..65536)
 *   delay_mix_q15       (0..32768)
 *   delay_feedback_q15  (0..32768)
 *   reverb_mix_q15      (0..32768)
 *   reverb_feedback_q15 (0..32768)
 *   reverb_damp_q15     (0..32768)
 */

#ifndef APP_COM_RX_RING_SIZE
/* Larger RX ring so we don't corrupt commands when the audio/DSP load is high.
 * Dropping bytes can turn valid commands into garbage, leading to ERR UNKNOWN.
 */
#define APP_COM_RX_RING_SIZE 1024u
#endif

#ifndef APP_COM_LINE_MAX
#define APP_COM_LINE_MAX 160u
#endif

#ifndef APP_COM_RX_DMA_SIZE
#define APP_COM_RX_DMA_SIZE 512u
#endif

static UART_HandleTypeDef *s_uart = NULL;

static volatile uint8_t s_rx_ring[APP_COM_RX_RING_SIZE];
static volatile uint16_t s_rx_wr = 0;
static volatile uint16_t s_rx_rd = 0;

static uint8_t s_rx_byte = 0;

static char s_line[APP_COM_LINE_MAX];
static uint16_t s_line_len = 0;

static volatile uint8_t s_tx_ring[APP_COM_TX_RING_SIZE];
static volatile uint16_t s_tx_wr = 0;
static volatile uint16_t s_tx_rd = 0;
static volatile uint8_t s_tx_busy = 0;
static volatile uint16_t s_tx_last_len = 0;

static uint8_t s_rx_dma[APP_COM_RX_DMA_SIZE];

typedef enum
{
  APP_COM_RX_MODE_BYTE = 0,
  APP_COM_RX_MODE_IDLE_IT = 1,
  APP_COM_RX_MODE_IDLE_DMA = 2,
} AppComRxMode;

static volatile AppComRxMode s_rx_mode = APP_COM_RX_MODE_BYTE;

static inline uint16_t ring_next(uint16_t idx)
{
  return (uint16_t)((idx + 1u) % APP_COM_RX_RING_SIZE);
}

static inline uint16_t tx_ring_next(uint16_t idx)
{
  return (uint16_t)((idx + 1u) % APP_COM_TX_RING_SIZE);
}

static uint16_t tx_ring_free(void)
{
  uint16_t rd = s_tx_rd;
  uint16_t wr = s_tx_wr;
  if (wr >= rd)
  {
    return (uint16_t)((APP_COM_TX_RING_SIZE - (wr - rd)) - 1u);
  }
  return (uint16_t)((rd - wr) - 1u);
}

static void tx_kick(void)
{
  if (s_uart == NULL)
  {
    return;
  }

  if (s_tx_busy)
  {
    return;
  }

  uint16_t rd = s_tx_rd;
  uint16_t wr = s_tx_wr;
  if (rd == wr)
  {
    return;
  }

  /* Send the largest contiguous chunk (until wrap or wr). */
  uint16_t len = 0;
  if (wr > rd)
  {
    len = (uint16_t)(wr - rd);
  }
  else
  {
    len = (uint16_t)(APP_COM_TX_RING_SIZE - rd);
  }

  s_tx_busy = 1;
  s_tx_last_len = len;
  if (HAL_UART_Transmit_IT(s_uart, (uint8_t *)&s_tx_ring[rd], len) != HAL_OK)
  {
    s_tx_busy = 0;
    s_tx_last_len = 0;
  }
}

static void tx_enqueue_bytes(const uint8_t *data, uint16_t len)
{
  if (s_uart == NULL || data == NULL || len == 0)
  {
    return;
  }

  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  if (tx_ring_free() < len)
  {
    /* Drop if TX ring is full; prefer dropping replies over blocking audio/DSP. */
    if (!primask)
    {
      __enable_irq();
    }
    return;
  }

  for (uint16_t i = 0; i < len; i++)
  {
    s_tx_ring[s_tx_wr] = data[i];
    s_tx_wr = tx_ring_next(s_tx_wr);
  }

  if (!primask)
  {
    __enable_irq();
  }

  tx_kick();
}

static void uart_send_line(const char *line)
{
  if (s_uart == NULL || line == NULL)
  {
    return;
  }

  const uint16_t n = (uint16_t)strlen(line);
  tx_enqueue_bytes((const uint8_t *)line, n);
  tx_enqueue_bytes((const uint8_t *)"\n", 1);
}

static void trim_inplace(char *s)
{
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || isspace((unsigned char)s[n - 1])))
  {
    s[n - 1] = 0;
    n--;
  }

  size_t i = 0;
  while (s[i] != 0 && isspace((unsigned char)s[i]))
  {
    i++;
  }

  if (i > 0)
  {
    memmove(s, s + i, strlen(s + i) + 1);
  }
}

static bool parse_u32(const char *s, uint32_t *out)
{
  if (s == NULL || out == NULL)
  {
    return false;
  }

  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (end == s)
  {
    return false;
  }
  *out = (uint32_t)v;
  return true;
}

static bool parse_i32(const char *s, int32_t *out)
{
  if (s == NULL || out == NULL)
  {
    return false;
  }

  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (end == s)
  {
    return false;
  }
  *out = (int32_t)v;
  return true;
}

static bool map_param(const char *name, AppDspParamId *out)
{
  if (name == NULL || out == NULL)
  {
    return false;
  }

  if (strcmp(name, "dist_drive_q8") == 0)
  {
    *out = APP_DSP_PARAM_DIST_DRIVE_Q8;
    return true;
  }
  if (strcmp(name, "gain_q15") == 0)
  {
    *out = APP_DSP_PARAM_GAIN_Q15;
    return true;
  }
  if (strcmp(name, "delay_mix_q15") == 0)
  {
    *out = APP_DSP_PARAM_DELAY_MIX_Q15;
    return true;
  }
  if (strcmp(name, "delay_feedback_q15") == 0)
  {
    *out = APP_DSP_PARAM_DELAY_FEEDBACK_Q15;
    return true;
  }
  if (strcmp(name, "reverb_mix_q15") == 0)
  {
    *out = APP_DSP_PARAM_REVERB_MIX_Q15;
    return true;
  }
  if (strcmp(name, "reverb_feedback_q15") == 0)
  {
    *out = APP_DSP_PARAM_REVERB_FEEDBACK_Q15;
    return true;
  }
  if (strcmp(name, "reverb_damp_q15") == 0)
  {
    *out = APP_DSP_PARAM_REVERB_DAMP_Q15;
    return true;
  }

  return false;
}

static void handle_line(char *line)
{
  trim_inplace(line);
  if (line[0] == 0)
  {
    return;
  }

  /* Keep a copy for debug error replies. */
  char line_copy[APP_COM_LINE_MAX];
  (void)snprintf(line_copy, sizeof(line_copy), "%s", line);

  char *cmd = strtok(line, " \t");
  if (cmd == NULL)
  {
    return;
  }

  if (strcmp(cmd, "PING") == 0)
  {
    uart_send_line("PONG");
    return;
  }

  if (strcmp(cmd, "STATUS") == 0)
  {
    char buf[200];
    uint32_t mask = AppDsp_GetFxMask();
    int32_t dist_drive = AppDsp_GetParam(APP_DSP_PARAM_DIST_DRIVE_Q8);
    int32_t gain_q15 = AppDsp_GetParam(APP_DSP_PARAM_GAIN_Q15);
    int32_t delay_mix = AppDsp_GetParam(APP_DSP_PARAM_DELAY_MIX_Q15);
    int32_t delay_fb = AppDsp_GetParam(APP_DSP_PARAM_DELAY_FEEDBACK_Q15);
    int32_t rev_mix = AppDsp_GetParam(APP_DSP_PARAM_REVERB_MIX_Q15);
    int32_t rev_fb = AppDsp_GetParam(APP_DSP_PARAM_REVERB_FEEDBACK_Q15);
    int32_t rev_damp = AppDsp_GetParam(APP_DSP_PARAM_REVERB_DAMP_Q15);

    (void)snprintf(buf, sizeof(buf),
                   "STATUS FXMASK=%lu dist_drive_q8=%ld gain_q15=%ld delay_mix_q15=%ld delay_feedback_q15=%ld reverb_mix_q15=%ld reverb_feedback_q15=%ld reverb_damp_q15=%ld",
                   (unsigned long)mask,
                   (long)dist_drive,
                   (long)gain_q15,
                   (long)delay_mix,
                   (long)delay_fb,
                   (long)rev_mix,
                   (long)rev_fb,
                   (long)rev_damp);
    uart_send_line(buf);
    return;
  }

  if (strcmp(cmd, "FXMASK") == 0)
  {
    char *arg = strtok(NULL, " \t");
    uint32_t mask = 0;
    if (!parse_u32(arg, &mask))
    {
      uart_send_line("ERR FXMASK");
      return;
    }
    AppDsp_SetFxMask(mask);

    char buf[48];
    (void)snprintf(buf, sizeof(buf), "OK FXMASK %lu", (unsigned long)mask);
    uart_send_line(buf);
    return;
  }

  if (strcmp(cmd, "PSET") == 0)
  {
    char *pname = strtok(NULL, " \t");
    char *pval = strtok(NULL, " \t");

    AppDspParamId id;
    int32_t v = 0;
    if (!map_param(pname, &id) || !parse_i32(pval, &v))
    {
      char buf[160];
      (void)snprintf(buf, sizeof(buf), "ERR PSET name=%s val=%s", (pname != NULL) ? pname : "?", (pval != NULL) ? pval : "?");
      uart_send_line(buf);
      return;
    }

    AppDsp_SetParam(id, v);

    char buf[96];
    (void)snprintf(buf, sizeof(buf), "OK PSET %s %ld", pname, (long)v);
    uart_send_line(buf);
    return;
  }

  {
    char buf[160];
    (void)snprintf(buf, sizeof(buf), "ERR UNKNOWN cmd=%s line=%s", (cmd != NULL) ? cmd : "?", line_copy);
    uart_send_line(buf);
  }
}

void AppCom_Init(UART_HandleTypeDef *huart)
{
  s_uart = huart;
  s_rx_wr = 0;
  s_rx_rd = 0;
  s_line_len = 0;

  s_tx_wr = 0;
  s_tx_rd = 0;
  s_tx_busy = 0;
  s_tx_last_len = 0;

  if (s_uart != NULL)
  {
    /* Robust RX without spamming byte IRQs:
     * - Prefer ReceiveToIdle DMA when DMA is configured.
     * - Else use ReceiveToIdle IT (no DMA required).
     * - Fall back to byte-by-byte RX only if idle-mode can't start.
     */
    s_rx_mode = APP_COM_RX_MODE_BYTE;

    if (s_uart->hdmarx != NULL)
    {
      if (HAL_UARTEx_ReceiveToIdle_DMA(s_uart, s_rx_dma, (uint16_t)APP_COM_RX_DMA_SIZE) == HAL_OK)
      {
        s_rx_mode = APP_COM_RX_MODE_IDLE_DMA;
        __HAL_DMA_DISABLE_IT(s_uart->hdmarx, DMA_IT_HT);
      }
    }

    if (s_rx_mode == APP_COM_RX_MODE_BYTE)
    {
      if (HAL_UARTEx_ReceiveToIdle_IT(s_uart, s_rx_dma, (uint16_t)APP_COM_RX_DMA_SIZE) == HAL_OK)
      {
        s_rx_mode = APP_COM_RX_MODE_IDLE_IT;
      }
    }

    if (s_rx_mode == APP_COM_RX_MODE_BYTE)
    {
      (void)HAL_UART_Receive_IT(s_uart, &s_rx_byte, 1);
    }
    uart_send_line("READY");
  }
}

void AppCom_OnUartTxCplt(UART_HandleTypeDef *huart)
{
  if (s_uart == NULL || huart != s_uart)
  {
    return;
  }

  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  /* Advance read pointer by the actual length we started transmitting. */
  const uint16_t sent = s_tx_last_len;
  if (sent > 0)
  {
    s_tx_rd = (uint16_t)((s_tx_rd + sent) % APP_COM_TX_RING_SIZE);
  }
  s_tx_last_len = 0;

  s_tx_busy = 0;

  if (!primask)
  {
    __enable_irq();
  }

  tx_kick();
}

void AppCom_OnUartRxCplt(UART_HandleTypeDef *huart)
{
  if (s_uart == NULL || huart != s_uart)
  {
    return;
  }

  if (s_rx_mode != APP_COM_RX_MODE_BYTE)
  {
    /* RX is handled by RxEvent callback in idle mode. */
    return;
  }

  uint16_t next = ring_next(s_rx_wr);
  if (next != s_rx_rd)
  {
    s_rx_ring[s_rx_wr] = s_rx_byte;
    s_rx_wr = next;
  }

  (void)HAL_UART_Receive_IT(s_uart, &s_rx_byte, 1);
}

void AppCom_OnUartRxEvent(UART_HandleTypeDef *huart, uint16_t size)
{
  if (s_uart == NULL || huart != s_uart)
  {
    return;
  }

  if (s_rx_mode == APP_COM_RX_MODE_BYTE)
  {
    return;
  }

  const uint16_t n = (size > (uint16_t)APP_COM_RX_DMA_SIZE) ? (uint16_t)APP_COM_RX_DMA_SIZE : size;
  for (uint16_t i = 0; i < n; i++)
  {
    const uint8_t b = s_rx_dma[i];
    uint16_t next = ring_next(s_rx_wr);
    if (next != s_rx_rd)
    {
      s_rx_ring[s_rx_wr] = b;
      s_rx_wr = next;
    }
  }

  /* Restart RX-to-idle DMA for next burst. */
  if (s_rx_mode == APP_COM_RX_MODE_IDLE_DMA && s_uart->hdmarx != NULL)
  {
    (void)HAL_UARTEx_ReceiveToIdle_DMA(s_uart, s_rx_dma, (uint16_t)APP_COM_RX_DMA_SIZE);
    __HAL_DMA_DISABLE_IT(s_uart->hdmarx, DMA_IT_HT);
  }
  else
  {
    (void)HAL_UARTEx_ReceiveToIdle_IT(s_uart, s_rx_dma, (uint16_t)APP_COM_RX_DMA_SIZE);
  }
}

void AppCom_OnUartError(UART_HandleTypeDef *huart)
{
  if (s_uart == NULL || huart != s_uart)
  {
    return;
  }

  /* Try to recover by restarting RX. */
  (void)HAL_UART_AbortReceive_IT(s_uart);
  (void)HAL_UART_AbortReceive(s_uart);

  if (s_rx_mode == APP_COM_RX_MODE_IDLE_DMA && s_uart->hdmarx != NULL)
  {
    if (HAL_UARTEx_ReceiveToIdle_DMA(s_uart, s_rx_dma, (uint16_t)APP_COM_RX_DMA_SIZE) == HAL_OK)
    {
      __HAL_DMA_DISABLE_IT(s_uart->hdmarx, DMA_IT_HT);
      goto rx_ok;
    }
    /* DMA path failed, degrade. */
    s_rx_mode = APP_COM_RX_MODE_IDLE_IT;
  }

  if (s_rx_mode == APP_COM_RX_MODE_IDLE_IT)
  {
    if (HAL_UARTEx_ReceiveToIdle_IT(s_uart, s_rx_dma, (uint16_t)APP_COM_RX_DMA_SIZE) == HAL_OK)
    {
      goto rx_ok;
    }
    /* Idle IT failed, degrade to byte mode. */
    s_rx_mode = APP_COM_RX_MODE_BYTE;
  }

  (void)HAL_UART_Receive_IT(s_uart, &s_rx_byte, 1);

rx_ok:

  /* Also recover TX if it got stuck. */
  (void)HAL_UART_AbortTransmit_IT(s_uart);
  s_tx_busy = 0;
  tx_kick();
}

void AppCom_Poll(void)
{
  while (s_rx_rd != s_rx_wr)
  {
    uint8_t b = s_rx_ring[s_rx_rd];
    s_rx_rd = ring_next(s_rx_rd);

    if (b == '\n')
    {
      s_line[s_line_len] = 0;
      handle_line(s_line);
      s_line_len = 0;
      continue;
    }

    if (b == '\r')
    {
      continue;
    }

    if (s_line_len + 1u < APP_COM_LINE_MAX)
    {
      s_line[s_line_len++] = (char)b;
    }
    else
    {
      /* Line too long: reset. */
      s_line_len = 0;
    }
  }
}
