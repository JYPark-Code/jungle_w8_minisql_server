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

### (2) Router-level dict cache + Reader-Writer Lock

> **2026-04-22 설계 변경**: 초안의 "엔진 내부 query cache" 를 **router
> 레벨의 `/api/dict` 전용 cache** 로 pivot. 사전 서비스 범위에 맞춰
> SQL normalize / lifetime / race window 복잡도를 제거하고 엔진 핵심
> 경로를 무오염으로 유지.

- **(a) 왜**: 영한 사전 서비스에서 "apple" 같은 hot 한 단어 조회가 반복
  될 때 매번 파싱 + storage 탐색 + JSON 직렬화 비용이 중복. 캐시 범위가
  `/api/dict` 라는 1 개 엔드포인트로 **명확히 제한** 되므로, 범용 query
  cache 보다 훨씬 단순하게 구현 가능
- **(b) 설계**:
  - 위치: `src/cache.c` (신규) 의 generic LRU + `src/router.c` 의
    `/api/dict` 핸들러 안에서만 호출
  - 자료구조: hashmap + doubly linked list (표준 LRU), capacity 고정
  - 동시성: 1 개 `pthread_rwlock_t` 로 전체 캐시 보호 —
    get 은 rdlock, put / invalidate 는 wrlock
  - 일관성 정책: **invalidate-on-write** 채택 (회의 확정)
    - `/api/admin/insert` 성공 시 해당 english key invalidate
    - 보수적 fallback: `cache_invalidate_all()` 도 허용
  - key 체계: 영한 방향은 `english` 단어 원본 (normalize 불필요), 역방향
    은 `"ko:" + korean_word` 프리픽스로 분리 (캐시 적용 선택)
  - 크기 초기값: 1024 entry (측정 후 재조정)
  - 초기화: `pthread_once` lazy init 또는 `router.c` 파일 scope static.
    server.c 경유 금지 (TEAM_RULES §10-3 참조)
- **(c) 영향 범위**:
  - 필수: `src/cache.c` (신규), `include/cache.h` (신규),
    `src/router.c` (Zone R-DICT-CACHE / R-STATS / R-INIT),
    `tests/test_cache.c`
  - **엔진 핵심 경로 (`src/engine.c`) 는 건드리지 않음** — 초안에서
    변경됨
  - 연쇄: `/api/stats` 응답에 `cache_hits` / `cache_misses` 카운터 추가

### (3) Trie 기반 prefix search (ASCII 영어 only)

- **(a) 왜**: B+Tree 는 **prefix 쿼리가 구조적으로 불가** (정확 매칭 /
  range 만). 영한 사전 autocomplete 유스케이스에서 영어 단어 prefix 로
  매칭되는 row 들을 빠르게 얻으려면 별도 인덱스 필요
- **(b) 설계**:
  - 자료구조: **표준 trie**, 노드당 자식 **26 개 고정 배열** (ASCII
    소문자 a-z). compressed trie 는 Round 2 범위 외
  - 인덱스 대상: `dictionary.english` 컬럼 만. 한글 body 는 인덱스 대상
    아님
  - 노드에 `row_id` 저장 (단어 끝 노드만 non-null) + 26 개 자식 포인터
  - 삽입: `O(k)` (k = word 길이)
  - 정확 매칭: `O(k)` — 없으면 -1
  - prefix 매칭: `O(k + m)` (m = prefix 로 시작하는 단어 수,
    `max_out` 으로 상한)
  - 동시성: read 다수 / write 소수. 1 차 구현은 engine_lock 테이블
    wrlock 하에서 일괄 보호 (세밀한 trie 내부 lock 은 향후)
  - **입력 검증**: non-ASCII / 대문자 / 숫자 / 기호 prefix 는 `/api/autocomplete`
    핸들러에서 400 반환. trie 진입 전 차단
- **(c) 영향 범위**:
  - 필수: `src/trie.c` (신규), `include/trie.h` (신규),
    `src/engine.c` (Zone T1 prefix dispatcher + Zone IT init/teardown),
    `tests/test_trie.c`
  - 연쇄: `router.c` 에 `/api/autocomplete` endpoint 추가 (Zone R-AUTO)

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
┌──────────────────────────────────────────────┐
│ Router                                       │
│                                              │
│  /api/dict  ──► Dict Cache (LRU + RWLock) ┐  │
│                   │ hit  → return JSON    │  │
│                   │ miss ──┐              │  │
│                            ▼              │  │
│  /api/autocomplete ──► engine → Trie      │  │
│  /api/admin/insert ──► engine → Storage   │  │
│                            (on INSERT)    │  │
│                            cache_invalidate◀─┘  │
└──────┬───────────────────────────────────────┘
       ▼
┌───────────────────────────────┐
│ Engine Layer (engine_lock 경유) │
│  Query Parser → Planner       │
└──────┬────────────────────────┘
       ▼
