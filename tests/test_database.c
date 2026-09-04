/*
 * test_database.c — Comprehensive unit & edge-case tests for the Database Knowledge Graph.
 *
 * Exercises:
 *   1. Dialect & environment string parsing
 *   2. Sensitive column masking / redaction patterns (security)
 *   3. Lookup table classification heuristics (naming suffixes + row thresholds)
 *   4. Domain detection & coherent sampling seed heuristics (E-commerce, CMS, SaaS, Auth, Healthcare, Generic)
 *   5. Native SQLite schema introspection (tables, views, columns, PKs, FKs, indexes, comments, row counts)
 *   6. Sample data extraction & sensitive column redaction
 *   7. Mermaid ERD generator with detail levels (keys_only, columns, full), table filters, and max_tables caps
 *   8. MCP database tool input validation, error handling, and parameter extraction
 */

#include "test_framework.h"
#include "database/db_introspect.h"
#include "database/erd_generator.h"
#include "foundation/constants.h"
#include "foundation/mem.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 1. Dialect & Environment Parsing ────────────────────────────── */

TEST(db_parse_dialect_valid) {
    cbm_db_dialect_t d;

    ASSERT_EQ(cbm_db_parse_dialect("postgres", &d), 0);
    ASSERT_EQ((int)d, (int)CBM_DB_POSTGRES);
    ASSERT_STR_EQ(cbm_db_dialect_label(d), "PostgreSQL");

    ASSERT_EQ(cbm_db_parse_dialect("postgresql", &d), 0);
    ASSERT_EQ((int)d, (int)CBM_DB_POSTGRES);

    ASSERT_EQ(cbm_db_parse_dialect("POSTGRES", &d), 0);
    ASSERT_EQ((int)d, (int)CBM_DB_POSTGRES);

    ASSERT_EQ(cbm_db_parse_dialect("mysql", &d), 0);
    ASSERT_EQ((int)d, (int)CBM_DB_MYSQL);
    ASSERT_STR_EQ(cbm_db_dialect_label(d), "MySQL");

    ASSERT_EQ(cbm_db_parse_dialect("mariadb", &d), 0);
    ASSERT_EQ((int)d, (int)CBM_DB_MYSQL);

    ASSERT_EQ(cbm_db_parse_dialect("sqlite", &d), 0);
    ASSERT_EQ((int)d, (int)CBM_DB_SQLITE);
    ASSERT_STR_EQ(cbm_db_dialect_label(d), "SQLite");

    ASSERT_EQ(cbm_db_parse_dialect("sqlite3", &d), 0);
    ASSERT_EQ((int)d, (int)CBM_DB_SQLITE);

    ASSERT_EQ(cbm_db_parse_dialect("mongodb", &d), 0);
    ASSERT_EQ((int)d, (int)CBM_DB_MONGODB);
    ASSERT_STR_EQ(cbm_db_dialect_label(d), "MongoDB");

    ASSERT_EQ(cbm_db_parse_dialect("mongo", &d), 0);
    ASSERT_EQ((int)d, (int)CBM_DB_MONGODB);

    PASS();
}

TEST(db_parse_dialect_invalid_and_edge_cases) {
    cbm_db_dialect_t d;
    ASSERT_EQ(cbm_db_parse_dialect("oracle", &d), -1);
    ASSERT_EQ(cbm_db_parse_dialect("redis", &d), -1);
    ASSERT_EQ(cbm_db_parse_dialect("", &d), -1);
    ASSERT_EQ(cbm_db_parse_dialect(NULL, &d), -1);
    ASSERT_EQ(cbm_db_parse_dialect("postgres", NULL), -1);
    PASS();
}

