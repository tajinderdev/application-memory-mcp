/*
 * correction_memory.c
 */
#include "mcp/correction_memory.h"
#include "yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Extract the first modified file path from a CHANGE_CORRELATION payload.
 * Expected shape: [{..., "file": "path/to/file", ...}, ...]
 */
static char *extract_first_file(const char *json_payload) {
    if (!json_payload) return NULL;
    yyjson_doc *doc = yyjson_read(json_payload, strlen(json_payload), 0);
    if (!doc) return NULL;

    char *res = NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    /* CHANGE_CORRELATION payloads are JSON arrays of change objects */
    if (yyjson_is_arr(root)) {
        yyjson_val *first = yyjson_arr_get_first(root);
        if (first && yyjson_is_obj(first)) {
            yyjson_val *fval = yyjson_obj_get(first, "file");
            if (fval && yyjson_is_str(fval)) {
                res = strdup(yyjson_get_str(fval));
            }
        }
    } else if (yyjson_is_obj(root)) {
        /* Handle single-object shape defensively */
        yyjson_val *fval = yyjson_obj_get(root, "file");
        if (fval && yyjson_is_str(fval)) {
            res = strdup(yyjson_get_str(fval));
        }
    }
    yyjson_doc_free(doc);
    return res;
}

/* Extract a failure signature string from a FAILURE_CORRELATION payload.
 * Expected shape: [{..., "failure_snippet": "...", ...}, ...]
 * Also handles CORRECTION_MEMORY payload which embeds the failure sub-object:
 * {"attempted_change": ..., "failure": [{..., "failure_snippet": "..."}, ...], ...}
 */
static char *extract_failure_snippet_from(const char *json_payload) {
    if (!json_payload) return NULL;
    yyjson_doc *doc = yyjson_read(json_payload, strlen(json_payload), 0);
    if (!doc) return NULL;

    char *res = NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);

    /* FAILURE_CORRELATION: top-level array */
    if (yyjson_is_arr(root)) {
        yyjson_val *first = yyjson_arr_get_first(root);
        if (first && yyjson_is_obj(first)) {
            yyjson_val *fval = yyjson_obj_get(first, "failure_snippet");
            if (fval && yyjson_is_str(fval)) {
                res = strdup(yyjson_get_str(fval));
            }
        }
    }
    /* CORRECTION_MEMORY: object with embedded "failure" field */
    else if (yyjson_is_obj(root)) {
        yyjson_val *fail_val = yyjson_obj_get(root, "failure");
        if (fail_val && yyjson_is_arr(fail_val)) {
            yyjson_val *first = yyjson_arr_get_first(fail_val);
            if (first && yyjson_is_obj(first)) {
                yyjson_val *fval = yyjson_obj_get(first, "failure_snippet");
                if (fval && yyjson_is_str(fval)) {
                    res = strdup(yyjson_get_str(fval));
                }
            }
        }
    }

    yyjson_doc_free(doc);
    return res;
}

/* Fuzzy match: returns true if the two failure snippets appear to describe the same error. */
static bool failure_snippets_match(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    if (alen == 0 || blen == 0) return false;

    /* Exact containment */
    if (strstr(a, b) || strstr(b, a)) return true;

    /* Prefix match (first 30 chars) — catches "same error, different stack depth" */
    size_t pfx = 30;
    if (alen >= pfx && blen >= pfx && strncmp(a, b, pfx) == 0) return true;

    return false;
}

static void promote_engineering_pattern(cbm_history_store_t *hs,
                                        const char *project,
                                        const char *session_id,
                                        const char *thread_id,
                                        int64_t timestamp_ms,
                                        const char *failure_snippet,
                                        const char *correction_json) {
    if (!failure_snippet || failure_snippet[0] == '\0') return;

    char **payloads = NULL;
    int count = 0;
    /* Query all CORRECTION_MEMORY events globally (not per-thread) for idempotency. */
    if (cbm_history_query_events(hs, project, NULL, "CORRECTION_MEMORY", &payloads, &count) != CBM_HISTORY_OK) {
        return;
    }

    int match_count = 0;
    for (int i = 0; i < count; i++) {
        /* Extract snippet from the embedded failure field of the CORRECTION_MEMORY record. */
        char *past_snippet = extract_failure_snippet_from(payloads[i]);
        if (past_snippet) {
            if (failure_snippets_match(failure_snippet, past_snippet)) {
                match_count++;
            }
            free(past_snippet);
        }
        free(payloads[i]);
    }
    if (payloads) free(payloads);

    /* Only promote once the pattern has been corrected 3+ times. */
    if (match_count >= 3) {
        yyjson_mut_doc *pdoc = yyjson_mut_doc_new(NULL);
        if (!pdoc) return;
        yyjson_mut_val *pobj = yyjson_mut_obj(pdoc);
        /* BUG-FIX: set the document root */
        yyjson_mut_doc_set_root(pdoc, pobj);

        yyjson_mut_obj_add_str(pdoc, pobj, "pattern_type", "recurring_failure_correction");
        /* Store integer count for numeric queries */
        yyjson_mut_obj_add_int(pdoc, pobj, "evidence_count", match_count);
        yyjson_mut_obj_add_str(pdoc, pobj, "failure_signature", failure_snippet);

        /* Embed the latest correction JSON as a raw sub-object */
        yyjson_doc *corr_doc = yyjson_read(correction_json, strlen(correction_json), 0);
        if (corr_doc) {
            yyjson_mut_obj_add_val(pdoc, pobj, "latest_correction",
                                   yyjson_val_mut_copy(pdoc, yyjson_doc_get_root(corr_doc)));
            yyjson_doc_free(corr_doc);
        } else {
            yyjson_mut_obj_add_str(pdoc, pobj, "latest_correction", correction_json);
        }

        size_t jslen = 0;
        const char *pattern_json = yyjson_mut_write(pdoc, 0, &jslen);
        if (pattern_json && jslen > 0) {
            cbm_history_append_event(hs, project, session_id, thread_id, timestamp_ms,
                                     "ENGINEERING_PATTERN", pattern_json);
            free((void*)pattern_json);
        }
        yyjson_mut_doc_free(pdoc);
    }
}

