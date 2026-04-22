#include "engine.h"
#include "router.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char s_web_root[512] = "./web";

void router_set_web_root(const char *web_root) {
    if (!web_root || web_root[0] == '\0') return;
    snprintf(s_web_root, sizeof(s_web_root), "%s", web_root);
}

static int method_is(const http_request_t *req, const char *method) {
    return req && strcmp(req->method, method) == 0;
}

static int query_has_single_mode(const char *query) {
    const char *p = query;
    size_t key_len = strlen("mode=single");

    while (p && *p) {
        const char *next = strchr(p, '&');
        size_t len = next ? (size_t)(next - p) : strlen(p);
        if (len == key_len && strncmp(p, "mode=single", key_len) == 0) return 1;
        p = next ? next + 1 : NULL;
    }
    return 0;
}

static int from_hex(char c) {
    if (c >= '0' && c <= '9') return (char)(c - '0');
    if (c >= 'a' && c <= 'f') return (char)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (char)(c - 'A' + 10);
    return -1;
}

static void url_decode(const char *src, char *dst, size_t dst_size) {
    size_t out = 0;

    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    while (*src && out + 1U < dst_size) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = from_hex(src[1]);
            int lo = from_hex(src[2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }
        dst[out++] = (*src == '+') ? ' ' : *src;
        src++;
    }
    dst[out] = '\0';
}

static int query_param(const char *query, const char *key, char *out, size_t out_size) {
    const char *p = query;
    size_t key_len = strlen(key);

    if (!out || out_size == 0) return -1;
    out[0] = '\0';

    while (p && *p) {
        const char *next = strchr(p, '&');
        size_t len = next ? (size_t)(next - p) : strlen(p);
        if (len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            char encoded[1024];
            size_t value_len = len - key_len - 1U;
            if (value_len >= sizeof(encoded)) value_len = sizeof(encoded) - 1U;
            memcpy(encoded, p + key_len + 1U, value_len);
            encoded[value_len] = '\0';
            url_decode(encoded, out, out_size);
            return 0;
        }
        p = next ? next + 1 : NULL;
    }
    return -1;
}

static int set_body(http_response_t *resp, int status, const char *content_type,
                    const char *body) {
    size_t len;

    if (!resp || !body) return -1;
    len = strlen(body);
    resp->body = malloc(len + 1U);
    if (!resp->body) return -1;
    memcpy(resp->body, body, len + 1U);
    resp->body_len = len;
    resp->status = status;
    resp->content_type = content_type;
    return 0;
}

static int set_jsonf(http_response_t *resp, int status, const char *fmt,
                     const char *arg) {
    char body[1024];
    int n = snprintf(body, sizeof(body), fmt, arg ? arg : "");
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return set_body(resp, 500, "application/json",
                        "{\"ok\":false,\"error\":\"response_too_large\"}");
    }
    return set_body(resp, status, "application/json", body);
}

static void take_engine_result(http_response_t *resp, engine_result_t *result) {
    if (result->json) {
        resp->status = result->ok ? 200 : 500;
        resp->content_type = "application/json";
        resp->body = result->json;
        resp->body_len = strlen(result->json);
        result->json = NULL;
        return;
    }

    set_body(resp, result->ok ? 200 : 500, "application/json",
             result->ok
                 ? "{\"ok\":true}"
                 : "{\"ok\":false,\"error\":\"engine_unavailable\"}");
}

static const char *content_type_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcmp(dot, ".json") == 0) return "application/json";
    return "application/octet-stream";
}

static int read_file_response(const char *path, http_response_t *resp) {
    FILE *fp;
    struct stat st;
    long size;
    size_t read_size;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return set_jsonf(resp, 404,
                         "{\"ok\":false,\"error\":\"not_found\",\"path\":\"%s\"}",
                         path);
    }

    fp = fopen(path, "rb");
    if (!fp) {
        return set_body(resp, 404, "application/json",
                        "{\"ok\":false,\"error\":\"not_found\"}");
    }

    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return set_body(resp, 500, "application/json",
                        "{\"ok\":false,\"error\":\"read_failed\"}");
    }
    size = ftell(fp);
    if (size < 0 || fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return set_body(resp, 500, "application/json",
                        "{\"ok\":false,\"error\":\"read_failed\"}");
    }

    resp->body = malloc((size_t)size + 1U);
    if (!resp->body) {
        fclose(fp);
        return -1;
    }

    read_size = fread(resp->body, 1U, (size_t)size, fp);
    fclose(fp);
    if (read_size != (size_t)size) {
        free(resp->body);
        resp->body = NULL;
        return set_body(resp, 500, "application/json",
                        "{\"ok\":false,\"error\":\"read_failed\"}");
    }

    resp->body[(size_t)size] = '\0';
    resp->body_len = (size_t)size;
    resp->status = 200;
    resp->content_type = content_type_for_path(path);
    return 0;
}

