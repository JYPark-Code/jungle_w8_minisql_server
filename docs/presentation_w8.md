# W8 minisqld — 발표자 참고 문서

> **수요코딩회 1일 해커톤 (2026-04-22)** 기록.
> W7 의 B+Tree 인덱스 엔진을 단일 C 데몬으로 감싸, 외부 클라이언트가
> HTTP 로 SQL 을 실행할 수 있는 멀티스레드 미니 DBMS 완성.
> 발표 시나리오: **영한 사전 (English → Korean)**.

---

## 1. 한 줄 요약

> "100 명이 영어 단어를 검색 + 운영자가 새 단어를 등록 + 자동완성이
> 동시에 돌아간다. 단일 C 데몬, **4 → 16 동적 워커 풀** + **rwlock+atomic LRU
> dict cache** + **ASCII Trie prefix 인덱스** 가 read 폭주를 흡수하면서도
> write 를 막지 않는다."

- 언어: C11, `-Wall -Wextra -Wpedantic`
- 외부 의존: `pthread` 만 (HTTP/JSON 라이브러리 0 개, 직접 작성)
- 산출물: `./minisqld` 단일 바이너리 + `./minisqld-repl` (백업 데모용)

---

## 2. 팀 구성 & 담당 모듈

| 팀원 | 담당 | 핵심 산출물 |
|---|---|---|
| **지용 (PM)** | 인프라·무결성·통합 | 저장소 셋업, 공개 헤더 5종, Makefile, CI 4잡, `engine_lock` 동시성 층, REPL 클라이언트, Round 2 threadpool 동적 확장 + graceful shutdown, 안정화 fix |
| **김용 (용 형님)** | Server / Protocol / Router + Trie | PR #2 HTTP/1.1 서버 + accept loop + 라우터, PR #16 Trie prefix engine path, PR #19 `/api/autocomplete` · `/api/admin/insert` router 엔드포인트 |
| **남동현 (동현)** | Engine (SQL 실행 어댑터) + Dict Cache | PR #4 engine 어댑터 (W7 parser/executor 를 `engine_exec_sql` 계약으로 감쌈) + EXPLAIN/BETWEEN, PR #18 router-level dict cache (LRU) |
| **백승진 (승진)** | Thread Pool + Concurrency FE | PR #5 고정 워커 풀 (mutex+condvar job queue), PR #21 `concurrency.html` 영한 사전 중심 재설계 |

---

## 3. 마일스톤 타임라인

```
MP0 ─── MP1 ─── MP2 ─── MP3 ─── MP4
(인프라)  (Round1)  (mix-merge)  (Round2)  (안정화)
   지용    4인 병렬   지용         4인 병렬   지용+4인
```

### MP0 — 레포 인프라 셋업 (지용, ~2h)

Round 1 이 병렬로 시작되기 전에 **인터페이스 계약 확정 + 스텁 + CI** 를
먼저 올려서 4 명이 충돌 없이 동시에 코딩 가능한 환경을 만드는 단계.

| 커밋 | 내용 |
|---|---|
| `137d04a` | W6/W7 라운드 자산을 `prev_project/` 로 아카이빙 |
| `eec9bd4` | 협업 문서 3종 초안 (`CLAUDE.md`, `TEAM_RULES.md`, `agent.md`) |
| `16362fe` | 브랜치 전략·보호 규약·CI 잡 정의 |
| `5bd8cbc` | 공개 헤더 5종 (`include/server.h`, `threadpool.h`, `engine.h`, `router.h`, `protocol.h`) — 팀원 전원이 이 시그니처를 **복사 붙여넣기** 하도록 계약 고정 |
| `6bb6d73` | `engine_lock` 동시성 층 (테이블 RW + catalog lock + single mode) |
| `68c386f` | `main.c` 를 W8 데몬 엔트리로 재작성 (CLI 파싱 + SIGINT + `server_run`) |
| `9694564` | Makefile: `./minisqld` 빌드 + `make tsan` + `make valgrind` |
| `efd5c8c` | CI 워크플로우 — 4잡 병렬 (build / test / tsan / valgrind) |
| `6072201` | 팀원 4 영역 stub `.c` 5종 추가 (MP0 링크 통과용) |

