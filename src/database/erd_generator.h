/*
 * erd_generator.h — Mermaid ER diagram generation from database graph nodes.
 *
 * Generates erDiagram syntax from the in-memory cbm_db_result_t structure
 * with configurable detail levels and table caps for readable output.
 */
#ifndef CBM_ERD_GENERATOR_H
#define CBM_ERD_GENERATOR_H

#include "database/db_introspect.h"

/* Detail levels for ERD generation. */
typedef enum {
    CBM_ERD_KEYS_ONLY = 0, /* Only PK and FK columns */
    CBM_ERD_COLUMNS = 1,   /* All columns without types */
    CBM_ERD_FULL = 2,      /* All columns with data types */
} cbm_erd_detail_t;

/* Generate a Mermaid erDiagram string from a database introspection result.
 *
 * tables/table_count: optional filter — only include these tables.
 *   Pass NULL/0 to include all tables.
 * detail: how much column information to include.
 * max_tables: cap on number of tables to include (0 = no cap).
 *
 * Returns a heap-allocated string (caller frees) or NULL on failure. */
char *cbm_erd_generate_mermaid(const cbm_db_result_t *result,
                               const char *const *tables, int table_count,
                               cbm_erd_detail_t detail, int max_tables);

/* Parse a detail level string. Returns 0 on success. */
int cbm_erd_parse_detail(const char *s, cbm_erd_detail_t *out);

#endif /* CBM_ERD_GENERATOR_H */
