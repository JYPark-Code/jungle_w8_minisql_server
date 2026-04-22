/* server.c — HTTP accept loop (stub, 용 형님 담당)
 * ============================================================================
 * 이 파일은 MP0 에서 PM 이 배치한 링크 통과용 stub 입니다.
 * 실제 구현은 feature/server-protocol 브랜치에서 용 형님이 작성합니다.
 *
 * 담당자 구현 가이드:
 *   1. engine_init(cfg->data_dir)          — 엔진 부팅
 *   2. threadpool_t *tp = threadpool_create(cfg->workers)
 *   3. socket/bind/listen/accept loop       — accept fd 를 job 으로 enqueue
 *   4. SIGINT 에서 server_shutdown() 호출 → accept loop 탈출 플래그
 *   5. 종료 시 threadpool_shutdown(tp); engine_shutdown();
 * ============================================================================
 */

#include "server.h"
#include "engine.h"
#include "threadpool.h"

#include <stdio.h>
#include <stdatomic.h>

static atomic_int s_shutdown_flag = 0;

int server_run(const server_config_t *cfg) {
    (void)cfg;
    fprintf(stderr, "[server] stub — 용 형님 구현 예정 (feature/server-protocol)\n");
    return 0;
}

void server_shutdown(void) {
    atomic_store(&s_shutdown_flag, 1);
}
