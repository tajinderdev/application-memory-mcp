/*
 * history_store.h — Isolated SQLite store for engineering history,
 * independent of the codebase and database knowledge graphs.
 *
 * Thread safety: one store handle per thread.
 */
#ifndef CBM_HISTORY_STORE_H
#define CBM_HISTORY_STORE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct cbm_history_store cbm_history_store_t;

#define CBM_HISTORY_OK 0
#define CBM_HISTORY_ERR (-1)
#define CBM_HISTORY_NOT_FOUND (-2)

/* Open or create a history store database at the given filesystem path. */
cbm_history_store_t *cbm_history_store_open(const char *db_path);

/* Get the sqlite3 handle. */
struct sqlite3 *cbm_history_store_get_db(cbm_history_store_t *hs);

/* Close the history store handle. */
void cbm_history_store_close(cbm_history_store_t *hs);

/* Initialize the database schema (tables, indices). */
int cbm_history_store_init_schema(cbm_history_store_t *hs);

/* Append a new event to the history timeline.
 * project: Current workspace/project ID
 * session_id / thread_id: ThreadWeaver identifiers
 * timestamp_ms: Unix epoch timestamp
 * event_type: E.g., "USER_INPUT", "PLANNER_RESPONSE", "FILE_DIFF", "COMMAND"
 * payload_json: The raw JSON object associated with this event.
 */
int cbm_history_append_event(cbm_history_store_t *hs,
                             const char *project,
                             const char *session_id,
                             const char *thread_id,
                             int64_t timestamp_ms,
                             const char *event_type,
                             const char *payload_json);
/* Query history events.
 * Returns an array of heap-allocated JSON payload strings.
 * out_count is set to the number of returned events.
 * The caller must free each string and the array itself.
 * If thread_id is NULL, queries all threads in the project.
 */
int cbm_history_query_events(cbm_history_store_t *hs,
                             const char *project,
                             const char *thread_id,
                             const char *event_type,
                             char ***out_payloads,
                             int *out_count);

/* Fetch the timeline of events for a specific thread, ordered by timestamp ascending.
 * Returns arrays of event types and payloads. 
 */
int cbm_history_query_thread_timeline(cbm_history_store_t *hs,
                                      const char *project,
                                      const char *thread_id,
                                      char ***out_types,
                                      char ***out_payloads,
                                      int *out_count);

/* Returns 1 if the thread has already been ingested into the history store,
 * 0 if not found, -1 on error. Used for idempotency in index_context. */
int cbm_history_thread_exists(cbm_history_store_t *hs,
                              const char *project,
                              const char *thread_id);

#endif
