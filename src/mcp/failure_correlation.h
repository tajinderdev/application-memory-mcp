/*
 * failure_correlation.h — Engine to correlate failures with past codebase edits.
 */
#ifndef CBM_FAILURE_CORRELATION_H
#define CBM_FAILURE_CORRELATION_H

#include "store/history_store.h"
#include <stdint.h>

/*
 * Scans a ThreadWeaver transcript payload for tool execution failures 
 * and user-reported stack traces.
 * Cross-references these failures with prior CHANGE_CORRELATION events
 * within the same thread to establish root-cause correlations.
 * Appends a FAILURE_CORRELATION event to the history_store.
 *
 * Returns 0 on success, or -1 on error.
 */
int cbm_correlate_failure_event(cbm_history_store_t *history_store,
                                const char *project,
                                const char *session_id,
                                const char *thread_id,
                                int64_t timestamp_ms,
                                const char *transcript_json);

#endif /* CBM_FAILURE_CORRELATION_H */
