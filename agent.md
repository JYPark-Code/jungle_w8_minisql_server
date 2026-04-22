# agent.md — W8 Round 2: 설계 요약 + 팀원 작업 영역 + 에이전트 규칙

이 파일은 W8 **2차 리팩토링 라운드**의 설계 요약, 팀원별 작업 영역,
그리고 코드 수정을 수행하는 에이전트가 따라야 할 행동 규칙을 정의한다.

작업 시작 전에 본 문서 전체를 읽고, 본인 담당 외 모듈을 건드릴 때는
"4. 팀원 작업 영역" 의 담당자 컨벤션과 zone 매트릭스를 먼저 확인할 것.

> **Source of truth (이 문서는 요약본)**
> - `CLAUDE.md § 2차 리팩토링 결정사항 / 모듈 간 인터페이스` — 함수 시그니처, 자료구조 디테일
> - `TEAM_RULES.md § 10` — 브랜치별 작업 소유권, 교차 파일 수정 규약 매트릭스
> - `docs/round2_integration_map.md` — 동현(cache) / 용(trie) 의 router·engine zone 분할
> - `w8_handoff.md` — 1차 mix-merge 결과와 그때까지의 작업 분할 히스토리

---

## 1. 목표

W7 B+Tree Index DB 엔진을 단일 C 데몬으로 감싸 외부 클라이언트가
HTTP 로 SQL 을 실행할 수 있는 멀티스레드 미니 DBMS 를 완성한다.

**2차 발표 시나리오: 영한 사전 (English → Korean)**

> "다수 사용자가 영어 단어를 검색 (`/api/dict?english=apple`) 해서 한글
> 뜻을 받아가는 도중, 운영자가 새 단어를 등록 (`POST /api/admin/insert`).
> 동적 쓰레드 풀 + **router-level dict cache** (`src/dict_cache.c`) +
> RW lock 으로 read 폭주를 흡수하면서 write 도 막히지 않는다. 영어 prefix
> 자동완성은 Trie."

사전 방향: 영한 (English key → Korean meaning) 이 primary.
역방향 (한글 body → 영어) 은 인덱스 없이 선형 스캔, 동작은 OK / 느림.

---

## 2. 설계 결정 요약 (4 항목)

> 각 항목의 (왜 / 설계 / 영향 범위) 상세는 `CLAUDE.md § 2차 리팩토링
> 결정사항` 참조. 본 섹션은 에이전트가 코드 수정 시 떠올려야 할
> **요지** 만 적는다.

### 2-1. 동적 쓰레드 풀
- 시작 4 워커, 사용률 ≥ 80% 가 일정 시간 지속되면 **+4 확장**, 상한 **16**
- 모니터 쓰레드가 1 초 간격으로 atomic stats 만 읽고 판단
- scale-down 은 Round 2 범위 외 (향후 과제)
- graceful shutdown: accept 중단 → drain (timeout) → join

### 2-2. **Router-level dict cache** + Reader-Writer Lock ⚠ 2026-04-22 pivot
- 위치: `src/dict_cache.c` (LRU) + `src/dict_cache.h` (private 헤더, src/ 안)
  를 **`src/router.c` 의 `/api/dict` 핸들러 안에서만** 호출
- **`src/engine.c` 는 dict cache 를 모른다.** 엔진 핵심 경로 (parse →
  execute → storage) 무오염
- 캐시 키 체계 — prefix 분리:
  - `english:<word>` (영한 primary, 예: `english:apple`)
  - `id:<N>` (id 조회 옵션)
  - 역방향 `?korean=` 은 dict cache 미적용 (engine 직행)
- 일관성: invalidate-on-write (`/api/admin/insert` 성공 시 해당
  `english:<word>` key invalidate)
- 동시성: 단일 `pthread_rwlock_t` (또는 mutex) — get=rdlock, put/invalidate=wrlock
- **cache miss 후 DB 조회는 cache lock 밖에서 수행** → 전체 캐시가 오래
  잠기지 않음. 같은 단어 동시 miss 시 중복 DB 조회는 허용 trade-off

> 초안의 "엔진 내부 query cache" 는 SQL normalize / lifetime / race
> window 복잡도 때문에 기각. 사전 서비스 범위에선 엔드포인트 단위 캐싱이
> 충분하다는 동현 의견 수용 (회의: 2026-04-22).

