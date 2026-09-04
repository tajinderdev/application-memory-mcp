/*
 * db_postgres_introspect.c — PostgreSQL database introspector via psql CLI.
 *
 * Uses the subprocess infrastructure to execute psql queries against
 * information_schema and pg_catalog, parsing pipe-delimited output
 * into the common cbm_db_result_t structure.
 */
#include "database/db_introspect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foundation/constants.h"
#include "foundation/mem.h"
#include "foundation/subprocess.h"
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

/* Trim leading and trailing whitespace in-place. Returns s. */
static char *trim(char *s) {
    if (!s) {
        return s;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';
    return s;
}

/* ── psql execution helper ─────────────────────────────────────── */

/* Collected output from a psql subprocess. */
typedef struct {
    char *output;
    size_t len;
    size_t cap;
} psql_output_t;

static void psql_log_cb(const char *line, void *ud) {
    psql_output_t *out = (psql_output_t *)ud;
    if (!line || !out) {
        return;
    }
    size_t llen = strlen(line);
    size_t needed = out->len + llen + CBM_SZ_2; /* +newline+NUL */
    if (needed > out->cap) {
        size_t newcap = needed * CBM_SZ_2;
        char *nb = cbm_realloc(out->output, newcap);
        if (!nb) {
            return;
        }
        out->output = nb;
        out->cap = newcap;
    }
    memcpy(out->output + out->len, line, llen);
    out->len += llen;
    out->output[out->len++] = '\n';
    out->output[out->len] = '\0';
}

/* Run a psql command and capture stdout. Returns 0 on success. */
static int run_psql(const char *connection_string, const char *query,
                    char **output_out) {
    /* Build psql command: psql <uri> -t -A -F '|' -c "<query>" */
    char cmd[CBM_SZ_8K];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
             "psql \"%s\" -t -A -F \"|\" -c \"%s\" 2>NUL",
             connection_string, query);
#else
    snprintf(cmd, sizeof(cmd),
             "psql '%s' -t -A -F '|' -c '%s' 2>/dev/null",
             connection_string, query);
#endif

    psql_output_t capture = {0};
    capture.cap = CBM_SZ_4K;
    capture.output = cbm_calloc(capture.cap, 1);
    if (!capture.output) {
        return -1;
    }

    char log_path[CBM_SZ_1K];
    snprintf(log_path, sizeof(log_path), "%s", "");

    /* Use a simple popen-based approach since subprocess.c is designed for
     * supervised long-running children with log-file tailing. For short
     * queries, popen is simpler and sufficient. */
#ifdef _WIN32
    FILE *fp = _popen(cmd, "r");
#else
    FILE *fp = popen(cmd, "r");
#endif
    if (!fp) {
        cbm_free(capture.output);
        return -1;
    }

    char buf[CBM_SZ_4K];
    while (fgets(buf, sizeof(buf), fp)) {
        psql_log_cb(buf, &capture);
    }

#ifdef _WIN32
    int rc = _pclose(fp);
#else
    int rc = pclose(fp);
#endif

    if (rc != 0) {
        cbm_free(capture.output);
        return -1;
    }

    *output_out = capture.output;
    return 0;
}

/* ── Parse pipe-delimited rows ─────────────────────────────────── */

/* Split a line by '|' into fields. Returns field count. */
static int split_pipe(char *line, char **fields, int max_fields) {
    int count = 0;
    char *p = line;
    while (p && count < max_fields) {
        fields[count] = p;
        char *sep = strchr(p, '|');
        if (sep) {
            *sep = '\0';
            p = sep + 1;
        } else {
            p = NULL;
        }
        fields[count] = trim(fields[count]);
        count++;
    }
    return count;
}

/* ── Public: PostgreSQL introspection backend ──────────────────── */

