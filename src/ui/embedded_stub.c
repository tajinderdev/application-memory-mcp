/*
 * embedded_stub.c — Empty asset table when built without frontend.
 *
 * Used by the standard `cbm` target (no Node.js required).
 * The `cbm-with-ui` target replaces this with generated embedded_assets.c.
 */
#include "ui/embedded_assets.h"

#include <stddef.h>
#include <string.h>

static const unsigned char g_fallback_html[] = "<!doctype html><html><body><h3>Codebase Memory MCP API Server</h3></body></html>";

cbm_embedded_file_t CBM_EMBEDDED_FILES[] = {
    {"/index.html", g_fallback_html, sizeof(g_fallback_html) - 1, "text/html"}
};
const int CBM_EMBEDDED_FILE_COUNT = 1;

const cbm_embedded_file_t *cbm_embedded_lookup(const char *path) {
    if (path && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        return &CBM_EMBEDDED_FILES[0];
    }
    for (int i = 0; i < CBM_EMBEDDED_FILE_COUNT; i++) {
        if (strcmp(CBM_EMBEDDED_FILES[i].path, path) == 0) {
            return &CBM_EMBEDDED_FILES[i];
        }
    }
    return NULL;
}
