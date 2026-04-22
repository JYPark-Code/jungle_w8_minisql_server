#include "engine.h"
#include "dict_cache.h"
#include "router.h"
#include "protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DICT_CACHE_CAPACITY 1024
#define AUTOCOMPLETE_DEFAULT_LIMIT 5
#define AUTOCOMPLETE_MAX_LIMIT 20

static char s_web_root[512] = "./web";
static dict_cache_t *s_dict_cache;

void router_set_web_root(const char *web_root) {
    if (!web_root || web_root[0] == '\0') return;
    snprintf(s_web_root, sizeof(s_web_root), "%s", web_root);
}

static dict_cache_t *router_dict_cache(void) {
    if (s_dict_cache == NULL) {
        s_dict_cache = dict_cache_create(DICT_CACHE_CAPACITY);
    }
    return s_dict_cache;
}

static int method_is(const http_request_t *req, const char *method) {
    return req && strcmp(req->method, method) == 0;
}

static double router_now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
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

static int set_cached_body(http_response_t *resp, char *body) {
    if (resp == NULL || body == NULL) return -1;
    resp->status = 200;
    resp->content_type = "application/json";
    resp->body = body;
    resp->body_len = strlen(body);
    return 0;
}

static void append_json_escaped(char *out, size_t out_size, size_t *offset,
                                const char *text) {
    const unsigned char *p = (const unsigned char *)text;

    if (out == NULL || out_size == 0 || offset == NULL) return;
    while (p != NULL && *p != '\0' && *offset + 1U < out_size) {
        unsigned char ch = *p++;
        if ((ch == '"' || ch == '\\') && *offset + 2U < out_size) {
            out[(*offset)++] = '\\';
            out[(*offset)++] = (char)ch;
        } else if (ch == '\n' && *offset + 2U < out_size) {
            out[(*offset)++] = '\\';
            out[(*offset)++] = 'n';
        } else if (ch == '\r' && *offset + 2U < out_size) {
            out[(*offset)++] = '\\';
            out[(*offset)++] = 'r';
        } else if (ch == '\t' && *offset + 2U < out_size) {
            out[(*offset)++] = '\\';
            out[(*offset)++] = 't';
        } else if (ch >= 0x20U) {
            out[(*offset)++] = (char)ch;
        }
    }
    out[*offset] = '\0';
}

static int append_char(char *out, size_t out_size, size_t *offset, char ch) {
    if (out == NULL || out_size == 0 || offset == NULL) return -1;
    if (*offset + 1U >= out_size) return -1;
    out[(*offset)++] = ch;
    out[*offset] = '\0';
    return 0;
}

static int sql_escape_literal(const char *src, char *dst, size_t dst_size) {
    size_t out = 0;

    if (src == NULL || dst == NULL || dst_size == 0) return -1;
    while (*src != '\0') {
        if (*src == '\'') {
            if (out + 2U >= dst_size) return -1;
            dst[out++] = '\'';
            dst[out++] = '\'';
        } else {
            if (out + 1U >= dst_size) return -1;
            dst[out++] = *src;
        }
        src++;
    }
    dst[out] = '\0';
    return 0;
}

static int is_positive_int_text(const char *text) {
    if (text == NULL || text[0] == '\0') return 0;
    while (*text != '\0') {
        if (!isdigit((unsigned char)*text)) return 0;
        text++;
    }
    return 1;
}

static int is_ascii_lowercase_word(const char *text) {
    const unsigned char *p = (const unsigned char *)text;

    if (text == NULL || text[0] == '\0') return 0;
    while (*p != '\0') {
        if (*p < 'a' || *p > 'z') return 0;
        p++;
    }
    return 1;
}

static int parse_limit_param(const char *query, int *out_limit) {
    char limit_text[32];
    int limit;

    if (out_limit == NULL) return -1;
    *out_limit = AUTOCOMPLETE_DEFAULT_LIMIT;

    if (query_param(query, "limit", limit_text, sizeof(limit_text)) != 0) {
        return 0;
    }
    if (!is_positive_int_text(limit_text)) {
        return -1;
    }

    limit = atoi(limit_text);
    if (limit <= 0 || limit > AUTOCOMPLETE_MAX_LIMIT) {
        return -1;
    }

    *out_limit = limit;
    return 0;
}

