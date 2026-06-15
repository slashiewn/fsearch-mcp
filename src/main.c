/* fsearch-mcp — FSearch-powered MCP server for Claude Code
 * Copyright © 2026
 * License: GPL v2 (inherited from FSearch core)
 *
 * Provides file search via MCP over stdio.
 * Uses the FSearch database engine as its indexing backend.
 */

#include "config.h"
#include "fsearch_apitypes.h"
#include "fsearch_database.h"
#include "fsearch_database_file.h"
#include "fsearch_database_index_store.h"
#include "fsearch_database_index_properties.h"
#include "fsearch_database_search_view.h"
#include "fsearch_database_search_info.h"
#include "fsearch_database_entry.h"
#include "fsearch_database_include.h"
#include "fsearch_query.h"
#include "fsearch_filter.h"
#include "fsearch_filter_manager.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <pthread.h>

/* ──────────────────────── JSON helpers ──────────────────────── */

typedef struct {
    char *buf;
    size_t cap, len;
} JsonBuilder;

static void jb_init(JsonBuilder *jb) {
    jb->buf = NULL; jb->cap = jb->len = 0;
}

static void jb_append(JsonBuilder *jb, const char *s, size_t n) {
    if (jb->len + n + 1 > jb->cap) {
        jb->cap = jb->cap ? jb->cap * 2 : 8192;
        if (jb->len + n + 1 > jb->cap) jb->cap = jb->len + n + 1;
        jb->buf = g_realloc(jb->buf, jb->cap);
    }
    memcpy(jb->buf + jb->len, s, n);
    jb->len += n;
    jb->buf[jb->len] = '\0';
}

static void jb_puts(JsonBuilder *jb, const char *s) {
    jb_append(jb, s, strlen(s));
}

static void jb_json_str(JsonBuilder *jb, const char *s) {
    jb_append(jb, "\"", 1);
    while (s && *s) {
        unsigned char c = *s++;
        if (c == '"' || c == '\\') { jb_append(jb, "\\", 1); jb_append(jb, (char*)&c, 1); }
        else if (c < 0x20) { char esc[8]; snprintf(esc, 8, "\\u%04x", c); jb_puts(jb, esc); }
        else jb_append(jb, (char*)&c, 1);
    }
    jb_append(jb, "\"", 1);
}

static void jb_free(JsonBuilder *jb) {
    g_free(jb->buf);
    jb->buf = NULL; jb->cap = jb->len = 0;
}

static void send_jsonrpc(unsigned id, const char *body) {
    char header[256];
    int n = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", strlen(body));
    fwrite(header, 1, n, stdout);
    fwrite(body, 1, strlen(body), stdout);
    fflush(stdout);
}

static void send_result(unsigned id, const char *result_json) {
    char *msg = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":%u,\"result\":%s}", id, result_json);
    send_jsonrpc(id, msg);
    g_free(msg);
}

static void send_error(unsigned id, int code, const char *msg_text) {
    /* Quick inline JSON string escaping for error messages */
    const char *safe = msg_text ? msg_text : "unknown error";
    /* Count length needed */
    size_t slen = 0;
    const char *p = safe;
    while (*p) {
        if (*p == '"' || *p == '\\' || *p < 0x20) slen += 4;
        else slen++;
        p++;
    }
    /* Build the JSON string manually to avoid another g_strdup_printf layer */
    size_t mlen = slen + 2; /* quotes + content */
    char *msg = g_malloc(256 + mlen);
    int n = snprintf(msg, 256 + mlen,
        "{\"jsonrpc\":\"2.0\",\"id\":%u,\"error\":{\"code\":%d,\"message\":\"",
        id, code);
    p = safe;
    while (*p) {
        unsigned char c = *p++;
        if (c == '"') { msg[n++] = '\\'; msg[n++] = '"'; }
        else if (c == '\\') { msg[n++] = '\\'; msg[n++] = '\\'; }
        else if (c < 0x20) { n += snprintf(msg+n, 6, "\\u%04x", c); }
        else msg[n++] = c;
    }
    n += snprintf(msg+n, 48, "\"}}");
    msg[n] = '\0';
    send_jsonrpc(id, msg);
    g_free(msg);
}

