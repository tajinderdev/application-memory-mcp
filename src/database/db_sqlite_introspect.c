/*
 * db_sqlite_introspect.c — Native SQLite database introspector.
 *
 * Uses the vendored sqlite3.h directly (no subprocess) to extract
 * tables, columns, primary keys, foreign keys, indexes, and sample
 * data from a local SQLite database file.
 */
#include "database/db_introspect.h"

#include <sqlite3/sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foundation/constants.h"
#include "foundation/mem.h"
#include <yyjson/yyjson.h>

/* ── Internal helpers ──────────────────────────────────────────── */

static char *sdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *d = cbm_malloc(len + SKIP_ONE);
    if (d) {
        memcpy(d, s, len + SKIP_ONE);
    }
    return d;
}

/* ── Column extraction via PRAGMA table_info ────────────────────── */

static int extract_columns(sqlite3 *db, const char *table_name,
                           cbm_db_column_t **out, int *out_count) {
    char sql[CBM_SZ_512];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(\"%s\")", table_name);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        *out = NULL;
        *out_count = 0;
        return -1;
    }

    /* First pass: count rows. */
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        count++;
    }
    sqlite3_reset(stmt);

    if (count == 0) {
        sqlite3_finalize(stmt);
        *out = NULL;
        *out_count = 0;
        return 0;
    }

    cbm_db_column_t *cols = cbm_calloc((size_t)count, sizeof(cbm_db_column_t));
    if (!cols) {
        sqlite3_finalize(stmt);
        return -1;
    }

    int i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < count) {
        /* PRAGMA table_info columns: cid, name, type, notnull, dflt_value, pk */
        cols[i].ordinal = sqlite3_column_int(stmt, 0) + SKIP_ONE; /* 0-based → 1-based */
        cols[i].name = sdup((const char *)sqlite3_column_text(stmt, 1));
        cols[i].data_type = sdup((const char *)sqlite3_column_text(stmt, CBM_SZ_2));
        cols[i].is_nullable = sqlite3_column_int(stmt, CBM_SZ_3) == 0;
        const char *dflt = (const char *)sqlite3_column_text(stmt, CBM_SZ_4);
        cols[i].default_value = dflt ? sdup(dflt) : NULL;
        cols[i].is_pk = sqlite3_column_int(stmt, CBM_SZ_5) > 0;
        cols[i].is_fk = false; /* populated later from FK list */
        cols[i].is_unique = false; /* populated later from index list */
        cols[i].comment = NULL;
        i++;
    }
    sqlite3_finalize(stmt);

    *out = cols;
    *out_count = i;
    return 0;
}

/* ── Foreign key extraction via PRAGMA foreign_key_list ─────────── */

