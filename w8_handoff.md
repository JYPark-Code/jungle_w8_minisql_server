# w8_handoff.md — 1차 mix-merge → 2차 리팩토링 인수인계

> 1차 mix-merge 가 dev 에 완료되었습니다. 이 문서는 2차 리팩토링 시작 전
> 팀원들이 개별 검증하고, 회의에서 합의할 항목들을 정리한 인수인계 노트입니다.
> 읽은 순서: (1) 실행 → (2) 본인 역할 체크리스트 → (3) 회의 어젠다.

---

## 0. 1차 mix-merge 결과 (dev 기준)

| PR | 담당 | 상태 | 비고 |
|---|---|---|---|
| #5 | 승진 (threadpool) | MERGED | `threadpool.c` + `test_threadpool.c` + `web/concurrency.html` (탭 구조 + 탭 c) |
| #4 | 동현 (engine) | MERGED | `engine.c` + `test_engine_concurrent.c`, `engine_lock_*` 22 회 경유, EXPLAIN / BETWEEN 포함 |
| #2 | 용 형님 (server/protocol/router) | MERGED | accept loop + HTTP 파서 + 라우팅, `web/stress.html` (탭 (a) 실동작), test_protocol |
| #1 | 지용 (pm-infra) | MERGED | `test_engine_lock.c` + REPL + Makefile merge |

**main 은 건드리지 않음** (시연 회의 + 2차 리팩토링 이후 `dev → main` 예정).

### Web 데모 자산 보존

- `web/concurrency.html` (승진 원본, 1277 줄) — 탭 네비게이션 + 탭 (c) RW Contention 폴링 UI
- `web/stress.html` (용 형님 원본에서 rename, 264 줄) — Concurrent Stress 실동작 (multi vs single)
- 2차에서 **두 페이지를 하나로 통합** 예정 (용 형님 `runBatch` JS 를 `concurrency.html` 탭 (a) panel 로 이식)

---

## 1. 실행 방법 (데몬 + 웹)

```bash
git fetch origin
git checkout dev
git pull origin dev

mkdir -p data
make clean
make
```

빌드 성공 후 아래 중 하나로 기동:

```bash
# 기본 기동
./minisqld --port 8080 --workers 8 --data-dir ./data --web-root ./web

# 다른 포트
./minisqld --port 9090 --workers 4 --data-dir ./data --web-root ./web
```

stderr 에 `[server] listening on port 8080` 표시되면 정상.
`Ctrl+C` 로 graceful shutdown (accept loop 탈출 → threadpool drain → engine shutdown).

### 브라우저 URL

| URL | 내용 |
|---|---|
| `http://localhost:8080/` | 자동 리다이렉트 → `/concurrency.html` (탭 구조 + 탭 c 폴링) |
| `http://localhost:8080/concurrency.html` | 승진 탭 UI. 탭 (a) 는 스캐폴드만 (input disabled), 탭 (c) 는 `/api/stats` 폴링 동작 |
| `http://localhost:8080/stress.html` | 용 형님 Concurrent Stress. multi vs single mode 버튼 있음 |

### curl 스모크 테스트

```bash
# 1. stats (항상 200)
curl http://localhost:8080/api/stats

# 2. CREATE TABLE
curl -X POST http://localhost:8080/api/query \
     -H "Content-Type: text/plain" \
     -d "CREATE TABLE users (id INT, name VARCHAR)"

# 3. INSERT
curl -X POST http://localhost:8080/api/query \
     -H "Content-Type: text/plain" \
     -d "INSERT INTO users (name) VALUES ('alice')"

# 4. SELECT with index
curl -X POST http://localhost:8080/api/query \
     -H "Content-Type: text/plain" \
     -d "SELECT * FROM users WHERE id = 1"

# 5. mode=single (전역 직렬화) 비교
curl -X POST "http://localhost:8080/api/query?mode=single" \
     -H "Content-Type: text/plain" \
     -d "SELECT * FROM users WHERE id = 1"

# 6. EXPLAIN (index_used / nodes_visited)
curl "http://localhost:8080/api/explain?sql=SELECT%20*%20FROM%20users%20WHERE%20id%3D1"
```

