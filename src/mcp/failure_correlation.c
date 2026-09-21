/*
 * failure_correlation.c
 */
#include "mcp/failure_correlation.h"
#include "yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *text;
    const char *source;
} failure_entry_t;

typedef struct {
    failure_entry_t *entries;
    int count;
    int cap;
} failure_list_t;

static void add_failure(failure_list_t *list, const char *text, const char *source) {
    if (list->count >= list->cap) {
        list->cap = list->cap == 0 ? 4 : list->cap * 2;
        list->entries = realloc(list->entries, list->cap * sizeof(failure_entry_t));
    }
    list->entries[list->count].text = text;
    list->entries[list->count].source = source;
    list->count++;
}

/* Recursive JSON scanner to find tool errors or stack traces in user messages */
static void search_json_for_failures(yyjson_val *val, failure_list_t *list) {
    if (!val) return;
    
    if (yyjson_is_obj(val)) {
        /* Check if it's a tool result with an error */
        yyjson_val *is_err = yyjson_obj_get(val, "isError");
        if (is_err && yyjson_get_bool(is_err)) {
            yyjson_val *content = yyjson_obj_get(val, "content");
            if (content && yyjson_is_arr(content)) {
                yyjson_val *first = yyjson_arr_get_first(content);
                if (first && yyjson_is_obj(first)) {
                    yyjson_val *text = yyjson_obj_get(first, "text");
                    if (text && yyjson_is_str(text)) {
                        add_failure(list, yyjson_get_str(text), "tool_error");
                    }
                }
            }
        }
        
        /* Check raw text for common failure signatures (stack traces, exceptions) */
        yyjson_val *text_val = yyjson_obj_get(val, "text");
        if (text_val && yyjson_is_str(text_val)) {
            const char *str = yyjson_get_str(text_val);
            if (strstr(str, "Exception:") || strstr(str, "Error:") || 
                strstr(str, "Traceback (most recent call last):") || 
                strstr(str, "panic:") || strstr(str, "fatal error:")) {
                add_failure(list, str, "user_stacktrace");
            }
        }

        /* Recurse */
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(val, &iter);
        yyjson_val *k, *v;
        while ((k = yyjson_obj_iter_next(&iter))) {
            v = yyjson_obj_iter_get_val(k);
            search_json_for_failures(v, list);
        }
    } else if (yyjson_is_arr(val)) {
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(val, idx, max, v) {
            search_json_for_failures(v, list);
        }
    }
}

/* Check if the failure string contains the target string */
static bool string_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    return strstr(haystack, needle) != NULL;
}

