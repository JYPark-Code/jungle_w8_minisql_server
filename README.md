# minisqld — Multi-threaded Mini DBMS in Pure C

> **C11 단일 프로세스 HTTP DBMS 데몬.** 외부 런타임 의존성 0 개 (`pthread`
> 만 사용). W7 의 B+Tree 인덱스 엔진 위에 직접 작성한 HTTP/1.1 서버 +
> 자가조정 스레드풀 + router-level dict cache + Trie prefix 인덱스를 얹은
> **영한 사전 시연 환경**.

수요코딩회 하루짜리 해커톤 → 두 차례 리팩토링. **Round 1** 은 엔진을 고정
풀 + 테이블 RW 락 하의 HTTP 서버로 감쌌고, **Round 2** 는 자가조정 풀 +
dict cache + Trie + 새 FE 로 사전 서비스 데모를 완성.

영문판: [`README_EN.md`](README_EN.md) (동일 내용 아카이브).

---

## 발표 한 줄

> **"100 명이 영어 단어 검색 + 운영자가 새 단어 등록 + 자동완성 입력. 하나의
> C 데몬, 4 → 16 동적 워커 풀, rwlock + atomic LRU dict cache 가 read 폭주를
> 흡수하면서 write 도 막히지 않는다."**

W7 (요청당 1.8 s subprocess rebuild) 의 한계 → in-process 데몬화 →
end-to-end SQL 처리량이 `make bench` 1,842× 자료구조 수치에 근접.

---

## 프로젝트 상태

| Round | 범위 | 상태 |
|---|---|---|
| Round 1 (4 PR, MP0 ~ MP4) | 데몬 뼈대, HTTP/1.1 서버, 고정 스레드풀, 테이블 RW 락, mix-merge | ✅ `dev` 머지 |
| Round 2 (15+ PR) | 동적 풀 (4 → 16), router-level dict cache (rwlock + atomic LRU), Trie ASCII prefix, FE 재디자인, warming-up 게이트, 10 만 행 fixture | ✅ `dev` 머지 |
| Final | `dev → main`, 발표 리허설, README 수치 채우기 | ⏳ 진행 중 |

---

## 시연 시나리오 — 영한 사전