### 2-3. Trie 기반 prefix search (ASCII 영어 only)
- 인덱스 대상: `dictionary.english` 컬럼만. 노드당 자식 26 고정 (a-z)
- `/api/autocomplete?prefix=ap` → trie 직접 조회 (캐시 미적용)
- 한글 / 대문자 / 숫자 / 기호 prefix 는 router 핸들러에서 400, trie 진입 전 차단
- 동시성: 1차는 engine_lock 의 테이블 wrlock 에 의존 (trie 내부 세분화 락 X)
- INSERT 는 B+Tree + Trie 두 인덱스 모두 갱신 (둘 다 성공해야 commit)

### 2-4. FE 디자인 — 애플 / 토스 톤 + 자동완성
- light / minimal, accent 1색, 폰트 Pretendard / Inter
- 검색창 debounce 150 ms → `/api/autocomplete?prefix=` 로 5 개 suggestion
- 운영자 INSERT 패널은 별도 "Admin" 탭

---

## 3. 아키텍처 (Round 2)

```
[Browser / curl / REPL]
        │ HTTP/1.1 (Connection: close)
        ▼
┌──────────────────────────────────────────────┐
│  minisqld (단일 프로세스)                      │
│                                                │
│  server.c        accept loop + graceful       │
│       │          shutdown (SIGINT)             │
│       │ enqueue(fd)                            │
│       ▼                                        │
│  threadpool.c    동적 N=4~16 (+4 @ ≥80%)      │
│       │                                        │
│       ▼                                        │
│  protocol.c      HTTP parse                   │
│  router.c        method+path → handler        │
│       │                                        │
│       │   /api/dict ──► cache.c (LRU+rwlock)  │
│       │                  │ hit  → JSON return │
│       │                  │ miss               │
│       ▼                  ▼                    │
│  engine.c (engine_lock 경유, **cache 모름**)  │
│       │                                        │
│       ▼                                        │
│  parser → executor → storage                  │
│                  ├── bptree   (equality)      │
│                  └── trie     (prefix, ASCII) │
└──────────────────────────────────────────────┘
```

저장소는 **flat `src/*.c` 구조** 다. 신설 모듈 (`src/dict_cache.c` +
`src/dict_cache.h`, `src/trie.c` + `include/trie.h`) 도 같은 컨벤션을
따른다 (서브디렉토리 만들지 말 것). dict_cache 헤더는 도메인 특화 모듈이라
`include/` 가 아닌 `src/` 안 private 헤더로 두어 router.c 만 include 한다.

---

## 4. 팀원 작업 영역

각 모듈에는 **소유자**가 있다. 본인 담당 외 모듈을 건드릴 때는:
1. 해당 담당자의 기존 파일을 먼저 읽고 컨벤션 (네이밍, 에러 처리,
   주석 스타일, 락 획득 패턴) 확인
2. 인터페이스 (`include/*.h`) 변경이 필요하면 수정 전에 PM 에게 확인
3. 가능하면 PR 코멘트로 요청, 직접 수정은 마지막 수단

> 정확한 zone 단위 매트릭스는 `TEAM_RULES.md § 10` 와
> `docs/round2_integration_map.md` 가 단일 출처. 아래는 요약이며,
> 충돌 시 위 두 문서가 우선.

### 4-1. 지용 (PM) — 동적 쓰레드 풀 + 서버
- **브랜치**: `feature/dynamic-threadpool`
- **Owns**: `src/threadpool.c`, `include/threadpool.h` (API 추가),
  `src/server.c` (graceful shutdown 연계), `src/main.c`,
  `tests/test_threadpool.c`, `client/repl.c`
- **참고할 컨벤션**: 1차 `src/threadpool.c`, `src/server.c`

### 4-2. 동현 — Router-level dict cache
- **브랜치**: `feature/lru-cache`
- **Owns**: `src/dict_cache.c` (신규), `src/dict_cache.h` (신규, private 헤더),
  `src/router.c` 의 Zone R-DICT-CACHE / R-STATS / R-INIT,
  `tests/test_dict_cache.c` (신규)
- **건드리지 않음**: `src/engine.c` (cache pivot 으로 zone 소멸),
  `src/server.c` (cache 라이프사이클은 router lazy init)
- **참고할 컨벤션**: `src/router.c` (1차), `src/engine_lock.c` (rwlock 패턴)

### 4-3. 용 — Trie + parser/executor prefix
- **브랜치**: `feature/trie-prefix`
- **Owns**: `src/trie.c` (신규), `include/trie.h` (신규),
  `src/engine.c` 의 Zone T1 (prefix dispatcher) + IT (init/teardown),
  `src/router.c` 의 Zone R-AUTO + R-ADMIN-INSERT,
  `tests/test_trie.c` (신규)
- **참고할 컨벤션**: `src/bptree.c` (인덱스 톤), `src/parser.c`

