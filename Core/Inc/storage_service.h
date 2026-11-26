#pragma once
#include "storage.h"
/* Single flash/data worker owner; mount may scan retained data and yields via
 * the supplied I/O callback. Step services bounded record/command/deletion work. */
int storage_service_init(const storage_io *io);
void storage_service_step(void);
const storage *storage_service_state(void);
int storage_service_debug_last(uint8_t out[80]);
