/*
 * orchestrator.c
 */
#include "mcp/orchestrator.h"
#include "yyjson.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} md_buf_t;

static void md_buf_free(md_buf_t *md) {
    free(md->buf);
    md->buf = NULL;
    md->len = 0;
    md->cap = 0;
}

static int md_append(md_buf_t *md, const char *fmt, ...) {
    if (!md->buf) {
        md->cap = 2048;
        md->buf = malloc(md->cap);
        if (!md->buf) return -1;
        md->buf[0] = '\0';
        md->len = 0;
    }

    /* Use a heap buffer to avoid stack overflow on large payloads. */
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (n < 0) {
        va_end(args2);
        return -1;
    }

    /* Ensure capacity */
    while (md->len + (size_t)n + 1 > md->cap) {
        md->cap = md->cap * 2 + (size_t)n + 1;
        char *tmp = realloc(md->buf, md->cap);
        if (!tmp) {
            va_end(args2);
            return -1;
        }
        md->buf = tmp;
    }

    vsnprintf(md->buf + md->len, md->cap - md->len, fmt, args2);
    va_end(args2);
    md->len += (size_t)n;
    return 0;
}

/* Case-insensitive substring search without copying. */
static bool string_contains_ignore_case(const char *haystack, const char *needle) {
    if (!haystack || !needle || needle[0] == '\0') return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            char hc = haystack[i + j];
            char nc = needle[j];
            /* tolower equivalent without locale dependency */
            if (hc >= 'A' && hc <= 'Z') hc += 32;
            if (nc >= 'A' && nc <= 'Z') nc += 32;
            if (hc != nc) break;
        }
        if (j == nlen) return true;
    }
    return false;
}

static void append_history_tier(cbm_history_store_t *hs, const char *project, const char *query,
                                const char *event_type, const char *tier_name, md_buf_t *md) {
    char **payloads = NULL;
    int count = 0;
    if (cbm_history_query_events(hs, project, NULL, event_type, &payloads, &count) != CBM_HISTORY_OK) {
        return;
    }

    int matched = 0;
    for (int i = 0; i < count; i++) {
        if (string_contains_ignore_case(payloads[i], query)) {
            if (matched == 0) {
                if (strncmp(tier_name, "###", 3) == 0) {
                    md_append(md, "%s\n\n", tier_name);
                } else {
                    md_append(md, "### %s\n\n", tier_name);
                }
            }

            yyjson_doc *doc = yyjson_read(payloads[i], strlen(payloads[i]), 0);
            if (doc) {
                size_t jslen = 0;
                const char *pretty = yyjson_write(doc, YYJSON_WRITE_PRETTY, &jslen);
                if (pretty) {
                    md_append(md, "```json\n%s\n```\n\n", pretty);
                    free((void*)pretty);
                }
                yyjson_doc_free(doc);
            } else {
                /* Fallback: show raw payload so context is never lost */
                md_append(md, "```\n%s\n```\n\n", payloads[i]);
            }
            matched++;
            if (matched >= 5) break; /* limit to top 5 */
        }
    }

    for (int i = 0; i < count; i++) free(payloads[i]);
    if (payloads) free(payloads);
}

static void append_codebase_tier(cbm_store_t *codebase, const char *project, const char *query, md_buf_t *md) {
    md_append(md, "### Tier 3: Codebase & Database Graph Dependencies\n\n");

    cbm_search_params_t p = {0};
    p.project = project;
    p.name_pattern = query;
    p.limit = 5;

    cbm_search_output_t out = {0};
    if (cbm_store_search(codebase, &p, &out) == CBM_STORE_OK && out.count > 0) {
        for (int i = 0; i < out.count; i++) {
            cbm_node_t *n = &out.results[i].node;
            md_append(md, "- **%s** (`%s`) in `%s`\n",
                      n->name ? n->name : "?",
                      n->label ? n->label : "?",
                      n->file_path ? n->file_path : "?");

            /* Traverse edges to find DB dependencies */
            cbm_edge_t *edges = NULL;
            int ecount = 0;
            if (cbm_store_find_edges_by_source(codebase, n->id, &edges, &ecount) == CBM_STORE_OK) {
                for (int j = 0; j < ecount; j++) {
                    if (!edges[j].type) continue;
                    if (strcmp(edges[j].type, "QUERIES") == 0 ||
                        strcmp(edges[j].type, "USES_TABLE") == 0 ||
                        strcmp(edges[j].type, "REFERENCES_TABLE") == 0) {
                        cbm_node_t tgt = {0};
                        if (cbm_store_find_node_by_id(codebase, edges[j].target_id, &tgt) == CBM_STORE_OK) {
                            md_append(md, "  - DB Dependency: `[%s] %s`\n",
                                      tgt.label ? tgt.label : "?",
                                      tgt.name ? tgt.name : "?");
                        }
                    }
                }
                cbm_store_free_edges(edges, ecount);
            }
        }
        md_append(md, "\n");
    } else {
        md_append(md, "_No direct structural nodes found for query._\n\n");
    }
    cbm_store_search_free(&out);
}