### 4-4. 승진 — FE + API 응답 스키마
- **Owns**: `web/concurrency.html` (재디자인), 자동완성 UI,
  `/api/dict` / `/api/autocomplete` / `/api/stats` / `/api/admin/insert`
  의 **응답 JSON 계약 결정권자**
- **건드리지 않음**: 백엔드 핸들러 본체 (스키마만 합의 후 router 담당과 머지)
- **참고할 컨벤션**: 1차 `web/concurrency.html` (탭 구조), `src/router.c`
  (응답 직렬화 패턴)

> W7 엔진 코드 (`parser.c`, `executor.c`, `storage.c`, `bptree.c`)
> 직접 수정 권한은 동현 (engine 담당). 용 의 trie 작업이 parser/executor
> 분기를 건드릴 때는 동현 사전 합의 필수.

---

## 5. 에이전트 작업 시 주의사항

### 5-1. 공유 자료구조 접근 규칙
- **모든 cross-thread 공유 자료구조는 `pthread_rwlock_t` 로 보호.**
  read-heavy 면 mutex 가 아니라 rwlock 으로 시작
- read-only 경로는 반드시 `pthread_rwlock_rdlock`, write 경로만 `wrlock`
- 락 보유 중 syscall / malloc 최소화. 가능하면 lock 밖에서 준비 후 swap-in
- **락 획득 순서 (절대 어기지 말 것)**: catalog → table → cache
  - 역순으로 잡는 코드는 PR 리젝트
- 락 해제는 `goto cleanup` 단일 출구 패턴 권장 (early return 금지)

### 5-2. Dict Cache 위치 / 호출 경계 ⚠
- **`dict_cache_*` 호출은 `src/router.c` 의 `/api/dict` 핸들러 안에서만.**
- ❌ `src/engine.c` 에서 `dict_cache_*` 호출 금지 (엔진 핵심 경로 무오염)
- ❌ `src/server.c` 에서 cache 라이프사이클 훅 직접 추가 금지
  → `pthread_once` lazy init 또는 router.c 파일 scope static 으로 처리
- **cache miss 후 DB 조회는 cache lock 밖에서 수행.** 같은 단어 동시 miss
  시 중복 DB 조회 발생 가능하나 결과 정합성 문제 X — 허용 trade-off
- `/api/admin/insert` 성공 경로에서는 반드시
  `dict_cache_invalidate("english:" + english)` 호출 (write 후 stale 응답 방지)
- 캐시 hit 으로 응답한 경우에도 stats 카운터 (`dict_cache_hits()`) 는 증가
  (응답 JSON 키는 `cache_hit`/`cache_hits` 짧은 형태 유지)

### 5-3. threadpool worker 금지 사항
- **워커 안에서 blocking I/O 금지**: `sleep`, `usleep`, fsync 루프,
  외부 프로세스 wait, DNS resolve 등
  - 클라이언트 socket read/write 는 예외 (job 자체임)
- **워커 안에서 다른 워커에 submit 금지** (큐 full 시 self-deadlock)
- 긴 작업은 timeout 으로 끊고 클라이언트에 503 응답
- 워커 죽으면 풀 전체 죽음. 절대 `pthread_exit` 직접 호출 금지

### 5-4. Trie 와 B+Tree 일관성
- INSERT 는 두 인덱스 모두 갱신. 둘 다 성공해야 commit
- 한쪽만 갱신된 채로 리턴하는 경로 금지 (rollback or fail-fast)
- DELETE 도 동일
- Trie 입력 검증은 router 핸들러에서 (non-ASCII / 대문자 / 기호 → 400).
  trie 내부 자체는 a-z 가정으로 단순화

### 5-5. 동적 쓰레드 풀 변경 시
- 워커 수 변경 결정은 모니터 쓰레드만. 워커가 자기 자신을 spawn 금지
- 워커 추가는 atomic 카운터 증가 후 `pthread_create`. 실패 시 롤백
- 16 상한은 `#define` 한 곳에서만 관리

### 5-6. 인터페이스 / 빌드
- `include/*.h` 기존 시그니처는 PM 승인 없이 변경 금지
  (단, **신규 헤더** `src/dict_cache.h` (private), `include/trie.h` 는
  해당 담당자가 생성 OK)
- 외부 라이브러리 추가 금지 (pthread 만 허용)
- 함수 시그니처 등 구현 디테일은 본 문서에 적지 말고 `CLAUDE.md` 참조
- 커밋 전 `make` 경고 0, `make test` 통과, 동시성 코드면 `make tsan` 통과

---

## 6. 테스트 / 벤치마크 시나리오 — 영한 사전

