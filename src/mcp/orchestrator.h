/*
 * orchestrator.h — Engineering Context Engine
 */
#ifndef CBM_ORCHESTRATOR_H
#define CBM_ORCHESTRATOR_H

#include "store/store.h"
#include "store/history_store.h"

/*
 * Builds a unified engineering context string (Markdown) for a given query.
 * The query is searched against the codebase graph, database dependencies,
 * and the history store (Patterns, Corrections, Failures).
 * Results are ranked by relevance/tier.
 * 
 * Returns 0 on success, assigning *out_markdown to a heap-allocated string.
 */
int cbm_build_engineering_context(cbm_store_t *codebase,
                                  cbm_history_store_t *history,
                                  const char *project,
                                  const char *query,
                                  char **out_markdown);

int cbm_inspect_session(cbm_history_store_t *history,
                        const char *project,
                        const char *session_id,
                        char **out_markdown);

int cbm_inspect_history_type(cbm_history_store_t *history,
                             const char *project,
                             const char *event_type,
                             const char *query,
                             char **out_markdown);

#endif /* CBM_ORCHESTRATOR_H */
