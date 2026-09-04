/*
 * db_mongodb_introspect.c — MongoDB database introspector via mongosh CLI.
 *
 * Uses popen to execute mongosh commands, extracting collection lists,
 * index definitions, and document samples to infer schema structure.
 */
#include "database/db_introspect.h"

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

/* ── mongosh execution ─────────────────────────────────────────── */

static int run_mongosh(const char *connection_string, const char *js_code,
                       char **output_out) {
    char cmd[CBM_SZ_8K];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
             "mongosh \"%s\" --quiet --eval \"%s\" 2>NUL",
             connection_string, js_code);
#else
    snprintf(cmd, sizeof(cmd),
             "mongosh '%s' --quiet --eval '%s' 2>/dev/null",
             connection_string, js_code);
#endif

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

/* ── Infer field types from a sampled document set ─────────────── */

/* Map a BSON type name to a simplified data type string. */
static const char *bson_type_to_str(const char *bson_type) {
    if (!bson_type) {
        return "unknown";
    }
    if (strcmp(bson_type, "String") == 0 || strcmp(bson_type, "string") == 0) {
        return "String";
    }
    if (strcmp(bson_type, "Int32") == 0 || strcmp(bson_type, "int") == 0) {
        return "Int32";
    }
    if (strcmp(bson_type, "Double") == 0 || strcmp(bson_type, "double") == 0) {
        return "Double";
    }
    if (strcmp(bson_type, "Boolean") == 0 || strcmp(bson_type, "bool") == 0) {
        return "Boolean";
    }
    if (strcmp(bson_type, "ObjectId") == 0 || strcmp(bson_type, "objectId") == 0) {
        return "ObjectId";
    }
    if (strcmp(bson_type, "Date") == 0 || strcmp(bson_type, "date") == 0) {
        return "Date";
    }
    if (strcmp(bson_type, "Array") == 0 || strcmp(bson_type, "array") == 0) {
        return "Array";
    }
    if (strcmp(bson_type, "Object") == 0 || strcmp(bson_type, "object") == 0) {
        return "Object";
    }
    return bson_type;
}

/* ── Public: MongoDB introspection backend ─────────────────────── */

