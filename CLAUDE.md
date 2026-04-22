# CLAUDE.md — 레포 전역 규칙

이 파일은 이 레포에서 Claude Code 세션이 따라야 할 규칙을 정의합니다.
모든 팀원의 작업은 이 규칙을 기준으로 합니다.

---

## 프로젝트 컨텍스트

- **이름**: minisqld — Multi-threaded Mini DBMS in Pure C
- **목표**: W7 B+Tree Index DB 엔진 위에 HTTP API 서버를 얹어
  외부 클라이언트가 SQL 을 실행할 수 있는 단일 C 데몬 완성
- **기간**: 수요코딩회 하루
- **언어**: C (C11, GCC, `-Wall -Wextra -Wpedantic`)
- **외부 의존**: pthread 만 허용. HTTP/JSON 라이브러리 금지

---

## 빌드

```bash
make              # 데몬 빌드 (./minisqld)
make test         # 회귀 + 동시성 테스트
make tsan         # ThreadSanitizer 빌드
make valgrind     # 메모리 누수 검사
make bench        # B+ 트리 pure 벤치 (W7 자산)
make clean
```

**커밋 전 반드시 확인**:
1. `make` 빌드 경고 0
2. `make test` 통과 (W7 회귀 227 개 + 본인 파트 단위 테스트)
3. `make tsan` 빌드 성공 (동시성 건드린 PR 만)

---

## 작업 흐름

### 1. 레포 clone 직후

```bash
git clone https://github.com/JYPark-Code/jungle_w8_minisqld.git
cd jungle_w8_minisqld
git fetch origin
git checkout <본인 브랜치>    # PM 이 미리 만들어둠
```

### 2. 본인 브랜치 확인 후 먼저 읽을 파일

1. `README.md` — 프로젝트 개요
2. `agent.md` — **이번 라운드 설계 결정 전체.** 반드시 읽을 것
3. `include/*.h` — PM 이 확정한 인터페이스. 이 시그니처를 **절대 임의로 바꾸지 말 것**
4. 본인 담당 `.c` 파일 (없으면 새로 생성)

### 3. 작업 시작

- 인터페이스(`.h`)를 바꿔야겠다고 판단되면 → 코드 수정 전에 PM(지용) 에게 먼저 Slack 으로 확인
- 본인 담당 외 파일은 수정 금지. 필요하면 PR 코멘트로 요청
- W7 엔진 코드(`src/parser.c`, `executor.c`, `storage.c`, `bptree.c`) 수정은
  **동현(engine 담당) 만 가능**. 다른 사람은 건드리지 말 것

### 4. 커밋 전

- `make` 빌드 경고 0 확인
- `make test` 통과 확인
- Angular commit convention + **한국어 body**

---

## 커밋 메시지 규약

**Angular convention, 한국어 body.**

<type>: <한국어 요약 50자 이내>

본문은 한국어
무엇을 바꿨는지, 왜 바꿨는지 bullet 로 작성
Claude 가 작성한 경우 끝에 Co-Authored-By 추가

**type 종류:**
- `feat`: 새 기능
- `fix`: 버그 수정
- `refactor`: 동작 변경 없는 내부 구조 개선
- `test`: 테스트 추가/수정
- `docs`: 문서 수정
- `chore`: 빌드/설정/의존성 등
- `perf`: 성능 개선

**예시:**
feat: threadpool 에 blocking job queue 구현

mutex + condvar 기반 submit/worker 구조 추가
shutdown 시 drain 후 종료 처리
단위 테스트 test_threadpool.c 추가 (동시 enqueue 10K 검증)

Co-Authored-By: Claude noreply@anthropic.com

---

## PR 규약

- **target 브랜치**: `main` 으로 바로 (dev 브랜치 사용 X, PM 이 mix-merge 로 통합)
- **크기**: PR 하나당 하나의 논리적 작업. 여러 기능 섞지 말 것
- **리뷰어**: PM(지용) 필수
- **체크리스트**:
  - [ ] `make` 빌드 경고 0
  - [ ] `make test` 통과
  - [ ] 본인 파트 단위 테스트 추가
  - [ ] 동시성 코드면 `make tsan` 통과
  - [ ] `include/*.h` 인터페이스 계약 준수

---

## 절대 금지

- `include/*.h` 인터페이스를 PM 승인 없이 변경
- 외부 HTTP/JSON 라이브러리 추가 (pthread 외 전부 금지)
- W7 엔진 코드(`src/parser.c`, `executor.c`, `storage.c`, `bptree.c`)를 engine 담당 외 수정
- `prev_project/`, `agent/` 하위 수정 (읽기 전용 아카이브)
- `tests/` 의 W7 회귀 테스트 삭제/수정 (기존 회귀 보장)
- force push (`--force`) 금지, 필요 시 `--force-with-lease`

