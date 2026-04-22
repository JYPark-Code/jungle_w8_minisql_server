# minisqld — Multi-threaded Mini DBMS in Pure C

> **Week 8.** W7 B+Tree Index DB 엔진 위에 **단일 C 데몬**으로 동작하는
> 멀티스레드 HTTP API 서버를 얹어, 외부 클라이언트가 SQL 을 실행할 수 있는
> 미니 DBMS 를 완성한다.

**핵심 차별점 (목표)**
- 순수 C11, 외부 의존 0 (HTTP/JSON 라이브러리 없이 직접 구현, pthread 만 허용)
- Thread pool + blocking job queue 로 동시 요청 처리
- 테이블 단위 `pthread_rwlock_t` 으로 SELECT 는 공유, DML 은 직렬화
- 단일 프로세스 상주 → W7 의 "subprocess 당 rebuild 고정비" 제거
- `?mode=single` 토글로 전역 직렬화 baseline 과 비교 가능

---

## 현재 개발 상태

**Round 4 (W8) — 1차 mix-merge 완료, 시연 회의 + 2차 리팩토링 대기.**

| MP | 내용 | 상태 |
|---|---|---|
| MP0 | PM 선작업 — 인터페이스 헤더 5종, engine_lock, Makefile, CI 4잡, stub | ✅ 완료 |
| MP1~MP3 | 팀원 각자 구현 + 단위 테스트 | ✅ 완료 |
| MP4 | 1차 PR 4 종 제출 + PM 1차 mix-merge (dev) | ✅ 완료 |
| MP5a | 시연 회의 + 2차 리팩토링 (탭 UI 통합 / 더미 주입 / stats 보강) | ⏳ 진행 |
| MP5b | TSan 통합 회귀 + loadtest 스크립트 | ⏳ |
| MP6 | README 실측 수치 확정 + 발표 리허설 + `dev → main` | ⏳ |

**지금 실제로 동작하는 것 (dev 기준)**
- `make` — `./minisqld` 전체 링크 성공
- `./minisqld --port 8080 --workers 8 --data-dir ./data --web-root ./web` 으로 기동,
  HTTP/1.1 API 서버 + 정적 파일 서빙 정상
- `/api/query` / `/api/query?mode=single` / `/api/explain` / `/api/stats` 동작
- `/` → `web/concurrency.html` (탭 구조 + 탭 (c) RW Contention 폴링)
- `/stress.html` → Concurrent Stress 실동작 (multi vs single 비교 UI)
- `make test` — W7 회귀 227 + `test_engine_lock` 8 + `test_threadpool` + `test_engine_concurrent` + `test_protocol` 통과
- `make tsan` — `./minisqld_tsan` 빌드
- `make valgrind` — W7 + W8 테스트 누수 0
- `make repl` — `./minisqld-repl` ANSI CLI (발표 백업 시연용)
- CI 4 개 잡 (build / test / tsan / valgrind) dev 에서 전부 green

**2차 리팩토링에서 정리할 것**
- `web/concurrency.html` 탭 (a) 스캐폴드에 `stress.html` 의 `runBatch` JS 이식 → 한 페이지로 통합
- `/api/inject` 501 → 실구현 (더미 10 만 행 주입)
- `stats.c` 분리 + `qps_last_sec` sliding window 추가
- stress 데모 측정 지표 (평균 latency / QPS) 추가 — 브라우저 제약 §아래 참조
- CI `toolchain install` 단계 스피드업 (`chore/ci-speedup`)

---

## 빠른 시작 (현재 상태 기준)

```bash
git clone https://github.com/JYPark-Code/jungle_w8_minisql_server.git
cd jungle_w8_minisql_server

# 빌드 + 회귀
make                        # ./minisqld + stub 링크 통과
make test                   # W7 227개 + engine_lock 8개 통과

# 데몬 기동 (MP0 시점엔 accept loop 가 stub 이라 즉시 종료)
./minisqld --version
./minisqld --help

# REPL 클라이언트 빌드 (서버 구현 후 실제 연결)
make repl
./minisqld-repl --help
```

**MP4~5 이후 예상 사용 흐름 (현재는 동작 X):**
```bash
./minisqld --port 8080 --workers 8 --data-dir ./data --web-root ./web &
curl -X POST http://localhost:8080/api/query \
     -H "Content-Type: text/plain" \
     -d "SELECT * FROM users WHERE id = 1"
# 또는
./minisqld-repl --port 8080
# 또는 브라우저로 http://localhost:8080 → 동시성 데모
```

---

## 아키텍처

```
[Client / Browser]
         │ HTTP/1.1
         ▼
[server.c]       accept loop
         │  enqueue(fd)
         ▼
[threadpool.c]   N workers (mutex + condvar blocking queue)
         │
         ▼
[protocol.c] HTTP parse → [router.c] method+path dispatch
         │
         ▼
[engine.c]       engine_lock 경유: 테이블 RW lock / catalog lock / single mutex
         │
         ▼
[parser / executor / storage / bptree]   (W7 엔진 자산, in-process 재사용)
```

