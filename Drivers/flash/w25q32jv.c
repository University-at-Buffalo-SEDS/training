#include "w25q32jv.h"
#include "main.h"
#include "spi_bus.h"
#include "stm32g4xx_hal_def.h"
#include "tx_api.h"         
#include <stdint.h>
#include <string.h>

/* Read the first status register 
 * @param dev - pointer to the W25Q32JV device
 * @param sr - Buffer to store the status register value
 * @return - W25Q_OK if the status is read successfully, W25Q_ERR_LOCK if the bus is not available, or W25Q_ERR_HAL otherwise
 */
static w25q_status_t read_sr1(w25q_t *dev, uint8_t *sr)
{

    uint8_t cmd = W25Q_CMD_READ_STATUS1;
    HAL_StatusTypeDef status;
    status = spi_bus_read(SPI_FLASH, &cmd, 1, sr, 1);
    if (status == HAL_OK) {
        return W25Q_OK;
    } else {
        return W25Q_ERR_HAL;
    }
}

/* Waits until the device is not busy or timeout occurs
 * The chip ignores every instruction except Read Status while BUSY is set, so call this before any command
 * The bus is released between polls
 * @param dev - pointer to the W25Q32JV device
 * @param timeout_ms - Timeout in milliseconds
 * @return - W25Q_OK if the device is not busy, W25Q_ERR_TIMEOUT if timeout occurs, or the error code of the failed status read otherwise
 */
static w25q_status_t wait_not_busy(w25q_t *dev, uint32_t timeout_ms)
{
    uint8_t sr;
    w25q_status_t st;
    uint32_t t0 = HAL_GetTick();
    /* While runtime is less than timeout, read status register 1, check if status is OK */
    do {
        st = read_sr1(dev, &sr);
        if (st != W25Q_OK) {
            return st;
        }
        if (!(sr & W25Q_SR1_BUSY)) {
            return W25Q_OK;
        }
        /* Places the current thread at the end of the ready list for its priority */
        tx_thread_relinquish();
    } while ((HAL_GetTick() - t0) < timeout_ms);
    return W25Q_ERR_TIMEOUT;
}
/* Checks if a given address range is valid for the W25Q32JV flash chip
 * @param addr - Starting address
 * @param len - Length of the range
 * @return - true if the range is valid, false otherwise
 */
static inline int w25q_range_ok(uint32_t addr, size_t len)
{
    if (addr > W25Q_CAPACITY_BYTES) {
        return 0; // false 
    }
    if (len > (size_t)(W25Q_CAPACITY_BYTES - addr)) {
        return 0; // false 
    }
    return 1; // true 
}

/* Sends the Write Enable (06h) instruction to set the WEL bit
 * Required prior to any Page Program, Sector/Block/Chip Erase, Write Status Register, etc... command
 * Assumes the caller already holds the SPI bus lock
 * Drives CS high immediately after transmitting 06h to latch the instruction
 * Does NOT poll the Status Register to verify WEL
 * @param dev - pointer to the W25Q32JV device
 * @return - W25Q_OK if the command is sent successfully, or W25Q_ERR_HAL otherwise
 */
static w25q_status_t write_enable_cmd(w25q_t *dev)
{
    uint8_t cmd = W25Q_CMD_WRITE_ENABLE;
    HAL_StatusTypeDef st;
    st = spi_bus_write(SPI_FLASH, &cmd,
                       1); // Send Write Enable command, command size 1 byte
    if (st == HAL_OK) {
        return W25Q_OK;
    } else {
        return W25Q_ERR_HAL;
    }
}

/* Acquires the SPI bus, issues the Write Enable (06h) command
 * Reads Status Register 1 (SR1) to confirm that the WEL bit was successfully latched
 * Releases the bus before returning status 
 * WEL clears itself after every program or erase, so this is needed before each one (8.2 Write Enable - W25Q32JV Datasheet)
 * @param dev - pointer to the W25Q32JV device
 * @return - W25Q_OK if the command is sent successfully and WEL is set, W25Q_ERR_WEL if WEL is not set, or another error code otherwise
 */
static w25q_status_t write_enable(w25q_t *dev)
{
    w25q_status_t st;
    st = write_enable_cmd(dev);
    if (st == W25Q_OK) {
        uint8_t sr = 0; // clear status register
        st = read_sr1(dev, &sr);
        if (st == W25Q_OK && !(sr & W25Q_SR1_WEL)) {
            st = W25Q_ERR_WEL;
        }
    }
    return st;
}