int cbm_build_engineering_context(cbm_store_t *codebase,
                                  cbm_history_store_t *history,
                                  const char *project,
                                  const char *query,
                                  char **out_markdown) {
    if (!history || !project || !query || !out_markdown) return -1;

    md_buf_t md = {0};
    if (md_append(&md, "# Engineering Context: `%s`\n\n", query) < 0) {
        md_buf_free(&md);
        return -1;
    }

    /* Tier 1: Patterns (highest priority — established organizational rules) */
    append_history_tier(history, project, query, "ENGINEERING_PATTERN", "### Tier 1: Engineering Patterns", &md);

    /* Tier 2: Corrections (prior known gotchas and their fixes) */
    append_history_tier(history, project, query, "CORRECTION_MEMORY", "### Tier 2: Previous Corrections", &md);

    /* Tier 3: Code & DB Graph (structural dependencies) */
    if (codebase) {
        append_codebase_tier(codebase, project, query, &md);
    }

    /* Tier 4: Failures (unresolved errors linked to this area) */
    append_history_tier(history, project, query, "FAILURE_CORRELATION", "### Tier 4: Recent Failures", &md);

    /* Tier 5: Recent changes (context for what has changed recently) */
    append_history_tier(history, project, query, "CHANGE_CORRELATION", "### Tier 5: Recent Changes", &md);

    *out_markdown = md.buf;
    return 0;
}

int cbm_inspect_session(cbm_history_store_t *history,
                        const char *project,
                        const char *session_id,
                        char **out_markdown) {
    if (!history || !project || !session_id || !out_markdown) return -1;

    md_buf_t md = {0};
    md_append(&md, "# Session Inspection: `%s`\n\n", session_id);

    char **types = NULL;
    char **payloads = NULL;
    int count = 0;

    if (cbm_history_query_thread_timeline(history, project, session_id, &types, &payloads, &count) == CBM_HISTORY_OK) {
        if (count == 0) {
            md_append(&md, "_No events found for session `%s`._\n\n", session_id);
        }
        for (int i = 0; i < count; i++) {
            md_append(&md, "### [%d] %s\n\n", i + 1, types[i]);
            yyjson_doc *doc = yyjson_read(payloads[i], strlen(payloads[i]), 0);
            if (doc) {
                size_t jslen = 0;
                const char *pretty = yyjson_write(doc, YYJSON_WRITE_PRETTY, &jslen);
                if (pretty) {
                    md_append(&md, "```json\n%s\n```\n\n", pretty);
                    free((void*)pretty);
                }
                yyjson_doc_free(doc);
            } else {
                md_append(&md, "```\n%s\n```\n\n", payloads[i]);
            }
        }
        for (int i = 0; i < count; i++) {
            free(types[i]);
            free(payloads[i]);
        }
        if (types) free(types);
        if (payloads) free(payloads);
    } else {
        md_append(&md, "_Could not query history for session `%s`._\n\n", session_id);
    }

    *out_markdown = md.buf;
    return md.buf ? 0 : -1;
}

int cbm_inspect_history_type(cbm_history_store_t *history,
                             const char *project,
                             const char *event_type,
                             const char *query,
                             char **out_markdown) {
    if (!history || !project || !event_type || !query || !out_markdown) return -1;

    md_buf_t md = {0};
    md_append(&md, "# Inspect %s: `%s`\n\n", event_type, query);

    if (strcmp(event_type, "ENGINEERING_PATTERN") == 0) {
        md_append(&md, "> **IMPORTANT**: These are candidate patterns based on historical evidence. "
                       "They are NOT absolute project rules unless explicitly confirmed by the user.\n\n");
    }

    append_history_tier(history, project, query, event_type, event_type, &md);

    if (!md.buf) {
        /* Ensure we always return at least a valid empty response */
        md_append(&md, "_No results found for query `%s`._\n", query);
    }

    *out_markdown = md.buf;
    return md.buf ? 0 : -1;
}
