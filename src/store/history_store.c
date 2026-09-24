/*
 * history_store.c — Isolated SQLite store for the Context History Graph.
 *
 * Kept strictly separate from the codebase graph (different .db file).
 */
#include "store/history_store.h"
#include "sqlite3.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct cbm_history_store {
    sqlite3 *db;
    sqlite3_stmt *stmt_append;
};

/* ── Open / Close ──────────────────────────────────────────────── */

cbm_history_store_t *cbm_history_store_open(const char *db_path) {
    if (!db_path || db_path[0] == '\0') return NULL;

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(db_path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "history_store: cannot open '%s': %s\n",
                db_path, db ? sqlite3_errmsg(db) : "unknown");
        if (db) sqlite3_close(db);
        return NULL;
    }

    /* Performance + safety pragmas — ignore errors (best-effort). */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;",     NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;",   NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;",      NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;",    NULL, NULL, NULL);

    cbm_history_store_t *hs = calloc(1, sizeof(cbm_history_store_t));
    if (!hs) {
        fprintf(stderr, "history_store: out of memory allocating store\n");
        sqlite3_close(db);
        return NULL;
    }
    hs->db = db;
    return hs;
}

struct sqlite3 *cbm_history_store_get_db(cbm_history_store_t *hs) {
    return hs ? hs->db : NULL;
}

void cbm_history_store_close(cbm_history_store_t *hs) {
    if (!hs) return;
    if (hs->stmt_append) {
        sqlite3_finalize(hs->stmt_append);
        hs->stmt_append = NULL;
    }
    if (hs->db) {
        sqlite3_close(hs->db);
        hs->db = NULL;
    }
    free(hs);
}

/* ── Schema ────────────────────────────────────────────────────── */

int cbm_history_store_init_schema(cbm_history_store_t *hs) {
    if (!hs || !hs->db) return CBM_HISTORY_ERR;

    const char *schema =
        "CREATE TABLE IF NOT EXISTS session_events ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  project      TEXT    NOT NULL,"
        "  session_id   TEXT    NOT NULL,"
        "  thread_id    TEXT    NOT NULL,"
        "  timestamp_ms INTEGER NOT NULL,"
        "  event_type   TEXT    NOT NULL,"
        "  payload_json TEXT    NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_events_session"
        "  ON session_events(session_id, timestamp_ms);"
        "CREATE INDEX IF NOT EXISTS idx_events_project"
        "  ON session_events(project, timestamp_ms);";

    char *errmsg = NULL;
    int rc = sqlite3_exec(hs->db, schema, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "history_store: schema init error: %s\n",
                errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return CBM_HISTORY_ERR;
    }
    return CBM_HISTORY_OK;
}

/* ── Append ────────────────────────────────────────────────────── */