/* Encodes an address into a 3-byte array in MSB-first order
 * This is a position inside the external flash chip (0x000000 to 0x3FFFFF), sent after the command byte
 * No range check here, only the low 24 bits are kept, so check with w25q_range_ok first
 * @param out - Output buffer of at least 3 bytes
 * @param addr - Address to encode
  */
static inline void encode_addr_msb_first(uint8_t out[3], uint32_t addr)
{
    out[0] = (uint8_t)(addr >> 16);
    out[1] = (uint8_t)(addr >> 8);
    out[2] = (uint8_t)(addr);
}

/* Read into buf while the caller already holds the bus 
 * One (03h) transaction, no dummy byte for this command (8.2 Read Data - W25Q32JV Datasheet)
 * @param dev - pointer to the W25Q32JV device
 * @param addr - First byte address
 * @param buf - Buffer to store the data
 * @param len - Number of bytes, at most the HAL limit (65535), callers use W25Q_READ_CHUNK or less
 * @return - W25Q_OK if the bytes are read successfully, or W25Q_ERR_HAL otherwise
 */
static w25q_status_t read(w25q_t *dev, uint32_t addr, uint8_t *buf, size_t len)
{
    /* Header for the read command - hdr[0] command, hdr[1-3] address */
    uint8_t hdr[4];
    HAL_StatusTypeDef st;

    hdr[0] = W25Q_CMD_READ_DATA;
    encode_addr_msb_first(&hdr[1], addr);

    st = spi_bus_read(SPI_FLASH, &hdr[0], 4, buf, len);
    if (st == HAL_OK) {
        return W25Q_OK;
    } else {
        return W25Q_ERR_HAL;
    }

}

/* Read the JEDEC ID 
* @param dev - pointer to the W25Q32JV device
* @param id - Buffer to store the 3-byte JEDEC ID
* @return - W25Q_OK if the ID is read successfully, or W25Q_ERR_PARAM otherwise
*/
w25q_status_t w25q_read_jedec_id(w25q_t *dev, uint8_t id[3])
{
    uint8_t cmd = W25Q_CMD_JEDEC_ID; 
    HAL_StatusTypeDef st;
    if (!dev || !id) { 
        return W25Q_ERR_PARAM;
    }

    st = spi_bus_read(SPI_FLASH, &cmd, 1, id, 3);
    if (st == HAL_OK) {
        return W25Q_OK;
    } else {
        return W25Q_ERR_HAL;
    }
    
}

/* Fills in the device, waits tPUW and checks the ID
 * CS is pulled high first so the chip is not selected during the wait
 * A wrong ID is rejected, the geometry is never guessed (SDD 3)
 * tPUW - Time Delay Before Write Instruction, min 5ms (9.3 - W25Q32JV Datasheet)
 * @param dev - pointer to the W25Q32JV device
 * @param hspi - SPI1 handle
 * @param cs_port - GPIO port of the flash chip select
 * @param cs_pin - GPIO pin of the flash chip select
 * @return - W25Q_OK if the ID is EF 40 16, W25Q_ERR_PARAM for a null pointer, W25Q_ERR_ID if the ID differs, or another error code otherwise
 */
w25q_status_t w25q_init(w25q_t *dev, SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
    uint8_t id[3]; /* Buffer for JEDEC ID */
    w25q_status_t st;

    /* Guard against NULL pointers */
    if (!dev || !hspi || !cs_port) { 
        return W25Q_ERR_PARAM; 
    }

    dev -> hspi    = hspi;
    dev -> cs_port = cs_port;  
    dev -> cs_pin  = cs_pin; 

    HAL_Delay(W25Q_T_PUW_MIN_MS);   /* tPUW before the first write instruction, needs minimum of 5ms delay (9.3 - W25Q32JV Datasheet) */

    st = w25q_read_jedec_id(dev, id); 
    if (st != W25Q_OK) { 
        return st; 
    }

    dev -> jedec_id[0] = id[0]; /* Manufacturer ID */
    dev -> jedec_id[1] = id[1]; /* Memory Type */
    dev -> jedec_id[2] = id[2]; /* Capacity */

    // Pack the 3-byte JEDEC ID into a 24-bit integer and compare with the expected value
    uint32_t v = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
    if (v != W25Q32JV_JEDEC_ID) { 
        return W25Q_ERR_ID; 
    }

    return W25Q_OK;
}