int cbm_synthesize_corrections(cbm_history_store_t *history_store,
                               const char *project,
                               const char *session_id,
                               const char *thread_id,
                               int64_t timestamp_ms) {
    if (!history_store || !project || !thread_id) return -1;

    char **types = NULL;
    char **payloads = NULL;
    int count = 0;

    if (cbm_history_query_thread_timeline(history_store, project, thread_id,
                                          &types, &payloads, &count) != CBM_HISTORY_OK) {
        return -1;
    }

    /* Need at least 3 events for a Change -> Failure -> Change cycle. */
    if (count >= 3) {
        for (int i = 1; i < count - 1; i++) {
            if (strcmp(types[i], "FAILURE_CORRELATION") != 0) continue;

            /* Find the last CHANGE before index i */
            int c1_idx = -1;
            for (int j = i - 1; j >= 0; j--) {
                if (strcmp(types[j], "CHANGE_CORRELATION") == 0) {
                    c1_idx = j;
                    break;
                }
            }

            /* Find the first CHANGE after index i */
            int c2_idx = -1;
            for (int j = i + 1; j < count; j++) {
                if (strcmp(types[j], "CHANGE_CORRELATION") == 0) {
                    c2_idx = j;
                    break;
                }
            }

            if (c1_idx < 0 || c2_idx < 0) continue;

            char *f1 = extract_first_file(payloads[c1_idx]);
            char *f2 = extract_first_file(payloads[c2_idx]);

            /* Correction confirmed when both changes target the same file. */
            if (f1 && f2 && strcmp(f1, f2) == 0) {
                yyjson_mut_doc *cdoc = yyjson_mut_doc_new(NULL);
                if (!cdoc) { free(f1); free(f2); continue; }

                yyjson_mut_val *cobj = yyjson_mut_obj(cdoc);
                /* BUG-FIX: set the document root so yyjson_mut_write produces valid JSON */
                yyjson_mut_doc_set_root(cdoc, cobj);

                yyjson_doc *d_c1 = yyjson_read(payloads[c1_idx], strlen(payloads[c1_idx]), 0);
                yyjson_doc *d_f  = yyjson_read(payloads[i],       strlen(payloads[i]),       0);
                yyjson_doc *d_c2 = yyjson_read(payloads[c2_idx], strlen(payloads[c2_idx]), 0);

                if (d_c1) yyjson_mut_obj_add_val(cdoc, cobj, "attempted_change",
                                                 yyjson_val_mut_copy(cdoc, yyjson_doc_get_root(d_c1)));
                if (d_f)  yyjson_mut_obj_add_val(cdoc, cobj, "failure",
                                                 yyjson_val_mut_copy(cdoc, yyjson_doc_get_root(d_f)));
                if (d_c2) yyjson_mut_obj_add_val(cdoc, cobj, "successful_correction",
                                                 yyjson_val_mut_copy(cdoc, yyjson_doc_get_root(d_c2)));

                size_t jslen = 0;
                const char *corr_json = yyjson_mut_write(cdoc, 0, &jslen);
                if (corr_json && jslen > 0) {
                    cbm_history_append_event(history_store, project, session_id, thread_id,
                                             timestamp_ms, "CORRECTION_MEMORY", corr_json);

                    /* Attempt pattern promotion using snippet from the failure payload. */
                    char *fsnip = extract_failure_snippet_from(payloads[i]);
                    if (fsnip) {
                        promote_engineering_pattern(history_store, project, session_id, thread_id,
                                                    timestamp_ms, fsnip, corr_json);
                        free(fsnip);
                    }
                    free((void*)corr_json);
                }

                if (d_c1) yyjson_doc_free(d_c1);
                if (d_f)  yyjson_doc_free(d_f);
                if (d_c2) yyjson_doc_free(d_c2);
                yyjson_mut_doc_free(cdoc);
            }

            if (f1) free(f1);
            if (f2) free(f2);
        }
    }

    for (int i = 0; i < count; i++) {
        free(types[i]);
        free(payloads[i]);
    }
    if (types) free(types);
    if (payloads) free(payloads);

    return 0;
}
