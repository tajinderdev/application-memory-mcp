/*
 * test_e2e_context_pipeline.c
 *
 * End-to-end test for the full Engineering Context Pipeline:
 *
 * User instruction → agent change → file diff → test failure →
 * developer correction → successful implementation →
 * event ingestion → session reconstruction → change correlation →
 * failure correlation → correction memory →
 * engineering pattern promotion → context retrieval via MCP.
 *
 * Uses in-memory SQLite (":memory:") so it requires zero filesystem setup.
 * Build: link against history_store.o, correction_memory.o
 *        (no codebase_store or HTTP needed for this test).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "store/history_store.h"
#include "store/store.h"
#include "mcp/correction_memory.h"
#include "mcp/orchestrator.h"

/* ── Codebase Store Stubs for isolated test execution ─────────────────────── */

int cbm_store_search(cbm_store_t *s, const cbm_search_params_t *p, cbm_search_output_t *out) {
    (void)s; (void)p;
    if (!out) return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    return CBM_STORE_OK;
}

void cbm_store_search_free(cbm_search_output_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}

int cbm_store_find_edges_by_source(cbm_store_t *s, int64_t source_id, cbm_edge_t **out_edges, int *out_count) {
    (void)s; (void)source_id;
    if (out_edges) *out_edges = NULL;
    if (out_count) *out_count = 0;
    return CBM_STORE_OK;
}

int cbm_store_find_node_by_id(cbm_store_t *s, int64_t id, cbm_node_t *out_node) {
    (void)s; (void)id;
    if (out_node) memset(out_node, 0, sizeof(*out_node));
    return CBM_STORE_NOT_FOUND;
}

