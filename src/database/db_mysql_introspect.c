/*
 * db_mysql_introspect.c — MySQL/MariaDB database introspector via mysql CLI.
 *
 * Uses popen to execute mysql queries against information_schema,
 * parsing tab-delimited output into the common cbm_db_result_t structure.
 */
#include "database/db_introspect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foundation/constants.h"
#include "foundation/mem.h"

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

/* ── mysql CLI execution ───────────────────────────────────────── */

/* Parse a MySQL URI: mysql://user:pass@host:port/dbname
 * Extracts components for building the mysql CLI command. */
typedef struct {
    char user[CBM_SZ_128];
    char password[CBM_SZ_128];
    char host[CBM_SZ_256];
    char port[CBM_SZ_16];
    char database[CBM_SZ_128];
} mysql_conn_parts_t;

static bool parse_mysql_uri(const char *uri, mysql_conn_parts_t *parts) {
    memset(parts, 0, sizeof(*parts));

    /* Skip protocol prefix. */
    const char *p = uri;
    if (strncmp(p, "mysql://", CBM_SZ_8) == 0) {
        p += CBM_SZ_8;
    } else if (strncmp(p, "mariadb://", 10) == 0) {
        p += 10;
    }

    /* user:password@host:port/database */
    const char *at = strchr(p, '@');
    if (at) {
        /* Extract user:password. */
        const char *colon = strchr(p, ':');
        if (colon && colon < at) {
            size_t ulen = (size_t)(colon - p);
            if (ulen >= sizeof(parts->user)) {
                ulen = sizeof(parts->user) - 1;
            }
            memcpy(parts->user, p, ulen);
            size_t plen = (size_t)(at - colon - 1);
            if (plen >= sizeof(parts->password)) {
                plen = sizeof(parts->password) - 1;
            }
            memcpy(parts->password, colon + 1, plen);
        } else {
            size_t ulen = (size_t)(at - p);
            if (ulen >= sizeof(parts->user)) {
                ulen = sizeof(parts->user) - 1;
            }
            memcpy(parts->user, p, ulen);
        }
        p = at + 1;
    }

    /* host:port/database */
    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *port_colon = strchr(p, ':');

    if (port_colon && port_colon < host_end) {
        size_t hlen = (size_t)(port_colon - p);
        if (hlen >= sizeof(parts->host)) {
            hlen = sizeof(parts->host) - 1;
        }
        memcpy(parts->host, p, hlen);
        size_t portlen = (size_t)(host_end - port_colon - 1);
        if (portlen >= sizeof(parts->port)) {
            portlen = sizeof(parts->port) - 1;
        }
        memcpy(parts->port, port_colon + 1, portlen);
    } else {
        size_t hlen = (size_t)(host_end - p);
        if (hlen >= sizeof(parts->host)) {
            hlen = sizeof(parts->host) - 1;
        }
        memcpy(parts->host, p, hlen);
    }

    if (slash) {
        /* Remove query string if present. */
        const char *qmark = strchr(slash + 1, '?');
        size_t dlen = qmark ? (size_t)(qmark - slash - 1) : strlen(slash + 1);
        if (dlen >= sizeof(parts->database)) {
            dlen = sizeof(parts->database) - 1;
        }
        memcpy(parts->database, slash + 1, dlen);
    }

    return parts->database[0] != '\0';
}

static int run_mysql(const char *connection_string, const char *query, char **output_out) {
    mysql_conn_parts_t parts;
    if (!parse_mysql_uri(connection_string, &parts)) {
        return -1;
    }

    /* Build mysql command. */
    char cmd[CBM_SZ_8K];
    int off = 0;
    off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, "mysql -N -B");
    if (parts.user[0]) {
        off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, " -u \"%s\"", parts.user);
    }
    if (parts.password[0]) {
        off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, " -p\"%s\"", parts.password);
    }
    if (parts.host[0]) {
        off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, " -h \"%s\"", parts.host);
    }
    if (parts.port[0]) {
        off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, " -P %s", parts.port);
    }
    off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, " \"%s\"", parts.database);
    off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, " -e \"%s\"", query);

#ifdef _WIN32
    off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, " 2>NUL");
#else
    off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, " 2>/dev/null");
#endif
    (void)off;

#ifdef _WIN32
    FILE *fp = _popen(cmd, "r");
#else
    FILE *fp = popen(cmd, "r");
