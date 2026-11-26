#pragma once
#include <stdint.h>
typedef struct { int unused; } SPI_HandleTypeDef;
typedef struct { int unused; } GPIO_TypeDef;
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
#define GPIOB ((void *)1)
#define GPIO_PIN_11 (1u << 11)
#define GPIO_PIN_12 (1u << 12)
#define GPIO_PIN_13 (1u << 13)
#define GPIO_PIN_14 (1u << 14)
#define GPIO_PIN_RESET 0
#define GPIO_PIN_SET 1
void HAL_GPIO_WritePin(void *, uint16_t, int);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
uint32_t HAL_GetTick(void);
uint32_t HAL_GetUIDw0(void);
uint32_t HAL_GetUIDw1(void);
uint32_t HAL_GetUIDw2(void);