static int serve_static(const http_request_t *req, http_response_t *resp) {
    const char *request_path = req->path;
    char path[1024];

    if (!method_is(req, "GET")) {
        return set_body(resp, 405, "application/json",
                        "{\"ok\":false,\"error\":\"method_not_allowed\"}");
    }

    if (!request_path || request_path[0] != '/' ||
        strstr(request_path, "..") != NULL || request_path[1] == '/') {
        return set_body(resp, 400, "application/json",
                        "{\"ok\":false,\"error\":\"bad_path\"}");
    }

    if (strcmp(request_path, "/") == 0) {
        request_path = "/concurrency.html";
    }

    if (snprintf(path, sizeof(path), "%s%s", s_web_root, request_path) >= (int)sizeof(path)) {
        return set_body(resp, 400, "application/json",
                        "{\"ok\":false,\"error\":\"path_too_long\"}");
    }

    return read_file_response(path, resp);
}

int router_dispatch(const http_request_t *req, http_response_t *resp) {
    if (!resp) return -1;
    memset(resp, 0, sizeof(*resp));

    if (!req) {
        return set_body(resp, 400, "application/json",
                        "{\"ok\":false,\"error\":\"bad_request\"}");
    }

    if (strcmp(req->path, "/api/query") == 0) {
        engine_result_t result;
        if (!method_is(req, "POST")) {
            return set_body(resp, 405, "application/json",
                            "{\"ok\":false,\"error\":\"method_not_allowed\"}");
        }
        if (!req->body || req->content_length == 0) {
            return set_body(resp, 400, "application/json",
                            "{\"ok\":false,\"error\":\"empty_sql\"}");
        }
        result = engine_exec_sql(req->body, query_has_single_mode(req->query));
        take_engine_result(resp, &result);
        engine_result_free(&result);
        return 0;
    }

    if (strcmp(req->path, "/api/explain") == 0) {
        char sql[1024];
        engine_result_t result;
        if (!method_is(req, "GET")) {
            return set_body(resp, 405, "application/json",
                            "{\"ok\":false,\"error\":\"method_not_allowed\"}");
        }
        if (query_param(req->query, "sql", sql, sizeof(sql)) != 0 || sql[0] == '\0') {
            return set_body(resp, 400, "application/json",
                            "{\"ok\":false,\"error\":\"missing_sql\"}");
        }
        result = engine_explain(sql);
        take_engine_result(resp, &result);
        engine_result_free(&result);
        return 0;
    }

    if (strcmp(req->path, "/api/stats") == 0) {
        uint64_t total = 0;
        uint64_t lock_wait = 0;
        char body[256];
        if (!method_is(req, "GET")) {
            return set_body(resp, 405, "application/json",
                            "{\"ok\":false,\"error\":\"method_not_allowed\"}");
        }
        engine_get_stats(&total, &lock_wait);
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"total_queries\":%llu,\"lock_wait_ns_total\":%llu}",
                 (unsigned long long)total, (unsigned long long)lock_wait);
        return set_body(resp, 200, "application/json", body);
    }

    if (strcmp(req->path, "/api/inject") == 0) {
        if (!method_is(req, "POST")) {
            return set_body(resp, 405, "application/json",
                            "{\"ok\":false,\"error\":\"method_not_allowed\"}");
        }
        return set_body(resp, 501, "application/json",
                        "{\"ok\":false,\"error\":\"not_implemented\",\"message\":\"/api/inject is outside server-protocol scope\"}");
    }

    if (strncmp(req->path, "/api/", 5) == 0) {
        return set_body(resp, 404, "application/json",
                        "{\"ok\":false,\"error\":\"not_found\"}");
    }

    return serve_static(req, resp);
}