static int extract_foreign_keys(sqlite3 *db, const char *table_name,
                                cbm_db_fk_t **out, int *out_count,
                                cbm_db_column_t *columns, int col_count) {
    char sql[CBM_SZ_512];
    snprintf(sql, sizeof(sql), "PRAGMA foreign_key_list(\"%s\")", table_name);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        *out = NULL;
        *out_count = 0;
        return -1;
    }

    /* Collect FK entries. SQLite foreign_key_list returns one row per
     * FK column with columns: id, seq, table, from, to, on_update, on_delete, match.
     * Multiple rows with the same id belong to the same composite FK. */
    enum { MAX_FKS = 256 };
    cbm_db_fk_t fks[MAX_FKS];
    memset(fks, 0, sizeof(fks));
    int fk_count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int fk_id = sqlite3_column_int(stmt, 0);
        const char *ref_table = (const char *)sqlite3_column_text(stmt, CBM_SZ_2);
        const char *from_col = (const char *)sqlite3_column_text(stmt, CBM_SZ_3);
        const char *to_col = (const char *)sqlite3_column_text(stmt, CBM_SZ_4);
        const char *on_update = (const char *)sqlite3_column_text(stmt, CBM_SZ_5);
        const char *on_delete = (const char *)sqlite3_column_text(stmt, CBM_SZ_6);

        /* Find or create FK entry for this id. */
        int found = -1;
        for (int i = 0; i < fk_count; i++) {
            /* Match by constraint name which encodes the fk_id. */
            char id_str[CBM_SZ_32];
            snprintf(id_str, sizeof(id_str), "fk_%s_%d", table_name, fk_id);
            if (fks[i].constraint_name && strcmp(fks[i].constraint_name, id_str) == 0) {
                found = i;
                break;
            }
        }

        if (found < 0 && fk_count < MAX_FKS) {
            found = fk_count++;
            char name[CBM_SZ_128];
            snprintf(name, sizeof(name), "fk_%s_%d", table_name, fk_id);
            fks[found].constraint_name = sdup(name);
            fks[found].target_table = sdup(ref_table);
            fks[found].target_schema = NULL;
            fks[found].on_update = sdup(on_update);
            fks[found].on_delete = sdup(on_delete);
            fks[found].source_columns = NULL;
            fks[found].source_column_count = 0;
            fks[found].target_columns = NULL;
            fks[found].target_column_count = 0;
        }

        if (found >= 0) {
            /* Append source column. */
            int sc = fks[found].source_column_count;
            fks[found].source_columns = cbm_realloc(
                fks[found].source_columns, (size_t)(sc + 1) * sizeof(char *));
            if (fks[found].source_columns) {
                fks[found].source_columns[sc] = sdup(from_col);
                fks[found].source_column_count = sc + 1;
            }
            /* Append target column. */
            int tc = fks[found].target_column_count;
            fks[found].target_columns = cbm_realloc(
                fks[found].target_columns, (size_t)(tc + 1) * sizeof(char *));
            if (fks[found].target_columns) {
                fks[found].target_columns[tc] = sdup(to_col);
                fks[found].target_column_count = tc + 1;
            }

            /* Mark source columns as FK in the column list. */
            if (from_col && columns) {
                for (int c = 0; c < col_count; c++) {
                    if (columns[c].name && strcmp(columns[c].name, from_col) == 0) {
                        columns[c].is_fk = true;
                    }
                }
            }
        }
    }
    sqlite3_finalize(stmt);

    if (fk_count > 0) {
        *out = cbm_calloc((size_t)fk_count, sizeof(cbm_db_fk_t));
        if (*out) {
            memcpy(*out, fks, (size_t)fk_count * sizeof(cbm_db_fk_t));
        }
    } else {
        *out = NULL;
    }
    *out_count = fk_count;
    return 0;
}

/* ── Index extraction via PRAGMA index_list + index_info ────────── */

static int extract_indexes(sqlite3 *db, const char *table_name,
                           cbm_db_index_t **out, int *out_count,
                           cbm_db_column_t *columns, int col_count) {
    char sql[CBM_SZ_512];
    snprintf(sql, sizeof(sql), "PRAGMA index_list(\"%s\")", table_name);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        *out = NULL;
        *out_count = 0;
        return -1;
    }

    /* Collect index metadata. */
    enum { MAX_INDEXES = 128 };
    cbm_db_index_t idxs[MAX_INDEXES];
    memset(idxs, 0, sizeof(idxs));
    int idx_count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && idx_count < MAX_INDEXES) {
        /* index_list columns: seq, name, unique, origin, partial */
        const char *idx_name = (const char *)sqlite3_column_text(stmt, 1);
        int is_unique = sqlite3_column_int(stmt, CBM_SZ_2);
        const char *origin = (const char *)sqlite3_column_text(stmt, CBM_SZ_3);

        idxs[idx_count].name = sdup(idx_name);
        idxs[idx_count].is_unique = is_unique != 0;
        idxs[idx_count].is_primary = origin && strcmp(origin, "pk") == 0;
        idxs[idx_count].index_type = sdup("BTREE");

        /* Get columns for this index via PRAGMA index_info. */
        char info_sql[CBM_SZ_512];
        snprintf(info_sql, sizeof(info_sql), "PRAGMA index_info(\"%s\")", idx_name);
        sqlite3_stmt *info_stmt = NULL;
        if (sqlite3_prepare_v2(db, info_sql, -1, &info_stmt, NULL) == SQLITE_OK) {
            /* Count columns. */
            int ccnt = 0;
            while (sqlite3_step(info_stmt) == SQLITE_ROW) {
                ccnt++;
            }
            sqlite3_reset(info_stmt);

            if (ccnt > 0) {
                idxs[idx_count].columns = cbm_calloc((size_t)ccnt, sizeof(char *));
                int ci = 0;
                while (sqlite3_step(info_stmt) == SQLITE_ROW && ci < ccnt) {
                    const char *cname = (const char *)sqlite3_column_text(info_stmt, CBM_SZ_2);
                    idxs[idx_count].columns[ci] = sdup(cname);

                    /* Mark unique columns. */
                    if (is_unique && cname && columns) {
                        for (int c = 0; c < col_count; c++) {
                            if (columns[c].name && strcmp(columns[c].name, cname) == 0) {
                                columns[c].is_unique = true;
                            }
                        }
                    }
                    ci++;
                }
                idxs[idx_count].column_count = ci;
            }
            sqlite3_finalize(info_stmt);
        }
        idx_count++;
    }
    sqlite3_finalize(stmt);

    if (idx_count > 0) {
        *out = cbm_calloc((size_t)idx_count, sizeof(cbm_db_index_t));
        if (*out) {
            memcpy(*out, idxs, (size_t)idx_count * sizeof(cbm_db_index_t));
        }
    } else {
        *out = NULL;
    }
    *out_count = idx_count;
    return 0;
}