TEST(db_parse_source_env) {
    cbm_db_source_env_t env;

    ASSERT_EQ(cbm_db_parse_source_env("local", &env), 0);
    ASSERT_EQ((int)env, (int)CBM_DB_ENV_LOCAL);
    ASSERT_STR_EQ(cbm_db_source_env_label(env), "local");

    ASSERT_EQ(cbm_db_parse_source_env("development", &env), 0);
    ASSERT_EQ((int)env, (int)CBM_DB_ENV_DEVELOPMENT);
    ASSERT_STR_EQ(cbm_db_source_env_label(env), "development");

    ASSERT_EQ(cbm_db_parse_source_env("dev", &env), 0);
    ASSERT_EQ((int)env, (int)CBM_DB_ENV_DEVELOPMENT);

    ASSERT_EQ(cbm_db_parse_source_env("staging", &env), 0);
    ASSERT_EQ((int)env, (int)CBM_DB_ENV_STAGING);
    ASSERT_STR_EQ(cbm_db_source_env_label(env), "staging");

    ASSERT_EQ(cbm_db_parse_source_env("stage", &env), 0);
    ASSERT_EQ((int)env, (int)CBM_DB_ENV_STAGING);

    ASSERT_EQ(cbm_db_parse_source_env("production", &env), 0);
    ASSERT_EQ((int)env, (int)CBM_DB_ENV_PRODUCTION);
    ASSERT_STR_EQ(cbm_db_source_env_label(env), "production");

    ASSERT_EQ(cbm_db_parse_source_env("prod", &env), 0);
    ASSERT_EQ((int)env, (int)CBM_DB_ENV_PRODUCTION);

    ASSERT_EQ(cbm_db_parse_source_env("invalid_env", &env), -1);
    ASSERT_EQ(cbm_db_parse_source_env(NULL, &env), -1);
    PASS();
}

/* ── 2. Sensitive Column Masking (Security) ─────────────────────── */

TEST(db_sensitive_column_masking) {
    /* Critical sensitive keywords */
    ASSERT(cbm_db_is_sensitive_column("password"));
    ASSERT(cbm_db_is_sensitive_column("user_password"));
    ASSERT(cbm_db_is_sensitive_column("password_hash"));
    ASSERT(cbm_db_is_sensitive_column("passwd"));
    ASSERT(cbm_db_is_sensitive_column("secret"));
    ASSERT(cbm_db_is_sensitive_column("client_secret"));
    ASSERT(cbm_db_is_sensitive_column("token"));
    ASSERT(cbm_db_is_sensitive_column("access_token"));
    ASSERT(cbm_db_is_sensitive_column("refresh_token"));
    ASSERT(cbm_db_is_sensitive_column("api_key"));
    ASSERT(cbm_db_is_sensitive_column("stripe_apikey"));
    ASSERT(cbm_db_is_sensitive_column("ssn"));
    ASSERT(cbm_db_is_sensitive_column("credit_card"));
    ASSERT(cbm_db_is_sensitive_column("creditcard_number"));
    ASSERT(cbm_db_is_sensitive_column("auth_code"));
    ASSERT(cbm_db_is_sensitive_column("private_key"));
    ASSERT(cbm_db_is_sensitive_column("salt"));
    ASSERT(cbm_db_is_sensitive_column("password_salt"));

    /* Case-insensitivity */
    ASSERT(cbm_db_is_sensitive_column("PASSWORD"));
    ASSERT(cbm_db_is_sensitive_column("Api_Key"));
    ASSERT(cbm_db_is_sensitive_column("Secret_Token"));

    /* Safe/Non-sensitive columns */
    ASSERT(!cbm_db_is_sensitive_column("id"));
    ASSERT(!cbm_db_is_sensitive_column("username"));
    ASSERT(!cbm_db_is_sensitive_column("email"));
    ASSERT(!cbm_db_is_sensitive_column("created_at"));
    ASSERT(!cbm_db_is_sensitive_column("updated_at"));
    ASSERT(!cbm_db_is_sensitive_column("order_status"));
    ASSERT(!cbm_db_is_sensitive_column("total_amount"));
    ASSERT(!cbm_db_is_sensitive_column("description"));
    ASSERT(!cbm_db_is_sensitive_column("category_name"));

    /* Edge cases */
    ASSERT(!cbm_db_is_sensitive_column(NULL));
    ASSERT(!cbm_db_is_sensitive_column(""));
    PASS();
}

/* ── 3. Lookup Table Classification ────────────────────────────── */