---

## Claude Code 사용 시 주의

- **긴 코드 생성 후 반드시 본인이 읽고 이해**. 바이브 코딩이어도
  "왜 이 코드가 동작하는지" 설명 가능해야 함 (과제 요건)
- 인터페이스 시그니처는 `include/*.h` 의 것을 **복사 붙여넣기**. 임의 수정 X
- 동시성 관련 코드는 생성 후 반드시 `make tsan` 으로 검증
- 빌드 실패하는 코드를 커밋하지 말 것. Claude 도 `make` 돌려서 확인 후 커밋

---

## 2차 리팩토링 결정사항

Round 1 (1차 mix-merge 머지 완료) 위에 얹는 4 개 항목. 각 항목은
`(a) 왜 필요한가 → (b) 설계 방향 → (c) 예상 영향 범위` 로 정리.

### (1) 동적 쓰레드 풀

- **(a) 왜**: Round 1 의 고정 N (기본 8) 은 부하가 튀는 순간 queue 만 늘고
  latency 가 급등. 반대로 idle 시 N 만큼의 메모리/스레드 고정 점유.
  peak 대응 + 평균 자원 절약을 위해 자가 조정 필요
- **(b) 설계**:
  - `core_n_workers = 4` 로 시작, 필요 시 확장
  - 트리거: `active_workers / n_workers >= 0.8` 이 **일정 시간 지속** 될 때
    (모니터 스레드가 1 초 간격 샘플, 3 회 연속 고점 → 확장)
  - 증가 단위: 현재 풀 크기 **+4**
  - 상한: **16** (cap 초과 시 no-op)
  - 감소: 본 라운드는 **scale-down 구현 없음** (향후 과제)
  - graceful shutdown: accept 중단 → drain (timeout_ms 까지 대기) →
    worker join → 초과 시 강제 종료
- **(c) 영향 범위**:
  - 필수: `src/threadpool.c`, `include/threadpool.h` (API 추가),
    `src/server.c` (shutdown 경로 연계), `tests/test_threadpool.c` 보강
  - 연쇄: `/api/stats` 응답에 `active_workers`/`n_workers_cap`/
    `utilization` 필드 추가 (FE 가 폴링)

### (2) LRU 캐시 + Reader-Writer Lock

- **(a) 왜**: 같은 query (특히 autocomplete 의 짧은 prefix) 가 반복되면
  매번 파싱 + 트리 탐색 + JSON 직렬화 비용이 중복. hot 한 key 에 대한
  결과를 캐싱해 read 경로 단축
- **(b) 설계**:
  - 자료구조: hashmap + doubly linked list (표준 LRU), capacity 고정
  - 동시성: 1 개 `pthread_rwlock_t` 로 전체 캐시 보호 —
    get 은 rdlock, put / invalidate 는 wrlock
  - 일관성 정책: **invalidate-on-write** 채택 (회의 확정)
    - INSERT 발생 시 해당 테이블의 affected key 삭제
    - 안전 쪽 기본값: INSERT 시 `cache_invalidate_all()` 도 허용 (table
      단위 추적 복잡도 방어)
  - key 체계: `normalize(sql) + "|" + mode(multi|single)` (whitespace /
    대소문자 normalize 후 hash)
  - 크기 초기값: 1024 entry (측정 후 재조정)
- **(c) 영향 범위**:
  - 필수: `src/cache.c` (신규), `include/cache.h` (신규),
    `src/engine.c` (exec_sql 진입 시 cache_get, INSERT 후 invalidate),
    `tests/test_cache.c`
  - 연쇄: `/api/stats` 응답에 `cache_hits` / `cache_misses` 카운터 추가

### (3) Trie 기반 prefix search

- **(a) 왜**: B+Tree 는 **prefix 쿼리가 구조적으로 불가** (정확 매칭 /
  range 만). 어학사전 autocomplete 유스케이스에서 단어 prefix 로
  매칭되는 row 들을 빠르게 얻으려면 별도 인덱스 필요
- **(b) 설계**:
  - 자료구조: compressed 아닌 **표준 trie** (노드당 26 ~ UTF-8 지원
    고려). 처음엔 ASCII 소문자만으로 시작, 확장 가능성 열어둠
  - 노드에 `row_id` 저장 (단어 끝 노드만 non-null) + optional 자식
    포인터
  - 삽입: `O(k)` (k = word 길이)
  - 정확 매칭: `O(k)` — 없으면 -1
  - prefix 매칭: `O(k + m)` (m = prefix 로 시작하는 단어 수,
    `max_out` 으로 상한)
  - 동시성: read 다수 / write 소수. 1 차 구현은 engine_lock 테이블
    wrlock 하에서 일괄 보호 (세밀한 trie 내부 lock 은 향후)
