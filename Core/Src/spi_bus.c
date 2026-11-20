#include "spi_bus.h"

extern SPI_HandleTypeDef hspi1;
/* Shared by sensor transactions and the W25Q flash driver. */
TX_MUTEX spi1_mutex;
static const uint16_t pins[] = {accel_CS_Pin, gyro_CS_Pin, baro_CS_Pin, flash_CS_Pin};

UINT spi_bus_init(void)
{
    return tx_mutex_create(&spi1_mutex, "SPI1", TX_INHERIT);
}

static HAL_StatusTypeDef transfer(spi_device device, const uint8_t *header,
                                 uint16_t header_size, uint8_t *data, uint16_t size)
{
    if ((unsigned)device >= sizeof(pins) / sizeof(pins[0]) || !header || !header_size ||
        (size && !data)) return HAL_ERROR;
    if (tx_mutex_get(&spi1_mutex, TX_TIMER_TICKS_PER_SECOND / 10 + 1) != TX_SUCCESS)
        return HAL_TIMEOUT;
    /* All four selects are on GPIOB on this board. */
    HAL_GPIO_WritePin(GPIOB, accel_CS_Pin | gyro_CS_Pin | baro_CS_Pin | flash_CS_Pin,
                      GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, pins[device], GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi1, (uint8_t *)header, header_size, 10);
    if (status == HAL_OK && size) status = HAL_SPI_Receive(&hspi1, data, size, 10);
    HAL_GPIO_WritePin(GPIOB, pins[device], GPIO_PIN_SET);
    tx_mutex_put(&spi1_mutex);
    return status;
}

HAL_StatusTypeDef spi_bus_read(spi_device device, const uint8_t *header,
                              uint16_t header_size, uint8_t *data, uint16_t size)
{
    return transfer(device, header, header_size, data, size);
}

HAL_StatusTypeDef spi_bus_write(spi_device device, const uint8_t *data, uint16_t size)
{
    return transfer(device, data, size, NULL, 0);
}
