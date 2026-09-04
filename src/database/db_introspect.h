/*
 * db_introspect.h — Database introspection engine for Full Stack Application Memory MCP.
 *
 * Extracts schema metadata (tables, columns, PKs, FKs, indexes),
 * generates compact graph representations, and provides domain-aware
 * coherent sampling. Supports PostgreSQL, MySQL, SQLite, and MongoDB
 * through native and CLI-bridge backends.
 *
 * Thread safety: a single introspection context must not be used
 * concurrently. One context per thread or external synchronization.
 */
#ifndef CBM_DB_INTROSPECT_H
#define CBM_DB_INTROSPECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Memory allocator aliases */
#ifndef cbm_malloc
#define cbm_malloc(sz) malloc(sz)
#define cbm_calloc(n, sz) calloc((n), (sz))
#define cbm_realloc(p, sz) realloc((p), (sz))
#define cbm_free(p) free(p)
#endif

/* ── Database dialects ─────────────────────────────────────────── */

typedef enum {
    CBM_DB_SQLITE = 0,
    CBM_DB_POSTGRES = 1,
    CBM_DB_MYSQL = 2,
    CBM_DB_MONGODB = 3,
} cbm_db_dialect_t;

/* ── Source environment labels ─────────────────────────────────── */

typedef enum {
    CBM_DB_ENV_LOCAL = 0,
    CBM_DB_ENV_DEVELOPMENT = 1,
    CBM_DB_ENV_STAGING = 2,
    CBM_DB_ENV_PRODUCTION = 3,
} cbm_db_source_env_t;

/* ── Application domain detection ──────────────────────────────── */

typedef enum {
    CBM_DB_DOMAIN_UNKNOWN = 0,
    CBM_DB_DOMAIN_ECOMMERCE = 1,
    CBM_DB_DOMAIN_CMS = 2,
    CBM_DB_DOMAIN_SAAS = 3,
    CBM_DB_DOMAIN_AUTH = 4,
    CBM_DB_DOMAIN_HEALTHCARE = 5,
} cbm_db_domain_t;

/* ── Column metadata ───────────────────────────────────────────── */

typedef struct {
    char *name;
    char *data_type;
    int ordinal;         /* 1-based column position */
    bool is_pk;
    bool is_nullable;
    bool is_unique;
    bool is_fk;
    char *default_value; /* NULL if none */
    char *comment;       /* NULL if none */
} cbm_db_column_t;

/* ── Index metadata ────────────────────────────────────────────── */

typedef struct {
    char *name;
    char *index_type;    /* "BTREE", "HASH", "GIN", "GIST", "" */
    bool is_unique;
    bool is_primary;
    char **columns;      /* column names covered */
    int column_count;
} cbm_db_index_t;

/* ── Foreign key metadata ──────────────────────────────────────── */

typedef struct {
    char *constraint_name;
    char **source_columns;
    int source_column_count;
    char *target_table;
    char *target_schema;  /* NULL if same schema */
    char **target_columns;
    int target_column_count;
    char *on_delete;      /* "CASCADE", "SET NULL", "RESTRICT", "NO ACTION" */
    char *on_update;
} cbm_db_fk_t;

/* ── Table metadata ────────────────────────────────────────────── */

typedef struct {
    char *name;
    char *schema_name;     /* "public", "dbo", "main", etc. */
    char *table_type;      /* "BASE TABLE", "VIEW", "COLLECTION" */
    int64_t row_count_estimate;
    bool is_lookup;        /* detected as lookup/reference table */
    bool is_view;
    char *comment;         /* NULL if none */

    cbm_db_column_t *columns;
    int column_count;

    cbm_db_index_t *indexes;
    int index_count;

    cbm_db_fk_t *foreign_keys;
    int fk_count;
} cbm_db_table_t;

/* ── Schema metadata ───────────────────────────────────────────── */

typedef struct {
    char *name;            /* "public", "main", etc. */
    cbm_db_table_t *tables;
    int table_count;
} cbm_db_schema_t;

/* ── Sample data row ───────────────────────────────────────────── */

typedef struct {
    char *table_name;
    char *rows_json;       /* JSON array of row objects */
    int row_count;
    bool is_full_cache;    /* true = entire lookup table cached */
    bool is_encrypted;
} cbm_db_sample_t;

/* ── Full database introspection result ────────────────────────── */

