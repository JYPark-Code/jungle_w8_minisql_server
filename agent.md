# agent.md — W8 Round 설계 결정 + 4 분할 + 마일스톤

이 파일은 이번 라운드(W8) 의 **모든 설계 결정과 작업 분할**을 담습니다.
팀원은 본인 작업 시작 전 이 파일 전체를 읽어야 합니다.

---

## 1. 목표

W7 B+Tree Index DB 엔진을 **단일 C 데몬**으로 감싸서, 외부 클라이언트가
HTTP 로 SQL 을 실행할 수 있는 멀티스레드 미니 DBMS 를 완성한다.

**발표 핵심 메시지:**
> "W7 의 subprocess 모델 한계(요청당 1.8s rebuild 고정비) 를 데몬화로 제거.
> 결과: `make bench` 1,842× 수치에 SQL end-to-end 성능이 근접."

---

## 2. 설계 결정 (변경 불가)

### 2-1. 프로세스 모델
- **단일 C 데몬** (`./minisqld`). Python 중계 서버 없음
- 정적 파일 서빙도 데몬이 직접 수행 (`--web-root ./web`)

### 2-2. 프로토콜
- HTTP/1.1, 직접 작성 (외부 라이브러리 0)
- Request body 는 **raw SQL** (Content-Type: text/plain)
- Response 만 JSON
- Keep-alive 미지원 (connection = job 단위, close 로 끝)

### 2-3. 동시성
- **Thread pool**: 고정 크기 N workers, blocking queue (mutex + condvar)
- **락 전략**:
  - 테이블 단위 `pthread_rwlock_t` (SELECT = rdlock, INSERT/UPDATE/DELETE = wrlock)
  - 글로벌 catalog lock 1개 (CREATE/DROP TABLE)
  - `mode=single` 토글 시 전역 mutex 로 모든 요청 직렬화
- **Stats**: atomic 카운터

### 2-4. 데모 페이지 (`web/`)
- **탭 (a) Concurrent Stress**: 동시 요청 수 슬라이더, 싱글 vs 멀티 모드 비교
- **탭 (c) RW Contention**: SELECT 동시 100 vs INSERT 동시 100 병렬 시각화
- 이전 데모(W6/W7) 는 `prev_project/` 에 아카이브, Round 4 와 무관

### 2-5. `mode=single` 토글
- 데몬에 쿼리 파라미터 `?mode=single` 붙이면 전역 mutex 로 강제 직렬화
- 발표 데모용 "비교 baseline". 구현 ~10 줄

---

## 3. 아키텍처
[Client / Browser]
│ HTTP/1.1
▼
┌─────────────────────────────────────────┐
│  minisqld (단일 프로세스)                │
│                                          │
│  server.c        accept loop            │
│       │                                  │
│       │ enqueue(fd)                      │
│       ▼                                  │
│  threadpool.c    N workers              │
│       │                                  │
│       ▼                                  │
│  protocol.c      HTTP parse             │
│  router.c        method+path → handler  │
│       │                                  │
│       ▼                                  │
│  engine.c        RW lock per table      │
│       │          global catalog lock    │
│       ▼                                  │
│  [parser/executor/storage/bptree]       │
│  (W7 엔진 자산, in-process 재사용)      │
└─────────────────────────────────────────┘

---

## 4. 작업 분할

### 4-1. 지용 (PM) — 인터페이스 + concurrency core + mix-merge
**Owns**: `include/*.h` 전체, `src/engine_lock.c` (concurrency layer)

**Files**:
- `include/server.h`, `threadpool.h`, `engine.h`, `protocol.h`, `router.h`
- `src/engine_lock.c` — 테이블별 RW lock 등록/획득/해제 + catalog lock
- `src/main.c` 확장 (CLI 옵션 파싱, 데몬 부팅)
- `tests/test_engine_lock.c`
- (bonus) `client/repl.c` — ANSI REPL 클라이언트

**Responsibilities**:
- MP0 에서 인터페이스 확정 + 팀 공유
- MP4 이후 팀 PR 을 mix-merge
- 통합 빌드 책임

---

### 4-2. 동현 — engine.c thread-safe wrapping
**Owns**: `src/engine.c`, W7 엔진 코드 수정 권한

**Files**:
- `src/engine.c` — `engine_init`, `engine_exec_sql`, `engine_shutdown`,
  `engine_explain`, `engine_get_stats`
- `src/storage.c`, `src/index_registry.c` (thread-safety 수정, 최소 변경)
- `tests/test_engine_concurrent.c`

**Responsibilities**:
- W7 엔진의 글로벌 상태 (`s_meta`, `index_registry`, append FP cache) 를
  engine_lock 하에서 안전하게 호출하도록 래핑
- `mode=single` 토글 구현 (전역 mutex)
- EXPLAIN 명령 구현 (인덱스 사용 여부, 노드 visit 수, lock wait ms)

**주의**:
- W7 엔진 로직 자체는 바꾸지 말 것. thread-safety 가 필요한 최소한의
  수정만 하고, 가능하면 engine_lock 레이어에서 보호하는 방향
- 모든 수정 후 W7 회귀 테스트 227 개 통과해야 함

---

### 4-3. 용 형님 — network + HTTP + router + 정적 서빙 + 탭 (a)
**Owns**: `src/server.c`, `src/protocol.c`, `src/router.c`, `web/concurrency.html` 탭 (a)