**효과**: 팀원들은 `git checkout <본인-브랜치>` 만으로 빌드·테스트 통과
환경에서 즉시 작업 시작 가능했음.

---

### MP1 — Round 1 4인 병렬 기반 모듈

4 명이 각자 자기 브랜치에서 PR 하나씩. **`include/*.h` 시그니처는
MP0 에서 확정** 되어 있어 인터페이스 합의 없이 독립 구현 가능.

| PR | 담당 | 내용 | 라인 수 |
|---|---|---|---|
| **#1** | 지용 | `engine_lock` 단위 테스트 8종 + ANSI REPL 클라이언트 + Makefile merge | +865 |
| **#2** | 용 | HTTP 서버 프로토콜 + 라우터 (`server.c` / `protocol.c` / `router.c`), `web/stress.html` 실동작 | +1,122 |
| **#4** | 동현 | W7 parser/executor 를 `engine_exec_sql` JSON 계약으로 감쌈, `engine_lock` 22 회 경유, `EXPLAIN` / `BETWEEN` 지원 | +1,090 |
| **#5** | 승진 | 고정 N 워커 풀 (mutex + condvar job queue), `test_threadpool.c`, `web/concurrency.html` 탭 구조 + 탭 c 폴링 UI | +2,038 |

**결과**: dev 브랜치에서 SQL 쿼리 서빙하는 HTTP 데몬 동작.

---

### MP2 — 1차 mix-merge + 인수인계 (지용)

4 PR 을 dev 에 통합 후, 실측·휴지 포인트 확인·Round 2 어젠다 문서화.

