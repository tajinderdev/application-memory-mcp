/*
 * erd_generator.c — Mermaid ER diagram generation from database metadata.
 *
 * Produces valid Mermaid erDiagram syntax from the cbm_db_result_t
 * structure, with configurable detail levels (keys_only, columns, full)
 * and a table cap for readable output.
 */
#include "database/erd_generator.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foundation/constants.h"
#include "foundation/mem.h"
#include "mcp/compact_out.h" /* cbm_sb_t */

/* ── Internal helpers ──────────────────────────────────────────── */

/* Sanitize a name for Mermaid: replace spaces/special chars with underscore. */
static void sanitize_name(const char *src, char *dst, size_t dstsz) {
    if (!src || dstsz == 0) {
        if (dstsz > 0) {
            dst[0] = '\0';
        }
        return;
    }
    size_t i = 0;
    for (; src[i] && i < dstsz - 1; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            dst[i] = c;
        } else {
            dst[i] = '_';
        }
    }
    dst[i] = '\0';
}

/* Map data type to a simplified Mermaid-friendly type string. */
static const char *simplify_type(const char *dtype) {
    if (!dtype) {
        return "unknown";
    }
    /* Normalize common types. */
    if (strstr(dtype, "int") || strstr(dtype, "INT")) {
        return "int";
    }
    if (strstr(dtype, "varchar") || strstr(dtype, "VARCHAR") ||
        strstr(dtype, "character varying") || strstr(dtype, "text") ||
        strstr(dtype, "TEXT") || strstr(dtype, "String")) {
        return "string";
    }
    if (strstr(dtype, "bool") || strstr(dtype, "BOOL") || strstr(dtype, "Boolean")) {
        return "bool";
    }
    if (strstr(dtype, "float") || strstr(dtype, "FLOAT") ||
        strstr(dtype, "double") || strstr(dtype, "DOUBLE") ||
        strstr(dtype, "real") || strstr(dtype, "REAL") ||
        strstr(dtype, "decimal") || strstr(dtype, "DECIMAL") ||
        strstr(dtype, "numeric") || strstr(dtype, "NUMERIC")) {
        return "float";
    }
    if (strstr(dtype, "date") || strstr(dtype, "DATE") ||
        strstr(dtype, "time") || strstr(dtype, "TIME") ||
        strstr(dtype, "timestamp") || strstr(dtype, "TIMESTAMP")) {
        return "datetime";
    }
    if (strstr(dtype, "blob") || strstr(dtype, "BLOB") ||
        strstr(dtype, "bytea") || strstr(dtype, "binary")) {
        return "blob";
    }
    if (strstr(dtype, "json") || strstr(dtype, "JSON") ||
        strstr(dtype, "jsonb")) {
        return "json";
    }
    if (strstr(dtype, "uuid") || strstr(dtype, "UUID")) {
        return "uuid";
    }
    if (strstr(dtype, "ObjectId")) {
        return "ObjectId";
    }
    return dtype;
}