/* ── Row count estimation ──────────────────────────────────────── */

static int64_t count_rows(sqlite3 *db, const char *table_name) {
    char sql[CBM_SZ_512];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%s\"", table_name);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

/* ── Sample data extraction ────────────────────────────────────── */

static char *extract_sample_rows(sqlite3 *db, const char *table_name,
                                 cbm_db_column_t *columns, int col_count,
                                 int max_rows, int *row_count_out) {
    char sql[CBM_SZ_512];
    snprintf(sql, sizeof(sql), "SELECT * FROM \"%s\" LIMIT %d", table_name, max_rows);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        *row_count_out = 0;
        return NULL;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, arr);

    int rows = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        yyjson_mut_val *row = yyjson_mut_obj(doc);
        int ncols = sqlite3_column_count(stmt);
        for (int c = 0; c < ncols; c++) {
            const char *cname = sqlite3_column_name(stmt, c);

            /* Mask sensitive columns. */
            if (cbm_db_is_sensitive_column(cname)) {
                yyjson_mut_obj_add_str(doc, row, cname, "***REDACTED***");
                continue;
            }

            int ctype = sqlite3_column_type(stmt, c);
            switch (ctype) {
            case SQLITE_INTEGER:
                yyjson_mut_obj_add_int(doc, row, cname, sqlite3_column_int64(stmt, c));
                break;
            case SQLITE_FLOAT:
                yyjson_mut_obj_add_real(doc, row, cname, sqlite3_column_double(stmt, c));
                break;
            case SQLITE_TEXT:
                yyjson_mut_obj_add_str(doc, row, cname,
                                       (const char *)sqlite3_column_text(stmt, c));
                break;
            case SQLITE_NULL:
                yyjson_mut_obj_add_null(doc, row, cname);
                break;
            case SQLITE_BLOB:
                yyjson_mut_obj_add_str(doc, row, cname, "<BLOB>");
                break;
            default:
                break;
            }
        }
        yyjson_mut_arr_add_val(arr, row);
        rows++;
    }
    sqlite3_finalize(stmt);

    *row_count_out = rows;
    size_t json_len = 0;
    char *json = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &json_len);
    yyjson_mut_doc_free(doc);

    /* Copy to cbm_malloc'd buffer so all memory uses the same allocator. */
    if (json) {
        char *copy = cbm_malloc(json_len + SKIP_ONE);
        if (copy) {
            memcpy(copy, json, json_len + SKIP_ONE);
        }
        free(json); /* yyjson uses stdlib free */
        return copy;
    }
    return NULL;
}

/* ── Public: SQLite introspection backend ──────────────────────── */