| 커밋 / PR | 내용 |
|---|---|
| `a14f4f7` (#6) | `w8_handoff.md` 작성 — 팀원별 체크리스트 + 회의 어젠다 |
| `3eb2886` (#7) | README 에 실측 수치 + 브라우저 per-origin 제약 (`6 연결`) 주의 사항 |
| `8e411b5` (#8) | `stress.html` → `concurrency.html` 탭 (a) 통합 (발표 진입점 단일화) |

**회의 결정 (2026-04-22)**:
- 캐시 위치 **engine 내부 → router level** 로 pivot (동현 제안 수용)
- 사전 방향 **영한** primary 확정, 역방향은 선형 스캔 fallback

---

### MP3 — Round 2 리팩토링 (4 항목 병렬)

#### (1) 동적 쓰레드 풀 — 지용 (PR #15)

- 시작 4 워커, 사용률 ≥ 80% 지속 시 **+4 확장**, 상한 **16**
- 모니터 스레드가 1 초 간격 atomic stats 만 읽고 판단
- `threadpool_shutdown_graceful(timeout_ms)` — accept 중단 → drain → join
- `threadpool_get_utilization()` atomic 스냅샷 API 추가
- `+679 / -95` (기존 API 하위호환)

#### (2) Router-level Dict Cache — 동현 (PR #18)

- `src/dict_cache.c` + `src/dict_cache.h` (private 헤더, `router.c` 만 include)
- LRU (hashmap + doubly linked list) capacity 1024
- 키 체계: `english:<word>` / `id:<N>` (prefix 분리)
- **invalidate-on-write** 정책 (POST `/api/admin/insert` 성공 시 해당 key invalidate)
- **cache miss 후 DB 조회는 cache lock 밖에서 수행** → 캐시가 오래 잠기지 않음
- 엔진 핵심 경로 `engine.c` 는 캐시를 **모른다** (설계 원칙)
- `+699`

#### (3) Trie 기반 prefix 검색 (ASCII a-z) — 용 (PR #16, #19)

- `src/trie.c` + `include/trie.h`, 노드당 자식 26 고정 (a-z)
- `engine.c` 에 Zone T1 / Zone IT 삽입:
  - `should_use_trie_prefix(sql, prefix_buf)` 판정 함수
  - `dictionary` 테이블의 `WHERE english LIKE 'xx%'` 만 trie 경로
  - 한글·대문자·기호 prefix 는 router 에서 400 차단, trie 진입 전
- INSERT/UPDATE/DELETE 성공 후 **전체 rebuild** (단순 정책, 단어 수 늘면 증분 전환 검토)
- `/api/autocomplete` · `/api/admin/insert` 엔드포인트 (PR #19, +673)

#### (4) FE 디자인 재설계 (애플/토스 톤) — 승진 (PR #21)

- `concurrency.html` 전면 재작성 (`+948 / -1327`)
- light / minimal, accent 1색, Pretendard / Inter 폰트
- 검색창 debounce 150 ms → `/api/autocomplete?prefix=` 5 개 suggestion
- 운영자 INSERT 패널 별도 "Admin" 탭 토글

---

### MP4 — Round 2 안정화 + 발표 준비 (지용)

Round 2 병렬 머지 직후 발견된 integration 이슈를 PM 이 일괄 처리.
**모든 PR 이 `/api/*` 엔드포인트가 실제 브라우저에서 동작하도록 하는 목적**.

| PR | 이슈 | 해결 |
|---|---|---|
| #20 | CI apt 설치 오버헤드 (4잡 × 1~3분 중복) | `build/test/tsan` 잡에서 apt 제거, `valgrind` 잡만 경량 install |
| #22 | 데몬 기동 직후 `/api/*` 가 engine 초기화 전에 500 반환 | `warming_up` 503 게이트 + `/api/dict?nocache=1` (디버그용) + dictionary fixture + shutdown drain |
| #23 | FE 자동완성이 mock 데이터였음 | `/api/autocomplete` 실엔드포인트 활성화 |
| #24 → #25 | 단일 mutex 로 오히려 read-heavy 부하에서 캐시가 느림 | `dict_cache` 단일 mutex → **rwlock + atomic LRU counter** 로 교체 |
| #26 | writer 대기 중 FE 가 오탐 timeout | `QUERY_TIMEOUT_MS` 8s → 60s |
| #27 | 빈 DB 기동 시 `/api/dict` 가 503 무한 대기 | dictionary 테이블 자동 생성 (chicken-and-egg) |
| #28 | dict 검색 결과가 FE 에 비어 보임 | `extractRows` 를 `engine.statements[0]` 경로로 수정 |

**데이터 fixture 작업 (동일 날짜, 발표용 데모 데이터)**:

| 커밋 | 내용 |
|---|---|
| `fc3c3b5` | kengdic 기반 dictionary ~10 만 행 추가 |
| `b668116` | 번역 표현 정제 (1차) |
| `0e7184e` | `data/` 디렉터리 `.gitignore` 해제 |
| `a8f6b4e` | `Stress Test` 탭에 `nocache=1` 강제 — 캐시 히트 편향 제거 |
| `9312f5a` | `extractRows` legacy `engine.columns/rows` fallback |
| `7a882a1` | cache hit 조회 시간 별도 표시 (FE) |

> 🔸 **후반 수정 작업 (2026-04-23 이후)** — 데이터 재생성·R/W 테스트
> 튜닝 등은 본 문서 범위 외. 필요 시 `git log --since=2026-04-23` 참조.

---

## 4. 팀원별 기여 상세

### 🧑‍💻 지용 (PM, `jypark / Ji Yong Park`)

- **MP0 인프라**: 헤더 계약 5종, Makefile, CI 4잡, `engine_lock`, `main.c` 엔트리, 스텁
- **Round 1**: `engine_lock` 단위 테스트 8 종 + ANSI REPL (`minisqld-repl`)
- **Round 2 Threadpool**: 동적 4→16 확장, graceful shutdown, `/api/stats` 필드 추가 (PR #15)
- **Round 2 안정화 7 PR**: warming_up 게이트, dict_cache rwlock 교체, timeout 조정, 빈 DB fix 등 (PR #20, #22, #23, #25, #26, #27, #28)
- **문서 체계**: `CLAUDE.md`, `TEAM_RULES.md`, `agent.md`, `w8_handoff.md`, `docs/round2_integration_map.md`, README 한글·영문 2종

### 🌐 용 (`김용 / kim8796`)

- **PR #2 HTTP 서버** (`src/server.c`, `protocol.c`, `router.c`): accept loop,
  HTTP/1.1 파서 (리퀘스트 라인 + 헤더 + 바디), 라우팅 디스패처,
  `serve_static` (`web/` 서빙), `web/stress.html` 초안
- **PR #16 Trie engine path**: `src/trie.c` + `include/trie.h`,
  `should_use_trie_prefix` 판정, `engine.c` Zone T1/IT 삽입,
  trie rebuild 정책
- **PR #19 Router endpoints**: `/api/autocomplete?prefix=`,
  `POST /api/admin/insert` (JSON 바디 파싱 + dictionary INSERT +
  trie 업데이트 + `dict_cache_invalidate`)

### 🔧 동현 (`남동현 / ehdgus5178`)

- **PR #4 Engine 어댑터**: W7 `parser.c` + `executor.c` 를 `engine_exec_sql(sql, json_out)` JSON 계약으로 감싸고 `engine_lock` 22 회 경유, `EXPLAIN` + `BETWEEN` 실장, `test_engine_concurrent.c`
- **PR #18 Dict Cache** (설계 pivot 수용 후 구현):
  - `src/dict_cache.c` / `dict_cache.h` (private)
  - LRU (hashmap + doubly linked list), capacity 1024
  - `dict_cache_get` / `put` / `invalidate` / `invalidate_all` / `hits` / `misses`
  - `router.c` `/api/dict` 핸들러 안에서만 호출 (engine.c 무오염)
  - `test_dict_cache.c`

### ⚙️ 승진 (`백승진 / vromotion`)

- **PR #5 Thread Pool**: `src/threadpool.c` (`threadpool_create` / `submit` / `shutdown`) — mutex + condvar job queue, `test_threadpool.c` (enqueue 10K 검증), `web/concurrency.html` 탭 구조 + 탭 (c) `/api/stats` 폴링 UI 초안
- **PR #21 FE 재설계**: `concurrency.html` 영한 사전 중심으로 전면 재작성 (+948 / -1327), 검색 UX, autocomplete 박스, 애플/토스 톤 스타일

---

## 5. Round 2 최종 아키텍처

```
[Browser / curl / REPL]
       │ HTTP/1.1 (Connection: close)
       ▼
┌──────────────────────────────────────────────┐
│ server.c        accept loop + graceful        │
│     │           shutdown (SIGINT)             │
│     │ enqueue(fd)                             │
│     ▼                                         │
│ threadpool.c    동적 N=4~16 (+4 @ ≥80%)       │
│     │                                         │
│     ▼                                         │
│ protocol.c      HTTP parse                    │
│ router.c        method + path → handler       │
│     │                                         │
│     │  /api/dict ──► dict_cache.c (LRU+rwlock)│
│     │                 │ hit  → JSON return    │
│     │                 │ miss ──┐              │
│     │                          ▼              │
│     │  /api/autocomplete ──► engine.c → Trie  │
│     │  /api/admin/insert ──► engine.c → Storage│
│     │                         (on INSERT)     │
│     │           dict_cache_invalidate ◀───────┘
│     ▼                                         │
│ engine.c  (engine_lock 경유)                   │
│  Parser → Planner → [Trie | B+Tree | Scan]    │
│                     │       │       │         │
│                     ▼       ▼       ▼         │
│                trie.c  bptree.c  storage.c    │
└───────────────────────────────────────────────┘
```

**핵심 설계**: cache 는 **엔진 내부가 아니라 router 레벨** 에 위치.
`/api/dict` 만 캐시의 소비자이고, 엔진 핵심 경로 (parse → execute →
storage) 는 캐시를 모른다. → invalidation 단순화, 디버깅 용이.

---

## 6. 발표 시나리오 (영한 사전)

```bash
# 데몬 기동
./minisqld --port 8080 --workers 4 --data-dir ./data --web-root ./web
```

### Step 1. 단일 단어 조회 — cache hit 동작

```bash
curl 'http://localhost:8080/api/dict?english=apple'
# 1회차: cache_hit:false (engine 경유)
curl 'http://localhost:8080/api/dict?english=apple'
# 2회차: cache_hit:true (dict_cache rdlock 만, engine 미경유)
```

### Step 2. SELECT 폭주 — 동적 풀 확장 + cache 효과

```bash
# (a) 100 동시, 같은 단어 — cache hit heavy
seq 100 | xargs -P 100 -I{} curl -s 'http://localhost:8080/api/dict?english=apple' > /dev/null

# (b) 100 동시, 같은 단어 + nocache=1 — 테이블 rdlock 경쟁
seq 100 | xargs -P 100 -I{} curl -s 'http://localhost:8080/api/dict?english=apple&nocache=1' > /dev/null

curl 'http://localhost:8080/api/stats'
# active_workers (확장 결과) / cache_hits / cache_misses / queue_depth
```

### Step 3. SELECT 폭주 + 운영자 INSERT 동시 — rwlock 효과

```bash
# 터미널 1: SELECT 폭주
while true; do curl -s 'http://localhost:8080/api/dict?english=apple' >/dev/null; done &

# 터미널 2: 운영자 INSERT (wrlock → SELECT 잠깐 대기 → cache invalidate)
curl -X POST http://localhost:8080/api/admin/insert \
     -H "Content-Type: application/json" \
     -d '{"english":"newword","korean":"신조어"}'
```

### Step 4. 자동완성 (Trie prefix)

```bash
curl 'http://localhost:8080/api/autocomplete?prefix=app&limit=5'
# → {"ok":true,"suggestions":["apple","apply","approach",...],"elapsed_ms":N}

curl 'http://localhost:8080/api/autocomplete?prefix=사'
# → 400 (한글 prefix 차단, trie 진입 전)
```

### Step 5. 백업 데모 — REPL

```bash
./minisqld-repl
> SELECT * FROM dictionary WHERE english='apple';
> \s                          # /api/stats
> \e SELECT ... WHERE id=1    # EXPLAIN (index_used, nodes_visited)
> \single on                  # 전역 직렬화 토글 (비교 시연)
```

---

## 7. 핵심 설계 결정 요약

| 결정 | 이유 |
|---|---|
| cache 를 **engine 내부 → router level** 로 pivot | SQL normalize / lifetime / race window 복잡도 제거, 엔진 핵심 경로 무오염 |
| cache miss 후 DB 조회를 **cache lock 밖에서** 수행 | 캐시가 오래 잠기지 않음 (동시 miss 시 중복 DB 조회는 허용 trade-off) |
| invalidate-on-write (write-through 기각) | write-through 는 락 구간 길어지고 read 경로 단축 효과 감소 |
| Trie **ASCII a-z only** | 한글 NFC 정규화 / 초성 검색 이슈 자체를 회피. 한글 body 역방향은 선형 스캔 fallback 으로 동작 |
| Thread pool **scale-down 미구현** | Round 2 범위는 `+4 확장` 까지. 유휴 워커 해제 / hysteresis 는 향후 과제 |
| Trie 내부 **세분화 락 미구현** | read 다수 / write 소수 — engine_lock 테이블 wrlock 에 일괄 의존으로 충분 |

---

## 8. 지표 & 증적

- 회귀 테스트: **W7 227 개 + W8 본인 파트 테스트 전부 통과** (`make test`)
- 빌드: `make` 경고 0, `make tsan` 통과 (동시성 건드린 PR 전부)
- PR 21 건 (Round 1: 4 / Round 2 리팩토링: 4 / Round 2 안정화: 7 / 문서·CI: 6)
- 데이터: `dictionary` 약 10 만 행 (kengdic 기반, fixture script 재생성 가능)

---

## 9. 알려진 한계 (발표 중 질문 대비)

- **HTTP Keep-alive 미지원** — 요청당 TCP 새로. 브라우저 per-origin 6
  연결 제한이 서버 멀티 이득을 희석 → 부하 측정은 `xargs -P` / REPL 권장
- **한글 역방향 조회 선형 스캔** — `/api/dict?korean=사과` 는 인덱스 없이
  10 만 행 스캔 (동작 OK / 느림, 수십~수백 ms)
- **Threadpool scale-down 없음** — peak 이후 워커가 상한 유지
- **Trie 전체 rebuild 정책** — INSERT 마다 전체 재구성. 단어 수 늘면 증분 삽입 전환 필요
