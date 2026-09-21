/*
 * correction_memory.h — Engine to detect and synthesize developer corrections.
 */
#ifndef CBM_CORRECTION_MEMORY_H
#define CBM_CORRECTION_MEMORY_H

#include "store/history_store.h"
#include <stdint.h>

/*
 * Analyzes the recent history of a thread to detect developer corrections.
 * A correction is defined as a sequence:
 * CHANGE_CORRELATION -> FAILURE_CORRELATION -> CHANGE_CORRELATION on the same file.
 * 
 * If a correction is detected, it logs a CORRECTION_MEMORY event.
 * It also checks past history across all threads for identical corrections
 * and if >3 are found, promotes it to an ENGINEERING_PATTERN event.
 *
 * Returns 0 on success, or -1 on error.
 */
int cbm_synthesize_corrections(cbm_history_store_t *history_store,
                               const char *project,
                               const char *session_id,
                               const char *thread_id,
                               int64_t timestamp_ms);

#endif /* CBM_CORRECTION_MEMORY_H */