void cbm_store_free_edges(cbm_edge_t *edges, int count) {
    (void)edges; (void)count;
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */

#define ASSERT_OK(expr, msg)                                      \
    do {                                                          \
        if (!(expr)) {                                            \
            fprintf(stderr, "FAIL [%s:%d] %s\n",                 \
                    __FILE__, __LINE__, msg);                     \
            abort();                                              \
        }                                                         \
    } while(0)

#define ASSERT_STR_CONTAINS(haystack, needle)                     \
    do {                                                          \
        if (!(haystack) || !strstr((haystack), (needle))) {       \
            fprintf(stderr, "FAIL [%s:%d] Expected to find \"%s\" in output.\n" \
                            "  Got: %s\n",                        \
                    __FILE__, __LINE__, (needle),                  \
                    (haystack) ? (haystack) : "<NULL>");           \
            abort();                                              \
        }                                                         \
    } while(0)

static cbm_history_store_t *open_in_memory_store(void) {
    cbm_history_store_t *hs = cbm_history_store_open(":memory:");
    ASSERT_OK(hs != NULL, "cbm_history_store_open(:memory:) returned NULL");
    int rc = cbm_history_store_init_schema(hs);
    ASSERT_OK(rc == CBM_HISTORY_OK, "cbm_history_store_init_schema failed");
    return hs;
}

/* ── Test Payloads ────────────────────────────────────────────────────────── */

/* Simulate the payload from change_correlation after an agent modifies a file. */
static const char *CHANGE_PAYLOAD_WRONG =
    "[{\"file\": \"src/auth/login.c\", \"type\": \"CHANGE_CORRELATION\","
    " \"nodes\": [{\"name\": \"handle_login\", \"label\": \"Function\"}]}]";

/* Simulate what failure_correlation produces after a test run fails. */
static const char *FAILURE_PAYLOAD =
    "[{\"type\": \"FAILURE_CORRELATION\","
    " \"failure_snippet\": \"SEGFAULT in handle_login at src/auth/login.c:42\","
    " \"confidence\": \"HIGH\","
    " \"related_change_file\": \"src/auth/login.c\"}]";

/* Simulate the change_correlation payload after the dev corrects the bug. */
static const char *CHANGE_PAYLOAD_FIX =
    "[{\"file\": \"src/auth/login.c\", \"type\": \"CHANGE_CORRELATION\","
    " \"nodes\": [{\"name\": \"handle_login\", \"label\": \"Function\"}],"
    " \"note\": \"Fixed null pointer dereference before token check\"}]";

/* ── Test Cases ───────────────────────────────────────────────────────────── */

static void test_01_append_and_query_events(cbm_history_store_t *hs) {
    printf("  [01] Append and query events...\n");

    int rc;
    rc = cbm_history_append_event(hs, "proj", "sess1", "thread1", 1000,
                                  "CHANGE_CORRELATION", CHANGE_PAYLOAD_WRONG);
    ASSERT_OK(rc == CBM_HISTORY_OK, "append CHANGE failed");

    rc = cbm_history_append_event(hs, "proj", "sess1", "thread1", 2000,
                                  "FAILURE_CORRELATION", FAILURE_PAYLOAD);
    ASSERT_OK(rc == CBM_HISTORY_OK, "append FAILURE failed");

    rc = cbm_history_append_event(hs, "proj", "sess1", "thread1", 3000,
                                  "CHANGE_CORRELATION", CHANGE_PAYLOAD_FIX);
    ASSERT_OK(rc == CBM_HISTORY_OK, "append FIX failed");

    char **payloads = NULL;
    int count = 0;
    rc = cbm_history_query_events(hs, "proj", "thread1", "CHANGE_CORRELATION",
                                  &payloads, &count);
    ASSERT_OK(rc == CBM_HISTORY_OK, "query CHANGE events failed");
    ASSERT_OK(count == 2, "expected 2 CHANGE events");
    for (int i = 0; i < count; i++) free(payloads[i]);
    free(payloads);

    printf("  [01] PASS\n");
}

static void test_02_thread_exists_idempotency(cbm_history_store_t *hs) {
    printf("  [02] Thread exists (idempotency guard)...\n");

    int exists = cbm_history_thread_exists(hs, "proj", "thread1");
    ASSERT_OK(exists == 1, "thread1 should exist after test_01 appended to it");

    int notexists = cbm_history_thread_exists(hs, "proj", "ghost-thread");
    ASSERT_OK(notexists == 0, "ghost-thread should not exist");

    printf("  [02] PASS\n");
}

static void test_03_session_timeline_reconstruction(cbm_history_store_t *hs) {
    printf("  [03] Session timeline reconstruction (ordered ASC)...\n");

    char **types = NULL;
    char **payloads = NULL;
    int count = 0;
    int rc = cbm_history_query_thread_timeline(hs, "proj", "thread1",
                                               &types, &payloads, &count);
    ASSERT_OK(rc == CBM_HISTORY_OK, "timeline query failed");
    ASSERT_OK(count == 3, "expected 3 events in timeline");

    /* Verify order: CHANGE → FAILURE → CHANGE */
    ASSERT_OK(strcmp(types[0], "CHANGE_CORRELATION") == 0, "event[0] should be CHANGE");
    ASSERT_OK(strcmp(types[1], "FAILURE_CORRELATION") == 0, "event[1] should be FAILURE");
    ASSERT_OK(strcmp(types[2], "CHANGE_CORRELATION") == 0, "event[2] should be CHANGE (fix)");

    /* Verify the fix payload contains the note */
    ASSERT_STR_CONTAINS(payloads[2], "null pointer");

    for (int i = 0; i < count; i++) { free(types[i]); free(payloads[i]); }
    free(types);
    free(payloads);

    printf("  [03] PASS\n");
}

static void test_04_correction_memory_synthesis(cbm_history_store_t *hs) {
    printf("  [04] Correction memory synthesis (Change→Failure→Change)...\n");

    int rc = cbm_synthesize_corrections(hs, "proj", "sess1", "thread1", 4000);
    ASSERT_OK(rc == 0, "cbm_synthesize_corrections returned error");

    /* Verify a CORRECTION_MEMORY event was stored. */
    char **payloads = NULL;
    int count = 0;
    rc = cbm_history_query_events(hs, "proj", NULL, "CORRECTION_MEMORY",
                                  &payloads, &count);
    ASSERT_OK(rc == CBM_HISTORY_OK, "query CORRECTION_MEMORY failed");
    ASSERT_OK(count >= 1, "expected at least 1 CORRECTION_MEMORY event");

    /* Verify the correction payload contains the three phases */
    ASSERT_STR_CONTAINS(payloads[0], "attempted_change");
    ASSERT_STR_CONTAINS(payloads[0], "failure");
    ASSERT_STR_CONTAINS(payloads[0], "successful_correction");

    /* Verify file linkage — correction references login.c */
    ASSERT_STR_CONTAINS(payloads[0], "login.c");

    for (int i = 0; i < count; i++) free(payloads[i]);
    free(payloads);

    printf("  [04] PASS\n");
}

static void test_05_engineering_pattern_promotion(cbm_history_store_t *hs) {
    printf("  [05] Engineering pattern promotion (3+ corrections)...\n");

    /* Simulate 2 more identical failure+fix cycles on different threads
     * to bring the count to 3 total, triggering pattern promotion. */
    for (int i = 2; i <= 3; i++) {
        char thr[32];
        snprintf(thr, sizeof(thr), "thread%d", i);

        cbm_history_append_event(hs, "proj", "sess1", thr, 1000,
                                  "CHANGE_CORRELATION", CHANGE_PAYLOAD_WRONG);
        cbm_history_append_event(hs, "proj", "sess1", thr, 2000,
                                  "FAILURE_CORRELATION", FAILURE_PAYLOAD);
        cbm_history_append_event(hs, "proj", "sess1", thr, 3000,
                                  "CHANGE_CORRELATION", CHANGE_PAYLOAD_FIX);
        cbm_synthesize_corrections(hs, "proj", "sess1", thr, 4000);
    }

    /* Now check if an ENGINEERING_PATTERN was stored. */
    char **payloads = NULL;
    int count = 0;
    int rc = cbm_history_query_events(hs, "proj", NULL, "ENGINEERING_PATTERN",
                                      &payloads, &count);
    ASSERT_OK(rc == CBM_HISTORY_OK, "query ENGINEERING_PATTERN failed");
    ASSERT_OK(count >= 1, "expected at least 1 ENGINEERING_PATTERN after 3 corrections");

    /* Verify pattern contains expected fields */
    ASSERT_STR_CONTAINS(payloads[0], "recurring_failure_correction");
    ASSERT_STR_CONTAINS(payloads[0], "SEGFAULT");

    for (int i = 0; i < count; i++) free(payloads[i]);
    free(payloads);

    printf("  [05] PASS\n");
}

static void test_06_query_filters_by_type(cbm_history_store_t *hs) {
    printf("  [06] Query filters correctly by event type...\n");

    char **payloads = NULL;
    int count = 0;

    /* Should NOT return any patterns when querying for CHANGE events */
    int rc = cbm_history_query_events(hs, "proj", NULL, "CHANGE_CORRELATION",
                                      &payloads, &count);
    ASSERT_OK(rc == CBM_HISTORY_OK, "query CHANGE failed");
    for (int i = 0; i < count; i++) {
        ASSERT_OK(strstr(payloads[i], "recurring_failure_correction") == NULL,
                  "CHANGE query should not return PATTERN payloads");
        free(payloads[i]);
    }
    free(payloads);

    printf("  [06] PASS\n");
}

static void test_07_null_safety(cbm_history_store_t *hs) {
    printf("  [07] NULL safety on public APIs...\n");

    /* None of these should crash. */
    ASSERT_OK(cbm_history_append_event(NULL, "p", "s", "t", 0, "T", "{}") == CBM_HISTORY_ERR,
              "NULL store should return ERR");
    ASSERT_OK(cbm_history_append_event(hs, NULL, "s", "t", 0, "T", "{}") == CBM_HISTORY_ERR,
              "NULL project should return ERR");
    ASSERT_OK(cbm_history_thread_exists(NULL, "p", "t") == -1,
              "NULL store thread_exists should return -1");

    char **payloads = NULL;
    int count = 0;
    ASSERT_OK(cbm_history_query_events(NULL, "p", NULL, "T", &payloads, &count) == CBM_HISTORY_ERR,
              "NULL store query should return ERR");

    ASSERT_OK(cbm_synthesize_corrections(NULL, "p", "s", "t", 0) == -1,
              "NULL store synthesize should return -1");

    printf("  [07] PASS\n");
}

static void test_08_orchestrator_context_and_inspection(cbm_history_store_t *hs) {
    printf("  [08] Orchestrator context & history inspection...\n");

    char *markdown = NULL;

    /* 1. Inspect session */
    int rc = cbm_inspect_session(hs, "proj", "thread1", &markdown);
    ASSERT_OK(rc == 0, "cbm_inspect_session should return 0");
    ASSERT_OK(markdown != NULL, "markdown should not be NULL");
    ASSERT_STR_CONTAINS(markdown, "Session Inspection");
    ASSERT_STR_CONTAINS(markdown, "login.c");
    free(markdown);
    markdown = NULL;

    /* 2. Inspect pattern history */
    rc = cbm_inspect_history_type(hs, "proj", "ENGINEERING_PATTERN", "SEGFAULT", &markdown);
    ASSERT_OK(rc == 0, "cbm_inspect_history_type should return 0");
    ASSERT_OK(markdown != NULL, "markdown should not be NULL");
    ASSERT_STR_CONTAINS(markdown, "Inspect ENGINEERING_PATTERN");
    ASSERT_STR_CONTAINS(markdown, "recurring_failure_correction");
    free(markdown);
    markdown = NULL;

    /* 3. Build full engineering context (without active codebase store) */
    rc = cbm_build_engineering_context(NULL, hs, "proj", "login", &markdown);
    ASSERT_OK(rc == 0, "cbm_build_engineering_context should succeed");
    ASSERT_OK(markdown != NULL, "markdown should not be NULL");
    ASSERT_STR_CONTAINS(markdown, "# Engineering Context: `login`");
    ASSERT_STR_CONTAINS(markdown, "Tier 1: Engineering Patterns");
    ASSERT_STR_CONTAINS(markdown, "Tier 2: Previous Corrections");
    ASSERT_STR_CONTAINS(markdown, "Tier 4: Recent Failures");
    ASSERT_STR_CONTAINS(markdown, "Tier 5: Recent Changes");
    ASSERT_STR_CONTAINS(markdown, "login.c");
    free(markdown);
    markdown = NULL;

    printf("  [08] PASS\n");
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== End-to-End Context Pipeline Test ===\n\n");

    cbm_history_store_t *hs = open_in_memory_store();

    test_01_append_and_query_events(hs);
    test_02_thread_exists_idempotency(hs);
    test_03_session_timeline_reconstruction(hs);
    test_04_correction_memory_synthesis(hs);
    test_05_engineering_pattern_promotion(hs);
    test_06_query_filters_by_type(hs);
    test_07_null_safety(hs);
    test_08_orchestrator_context_and_inspection(hs);

    cbm_history_store_close(hs);

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