/* Reads len bytes starting at addr
 * Waits for the chip to be idle first, then reads in W25Q_READ_CHUNK pieces
 * Each piece is its own bus hold, so a long read never holds the mutex for long and never exceeds the HAL size or timeout (SDD 4.2)
 * @param dev - pointer to the W25Q32JV device
 * @param addr - First byte address
 * @param buf - Buffer to store the data
 * @param len - Number of bytes to read
 * @return - W25Q_OK if all bytes are read, W25Q_ERR_PARAM for a bad request, or another error code otherwise
 */
w25q_status_t w25q_read(w25q_t *dev, uint32_t addr, uint8_t *buf, size_t len)
{
    w25q_status_t st;
    /* Validate pointers and verify (addr, addr + len) stays within the 4 MB chip boundary */
    if (!dev || !buf || !w25q_range_ok(addr, len)) { 
        return W25Q_ERR_PARAM; 
    }

    /* Poll Status Register until any prior erase/program finishes, 500ms max for a sector erase */
    st = wait_not_busy(dev, W25Q_T_SE_MAX_MS); //  
    if (st != W25Q_OK) { 
        return st; 
    }

    /* Loop until all requested bytes have been read */
    while (len != 0U) {
        /* Cap each SPI transaction to W25Q_READ_CHUNK bytes to avoid going over HAL limit */
        size_t n = (len > W25Q_READ_CHUNK) ? W25Q_READ_CHUNK : len; 

        /* CS low -> send Read Data (03h) + 24-bit address, for size 'n' -> CS high */
        st = read(dev, addr, buf, n);
        /* Release the bus immediately after reading chunk */
        if (st != W25Q_OK) { 
            return st; 
        }
        /* Advance flash address, buffer pointer, and remaining byte count */
        addr += (uint32_t)n;
        buf  += n;
        len  -= n;
    }
    return W25Q_OK;
}

/* Programs up to one page, then verifies it
 * Order - Validates alignment and bounds, waits for the chip to become idle, enables writing (06h + WEL check), 
 * transmits the Page Program instruction (02h) with a 24-bit address and payload, waits for the internal program cycle (tPP) to complete, 
 * and performs a chunked readback verification
 * Note - Target flash bytes must already be in the erased state (0xFF)
 * NOR flash programming can only clear bits from 1 to 0, never set 0 to 1. (8.2 Page Program - W25Q32JV Datasheet)
 * @param dev - pointer to the W25Q32JV device
 * @param addr - First byte address
 * @param buf - Data to program
 * @param len - Number of bytes, 1 to 256, and the range must not cross a 256-byte page boundary
 * @return - W25Q_OK if programmed and verified, W25Q_ERR_PARAM for a bad request, W25Q_ERR_VERIFY if the readback differs, or another error code otherwise
 */
w25q_status_t w25q_page_program(w25q_t *dev, uint32_t addr, const uint8_t *buf, size_t len)
{
    /* 4-byte command header, hdr[0] page program command (02h), [1-3] 24-bit MSB-first address */
    uint8_t hdr[4];
    w25q_status_t st;
    HAL_StatusTypeDef spistatus;

    /* Check for NULL pointers and valid length */
    if (!dev || !buf || len == 0 || len > W25Q_PAGE_SIZE) { 
        return W25Q_ERR_PARAM; 
    }
    /* Check if the address and length are within the valid range */
    if (!w25q_range_ok(addr, len)) { 
        return W25Q_ERR_PARAM; 
    }
    /* Check if the page boundary is crossed */
    if (((addr & (W25Q_PAGE_SIZE - 1U)) + len) > W25Q_PAGE_SIZE) {
        return W25Q_ERR_PARAM;   /* the chip would wrap inside the page and overwrite earlier bytes */
    }

    /* Page Program max time 10 ms */
    st = wait_not_busy(dev, W25Q_T_PP_MAX_MS); 
    if (st != W25Q_OK) { 
        return st; 
    }
    
    st = write_enable(dev);
    if (st != W25Q_OK) { 
        return st; 
    }

    hdr[0] = W25Q_CMD_PAGE_PROGRAM;
    encode_addr_msb_first(&hdr[1], addr);

    spistatus = spi_bus_write(SPI_FLASH, hdr, 4);
    if (spistatus == HAL_OK) {
        spistatus = spi_bus_write(SPI_FLASH, buf, len);
    }

    if (spistatus != HAL_OK) {
        return W25Q_ERR_HAL;
    }

    /* Page Program max time 10 ms */
    st = wait_not_busy(dev, W25Q_T_PP_MAX_MS);
    if (st != W25Q_OK) { 
        return st; 
    }

    /* Verify - check if bits changed, just because cycle ended dosen't mean it was successful */
    {
        uint8_t chunk[W25Q_VERIFY_CHUNK];
        for (size_t done = 0; done < len; ) {
            size_t m = len - done;
            if (m > W25Q_VERIFY_CHUNK) { 
                m = W25Q_VERIFY_CHUNK; 
            }

            /* Read back the programmed data in chunks and compare with the original buffer */
            st = read(dev, addr + (uint32_t)done, chunk, m);
            if (st != W25Q_OK) { 
                return st; 
            }
            /* compare the readback with the original buffer */
            if (memcmp(chunk, &buf[done], m) != 0) {
                return W25Q_ERR_VERIFY;   /* protected area, or the target was not erased */
            }
            /* advance offset */
            done += m;
        }
    }

    return W25Q_OK;
}