static int json_read_string_at(const char **cursor, char *out, size_t out_size) {
    const char *p;
    size_t len = 0;

    if (cursor == NULL || *cursor == NULL || out == NULL || out_size == 0) return -1;
    p = *cursor;
    if (*p != '"') return -1;
    p++;

    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;

        if (ch == '"') {
            out[len] = '\0';
            *cursor = p;
            return 0;
        }

        if (ch == '\\') {
            ch = (unsigned char)*p++;
            if (ch == '"' || ch == '\\' || ch == '/') {
                /* keep ch as-is */
            } else if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 't') {
                ch = '\t';
            } else {
                return -1;
            }
        }

        if (ch < 0x20U || len + 1U >= out_size) return -1;
        out[len++] = (char)ch;
    }

    return -1;
}

static int json_get_string_field(const char *body, const char *field,
                                 char *out, size_t out_size) {
    char needle[96];
    const char *p;
    int n;

    if (body == NULL || field == NULL || out == NULL || out_size == 0) return -1;
    out[0] = '\0';

    n = snprintf(needle, sizeof(needle), "\"%s\"", field);
    if (n < 0 || (size_t)n >= sizeof(needle)) return -1;

    p = body;
    while ((p = strstr(p, needle)) != NULL) {
        p += (size_t)n;
        while (isspace((unsigned char)*p)) p++;
        if (*p != ':') continue;
        p++;
        while (isspace((unsigned char)*p)) p++;
        return json_read_string_at(&p, out, out_size);
    }

    return -1;
}

static int skip_json_string(const char **cursor) {
    const char *p;

    if (cursor == NULL || *cursor == NULL) return -1;
    p = *cursor;
    if (*p != '"') return -1;
    p++;

    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;
        if (ch == '"') {
            *cursor = p;
            return 0;
        }
        if (ch == '\\') {
            if (*p == '\0') return -1;
            p++;
        }
    }

    return -1;
}

static int build_suggestions_array(const char *engine_json, char *out,
                                   size_t out_size) {
    const char *p;
    size_t offset = 0;
    int first = 1;

    if (engine_json == NULL || out == NULL || out_size == 0) return -1;
    out[0] = '\0';
    if (append_char(out, out_size, &offset, '[') != 0) return -1;

    p = strstr(engine_json, "\"rows\":[");
    if (p == NULL) {
        return append_char(out, out_size, &offset, ']');
    }

    p = strchr(p, '[');
    if (p == NULL) return -1;
    p++;

    while (*p != '\0') {
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '[') {
            p++;
            continue;
        }

        p++;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '"') {
            char value[256];
            if (json_read_string_at(&p, value, sizeof(value)) != 0) return -1;
            if (!first && append_char(out, out_size, &offset, ',') != 0) return -1;
            if (append_char(out, out_size, &offset, '"') != 0) return -1;
            append_json_escaped(out, out_size, &offset, value);
            if (append_char(out, out_size, &offset, '"') != 0) return -1;
            first = 0;
        }

        while (*p != '\0') {
            if (*p == '"') {
                if (skip_json_string(&p) != 0) return -1;
            } else if (*p == ']') {
                p++;
                break;
            } else {
                p++;
            }
        }
    }

    return append_char(out, out_size, &offset, ']');
}

static int build_autocomplete_response(const char *prefix,
                                       const engine_result_t *result,
                                       char **out_json) {
    const char *engine_json;
    char prefix_buf[512];
    char suggestions[4096];
    size_t prefix_off = 0;
    size_t needed;
    char *json;

    if (prefix == NULL || result == NULL || out_json == NULL) return -1;
    *out_json = NULL;
    engine_json = result->json ? result->json : "{}";

    prefix_buf[0] = '\0';
    append_json_escaped(prefix_buf, sizeof(prefix_buf), &prefix_off, prefix);
    if (build_suggestions_array(engine_json, suggestions, sizeof(suggestions)) != 0) {
        return -1;
    }

    needed = strlen("{\"ok\":") + strlen(result->ok ? "true" : "false") +
             strlen(",\"prefix\":\"") + strlen(prefix_buf) +
             strlen("\",\"suggestions\":") + strlen(suggestions) +
             strlen(",\"elapsed_ms\":") + 32U +
             strlen(",\"engine\":") + strlen(engine_json) +
             strlen("}") + 1U;

    json = malloc(needed);
    if (json == NULL) return -1;

    snprintf(json, needed,
             "{\"ok\":%s,\"prefix\":\"%s\",\"suggestions\":%s,"
             "\"elapsed_ms\":%.3f,\"engine\":%s}",
             result->ok ? "true" : "false", prefix_buf, suggestions,
             result->elapsed_ms, engine_json);
    *out_json = json;
    return 0;
}