┌───────────────────────────────┐
│ Trie (ASCII English only)     │
│  + B+Tree Storage             │
└───────────────────────────────┘
```

**핵심 차이 (2026-04-22 설계 변경)**: Cache 는 **엔진 내부가 아니라
router 레벨** 에 위치한다. `/api/dict` 엔드포인트만 캐시의 소비자이고,
engine 핵심 경로 (parse → execute → storage) 는 캐시를 모른다.

레이어별 동시성 전략 (Round 2 반영):

| Layer | File | 동시성 |
|---|---|---|
| Socket | `src/server.c` | `accept()` 메인 스레드, fd 를 pool 로 전달 |
| Thread pool | `src/threadpool.c` | mutex + condvar queue, **동적 resize** (R2) |
| HTTP | `src/protocol.c` | 워커당 stateless |
| **Router + Dict Cache** | `src/router.c`, `src/cache.c` (R2) | LRU 는 **단일 rwlock**, router 자체는 worker 당 stateless |
| Engine | `src/engine.c`, `src/engine_lock.c` | 테이블 RW + catalog + single-mode |
| Index | `src/bptree.c`, `src/trie.c` (R2, ASCII only) | engine 레이어 락 하에서 read |
| Storage | `src/storage.c` (W7) | engine 레이어가 write 직렬화. 한글 body 역방향 조회는 여기서 선형 스캔 |

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

### (b) Cache ↔ Router (`/api/dict` handler)

> 초안에서는 consumer 가 Query Executor (engine.c) 였으나, 2026-04-22
> 설계 변경으로 **Router** 가 유일한 소비자. 시그니처 자체는 초안과 동일.

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

/* /api/admin/insert 성공 경로에서 호출. */
void     cache_invalidate(cache_t *c, const char *key);
void     cache_invalidate_all(cache_t *c);

/* 모니터링 (락 없이 atomic 읽기) */
unsigned long cache_hits(const cache_t *c);
unsigned long cache_misses(const cache_t *c);

#endif
```

호출 위치는 `src/router.c` 의 `/api/dict` 핸들러 안에만. engine.c / server.c
에서 cache_* 를 호출하는 것은 **금지** (TEAM_RULES §10-3 설계 경계 참조).

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
사전 방향은 **영한** (영어 key → 한글 해석) 이 primary. 역방향 (한글 body
→ 영어) 은 선형 스캔으로 동작은 하되 느림 ("option B").

```
GET  /api/dict?word=<english>
     → 영한 조회 (primary). dict cache 체크 후 miss 면 engine 경유.
     → { "ok": true, "rows": [...], "elapsed_ms": N, "cache_hit": bool }
     → 404  { "ok": false, "error": "not_found" }

GET  /api/dict?korean=<ko>
     → 역방향 (한글 body → 영어). 인덱스 없음, 선형 스캔. 느릴 수 있음.
     → { "ok": true, "rows": [...], "elapsed_ms": N }

GET  /api/autocomplete?prefix=<p>&limit=<N>
     → ASCII 소문자 영어 prefix 만. 한글/대문자/기호는 400.
     → { "ok": true, "suggestions": ["word1", "word2", ...],
         "elapsed_ms": N }

POST /api/admin/insert
     Content-Type: application/json
     Body: { "english": "apple", "korean": "사과" }
     → dictionary 테이블에 INSERT + trie 업데이트 + cache invalidate.
     → { "ok": true, "row_id": N, "elapsed_ms": N }
     → 400  { "ok": false, "error": "invalid_body" }
```

- 모든 엔드포인트는 `engine_lock` 경유로 동시성 보호
- `/api/dict` 의 영한 방향만 **dict cache** 를 경유 (miss 시 engine 호출).
  역방향은 캐시 적용 여부 구현자 선택
- `/api/autocomplete` 는 trie 직접 조회 — 캐시 미적용 (prefix 조합이 많아
  hit rate 낮음)
- `/api/admin/insert` 는 테이블 wrlock + 성공 시 `cache_invalidate(english)`
  호출
- `/api/inject` (Round 1 placeholder) 는 Round 2 에서 완성 — 더미 주입
  용도, `/api/admin/insert` 와 유사 경로

---

## 알려진 이슈 / 향후 과제

- **스레드 풀 축소 로직 미구현.** Round 2 범위는 `+4` 확장까지.
  scale-down (유휴 워커 해제, hysteresis 등) 은 구현 난이도 평가 후
  결정. 확정되면 이 섹션 갱신
- **캐시 범위 = router 의 `/api/dict` 엔드포인트만.** 2026-04-22 설계
  변경으로 엔진 내부 query cache → router-level dict cache 로 pivot.
  - 근거: 사전 서비스는 엔드포인트 단위 캐싱으로 충분하고, engine 내부
    캐시는 SQL normalize / lifetime / race window 오버헤드가 큼
  - 영향: engine.c 동현 수정 zone 소멸, 동현-용 engine.c overlap 없어짐
- **캐시 일관성 정책**: **invalidate-on-write 채택**
  (회의 결정, README § Known Issues 와 동기화). write-through 는
  금회 범위 외 — 매 INSERT 가 캐시에 새 값을 prefill 하는 방식이라
  락 구간이 길어지고 read 경로 단축 효과도 감소하므로 이번엔 기각
- **영한 단방향 primary, 한글 body 역방향은 선형 스캔.** Trie 는 영어
  ASCII 컬럼 (`dictionary.english`) 에만 인덱스. 한글 body 컬럼
  (`dictionary.korean`) 은 인덱스 없음 → `?korean=사과` 조회는 기존
  executor 의 선형 스캔 fallback 으로 동작. 동작 OK / 느림 (10 만 행
  기준 수십~수백 ms 예상). 3 차 과제로 역방향 인덱스 추가 검토
- **Unicode NFC 정규화 / 초성 검색 범위 외.** Trie 가 영어 ASCII only
  라 한글 정규화 이슈 자체가 발생하지 않음. FE 는 영어 input 만 받도록
  필터링. 한글 입력 시 `/api/dict?korean=` 쿼리 파라미터로 넘기는 경로는
  허용하되 trie 를 안 탐
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