### REPL CLI (발표 백업 시연용)

```bash
make repl                     # ./minisqld-repl 빌드
./minisqld-repl               # localhost:8080 자동 연결
# 프롬프트에서:
#   SELECT * FROM users
# 내장 명령:
#   \h   (도움말)
#   \s   (GET /api/stats)
#   \e <SQL>  (EXPLAIN)
#   \single on|off  (직렬화 토글)
#   \q   (종료)
```

---

## 2. 팀원별 체크리스트 (2차 착수 전)

### 공통 (전원)

- [ ] `git fetch origin && git checkout dev && git pull origin dev`
- [ ] `make clean && make` — 경고 0
- [ ] `make test` — 회귀 전부 통과 (parser 208 + executor + storage + engine_lock + threadpool + index_registry)
- [ ] 위 브라우저 URL 3 개 접속 확인 (concurrency 탭 구조 / stress 비교 UI / `/api/stats` JSON)
- [ ] 위 curl 6 개 중 최소 1~5 실행 (6 은 index 준비 후)

---

### 🔧 동현 — `feature/engine-threadsafe`

- [ ] `SELECT / INSERT / UPDATE / DELETE / CREATE` 각 쿼리 curl 로 돌려서 JSON 응답 확인
- [ ] SELECT 100 동시 (`xargs -P` 등) 로 멀티 vs `?mode=single` 시간 차이 육안 확인
- [ ] `/api/explain` 으로 `index_used: true`, `nodes_visited` 값이 실제로 들어가는지 확인
- [ ] 2차 결정 필요 항목:
  - `/api/inject` 는 현재 용 형님 router 에서 501 반환. engine 이 담당할지, router 에 간단히 실장할지
  - `engine_explain` 의 JSON 포맷 (현재 응답 확인 후 UI 에 맞춰 보강할지)
- [ ] 부하용 스크립트 (scripts/ 에 간단 bash) 제안 — TSan 빌드에서 돌려볼 수 있도록

### 🌐 용 형님 — `feature/server-protocol`

- [ ] `stress.html` 에서 "Run" 눌러 실제로 요청 나가는지 / multi vs single 숫자 차이 나는지 확인
  - 테이블이 비어있으면 runBatch 결과가 공허할 수 있음 → `curl` 로 INSERT 먼저 넣어둘 것
- [ ] `serve_static` 이 탭 HTML 외에 CSS/JS (있다면) 도 서빙하는지 확인
- [ ] **2차 리팩토링 핵심 작업:**
  - `stress.html` 의 `runBatch` JS 로직을 `concurrency.html` 탭 (a) panel 안으로 이식
  - 스타일 통일 (승진 dark 테마 기반으로 용 형님 컴포넌트 재스타일)
- [ ] `router_set_web_root()` 내부 API — `include/router.h` 에 올릴지 static 유지할지 PM 과 상의
- [ ] `/api/inject` 501 → 실구현 (더미 주입) 담당 결정

### ⚙️ 승진 — `feature/threadpool-stats`

- [ ] 탭 (c) 에서 `/api/stats` 폴링 응답이 실제 `active_workers` / `queue_depth` 반영하는지 확인 (부하 중 숫자 변동 확인)
- [ ] 탭 (c) "요청 로그" 섹션이 실제 요청 결과로 채워지는지 end-to-end 테스트
- [ ] 2차 결정 필요 항목:
  - `stats.c` 분리 모듈을 새로 팔지, 현재 `engine_get_stats` + threadpool API 조합으로 유지할지
  - `qps_last_sec` (초당 QPS) 계산 로직 추가 여부 — sliding window atomic counter (10~20 줄)
- [ ] 탭 (c) 의 "read 100 vs write 100" 의 구체 시나리오 정의 (어떤 SQL 로 부하 줄지)

### 🧑‍💻 지용 (PM) — `feature/pm-infra`

- [ ] **시연 회의 어젠다 확정** (아래 §3)
- [ ] 2차 담당 배분 (아래 기본안):
  - 탭 통합 (용 형님) / stats 보강 (승진) / inject 실구현 (동현 or 용 형님)
  - 통합 테스트 스크립트 (PM)