static int build_admin_insert_response(const char *english,
                                       const engine_result_t *result,
                                       char **out_json) {
    const char *engine_json;
    char english_buf[512];
    size_t english_off = 0;
    size_t needed;
    char *json;

    if (english == NULL || result == NULL || out_json == NULL) return -1;
    *out_json = NULL;
    engine_json = result->json ? result->json : "{}";

    english_buf[0] = '\0';
    append_json_escaped(english_buf, sizeof(english_buf), &english_off, english);

    needed = strlen("{\"ok\":") + strlen(result->ok ? "true" : "false") +
             strlen(",\"english\":\"") + strlen(english_buf) +
             strlen("\",\"elapsed_ms\":") + 32U +
             strlen(",\"engine\":") + strlen(engine_json) +
             strlen("}") + 1U;

    json = malloc(needed);
    if (json == NULL) return -1;

    snprintf(json, needed,
             "{\"ok\":%s,\"english\":\"%s\",\"elapsed_ms\":%.3f,\"engine\":%s}",
             result->ok ? "true" : "false", english_buf, result->elapsed_ms,
             engine_json);
    *out_json = json;
    return 0;
}

static int wrap_dict_response(int ok, const char *mode, const char *query, int cache_hit,
                              double cache_lookup_ms, const char *engine_json,
                              char **out_json) {
    const char *prefix = "{\"ok\":";
    char query_buf[512];
    size_t query_off = 0;
    size_t needed;
    char *json;

    if (mode == NULL || query == NULL || engine_json == NULL || out_json == NULL) return -1;
    *out_json = NULL;
    query_buf[0] = '\0';
    append_json_escaped(query_buf, sizeof(query_buf), &query_off, query);

    needed = strlen(prefix) + strlen(ok ? "true" : "false") +
             strlen(",\"cache_hit\":") + strlen(cache_hit ? "true" : "false") +
             strlen(",\"cache_lookup_ms\":") + 32U +
             strlen(",\"mode\":\"") + strlen(mode) +
             strlen("\",\"query\":\"") + strlen(query_buf) +
             strlen("\",\"engine\":") + strlen(engine_json) +
             strlen("}") + 1U;

    json = malloc(needed);
    if (json == NULL) return -1;
    snprintf(json, needed,
             "%s%s,\"cache_hit\":%s,\"cache_lookup_ms\":%.6f,\"mode\":\"%s\",\"query\":\"%s\",\"engine\":%s}",
             prefix, ok ? "true" : "false", cache_hit ? "true" : "false",
             cache_lookup_ms, mode, query_buf, engine_json);
    *out_json = json;
    return 0;
}

static int handle_autocomplete_request(const http_request_t *req,
                                       http_response_t *resp) {
    char prefix[256];
    char escaped[512];
    char sql[768];
    int limit;
    engine_result_t result;
    char *response_json = NULL;
    int status;

    if (!method_is(req, "GET")) {
        return set_body(resp, 405, "application/json",
                        "{\"ok\":false,\"error\":\"method_not_allowed\"}");
    }
    if (query_param(req->query, "prefix", prefix, sizeof(prefix)) != 0 ||
        !is_ascii_lowercase_word(prefix)) {
        return set_body(resp, 400, "application/json",
                        "{\"ok\":false,\"error\":\"invalid_prefix\"}");
    }
    if (parse_limit_param(req->query, &limit) != 0) {
        return set_body(resp, 400, "application/json",
                        "{\"ok\":false,\"error\":\"invalid_limit\"}");
    }
    if (sql_escape_literal(prefix, escaped, sizeof(escaped)) != 0 ||
        snprintf(sql, sizeof(sql),
                 "SELECT english FROM dictionary WHERE english LIKE '%s%%' LIMIT %d;",
                 escaped, limit) >= (int)sizeof(sql)) {
        return set_body(resp, 400, "application/json",
                        "{\"ok\":false,\"error\":\"query_too_long\"}");
    }

    /* ROUND2-WARMUP */
    if (!engine_is_ready()) {
        return set_body(resp, 503, "application/json",
            "{\"ok\":false,\"error\":\"warming_up\",\"retry_after_ms\":500}");
    }

    result = engine_exec_sql(sql, false);
    if (build_autocomplete_response(prefix, &result, &response_json) != 0) {
        engine_result_free(&result);
        return set_body(resp, 500, "application/json",
                        "{\"ok\":false,\"error\":\"response_too_large\"}");
    }
    status = result.ok ? 200 : 500;
    engine_result_free(&result);
    if (set_cached_body(resp, response_json) != 0) return -1;
    resp->status = status;
    return 0;
}

