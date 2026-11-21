#pragma once
#include "main.h"
#include "tx_api.h"

typedef enum { SPI_ACCEL, SPI_GYRO, SPI_BARO, SPI_FLASH } spi_device;
UINT spi_bus_init(void);
/* Thread context only. Lock covers command, dummy bytes and payload together.
 * Storage uses this same API; release it while flash is internally busy. */
HAL_StatusTypeDef spi_bus_read(spi_device device, const uint8_t *header,
                              uint16_t header_size, uint8_t *data, uint16_t size);
HAL_StatusTypeDef spi_bus_write(spi_device device, const uint8_t *data, uint16_t size);
