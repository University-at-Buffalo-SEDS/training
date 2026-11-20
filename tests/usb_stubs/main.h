#pragma once
#include <stdint.h>
#define USB 0x1000U
#define PCD_SNG_BUF 0U
#define HAL_OK 0
typedef int PCD_HandleTypeDef;
int HAL_PCDEx_PMAConfig(PCD_HandleTypeDef *, unsigned, unsigned, unsigned);
int HAL_PCD_Start(PCD_HandleTypeDef *);
uint32_t HAL_GetTick(void);
uint32_t HAL_GetUIDw0(void);
uint32_t HAL_GetUIDw1(void);
uint32_t HAL_GetUIDw2(void);
void Error_Handler(void);