typedef struct {
    char *database_name;
    cbm_db_dialect_t dialect;
    char *version;         /* server version string */
    char *host;            /* "localhost", IP, or file path */
    cbm_db_source_env_t source_env;
    cbm_db_domain_t detected_domain;
    char *domain_label;    /* human-readable: "E-commerce", "CMS", etc. */
    char *seed_table;      /* table used as sampling seed */

    cbm_db_schema_t *schemas;
    int schema_count;

    cbm_db_sample_t *samples;
    int sample_count;

    char *error;           /* non-NULL on failure */
} cbm_db_result_t;

/* ── Connection profile ────────────────────────────────────────── */

typedef struct {
    cbm_db_dialect_t dialect;
    char *connection_string;  /* full URI or file path */
    cbm_db_source_env_t source_env;
    bool encrypt_samples;
    int sample_depth;         /* FK chain depth for coherent sampling (default 3) */
    char *sample_seed_table;  /* override auto-detected seed table */
} cbm_db_connect_opts_t;

/* ── Introspection API ─────────────────────────────────────────── */

/* Run full database introspection. Caller must free the result with
 * cbm_db_result_free(). Returns NULL on allocation failure. The result's
 * error field is non-NULL when the introspection itself failed (bad
 * connection, missing CLI tool, etc.). */
cbm_db_result_t *cbm_db_introspect(const cbm_db_connect_opts_t *opts);

/* Free an introspection result and all its owned strings/arrays. NULL-safe. */
void cbm_db_result_free(cbm_db_result_t *r);

/* ── Domain detection ──────────────────────────────────────────── */

/* Detect the application domain from table names. Returns the domain enum
 * and writes the recommended seed table name into seed_out (up to seed_sz
 * bytes). Falls back to CBM_DB_DOMAIN_UNKNOWN with the highest FK-degree
 * table as seed when no domain fingerprint matches. */
cbm_db_domain_t cbm_db_detect_domain(const char *const *table_names, int table_count,
                                     char *seed_out, size_t seed_sz);

/* Human-readable label for a domain enum value. Static string, do not free. */
const char *cbm_db_domain_label(cbm_db_domain_t domain);

/* ── Lookup table classification ───────────────────────────────── */

/* Returns true when a table should be fully cached as a lookup/reference
 * table. Decision based on row count (< 100) OR naming patterns matching
 * *_types, *_roles, *_status, *_permissions, *_categories, *_config,
 * *_settings. */
bool cbm_db_is_lookup_table(const char *table_name, int64_t row_count);

/* ── Sensitive column masking ──────────────────────────────────── */

/* Returns true when a column name matches sensitive patterns (*password*,
 * *secret*, *token*, *ssn*, *credit_card*, *auth*, *api_key*). Column
 * values should be replaced with "***REDACTED***" in sample data. */
bool cbm_db_is_sensitive_column(const char *column_name);

/* ── Dialect helpers ───────────────────────────────────────────── */

/* Parse a dialect string ("postgres", "mysql", "sqlite", "mongodb").
 * Returns 0 on success and writes to *out. Returns -1 for unknown. */
int cbm_db_parse_dialect(const char *s, cbm_db_dialect_t *out);

/* Static label for a dialect. Do not free. */
const char *cbm_db_dialect_label(cbm_db_dialect_t d);

/* Parse a source environment string. Returns 0 on success. */
int cbm_db_parse_source_env(const char *s, cbm_db_source_env_t *out);

/* Static label for a source env. Do not free. */
const char *cbm_db_source_env_label(cbm_db_source_env_t e);

/* ── Dialect-specific introspection backends ───────────────────── */

/* Each backend fills the result struct. They are called by cbm_db_introspect
 * after connection string parsing. Returns 0 on success; on failure the
 * result's error field is set and the return is -1. */
int cbm_db_introspect_sqlite(const cbm_db_connect_opts_t *opts, cbm_db_result_t *out);
int cbm_db_introspect_postgres(const cbm_db_connect_opts_t *opts, cbm_db_result_t *out);
int cbm_db_introspect_mysql(const cbm_db_connect_opts_t *opts, cbm_db_result_t *out);
int cbm_db_introspect_mongodb(const cbm_db_connect_opts_t *opts, cbm_db_result_t *out);

#endif /* CBM_DB_INTROSPECT_H */