int cbm_db_introspect_mongodb(const cbm_db_connect_opts_t *opts, cbm_db_result_t *out) {
    if (!opts || !opts->connection_string || !out) {
        if (out) {
            out->error = sdup("invalid arguments for MongoDB introspection");
        }
        return -1;
    }

    /* Test connection and get database name. */
    char *dbname_out = NULL;
    if (run_mongosh(opts->connection_string, "db.getName()", &dbname_out) != 0) {
        out->error = sdup("failed to connect to MongoDB. Ensure mongosh is installed "
                          "and the connection string is valid (e.g. mongodb://host/db)");
        return -1;
    }
    out->database_name = sdup(trim(dbname_out));
    cbm_free(dbname_out);
    out->host = sdup("remote");

    /* Get server version. */
    char *ver_out = NULL;
    if (run_mongosh(opts->connection_string, "db.version()", &ver_out) == 0) {
        out->version = sdup(trim(ver_out));
        cbm_free(ver_out);
    } else {
        out->version = sdup("unknown");
    }

    /* List collections. */
    char *coll_out = NULL;
    const char *coll_js =
        "JSON.stringify(db.getCollectionNames())";
    if (run_mongosh(opts->connection_string, coll_js, &coll_out) != 0) {
        out->error = sdup("failed to list MongoDB collections");
        return -1;
    }

    /* Parse collection names JSON array. */
    yyjson_doc *coll_doc = yyjson_read(trim(coll_out), strlen(trim(coll_out)), 0);
    cbm_free(coll_out);
    if (!coll_doc) {
        out->error = sdup("failed to parse MongoDB collection list");
        return -1;
    }

    yyjson_val *coll_arr = yyjson_doc_get_root(coll_doc);
    int coll_count = yyjson_arr_size(coll_arr);

    /* MongoDB uses a single implicit schema. */
    out->schema_count = 1;
    out->schemas = cbm_calloc(1, sizeof(cbm_db_schema_t));
    out->schemas[0].name = sdup("default");
    out->schemas[0].tables = cbm_calloc((size_t)(coll_count > 0 ? coll_count : 1),
                                        sizeof(cbm_db_table_t));

    int ti = 0;
    yyjson_val *coll_val;
    yyjson_arr_iter coll_iter;
    yyjson_arr_iter_init(coll_arr, &coll_iter);
    while ((coll_val = yyjson_arr_iter_next(&coll_iter)) != NULL && ti < coll_count) {
        const char *cname = yyjson_get_str(coll_val);
        if (!cname) {
            continue;
        }
        /* Skip system collections. */
        if (strncmp(cname, "system.", CBM_SZ_7) == 0) {
            continue;
        }

        cbm_db_table_t *tbl = &out->schemas[0].tables[ti];
        tbl->name = sdup(cname);
        tbl->schema_name = sdup("default");
        tbl->table_type = sdup("COLLECTION");
        tbl->is_view = false;
        tbl->comment = NULL;

        /* Document count. */
        char count_js[CBM_SZ_512];
        snprintf(count_js, sizeof(count_js),
                 "db.getCollection('%s').estimatedDocumentCount()", cname);
        char *count_out = NULL;
        if (run_mongosh(opts->connection_string, count_js, &count_out) == 0) {
            tbl->row_count_estimate = atoll(trim(count_out));
            cbm_free(count_out);
        } else {
            tbl->row_count_estimate = -1;
        }

        /* Infer schema from first 50 documents by collecting field names + types. */
        char schema_js[CBM_SZ_1K];
        snprintf(schema_js, sizeof(schema_js),
                 "JSON.stringify(db.getCollection('%s').find().limit(50).toArray()"
                 ".reduce(function(acc,doc){Object.keys(doc).forEach(function(k){"
                 "var t=typeof doc[k];if(doc[k]===null)t='null';"
                 "else if(Array.isArray(doc[k]))t='array';"
                 "else if(doc[k] instanceof Date)t='date';"
                 "else if(doc[k] && doc[k]._bsontype)t=doc[k]._bsontype;"
                 "if(!acc[k])acc[k]=t;});return acc;},{}))",
                 cname);

        char *schema_out = NULL;
        if (run_mongosh(opts->connection_string, schema_js, &schema_out) == 0) {
            yyjson_doc *sdoc = yyjson_read(trim(schema_out), strlen(trim(schema_out)), 0);
            cbm_free(schema_out);
            if (sdoc) {
                yyjson_val *sroot = yyjson_doc_get_root(sdoc);
                int field_count = yyjson_obj_size(sroot);
                if (field_count > 0) {
                    tbl->columns = cbm_calloc((size_t)field_count, sizeof(cbm_db_column_t));
                    int ci = 0;
                    yyjson_obj_iter siter;
                    yyjson_obj_iter_init(sroot, &siter);
                    yyjson_val *key;
                    while ((key = yyjson_obj_iter_next(&siter)) != NULL) {
                        yyjson_val *val = yyjson_obj_iter_get_val(key);
                        tbl->columns[ci].name = sdup(yyjson_get_str(key));
                        tbl->columns[ci].data_type =
                            sdup(bson_type_to_str(yyjson_get_str(val)));
                        tbl->columns[ci].ordinal = ci + 1;
                        tbl->columns[ci].is_pk = strcmp(yyjson_get_str(key), "_id") == 0;
                        tbl->columns[ci].is_nullable = true;
                        tbl->columns[ci].is_unique = tbl->columns[ci].is_pk;
                        tbl->columns[ci].is_fk = false;
                        /* Detect ObjectId references as potential FKs. */
                        if (val && yyjson_get_str(val) &&
                            strcmp(yyjson_get_str(val), "ObjectId") == 0 &&
                            strcmp(yyjson_get_str(key), "_id") != 0) {
                            tbl->columns[ci].is_fk = true;
                        }
                        tbl->columns[ci].default_value = NULL;
                        tbl->columns[ci].comment = NULL;
                        ci++;
                    }
                    tbl->column_count = ci;
                }
                yyjson_doc_free(sdoc);
            }
        }

        /* Get indexes. */
        char idx_js[CBM_SZ_512];
        snprintf(idx_js, sizeof(idx_js),
                 "JSON.stringify(db.getCollection('%s').getIndexes())", cname);
        char *idx_out = NULL;
        if (run_mongosh(opts->connection_string, idx_js, &idx_out) == 0) {
            yyjson_doc *idoc = yyjson_read(trim(idx_out), strlen(trim(idx_out)), 0);
            cbm_free(idx_out);
            if (idoc) {
                yyjson_val *iarr = yyjson_doc_get_root(idoc);
                int idx_count = yyjson_arr_size(iarr);
                if (idx_count > 0) {
                    tbl->indexes = cbm_calloc((size_t)idx_count, sizeof(cbm_db_index_t));
                    int ii = 0;
                    yyjson_val *ival;
                    yyjson_arr_iter iiter;
                    yyjson_arr_iter_init(iarr, &iiter);
                    while ((ival = yyjson_arr_iter_next(&iiter)) != NULL && ii < idx_count) {
                        yyjson_val *name_val = yyjson_obj_get(ival, "name");
                        yyjson_val *unique_val = yyjson_obj_get(ival, "unique");
                        yyjson_val *key_val = yyjson_obj_get(ival, "key");

                        tbl->indexes[ii].name =
                            sdup(name_val ? yyjson_get_str(name_val) : "unknown");
                        tbl->indexes[ii].is_unique =
                            unique_val ? yyjson_get_bool(unique_val) : false;
                        tbl->indexes[ii].is_primary =
                            name_val && strcmp(yyjson_get_str(name_val), "_id_") == 0;
                        tbl->indexes[ii].index_type = sdup("BTREE");

                        /* Extract indexed field names. */
                        if (key_val) {
                            int kcnt = yyjson_obj_size(key_val);
                            if (kcnt > 0) {
                                tbl->indexes[ii].columns =
                                    cbm_calloc((size_t)kcnt, sizeof(char *));
                                int ki = 0;
                                yyjson_obj_iter kiter;
                                yyjson_obj_iter_init(key_val, &kiter);
                                yyjson_val *kkey;
                                while ((kkey = yyjson_obj_iter_next(&kiter)) != NULL) {
                                    tbl->indexes[ii].columns[ki++] =
                                        sdup(yyjson_get_str(kkey));
                                }
                                tbl->indexes[ii].column_count = ki;
                            }
                        }
                        ii++;
                    }
                    tbl->index_count = ii;
                }
                yyjson_doc_free(idoc);
            }
        }

        ti++;
    }
    out->schemas[0].table_count = ti;
    yyjson_doc_free(coll_doc);

    return 0;
}