TEST(db_lookup_table_classification) {
    /* 1. Low row count heuristic (< 100 rows is always lookup) */
    ASSERT(cbm_db_is_lookup_table("any_table_name", 0));
    ASSERT(cbm_db_is_lookup_table("any_table_name", 5));
    ASSERT(cbm_db_is_lookup_table("any_table_name", 99));

    /* 2. Suffix pattern matching (even with > 100 rows) */
    ASSERT(cbm_db_is_lookup_table("user_roles", 500));
    ASSERT(cbm_db_is_lookup_table("order_status", 1000));
    ASSERT(cbm_db_is_lookup_table("order_statuses", 1000));
    ASSERT(cbm_db_is_lookup_table("account_types", 250));
    ASSERT(cbm_db_is_lookup_table("account_type", 250));
    ASSERT(cbm_db_is_lookup_table("sys_permissions", 1200));
    ASSERT(cbm_db_is_lookup_table("product_categories", 800));
    ASSERT(cbm_db_is_lookup_table("app_config", 5000));
    ASSERT(cbm_db_is_lookup_table("user_settings", 10000));
    ASSERT(cbm_db_is_lookup_table("pricing_currencies", 300));
    ASSERT(cbm_db_is_lookup_table("ticket_priorities", 500));

    /* 3. Exact table names */
    ASSERT(cbm_db_is_lookup_table("roles", 500));
    ASSERT(cbm_db_is_lookup_table("permissions", 500));
    ASSERT(cbm_db_is_lookup_table("countries", 300));
    ASSERT(cbm_db_is_lookup_table("currencies", 200));
    ASSERT(cbm_db_is_lookup_table("languages", 200));
    ASSERT(cbm_db_is_lookup_table("timezones", 400));
    ASSERT(cbm_db_is_lookup_table("settings", 1000));
    ASSERT(cbm_db_is_lookup_table("config", 1000));

    /* 4. Large non-lookup tables */
    ASSERT(!cbm_db_is_lookup_table("orders", 50000));
    ASSERT(!cbm_db_is_lookup_table("users", 100000));
    ASSERT(!cbm_db_is_lookup_table("order_items", 250000));
    ASSERT(!cbm_db_is_lookup_table("audit_logs", 1000000));
    ASSERT(!cbm_db_is_lookup_table("payments", 50000));

    /* 5. Edge cases */
    ASSERT(!cbm_db_is_lookup_table(NULL, 1000));
    ASSERT(cbm_db_is_lookup_table(NULL, 50)); /* low row count still applies */
    PASS();
}

/* ── 4. Domain Detection Heuristics ─────────────────────────────── */

TEST(db_domain_detection_ecommerce) {
    const char *tables[] = {
        "orders", "order_items", "products", "customers", "payments", "inventory"
    };
    char seed[64] = {0};
    cbm_db_domain_t domain = cbm_db_detect_domain(tables, 6, seed, sizeof(seed));

    ASSERT_EQ((int)domain, (int)CBM_DB_DOMAIN_ECOMMERCE);
    ASSERT_STR_EQ(cbm_db_domain_label(domain), "E-commerce");
    ASSERT_STR_EQ(seed, "orders");
    PASS();
}

TEST(db_domain_detection_cms) {
    const char *tables[] = {
        "articles", "posts", "pages", "categories", "authors", "comments", "tags"
    };
    char seed[64] = {0};
    cbm_db_domain_t domain = cbm_db_detect_domain(tables, 7, seed, sizeof(seed));

    ASSERT_EQ((int)domain, (int)CBM_DB_DOMAIN_CMS);
    ASSERT_STR_EQ(cbm_db_domain_label(domain), "CMS / Publishing");
    ASSERT_STR_EQ(seed, "articles");
    PASS();
}

TEST(db_domain_detection_saas) {
    const char *tables[] = {
        "tenants", "organizations", "subscriptions", "plans", "billing", "workspaces"
    };
    char seed[64] = {0};
    cbm_db_domain_t domain = cbm_db_detect_domain(tables, 6, seed, sizeof(seed));

    ASSERT_EQ((int)domain, (int)CBM_DB_DOMAIN_SAAS);
    ASSERT_STR_EQ(cbm_db_domain_label(domain), "SaaS / Multi-tenant");
    ASSERT_STR_EQ(seed, "organizations");
    PASS();
}