| 레이어 | 파일 | 동시성 전략 |
|---|---|---|
| Socket | `src/server.c` | accept 는 메인 스레드, fd 를 job queue 로 전달 |
| Thread pool | `src/threadpool.c` | mutex + condvar 기반 blocking queue |
| HTTP | `src/protocol.c` + `src/router.c` | stateless (worker 당 요청 독립) |
| Engine | `src/engine.c` + `src/engine_lock.c` | 테이블 RW lock + 글로벌 catalog lock + single-mode mutex |
| Storage | `src/storage.c` (W7) | engine 레이어가 락으로 보호 |

자세한 다이어그램: [`docs/architecture.svg`](docs/architecture.svg) (MP5 에서 갱신 예정)

---

## API 설계

| Method | Path | 설명 | 상태 |
|---|---|---|---|
| POST | `/api/query` | SQL 실행, body 는 raw SQL (text/plain) | 🚧 stub |
| POST | `/api/query?mode=single` | 전역 mutex 로 강제 직렬화 (비교 baseline) | 🚧 stub |
| POST | `/api/inject` | 더미 데이터 주입 | 🚧 stub |
| GET | `/api/stats` | active workers, qps, lock wait 통계 | 🚧 stub |
| GET | `/api/explain?sql=...` | 인덱스 사용 여부, 노드 visit 수 | 🚧 stub |
| GET | `/` 이하 | 정적 파일 서빙 (`--web-root`) | 🚧 stub |

**응답 포맷 (합의):** `Content-Type: application/json`
```json
{"ok": true, "rows": [...], "elapsed_ms": 2.3, "index_used": true}
```

---

## 빌드 타겟

```bash
make                # ./minisqld 데몬 빌드                       ✅
make test           # W7 회귀 227 + engine_lock 단위 테스트 8    ✅
make tsan           # ThreadSanitizer 빌드 (./minisqld_tsan)     ✅
make valgrind       # W7 + engine_lock 누수/invalid 0            ✅
make bench          # B+Tree pure 벤치 (W7 자산)                 ✅
make repl           # ./minisqld-repl (ANSI CLI 클라이언트)       ✅
make loadtest       # 동시 N 요청 부하 테스트                      🚧 placeholder (MP5)
make clean          # 빌드 산출물 + data/ 잔여물 제거              ✅
```

**CFLAGS**: `-Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L`.
CI 는 `-Werror` 로 돌아감.

---

## 성능 (1차 mix-merge 직후 스냅샷)

> 이 수치는 **1 차 머지 직후 초기 측정값** 입니다. 2차 리팩토링 완료 후
> 더미 데이터 10만 행 기준으로 재측정해 최종 수치로 대체됩니다.

### 환경
- devcontainer (linux, ubuntu-latest 계열), 8 worker threads
- 테이블: `users` (빈 상태), SQL: `SELECT * FROM users WHERE id = 1`

### 서버 측 처리량 (curl 기준, 클라이언트 병목 없음)

64 요청을 **동시성 정도만 바꾸어** 발사한 전체 소요 시간:

| 시나리오 | 총 소요 | vs 순차 | 비고 |
|---|---|---|---|
| 순차 (`xargs -P 1`) | **380 ms** | 1.0× | HTTP + TCP 오버헤드 baseline |
| **멀티 (`xargs -P 8`)** | **85 ms** | **4.5×** | 실효 병렬 4~5 worker |
| 싱글 (`?mode=single`, `xargs -P 8`) | 145 ms | 2.6× | 전역 mutex 로 직렬화 |

- **멀티 vs 싱글 = 1.7× 차이** (쓰레드풀 + 테이블 RW lock 효과)
- 빈 테이블이라 쿼리 work 가 아주 작아 메시지가 희석됨. 10만 행 주입 후
  재측정 시 훨씬 큰 격차 예상 (2차 리팩토링 항목)

재현:
```bash
seq 1 64 | xargs -P 8 -I {} curl -s -o /dev/null http://localhost:8080/api/query \
  -H "Content-Type: text/plain" -d "SELECT * FROM users WHERE id = 1"
```

### ⚠️ 알려진 제약: 브라우저 기반 스트레스 테스트 왜곡

`web/stress.html` (용 형님 탭) 에서 "멀티" vs "싱글" 을 비교할 때
**거의 비슷한 시간이 나오는 현상** 이 보입니다. 서버 버그가 **아니라**
브라우저 특성 때문입니다:

1. 브라우저의 **per-origin 동시 HTTP/1.1 연결 한계 6** (Chrome/Firefox 공통)
   → JS 가 64 동시 요청을 보내도 실제로는 6 개씩 직렬 처리
2. 서버가 `Connection: close` 운용 (Keep-alive 미지원)
   → 요청마다 TCP 핸드셰이크 추가, 브라우저 내부 큐잉 증폭
