#pragma once
#include "records.h"

#define COMPACT_SAMPLES_PER_PAGE 8U
/* DRP2 compact data page (256 bytes, all multi-byte integers little-endian).
 *
 * Bytes    Header field
 *  0–3    "DRP2" magic identifies the page format.
 *  4      Format version, currently 2.
 *  5      Number of populated sample slots (1–8 on a committed page).
 *  6      Trigger reason mask (1 acceleration, 2 freefall, 4 barometer).
 *  7      Zero-based index of the trigger sample. Zero when the mask is zero.
 *  8–11   Sequence number of the first sample.
 * 12–19   Acquisition timestamp of the first sample in microseconds.
 * 20–23   ID of the physical session that owns this page.
 *
 * Bytes 24–247 hold eight 28-byte slots. Each stores raw sensor data and
 * 16-bit sequence and millisecond deltas from the first sample. Bytes 248–251
 * hold the CRC over the header and slots. Bytes 252–255 hold the commit word.
 * Calibration and pressure reference live in the session's DLH1 header.
 * See docs/storage-integration.md for the full byte layout and an example.
 */
void compact_page_init(uint8_t page[256], uint32_t session);
/* Return 1 on append. A full page or out-of-range delta is left unchanged so
 * the caller can flush it and retry the sample in a fresh page. */
int compact_page_add(uint8_t page[256], const logger_record *raw, unsigned trigger);
int compact_page_valid(const uint8_t page[256], uint32_t session);
/* Decode a slot after the caller has validated the page. */
int compact_page_raw(const uint8_t page[256], unsigned sample, logger_record *raw);
void compact_page_seal(uint8_t page[256]);
uint64_t record_timestamp(const logger_record *record);