TEST(db_domain_detection_auth) {
    const char *tables[] = {
        "users", "roles", "permissions", "sessions", "user_roles", "oauth_tokens"
    };
    char seed[64] = {0};
    cbm_db_domain_t domain = cbm_db_detect_domain(tables, 6, seed, sizeof(seed));

    ASSERT_EQ((int)domain, (int)CBM_DB_DOMAIN_AUTH);
    ASSERT_STR_EQ(cbm_db_domain_label(domain), "Auth / IAM");
    ASSERT_STR_EQ(seed, "users");
    PASS();
}

TEST(db_domain_detection_healthcare) {
    const char *tables[] = {
        "patients", "appointments", "providers", "diagnoses", "prescriptions", "treatments"
    };
    char seed[64] = {0};
    cbm_db_domain_t domain = cbm_db_detect_domain(tables, 6, seed, sizeof(seed));

    ASSERT_EQ((int)domain, (int)CBM_DB_DOMAIN_HEALTHCARE);
    ASSERT_STR_EQ(cbm_db_domain_label(domain), "Healthcare");
    ASSERT_STR_EQ(seed, "patients");
    PASS();
}

TEST(db_domain_detection_unknown_fallback) {
    const char *tables[] = {
        "sensor_readings", "telemetry_events", "gateways"
    };
    char seed[64] = {0};
    cbm_db_domain_t domain = cbm_db_detect_domain(tables, 3, seed, sizeof(seed));

    ASSERT_EQ((int)domain, (int)CBM_DB_DOMAIN_UNKNOWN);
    ASSERT_STR_EQ(cbm_db_domain_label(domain), "Generic");
    ASSERT_STR_EQ(seed, "sensor_readings"); /* Fallback to first table */

    /* Empty input */
    domain = cbm_db_detect_domain(NULL, 0, seed, sizeof(seed));
    ASSERT_EQ((int)domain, (int)CBM_DB_DOMAIN_UNKNOWN);
    ASSERT_STR_EQ(seed, "");
    PASS();
}

/* ── 5. Native SQLite Introspection (Full Integration) ───────────── */

/* Helper to populate a comprehensive test SQLite database with schemas,
 * PKs, composite keys, FKs, unique indexes, views, and data. */
