/*
 * change_correlation.c
 */
#include "mcp/change_correlation.h"
#include "yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Helper: string ends with */
static bool ends_with_ignore_case(const char *str, const char *suffix) {
    if (!str || !suffix) return false;
    size_t len_str = strlen(str);
    size_t len_suf = strlen(suffix);
    if (len_suf > len_str) return false;
    const char *p1 = str + (len_str - len_suf);
    /* Simple case-insensitive comparison (assuming ASCII paths) */
    while (*p1 && *suffix) {
        char c1 = *p1 >= 'A' && *p1 <= 'Z' ? *p1 + 32 : *p1;
        char c2 = *suffix >= 'A' && *suffix <= 'Z' ? *suffix + 32 : *suffix;
        /* Also treat \ and / as equivalent for Windows/POSIX path matching */
        if (c1 == '\\') c1 = '/';
        if (c2 == '\\') c2 = '/';
        if (c1 != c2) return false;
        p1++; suffix++;
    }
    return true;
}

static const char *resolve_rel_path(const char *abs_path, char **files, int file_count) {
    if (!abs_path || !files) return NULL;
    for (int i = 0; i < file_count; i++) {
        if (ends_with_ignore_case(abs_path, files[i])) {
            return files[i];
        }
    }
    return NULL;
}

/* 
 * Walk the JSON tree to find edit tool calls.
 * A very resilient search looking for objects that contain:
 * "TargetFile", "StartLine", and "EndLine".
 * Also supports "target_file", "start_line", "end_line".
 */
typedef struct {
    const char *file_path;
    int start_line;
    int end_line;
} diff_entry_t;

typedef struct {
    diff_entry_t *entries;
    int count;
    int cap;
} diff_list_t;

static void add_diff(diff_list_t *list, const char *file, int start, int end) {
    if (list->count >= list->cap) {
        list->cap = list->cap == 0 ? 4 : list->cap * 2;
        list->entries = realloc(list->entries, list->cap * sizeof(diff_entry_t));
    }
    list->entries[list->count].file_path = file;
    list->entries[list->count].start_line = start;
    list->entries[list->count].end_line = end;
    list->count++;
}

static void search_json_for_diffs(yyjson_val *val, diff_list_t *list) {
    if (!val) return;
    if (yyjson_is_obj(val)) {
        /* Check if this object represents a file edit */
        yyjson_val *tf = yyjson_obj_get(val, "TargetFile");
        if (!tf) tf = yyjson_obj_get(val, "target_file");
        
        yyjson_val *sl = yyjson_obj_get(val, "StartLine");
        if (!sl) sl = yyjson_obj_get(val, "start_line");
        
        yyjson_val *el = yyjson_obj_get(val, "EndLine");
        if (!el) el = yyjson_obj_get(val, "end_line");
        
        if (tf && yyjson_is_str(tf)) {
            const char *file = yyjson_get_str(tf);
            int start = sl && yyjson_is_int(sl) ? yyjson_get_int(sl) : 1;
            int end = el && yyjson_is_int(el) ? yyjson_get_int(el) : 999999;
            add_diff(list, file, start, end);
        }
        
        /* Recurse into object values */
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(val, &iter);
        yyjson_val *k, *v;
        while ((k = yyjson_obj_iter_next(&iter))) {
            v = yyjson_obj_iter_get_val(k);
            search_json_for_diffs(v, list);
        }
    } else if (yyjson_is_arr(val)) {
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(val, idx, max, v) {
            search_json_for_diffs(v, list);
        }
    }
}

/* 
 * Database Correlation
 * Finds database entities connected to an AST node.
 */