int cbm_db_introspect_sqlite(const cbm_db_connect_opts_t *opts, cbm_db_result_t *out) {
    if (!opts || !opts->connection_string || !out) {
        if (out) {
            out->error = sdup("invalid arguments for SQLite introspection");
        }
        return -1;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(opts->connection_string, &db,
                             SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) {
        char err[CBM_SZ_512];
        snprintf(err, sizeof(err), "failed to open SQLite database '%s': %s",
                 opts->connection_string, db ? sqlite3_errmsg(db) : "unknown error");
        if (db) {
            sqlite3_close(db);
        }
        out->error = sdup(err);
        return -1;
    }

    out->database_name = sdup(opts->connection_string);
    out->host = sdup("local");
    out->version = sdup(sqlite3_libversion());

    /* SQLite has a single "main" schema. */
    out->schema_count = 1;
    out->schemas = cbm_calloc(1, sizeof(cbm_db_schema_t));
    if (!out->schemas) {
        sqlite3_close(db);
        out->error = sdup("allocation failure");
        return -1;
    }
    out->schemas[0].name = sdup("main");

    /* List tables (excluding sqlite_ internal tables). */
    const char *list_sql =
        "SELECT name, type FROM sqlite_master "
        "WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' "
        "ORDER BY name";
    sqlite3_stmt *list_stmt = NULL;
    if (sqlite3_prepare_v2(db, list_sql, -1, &list_stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        out->error = sdup("failed to list SQLite tables");
        return -1;
    }

    /* Count tables. */
    int table_count = 0;
    while (sqlite3_step(list_stmt) == SQLITE_ROW) {
        table_count++;
    }
    sqlite3_reset(list_stmt);

    if (table_count == 0) {
        sqlite3_finalize(list_stmt);
        sqlite3_close(db);
        out->schemas[0].tables = NULL;
        out->schemas[0].table_count = 0;
        return 0;
    }

    out->schemas[0].tables = cbm_calloc((size_t)table_count, sizeof(cbm_db_table_t));
    if (!out->schemas[0].tables) {
        sqlite3_finalize(list_stmt);
        sqlite3_close(db);
        out->error = sdup("allocation failure");
        return -1;
    }

    int ti = 0;
    while (sqlite3_step(list_stmt) == SQLITE_ROW && ti < table_count) {
        const char *tname = (const char *)sqlite3_column_text(list_stmt, 0);
        const char *ttype = (const char *)sqlite3_column_text(list_stmt, 1);

        cbm_db_table_t *tbl = &out->schemas[0].tables[ti];
        tbl->name = sdup(tname);
        tbl->schema_name = sdup("main");
        tbl->is_view = ttype && strcmp(ttype, "view") == 0;
        tbl->table_type = sdup(tbl->is_view ? "VIEW" : "BASE TABLE");
        tbl->comment = NULL;

        /* Row count. */
        if (!tbl->is_view) {
            tbl->row_count_estimate = count_rows(db, tname);
        } else {
            tbl->row_count_estimate = -1;
        }

        /* Columns. */
        extract_columns(db, tname, &tbl->columns, &tbl->column_count);

        /* Foreign keys. */
        extract_foreign_keys(db, tname, &tbl->foreign_keys, &tbl->fk_count,
                             tbl->columns, tbl->column_count);

        /* Indexes. */
        extract_indexes(db, tname, &tbl->indexes, &tbl->index_count,
                        tbl->columns, tbl->column_count);

        ti++;
    }
    out->schemas[0].table_count = ti;
    sqlite3_finalize(list_stmt);

    /* Extract sample data. */
    enum { DEFAULT_SAMPLE_ROWS = 5, LOOKUP_MAX_ROWS = 500 };
    int sample_cap = ti;
    out->samples = cbm_calloc((size_t)sample_cap, sizeof(cbm_db_sample_t));
    int si = 0;

    for (int t = 0; t < ti && si < sample_cap; t++) {
        cbm_db_table_t *tbl = &out->schemas[0].tables[t];
        if (tbl->is_view) {
            continue; /* skip views for sampling */
        }
        bool is_lookup = cbm_db_is_lookup_table(tbl->name, tbl->row_count_estimate);
        int max_rows = is_lookup ? LOOKUP_MAX_ROWS : DEFAULT_SAMPLE_ROWS;

        int row_count = 0;
        char *json = extract_sample_rows(db, tbl->name, tbl->columns, tbl->column_count,
                                         max_rows, &row_count);
        if (json && row_count > 0) {
            out->samples[si].table_name = sdup(tbl->name);
            out->samples[si].rows_json = json;
            out->samples[si].row_count = row_count;
            out->samples[si].is_full_cache = is_lookup;
            out->samples[si].is_encrypted = false;
            si++;
        } else {
            cbm_free(json);
        }
    }
    out->sample_count = si;

    sqlite3_close(db);
    return 0;
}