### 6-1. 데이터셋
- 테이블: `dictionary (english VARCHAR, korean VARCHAR)`
- 인덱스: `english` 컬럼 (B+Tree equality + Trie prefix). `korean` 은 인덱스 없음
- 규모: ~10만 entries (시드 fixture 는 `scripts/` 에 추가 예정)

### 6-2. 부하 패턴
| 시나리오 | 엔드포인트 | 동시 | 목적 |
|---|---|---|---|
| 정상 | `/api/dict?english=*` 95% + `/api/admin/insert` 5% | 50 | dict cache hit rate, p95 latency |
| Read 스파이크 | `/api/dict?english=apple` 100% | 200 | 동적 풀 4→16 확장 + 캐시 hit 효과 |
| id 조회 | `/api/dict?id=N` 100% | 50 | id 키 prefix 정확 동작 확인 |
| 운영자 INSERT | `/api/admin/insert` 1 req/s + 위 부하 | + | rwlock write 가 read 차단도, invalidate 정확성 |
| 자동완성 | `/api/autocomplete?prefix=ap` 100% | 50 | trie 경로 latency, 캐시 미적용 확인 |
| 역방향 | `/api/dict?korean=사과` 100% | 20 | 인덱스 없는 선형 스캔 + dict cache 미적용 baseline |

### 6-3. 검증 항목
- p50 / p95 / p99 latency, QPS
- dict cache hit rate (정상 시나리오 ≥ 80% 목표)
- 워커 수 시계열 (스파이크 4 → 16 도달까지 시간)
- `/api/admin/insert` 직후 동일 단어 조회 시 stale 응답 없음 (invalidate 검증)
- prefix 쿼리: trie 노드 visit 수, suggestion latency
- `mode=single` 대비 throughput 비율 (발표 임팩트 수치)

### 6-4. 회귀
- W7 회귀 227 개는 어떤 변경 후에도 통과해야 함
- 신규 `test_dict_cache.c`, `test_trie.c` 는 각 PR 에 동봉
- 동시성 변경 PR 은 `make tsan` 통과 필수

---

## 7. 발표 시나리오 (4 분, 영한 사전)

```
0:00 ─ 0:30   문제: 사전 검색 폭주 + 운영자 단어 추가가 동시에 발생
0:30 ─ 1:00   아키텍처 한 장: 동적 풀 + router dict cache + rwlock + trie
1:00 ─ 1:45   데모: read 200 동시 발사 → 워커 4 → 16 자동 확장 시계열
1:45 ─ 2:30   데모: 동일 단어 반복 조회 → cache hit rate 상승 (대시보드)
2:30 ─ 3:15   데모: 자동완성 (`/api/autocomplete?prefix=ap`) 5 개 suggestion
3:15 ─ 3:45   데모: 운영자 INSERT 직후 invalidate 검증 (stale 없음)
3:45 ─ 4:00   수치: cache hit rate, mode=single 대비 throughput N×
```

발표: **목요일 오전**, 4 분 데모 + 1 분 QnA.

---

## 8. 금지 사항

- `include/*.h` 기존 헤더 PM 승인 없이 수정 (신규 `src/dict_cache.h` private,
  `include/trie.h` 는 OK)
- 외부 HTTP / JSON 라이브러리 추가 (pthread 만 허용)
- W7 엔진 코드 (`parser.c`, `executor.c`, `storage.c`, `bptree.c`) 를
  동현 외 다른 사람이 수정 (용 의 trie 분기는 사전 합의 후 zone 한정)
- W7 회귀 테스트 227 개 깨지는 PR 머지
- `prev_project/`, `agent/` 수정 (아카이브 영역)
- **`src/engine.c` 에서 `dict_cache_*` 호출 — dict cache 는 router 의
  `/api/dict` 핸들러 안에만 존재** (2026-04-22 회의 결정)
- **`src/server.c` 에서 dict cache 라이프사이클 훅 추가** — router lazy
  init 사용
- threadpool 워커 안에서 blocking syscall 호출
- Trie 와 B+Tree 둘 중 하나만 갱신하는 INSERT / DELETE
- 락 획득 순서 위반 (catalog → table → cache 외)
- force push (`--force`). 필요 시 `--force-with-lease`

---

_2026-04-22 회의 결정 (cache pivot, 영한 방향) 반영. 1차 라운드 히스토리는
`w8_handoff.md`, 함수 시그니처 / 자료구조 디테일은 `CLAUDE.md`, zone 단위
교차 수정 매트릭스는 `TEAM_RULES.md § 10` 와 `docs/round2_integration_map.md` 참조._