static void add_db_entities_for_node(cbm_store_t *store, const char *project, 
                                     cbm_node_t *ast_node, yyjson_mut_doc *doc, 
                                     yyjson_mut_val *db_arr) {
    if (!store || !ast_node || !db_arr) return;
    
    int db_found = 0;
    
    /* 1. Explicit Graph Edges */
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    if (cbm_store_find_edges_by_source(store, ast_node->id, &edges, &edge_count) == CBM_STORE_OK) {
        for (int i = 0; i < edge_count; i++) {
            if (!edges[i].type) continue;
            if (strcmp(edges[i].type, "QUERIES") == 0 || 
                strcmp(edges[i].type, "USES_TABLE") == 0 || 
                strcmp(edges[i].type, "REFERENCES_TABLE") == 0) {
                
                cbm_node_t tgt;
                if (cbm_store_find_node_by_id(store, edges[i].target_id, &tgt) == CBM_STORE_OK) {
                    if (tgt.label && (strcmp(tgt.label, "Table") == 0 || 
                                      strcmp(tgt.label, "Column") == 0 || 
                                      strcmp(tgt.label, "View") == 0)) {
                        yyjson_mut_val *db_obj = yyjson_mut_obj(doc);
                        yyjson_mut_obj_add_str(doc, db_obj, "name", tgt.name);
                        yyjson_mut_obj_add_str(doc, db_obj, "label", tgt.label);
                        yyjson_mut_obj_add_str(doc, db_obj, "correlation_type", "explicit_edge");
                        yyjson_mut_arr_add_val(db_arr, db_obj);
                        db_found++;
                    }
                }
            }
        }
        cbm_store_free_edges(edges, edge_count);
    }
    
    /* 2. Lexical Fallback (if no explicit edges found) */
    if (db_found == 0 && ast_node->name) {
        const char *name = ast_node->name;
        /* Check if the node acts as a DB abstraction */
        if (strstr(name, "Model") || strstr(name, "model") || 
            strstr(name, "Repository") || strstr(name, "repository") || 
            strstr(name, "Migration") || strstr(name, "migration")) {
            
            /* Derive putative table name (e.g., UserModel -> user) */
            char putative[256];
            int p = 0;
            for (int i = 0; name[i] && p < 250; i++) {
                if (name[i] >= 'A' && name[i] <= 'Z') {
                    if (i > 0 && name[i-1] >= 'a' && name[i-1] <= 'z') {
                        putative[p++] = '_';
                    }
                    putative[p++] = name[i] + 32; /* to lower */
                } else {
                    putative[p++] = name[i];
                }
            }
            putative[p] = '\0';
            
            /* Strip common suffixes */
            char *suf;
            if ((suf = strstr(putative, "_model"))) *suf = '\0';
            if ((suf = strstr(putative, "_repository"))) *suf = '\0';
            if ((suf = strstr(putative, "_migration"))) *suf = '\0';
            
            /* Search for Table nodes matching this name */
            cbm_node_t *tables = NULL;
            int table_count = 0;
            if (cbm_store_find_nodes_by_label(store, project, "Table", &tables, &table_count) == CBM_STORE_OK) {
                for (int i = 0; i < table_count; i++) {
                    if (!tables[i].name) continue;
                    /* Match exactly, or plural 's', or if table contains putative */
                    if (strcmp(tables[i].name, putative) == 0 || strstr(tables[i].name, putative)) {
                        yyjson_mut_val *db_obj = yyjson_mut_obj(doc);
                        yyjson_mut_obj_add_str(doc, db_obj, "name", tables[i].name);
                        yyjson_mut_obj_add_str(doc, db_obj, "label", tables[i].label);
                        yyjson_mut_obj_add_str(doc, db_obj, "correlation_type", "lexical_fallback");
                        yyjson_mut_arr_add_val(db_arr, db_obj);
                    }
                }
                cbm_store_free_nodes(tables, table_count);
            }
        }
    }
}

