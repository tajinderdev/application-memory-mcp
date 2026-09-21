/*
 * change_correlation.h — Engine to bridge historical diff events 
 * with the codebase memory AST graph.
 */
#ifndef CBM_CHANGE_CORRELATION_H
#define CBM_CHANGE_CORRELATION_H

#include "store/store.h"
#include "store/history_store.h"

#include <stdint.h>
#include <stdbool.h>

/*
 * Scans a ThreadWeaver transcript payload for file modification tool calls.
 * For any detected changes, it queries the codebase_store to identify the 
 * exact AST nodes modified and their dependencies, then appends a 
 * CHANGE_CORRELATION event to the history_store.
 *
 * codebase_store:   The main codebase graph.
 * history_store:    The historical timeline store.
 * project:          The current project name.
 * session_id:       The ThreadWeaver session.
 * thread_id:        The ThreadWeaver thread.
 * timestamp_ms:     The event time.
 * transcript_json:  The raw transcript payload fetched from ThreadWeaver.
 *
 * Returns 0 on success, or -1 on error.
 */
int cbm_correlate_transcript_event(cbm_store_t *codebase_store,
                                   cbm_history_store_t *history_store,
                                   const char *project,
                                   const char *session_id,
                                   const char *thread_id,
                                   int64_t timestamp_ms,
                                   const char *transcript_json);

#endif /* CBM_CHANGE_CORRELATION_H */
