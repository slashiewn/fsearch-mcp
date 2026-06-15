#pragma once
/* Stub replacement for Pango types and functions.
   FSearch uses Pango only for UI highlighting (bold text in search results).
   The daemon doesn't render anything — these stubs let the query engine
   compile without pulling in libpango. */

#include <glib.h>
#include <stdlib.h>

G_BEGIN_DECLS

/* Complete struct definitions (fields unused, just enough to compile) */
typedef struct _PangoAttribute {
    guint start_index;
    guint end_index;
    int dummy;
} PangoAttribute;

typedef struct _PangoAttrList {
    int dummy;
} PangoAttrList;

typedef struct _PangoAttrString {
    int dummy;
} PangoAttrString;

enum {
    PANGO_WEIGHT_BOLD = 700,
};

/* All functions are no-ops — highlight metadata is discarded */

static inline PangoAttribute *
pango_attr_weight_new(int weight) {
    return NULL;
}

static inline PangoAttrList *
pango_attr_list_new(void) {
    return NULL;
}

static inline PangoAttrList *
pango_attr_list_ref(PangoAttrList *list) {
    return list;
}

static inline void
pango_attr_list_unref(void *list) {
    (void)list;
}

static inline void
pango_attr_list_change(PangoAttrList *list, PangoAttribute *attr) {
    (void)list;
    free(attr);
}

G_END_DECLS