int cbm_db_introspect_postgres(const cbm_db_connect_opts_t *opts, cbm_db_result_t *out) {
    if (!opts || !opts->connection_string || !out) {
        if (out) {
            out->error = sdup("invalid arguments for PostgreSQL introspection");
        }
        return -1;
    }

    /* Test connection and get version. */
    char *version_out = NULL;
    if (run_psql(opts->connection_string, "SELECT version()", &version_out) != 0) {
        out->error = sdup("failed to connect to PostgreSQL. Ensure psql is installed and "
                          "the connection string is valid (e.g. postgres://user:pass@host/db)");
        return -1;
    }
    out->version = sdup(trim(version_out));
    cbm_free(version_out);

    /* Get database name. */
    char *dbname_out = NULL;
    if (run_psql(opts->connection_string, "SELECT current_database()", &dbname_out) == 0) {
        out->database_name = sdup(trim(dbname_out));
        cbm_free(dbname_out);
    } else {
        out->database_name = sdup("unknown");
    }
    out->host = sdup("remote");

    /* Get schemas. */
    char *schemas_out = NULL;
    const char *schema_query =
        "SELECT schema_name FROM information_schema.schemata "
        "WHERE schema_name NOT IN ('pg_catalog','information_schema','pg_toast') "
        "ORDER BY schema_name";
    if (run_psql(opts->connection_string, schema_query, &schemas_out) != 0) {
        out->error = sdup("failed to list schemas");
        return -1;
    }

    /* Parse schema names. */
    enum { MAX_SCHEMAS = 64 };
    char *schema_names[MAX_SCHEMAS];
    int schema_count = 0;
    char *line = strtok(schemas_out, "\n");
    while (line && schema_count < MAX_SCHEMAS) {
        char *trimmed = trim(line);
        if (trimmed[0] != '\0') {
            schema_names[schema_count++] = sdup(trimmed);
        }
        line = strtok(NULL, "\n");
    }
    cbm_free(schemas_out);

    if (schema_count == 0) {
        /* Default to "public" if no schemas found. */
        schema_names[0] = sdup("public");
        schema_count = 1;
    }

    out->schemas = cbm_calloc((size_t)schema_count, sizeof(cbm_db_schema_t));
    out->schema_count = schema_count;

    for (int si = 0; si < schema_count; si++) {
        out->schemas[si].name = schema_names[si];

        /* Get tables for this schema. */
        char tbl_query[CBM_SZ_2K];
        snprintf(tbl_query, sizeof(tbl_query),
                 "SELECT table_name, table_type "
                 "FROM information_schema.tables "
                 "WHERE table_schema = '%s' "
                 "ORDER BY table_name",
                 schema_names[si]);

        char *tbl_out = NULL;
        if (run_psql(opts->connection_string, tbl_query, &tbl_out) != 0) {
            continue;
        }

        /* Count tables. */
        enum { MAX_TABLES = 1024 };
        char *tbl_names[MAX_TABLES];
        char *tbl_types[MAX_TABLES];
        int tbl_count = 0;

        char *tline = strtok(tbl_out, "\n");
        while (tline && tbl_count < MAX_TABLES) {
            char *fields[CBM_SZ_8];
            int fc = split_pipe(tline, fields, CBM_SZ_8);
            if (fc >= CBM_SZ_2) {
                tbl_names[tbl_count] = sdup(fields[0]);
                tbl_types[tbl_count] = sdup(fields[1]);
                tbl_count++;
            }
            tline = strtok(NULL, "\n");
        }
        cbm_free(tbl_out);

        out->schemas[si].tables = cbm_calloc((size_t)tbl_count, sizeof(cbm_db_table_t));
        out->schemas[si].table_count = tbl_count;

        for (int ti = 0; ti < tbl_count; ti++) {
            cbm_db_table_t *tbl = &out->schemas[si].tables[ti];
            tbl->name = tbl_names[ti];
            tbl->schema_name = sdup(schema_names[si]);
            tbl->table_type = tbl_types[ti];
            tbl->is_view = tbl_types[ti] && strcmp(tbl_types[ti], "VIEW") == 0;
            tbl->comment = NULL;

            /* Row count estimate from pg_class. */
            char cnt_query[CBM_SZ_1K];
            snprintf(cnt_query, sizeof(cnt_query),
                     "SELECT reltuples::bigint FROM pg_class c "
                     "JOIN pg_namespace n ON c.relnamespace = n.oid "
                     "WHERE c.relname = '%s' AND n.nspname = '%s'",
                     tbl_names[ti], schema_names[si]);
            char *cnt_out = NULL;
            if (run_psql(opts->connection_string, cnt_query, &cnt_out) == 0) {
                tbl->row_count_estimate = atoll(trim(cnt_out));
                cbm_free(cnt_out);
            } else {
                tbl->row_count_estimate = -1;
            }

            /* Columns. */
            char col_query[CBM_SZ_2K];
            snprintf(col_query, sizeof(col_query),
                     "SELECT ordinal_position, column_name, data_type, "
                     "is_nullable, column_default "
                     "FROM information_schema.columns "
                     "WHERE table_schema = '%s' AND table_name = '%s' "
                     "ORDER BY ordinal_position",
                     schema_names[si], tbl_names[ti]);
            char *col_out = NULL;
            if (run_psql(opts->connection_string, col_query, &col_out) == 0) {
                /* Parse columns. */
                enum { MAX_COLS = 512 };
                cbm_db_column_t cols[MAX_COLS];
                memset(cols, 0, sizeof(cols));
                int col_count = 0;
                char *cline = strtok(col_out, "\n");
                while (cline && col_count < MAX_COLS) {
                    char *fields[CBM_SZ_8];
                    int fc = split_pipe(cline, fields, CBM_SZ_8);
                    if (fc >= CBM_SZ_5) {
                        cols[col_count].ordinal = atoi(fields[0]);
                        cols[col_count].name = sdup(fields[1]);
                        cols[col_count].data_type = sdup(fields[CBM_SZ_2]);
                        cols[col_count].is_nullable = strcmp(fields[CBM_SZ_3], "YES") == 0;
                        cols[col_count].default_value =
                            (fields[CBM_SZ_4][0] != '\0') ? sdup(fields[CBM_SZ_4]) : NULL;
                        cols[col_count].is_pk = false;
                        cols[col_count].is_fk = false;
                        cols[col_count].is_unique = false;
                        cols[col_count].comment = NULL;
                        col_count++;
                    }
                    cline = strtok(NULL, "\n");
                }
                cbm_free(col_out);

                if (col_count > 0) {
                    tbl->columns = cbm_calloc((size_t)col_count, sizeof(cbm_db_column_t));
                    memcpy(tbl->columns, cols, (size_t)col_count * sizeof(cbm_db_column_t));
                    tbl->column_count = col_count;
                }
            } else {
                cbm_free(col_out);
            }

            /* Primary keys. */
            char pk_query[CBM_SZ_2K];
            snprintf(pk_query, sizeof(pk_query),
                     "SELECT kcu.column_name "
                     "FROM information_schema.table_constraints tc "
                     "JOIN information_schema.key_column_usage kcu "
                     "ON tc.constraint_name = kcu.constraint_name "
                     "AND tc.table_schema = kcu.table_schema "
                     "WHERE tc.constraint_type = 'PRIMARY KEY' "
                     "AND tc.table_schema = '%s' AND tc.table_name = '%s'",
                     schema_names[si], tbl_names[ti]);
            char *pk_out = NULL;
            if (run_psql(opts->connection_string, pk_query, &pk_out) == 0) {
                char *pkline = strtok(pk_out, "\n");
                while (pkline) {
                    char *pk_col = trim(pkline);
                    for (int c = 0; c < tbl->column_count; c++) {
                        if (tbl->columns[c].name && strcmp(tbl->columns[c].name, pk_col) == 0) {
                            tbl->columns[c].is_pk = true;
                        }
                    }
                    pkline = strtok(NULL, "\n");
                }
                cbm_free(pk_out);
            }

            /* Foreign keys. */
            char fk_query[CBM_SZ_4K];
            snprintf(fk_query, sizeof(fk_query),
                     "SELECT tc.constraint_name, kcu.column_name, "
                     "ccu.table_name AS ref_table, ccu.column_name AS ref_column, "
                     "rc.update_rule, rc.delete_rule "
                     "FROM information_schema.table_constraints tc "
                     "JOIN information_schema.key_column_usage kcu "
                     "ON tc.constraint_name = kcu.constraint_name "
                     "AND tc.table_schema = kcu.table_schema "
                     "JOIN information_schema.constraint_column_usage ccu "
                     "ON tc.constraint_name = ccu.constraint_name "
                     "JOIN information_schema.referential_constraints rc "
                     "ON tc.constraint_name = rc.constraint_name "
                     "WHERE tc.constraint_type = 'FOREIGN KEY' "
                     "AND tc.table_schema = '%s' AND tc.table_name = '%s'",
                     schema_names[si], tbl_names[ti]);
            char *fk_out = NULL;
            if (run_psql(opts->connection_string, fk_query, &fk_out) == 0) {
                enum { MAX_FKS = 256 };
                cbm_db_fk_t fks[MAX_FKS];
                memset(fks, 0, sizeof(fks));
                int fk_count = 0;

                char *fkline = strtok(fk_out, "\n");
                while (fkline && fk_count < MAX_FKS) {
                    char *fields[CBM_SZ_8];
                    int fc = split_pipe(fkline, fields, CBM_SZ_8);
                    if (fc >= CBM_SZ_6) {
                        /* Check if this constraint already exists. */
                        int found = -1;
                        for (int f = 0; f < fk_count; f++) {
                            if (fks[f].constraint_name &&
                                strcmp(fks[f].constraint_name, fields[0]) == 0) {
                                found = f;
                                break;
                            }
                        }
                        if (found < 0) {
                            found = fk_count++;
                            fks[found].constraint_name = sdup(fields[0]);
                            fks[found].target_table = sdup(fields[CBM_SZ_2]);
                            fks[found].target_schema = NULL;
                            fks[found].on_update = sdup(fields[CBM_SZ_4]);
                            fks[found].on_delete = sdup(fields[CBM_SZ_5]);
                            fks[found].source_columns = cbm_calloc(1, sizeof(char *));
                            fks[found].source_columns[0] = sdup(fields[1]);
                            fks[found].source_column_count = 1;
                            fks[found].target_columns = cbm_calloc(1, sizeof(char *));
                            fks[found].target_columns[0] = sdup(fields[CBM_SZ_3]);
                            fks[found].target_column_count = 1;
                        }
                        /* Mark FK columns. */
                        for (int c = 0; c < tbl->column_count; c++) {
                            if (tbl->columns[c].name &&
                                strcmp(tbl->columns[c].name, fields[1]) == 0) {
                                tbl->columns[c].is_fk = true;
                            }
                        }
                    }
                    fkline = strtok(NULL, "\n");
                }
                cbm_free(fk_out);

                if (fk_count > 0) {
                    tbl->foreign_keys = cbm_calloc((size_t)fk_count, sizeof(cbm_db_fk_t));
                    memcpy(tbl->foreign_keys, fks, (size_t)fk_count * sizeof(cbm_db_fk_t));
                    tbl->fk_count = fk_count;
                }
            }

            /* Indexes via pg_indexes. */
            char idx_query[CBM_SZ_2K];
            snprintf(idx_query, sizeof(idx_query),
                     "SELECT indexname, indexdef "
                     "FROM pg_indexes "
                     "WHERE schemaname = '%s' AND tablename = '%s'",
                     schema_names[si], tbl_names[ti]);
            char *idx_out = NULL;
            if (run_psql(opts->connection_string, idx_query, &idx_out) == 0) {
                enum { MAX_IDXS = 128 };
                cbm_db_index_t idxs[MAX_IDXS];
                memset(idxs, 0, sizeof(idxs));
                int idx_count = 0;

                char *idxline = strtok(idx_out, "\n");
                while (idxline && idx_count < MAX_IDXS) {
                    char *fields[CBM_SZ_4];
                    int fc = split_pipe(idxline, fields, CBM_SZ_4);
                    if (fc >= CBM_SZ_2) {
                        idxs[idx_count].name = sdup(fields[0]);
                        /* Parse UNIQUE from indexdef. */
                        const char *def = fields[1];
                        idxs[idx_count].is_unique =
                            def && (strstr(def, "UNIQUE") != NULL);
                        idxs[idx_count].is_primary =
                            def && (strstr(def, "PRIMARY") != NULL ||
                                    strstr(def, "_pkey") != NULL);
                        idxs[idx_count].index_type = sdup("BTREE");
                        if (def && strstr(def, "USING hash")) {
                            cbm_free(idxs[idx_count].index_type);
                            idxs[idx_count].index_type = sdup("HASH");
                        } else if (def && strstr(def, "USING gin")) {
                            cbm_free(idxs[idx_count].index_type);
                            idxs[idx_count].index_type = sdup("GIN");
                        } else if (def && strstr(def, "USING gist")) {
                            cbm_free(idxs[idx_count].index_type);
                            idxs[idx_count].index_type = sdup("GIST");
                        }

                        /* Extract column names from parentheses in indexdef. */
                        const char *paren = def ? strchr(def, '(') : NULL;
                        if (paren) {
                            paren++;
                            const char *end_paren = strchr(paren, ')');
                            if (end_paren) {
                                size_t clen = (size_t)(end_paren - paren);
                                char col_buf[CBM_SZ_1K];
                                if (clen < sizeof(col_buf)) {
                                    memcpy(col_buf, paren, clen);
                                    col_buf[clen] = '\0';
                                    /* Split by comma. */
                                    char *ccols[CBM_SZ_32];
                                    int ccnt = 0;
                                    char *cp = strtok(col_buf, ",");
                                    while (cp && ccnt < CBM_SZ_32) {
                                        ccols[ccnt++] = trim(cp);
                                        cp = strtok(NULL, ",");
                                    }
                                    if (ccnt > 0) {
                                        idxs[idx_count].columns =
                                            cbm_calloc((size_t)ccnt, sizeof(char *));
                                        for (int c = 0; c < ccnt; c++) {
                                            idxs[idx_count].columns[c] = sdup(ccols[c]);
                                        }
                                        idxs[idx_count].column_count = ccnt;
                                    }
                                }
                            }
                        }
                        idx_count++;
                    }
                    idxline = strtok(NULL, "\n");
                }
                cbm_free(idx_out);

                if (idx_count > 0) {
                    tbl->indexes = cbm_calloc((size_t)idx_count, sizeof(cbm_db_index_t));
                    memcpy(tbl->indexes, idxs, (size_t)idx_count * sizeof(cbm_db_index_t));
                    tbl->index_count = idx_count;
                }
            }
        }
    }

    /* Sample data extraction omitted for remote DBs to avoid network overhead.
     * Users can trigger coherent sampling explicitly. A future pass can query
     * via psql SELECT ... LIMIT N with FK-chain following. */

    return 0;
}