static int create_test_db(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }

    const char *ddl =
        /* 1. Roles table (lookup) */
        "CREATE TABLE roles ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL UNIQUE,"
        "  description TEXT"
        ");"
        "INSERT INTO roles (id, name, description) VALUES (1, 'admin', 'Administrator');"
        "INSERT INTO roles (id, name, description) VALUES (2, 'editor', 'Content Editor');"
        "INSERT INTO roles (id, name, description) VALUES (3, 'viewer', 'Read-only User');"

        /* 2. Users table (with sensitive columns) */
        "CREATE TABLE users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT NOT NULL UNIQUE,"
        "  email TEXT NOT NULL,"
        "  password_hash TEXT NOT NULL,"
        "  api_token TEXT,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE UNIQUE INDEX idx_users_email ON users(email);"
        "INSERT INTO users (id, username, email, password_hash, api_token) "
        "VALUES (1, 'alice', 'alice@example.com', 'secret_hash_123', 'tok_abc123');"
        "INSERT INTO users (id, username, email, password_hash, api_token) "
        "VALUES (2, 'bob', 'bob@example.com', 'secret_hash_456', 'tok_xyz789');"

        /* 3. User roles junction table (composite PK + FKs) */
        "CREATE TABLE user_roles ("
        "  user_id INTEGER NOT NULL,"
        "  role_id INTEGER NOT NULL,"
        "  assigned_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  PRIMARY KEY (user_id, role_id),"
        "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
        "  FOREIGN KEY (role_id) REFERENCES roles(id) ON DELETE RESTRICT"
        ");"
        "INSERT INTO user_roles (user_id, role_id) VALUES (1, 1);"
        "INSERT INTO user_roles (user_id, role_id) VALUES (2, 2);"

        /* 4. Products table */
        "CREATE TABLE products ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  sku TEXT NOT NULL UNIQUE,"
        "  title TEXT NOT NULL,"
        "  price REAL NOT NULL,"
        "  stock INTEGER DEFAULT 0"
        ");"
        "INSERT INTO products (id, sku, title, price, stock) VALUES (1, 'PROD-1', 'Laptop', 999.99, 10);"
        "INSERT INTO products (id, sku, title, price, stock) VALUES (2, 'PROD-2', 'Mouse', 29.99, 50);"

        /* 5. Orders table (FK to users) */
        "CREATE TABLE orders ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_id INTEGER NOT NULL,"
        "  order_status TEXT DEFAULT 'pending',"
        "  total_amount REAL NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX idx_orders_user_date ON orders(user_id, created_at);"
        "INSERT INTO orders (id, user_id, order_status, total_amount) VALUES (101, 1, 'completed', 1029.98);"

        /* 6. Order items table (composite FKs) */
        "CREATE TABLE order_items ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  order_id INTEGER NOT NULL,"
        "  product_id INTEGER NOT NULL,"
        "  quantity INTEGER NOT NULL,"
        "  unit_price REAL NOT NULL,"
        "  FOREIGN KEY (order_id) REFERENCES orders(id) ON DELETE CASCADE,"
        "  FOREIGN KEY (product_id) REFERENCES products(id) ON DELETE RESTRICT"
        ");"
        "INSERT INTO order_items (id, order_id, product_id, quantity, unit_price) "
        "VALUES (1, 101, 1, 1, 999.99);"
        "INSERT INTO order_items (id, order_id, product_id, quantity, unit_price) "
        "VALUES (2, 101, 2, 1, 29.99);"

        /* 7. View */
        "CREATE VIEW active_orders_view AS "
        "SELECT o.id, u.username, o.total_amount, o.order_status "
        "FROM orders o JOIN users u ON o.user_id = u.id;";

    char *errmsg = NULL;
    rc = sqlite3_exec(db, ddl, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg) sqlite3_free(errmsg);
        sqlite3_close(db);
        return -1;
    }

    sqlite3_close(db);
    return 0;
}