/* ────────────── MCP handlers ────────────── */

static void handle_initialize(unsigned id) {
    char *full = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":%u,\"result\":{"
        "\"protocolVersion\":\"2024-11-05\","
        "\"serverInfo\":{\"name\":\"fsearch-mcp\",\"version\":\"" PACKAGE_VERSION "\"},"
        "\"capabilities\":{\"tools\":{}}}}", id);
    send_jsonrpc(id, full);
    g_free(full);
}

static void handle_list_tools(unsigned id) {
    const char *fmt =
        "{\"jsonrpc\":\"2.0\",\"id\":%u,\"result\":{\"tools\":["
        "{\"name\":\"search\",\"description\":\"Search indexed files by name or path pattern. Supports glob patterns (*.rs, **/test/*), partial names. Returns matching file paths.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query (glob or partial name)\"},\"limit\":{\"type\":\"number\",\"description\":\"Max results (default 50)\"}},\"required\":[\"query\"]}},"
        "{\"name\":\"scan\",\"description\":\"Scan directory and build/update the file index.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory to scan (default: current dir)\"}},\"required\":[]}},"
        "{\"name\":\"save\",\"description\":\"Save the current file index to disk.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Database file path\"}},\"required\":[]}},"
        "{\"name\":\"load\",\"description\":\"Load a previously saved file index from disk.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Database file path\"}},\"required\":[]}},"
        "{\"name\":\"status\",\"description\":\"Get database status: file count, indexed directories.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}}"
        "]}}";
    char *full = g_strdup_printf(fmt, id);
    send_jsonrpc(id, full);
    g_free(full);
}

/* ────────────── Database state ────────────── */

static FsearchDatabaseIndexStore *g_store = NULL;
static char *g_db_path = NULL;
static char *g_index_dir = NULL;
static pthread_mutex_t g_store_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_scanning = 0;
static volatile int g_verbose = 0;
static volatile uint32_t g_scan_files = 0;
static pthread_mutex_t g_stdout_lock = PTHREAD_MUTEX_INITIALIZER;

/* Thread-safe stdout write */
static void safe_write(const char *hdr, size_t hlen, const char *body, size_t blen) {
    pthread_mutex_lock(&g_stdout_lock);
    fwrite(hdr, 1, hlen, stdout);
    fwrite(body, 1, blen, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&g_stdout_lock);
}