int cbm_correlate_failure_event(cbm_history_store_t *history_store,
                                const char *project,
                                const char *session_id,
                                const char *thread_id,
                                int64_t timestamp_ms,
                                const char *transcript_json) {
    if (!history_store || !transcript_json) return -1;
    
    yyjson_doc *doc = yyjson_read(transcript_json, strlen(transcript_json), 0);
    if (!doc) return 0;
    
    failure_list_t failures = {0};
    search_json_for_failures(yyjson_doc_get_root(doc), &failures);
    yyjson_doc_free(doc);
    
    if (failures.count == 0) {
        if (failures.entries) free(failures.entries);
        return 0;
    }
    
    /* Fetch prior CHANGE_CORRELATION events for this thread */
    char **prev_changes = NULL;
    int change_count = 0;
    if (cbm_history_query_events(history_store, project, thread_id, "CHANGE_CORRELATION", 
                                 &prev_changes, &change_count) != CBM_HISTORY_OK) {
        free(failures.entries);
        return -1;
    }
    
    if (change_count == 0) {
        /* No prior changes to correlate to, but we still log the failure */
    }
    
    yyjson_mut_doc *out_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *out_arr = yyjson_mut_arr(out_doc);
    
    for (int i = 0; i < failures.count; i++) {
        yyjson_mut_val *fail_obj = yyjson_mut_obj(out_doc);
        
        /* Truncate failure text to avoid massive JSON blobs (e.g. huge stacktraces) */
        char snippet[1024] = {0};
        strncpy(snippet, failures.entries[i].text, 1000);
        if (strlen(failures.entries[i].text) > 1000) strcat(snippet, "...");
        
        yyjson_mut_obj_add_str(out_doc, fail_obj, "failure_snippet", snippet);
        yyjson_mut_obj_add_str(out_doc, fail_obj, "source", failures.entries[i].source);
        
        yyjson_mut_val *correlations = yyjson_mut_arr(out_doc);
        int max_confidence = 0; // 0 = None, 1 = Low, 2 = Medium, 3 = High
        
        for (int j = 0; j < change_count; j++) {
            yyjson_doc *cdoc = yyjson_read(prev_changes[j], strlen(prev_changes[j]), 0);
            if (!cdoc) continue;
            
            yyjson_val *root = yyjson_doc_get_root(cdoc);
            size_t idx, max;
            yyjson_val *cblock;
            yyjson_arr_foreach(root, idx, max, cblock) {
                if (!yyjson_is_obj(cblock)) continue;
                
                const char *cfile = yyjson_get_str(yyjson_obj_get(cblock, "file"));
                bool matched_file = string_contains(failures.entries[i].text, cfile);
                
                bool matched_node = false;
                yyjson_val *mnodes = yyjson_obj_get(cblock, "modified_nodes");
                if (mnodes && yyjson_is_arr(mnodes)) {
                    size_t nidx, nmax;
                    yyjson_val *node;
                    yyjson_arr_foreach(mnodes, nidx, nmax, node) {
                        const char *nname = yyjson_get_str(yyjson_obj_get(node, "name"));
                        if (string_contains(failures.entries[i].text, nname)) {
                            matched_node = true;
                            break;
                        }
                    }
                }
                
                bool matched_db = false;
                yyjson_val *db_ents = yyjson_obj_get(cblock, "affected_database_entities");
                if (db_ents && yyjson_is_arr(db_ents)) {
                    size_t didx, dmax;
                    yyjson_val *db_ent;
                    yyjson_arr_foreach(db_ents, didx, dmax, db_ent) {
                        const char *dname = yyjson_get_str(yyjson_obj_get(db_ent, "name"));
                        if (string_contains(failures.entries[i].text, dname)) {
                            matched_db = true;
                            break;
                        }
                    }
                }
                
                if (matched_file || matched_node || matched_db) {
                    yyjson_mut_val *corr = yyjson_mut_obj(out_doc);
                    yyjson_mut_obj_add_str(out_doc, corr, "file", cfile);
                    yyjson_mut_obj_add_str(out_doc, corr, "confidence", "High");
                    
                    yyjson_mut_val *ev = yyjson_mut_arr(out_doc);
                    if (matched_file) yyjson_mut_arr_add_str(out_doc, ev, "file_mentioned_in_stacktrace");
                    if (matched_node) yyjson_mut_arr_add_str(out_doc, ev, "symbol_mentioned_in_stacktrace");
                    if (matched_db)   yyjson_mut_arr_add_str(out_doc, ev, "database_entity_mentioned_in_error");
                    yyjson_mut_obj_add_val(out_doc, corr, "evidence", ev);
                    
                    yyjson_mut_arr_add_val(correlations, corr);
                    max_confidence = 3;
                } else if (max_confidence == 0) {
                    /* Low confidence default: it happened after this change */
                    yyjson_mut_val *corr = yyjson_mut_obj(out_doc);
                    yyjson_mut_obj_add_str(out_doc, corr, "file", cfile);
                    yyjson_mut_obj_add_str(out_doc, corr, "confidence", "Low");
                    yyjson_mut_val *ev = yyjson_mut_arr(out_doc);
                    yyjson_mut_arr_add_str(out_doc, ev, "temporal_proximity");
                    yyjson_mut_obj_add_val(out_doc, corr, "evidence", ev);
                    yyjson_mut_arr_add_val(correlations, corr);
                }
            }
            yyjson_doc_free(cdoc);
        }
        
        yyjson_mut_obj_add_val(out_doc, fail_obj, "correlated_changes", correlations);
        yyjson_mut_arr_add_val(out_arr, fail_obj);
    }
    
    if (yyjson_mut_arr_size(out_arr) > 0) {
        const char *failure_json = yyjson_mut_write(out_doc, 0, NULL);
        if (failure_json) {
            cbm_history_append_event(history_store, project, session_id, thread_id,
                                     timestamp_ms, "FAILURE_CORRELATION", failure_json);
            free((void*)failure_json);
        }
    }
    
    yyjson_mut_doc_free(out_doc);
    
    for (int i = 0; i < change_count; i++) {
        free(prev_changes[i]);
    }
    if (prev_changes) free(prev_changes);
    free(failures.entries);
    
    return 0;
}