- **(c) 영향 범위**:
  - 필수: `src/trie.c` (신규), `include/trie.h` (신규),
    `src/engine.c` (prefix 쿼리 dispatcher), `tests/test_trie.c`
  - 연쇄: `router.c` 에 `/autocomplete` endpoint 추가

### (4) FE 디자인 시스템 교체 (애플/토스 스타일)

- **(a) 왜**: Round 1 의 dark theme stress 페이지는 **발표 임팩트가 약함**
  (기술 지표는 있지만 일반 관객 / 채용담당자 기준 UX 인상이 낮음).
  외부 공개 repo 의 첫인상 개선 + 데모 유스케이스 (어학사전) 에 맞춘
  일반 사용자 UI 필요
- **(b) 설계**:
  - light / minimal: 흰 배경, 회색조 divider, strong 1 accent color
    (토스 블루 / 애플 블루 계열 중 택 1)
  - 폰트: system-ui / Inter / Pretendard (한글 가독성)
  - 레이아웃: hero (검색창 최상단) + result list (단어 카드) + footer
    에 stats strip
  - autocomplete 박스: 검색창 debounce 150 ms, `/autocomplete?prefix=`
    로 5 개 suggestion
  - 운영자 insert 패널: 별도 "Admin" 탭 (토글)
- **(c) 영향 범위**:
  - 필수: `web/concurrency.html` 전면 재작성 (또는 신규 HTML 로 대체)
  - 연쇄: router.c 의 `/` default 경로가 새 파일을 서빙하도록
    `router_set_web_root` 설정값 재확인

---

## 아키텍처 다이어그램

Round 2 완료 이후 기준:

```
┌─────────────┐
│ Client (FE) │  Apple/Toss-inspired minimal UI
└──────┬──────┘
       │ HTTP/1.1 (TCP, Connection: close)
       ▼
┌──────────────────────────────────────┐
│ Thread Pool (dynamic, 4 → 16)        │
│  trigger: utilization >= 80%, +4     │
│  graceful shutdown: drain + timeout  │
└──────┬───────────────────────────────┘
       ▼
┌───────────────────────────────┐
│ Query Parser → Planner        │
└──────┬────────────────────────┘
       ▼
┌───────────────────────────────┐       ┌─────────────────────────┐
│ Cache Layer  (LRU + RWLock)   │◀──────│ invalidate on INSERT    │
└──────┬────────────────────────┘       └──────────┬──────────────┘
       │ miss                                      │
       ▼                                           │
┌───────────────────────────────┐                  │
│ Trie Index  +  B+Tree Storage │──────────────────┘
└───────────────────────────────┘
```

레이어별 동시성 전략 (Round 2 반영):

| Layer | File | 동시성 |
|---|---|---|
| Socket | `src/server.c` | `accept()` 메인 스레드, fd 를 pool 로 전달 |
| Thread pool | `src/threadpool.c` | mutex + condvar queue, **동적 resize** (R2) |
| HTTP | `src/protocol.c`, `src/router.c` | 워커당 stateless |
| Cache | `src/cache.c` (R2) | **단일 rwlock** 하에 LRU 연산 전부 직렬화 |
| Engine | `src/engine.c`, `src/engine_lock.c` | 테이블 RW + catalog + single-mode |
| Index | `src/bptree.c`, `src/trie.c` (R2) | engine 레이어 락 하에서 read |
| Storage | `src/storage.c` (W7) | engine 레이어가 write 직렬화 |

---

## 모듈 간 인터페이스 (TBD — 팀 합의 필요)

> Round 2 는 4 개 작업이 병렬 진행되므로, 각 모듈의 공개 API 를
> **구현 시작 전** 확정해야 mix-merge 충돌을 줄일 수 있음. 아래는 PM
> 제안 초안이며, **각 담당자의 실제 구현 시작 전 리뷰 + 서명 필요**.
> 코드 스타일은 Round 1 의 `include/*.h` 규약 (opaque struct typedef /
> 동사_명사 / 0 성공 -1 실패 / const 표기) 그대로.

### (a) Trie ↔ Query Executor

```c
/* include/trie.h — TBD — 팀 합의 필요 (담당: 용) */
#ifndef TRIE_H
#define TRIE_H

typedef struct trie trie_t;

trie_t *trie_create(void);
void    trie_destroy(trie_t *t);

/* word: 소문자 ASCII 가정, NUL-terminated. row_id >= 0 */
int     trie_insert(trie_t *t, const char *word, int row_id);

/* 정확 매칭: row_id 반환, 없으면 -1 */
int     trie_search_exact(const trie_t *t, const char *word);

/* prefix 로 시작하는 row_id 를 최대 max_out 개까지 out 에 채움.
 * return = 실제 채운 개수 */
int     trie_search_prefix(const trie_t *t, const char *prefix,
                           int *out_row_ids, int max_out);

#endif
```

