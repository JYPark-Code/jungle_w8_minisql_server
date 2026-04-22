#include "protocol.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HTTP_MAX_HEADER_BYTES 8192
#define HTTP_MAX_BODY_BYTES   (1024 * 1024)

static const char *status_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default:  return "OK";
    }
}

static int ascii_ieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static char *trim(char *s) {
    char *end;
    while (*s && isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static char *find_header_end(char *buf, size_t len) {
    if (len < 4) return NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

static int parse_content_length(const char *value, size_t *out) {
    char *end = NULL;
    unsigned long parsed;

    if (!value || !out || *value == '\0') return -1;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value) return -1;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0' || parsed > HTTP_MAX_BODY_BYTES) return -1;

    *out = (size_t)parsed;
    return 0;
}

static int write_all(int fd, const char *buf, size_t len) {
    size_t written = 0;

    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        written += (size_t)n;
    }
    return 0;
}

int http_parse_request(int fd, http_request_t *out) {
    char header_buf[HTTP_MAX_HEADER_BYTES + 1];
    size_t total = 0;
    char *header_end = NULL;
    size_t header_len;
    char *line;
    char *save = NULL;
    char *target;
    char *version;
    size_t extra_len;
    size_t copied;

    if (fd < 0 || !out) return -1;
    memset(out, 0, sizeof(*out));

    while (total < HTTP_MAX_HEADER_BYTES) {
        ssize_t n = read(fd, header_buf + total, HTTP_MAX_HEADER_BYTES - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        total += (size_t)n;
        header_end = find_header_end(header_buf, total);
        if (header_end) break;
    }

    if (!header_end) return -1;
    header_len = (size_t)(header_end - header_buf) + 4U;
    header_buf[header_len - 2U] = '\0';
    header_buf[total] = '\0';

    line = strtok_r(header_buf, "\r\n", &save);
    if (!line) return -1;

    target = strchr(line, ' ');
    if (!target) return -1;
    *target++ = '\0';
    target = trim(target);

    version = strchr(target, ' ');
    if (!version) return -1;
    *version++ = '\0';
    version = trim(version);

    if ((!ascii_ieq(line, "GET") && !ascii_ieq(line, "POST")) ||
        strncmp(version, "HTTP/", 5) != 0) {
        return -1;
    }

    snprintf(out->method, sizeof(out->method), "%s", ascii_ieq(line, "GET") ? "GET" : "POST");

    {
        char *query = strchr(target, '?');
        if (query) {
            *query++ = '\0';
            snprintf(out->query, sizeof(out->query), "%s", query);
        }
        snprintf(out->path, sizeof(out->path), "%s", target);
    }

    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
        char *colon = strchr(line, ':');
        char *name;
        char *value;

        if (!colon) return -1;
        *colon++ = '\0';
        name = trim(line);
        value = trim(colon);

        if (ascii_ieq(name, "Content-Length")) {
            if (parse_content_length(value, &out->content_length) != 0) return -1;
        } else if (ascii_ieq(name, "Content-Type")) {
            snprintf(out->content_type, sizeof(out->content_type), "%s", value);
        }
    }

    if (out->content_length == 0) return 0;

    out->body = malloc(out->content_length + 1U);
    if (!out->body) return -1;

    extra_len = total - header_len;
    copied = extra_len < out->content_length ? extra_len : out->content_length;
    if (copied > 0) {
        memcpy(out->body, header_buf + header_len, copied);
    }

    while (copied < out->content_length) {
        ssize_t n = read(fd, out->body + copied, out->content_length - copied);
        if (n < 0) {
            if (errno == EINTR) continue;
            http_request_free(out);
            return -1;
        }
        if (n == 0) {
            http_request_free(out);
            return -1;
        }
        copied += (size_t)n;
    }

    out->body[out->content_length] = '\0';
    return 0;
}

int http_write_response(int fd, const http_response_t *resp) {
    char header[512];
    const char *content_type;
    const char *body;
    size_t body_len;
    int header_len;

    if (fd < 0 || !resp) return -1;

    content_type = resp->content_type ? resp->content_type : "application/octet-stream";
    body = resp->body ? resp->body : "";
    body_len = resp->body ? resp->body_len : 0U;

    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          resp->status, status_reason(resp->status),
                          content_type, body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) return -1;

    if (write_all(fd, header, (size_t)header_len) != 0) return -1;
    if (body_len > 0 && write_all(fd, body, body_len) != 0) return -1;
    return 0;
}

void http_request_free(http_request_t *req) {
    if (!req) return;
    free(req->body);
    req->body = NULL;
}