int cbm_correlate_transcript_event(cbm_store_t *codebase_store,
                                   cbm_history_store_t *history_store,
                                   const char *project,
                                   const char *session_id,
                                   const char *thread_id,
                                   int64_t timestamp_ms,
                                   const char *transcript_json) {
    if (!codebase_store || !history_store || !transcript_json) return -1;
    
    yyjson_doc *doc = yyjson_read(transcript_json, strlen(transcript_json), 0);
    if (!doc) return 0; /* invalid JSON, nothing to correlate */
    
    diff_list_t diffs = {0};
    search_json_for_diffs(yyjson_doc_get_root(doc), &diffs);
    yyjson_doc_free(doc);
    
    if (diffs.count == 0) {
        if (diffs.entries) free(diffs.entries);
        return 0; /* no diffs found */
    }
    
    /* Fetch all project files to resolve absolute paths to relative */
    char **project_files = NULL;
    int file_count = 0;
    cbm_store_list_files(codebase_store, project, &project_files, &file_count);
    
    /* We will build a JSON array of correlated change blocks */
    yyjson_mut_doc *out_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *out_arr = yyjson_mut_arr(out_doc);
    
    for (int i = 0; i < diffs.count; i++) {
        const char *rel_path = resolve_rel_path(diffs.entries[i].file_path, project_files, file_count);
        if (!rel_path) continue; /* file not in graph */
        
        cbm_node_t *nodes = NULL;
        int node_count = 0;
        int rc = cbm_store_find_nodes_by_file_overlap(codebase_store, project, rel_path,
                                                      diffs.entries[i].start_line,
                                                      diffs.entries[i].end_line,
                                                      &nodes, &node_count);
        if (rc == CBM_STORE_OK && node_count > 0) {
            yyjson_mut_val *diff_obj = yyjson_mut_obj(out_doc);
            yyjson_mut_obj_add_str(out_doc, diff_obj, "file", rel_path);
            yyjson_mut_val *mod_nodes = yyjson_mut_arr(out_doc);
            
            for (int j = 0; j < node_count; j++) {
                yyjson_mut_val *node_obj = yyjson_mut_obj(out_doc);
                yyjson_mut_obj_add_str(out_doc, node_obj, "name", nodes[j].name);
                yyjson_mut_obj_add_str(out_doc, node_obj, "label", nodes[j].label);
                
                char **callers = NULL, **callees = NULL;
                int caller_count = 0, callee_count = 0;
                cbm_store_node_neighbor_names(codebase_store, nodes[j].id, 10,
                                              &callers, &caller_count,
                                              &callees, &callee_count);
                
                if (caller_count > 0) {
                    yyjson_mut_val *caller_arr = yyjson_mut_arr(out_doc);
                    for (int k = 0; k < caller_count; k++) {
                        yyjson_mut_arr_add_str(out_doc, caller_arr, callers[k]);
                        free(callers[k]);
                    }
                    yyjson_mut_obj_add_val(out_doc, node_obj, "upstream_callers", caller_arr);
                    free(callers);
                }
                
                if (callee_count > 0) {
                    yyjson_mut_val *callee_arr = yyjson_mut_arr(out_doc);
                    for (int k = 0; k < callee_count; k++) {
                        yyjson_mut_arr_add_str(out_doc, callee_arr, callees[k]);
                        free(callees[k]);
                    }
                    yyjson_mut_obj_add_val(out_doc, node_obj, "downstream_dependencies", callee_arr);
                    free(callees);
                }
                
                /* Database correlation */
                yyjson_mut_val *db_arr = yyjson_mut_arr(out_doc);
                add_db_entities_for_node(codebase_store, project, &nodes[j], out_doc, db_arr);
                if (yyjson_mut_arr_size(db_arr) > 0) {
                    yyjson_mut_obj_add_val(out_doc, node_obj, "affected_database_entities", db_arr);
                }
                
                yyjson_mut_arr_add_val(mod_nodes, node_obj);
            }
            cbm_store_free_nodes(nodes, node_count);
            
            yyjson_mut_obj_add_val(out_doc, diff_obj, "modified_nodes", mod_nodes);
            
            /* Dependent files (who consumes this file's exports) */
            char **dep_files = NULL;
            int dep_count = 0;
            const char *targets[] = { rel_path };
            cbm_store_get_dependent_files(codebase_store, project, targets, 1, &dep_files, &dep_count);
            if (dep_count > 0) {
                yyjson_mut_val *dep_arr = yyjson_mut_arr(out_doc);
                for (int k = 0; k < dep_count; k++) {
                    yyjson_mut_arr_add_str(out_doc, dep_arr, dep_files[k]);
                }
                yyjson_mut_obj_add_val(out_doc, diff_obj, "dependent_files", dep_arr);
                cbm_store_free_dependent_files(dep_files, dep_count);
            }
            
            yyjson_mut_arr_add_val(out_arr, diff_obj);
        }
    }
    
    if (project_files) {
        for (int i = 0; i < file_count; i++) free(project_files[i]);
        free(project_files);
    }
    free(diffs.entries);
    
    if (yyjson_mut_arr_size(out_arr) > 0) {
        const char *correlation_json = yyjson_mut_write(out_doc, 0, NULL);
        if (correlation_json) {
            cbm_history_append_event(history_store, project, session_id, thread_id,
                                     timestamp_ms, "CHANGE_CORRELATION", correlation_json);
            free((void*)correlation_json);
        }
    }
    
    yyjson_mut_doc_free(out_doc);
    return 0;
}