/* Erases one 4KB sector, then checks if it is blank
 * ORDER - wait idle, Write Enable (WEL checked), 20h + address, wait for BUSY to clear, read the whole sector and check for 0xFF
 * Only maintenance/deletion code should call this (SDD 7.2)
 * @param dev - pointer to the W25Q32JV device
 * @param addr - Sector base address, must be a multiple of 4096
 * @return - W25Q_OK if erased and blank, W25Q_ERR_PARAM for a bad address, W25Q_ERR_VERIFY if a byte is not 0xFF, or another error code otherwise
 */
w25q_status_t w25q_sector_erase(w25q_t *dev, uint32_t addr)
{
    /* 4-byte command header, hdr[0] page program command (20h), [1-3] 24-bit MSB-first address */
    uint8_t hdr[4];
    w25q_status_t st;
    HAL_StatusTypeDef spistatus;

    /* Check for NULL pointer */
    if (!dev) {
        return W25Q_ERR_PARAM; 
    }
    /* Check if the address is within the valid range (0x000000 to 0x3FFFFF) */
    if (!w25q_range_ok(addr, 1)) { 
        return W25Q_ERR_PARAM; 
    }

    /* Enforce 4 KB (0x1000) sector alignment, lower 12 address bits must be zero */
    if (addr & (W25Q_SECTOR_SIZE - 1U)) { 
        return W25Q_ERR_PARAM; 
    }

    /* Sector Erase max time is 400 ms */
    st = wait_not_busy(dev, W25Q_T_SE_MAX_MS); 
    if (st != W25Q_OK) { 
        return st; 
    }

    st = write_enable(dev);
    if (st != W25Q_OK) { 
        return st; 
    }

    hdr[0] = W25Q_CMD_SECTOR_ERASE;
    encode_addr_msb_first(&hdr[1], addr);

    spistatus = spi_bus_write(SPI_FLASH, hdr, 4);
    if (spistatus != HAL_OK) {
        return W25Q_ERR_HAL;
    }

    /* Sector Erase max time is 400 ms */
    st = wait_not_busy(dev, W25Q_T_SE_MAX_MS);
    if (st != W25Q_OK) { 
        return st; 
    }

    /* Check if blank, every byte must read 0xFF */
    {
        uint8_t chunk[W25Q_VERIFY_CHUNK];
        for (uint32_t off = 0; off < W25Q_SECTOR_SIZE; off += W25Q_VERIFY_CHUNK) {
            /* Read W25Q_VERIFY_CHUNK bytes at current sector offset */
            st = read(dev, addr + off, chunk, W25Q_VERIFY_CHUNK);
            if (st != W25Q_OK) { 
                return st; 
            }
            /* Scan every byte in the chunk, any non-0xFF byte indicates failure */
            for (uint32_t k = 0; k < W25Q_VERIFY_CHUNK; k++) {
                if (chunk[k] != 0xFFU) { 
                    return W25Q_ERR_VERIFY; 
                }
            }
        }
    }
    return W25Q_OK;
}