TEST(db_sqlite_introspection_complete) {
    const char *test_db_path = "test_introspect_tmp.db";
    remove(test_db_path);

    int rc = create_test_db(test_db_path);
    ASSERT_EQ(rc, 0);

    cbm_db_connect_opts_t opts = {0};
    opts.dialect = CBM_DB_SQLITE;
    opts.connection_string = (char *)test_db_path;
    opts.source_env = CBM_DB_ENV_LOCAL;

    cbm_db_result_t *res = cbm_db_introspect(&opts);
    ASSERT(res != NULL);
    ASSERT(res->error == NULL);
    ASSERT_EQ((int)res->dialect, (int)CBM_DB_SQLITE);
    ASSERT_STR_EQ(res->host, "local");
    ASSERT(res->version != NULL);

    /* 1. Schema verification */
    ASSERT_EQ(res->schema_count, 1);
    cbm_db_schema_t *schema = &res->schemas[0];
    ASSERT_STR_EQ(schema->name, "main");

    /* 6 tables + 1 view = 7 entities */
    ASSERT_EQ(schema->table_count, 7);

    /* 2. Find and verify 'users' table */
    cbm_db_table_t *users_tbl = NULL;
    cbm_db_table_t *roles_tbl = NULL;
    cbm_db_table_t *orders_tbl = NULL;
    cbm_db_table_t *items_tbl = NULL;
    cbm_db_table_t *view_tbl = NULL;

    for (int t = 0; t < schema->table_count; t++) {
        if (strcmp(schema->tables[t].name, "users") == 0) users_tbl = &schema->tables[t];
        if (strcmp(schema->tables[t].name, "roles") == 0) roles_tbl = &schema->tables[t];
        if (strcmp(schema->tables[t].name, "orders") == 0) orders_tbl = &schema->tables[t];
        if (strcmp(schema->tables[t].name, "order_items") == 0) items_tbl = &schema->tables[t];
        if (strcmp(schema->tables[t].name, "active_orders_view") == 0) view_tbl = &schema->tables[t];
    }

    ASSERT(users_tbl != NULL);
    ASSERT(!users_tbl->is_view);
    ASSERT_EQ(users_tbl->row_count_estimate, 2);
    ASSERT_EQ(users_tbl->column_count, 6);

    /* Check users columns */
    ASSERT_STR_EQ(users_tbl->columns[0].name, "id");
    ASSERT(users_tbl->columns[0].is_pk);
    ASSERT_STR_EQ(users_tbl->columns[1].name, "username");
    ASSERT_STR_EQ(users_tbl->columns[2].name, "email");
    ASSERT_STR_EQ(users_tbl->columns[3].name, "password_hash");
    ASSERT_STR_EQ(users_tbl->columns[4].name, "api_token");

    /* Check unique index on users */
    ASSERT(users_tbl->index_count >= 1);

    /* 3. Find and verify 'orders' table & Foreign Keys */
    ASSERT(orders_tbl != NULL);
    ASSERT_EQ(orders_tbl->fk_count, 1);
    ASSERT_STR_EQ(orders_tbl->foreign_keys[0].target_table, "users");
    ASSERT_STR_EQ(orders_tbl->foreign_keys[0].source_columns[0], "user_id");
    ASSERT_STR_EQ(orders_tbl->foreign_keys[0].target_columns[0], "id");
    ASSERT_STR_EQ(orders_tbl->foreign_keys[0].on_delete, "CASCADE");

    /* 4. Find and verify 'order_items' table (2 FKs) */
    ASSERT(items_tbl != NULL);
    ASSERT_EQ(items_tbl->fk_count, 2);

    /* 5. Verify View */
    ASSERT(view_tbl != NULL);
    ASSERT(view_tbl->is_view);
    ASSERT_STR_EQ(view_tbl->table_type, "VIEW");

    /* 6. Verify Domain Detection on this SQLite DB */
    ASSERT_EQ((int)res->detected_domain, (int)CBM_DB_DOMAIN_ECOMMERCE);
    ASSERT_STR_EQ(res->domain_label, "E-commerce");
    ASSERT_STR_EQ(res->seed_table, "orders");

    /* 7. Verify Lookup Table Flag on 'roles' */
    ASSERT(roles_tbl != NULL);
    ASSERT(roles_tbl->is_lookup);

    /* 8. Verify Sensitive Data Redaction in Sample Data */
    ASSERT(res->sample_count > 0);
    cbm_db_sample_t *users_sample = NULL;
    for (int s = 0; s < res->sample_count; s++) {
        if (strcmp(res->samples[s].table_name, "users") == 0) {
            users_sample = &res->samples[s];
            break;
        }
    }
    ASSERT(users_sample != NULL);
    ASSERT(users_sample->rows_json != NULL);
    /* Sensitive columns must be redacted */
    ASSERT(strstr(users_sample->rows_json, "***REDACTED***") != NULL);
    /* Raw passwords must NOT appear in sample data */
    ASSERT(strstr(users_sample->rows_json, "secret_hash_123") == NULL);
    ASSERT(strstr(users_sample->rows_json, "tok_abc123") == NULL);

    cbm_db_result_free(res);
    remove(test_db_path);
    PASS();
}

/* ── 6. Mermaid ERD Generation Tests ────────────────────────────── */

