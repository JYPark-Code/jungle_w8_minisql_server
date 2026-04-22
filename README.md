# minisqld — Multi-threaded Mini DBMS in Pure C

> C11 로 작성된 **단일 프로세스 HTTP DBMS 데몬**. 외부 런타임 의존성 0 개
> (`pthread` 만 사용). W7 B+Tree 인덱스 엔진 위에 직접 작성한 HTTP/1.1
> 서버를 얹은 구조.

수요코딩회 하루짜리 해커톤으로 시작해, 두 차례 리팩토링 라운드를 거친
프로젝트. Round 1 은 엔진을 고정 크기 스레드풀 + 테이블 단위 RW 락 하의
HTTP 서버로 감쌌고, Round 2 에서 자가 조정 스레드풀 · 결과 캐시 · 자동완성
용 Trie prefix 인덱스를 추가한다.

영문판: [`README_EN.md`](README_EN.md) (동일 내용 아카이빙).

---

## 프로젝트 상태

**Round 2 리팩토링 — 진행 중.** Round 1 은 4 개 feature PR 이 `dev` 에
머지 완료. Round 2 작업은 담당자별 브랜치에서 진행 중이며 범위는 고정.
담당자 매핑은 [Team](#team) 참조.

| Round | 범위 | 상태 |
|---|---|---|
| Round 1 (MP0~MP4) | 데몬 뼈대, HTTP/1.1 서버, 고정 스레드풀, 테이블 RW 락, stub 기반 mix-merge | ✅ `dev` 머지 완료 |
| Round 2 | 동적 스레드풀, router-level dict cache, Trie prefix 검색 (ASCII 영어), UI 재디자인 | ⏳ 진행 중 |
| Final | 통합 빌드, 벤치마크 수치 확정, `dev → main` | ⏳ 대기 |

---

## Features

**Round 1 (머지 완료)**
- Pure C11 — HTTP / JSON 라이브러리 미사용. 런타임 의존성은 `pthread`
  하나뿐.
- HTTP/1.1 서버 직접 구현 (`src/server.c`, `src/protocol.c`,
  `src/router.c`). 정적 파일 서빙 포함.
- 고정 크기 워커 스레드풀 + blocking job queue
  (`src/threadpool.c`).
- 테이블 단위 reader/writer lock + 전역 catalog lock
  (`src/engine_lock.c`). SELECT 는 동시 실행, INSERT/UPDATE/DELETE 는
  테이블 단위 직렬화.
- `?mode=single` 토글로 전역 mutex 기반 baseline 제공. 멀티 워커
  경로와 A/B 비교에 사용.
- W7 B+Tree 인덱스 (`src/bptree.c`, `src/index_registry.c`) 를
  in-process 로 재사용 — 요청당 subprocess 스폰 없음.
- ANSI REPL 클라이언트 (`client/repl.c`). 터미널 사용 및 데모 백업용.

**Round 2 (진행 중)**
- **동적 스레드풀.** 워커 4 개로 시작, 사용률 80% 도달 시 +4 씩 확장,
  상한 16. 축소 정책은 이 라운드 범위 외 —
  [Known Issues](#known-issues--future-work) 참조.
- **Router-level dict cache + reader/writer lock.** `/api/dict`
  엔드포인트 전용 LRU 캐시. 다수 reader 가 공유하고 writer
  (`/api/admin/insert`) 는 해당 key 를 invalidate. 엔진 내부가 아닌
  router 레벨에 둠으로써 엔진 핵심 경로를 오염시키지 않고 캐시 범위를
  사전 서비스에 한정.
- **Trie 기반 prefix 검색 (ASCII 영어 only).** B+Tree point/range 조회에
  더해, 영어 단어 prefix 로 `O(k)` 의 autocomplete 지원. 한글 body 컬럼
  은 인덱스 대상이 아님.
- **프론트엔드 재디자인.** Round 1 의 dark theme stress 페이지를 애플 ·
  토스 스타일의 minimal UI 로 교체.

---

## Architecture

```
┌─────────────┐
│ Client (FE) │  Apple/Toss-inspired UI
└──────┬──────┘
       │ HTTP/1.1 (TCP, Connection: close)
       ▼
┌──────────────────────────────────────┐
│ Thread Pool (dynamic, 4 → 16)        │
│  trigger: utilization >= 80%, +4     │
└──────┬───────────────────────────────┘
       ▼
┌───────────────────────────────┐
│ Query Parser → Planner        │
└──────┬────────────────────────┘
       ▼
┌───────────────────────────────┐       ┌─────────────────────────┐
│ Router + Dict Cache (LRU+RW)  │◀──────│ invalidate on INSERT    │
└──────┬────────────────────────┘       └──────────┬──────────────┘
       │ miss                                      │
       ▼                                           │
┌───────────────────────────────┐                  │
│ Trie Index  +  B+Tree Storage │──────────────────┘
└───────────────────────────────┘
```

| 레이어 | 파일 | 동시성 전략 |
|---|---|---|
| Socket | `src/server.c` | 메인 스레드 `accept()`, `fd` 를 pool 로 전달 |
| Thread pool | `src/threadpool.c` | mutex + condvar queue, 동적 resize (R2) |
| HTTP | `src/protocol.c`, `src/router.c` | 워커당 stateless |
| Router + Dict Cache | `src/router.c`, `src/dict_cache.c` (R2) | LRU 는 단일 rwlock (또는 mutex), router 자체는 worker 당 stateless. cache miss 후 DB 조회는 lock 밖 |
| Engine | `src/engine.c`, `src/engine_lock.c` | 테이블 RW lock + catalog lock + single-mode mutex |
| Index | `src/bptree.c`, `src/trie.c` (R2, ASCII 영어 only) | read 측은 engine 레이어 락 하에서 수행. 한글 body 역방향 조회는 선형 스캔 fallback |
| Storage | `src/storage.c` (W7) | engine 레이어가 write 를 직렬화 |

다이어그램은 `CLAUDE.md § Architecture` 와 동일. Round 2 의 모듈 경계
계약은 `CLAUDE.md § 모듈 간 인터페이스` 에 정의되어 있으며 4 팀이 시그니처
확정 전까지 `TBD` 상태.

---

## Demo Scenario

**유스케이스: 영한 사전 서비스.** 영어 단어 입력 → 한글 해석 반환.
Round 2 의 3 개 기능을 같은 데이터셋 위에서 한 번에 시연.

1. **동시 조회 (`/api/dict?english=apple`).** 다수 사용자가 동시에 영어
   단어로 한글 뜻 조회. router 의 dict cache 가 반복 조회를 흡수
   (key prefix `english:<word>`). id 조회 (`?id=N`) 도 동일 경로
   (key prefix `id:<N>`).
2. **운영자 등록 (`/api/admin/insert`).** 관리자가 새 단어 쌍 추가. 테이블
   write lock 직렬화 + 해당 english key 의 dict cache invalidation 자극.
3. **실시간 자동완성 (`/api/autocomplete?prefix=app`).** 입력 중 영어 prefix
   로 Trie 조회 → 매칭 단어 목록 반환. ASCII 소문자 영어 only.

**역방향 조회 (option B)**: `/api/dict?korean=사과` 로 한글 body 입력 시에도
동작한다. 인덱스가 없어 선형 스캔 경로 (느림) 이지만, "인덱스 있는 vs
없는" 비교 데모로 활용 가능.

데모 페이지 (`web/concurrency.html`) 가 위 세 흐름을 돌리며,
`GET /api/stats` 가 내보내는 지표 (active workers, queue depth, 누적 lock
wait) 를 실시간으로 표시.

주의: 브라우저의 per-origin HTTP/1.1 동시 연결 제한은 6. 내장 stress 탭만
으로는 서버의 멀티 워커 throughput 을 완전히 드러낼 수 없음. 실제 수치는
`xargs -P` 또는 REPL 사용 — [Benchmark](#benchmark) 참조.

---

## Quick Start

```bash
git clone https://github.com/JYPark-Code/jungle_w8_minisql_server.git
cd jungle_w8_minisql_server

# 빌드 (C11 toolchain + pthread 외에 의존성 없음)
make

# 데이터 디렉토리 생성 후 데몬 실행
mkdir -p data
./minisqld --port 8080 --workers 8 --data-dir ./data --web-root ./web
```

stderr 에 `[server] listening on port 8080` 이 찍히면 준비 완료.
브라우저로 <http://localhost:8080/> 접속하거나 API 직접 호출:

```bash
curl -X POST http://localhost:8080/api/query \
     -H "Content-Type: text/plain" \
     -d "SELECT * FROM users WHERE id = 1"
```

### 요구사항
- GCC 9+ (또는 동급 C11 지원의 Clang)
- GNU Make
- `pthread` (리눅스 glibc 에 기본 포함)
- `valgrind` (선택 — `make valgrind` 실행 시)

추가 라이브러리, 런타임, 패키지 매니저 없음. 빌드 산출물은 단일 바이너리.

### CLI 옵션

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--port N` | `8080` | Listen 포트 |
| `--workers N` | `8` | 초기 워커 수. Round 2 에서 16 까지 동적 확장 |
| `--data-dir PATH` | `./data` | CSV / 스키마 루트 |
| `--web-root PATH` | `./web` | 정적 파일 루트 |
| `--help` | — | 사용법 |
| `--version` | — | 버전 |

---

## API

모든 응답은 `Content-Type: application/json`.

| Method | Path | 설명 |
|---|---|---|
| `POST` | `/api/query` | 하나 이상의 SQL 실행 (body: raw SQL, `text/plain`) |
| `POST` | `/api/query?mode=single` | 위와 동일하지만 전역 mutex 로 직렬화. serial baseline 용 |
| `GET` | `/api/explain?sql=...` | 해당 SQL 의 인덱스 사용 여부 및 노드 visit 수 |
| `GET` | `/api/stats` | 누적 카운터 (total queries, lock wait, active workers) |
| `POST` | `/api/inject` | 더미 행 대량 주입. **Round 1 에서는 placeholder** (501 반환). Round 2 에서 연결. |
| `GET` | `/` 및 `/<file>` | `--web-root` 하의 정적 파일 |

Round 2 에서 추가 예정 (영한 사전 서비스):

| Method | Path | 설명 |
|---|---|---|
| `GET` | `/api/dict?english=<english>` | 영한 조회 (primary). dict cache 경유 (key `english:<word>`). miss 시 engine 호출 |
| `GET` | `/api/dict?id=<N>` | id 조회 (옵션). dict cache 경유 (key `id:<N>`) |
| `GET` | `/api/dict?korean=<한글>` | 역방향 조회 (option B). 인덱스 없음, 선형 스캔 경로 (느림). dict cache 미적용 |
| `GET` | `/api/autocomplete?prefix=<english>` | Trie prefix 쿼리 (ASCII 영어 only, 길이 상한) |
| `POST` | `/api/admin/insert` | 사전 항목 등록. 성공 시 dict cache invalidate |

Round 2 엔드포인트 시그니처는 구현 머지 전까지 `CLAUDE.md` 의 `TBD`
계약으로 관리.

---

## Build Targets

```bash
make                # ./minisqld 빌드
make test           # 단위 + 회귀 테스트 (W7 + W8 모듈)
make tsan           # ThreadSanitizer 빌드 → ./minisqld_tsan
make valgrind       # valgrind --leak-check=full 로 테스트 재실행
make bench          # W7 자산 — B+Tree 순수 벤치 (참조용)
make repl           # ./minisqld-repl — 데몬용 ANSI HTTP 클라이언트
make clean
```

기본 빌드의 `CFLAGS`:
`-Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g`.
CI 는 `-Werror` 로 돌아감.

---

## Benchmark

> 아래 수치는 발표 이후 실측으로 채워질 예정. 라운드 간 비교가 가능하도록
> 표 형태만 먼저 고정.

계획된 방법론: 10 만 행 규모의 어학사전 테이블에 대한 랜덤 `SELECT` 10K
회. "Single-thread" 와 "Multi-thread" 열은 warm 캐시 제외, "+Cache"
열은 포함. 브라우저 연결 제한을 우회하기 위해 `xargs -P N` 으로 데몬에
직접 부하.

| 시나리오 | Single-thread | Multi-thread | +Cache | +Dynamic Pool |
|---|---|---|---|---|
| 1K SELECT | TBD | TBD | TBD | TBD |
| 10K SELECT | TBD | TBD | TBD | TBD |

**Round 1 snapshot** (`users` 테이블 비어있는 상태에서 `xargs -P 8` 로 64
요청, 데몬에 직접 부하):

| 시나리오 | 총 소요 |
|---|---|
| Serial (`xargs -P 1`) | 약 380 ms |
| Multi (`xargs -P 8`, `/api/query`) | 약 85 ms |
| Single (`xargs -P 8`, `?mode=single`) | 약 145 ms |

빈 테이블 기준이라 쿼리 work 가 매우 작고 HTTP 프레이밍 오버헤드가
지배적인 snapshot. Round 2 에서 10 만 행 어학사전으로 재측정 예정. 순수
B+Tree 참고 수치 (인덱스 조회가 선형 스캔 대비 최대 1,842 배) 는
[`docs/README_w7.md`](docs/README_w7.md) 참조.

---

## Team

- **지용 (Lead)** — 동적 스레드풀, graceful shutdown
- **동현** — LRU cache, reader-writer lock
- **승진** — 프론트엔드 (Apple/Toss 스타일), autocomplete UI
- **용** — Trie 자료구조, prefix search 쿼리 지원

Round 1 은 다른 기준으로 분배되었음 (엔진 / 서버 / 스레드풀 기본기 /
PM 인프라). 위 목록은 Round 2 기준.

---

## Known Issues / Future Work

- **스레드풀 축소 정책 미구현.** Round 2 범위는 사용률 80% 에서의 확장
  까지. 유휴 워커 해제 (scale-down) 는 후속 과제. 장기 구동 시 최대 크기
  에서 유지됨.
- **캐시 범위 = `/api/dict` 전용.** 엔진 내부 query cache 가 아니라
  router 레벨 dict cache. 캐시 일관성은 *invalidate-on-write* —
  `/api/admin/insert` 성공 시 해당 영어 key 의 엔트리를 drop.
- **영한 단방향 primary, 한글 body 역방향은 선형 스캔.** Trie 인덱스는
  `dictionary.english` 컬럼에만 존재. `/api/dict?korean=사과` 같은 역방향
  조회는 인덱스 없이 W7 executor 의 선형 스캔으로 동작 — 기능은 정상,
  속도는 느림. "인덱스 있는 vs 없는" 비교 데모로 사용 가능.
- **HTTP Keep-alive 미지원.** 요청당 별도 TCP 연결 (`Connection:
  close`). 브라우저 기반 스트레스 테스트는 per-origin 동시 연결 제한
  (6) 에 막힘. 실제 부하 수치는 `xargs -P` 또는 REPL 로.
- **`/api/inject` 는 Round 2 에서 완성.** Round 1 의 router 는 이
  엔드포인트에 대해 `501 Not Implemented` 반환.
- **CI toolchain 설치 오버헤드.** GitHub Actions 의 각 잡 (build /
  test / tsan / valgrind) 이 매번 `apt-get update` 를 수행.
  `valgrind` 외 잡에서는 제거할 예정 (`chore/ci-speedup`).

---

## Directory Structure

```
.
├── include/               공개 인터페이스 헤더 (PM 관리)
├── src/                   C 소스 (W7 엔진 + W8 레이어)
├── tests/                 회귀 + 동시성 단위 테스트
├── bench/                 B+Tree 순수 벤치 (W7)
├── scripts/               fixture 생성기 + 부하 스크립트
├── client/                ANSI REPL HTTP 클라이언트
├── web/                   프론트엔드 (동시성 데모)
├── docs/                  아키텍처 다이어그램, 이전 라운드 README
├── agent/                 이전 라운드 AI 협업 컨텍스트 아카이브
├── prev_project/          이전 라운드 웹 데모 아카이브
│   ├── sql_parser_demo/   W6 파서 데모
│   └── bp_tree_demo/      W7 B+Tree 데모
├── .github/workflows/     CI (build / test / tsan / valgrind)
├── CLAUDE.md              프로젝트 전반 기술 설계 및 의사결정 기록
├── agent.md               Round 1 설계 결정 및 마일스톤
├── TEAM_RULES.md          브랜치 / PR / CI 규약
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
