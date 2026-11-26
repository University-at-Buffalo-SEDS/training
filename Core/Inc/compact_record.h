#pragma once
#include "records.h"

#define COMPACT_SAMPLES_PER_PAGE 8U
/* Lossless sensor counts, ADCs, validity, errors, age and timing. Engineering
 * values are regenerated with the session's recorded factory/config metadata. */
void compact_page_init(uint8_t page[256], uint32_t session);
int compact_page_add(uint8_t page[256], const logger_record *raw, unsigned trigger);
int compact_page_valid(const uint8_t page[256], uint32_t session);
int compact_page_raw(const uint8_t page[256], unsigned sample, logger_record *raw);
void compact_page_seal(uint8_t page[256]);
uint64_t record_timestamp(const logger_record *record);