static int handle_admin_insert_request(const http_request_t *req,
                                       http_response_t *resp) {
    char english[256];
    char korean[256];
    char escaped_english[512];
    char escaped_korean[512];
    char sql[1024];
    engine_result_t result;
    char *response_json = NULL;
    int status;

    if (!method_is(req, "POST")) {
        return set_body(resp, 405, "application/json",
                        "{\"ok\":false,\"error\":\"method_not_allowed\"}");
    }
    if (req->body == NULL || req->content_length == 0) {
        return set_body(resp, 400, "application/json",
                        "{\"ok\":false,\"error\":\"empty_body\"}");
    }
    if (json_get_string_field(req->body, "english", english, sizeof(english)) != 0 ||
        json_get_string_field(req->body, "korean", korean, sizeof(korean)) != 0 ||
        !is_ascii_lowercase_word(english) || korean[0] == '\0') {
        return set_body(resp, 400, "application/json",
                        "{\"ok\":false,\"error\":\"invalid_payload\"}");
    }
    if (sql_escape_literal(english, escaped_english, sizeof(escaped_english)) != 0 ||
        sql_escape_literal(korean, escaped_korean, sizeof(escaped_korean)) != 0 ||
        snprintf(sql, sizeof(sql),
                 "INSERT INTO dictionary (english, korean) VALUES ('%s', '%s');",
                 escaped_english, escaped_korean) >= (int)sizeof(sql)) {
        return set_body(resp, 400, "application/json",
                        "{\"ok\":false,\"error\":\"payload_too_long\"}");
    }

    /* ROUND2-WARMUP */
    if (!engine_is_ready()) {
        return set_body(resp, 503, "application/json",
            "{\"ok\":false,\"error\":\"warming_up\",\"retry_after_ms\":500}");
    }

    result = engine_exec_sql(sql, false);
    if (result.ok) {
        dict_cache_t *cache = router_dict_cache();
        char key[320];
        if (cache != NULL &&
            snprintf(key, sizeof(key), "english:%s", english) < (int)sizeof(key)) {
            dict_cache_invalidate(cache, key);
        }
    }

    if (build_admin_insert_response(english, &result, &response_json) != 0) {
        engine_result_free(&result);
        return set_body(resp, 500, "application/json",
                        "{\"ok\":false,\"error\":\"response_too_large\"}");
    }
    status = result.ok ? 200 : 500;
    engine_result_free(&result);
    if (set_cached_body(resp, response_json) != 0) return -1;
    resp->status = status;
    return 0;
}