int cbm_history_append_event(cbm_history_store_t *hs,
                             const char *project,
                             const char *session_id,
                             const char *thread_id,
                             int64_t timestamp_ms,
                             const char *event_type,
                             const char *payload_json) {
    if (!hs || !hs->db) return CBM_HISTORY_ERR;
    if (!project || !session_id || !thread_id || !event_type || !payload_json) {
        fprintf(stderr, "history_store: append called with NULL argument\n");
        return CBM_HISTORY_ERR;
    }

    /* Prepare statement once; reuse across calls (SQLITE_PREPARE_PERSISTENT
     * was added in SQLite 3.20.0 — guard for older builds). */
    if (!hs->stmt_append) {
        const char *sql =
            "INSERT INTO session_events"
            " (project, session_id, thread_id, timestamp_ms, event_type, payload_json)"
            " VALUES (?, ?, ?, ?, ?, ?);";

#ifdef SQLITE_PREPARE_PERSISTENT
        int rc = sqlite3_prepare_v3(hs->db, sql, -1,
                                    SQLITE_PREPARE_PERSISTENT,
                                    &hs->stmt_append, NULL);
#else
        int rc = sqlite3_prepare_v2(hs->db, sql, -1, &hs->stmt_append, NULL);
#endif
        if (rc != SQLITE_OK) {
            fprintf(stderr, "history_store: prepare error: %s\n",
                    sqlite3_errmsg(hs->db));
            return CBM_HISTORY_ERR;
        }
    }

    sqlite3_reset(hs->stmt_append);
    sqlite3_clear_bindings(hs->stmt_append);

    sqlite3_bind_text (hs->stmt_append, 1, project,      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (hs->stmt_append, 2, session_id,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (hs->stmt_append, 3, thread_id,    -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(hs->stmt_append, 4, timestamp_ms);
    sqlite3_bind_text (hs->stmt_append, 5, event_type,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (hs->stmt_append, 6, payload_json, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(hs->stmt_append);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "history_store: insert error: %s\n",
                sqlite3_errmsg(hs->db));
        return CBM_HISTORY_ERR;
    }
    return CBM_HISTORY_OK;
}

/* ── Query ─────────────────────────────────────────────────────── */

int cbm_history_query_events(cbm_history_store_t *hs,
                             const char *project,
                             const char *thread_id,
                             const char *event_type,
                             char ***out_payloads,
                             int *out_count) {
    if (!hs || !hs->db || !project || !event_type || !out_payloads || !out_count) return CBM_HISTORY_ERR;
    *out_payloads = NULL;
    *out_count = 0;

    const char *sql;
    if (thread_id) {
        sql = "SELECT payload_json FROM session_events WHERE project = ? AND thread_id = ? AND event_type = ? ORDER BY timestamp_ms DESC LIMIT 100;";
    } else {
        sql = "SELECT payload_json FROM session_events WHERE project = ? AND event_type = ? ORDER BY timestamp_ms DESC LIMIT 100;";
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(hs->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return CBM_HISTORY_ERR;

    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
    if (thread_id) {
        sqlite3_bind_text(stmt, 2, thread_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, event_type, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_text(stmt, 2, event_type, -1, SQLITE_TRANSIENT);
    }

    int cap = 4;
    int count = 0;
    char **arr = malloc(cap * sizeof(char*));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            arr = realloc(arr, cap * sizeof(char*));
        }
        const char *payload = (const char *)sqlite3_column_text(stmt, 0);
        arr[count] = strdup(payload ? payload : "{}");
        count++;
    }

    sqlite3_finalize(stmt);
    
    *out_payloads = arr;
    *out_count = count;
    return CBM_HISTORY_OK;
}

int cbm_history_query_thread_timeline(cbm_history_store_t *hs,
                                      const char *project,
                                      const char *thread_id,
                                      char ***out_types,
                                      char ***out_payloads,
                                      int *out_count) {
    if (!hs || !hs->db || !project || !thread_id || !out_types || !out_payloads || !out_count) return CBM_HISTORY_ERR;
    *out_types = NULL;
    *out_payloads = NULL;
    *out_count = 0;

    const char *sql = "SELECT event_type, payload_json FROM session_events WHERE project = ? AND thread_id = ? ORDER BY timestamp_ms ASC;";
    
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(hs->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return CBM_HISTORY_ERR;

    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, thread_id, -1, SQLITE_TRANSIENT);

    int cap = 4;
    int count = 0;
    char **arr_types = malloc(cap * sizeof(char*));
    char **arr_payloads = malloc(cap * sizeof(char*));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            arr_types = realloc(arr_types, cap * sizeof(char*));
            arr_payloads = realloc(arr_payloads, cap * sizeof(char*));
        }
        const char *type = (const char *)sqlite3_column_text(stmt, 0);
        const char *payload = (const char *)sqlite3_column_text(stmt, 1);
        arr_types[count] = strdup(type ? type : "UNKNOWN");
        arr_payloads[count] = strdup(payload ? payload : "{}");
        count++;
    }

    sqlite3_finalize(stmt);

    *out_types = arr_types;
    *out_payloads = arr_payloads;
    *out_count = count;
    return CBM_HISTORY_OK;
}

int cbm_history_thread_exists(cbm_history_store_t *hs,
                              const char *project,
                              const char *thread_id) {
    if (!hs || !hs->db || !project || !thread_id) return -1;

    const char *sql =
        "SELECT 1 FROM session_events WHERE project = ? AND thread_id = ? LIMIT 1;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(hs->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, project,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, thread_id, -1, SQLITE_TRANSIENT);

    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = 1;
    }
    sqlite3_finalize(stmt);
    return found;
}