/* Store event callback — receives progress from FSearch engine during scan */
static void
store_event_cb(FsearchDatabaseIndexStore *store,
               FsearchDatabaseIndexStoreEventKind kind,
               gpointer data,
               gpointer user_data)
{
    (void)store;
    (void)user_data;
    char *path = (char *)data;

    if (kind == FSEARCH_DATABASE_INDEX_STORE_EVENT_PROGRESS) {
        g_scan_files++;
        if (g_verbose) {
            fprintf(stderr, "\r\033[K📁 已扫描 %u 个文件...  %s", g_scan_files, path ? path : "");
            fflush(stderr);
        }
        /* Send MCP notification on stdout (message framing only) */
        char *notif = g_strdup_printf(
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{"
            "\"files\":%u}}",
            g_scan_files);
        size_t nlen = strlen(notif);
        char header[64];
        int hn = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", nlen);
        safe_write(header, hn, notif, nlen);
        g_free(notif);

    } else if (kind == FSEARCH_DATABASE_INDEX_STORE_EVENT_CONTENT_CHANGED) {
        uint32_t nf = fsearch_database_index_store_get_num_files(store);
        uint32_t nd = fsearch_database_index_store_get_num_folders(store);
        if (g_verbose) {
            fprintf(stderr, "\r\033[K✅ 扫描完成: %u 文件, %u 目录\n", nf, nd);
            fflush(stderr);
        }
        /* Final progress notification */
        char *fin = g_strdup_printf(
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{"
            "\"files\":%u,\"folders\":%u,\"done\":true}}", nf, nd);
        size_t flen = strlen(fin);
        char fheader[64];
        int fhn = snprintf(fheader, sizeof fheader, "Content-Length: %zu\r\n\r\n", flen);
        safe_write(fheader, fhn, fin, flen);
        g_free(fin);
    }
    g_free(data);
}

/* Background scan thread */
typedef struct {
    char *path;
} ScanArg;

static void* scan_thread(void *arg) {
    ScanArg *sa = (ScanArg*)arg;

    /* Create include manager */
    FsearchDatabaseIncludeManager *im = fsearch_database_include_manager_new();
    FsearchDatabaseInclude *inc = fsearch_database_include_new(sa->path, TRUE, FALSE, TRUE, TRUE, 0, 0);
    if (inc) {
        fsearch_database_include_manager_add(im, inc);
        fsearch_database_include_unref(inc);
    }

    FsearchDatabaseExcludeManager *em = fsearch_database_exclude_manager_new();
    FsearchDatabaseIndexPropertyFlags flags =
        DATABASE_INDEX_PROPERTY_NAME |
        DATABASE_INDEX_PROPERTY_PATH |
        DATABASE_INDEX_PROPERTY_MODIFICATION_TIME;

    g_scan_files = 0;

    FsearchDatabaseIndexStore *store = fsearch_database_index_store_new(im, em, flags, store_event_cb, NULL);
    if (store) {
        fsearch_database_index_store_start(store, NULL);

        pthread_mutex_lock(&g_store_lock);
        if (g_store) fsearch_database_index_store_unref(g_store);
        g_store = store;
        g_scanning = 0;
        pthread_mutex_unlock(&g_store_lock);
    } else {
        pthread_mutex_lock(&g_store_lock);
        g_scanning = 0;
        pthread_mutex_unlock(&g_store_lock);
    }

    free(sa->path);
    free(sa);
    return NULL;
}

/* ────────────── search ────────────── */

static FsearchDatabaseIndexStore* get_store(void) {
    pthread_mutex_lock(&g_store_lock);
    FsearchDatabaseIndexStore *s = g_store;
    if (s) fsearch_database_index_store_ref(s);
    pthread_mutex_unlock(&g_store_lock);
    return s;
}

static void set_store(FsearchDatabaseIndexStore *s) {
    pthread_mutex_lock(&g_store_lock);
    if (g_store) fsearch_database_index_store_unref(g_store);
    g_store = s;
    if (s) fsearch_database_index_store_ref(s);
    pthread_mutex_unlock(&g_store_lock);
}

static void handle_search(unsigned id, const char *query_str, int limit) {
    FsearchDatabaseIndexStore *store = get_store();
    if (!store) {
        send_error(id, -32000, "Database not initialized. Call scan or load first.");
        return;
    }
    if (!query_str || !*query_str) {
        fsearch_database_index_store_unref(store);
        send_error(id, -32002, "Empty query");
        return;
    }
    if (limit <= 0 || limit > 500) limit = 50;

    /* Build query object (filter/filters/query_id all NULL/0) */
    FsearchQuery *query = fsearch_query_new(query_str, NULL, NULL, (FsearchQueryFlags)0, NULL);
    if (!query) {
        fsearch_database_index_store_unref(store);
        send_error(id, -32003, "Failed to parse query");
        return;
    }

    /* Execute search — view_id = 0 */
    uint32_t view_id = 0;
    gboolean ok = fsearch_database_index_store_search(
        store, view_id, query,
        DATABASE_INDEX_PROPERTY_PATH,
        FSEARCH_SORT_ASCENDING,
        NULL);

    if (!ok) {
        fsearch_query_unref(query);
        send_error(id, -32004, "Search failed");
        return;
    }

    FsearchDatabaseSearchView *view =
        fsearch_database_index_store_get_search_view(store, view_id);
    if (!view) {
        fsearch_query_unref(query);
        fsearch_database_index_store_unref(store);
        send_error(id, -32005, "No search view");
        return;
    }

    FsearchDatabaseSearchInfo *info = fsearch_database_search_view_get_info(view);
    uint32_t total = info ? fsearch_database_search_info_get_num_entries(info) : 0;

    JsonBuilder jb;
    jb_init(&jb);
    jb_puts(&jb, "{\"total\":");
    char tbuf[32]; snprintf(tbuf, sizeof tbuf, "%u", total);
    jb_puts(&jb, tbuf);
    jb_puts(&jb, ",\"matches\":[");

    uint32_t count = 0;
    uint32_t max = total < (uint32_t)limit ? total : (uint32_t)limit;
    for (uint32_t i = 0; i < max; i++) {
        FsearchDatabaseEntry *entry =
            fsearch_database_search_view_get_entry_for_idx(view, i);
        if (!entry) continue;
        GString *gs = db_entry_get_path_full(entry);
        if (!gs) continue;
        char *path = g_string_free(gs, FALSE);
        if (!path) continue;
        if (count > 0) jb_puts(&jb, ",");
        jb_json_str(&jb, path);
        g_free(path);
        count++;
    }
    jb_puts(&jb, "]}");

    send_result(id, jb.buf);
    jb_free(&jb);
    if (info) fsearch_database_search_info_unref(info);
    fsearch_query_unref(query);
    fsearch_database_index_store_unref(store);
}

/* ────────────── scan / save / load / status ────────────── */

static void handle_scan(unsigned id, const char *path) {
    if (!path || !*path) {
        path = g_index_dir ? g_index_dir : ".";
    }
    g_free(g_index_dir);
    g_index_dir = g_strdup(path);

    pthread_mutex_lock(&g_store_lock);
    if (g_scanning) {
        pthread_mutex_unlock(&g_store_lock);
        send_error(id, -32013, "Scan already in progress");
        return;
    }
    g_scanning = 1;
    pthread_mutex_unlock(&g_store_lock);

    /* Spawn background scan thread */
    ScanArg *sa = calloc(1, sizeof(ScanArg));
    sa->path = g_strdup(g_index_dir);

    pthread_t thr;
    pthread_create(&thr, NULL, scan_thread, sa);
    pthread_detach(thr);

    JsonBuilder jb;
    jb_init(&jb);
    jb_puts(&jb, "{\"status\":\"scanning\",\"path\":");
    jb_json_str(&jb, g_index_dir);
    jb_puts(&jb, "}");
    send_result(id, jb.buf);
    jb_free(&jb);
}

static void handle_save(unsigned id, const char *path) {
    FsearchDatabaseIndexStore *store = get_store();
    if (!store) {
        send_error(id, -32000, "No database to save");
        return;
    }
    const char *save_path = path ? path : (g_db_path ? g_db_path : "fsearch-index.db");
    gboolean ok = fsearch_database_file_save(store, save_path);
    fsearch_database_index_store_unref(store);
    if (!ok) {
        send_error(id, -32011, "Failed to save database");
        return;
    }
    JsonBuilder jb;
    jb_init(&jb);
    jb_puts(&jb, "{\"status\":\"saved\",\"path\":");
    jb_json_str(&jb, save_path);
    jb_puts(&jb, "}");
    send_result(id, jb.buf);
    jb_free(&jb);
}

static void handle_load(unsigned id, const char *path) {
    const char *load_path = path ? path : (g_db_path ? g_db_path : "fsearch-index.db");

    FsearchDatabaseIndexStore *store = NULL;
    gboolean ok = fsearch_database_file_load(
        load_path, NULL, &store, NULL, NULL);
    if (!ok || !store) {
        send_error(id, -32012, "Failed to load database");
        return;
    }
    set_store(store);
    fsearch_database_index_store_unref(store);

    store = get_store();
    JsonBuilder jb;
    jb_init(&jb);
    jb_puts(&jb, "{\"status\":\"loaded\",\"path\":");
    jb_json_str(&jb, load_path);
    jb_puts(&jb, ",\"files\":");
    char fc[32]; snprintf(fc, sizeof fc, "%u",
        store ? fsearch_database_index_store_get_num_files(store) : 0);
    jb_puts(&jb, fc);
    jb_puts(&jb, ",\"folders\":");
    char foc[32]; snprintf(foc, sizeof foc, "%u",
        store ? fsearch_database_index_store_get_num_folders(store) : 0);
    jb_puts(&jb, foc);
    jb_puts(&jb, "}");
    send_result(id, jb.buf);
    jb_free(&jb);
    if (store) fsearch_database_index_store_unref(store);
}

static void handle_status(unsigned id) {
    FsearchDatabaseIndexStore *store = get_store();
    if (!store) {
        send_result(id, "{\"initialized\":false,\"files\":0,\"folders\":0}");
        return;
    }
    char buf[256];
    snprintf(buf, sizeof buf,
        "{\"initialized\":true,\"files\":%u,\"folders\":%u}",
        fsearch_database_index_store_get_num_files(store),
        fsearch_database_index_store_get_num_folders(store));
    fsearch_database_index_store_unref(store);
    send_result(id, buf);
}

/* ────────────── JSON-RPC message parser ────────────── */

static void dispatch_message(const char *body, size_t len) {
    (void)len;
    unsigned id = 0;
    char method[128] = {0};
    char param_query[1024] = {0};
    char param_path[1024] = {0};
    int param_limit = 0;

    /* Extract "id":<number> */
    const char *idp = strstr(body, "\"id\"");
    if (idp) {
        idp = strchr(idp, ':');
        if (idp) {
            while (*idp && (*idp == ':' || *idp == ' ')) idp++;
            id = (unsigned)strtoul(idp, NULL, 10);
        }
    }

    /* Extract "method":"<name>" */
    const char *mp = strstr(body, "\"method\"");
    if (!mp) return;
    mp = strchr(mp, ':');
    if (!mp) return;
    mp = strchr(mp, '"');
    if (!mp) return;
    mp++;
    const char *me = strchr(mp, '"');
    if (!me) return;
    size_t mlen = me - mp;
    if (mlen >= sizeof(method)) mlen = sizeof(method) - 1;
    memcpy(method, mp, mlen);
    method[mlen] = '\0';

    /* Extract params */
    const char *pp = strstr(body, "\"params\"");
    if (pp) {
        const char *qp = strstr(pp, "\"query\"");
        if (qp) {
            qp = strchr(qp, ':');
            if (qp) { qp = strchr(qp, '"'); if (qp) { qp++;
                const char *qe = strchr(qp, '"');
                if (qe) {
                    size_t qlen = qe - qp;
                    if (qlen >= sizeof(param_query)) qlen = sizeof(param_query) - 1;
                    memcpy(param_query, qp, qlen); param_query[qlen] = '\0';
                    char *s = param_query, *d = param_query;
                    while (*s) { if (*s == '\\' && *(s+1)) s++; *d++ = *s++; }
                    *d = '\0';
                }
            }}
        }
        const char *ppp = strstr(pp, "\"path\"");
        if (ppp) {
            ppp = strchr(ppp, ':');
            if (ppp) { ppp = strchr(ppp, '"'); if (ppp) { ppp++;
                const char *pe = strchr(ppp, '"');
                if (pe) {
                    size_t plen = pe - ppp;
                    if (plen >= sizeof(param_path)) plen = sizeof(param_path) - 1;
                    memcpy(param_path, ppp, plen); param_path[plen] = '\0';
                }
            }}
        }
        const char *lp = strstr(pp, "\"limit\"");
        if (lp) { lp = strchr(lp, ':'); if (lp) param_limit = (int)strtol(lp+1, NULL, 10); }
    }

    /* Dispatch by method */
    if (g_str_equal(method, "initialize"))
        handle_initialize(id);
    else if (g_str_equal(method, "tools/list"))
        handle_list_tools(id);
    else if (g_str_equal(method, "tools/call")) {
        /* Extract tool name from params */
        char tool[64] = {0};
        const char *np = strstr(pp ? pp : body, "\"name\"");
        if (np) {
            np = strchr(np, ':');
            if (np) { np = strchr(np, '"'); if (np) { np++;
                const char *ne = strchr(np, '"');
                if (ne) {
                    size_t tnl = ne - np;
                    if (tnl >= sizeof(tool)) tnl = sizeof(tool) - 1;
                    memcpy(tool, np, tnl); tool[tnl] = '\0';
                }
            }}
        }
        /* Trim spaces */
        while (tool[0] == ' ') memmove(tool, tool+1, strlen(tool));

        if      (g_str_equal(tool, "search")) handle_search(id, param_query, param_limit);
        else if (g_str_equal(tool, "scan"))   handle_scan(id, param_path);
        else if (g_str_equal(tool, "save"))   handle_save(id, param_path);
        else if (g_str_equal(tool, "load"))   handle_load(id, param_path);
        else if (g_str_equal(tool, "status")) handle_status(id);
        else send_error(id, -32601, "Tool not found");
    }
    else
        send_error(id, -32601, "Method not found");
}

/* ────────────── stdio transport loop ────────────── */

static char input_buf[65536];
static size_t input_len = 0;

static void process_input(void) {
    char *header_end = strstr(input_buf, "\r\n\r\n");
    if (!header_end) return;

    unsigned long content_length = 0;
    char *cl = strstr(input_buf, "Content-Length:");
    if (cl) {
        cl += 15;
        while (*cl == ' ') cl++;
        content_length = strtoul(cl, NULL, 10);
    }

    size_t header_len = (header_end - input_buf) + 4;
    if (input_len < header_len + content_length) return;

    char *body = input_buf + header_len;
    char saved = body[content_length];
    body[content_length] = '\0';

    dispatch_message(body, content_length);

    body[content_length] = saved;
    size_t consumed = header_len + content_length;
    size_t remaining = input_len - consumed;
    if (remaining > 0)
        memmove(input_buf, input_buf + consumed, remaining);
    input_len = remaining;

    /* Try processing next message (in case of pipelined messages) */
    process_input();
}

/* ────────────── Main ────────────── */

int main(int argc, char *argv[]) {
    const char *db_path = "fsearch-index.db";
    const char *index_dir = NULL;

    for (int i = 1; i < argc; i++) {
        if (g_str_equal(argv[i], "--db") && i+1 < argc)       db_path = argv[++i];
        else if (g_str_equal(argv[i], "--dir") && i+1 < argc) index_dir = argv[++i];
        else if (g_str_equal(argv[i], "-v") || g_str_equal(argv[i], "--verbose")) g_verbose = 1;
        else if (g_str_equal(argv[i], "--help")) {
            printf("fsearch-mcp [options]\n"
                   "  --db <path>   persistent index database path\n"
                   "  --dir <path>  directory to scan\n"
                   "  -v, --verbose show scan progress on stderr\n"
                   "  --help        this message\n");
            return 0;
        }
    }

    g_db_path = g_strdup(db_path);
    if (index_dir) g_index_dir = g_strdup(index_dir);

    /* Try loading existing database at startup */
    if (access(g_db_path, R_OK) == 0) {
        handle_load(0, g_db_path);
    }

    /* Main I/O loop */
    while (TRUE) {
        size_t space = sizeof(input_buf) - input_len - 1;
        if (space == 0) { input_len = 0; space = sizeof(input_buf) - 1; }

        ssize_t n = read(STDIN_FILENO, input_buf + input_len, space);
        if (n <= 0) break;
        input_len += (size_t)n;
        input_buf[input_len] = '\0';

        process_input();
    }

    if (g_store) fsearch_database_index_store_unref(g_store);
    g_free(g_db_path);
    g_free(g_index_dir);

    return 0;
}