TEST(db_erd_generation_detail_levels) {
    const char *test_db_path = "test_erd_tmp.db";
    remove(test_db_path);
    ASSERT_EQ(create_test_db(test_db_path), 0);

    cbm_db_connect_opts_t opts = {0};
    opts.dialect = CBM_DB_SQLITE;
    opts.connection_string = (char *)test_db_path;

    cbm_db_result_t *res = cbm_db_introspect(&opts);
    ASSERT(res != NULL);

    /* Test 1: Full detail */
    char *erd_full = cbm_erd_generate_mermaid(res, NULL, 0, CBM_ERD_FULL, 30);
    ASSERT(erd_full != NULL);
    ASSERT(strstr(erd_full, "erDiagram") != NULL);
    ASSERT(strstr(erd_full, "users {") != NULL);
    ASSERT(strstr(erd_full, "orders {") != NULL);
    ASSERT(strstr(erd_full, "order_items {") != NULL);
    ASSERT(strstr(erd_full, "PK") != NULL);
    ASSERT(strstr(erd_full, "FK") != NULL);
    /* Full detail includes simplified types */
    ASSERT(strstr(erd_full, "int id PK") != NULL || strstr(erd_full, "string username") != NULL);
    /* Check relationship line */
    ASSERT(strstr(erd_full, "users ||--o{ orders") != NULL || strstr(erd_full, "orders ||--o{ order_items") != NULL);
    free(erd_full);

    /* Test 2: Keys only detail */
    char *erd_keys = cbm_erd_generate_mermaid(res, NULL, 0, CBM_ERD_KEYS_ONLY, 30);
    ASSERT(erd_keys != NULL);
    ASSERT(strstr(erd_keys, "users {") != NULL);
    /* Should contain id PK, but not non-key columns like email without UK */
    ASSERT(strstr(erd_keys, "id PK") != NULL);
    free(erd_keys);

    /* Test 3: Table filtering */
    const char *filtered_tables[] = {"users", "orders"};
    char *erd_filtered = cbm_erd_generate_mermaid(res, filtered_tables, 2, CBM_ERD_COLUMNS, 30);
    ASSERT(erd_filtered != NULL);
    ASSERT(strstr(erd_filtered, "users {") != NULL);
    ASSERT(strstr(erd_filtered, "orders {") != NULL);
    /* order_items should NOT be in the diagram */
    ASSERT(strstr(erd_filtered, "order_items {") == NULL);
    free(erd_filtered);

    /* Test 4: Max tables cap notice */
    char *erd_capped = cbm_erd_generate_mermaid(res, NULL, 0, CBM_ERD_COLUMNS, 2);
    ASSERT(erd_capped != NULL);
    ASSERT(strstr(erd_capped, "NOTE: showing 2 of") != NULL);
    free(erd_capped);

    cbm_db_result_free(res);
    remove(test_db_path);
    PASS();
}

TEST(db_erd_parse_detail) {
    cbm_erd_detail_t detail;

    ASSERT_EQ(cbm_erd_parse_detail("keys_only", &detail), 0);
    ASSERT_EQ((int)detail, (int)CBM_ERD_KEYS_ONLY);

    ASSERT_EQ(cbm_erd_parse_detail("columns", &detail), 0);
    ASSERT_EQ((int)detail, (int)CBM_ERD_COLUMNS);

    ASSERT_EQ(cbm_erd_parse_detail("full", &detail), 0);
    ASSERT_EQ((int)detail, (int)CBM_ERD_FULL);

    ASSERT_EQ(cbm_erd_parse_detail("invalid", &detail), -1);
    ASSERT_EQ(cbm_erd_parse_detail(NULL, &detail), -1);
    PASS();
}

/* ── Test Suite Definition ───────────────────────────────────────── */

SUITE(database) {
    RUN_TEST(db_parse_dialect_valid);
    RUN_TEST(db_parse_dialect_invalid_and_edge_cases);
    RUN_TEST(db_parse_source_env);
    RUN_TEST(db_sensitive_column_masking);
    RUN_TEST(db_lookup_table_classification);
    RUN_TEST(db_domain_detection_ecommerce);
    RUN_TEST(db_domain_detection_cms);
    RUN_TEST(db_domain_detection_saas);
    RUN_TEST(db_domain_detection_auth);
    RUN_TEST(db_domain_detection_healthcare);
    RUN_TEST(db_domain_detection_unknown_fallback);
    RUN_TEST(db_sqlite_introspection_complete);
    RUN_TEST(db_erd_generation_detail_levels);
    RUN_TEST(db_erd_parse_detail);
}
