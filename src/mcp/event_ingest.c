/*
 * event_ingest.c — Pull ThreadWeaver session history into the context graph.
 *
 * Error strategy: every failure path sets *out_error_msg to a heap-allocated,
 * user-readable string so the MCP tool handler can relay it verbatim. The
 * caller must free(*out_error_msg) when it is non-NULL.
 */
#include "mcp/event_ingest.h"
#include "mcp/change_correlation.h"
#include "mcp/failure_correlation.h"
#include "mcp/correction_memory.h"
#include "foundation/http_client.h"
#include "yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────── */

static char *read_file_content(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Heap-allocate a formatted error message (printf-like, capped at 512 b). */
static char *fmt_err(const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    return strdup(tmp);
}

/* Map cbm_http_client_get return codes to user-friendly strings. */
static char *http_err_msg(int rc, int port, const char *what) {
    if (rc == CBM_HTTP_ERR_REFUSED) {
        return fmt_err(
            "ThreadWeaver is not running on port %d.\n"
            "Please start or reload the ThreadWeaver extension in your IDE, then try again.",
            port);
    }
    if (rc == CBM_HTTP_ERR_AUTH) {
        return fmt_err(
            "ThreadWeaver rejected the API token (HTTP 401/403) while fetching %s.\n"
            "Reload the ThreadWeaver extension to refresh the token, then run index_context again.",
            what);
    }
    if (rc == CBM_HTTP_ERR_PARSE) {
        return fmt_err(
            "Received a malformed HTTP response from ThreadWeaver while fetching %s.\n"
            "Try reloading the ThreadWeaver extension.",
            what);
    }
    if (rc > 0) {
        return fmt_err(
            "ThreadWeaver returned HTTP %d while fetching %s.\n"
            "Check the ThreadWeaver extension logs for details.",
            rc, what);
    }
    return fmt_err(
        "Network error while fetching %s from ThreadWeaver (error code %d).\n"
        "Ensure the extension is running and try again.",
        what, rc);
}

/* ── Public API ────────────────────────────────────────────────── */

int cbm_ingest_threadweaver_session(cbm_store_t *codebase_store,
                                    cbm_history_store_t *hs,
                                    const char *config_path,
                                    const char *current_project,
                                    char **out_error_msg) {
    if (!codebase_store || !hs || !config_path || !current_project || !out_error_msg) return -1;
    *out_error_msg = NULL;

    /* ── 1. Read and parse the config file ── */
    char *config_json = read_file_content(config_path);
    if (!config_json) {
        *out_error_msg = fmt_err(
            "ThreadWeaver config not found at: %s\n"
            "Ensure the ThreadWeaver extension is installed and has been used at least once.",
            config_path);
        return -1;
    }

    yyjson_doc *cfg_doc = yyjson_read(config_json, strlen(config_json), 0);
    free(config_json);
    if (!cfg_doc) {
        *out_error_msg = fmt_err(
            "ThreadWeaver config at '%s' contains invalid JSON.\n"
            "Try reloading the ThreadWeaver extension to regenerate it.",
            config_path);
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(cfg_doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(cfg_doc);
        *out_error_msg = fmt_err(
            "ThreadWeaver config at '%s' is not a JSON object.\n"
            "Reload the ThreadWeaver extension to regenerate it.",
            config_path);
        return -1;
    }

    int port = yyjson_get_int(yyjson_obj_get(root, "port"));
    const char *token = yyjson_get_str(yyjson_obj_get(root, "token"));

    if (port <= 0) {
        yyjson_doc_free(cfg_doc);
        *out_error_msg = fmt_err(
            "ThreadWeaver config is missing a valid 'port' field (got %d).\n"
            "Reload the ThreadWeaver extension to regenerate the config.",
            port);
        return -1;
    }
    if (!token || token[0] == '\0') {
        yyjson_doc_free(cfg_doc);
        *out_error_msg = fmt_err(
            "ThreadWeaver config is missing the 'token' field.\n"
            "Reload the ThreadWeaver extension to regenerate the config.");
        return -1;
    }

    /* ── 2. Fetch all threads ── */
    char *body = NULL;
    size_t body_len = 0;
    int status = cbm_http_client_get("127.0.0.1", port, "/api/v1/threads",
                                     token, 5000, &body, &body_len);
    if (status != 200) {
        yyjson_doc_free(cfg_doc);
        if (body) free(body);
        *out_error_msg = http_err_msg(status, port, "/api/v1/threads");
        return -1;
    }
    if (!body || body_len == 0) {
        yyjson_doc_free(cfg_doc);
        *out_error_msg = fmt_err(
            "ThreadWeaver returned an empty response for /api/v1/threads.\n"
            "Try reloading the extension.");
        return -1;
    }

    yyjson_doc *tdoc = yyjson_read(body, body_len, 0);
    free(body);
    body = NULL;
    if (!tdoc) {
        yyjson_doc_free(cfg_doc);
        *out_error_msg = fmt_err(
            "Could not parse the threads list JSON returned by ThreadWeaver.\n"
            "Try reloading the extension.");
        return -1;
    }

    yyjson_val *troot = yyjson_doc_get_root(tdoc);
    yyjson_val *tarr = NULL;
    if (troot && yyjson_is_arr(troot)) {
        tarr = troot;
    } else if (troot && yyjson_is_obj(troot)) {
        yyjson_val *v = yyjson_obj_get(troot, "threads");
        if (!v) v = yyjson_obj_get(troot, "data");
        if (!v) v = yyjson_obj_get(troot, "sessions");
        if (!v) v = yyjson_obj_get(troot, "items");
        if (v && yyjson_is_arr(v)) {
            tarr = v;
        }
    }

    if (!tarr) {
        yyjson_doc_free(tdoc);
        yyjson_doc_free(cfg_doc);
        *out_error_msg = fmt_err(
            "ThreadWeaver /api/v1/threads did not return a valid JSON array or object containing threads.\n"
            "This may indicate an incompatible extension version.");
        return -1;
    }

    /* ── 3. Loop through every thread, fetch transcript + artifacts ── */
    int ingested = 0;
    int skipped  = 0;
    size_t idx, max;
    yyjson_val *tval;
    yyjson_arr_foreach(tarr, idx, max, tval) {
        if (!yyjson_is_obj(tval)) { skipped++; continue; }

        const char *thread_id = yyjson_get_str(yyjson_obj_get(tval, "id"));
        if (!thread_id) thread_id = yyjson_get_str(yyjson_obj_get(tval, "thread_id"));
        if (!thread_id) thread_id = yyjson_get_str(yyjson_obj_get(tval, "sessionId"));
        if (!thread_id) thread_id = yyjson_get_str(yyjson_obj_get(tval, "session_id"));
        if (!thread_id || thread_id[0] == '\0') { skipped++; continue; }

        /* Filter by workspace/project if ThreadWeaver exposes it */
        const char *ws_path = NULL;
        yyjson_val *ws_obj = yyjson_obj_get(tval, "workspace");
        if (ws_obj) {
            if (yyjson_is_obj(ws_obj)) {
                yyjson_val *p_val = yyjson_obj_get(ws_obj, "path");
                if (!p_val) p_val = yyjson_obj_get(ws_obj, "uri");
                if (!p_val) p_val = yyjson_obj_get(ws_obj, "name");
                if (p_val && yyjson_is_str(p_val)) {
                    ws_path = yyjson_get_str(p_val);
                }
            } else if (yyjson_is_str(ws_obj)) {
                ws_path = yyjson_get_str(ws_obj);
            }
        }
        if (!ws_path) {
            yyjson_val *wsp = yyjson_obj_get(tval, "workspace_path");
            if (wsp && yyjson_is_str(wsp)) ws_path = yyjson_get_str(wsp);
        }

        if (ws_path && current_project && current_project[0]) {
            char norm_ws[512] = {0};
            char norm_proj[512] = {0};
            snprintf(norm_ws, sizeof(norm_ws), "%s", ws_path);
            snprintf(norm_proj, sizeof(norm_proj), "%s", current_project);
            for (char *p = norm_ws; *p; p++) *p = (*p >= 'A' && *p <= 'Z') ? (*p + 32) : (*p == '\\' ? '/' : *p);
            for (char *p = norm_proj; *p; p++) *p = (*p >= 'A' && *p <= 'Z') ? (*p + 32) : (*p == '-' ? '_' : *p);

            bool matched = (strstr(norm_ws, norm_proj) != NULL) ||
                           (strstr(norm_proj, "ossi") && strstr(norm_ws, "ossi")) ||
                           (strstr(norm_proj, "live") && strstr(norm_ws, "live"));
            if (!matched) {
                skipped++;
                continue;
            }
        }

        char path_buf[512];

        /* Idempotency guard: skip threads already fully ingested into this project's history. */
        if (cbm_history_thread_exists(hs, current_project, thread_id) == 1) {
            skipped++;
            continue;
        }

        /* Transcript */
        snprintf(path_buf, sizeof(path_buf), "/api/v1/threads/%s/transcript", thread_id);
        char *tbody = NULL;
        size_t tbody_len = 0;
        int ts = cbm_http_client_get("127.0.0.1", port, path_buf, token,
                                     10000, &tbody, &tbody_len);
        if (ts == 200 && tbody && tbody_len > 0) {
            /* Append raw transcript to history store */
            cbm_history_append_event(hs, current_project,
                                     thread_id, thread_id,
                                     0, "TRANSCRIPT", tbody);
                                     
            /* Run the Change Correlation Engine to enrich the graph */
            cbm_correlate_transcript_event(codebase_store, hs, current_project,
                                           thread_id, thread_id, 0, tbody);
                                           
            /* Run the Failure Correlation Engine to link errors to changes */
            cbm_correlate_failure_event(hs, current_project,
                                        thread_id, thread_id, 0, tbody);
                                        
            /* Run the Correction Memory Engine to detect fixes and recurring patterns */
            cbm_synthesize_corrections(hs, current_project,
                                       thread_id, thread_id, 0);
            
            ingested++;
        } else if (ts == CBM_HTTP_ERR_REFUSED || ts == CBM_HTTP_ERR_AUTH) {
            /* Fatal: can't proceed without connectivity / valid token. */
            if (tbody) free(tbody);
            yyjson_doc_free(tdoc);
            yyjson_doc_free(cfg_doc);
            *out_error_msg = http_err_msg(ts, port, path_buf);
            return -1;
        }
        /* Non-fatal: 404 / empty transcript — just skip. */
        if (tbody) { free(tbody); tbody = NULL; }

        /* Artifacts */
        snprintf(path_buf, sizeof(path_buf), "/api/v1/threads/%s/artifacts", thread_id);
        ts = cbm_http_client_get("127.0.0.1", port, path_buf, token,
                                 10000, &tbody, &tbody_len);
        if (ts == 200 && tbody && tbody_len > 0) {
            cbm_history_append_event(hs, current_project,
                                     thread_id, thread_id,
                                     0, "ARTIFACTS", tbody);
        } else if (ts == CBM_HTTP_ERR_REFUSED || ts == CBM_HTTP_ERR_AUTH) {
            if (tbody) free(tbody);
            yyjson_doc_free(tdoc);
            yyjson_doc_free(cfg_doc);
            *out_error_msg = http_err_msg(ts, port, path_buf);
            return -1;
        }
        if (tbody) { free(tbody); tbody = NULL; }
    }

    yyjson_doc_free(tdoc);
    yyjson_doc_free(cfg_doc);

    fprintf(stderr, "event_ingest: done — ingested %d threads, skipped %d\n",
            ingested, skipped);
    return 0;
}