**Files**:
- `src/server.c` — socket 생성, bind, listen, accept loop, SIGINT graceful shutdown
- `src/protocol.c` — HTTP request line + header + body 파싱, response 직렬화
- `src/router.c` — method+path → handler 디스패치, 정적 파일 서빙 (`/`)
- `tests/test_protocol.c` — HTTP 파서 단위 테스트
- `web/concurrency.html` 의 탭 (a) Concurrent Stress 부분
  - 슬라이더 (동시 요청 1~64), 버튼, 막대 그래프
  - fetch() 로 `/api/query` 에 병렬 N 개 발사
  - 싱글 모드 (`?mode=single`) vs 멀티 모드 비교

**Responsibilities**:
- HTTP 파서는 GET/POST + Content-Length + body 만 처리 (200 줄 내외)
- 정적 파일 서빙은 Content-Type 매핑 (`.html`, `.js`, `.css`, `.svg`) 만
- 탭 (a) 프론트는 바이브 코딩으로 Chart.js 또는 순수 HTML 막대 그래프

---

### 4-4. 승진 — threadpool + stats + 탭 (c)
**Owns**: `src/threadpool.c`, `src/stats.c`, `web/concurrency.html` 탭 (c)

**Files**:
- `src/threadpool.c` — job queue (mutex + condvar), N worker threads,
  `threadpool_submit()`, `threadpool_shutdown()`
- `src/stats.c` — atomic 카운터 (`total_queries`, `active_workers`,
  `lock_wait_ns`, `qps_last_sec`)
- `tests/test_threadpool.c` — 동시 enqueue 10K, shutdown 정상 종료 검증
- `web/concurrency.html` 의 탭 (c) RW Contention 부분
  - 좌측: SELECT 100 동시 발사 → 모두 병렬 처리 (빠름)
  - 우측: INSERT 100 동시 발사 → 직렬화 (느림)
  - 폴링: `/api/stats` 를 1초마다 fetch 해서 실시간 대시보드

**Responsibilities**:
- threadpool 은 shutdown 시 drain 후 join 필수 (누수 방지)
- stats 는 전부 `stdatomic.h` 의 `atomic_uint_fast64_t` 로
- 탭 (c) 프론트는 탭 (a) 와 같은 `concurrency.html` 안에 탭 UI 로 구성.
  용 형님과 **공통 JS 모듈** (fetch 헬퍼, 차트 헬퍼) 맞춰서 합치기

---

## 5. 의도된 Overlap (mix-merge 포인트)

mix-merge 전제로 **일부러 겹치게** 설계한 지점. 같은 레이어를 두 명이
다른 각도로 써본 걸 PM 이 mix 해서 품질 올림.

| 지점 | 겹치는 두 사람 | 겹치는 이유 |
|---|---|---|
| Job 구조체 | 승진 (threadpool) ↔ 용 형님 (server) | fd 를 어떤 struct 로 넘길지. 둘 다 정의해보고 좋은 쪽 채택 |
| Response write 책임 | 용 형님 (protocol) ↔ 승진 (stats JSON 직렬화) | JSON 생성 유틸을 누가 가질지 |
| concurrency.html | 용 형님 (탭 a) ↔ 승진 (탭 c) | 공통 JS (fetch, chart) 는 하나로 머지 |

**PM 은 각자 PR 에서 좋은 쪽을 선택**해서 mix. 본인 원본이 안 채택돼도
서운해하지 말 것. 이게 이 라운드의 운영 방식.

---

## 6. 마일스톤

| MP | 시각 | 내용 | 책임 |
|---|---|---|---|
| MP0 | 10:30~ | PM 선작업: include 헤더 5 개 + Makefile + 문서 4 개 커밋 | 지용 |
| MP1 | PM 끝난 후 | 팀원 각자 브랜치 체크아웃 + 스켈레톤 커밋 | 전원 |
| MP2 | +90분 | 각자 1차 구현 (mock 기반) | 전원 |
| 점심 | | | |
| MP3 | +90분 | 2차 구현 + 단위 테스트 | 전원 |
| MP4 | +60분 | 1차 PR 제출, PM mix-merge 시작 | 전원 + PM |
| MP5 | +60분 | 통합 빌드 + 데모 페이지 연결 + `make tsan` 회귀 | PM 주도 |
| MP6 | +30분 | README 갱신, 발표 리허설 | 전원 |

발표: **목요일 오전**, 4 분 데모 + 1 분 QnA.

---

## 7. 발표 시나리오 (4 분)
0:00 ─ 0:30   문제: W7 의 subprocess 한계, 1.8s rebuild 고정비
0:30 ─ 1:00   아키텍처 한 장: 단일 C 데몬, thread pool, RW lock
1:00 ─ 2:00   데모 (a) Concurrent Stress: 싱글 vs 멀티 비교 (수치 임팩트)
2:00 ─ 3:00   데모 (c) RW Contention: read 동시 vs write 직렬
3:00 ─ 3:30   /api/stats 대시보드: 운영 감각
3:30 ─ 4:00   마무리: make bench 1842× → SQL end-to-end 도 N×

---

## 8. 금지 사항

- `include/*.h` PM 승인 없이 수정
- 외부 HTTP/JSON 라이브러리 추가 (pthread 만 허용)
- W7 엔진 코드(`parser.c`, `executor.c`, `storage.c`, `bptree.c`) 수정 —
  동현만 권한 있음
- W7 회귀 테스트 227 개 깨지는 PR 머지
- `prev_project/`, `agent/` 수정 (아카이브 영역)