#endif
    if (!fp) {
        return -1;
    }

    size_t cap = CBM_SZ_4K;
    char *buf = cbm_calloc(cap, 1);
    size_t len = 0;
    char line[CBM_SZ_4K];
    while (fgets(line, sizeof(line), fp)) {
        size_t llen = strlen(line);
        if (len + llen + 1 > cap) {
            cap = (len + llen + 1) * CBM_SZ_2;
            buf = cbm_realloc(buf, cap);
        }
        memcpy(buf + len, line, llen);
        len += llen;
        buf[len] = '\0';
    }

#ifdef _WIN32
    int rc = _pclose(fp);
#else
    int rc = pclose(fp);
#endif
    if (rc != 0) {
        cbm_free(buf);
        return -1;
    }
    *output_out = buf;
    return 0;
}

/* Split a tab-delimited line. */
static int split_tab(char *line, char **fields, int max_fields) {
    int count = 0;
    char *p = line;
    while (p && count < max_fields) {
        fields[count] = p;
        char *tab = strchr(p, '\t');
        if (tab) {
            *tab = '\0';
            p = tab + 1;
        } else {
            p = NULL;
        }
        fields[count] = trim(fields[count]);
        count++;
    }
    return count;
}

/* ── Public: MySQL introspection backend ───────────────────────── */

