/* server.h — HTTP accept loop + 데몬 부팅/셧다운 인터페이스
 * ============================================================================
 * 담당: 용 형님  (src/server.c)
 *
 * 역할:
 *   - TCP socket 생성 / bind / listen / accept loop
 *   - accept 된 fd 를 threadpool job 으로 enqueue
 *   - SIGINT 수신 시 server_shutdown() 으로 graceful drain
 *   - 정적 파일 서빙 root 는 server_config_t.web_root 로 지정
 *
 * 호출 흐름:
 *   main()  ──▶  server_run(cfg)           (blocking)
 *                      │
 *                      ├─ engine_init(data_dir)
 *                      ├─ threadpool_create(workers)
 *                      └─ accept loop ──▶ threadpool_submit(fd)
 *
 *   SIGINT ─▶  server_shutdown()           (accept loop 중단 플래그)
 *
 * 주의:
 *   - server_run 은 shutdown 신호 받기 전까지 return 하지 않는다.
 *   - server_shutdown 은 signal-safe 해야 한다 (atomic flag 만 건드릴 것).
 * ============================================================================
 */

#ifndef SERVER_H
#define SERVER_H

typedef struct server_config {
    int         port;        /* listen 포트 (기본 8080) */
    const char *web_root;    /* 정적 파일 루트 (예: "./web") */
    const char *data_dir;    /* CSV / 인덱스 데이터 디렉토리 (예: "./data") */
    int         workers;     /* threadpool worker 수 */
} server_config_t;

/* 데몬 부팅. accept loop 진입. shutdown 신호 전까지 block.
 * return 0: 정상 종료 (SIGINT), 비 0: 부팅 실패 */
int  server_run(const server_config_t *cfg);

/* SIGINT 핸들러에서 호출. accept loop 에 종료 플래그 세팅.
 * 실제 drain/join 은 server_run 내부에서 수행. */
void server_shutdown(void);

#endif /* SERVER_H */
