#pragma once
/* Shared type definitions for fsearch-mcp daemon.
   Replaces types that were previously provided by GTK headers. */

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Replacement for GtkSortType */
typedef enum {
    FSEARCH_SORT_ASCENDING  = 0,
    FSEARCH_SORT_DESCENDING = 1,
} FsearchSortType;

/* Application modes for MCP operation */
typedef enum {
    FSEARCH_MCP_MODE_SYNC,
    FSEARCH_MCP_MODE_ASYNC,
} FsearchMcpMode;

G_END_DECLS
