/*
 * event_ingest.h — Service to pull and ingest ThreadWeaver events
 * into the isolated Context History Graph.
 */
#ifndef CBM_EVENT_INGEST_H
#define CBM_EVENT_INGEST_H

#include "store/history_store.h"
#include <stdbool.h>
#include <stdarg.h>

typedef struct cbm_store cbm_store_t;

/*
 * Ingest a ThreadWeaver session into the history store.
 *
 * hs:              Open history store handle.
 * config_path:     Path to threadweaver_api.json.
 * current_project: ID/path of the active project (used for workspace filtering).
 * out_error_msg:   On failure, set to a heap-allocated user-readable error message.
 *                  The caller must free() it. Always NULL on success.
 *
 * Returns 0 on success, -1 on failure (see *out_error_msg for details).
 */
int cbm_ingest_threadweaver_session(cbm_store_t *codebase_store,
                                    cbm_history_store_t *hs,
                                    const char *config_path,
                                    const char *current_project,
                                    char **out_error_msg);

#endif /* CBM_EVENT_INGEST_H */