**데이터셋**: kengdic 기반 ~10 만 행 `dictionary (english, korean)`. 시드는
`scripts/gen_dictionary_fixture.py` 로 재생성 가능. fixture 가 없어도 데몬은
빈 dictionary 테이블을 자동 생성하고 `/api/admin/insert` 부터 받아 쓸 수
있음 (PR #27 의 chicken-and-egg fix).

### 1. 단일 단어 조회 — cache hit 동작 보기

```bash
curl 'http://localhost:8080/api/dict?english=apple'
# → {"ok":true,"rows":[{"korean":"사과"}],"elapsed_ms":N,"cache_hit":false}

curl 'http://localhost:8080/api/dict?english=apple'
# → cache_hit:true (두 번째는 dict_cache rdlock 만, engine 미경유)
```

### 2. 동시 SELECT 폭주 — 동적 풀 4 → 16 확장 + cache hit/miss 비교

```bash
# (a) 100 동시, 동일 단어 (cache hit-heavy: rwlock rdlock 만으로 해소)
seq 100 | xargs -P 100 -I{} \
    curl -s 'http://localhost:8080/api/dict?english=apple' > /dev/null

# (b) 100 동시, 동일 단어 + nocache=1 (cache 우회 → engine 직행, 테이블 rdlock 경쟁)
seq 100 | xargs -P 100 -I{} \
    curl -s 'http://localhost:8080/api/dict?english=apple&nocache=1' > /dev/null

curl 'http://localhost:8080/api/stats'
# → active_workers, queue_depth, total_workers (확장 결과), cache_hits/misses
```

`(a)` 가 `(b)` 보다 한 자릿수 빠르면 dict cache 효과. 이전엔 단일 mutex 라
오히려 (a) 가 느렸음 — Issue #24 → PR #25 에서 수정 (아래 트러블 로그 §2).

### 3. SELECT 폭주 + 운영자 INSERT 동시 — rwlock 효과

```bash
# 한 터미널: SELECT 폭주
while true; do
    curl -s 'http://localhost:8080/api/dict?english=apple' > /dev/null
done &

# 다른 터미널: 운영자 INSERT
curl -X POST http://localhost:8080/api/admin/insert \
     -H 'Content-Type: application/json' \
     -d '{"english":"orchestrate","korean":"편성하다"}'

# 결과: SELECT 가 잠깐 직렬화 후 다시 병렬. INSERT 직후
#       /api/dict?english=orchestrate 가 cache miss → engine 경유 → 새 결과.
```

### 4. 실시간 자동완성 — Trie

```bash
curl 'http://localhost:8080/api/autocomplete?prefix=app&limit=5'
# → {"ok":true,"suggestions":["app","apple","apply","approach","appear"],"elapsed_ms":<1}
```

브라우저: <http://localhost:8080/> — 단어 입력 → 자동완성 드롭다운 + 한글
뜻 카드. `web/concurrency.html` 의 R/W Contention 탭은 부하를 내장 발사기
로 직접. cache hit 으로 lock 경쟁이 가려지지 않게 `nocache=1` 토글이 적용
되어 있음 (commit `8cded49`, 트러블 로그 §6).

---

## Architecture

```
[브라우저 / curl / REPL]
        │ HTTP/1.1 (Connection: close)
        ▼
┌──────────────────────────────────────────────┐
│  minisqld (단일 프로세스)                     │
│                                                │
│  server.c        accept loop + graceful drain │
│       │          (SIGINT → drain → join)       │
│       │ enqueue(fd)                            │
│       ▼                                        │
│  threadpool.c    동적 N=4~16, 확장 +4 @ ≥80% │
│       │                                        │
│       ▼                                        │
│  protocol.c      HTTP parse                    │
│  router.c        method+path → handler         │
│       │                                        │
│       │   /api/dict ──► dict_cache.c (rwlock+LRU)│
│       │                  │ hit  → JSON return  │
│       │                  │ miss               │
│       ▼                  ▼                    │
│  engine.c (engine_lock 경유, **dict cache 모름**) │
│       │                                        │
│       ▼                                        │
│  parser → executor → storage                   │
│              ├── bptree (equality / id range) │
│              └── trie   (prefix, ASCII 영어)  │
└──────────────────────────────────────────────┘
```

| 레이어 | 파일 | 동시성 전략 |
|---|---|---|
| Socket | `src/server.c` | 메인 스레드 `accept()` + SIGINT 시 graceful drain (5 s) |
| Thread pool | `src/threadpool.c` | mutex + condvar queue, **동적 resize** (4 → 16, +4 @ ≥80% 3 회 연속) |
| HTTP | `src/protocol.c`, `src/router.c` | 워커당 stateless |
| Dict Cache | `src/dict_cache.c` | **`pthread_rwlock_t` + atomic LRU clock** — get rdlock, put / invalidate wrlock. cache miss 시 DB 조회는 cache lock 밖 |
| Engine | `src/engine.c`, `src/engine_lock.c` | 테이블 RW lock + catalog lock + `?mode=single` 직렬화 |
| Index | `src/bptree.c`, `src/trie.c` | engine 레이어 락 하에서 read |
| Storage | `src/storage.c` (W7) | engine 레이어가 write 직렬화 |

상세 모듈 경계는 `CLAUDE.md § 모듈 간 인터페이스`, Round 2 작업 zone
분할은 `docs/round2_integration_map.md` 참조.

---

## Engineering Decisions & Trouble Log

발표 / 리뷰 때 가장 자주 받을 질문에 대한 결정과 그 배경. 모두 `dev` /
`main` commit / PR 으로 추적 가능.

### 1. Cache 위치 — engine 내부 → router endpoint 단위 (PR #14)

**문제**. 초안은 `engine_exec_sql` 내부에 query cache 를 두는 안. SQL
normalize / 결과 lifetime / DML 전체 invalidate / lookup 과 clear 사이의
race window 등 엔진 핵심 경로에 책임 누적. W7 회귀 227 케이스 깨질 위험.

**결정**. 캐시 소비자를 `/api/dict` 엔드포인트 한 곳으로 한정. `src/dict_cache.c`
는 `src/router.c` 만 호출. `src/engine.c` 는 cache 의 존재를 모름. 캐시 키는
`english:<word>` / `id:<N>` prefix 로 검색 종류별 분리.

**효과**. 엔진 회귀 무영향. invalidate 책임이 단 한 곳 (`/api/admin/insert`).
디버깅 / 측정 / nocache 토글 모두 endpoint 단위에서 결정 가능.

### 2. dict_cache 단일 mutex → rwlock + atomic LRU (PR #25, Issue #24)

**증상**. 시연 중 `/api/dict?english=apple` 100 병렬 시
**cached 531 ms > nocache 106 ms 역전**. 캐시가 오히려 느림.

**원인**. dict_cache 가 단일 `pthread_mutex_t` 로 모든 경로 직렬화. 100
reader 가 한 락에 큐잉. 반면 `nocache=1` 경로는 engine 의 테이블 **rdlock**
하에서 진짜 병렬.

**해결**.
- `pthread_mutex_t` → `pthread_rwlock_t`. get 은 rdlock, put / invalidate
  만 wrlock
- `last_used` / `clock` / `hits` / `misses` 모두 `atomic_ulong` — read path
  lock-free
- 결과: cached 가 nocache 대비 다시 빨라짐 (정상 위계 회복)

### 3. 빈 DB 503 chicken-and-egg (PR #27)

**증상**. 새로 띄운 데몬에 `/api/dict` 호출 → 503 `warming_up`. 시간이
지나도 안 풀림. `/api/admin/insert` 도 503 → 데이터를 넣을 방법이 없음.

**원인**. `engine_is_ready()` = `s_engine_ready AND s_dictionary_trie_ready`.
후자는 dictionary 스키마가 있을 때만 true. 빈 DB 면 영원히 false. 모든
사전 엔드포인트 (insert 포함) 가 같은 게이트를 쓰니 데드락.

**해결**. `engine_init` 에서 dictionary 스키마 미존재 시 자동 생성 + 빈
trie 로 ready 표시. fixture 없는 첫 부팅에서도 `admin/insert → autocomplete
→ dict` 흐름이 동작.

### 4. warming-up 503 게이트 (PR #22)

**문제**. 데몬 부팅 / fixture import 도중 트래픽이 들어오면 trie 가 부분만
채워진 상태로 응답해 잘못된 결과. 운영자 import 와 사용자 트래픽이 겹치는
시점에 같은 race.

**해결**. `engine_is_ready()` 로 4 개 게이트 (`/api/dict` english /id / korean,
`/api/autocomplete`, `/api/admin/insert`) 보호 → 준비 안 되면 503 +
재시도 hint. FE 는 이를 받으면 backoff. ready 는 init 시 단방향 true,
shutdown 시 false (PR #27 의 자동 생성 후엔 부팅 1 초 내 true).

### 5. CI valgrind apt 16 분 hang (PR #20)

**증상**. 4 잡 (build / test / tsan / valgrind) 중 valgrind 만
`Install toolchain` step 에서 16 분 이상 hang. GitHub Actions runner 의
apt mirror 일시 장애.

**해결**. build / test / tsan 잡에서 apt 단계 완전 제거 (이미지 기본
toolchain 사용). valgrind 잡만 `valgrind` 패키지 설치 — 그것도
`--no-install-recommends + best-effort` 로 경량화. 평소 build 18 s /
test 42 s / tsan 37 s, valgrind 1 m 13 s 안정. 일시 hang 시
`gh run cancel <id> && gh run rerun <id> --failed` 로 즉시 회피.

### 6. R/W Contention 측정 cache hit 우회 (commit `8cded49` + PR #22 의 `?nocache=1`)

**문제**. `web/concurrency.html` 의 R/W Contention 탭에서 같은 단어를 100
reader / 1 writer 로 발사 → reader 들이 전부 cache hit → engine RW lock
경쟁이 측정에 안 잡힘. cache 효과만 보이고 락 경쟁은 평탄.

**해결**. 부하 발사기에서 `?nocache=1` 강제 → engine 직행. random key 옵션
도 추가해 hit rate 인위 조절. 시연 시 "캐시 효과 그래프" 와 "RW lock 경쟁
그래프" 를 분리해서 보여줌.

---

## Quick Start (시연 흐름)

```bash
git clone https://github.com/JYPark-Code/jungle_w8_minisql_server.git
cd jungle_w8_minisql_server

# 1. 빌드 (C11 toolchain + pthread 외 의존성 0)
make

# 2. (선택) 영한사전 fixture 생성 — kengdic 기반 ~10 만 행
mkdir -p data
python3 scripts/gen_dictionary_fixture.py    # data/tables/dictionary.csv 생성

# 3. 데몬 기동 (워커 8 시작, 부하에 따라 16 까지 자동 확장)
./minisqld --port 8080 --workers 8 --data-dir ./data --web-root ./web

# 4. 브라우저 → http://localhost:8080/  (자동완성 + 한글 뜻 카드)
# 5. 또는 curl 시연 (위 'Demo Scenario' 의 4 단계)
```

stderr 에 `[server] listening on port 8080` 이 찍히면 준비 완료. dictionary
가 비어 있어도 503 무한 대기 없이 `admin/insert → autocomplete` 경로 동작
(PR #27).

### 요구사항
- GCC 9+ (또는 동급 C11 지원의 Clang)
- GNU Make
- `pthread` (리눅스 glibc 에 기본 포함)
- `valgrind` (선택, `make valgrind`)
- `python3` (fixture 생성기 — 시연 데이터 필요 시)

추가 라이브러리 / 런타임 / 패키지 매니저 없음. 산출물은 단일 바이너리.

### CLI 옵션
| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--port N` | `8080` | Listen 포트 |
| `--workers N` | `8` | 초기 워커 수. 사용률 ≥ 80% 시 +4 씩 16 까지 자동 확장 |
| `--data-dir PATH` | `./data` | CSV / 스키마 루트 |
| `--web-root PATH` | `./web` | 정적 파일 루트 |
| `--help` / `--version` | — | |

---

## API

모든 응답은 `Content-Type: application/json`.

| Method | Path | 설명 |
|---|---|---|
| `GET` | `/api/dict?english=<en>` | 영한 조회 (primary). dict cache key `english:<en>`. miss → engine |
| `GET` | `/api/dict?id=<N>` | id 조회. dict cache key `id:<N>` |
| `GET` | `/api/dict?english=<en>&nocache=1` | 캐시 우회 (engine 직행). 측정 / 디버깅용 |
| `GET` | `/api/dict?korean=<한글>` | 역방향 (option B). 인덱스 없음 → 선형 스캔. dict cache 미적용 |
| `GET` | `/api/autocomplete?prefix=<en>&limit=<N>` | Trie prefix. ASCII 소문자 영어 only, 비ASCII / 대문자 → 400 |
| `POST` | `/api/admin/insert` | 사전 항목 등록 (body JSON `{"english":..,"korean":..}`). 성공 시 `english:<en>` cache invalidate |
| `POST` | `/api/query` | raw SQL 실행 (body `text/plain`) |
| `POST` | `/api/query?mode=single` | 위와 동일하되 전역 mutex 직렬화 (baseline) |
| `GET` | `/api/explain?sql=...` | 인덱스 사용 여부, 노드 visit 수 |
| `GET` | `/api/stats` | total_queries, lock_wait_ns, active_workers, total_workers, queue_depth, cache_hits, cache_misses |
| `GET` | `/` 및 `/<file>` | `--web-root` 하의 정적 파일 |

503 `warming_up` 응답은 데몬이 아직 ready 가 아닌 상태. 보통 부팅 후 < 1 s.

---

## Build Targets

```bash
make                # ./minisqld 빌드
make test           # 회귀 + W7 + Round 2 단위 테스트 (수백 케이스)
make tsan           # ThreadSanitizer 빌드
make valgrind       # valgrind --leak-check=full
make bench          # W7 자산 — B+Tree 순수 벤치
make repl           # ./minisqld-repl — ANSI HTTP 클라이언트
make clean
```

기본 `CFLAGS`:
`-Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g`.
CI 는 `-Werror`. 회귀 + 동시성 (TSan) + 메모리 (valgrind) 4 잡이 dev push /
PR 마다 실행 (`.github/workflows/ci.yml`).

---

## Benchmark

> 발표 직전 환경 고정 후 채워짐. 라운드 간 비교 가능하도록 표만 미리.

**지표 정의**
- *Single-thread*: 클라이언트 1 개 직렬 발사 (`xargs -P 1`)
- *Multi-thread*: 클라이언트 N 개 동시 발사 (`xargs -P N`)
- *+Cache*: dict cache 활성 (warm)
- *+Dynamic Pool*: 부하 중 워커 8 → 16 자동 확장 포함

**측정 명령 예** (10 만 행 dictionary fixture 가 있다는 가정)

```bash
# warm-up
seq 200 | xargs -P 100 -I{} \
    curl -s 'http://localhost:8080/api/dict?english=apple' > /dev/null

# 1K 시나리오
time seq 1000 | xargs -P 1  -I{} \
    curl -s "http://localhost:8080/api/dict?english=word{}" > /dev/null
time seq 1000 | xargs -P 50 -I{} \
    curl -s "http://localhost:8080/api/dict?english=word{}" > /dev/null
time seq 1000 | xargs -P 50 -I{} \
    curl -s "http://localhost:8080/api/dict?english=word{}&nocache=1" > /dev/null
```

| 시나리오 | Single-thread | Multi-thread | +Cache | +Dynamic Pool |
|---|---|---|---|---|
| 1 K SELECT | TBD | TBD | TBD | TBD |
| 10 K SELECT | TBD | TBD | TBD | TBD |

**Round 1 snapshot** (`users` 빈 테이블, `xargs -P 8` 64 요청, 데몬 직접):

| 시나리오 | 총 소요 |
|---|---|
| Serial (`xargs -P 1`) | 약 380 ms |
| Multi (`xargs -P 8`, `/api/query`) | 약 85 ms |
| Single (`xargs -P 8`, `?mode=single`) | 약 145 ms |

빈 테이블 + 매우 작은 쿼리라 HTTP 프레이밍 오버헤드가 지배적. Round 2 의
10 만 행 사전으로 재측정 시 의미 있는 수치 기대. 순수 B+Tree 기준치
(선형 스캔 대비 최대 1,842×) 는 [`docs/README_w7.md`](docs/README_w7.md).

---

## Team / 작업 분담

**Round 2 (현행)**
- **지용 (Lead / PM)** — 동적 스레드풀 + graceful shutdown (PR #15),
  fixture / warming-up 게이트 / nocache 옵션 (#22), 빈 DB 자동 생성 (#27),
  CI 경량화 (#20), mix-merge
- **동현** — Router-level dict cache 구현 (PR #18), single mutex →
  rwlock + atomic LRU 튜닝 (PR #25)
- **승진** — 프론트엔드 재디자인 (PR #21), 자동완성 UI, R/W contention
  시각화, FE 후처리 (#23, #26, #28)
- **용** — Trie 자료구조 (PR #16) + autocomplete / admin/insert 라우터
  (PR #19)

**Round 1**
- 동현 — engine.c thread-safe wrapping (PR #4)
- 용 — server / protocol / router (PR #2)
- 승진 — threadpool worker pool (PR #5)
- 지용 — pm-infra / engine_lock 테스트 / REPL (PR #1)

상세 owns 매트릭스: `TEAM_RULES.md § 10`.

---

## Known Issues / Future Work

- **HTTP Keep-alive 미지원.** 요청당 별도 TCP 연결 (`Connection: close`).
  브라우저는 per-origin 동시 연결 6 제한 → 내장 stress 탭만으로는 멀티
  워커 throughput 을 완전히 드러낼 수 없음. 실측은 `xargs -P` / REPL 권장
- **한글 body 역방향 인덱스 없음.** `/api/dict?korean=` 은 W7 executor 의
  선형 스캔 fallback (option B). 의도된 baseline 비교점
- **Trie 영어 ASCII only.** Unicode NFC 정규화 / 초성 검색 / 한글 자모
  분해 등은 범위 외. 확장 시 trie 노드 자식 배열 (현재 26 고정) 부터 재설계
- **dict cache 일관성 정책 = invalidate-on-write.** write-through 는 lock
  구간 길어져 read 성능을 깎는다는 판단으로 미채택
- **스레드풀 축소 정책.** 사용률 ≤ 30% × 30 회 연속 → -4 (floor = create
  초기값) 까지 PR #15 에 포함. hysteresis 2 초 적용. 더 정교한 시간축
  가중은 후속 과제
- **CI valgrind 평균 1 m 13 s, apt mirror 장애 시 hang.** PR #20 으로 빈도
  대폭 감소. 발생 시 cancel + rerun --failed 로 즉시 회피

---

## Directory Structure

```
.
├── include/               공개 인터페이스 헤더 (PM 관리)
├── src/                   C 소스 (W7 엔진 + W8 레이어 + dict_cache + trie)
│   ├── dict_cache.{c,h}   Round 2 router-level LRU (rwlock + atomic LRU)
│   └── trie.c             Round 2 ASCII 영어 prefix 인덱스
├── tests/                 회귀 + 동시성 단위 테스트 (W7 + dict_cache + trie + router)
├── bench/                 B+Tree 순수 벤치 (W7 자산)
├── scripts/               fixture 생성기 (gen_dictionary_fixture.py 등)
├── client/                ANSI REPL HTTP 클라이언트
├── web/                   프론트엔드 (concurrency.html — 탭 a, c)
├── docs/                  아키텍처 다이어그램, 이전 라운드 README, Round 2 Integration Map
├── agent/                 이전 라운드 AI 협업 컨텍스트 아카이브 (read-only)
├── prev_project/          이전 라운드 웹 데모 아카이브 (read-only)
│   ├── sql_parser_demo/   W6 파서 데모
│   └── bp_tree_demo/      W7 B+Tree 데모
├── .github/workflows/     CI (build / test / tsan / valgrind)
├── CLAUDE.md              레포 전역 규칙 + Round 2 결정사항 + 모듈 인터페이스
├── agent.md               Round 2 설계 요약 + 팀원 작업 영역 + 에이전트 규칙
├── TEAM_RULES.md          브랜치 / PR / CI 규약 (§ 10 Round 2 owns 매트릭스)
├── w8_handoff.md          Round 1 → Round 2 인수인계 노트
├── Makefile
├── README.md              (이 파일)
└── README_EN.md           영문판 (아카이브)
```

---

## Previous Rounds

- W6 SQL Parser — <https://github.com/JYPark-Code/jungle_w6_mini_mysql_sql_parser>
- W7 B+Tree Index DB — <https://github.com/JYPark-Code/jungle_w7_BplusTree_Index_DB>
- W7 최종 README — [`docs/README_w7.md`](docs/README_w7.md)
