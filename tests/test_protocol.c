#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                                      \
    if (cond) {                                                    \
        g_pass++;                                                  \
    } else {                                                       \
        g_fail++;                                                  \
        fprintf(stderr, "  FAIL: %s\n", msg);                     \
    }                                                              \
} while (0)

static int write_all(int fd, const char *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    return 0;
}

static int parse_from_text(const char *text, http_request_t *req) {
    int fds[2];
    int rc;

    if (pipe(fds) != 0) return -1;
    if (write_all(fds[1], text, strlen(text)) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    close(fds[1]);
    rc = http_parse_request(fds[0], req);
    close(fds[0]);
    return rc;
}

static char *write_response_to_text(const http_response_t *resp) {
    int fds[2];
    char buffer[4096];
    ssize_t n;
    char *out;

    if (pipe(fds) != 0) return NULL;
    if (http_write_response(fds[1], resp) != 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    close(fds[1]);

    n = read(fds[0], buffer, sizeof(buffer) - 1U);
    close(fds[0]);
    if (n < 0) return NULL;
    buffer[n] = '\0';

    out = malloc((size_t)n + 1U);
    if (!out) return NULL;
    memcpy(out, buffer, (size_t)n + 1U);
    return out;
}

static void test_get_query_split(void) {
    http_request_t req;
    memset(&req, 0, sizeof(req));

    fprintf(stderr, "[protocol] GET path/query split\n");
    CHECK(parse_from_text("GET /api/explain?sql=SELECT+1 HTTP/1.1\r\nHost: localhost\r\n\r\n",
                          &req) == 0,
          "parse GET");
    CHECK(strcmp(req.method, "GET") == 0, "method GET");
    CHECK(strcmp(req.path, "/api/explain") == 0, "path split");
    CHECK(strcmp(req.query, "sql=SELECT+1") == 0, "query split");
    CHECK(req.content_length == 0, "no body");
    http_request_free(&req);
}

static void test_post_body(void) {
    http_request_t req;
    const char *raw =
        "POST /api/query?mode=single HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 20\r\n"
        "\r\n"
        "SELECT * FROM users;";

    memset(&req, 0, sizeof(req));
    fprintf(stderr, "[protocol] POST body\n");
    CHECK(parse_from_text(raw, &req) == 0, "parse POST");
    CHECK(strcmp(req.method, "POST") == 0, "method POST");
    CHECK(strcmp(req.path, "/api/query") == 0, "path");
    CHECK(strcmp(req.query, "mode=single") == 0, "query");
    CHECK(strcmp(req.content_type, "text/plain") == 0, "content type");
    CHECK(req.content_length == 20, "content length");
    CHECK(req.body && strcmp(req.body, "SELECT * FROM users;") == 0, "body");
    http_request_free(&req);
}

static void test_malformed_request(void) {
    http_request_t req;
    memset(&req, 0, sizeof(req));

    fprintf(stderr, "[protocol] malformed request\n");
    CHECK(parse_from_text("PUT /api/query HTTP/1.1\r\nHost: localhost\r\n\r\n",
                          &req) != 0,
          "unsupported method rejected");
    http_request_free(&req);

    memset(&req, 0, sizeof(req));
    CHECK(parse_from_text("POST /api/query HTTP/1.1\r\nContent-Length: nope\r\n\r\n",
                          &req) != 0,
          "bad content length rejected");
    http_request_free(&req);
}

static void test_response_writer(void) {
    http_response_t resp;
    char *out;

    fprintf(stderr, "[protocol] response writer\n");
    memset(&resp, 0, sizeof(resp));
    resp.status = 200;
    resp.content_type = "application/json";
    resp.body = "{\"ok\":true}";
    resp.body_len = strlen(resp.body);

    out = write_response_to_text(&resp);
    CHECK(out != NULL, "response text");
    if (out) {
        CHECK(strstr(out, "HTTP/1.1 200 OK\r\n") != NULL, "status line");
        CHECK(strstr(out, "Content-Type: application/json\r\n") != NULL, "content type");
        CHECK(strstr(out, "Content-Length: 11\r\n") != NULL, "content length");
        CHECK(strstr(out, "Connection: close\r\n") != NULL, "connection close");
        CHECK(strstr(out, "\r\n\r\n{\"ok\":true}") != NULL, "body");
    }
    free(out);
}

int main(void) {
    test_get_query_split();
    test_post_body();
    test_malformed_request();
    test_response_writer();

    fprintf(stderr, "\n[PROTOCOL TESTS] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