3. 빈 테이블 SELECT 는 서버 work 가 HTTP 오버헤드 대비 너무 작아 차이가 노이즈에 묻힘

**진짜 서버 멀티 이득을 보려면** 위 `xargs` 명령을 쓰거나 `./minisqld-repl`
(향후 `\stress` 내장 명령) 을 사용하세요. 브라우저 시연용 수치는 상대비교
용도로만 참고.

2차 리팩토링에서 개선 예정:
- 더미 10만 행 주입 → 쿼리 work 증가 → 브라우저 6-limit 하에서도 차이 가시화
- stress UI 에 **평균 latency (ms/req)** / **QPS** 표기 추가 (총 시간만으로는 server-side 성능 분리 불가)
- `scripts/demo_stress.sh` 터미널 시연 스크립트 — 브라우저 제약 완전 우회

### 참고: 자료구조 pure 벤치
B+Tree 인덱스 vs 선형 탐색 수치는 W7 에서 측정한 **`make bench` 기준**
(up to 1,842×) — [`docs/README_w7.md`](docs/README_w7.md) 참조. Round 4 의
end-to-end 수치는 여기에 HTTP + lock 오버헤드가 상수로 더해진 값.

---

## 개발 참여 (팀원용)

**필수 숙지:**
1. [`TEAM_RULES.md`](TEAM_RULES.md) — 브랜치 전략 / PR 규약 / CI / 금지 사항
2. [`agent.md`](agent.md) — 라운드 설계 결정 + 4 분할 + 마일스톤
3. [`CLAUDE.md`](CLAUDE.md) — 레포 전역 규칙 (Claude Code 세션 포함)
4. `include/<본인파트>.h` — 구현 대상 인터페이스 시그니처 (임의 변경 금지)

**본인 브랜치:**
```bash
git fetch origin
git checkout feature/<영역>
```

| 담당 | 브랜치 | 구현 대상 |
|---|---|---|
| 동현 | `feature/engine-threadsafe` | `src/engine.c` (+ 최소 W7 엔진 수정) |
| 용 형님 | `feature/server-protocol` | `src/server.c`, `src/protocol.c`, `src/router.c`, `web/` 탭 (a) |
| 승진 | `feature/threadpool-stats` | `src/threadpool.c`, stats, `web/` 탭 (c) |
| 지용 (PM) | `feature/pm-infra` | `include/*.h`, `src/engine_lock.c`, `src/main.c`, Makefile, `client/repl.c` |

**PR 타겟**: `dev` (절대 `main` 아님). `dev` 보호 규약에 4 개 status check
(build/test/tsan/valgrind) 전부 green 이어야 merge 가능.

---

## 디렉토리

```
/
├─ include/               공개 인터페이스 (PM 관리, 시그니처 변경 금지)
├─ src/                   구현 (W7 엔진 + W8 신규 레이어)
├─ tests/                 회귀 + 동시성 단위 테스트
├─ bench/                 B+Tree pure 벤치 (W7 자산)
├─ scripts/               fixture 생성기
├─ client/                ANSI REPL CLI 클라이언트
├─ web/                   Round 4 동시성 데모 페이지 (구현 예정)
├─ docs/                  아키텍처 다이어그램 + W7 최종본 README
├─ agent/                 이전 라운드 AI 협업 컨텍스트 아카이브
├─ prev_project/          이전 라운드 웹 데모 자산 아카이브
│   ├─ sql_parser_demo/   W6
│   └─ bp_tree_demo/      W7
├─ .github/workflows/     CI (build / test / tsan / valgrind)
├─ CLAUDE.md              레포 전역 규칙
├─ agent.md               W8 라운드 설계 결정 + 분할
├─ TEAM_RULES.md          브랜치/PR/CI 규약
├─ claude_jiyong.md       PM 개인 컨텍스트
├─ Makefile
└─ README.md              (이 파일)
```

---

## 팀

| 이름 | 담당 | 브랜치 |
|---|---|---|
| 지용 (PM) | `include/*.h` 인터페이스 + `engine_lock` + mix-merge + ANSI REPL | `feature/pm-infra` |
| 동현 | `engine.c` thread-safe wrapping + EXPLAIN + `mode=single` | `feature/engine-threadsafe` |
| 용 형님 | `server.c` + `protocol.c` HTTP + `router.c` + 탭 (a) Stress 데모 | `feature/server-protocol` |
| 승진 | `threadpool.c` + `/api/stats` + 탭 (c) RW Contention 데모 | `feature/threadpool-stats` |

---

## 이전 라운드

- W6 SQL Parser: <https://github.com/JYPark-Code/jungle_w6_mini_mysql_sql_parser>
- W7 B+Tree Index DB: <https://github.com/JYPark-Code/jungle_w7_BplusTree_Index_DB>
- W7 최종본 README: [`docs/README_w7.md`](docs/README_w7.md)
