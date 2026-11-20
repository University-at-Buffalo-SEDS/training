#pragma once
#include "stm32g4xx_hal.h"
#include "tx_api.h"
#include <stdint.h>
#include <stddef.h>


/* 1. General Descriptions, 2. Features - W25Q32JV Datasheet  */
#define W25Q_PAGE_SIZE              256U
#define W25Q_SECTOR_SIZE            4096U
#define W25Q_BLOCK32_SIZE           (32U * 1024U)
#define W25Q_BLOCK64_SIZE           (64U * 1024U)
#define W25Q_CAPACITY_BYTES         (4U * 1024U * 1024U)          /* 4MB */
#define W25Q_LAST_ADDRESS           (W25Q_CAPACITY_BYTES - 1U)    /* 0x3FFFFF */

/* 8.1.2 - W25Q32JV Datasheet */
#define W25Q_CMD_WRITE_ENABLE       0x06U
#define W25Q_CMD_WRITE_DISABLE      0x04U
#define W25Q_CMD_READ_STATUS1       0x05U
#define W25Q_CMD_READ_DATA          0x03U
#define W25Q_CMD_PAGE_PROGRAM       0x02U
#define W25Q_CMD_SECTOR_ERASE       0x20U
#define W25Q_CMD_BLOCK32_ERASE      0x52U
#define W25Q_CMD_BLOCK64_ERASE      0xD8U
#define W25Q_CMD_CHIP_ERASE         0xC7U
#define W25Q_CMD_JEDEC_ID           0x9FU
#define W25Q_CMD_RELEASE_PD         0xABU
#define W25Q_READ_CHUNK             65535U    /* HAL Limit - maximum value of an unsigned 16-bit integer (uint16_t)*/

/* 7.1.1 BUSY, 7.1.2 WEL - W25Q32JV Datasheet */
#define W25Q_SR1_BUSY               0x01U /* S0 = bit 0 -> 1<<0 */
#define W25Q_SR1_WEL                0x02U /* S1 = bit 1 -> 1<<1 */

/* 8.1.1 - W25Q32JV Datasheet 
JEDEC ID for W25Q32JV 
EFh Manufacturer ID
40h Memory Type 
16h Capacity */
#define W25Q32JV_JEDEC_ID           0xEF4016U 

#define W25Q_T_PP_MAX_MS        3U
#define W25Q_T_SE_MAX_MS        400U
#define W25Q_T_PUW_MIN_MS       5U 
#define W25Q_SPI_TIMEOUT_MS     5U 
#define W25Q_LOCK_TIMEOUT_MS    5U
#define W25Q_VERIFY_CHUNK 64U
#define W25Q_READ_CHUNK 65535U
#define MS_TO_TICKS(ms) ((ULONG)(((ULONG)(ms) * TX_TIMER_TICKS_PER_SECOND + 999UL) / 1000UL))

typedef enum {
    W25Q_OK = 0,
    W25Q_ERR_PARAM,
    W25Q_ERR_TIMEOUT,
    W25Q_ERR_HAL,
    W25Q_ERR_ID,
    W25Q_ERR_LOCK,
    W25Q_ERR_WEL,
    W25Q_ERR_VERIFY,
} w25q_status_t;

typedef struct {
    SPI_HandleTypeDef *hspi;
    GPIO_TypeDef      *cs_port;
    uint16_t           cs_pin;
    uint8_t            jedec_id[3];  /* captured at init for status reporting */
} w25q_t;

/* Pulls CS high, optionally verifies JEDEC ID */
w25q_status_t w25q_init(w25q_t *dev, SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin);

/* Read `len` bytes from `addr` into `buf` (uses 0x03 Read Data) */
w25q_status_t w25q_read(w25q_t *dev, uint32_t addr, uint8_t *buf, size_t len);

/* Raw page program. Caller must have erased the target and must NOT cross a page boundary
 * len in 1..256 - SDD 7.1 handles the split */
w25q_status_t w25q_page_program(w25q_t *dev, uint32_t addr, const uint8_t *buf, size_t len);

/* Raw 4KB sector erase - SDD 7.2: only maintenance/deletion calls this */
w25q_status_t w25q_sector_erase(w25q_t *dev, uint32_t addr);

w25q_status_t w25q_read_jedec_id(w25q_t *dev, uint8_t id[3]);