- [ ] `chore/ci-speedup` 미룬 과제 — 2차 착수 직전 또는 병행 처리
- [ ] 발표 측정 환경 고정 (devcontainer 기준 스펙 기록)

---

## 3. 2차 리팩토링 회의 어젠다 (PM 주관)

회의 전에 아래 6 개 항목을 결정해두면 2차 작업이 병렬 진행 가능.

1. **UI 통합 방식**
   - (A) 단일 `concurrency.html` 에 탭 2 개 — 승진 구조 + 용 형님 runBatch 이식
   - (B) 2 페이지 유지 + 상단 nav 링크

2. **데모 대상 테이블**
   - `users` / `orders` / `payments` 중 어느 것
   - 스키마 확정 (컬럼 타입 + 인덱스 대상 컬럼)

3. **더미 데이터 규모**
   - 1 만 / 10 만 / 100 만
   - 주입 경로: 브라우저 `/api/inject` vs 시작 시 CSV 사전 배치

4. **측정 지표** (발표에서 강조할 숫자)
   - QPS (초당 쿼리 수)
   - latency p50 / p95 / p99
   - 총 실행 시간 (before/after 막대)
   - 위 중 1~2 개 선택

5. **발표 스토리 수치**
   - "N 배 빨라짐" 의 기준 지표 1 개 확정
   - W7 의 1842× (자료구조 pure) 와 어떻게 연결할지 멘트

6. **실패 시나리오 백업**
   - 발표 중 서버 죽으면: REPL 로 계속 / 녹화 재생 / 수치만 구두로
   - 리허설에서 1 회 "서버 강제 kill → 재기동" 연습

---

## 4. 알려진 이슈 / 폴더 남아있는 것들

- `web/concurrency.html` 탭 (a) 는 **스캐폴드만** (input disabled). 2차에서 `runBatch` 이식 필요
- `web/stress.html` 은 별도 페이지로 잠시 존재. 2차 이식 후 삭제 예정
- `/api/inject` 는 501 ("not_implemented, outside server-protocol scope"). 2차에서 구현
- `stats.c` 전용 모듈 없음. `engine_get_stats` 경유. 필요시 2차에서 분리
- `qps_last_sec` 등 시간축 stats 미구현
- `CFLAGS` 에 `-D_POSIX_C_SOURCE=200809L` 붙어있음 — strict C11 에서 strdup 등 쓰기 위함. 제거 금지
- CI 의 apt `toolchain install` 단계가 4 분+ 걸리는 경우 있음 → `chore/ci-speedup` 브랜치로 **build/test/tsan** 잡에서 apt 단계 제거 예정 (2차 병행)

---

## 5. 브랜치 상태

```
main                                 ← 건드리지 않음 (MP0 + 4 PR 이전 상태)
dev                                  ← 1차 mix-merge 완료, 모든 PR 통합됨
feature/engine-threadsafe            (PR #4 merged, 브랜치 보존)
feature/server-protocol              (PR #2 merged, 브랜치 보존)
feature/threadpool-stats             (PR #5 merged, 브랜치 보존)
feature/pm-infra                     (PR #1 merged, 브랜치 보존)
docs/w8-handoff                      (이 문서 PR 용)
```

feature 브랜치는 2차 리팩토링 완료 후 PM 이 일괄 정리 예정.

---

## 6. 2차 리팩토링 착수 순서 (권장)

1. **PM**: 회의 진행 → §3 어젠다 6 항목 결정 → 담당 배분
2. **전원**: dev 에서 새 branch 따서 작업 시작 (예: `feature/refactor-web-integration`, `feature/refactor-stats`, `feature/refactor-inject`)
3. **PM**: 어젠다 결정 직후 **핀 공지** (슬랙) — 모든 팀원이 같은 스키마/수치 기준으로 작업
4. **각 담당**: PR → dev → 1차와 동일 flow (PM self-merge 가능)
5. **PM**: 2차 PR 통합 후 **dev → main PR** 최종 배포
6. **전원**: 리허설 + README 수치 확정 → 발표

---

_1차 mix-merge: 2026-04-22. 작성: PM + Claude Opus 4.7._
