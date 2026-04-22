#include "server.h"
#include "engine.h"
#include "protocol.h"
#include "router.h"
#include "threadpool.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

static atomic_int s_shutdown_flag = 0;

void router_set_web_root(const char *web_root);

static int create_listen_socket(int port) {
    int fd;
    int opt = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 128) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void free_response(http_response_t *resp) {
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}

static void write_simple_error(int fd, int status, const char *body) {
    http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = status;
    resp.content_type = "application/json";
    resp.body = (char *)body;
    resp.body_len = strlen(body);
    http_write_response(fd, &resp);
}

static void handle_client(void *arg) {
    int fd = (int)(intptr_t)arg;
    http_request_t req;
    http_response_t resp;

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    if (http_parse_request(fd, &req) != 0) {
        write_simple_error(fd, 400, "{\"ok\":false,\"error\":\"bad_request\"}");
        close(fd);
        return;
    }

    if (router_dispatch(&req, &resp) != 0) {
        free_response(&resp);
        write_simple_error(fd, 500, "{\"ok\":false,\"error\":\"internal_error\"}");
    } else {
        http_write_response(fd, &resp);
    }

    free_response(&resp);
    http_request_free(&req);
    close(fd);
}

int server_run(const server_config_t *cfg) {
    int listen_fd;
    threadpool_t *pool;

    if (!cfg) return 1;
    atomic_store(&s_shutdown_flag, 0);
    router_set_web_root(cfg->web_root);

    if (engine_init(cfg->data_dir) != 0) {
        fprintf(stderr, "[server] engine_init failed\n");
        return 1;
    }

    pool = threadpool_create(cfg->workers);
    if (!pool) {
        fprintf(stderr, "[server] threadpool_create failed\n");
        engine_shutdown();
        return 1;
    }

    listen_fd = create_listen_socket(cfg->port);
    if (listen_fd < 0) {
        fprintf(stderr, "[server] listen failed on port %d: %s\n", cfg->port, strerror(errno));
        threadpool_shutdown(pool);
        engine_shutdown();
        return 1;
    }

    fprintf(stderr, "[server] listening on port %d\n", cfg->port);

    while (!atomic_load(&s_shutdown_flag)) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (atomic_load(&s_shutdown_flag)) break;
            fprintf(stderr, "[server] accept failed: %s\n", strerror(errno));
            continue;
        }

        if (threadpool_submit(pool, handle_client, (void *)(intptr_t)client_fd) != 0) {
            static const char busy_resp[] =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Connection: close\r\n"
                "Retry-After: 1\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 33\r\n"
                "\r\n"
                "{\"ok\":false,\"error\":\"queue_full\"}";
            ssize_t wn = write(client_fd, busy_resp, sizeof(busy_resp) - 1);
            (void)wn;
            close(client_fd);
        }
    }

    close(listen_fd);
    if (threadpool_shutdown_graceful(pool, 5000) != 0) {
        fprintf(stderr, "[server] threadpool drain timed out\n");
    }
    engine_shutdown();
    return 0;
}

void server_shutdown(void) {
    atomic_store(&s_shutdown_flag, 1);
}
