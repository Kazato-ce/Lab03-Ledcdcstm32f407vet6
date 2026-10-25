#ifndef APP_CDC_H
#define APP_CDC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void APP_CDC_Init(void);
void APP_CDC_Process(void);
void APP_CDC_OnReceive(const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