static int handle_dict_request(const http_request_t *req, http_response_t *resp) {
    dict_cache_t *cache;
    char english[256];
    char korean[256];
    char id[64];
    char escaped[512];
    char key[320];
    char sql[768];
    char *cached_json = NULL;
    char *response_json = NULL;
    engine_result_t result;
    const char *mode;
    const char *query;
    char nocache_param[8] = {0};
    int bypass_cache;
    int cache_found = 0;
    double cache_lookup_ms = 0.0;

    if (!method_is(req, "GET")) {
        return set_body(resp, 405, "application/json",
                        "{\"ok\":false,\"error\":\"method_not_allowed\"}");
    }

    cache = router_dict_cache();
    if (cache == NULL) {
        return set_body(resp, 500, "application/json",
                        "{\"ok\":false,\"error\":\"cache_unavailable\"}");
    }

    /* ROUND2-NOCACHE: 벤치마크용 cache 우회.
     * ?nocache=1 또는 ?nocache=true 면 lookup/put 전부 skip —
     * single vs multi 비교 시 캐시 히트로 인한 latency 왜곡 제거 목적. */
    bypass_cache = (query_param(req->query, "nocache", nocache_param, sizeof(nocache_param)) == 0 &&
                    (strcmp(nocache_param, "1") == 0 || strcmp(nocache_param, "true") == 0));

    if (query_param(req->query, "english", english, sizeof(english)) == 0 &&
        english[0] != '\0') {
        if (sql_escape_literal(english, escaped, sizeof(escaped)) != 0 ||
            snprintf(key, sizeof(key), "english:%s", english) >= (int)sizeof(key) ||
            snprintf(sql, sizeof(sql),
                     "SELECT korean FROM dictionary WHERE english = '%s';",
                     escaped) >= (int)sizeof(sql)) {
            return set_body(resp, 400, "application/json",
                            "{\"ok\":false,\"error\":\"query_too_long\"}");
        }
        mode = "english";
        query = english;
    } else if (query_param(req->query, "id", id, sizeof(id)) == 0 &&
               id[0] != '\0') {
        if (!is_positive_int_text(id) ||
            snprintf(key, sizeof(key), "id:%s", id) >= (int)sizeof(key) ||
            snprintf(sql, sizeof(sql),
                     "SELECT english, korean FROM dictionary WHERE id = %s;",
                     id) >= (int)sizeof(sql)) {
            return set_body(resp, 400, "application/json",
                            "{\"ok\":false,\"error\":\"invalid_id\"}");
        }
        mode = "id";
        query = id;
    } else if (query_param(req->query, "korean", korean, sizeof(korean)) == 0 &&
               korean[0] != '\0') {
        if (sql_escape_literal(korean, escaped, sizeof(escaped)) != 0 ||
            snprintf(sql, sizeof(sql),
                     "SELECT english FROM dictionary WHERE korean = '%s';",
                     escaped) >= (int)sizeof(sql)) {
            return set_body(resp, 400, "application/json",
                            "{\"ok\":false,\"error\":\"query_too_long\"}");
        }
        /* ROUND2-WARMUP */
        if (!engine_is_ready()) {
            return set_body(resp, 503, "application/json",
                "{\"ok\":false,\"error\":\"warming_up\",\"retry_after_ms\":500}");
        }
        result = engine_exec_sql(sql, false);
        if (wrap_dict_response(result.ok, "korean", korean, 0, cache_lookup_ms,
                               result.json ? result.json : "{}", &response_json) != 0) {
            engine_result_free(&result);
            return set_body(resp, 500, "application/json",
                            "{\"ok\":false,\"error\":\"response_too_large\"}");
        }
        engine_result_free(&result);
        return set_cached_body(resp, response_json);
    } else {
        return set_body(resp, 400, "application/json",
                        "{\"ok\":false,\"error\":\"missing_query\"}");
    }

    if (!bypass_cache) {
        double cache_lookup_started = router_now_ms();
        cache_found = dict_cache_get(cache, key, &cached_json);
        cache_lookup_ms = router_now_ms() - cache_lookup_started;
    }

    if (cache_found) {
        if (wrap_dict_response(1, mode, query, 1, cache_lookup_ms,
                               cached_json, &response_json) != 0) {
            free(cached_json);
            return set_body(resp, 500, "application/json",
                            "{\"ok\":false,\"error\":\"response_too_large\"}");
        }
        free(cached_json);
        return set_cached_body(resp, response_json);
    }

    /* ROUND2-WARMUP: cache miss 로 engine 을 건드려야 하는 지점에서만 체크.
     * 포맷 오류 (405/400) 는 engine 과 무관하게 먼저 거른 뒤, 실제로 engine
     * 자원을 써야 하는 순간에 준비 여부 판단. */
    if (!engine_is_ready()) {
        return set_body(resp, 503, "application/json",
            "{\"ok\":false,\"error\":\"warming_up\",\"retry_after_ms\":500}");
    }

    result = engine_exec_sql(sql, false);
    if (wrap_dict_response(result.ok, mode, query, 0, cache_lookup_ms,
                           result.json ? result.json : "{}", &response_json) != 0) {
        engine_result_free(&result);
        return set_body(resp, 500, "application/json",
                        "{\"ok\":false,\"error\":\"response_too_large\"}");
    }
    if (!bypass_cache && result.ok && result.json != NULL) {
        dict_cache_put(cache, key, result.json);
    }
    engine_result_free(&result);

    return set_cached_body(resp, response_json);
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

    if (strcmp(req->path, "/api/dict") == 0) {
        return handle_dict_request(req, resp);
    }

    if (strcmp(req->path, "/api/autocomplete") == 0) {
        return handle_autocomplete_request(req, resp);
    }

    if (strcmp(req->path, "/api/admin/insert") == 0) {
        return handle_admin_insert_request(req, resp);
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
        dict_cache_t *cache;
        unsigned long cache_hits = 0;
        unsigned long cache_misses = 0;
        char body[384];
        if (!method_is(req, "GET")) {
            return set_body(resp, 405, "application/json",
                            "{\"ok\":false,\"error\":\"method_not_allowed\"}");
        }
        cache = router_dict_cache();
        if (cache != NULL) {
            cache_hits = dict_cache_hits(cache);
            cache_misses = dict_cache_misses(cache);
        }
        engine_get_stats(&total, &lock_wait);
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"total_queries\":%llu,\"lock_wait_ns_total\":%llu,"
                 "\"cache_hits\":%lu,\"cache_misses\":%lu}",
                 (unsigned long long)total, (unsigned long long)lock_wait,
                 cache_hits, cache_misses);
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