### (b) Cache ↔ Query Executor

```c
/* include/cache.h — TBD — 팀 합의 필요 (담당: 동현) */
#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>
#include <stdbool.h>

typedef struct cache cache_t;

cache_t *cache_create(int capacity);
void     cache_destroy(cache_t *c);

/* rwlock 은 내부에서 처리. out_json 은 성공 시 heap (호출자 free). */
bool     cache_get(cache_t *c, const char *key,
                   char **out_json, size_t *out_len);

/* json 내용을 내부 복사. 기존 key 가 있으면 교체. */
int      cache_put(cache_t *c, const char *key,
                   const char *json, size_t len);

/* INSERT 등 write 경로에서 호출. */
void     cache_invalidate(cache_t *c, const char *key);
void     cache_invalidate_all(cache_t *c);

/* 모니터링 (락 없이 atomic 읽기) */
unsigned long cache_hits(const cache_t *c);
unsigned long cache_misses(const cache_t *c);

#endif
```

### (c) Thread Pool ↔ Server

기존 `include/threadpool.h` 에 아래 API **추가**. 기존 시그니처 변경 X.

```c
/* include/threadpool.h 에 추가 — TBD — 팀 합의 필요 (담당: 지용) */

/* 0.0 ~ 1.0. race-free atomic 스냅샷. */
double threadpool_get_utilization(const threadpool_t *tp);

/* timeout_ms 안에 drain + join 완료 시 0, 초과 시 -1 (강제 종료 경로). */
int    threadpool_shutdown_graceful(threadpool_t *tp, int timeout_ms);

/* 동적 resize. new_n 이 현재보다 크면 확장, 작으면 축소 (R2 범위 외 → -1). */
int    threadpool_resize(threadpool_t *tp, int new_n_workers);
```

### (d) FE ↔ Server (HTTP API)

Round 2 신규 엔드포인트. 모든 응답은 `Content-Type: application/json`.

```
GET  /search?q=<word>
     → { "ok": true, "rows": [...], "elapsed_ms": N, "cache_hit": bool }
     → 404  { "ok": false, "error": "not_found" }

GET  /autocomplete?prefix=<p>&limit=<N>
     → { "ok": true, "suggestions": ["word1", "word2", ...],
         "elapsed_ms": N }

POST /admin/insert
     Content-Type: application/json
     Body: { "word": "hello", "meaning": "인사말" }
     → { "ok": true, "row_id": N, "elapsed_ms": N }
     → 400  { "ok": false, "error": "invalid_body" }
```

- 모든 엔드포인트는 `engine_lock` 경유로 동시성 보호
- `/search`, `/autocomplete` 는 캐시 check 먼저 → miss 시 엔진 호출
- `/admin/insert` 는 테이블 wrlock + 성공 시 `cache_invalidate*` 호출

---

## 알려진 이슈 / 향후 과제

- **스레드 풀 축소 로직 미구현.** Round 2 범위는 `+4` 확장까지.
  scale-down (유휴 워커 해제, hysteresis 등) 은 구현 난이도 평가 후
  결정. 확정되면 이 섹션 갱신
- **캐시 일관성 정책**: **invalidate-on-write 채택**
  (회의 결정, README § Known Issues 와 동기화). write-through 는
  금회 범위 외 — 매 INSERT 가 캐시에 새 값을 prefill 하는 방식이라
  락 구간이 길어지고 read 경로 단축 효과도 감소하므로 이번엔 기각
- **Trie 내부 세분화 락 미구현.** 1 차 trie 구현은 engine_lock 의
  테이블 wrlock 에 의존. read 가 많고 write 가 드물기 때문에 일괄 락이
  성능적으로 충분하다는 전제. 병목 확인 시 read-copy-update 또는
  per-node mutex 도입 검토
- **`/api/inject` (Round 1 placeholder) 는 Round 2 에서 완성.**
  router 에서 501 반환 → Round 2 engine 경로로 연결 필요
- **HTTP Keep-alive 미지원.** 요청당 TCP 연결 새로 맺음. 브라우저의
  per-origin 동시 연결 제한 (6) 에 걸려 브라우저 기반 스트레스는
  서버 멀티 이득을 희석시킴. 부하 측정은 `xargs -P` / REPL 경유 권장
- **CI toolchain 설치 오버헤드.** `apt-get update` 가 build / test /
  tsan / valgrind 4 잡에서 반복 (각 1~3 분). `valgrind` 외 잡에서
  제거 예정 (`chore/ci-speedup`)
