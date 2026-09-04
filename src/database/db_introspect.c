/*
 * db_introspect.c — Core database introspection dispatcher.
 *
 * Routes introspection requests to dialect-specific backends, provides
 * domain detection heuristics, lookup-table classification, and
 * sensitive-column masking. All heavy I/O is delegated to the backends;
 * this file is pure logic and string operations.
 */
#include "database/db_introspect.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foundation/constants.h"
#include "foundation/mem.h"

/* ── Internal helpers ──────────────────────────────────────────── */

static char *istrdup(const char *s) {
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

/* Case-insensitive substring check. */
static bool icontains(const char *haystack, const char *needle) {
    if (!haystack || !needle) {
        return false;
    }
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) {
        return false;
    }
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

/* Case-insensitive string equality. */
static bool ieq(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

/* Case-insensitive suffix check. */
static bool iends_with(const char *s, const char *suffix) {
    if (!s || !suffix) {
        return false;
    }
    size_t slen = strlen(s);
    size_t sufflen = strlen(suffix);
    if (sufflen > slen) {
        return false;
    }
    return ieq(s + slen - sufflen, suffix);
}

/* ── Free helpers ──────────────────────────────────────────────── */

static void free_column(cbm_db_column_t *c) {
    if (!c) {
        return;
    }
    cbm_free(c->name);
    cbm_free(c->data_type);
    cbm_free(c->default_value);
    cbm_free(c->comment);
}

static void free_index(cbm_db_index_t *idx) {
    if (!idx) {
        return;
    }
    cbm_free(idx->name);
    cbm_free(idx->index_type);
    for (int i = 0; i < idx->column_count; i++) {
        cbm_free(idx->columns[i]);
    }
    cbm_free(idx->columns);
}

static void free_fk(cbm_db_fk_t *fk) {
    if (!fk) {
        return;
    }
    cbm_free(fk->constraint_name);
    for (int i = 0; i < fk->source_column_count; i++) {
        cbm_free(fk->source_columns[i]);
    }
    cbm_free(fk->source_columns);
    cbm_free(fk->target_table);
    cbm_free(fk->target_schema);
    for (int i = 0; i < fk->target_column_count; i++) {
        cbm_free(fk->target_columns[i]);
    }
    cbm_free(fk->target_columns);
    cbm_free(fk->on_delete);
    cbm_free(fk->on_update);
}

static void free_table(cbm_db_table_t *t) {
    if (!t) {
        return;
    }
    cbm_free(t->name);
    cbm_free(t->schema_name);
    cbm_free(t->table_type);
    cbm_free(t->comment);
    for (int i = 0; i < t->column_count; i++) {
        free_column(&t->columns[i]);
    }
    cbm_free(t->columns);
    for (int i = 0; i < t->index_count; i++) {
        free_index(&t->indexes[i]);
    }
    cbm_free(t->indexes);
    for (int i = 0; i < t->fk_count; i++) {
        free_fk(&t->foreign_keys[i]);
    }
    cbm_free(t->foreign_keys);
}

static void free_schema(cbm_db_schema_t *s) {
    if (!s) {
        return;
    }
    cbm_free(s->name);
    for (int i = 0; i < s->table_count; i++) {
        free_table(&s->tables[i]);
    }
    cbm_free(s->tables);
}

static void free_sample(cbm_db_sample_t *s) {
    if (!s) {
        return;
    }
    cbm_free(s->table_name);
    cbm_free(s->rows_json);
}

/* ── Public: Result lifecycle ──────────────────────────────────── */

void cbm_db_result_free(cbm_db_result_t *r) {
    if (!r) {
        return;
    }
    cbm_free(r->database_name);
    cbm_free(r->version);
    cbm_free(r->host);
    cbm_free(r->domain_label);
    cbm_free(r->seed_table);
    cbm_free(r->error);
    for (int i = 0; i < r->schema_count; i++) {
        free_schema(&r->schemas[i]);
    }
    cbm_free(r->schemas);
    for (int i = 0; i < r->sample_count; i++) {
        free_sample(&r->samples[i]);
    }
    cbm_free(r->samples);
    cbm_free(r);
}

/* ── Public: Dialect parsing ───────────────────────────────────── */

int cbm_db_parse_dialect(const char *s, cbm_db_dialect_t *out) {
    if (!s || !out) {
        return -1;
    }
    if (ieq(s, "postgres") || ieq(s, "postgresql")) {
        *out = CBM_DB_POSTGRES;
        return 0;
    }
    if (ieq(s, "mysql") || ieq(s, "mariadb")) {
        *out = CBM_DB_MYSQL;
        return 0;
    }
    if (ieq(s, "sqlite") || ieq(s, "sqlite3")) {
        *out = CBM_DB_SQLITE;
        return 0;
    }
    if (ieq(s, "mongodb") || ieq(s, "mongo")) {
        *out = CBM_DB_MONGODB;
        return 0;
    }
    return -1;
}

const char *cbm_db_dialect_label(cbm_db_dialect_t d) {
    switch (d) {
    case CBM_DB_SQLITE:
        return "SQLite";
    case CBM_DB_POSTGRES:
        return "PostgreSQL";
    case CBM_DB_MYSQL:
        return "MySQL";
    case CBM_DB_MONGODB:
        return "MongoDB";
    default:
        return "Unknown";
    }
}

/* ── Public: Source environment ─────────────────────────────────── */

int cbm_db_parse_source_env(const char *s, cbm_db_source_env_t *out) {
    if (!s || !out) {
        return -1;
    }
    if (ieq(s, "local")) {
        *out = CBM_DB_ENV_LOCAL;
        return 0;
    }
    if (ieq(s, "development") || ieq(s, "dev")) {
        *out = CBM_DB_ENV_DEVELOPMENT;
        return 0;
    }
    if (ieq(s, "staging") || ieq(s, "stage")) {
        *out = CBM_DB_ENV_STAGING;
        return 0;
    }
    if (ieq(s, "production") || ieq(s, "prod")) {
        *out = CBM_DB_ENV_PRODUCTION;
        return 0;
    }
    return -1;
}

const char *cbm_db_source_env_label(cbm_db_source_env_t e) {
    switch (e) {
    case CBM_DB_ENV_LOCAL:
        return "local";
    case CBM_DB_ENV_DEVELOPMENT:
        return "development";
    case CBM_DB_ENV_STAGING:
        return "staging";
    case CBM_DB_ENV_PRODUCTION:
        return "production";
    default:
        return "unknown";
    }
}

/* ── Public: Sensitive column masking ──────────────────────────── */

bool cbm_db_is_sensitive_column(const char *column_name) {
    if (!column_name) {
        return false;
    }
    static const char *const patterns[] = {
        "password", "passwd",     "secret",      "token",    "api_key",   "apikey",
        "ssn",      "credit_card","creditcard",  "auth",     "private_key",
        "access_key","secret_key","session_key", "hash",     "salt",
    };
    enum { PATTERN_COUNT = sizeof(patterns) / sizeof(patterns[0]) };
    for (int i = 0; i < PATTERN_COUNT; i++) {
        if (icontains(column_name, patterns[i])) {
            return true;
        }
    }
    return false;
}

/* ── Public: Lookup table classification ───────────────────────── */

bool cbm_db_is_lookup_table(const char *table_name, int64_t row_count) {
    enum { LOOKUP_ROW_THRESHOLD = 100 };
    if (row_count >= 0 && row_count < LOOKUP_ROW_THRESHOLD) {
        return true;
    }
    if (!table_name) {
        return false;
    }
    static const char *const suffixes[] = {
        "_types",      "_type",       "_roles",    "_role",     "_status",
        "_statuses",   "_permissions","_permission","_categories","_category",
        "_config",     "_configs",    "_settings", "_setting",  "_options",
        "_constants",  "_enums",      "_lookups",  "_lookup",   "_levels",
        "_priorities", "_states",     "_currencies",
    };
    enum { SUFFIX_COUNT = sizeof(suffixes) / sizeof(suffixes[0]) };
    for (int i = 0; i < SUFFIX_COUNT; i++) {
        if (iends_with(table_name, suffixes[i])) {
            return true;
        }
    }
    /* Also detect exact names commonly used as lookup tables. */
    static const char *const exact_names[] = {
        "permissions", "roles",    "statuses", "types",   "categories",
        "countries",   "states",   "provinces","currencies","languages",
        "timezones",   "settings", "config",   "options",
    };
    enum { EXACT_COUNT = sizeof(exact_names) / sizeof(exact_names[0]) };
    for (int i = 0; i < EXACT_COUNT; i++) {
        if (ieq(table_name, exact_names[i])) {
            return true;
        }
    }
    return false;
}

/* ── Public: Domain detection ──────────────────────────────────── */

/* Domain fingerprints: if enough table names match the marker set,
 * we classify the application domain. Each fingerprint defines the
 * marker table names, the minimum match count required, and the
 * recommended seed table for coherent sampling. */
typedef struct {
    cbm_db_domain_t domain;
    const char *const *markers;
    int marker_count;
    int min_matches;
    const char *seed_table;
} domain_fingerprint_t;

static const char *const ecommerce_markers[] = {
    "orders", "order_items", "products", "cart", "carts", "payments",
    "customers", "invoices", "shipping", "inventory",
};
static const char *const cms_markers[] = {
    "articles", "posts", "pages", "categories", "authors",
    "comments", "tags", "media", "content",
};
static const char *const saas_markers[] = {
    "tenants", "organizations", "subscriptions", "plans",
    "billing", "invoices", "workspaces",
};
static const char *const auth_markers[] = {
    "users", "roles", "permissions", "sessions", "user_roles",
    "role_permissions", "oauth_tokens",
};
static const char *const healthcare_markers[] = {
    "patients", "appointments", "providers", "diagnoses",
    "prescriptions", "medical_records", "treatments",
};

enum {
    ECOMMERCE_MARKER_COUNT = sizeof(ecommerce_markers) / sizeof(ecommerce_markers[0]),
    CMS_MARKER_COUNT = sizeof(cms_markers) / sizeof(cms_markers[0]),
    SAAS_MARKER_COUNT = sizeof(saas_markers) / sizeof(saas_markers[0]),
    AUTH_MARKER_COUNT = sizeof(auth_markers) / sizeof(auth_markers[0]),
    HEALTHCARE_MARKER_COUNT = sizeof(healthcare_markers) / sizeof(healthcare_markers[0]),
};

static const domain_fingerprint_t DOMAIN_FINGERPRINTS[] = {
    {CBM_DB_DOMAIN_ECOMMERCE, ecommerce_markers, ECOMMERCE_MARKER_COUNT, 3, "orders"},
    {CBM_DB_DOMAIN_CMS, cms_markers, CMS_MARKER_COUNT, 3, "articles"},
    {CBM_DB_DOMAIN_SAAS, saas_markers, SAAS_MARKER_COUNT, 2, "organizations"},
    {CBM_DB_DOMAIN_AUTH, auth_markers, AUTH_MARKER_COUNT, 3, "users"},
    {CBM_DB_DOMAIN_HEALTHCARE, healthcare_markers, HEALTHCARE_MARKER_COUNT, 3, "patients"},
};
enum { DOMAIN_COUNT = sizeof(DOMAIN_FINGERPRINTS) / sizeof(DOMAIN_FINGERPRINTS[0]) };

cbm_db_domain_t cbm_db_detect_domain(const char *const *table_names, int table_count,
                                     char *seed_out, size_t seed_sz) {
    if (!table_names || table_count <= 0) {
        if (seed_out && seed_sz > 0) {
            seed_out[0] = '\0';
        }
        return CBM_DB_DOMAIN_UNKNOWN;
    }

    /* Score each domain fingerprint. */
    int best_score = 0;
    int best_idx = -1;
    for (int d = 0; d < DOMAIN_COUNT; d++) {
        int score = 0;
        for (int m = 0; m < DOMAIN_FINGERPRINTS[d].marker_count; m++) {
            for (int t = 0; t < table_count; t++) {
                if (ieq(table_names[t], DOMAIN_FINGERPRINTS[d].markers[m])) {
                    score++;
                    break;
                }
            }
        }
        if (score >= DOMAIN_FINGERPRINTS[d].min_matches && score > best_score) {
            best_score = score;
            best_idx = d;
        }
    }

    if (best_idx >= 0) {
        if (seed_out && seed_sz > 0) {
            snprintf(seed_out, seed_sz, "%s", DOMAIN_FINGERPRINTS[best_idx].seed_table);
        }
        return DOMAIN_FINGERPRINTS[best_idx].domain;
    }

    /* Unknown domain: pick the first table as a fallback seed.
     * The caller should ideally use FK-degree centrality, but that requires
     * FK information not available at this point. */
    if (seed_out && seed_sz > 0 && table_count > 0) {
        snprintf(seed_out, seed_sz, "%s", table_names[0]);
    }
    return CBM_DB_DOMAIN_UNKNOWN;
}

const char *cbm_db_domain_label(cbm_db_domain_t domain) {
    switch (domain) {
    case CBM_DB_DOMAIN_ECOMMERCE:
        return "E-commerce";
    case CBM_DB_DOMAIN_CMS:
        return "CMS / Publishing";
    case CBM_DB_DOMAIN_SAAS:
        return "SaaS / Multi-tenant";
    case CBM_DB_DOMAIN_AUTH:
        return "Auth / IAM";
    case CBM_DB_DOMAIN_HEALTHCARE:
        return "Healthcare";
    case CBM_DB_DOMAIN_UNKNOWN:
    default:
        return "Generic";
    }
}

/* ── Public: Introspection dispatcher ──────────────────────────── */

cbm_db_result_t *cbm_db_introspect(const cbm_db_connect_opts_t *opts) {
    if (!opts || !opts->connection_string) {
        return NULL;
    }

    cbm_db_result_t *result = cbm_calloc(1, sizeof(cbm_db_result_t));
    if (!result) {
        return NULL;
    }

    result->dialect = opts->dialect;
    result->source_env = opts->source_env;

    int rc;
    switch (opts->dialect) {
    case CBM_DB_SQLITE:
        rc = cbm_db_introspect_sqlite(opts, result);
        break;
    case CBM_DB_POSTGRES:
        rc = cbm_db_introspect_postgres(opts, result);
        break;
    case CBM_DB_MYSQL:
        rc = cbm_db_introspect_mysql(opts, result);
        break;
    case CBM_DB_MONGODB:
        rc = cbm_db_introspect_mongodb(opts, result);
        break;
    default:
        result->error = istrdup("unsupported database dialect");
        return result;
    }

    if (rc != 0) {
        return result; /* error field already set by backend */
    }

    /* Domain detection: collect all table names across schemas. */
    int total_tables = 0;
    for (int s = 0; s < result->schema_count; s++) {
        total_tables += result->schemas[s].table_count;
    }

    if (total_tables > 0) {
        const char **all_names = cbm_calloc((size_t)total_tables, sizeof(char *));
        if (all_names) {
            int idx = 0;
            for (int s = 0; s < result->schema_count; s++) {
                for (int t = 0; t < result->schemas[s].table_count; t++) {
                    all_names[idx++] = result->schemas[s].tables[t].name;
                }
            }

            char seed[CBM_SZ_256];
            result->detected_domain = cbm_db_detect_domain(all_names, total_tables,
                                                           seed, sizeof(seed));
            result->domain_label = istrdup(cbm_db_domain_label(result->detected_domain));

            /* Use user override if provided, otherwise auto-detected seed. */
            if (opts->sample_seed_table && opts->sample_seed_table[0]) {
                result->seed_table = istrdup(opts->sample_seed_table);
            } else {
                result->seed_table = istrdup(seed);
            }
            cbm_free(all_names);
        }
    }

    /* Mark lookup tables. */
    for (int s = 0; s < result->schema_count; s++) {
        for (int t = 0; t < result->schemas[s].table_count; t++) {
            cbm_db_table_t *tbl = &result->schemas[s].tables[t];
            tbl->is_lookup = cbm_db_is_lookup_table(tbl->name, tbl->row_count_estimate);
        }
    }

    return result;
}