int cbm_db_introspect_mysql(const cbm_db_connect_opts_t *opts, cbm_db_result_t *out) {
    if (!opts || !opts->connection_string || !out) {
        if (out) {
            out->error = sdup("invalid arguments for MySQL introspection");
        }
        return -1;
    }

    mysql_conn_parts_t parts;
    if (!parse_mysql_uri(opts->connection_string, &parts)) {
        out->error = sdup("failed to parse MySQL connection string. "
                          "Expected format: mysql://user:pass@host:port/database");
        return -1;
    }

    /* Test connection. */
    char *version_out = NULL;
    if (run_mysql(opts->connection_string, "SELECT VERSION()", &version_out) != 0) {
        out->error = sdup("failed to connect to MySQL. Ensure mysql CLI is installed "
                          "and the connection string is valid");
        return -1;
    }
    out->version = sdup(trim(version_out));
    cbm_free(version_out);
    out->database_name = sdup(parts.database);
    out->host = sdup(parts.host[0] ? parts.host : "localhost");

    /* MySQL has a single schema per connection (the database). */
    out->schema_count = 1;
    out->schemas = cbm_calloc(1, sizeof(cbm_db_schema_t));
    out->schemas[0].name = sdup(parts.database);

    /* List tables. */
    char tbl_query[CBM_SZ_2K];
    snprintf(tbl_query, sizeof(tbl_query),
             "SELECT TABLE_NAME, TABLE_TYPE, TABLE_ROWS, TABLE_COMMENT "
             "FROM information_schema.TABLES "
             "WHERE TABLE_SCHEMA = '%s' ORDER BY TABLE_NAME",
             parts.database);

    char *tbl_out = NULL;
    if (run_mysql(opts->connection_string, tbl_query, &tbl_out) != 0) {
        out->error = sdup("failed to list MySQL tables");
        return -1;
    }

    /* Parse table list. */
    enum { MAX_TABLES = 1024 };
    cbm_db_table_t tbls[MAX_TABLES];
    memset(tbls, 0, sizeof(tbls));
    int tbl_count = 0;

    char *tline = strtok(tbl_out, "\n");
    while (tline && tbl_count < MAX_TABLES) {
        char *fields[CBM_SZ_8];
        int fc = split_tab(tline, fields, CBM_SZ_8);
        if (fc >= CBM_SZ_4) {
            tbls[tbl_count].name = sdup(fields[0]);
            tbls[tbl_count].schema_name = sdup(parts.database);
            tbls[tbl_count].table_type = sdup(fields[1]);
            tbls[tbl_count].is_view = fields[1] && strcmp(fields[1], "VIEW") == 0;
            tbls[tbl_count].row_count_estimate = atoll(fields[CBM_SZ_2]);
            tbls[tbl_count].comment = (fields[CBM_SZ_3][0] != '\0') ? sdup(fields[CBM_SZ_3]) : NULL;
            tbl_count++;
        }
        tline = strtok(NULL, "\n");
    }
    cbm_free(tbl_out);

    out->schemas[0].tables = cbm_calloc((size_t)tbl_count, sizeof(cbm_db_table_t));
    memcpy(out->schemas[0].tables, tbls, (size_t)tbl_count * sizeof(cbm_db_table_t));
    out->schemas[0].table_count = tbl_count;

    /* For each table, get columns + FKs + indexes. */
    for (int ti = 0; ti < tbl_count; ti++) {
        cbm_db_table_t *tbl = &out->schemas[0].tables[ti];

        /* Columns. */
        char col_query[CBM_SZ_2K];
        snprintf(col_query, sizeof(col_query),
                 "SELECT ORDINAL_POSITION, COLUMN_NAME, COLUMN_TYPE, "
                 "IS_NULLABLE, COLUMN_DEFAULT, COLUMN_KEY, COLUMN_COMMENT "
                 "FROM information_schema.COLUMNS "
                 "WHERE TABLE_SCHEMA = '%s' AND TABLE_NAME = '%s' "
                 "ORDER BY ORDINAL_POSITION",
                 parts.database, tbl->name);

        char *col_out = NULL;
        if (run_mysql(opts->connection_string, col_query, &col_out) == 0) {
            enum { MAX_COLS = 512 };
            cbm_db_column_t cols[MAX_COLS];
            memset(cols, 0, sizeof(cols));
            int col_count = 0;

            char *cline = strtok(col_out, "\n");
            while (cline && col_count < MAX_COLS) {
                char *cfields[CBM_SZ_8];
                int cfc = split_tab(cline, cfields, CBM_SZ_8);
                if (cfc >= CBM_SZ_7) {
                    cols[col_count].ordinal = atoi(cfields[0]);
                    cols[col_count].name = sdup(cfields[1]);
                    cols[col_count].data_type = sdup(cfields[CBM_SZ_2]);
                    cols[col_count].is_nullable = strcmp(cfields[CBM_SZ_3], "YES") == 0;
                    cols[col_count].default_value =
                        (strcmp(cfields[CBM_SZ_4], "NULL") != 0 && cfields[CBM_SZ_4][0])
                            ? sdup(cfields[CBM_SZ_4]) : NULL;
                    cols[col_count].is_pk = strcmp(cfields[CBM_SZ_5], "PRI") == 0;
                    cols[col_count].is_unique = strcmp(cfields[CBM_SZ_5], "UNI") == 0 ||
                                               cols[col_count].is_pk;
                    cols[col_count].is_fk = strcmp(cfields[CBM_SZ_5], "MUL") == 0;
                    cols[col_count].comment =
                        (cfields[CBM_SZ_6][0] != '\0') ? sdup(cfields[CBM_SZ_6]) : NULL;
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
        }

        /* Foreign keys. */
        char fk_query[CBM_SZ_4K];
        snprintf(fk_query, sizeof(fk_query),
                 "SELECT CONSTRAINT_NAME, COLUMN_NAME, REFERENCED_TABLE_NAME, "
                 "REFERENCED_COLUMN_NAME "
                 "FROM information_schema.KEY_COLUMN_USAGE "
                 "WHERE TABLE_SCHEMA = '%s' AND TABLE_NAME = '%s' "
                 "AND REFERENCED_TABLE_NAME IS NOT NULL",
                 parts.database, tbl->name);

        char *fk_out = NULL;
        if (run_mysql(opts->connection_string, fk_query, &fk_out) == 0) {
            enum { MAX_FKS = 256 };
            cbm_db_fk_t fks[MAX_FKS];
            memset(fks, 0, sizeof(fks));
            int fk_count = 0;

            char *fkline = strtok(fk_out, "\n");
            while (fkline && fk_count < MAX_FKS) {
                char *ffields[CBM_SZ_8];
                int ffc = split_tab(fkline, ffields, CBM_SZ_8);
                if (ffc >= CBM_SZ_4) {
                    fks[fk_count].constraint_name = sdup(ffields[0]);
                    fks[fk_count].target_table = sdup(ffields[CBM_SZ_2]);
                    fks[fk_count].target_schema = NULL;
                    fks[fk_count].on_delete = sdup("RESTRICT");
                    fks[fk_count].on_update = sdup("RESTRICT");
                    fks[fk_count].source_columns = cbm_calloc(1, sizeof(char *));
                    fks[fk_count].source_columns[0] = sdup(ffields[1]);
                    fks[fk_count].source_column_count = 1;
                    fks[fk_count].target_columns = cbm_calloc(1, sizeof(char *));
                    fks[fk_count].target_columns[0] = sdup(ffields[CBM_SZ_3]);
                    fks[fk_count].target_column_count = 1;
                    fk_count++;
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
    }

    return 0;
}