/* Check if a table name is in the filter list. */
static bool table_in_filter(const char *name, const char *const *filter, int filter_count) {
    if (!filter || filter_count <= 0) {
        return true; /* no filter = include all */
    }
    for (int i = 0; i < filter_count; i++) {
        if (filter[i] && strcmp(name, filter[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Relationship cardinality ──────────────────────────────────── */

/* Determine Mermaid relationship notation based on FK constraint properties.
 * Returns a static string like "||--o{" or "||--||". */
static const char *fk_cardinality(const cbm_db_fk_t *fk, const cbm_db_column_t *columns,
                                  int col_count) {
    /* Default: one-to-many (parent ||--o{ child). */
    if (!fk || !columns) {
        return "||--o{";
    }

    /* Check if the FK source column is unique — implies one-to-one. */
    for (int i = 0; i < fk->source_column_count; i++) {
        for (int c = 0; c < col_count; c++) {
            if (columns[c].name && fk->source_columns[i] &&
                strcmp(columns[c].name, fk->source_columns[i]) == 0) {
                if (columns[c].is_unique || columns[c].is_pk) {
                    return "||--||";
                }
            }
        }
    }

    /* Check if FK column is nullable — implies zero-or-many. */
    for (int i = 0; i < fk->source_column_count; i++) {
        for (int c = 0; c < col_count; c++) {
            if (columns[c].name && fk->source_columns[i] &&
                strcmp(columns[c].name, fk->source_columns[i]) == 0) {
                if (columns[c].is_nullable) {
                    return "}o--o{";
                }
            }
        }
    }

    return "||--o{";
}

/* ── Public: Parse detail level ────────────────────────────────── */

int cbm_erd_parse_detail(const char *s, cbm_erd_detail_t *out) {
    if (!s || !out) {
        return -1;
    }
    if (strcmp(s, "keys_only") == 0) {
        *out = CBM_ERD_KEYS_ONLY;
        return 0;
    }
    if (strcmp(s, "columns") == 0) {
        *out = CBM_ERD_COLUMNS;
        return 0;
    }
    if (strcmp(s, "full") == 0) {
        *out = CBM_ERD_FULL;
        return 0;
    }
    return -1;
}

/* ── Public: Generate Mermaid ERD ──────────────────────────────── */

char *cbm_erd_generate_mermaid(const cbm_db_result_t *result,
                               const char *const *tables, int table_count,
                               cbm_erd_detail_t detail, int max_tables) {
    if (!result) {
        return NULL;
    }

    cbm_sb_t sb;
    cbm_sb_init(&sb);
    cbm_sb_append(&sb, "erDiagram\n");

    int tables_emitted = 0;

    /* First pass: emit entity blocks with columns. */
    for (int s = 0; s < result->schema_count; s++) {
        for (int t = 0; t < result->schemas[s].table_count; t++) {
            if (max_tables > 0 && tables_emitted >= max_tables) {
                break;
            }
            const cbm_db_table_t *tbl = &result->schemas[s].tables[t];
            if (!table_in_filter(tbl->name, tables, table_count)) {
                continue;
            }

            char safe_name[CBM_SZ_256];
            sanitize_name(tbl->name, safe_name, sizeof(safe_name));

            /* Entity block. */
            char line[CBM_SZ_512];
            snprintf(line, sizeof(line), "    %s {\n", safe_name);
            cbm_sb_append(&sb, line);

            for (int c = 0; c < tbl->column_count; c++) {
                const cbm_db_column_t *col = &tbl->columns[c];

                /* Filter by detail level. */
                if (detail == CBM_ERD_KEYS_ONLY && !col->is_pk && !col->is_fk) {
                    continue;
                }

                /* Column annotation markers. */
                const char *marker = "";
                if (col->is_pk) {
                    marker = " PK";
                } else if (col->is_fk) {
                    marker = " FK";
                } else if (col->is_unique) {
                    marker = " UK";
                }

                char col_line[CBM_SZ_512];
                if (detail == CBM_ERD_FULL) {
                    const char *stype = simplify_type(col->data_type);
                    snprintf(col_line, sizeof(col_line), "        %s %s%s\n",
                             stype, col->name, marker);
                } else {
                    snprintf(col_line, sizeof(col_line), "        %s%s\n",
                             col->name, marker);
                }
                cbm_sb_append(&sb, col_line);
            }

            cbm_sb_append(&sb, "    }\n");
            tables_emitted++;
        }
    }

    /* Overflow notice. */
    if (max_tables > 0 && tables_emitted >= max_tables) {
        int total = 0;
        for (int s = 0; s < result->schema_count; s++) {
            total += result->schemas[s].table_count;
        }
        if (total > tables_emitted) {
            char notice[CBM_SZ_256];
            snprintf(notice, sizeof(notice),
                     "    %% NOTE: showing %d of %d tables. "
                     "Use 'tables' parameter to filter or raise 'max_tables'.\n",
                     tables_emitted, total);
            cbm_sb_append(&sb, notice);
        }
    }

    cbm_sb_append(&sb, "\n");

    /* Second pass: emit relationships from foreign keys. */
    for (int s = 0; s < result->schema_count; s++) {
        for (int t = 0; t < result->schemas[s].table_count; t++) {
            const cbm_db_table_t *tbl = &result->schemas[s].tables[t];
            if (!table_in_filter(tbl->name, tables, table_count)) {
                continue;
            }

            char src_name[CBM_SZ_256];
            sanitize_name(tbl->name, src_name, sizeof(src_name));

            for (int f = 0; f < tbl->fk_count; f++) {
                const cbm_db_fk_t *fk = &tbl->foreign_keys[f];
                if (!fk->target_table) {
                    continue;
                }

                /* Only emit if target table is also in the diagram. */
                if (!table_in_filter(fk->target_table, tables, table_count)) {
                    /* If no filter is set, include anyway (target exists in result). */
                    if (tables && table_count > 0) {
                        continue;
                    }
                }

                char tgt_name[CBM_SZ_256];
                sanitize_name(fk->target_table, tgt_name, sizeof(tgt_name));

                const char *card = fk_cardinality(fk, tbl->columns, tbl->column_count);

                /* Build label from FK column names. */
                char label[CBM_SZ_256] = "";
                if (fk->source_column_count > 0 && fk->source_columns[0]) {
                    snprintf(label, sizeof(label), "%s", fk->source_columns[0]);
                }

                char rel[CBM_SZ_512];
                snprintf(rel, sizeof(rel), "    %s %s %s : \"%s\"\n",
                         tgt_name, card, src_name, label);
                cbm_sb_append(&sb, rel);
            }
        }
    }

    return cbm_sb_finish(&sb);
}
