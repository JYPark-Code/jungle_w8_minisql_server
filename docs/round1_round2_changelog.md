# W8 Round 1 / Round 2 변경 이력 정리

> 커밋 히스토리 기반으로 1차 (mix-merge 완성) 와 2차 (리팩토링 완성) 의
> 변경 범위 · 발생 이슈 · 결정 근거를 한 문서에 정리.
>
> 관련 문서:
> - `w8_handoff.md` — 1차 mix-merge 직후 인수인계 스냅샷
> - `CLAUDE.md § 2차 리팩토링 결정사항` — Round 2 설계 4 항목
> - `docs/round2_integration_map.md` — Round 2 zone 경계 (engine.c / router.c)
> - `TEAM_RULES.md §10` — Round 2 작업 소유권

---

## 0. 타임라인 요약

| 구간 | 기간 | 대표 커밋 | 상태 |
|---|---|---|---|
| Round 0 (W7 인계) | ~2026-04-16 | `137d04a` W6/W7 자산 아카이빙 | B+Tree 엔진만 존재, HTTP 계층 없음 |
| Round 1 착수 ~ mix-merge | 2026-04-22 | `5bd8cbc` 헤더 → `3eb2886` 실측 반영 | 1차 완성 (HTTP 데몬 + 고정 threadpool) |
| Round 2 설계 | 2026-04-22 | `6846a26` 2차 결정사항 → `fd85217` cache pivot | 설계 확정 |
| Round 2 구현 | 2026-04-22 | `1861f3d` `5867339` `f8e0b62` `4a74de7` `1097361` | 4 개 축 병렬 |
| Round 2 안정화 | 2026-04-22 ~ 2026-04-23 | `fee676d` → `2bb9296` | 운영 이슈 대응 |

---

## 1. Round 1 — 1차 mix-merge 완성

### 1.1 목표
W7 에서 완성된 B+Tree 단일 프로세스 DB 엔진 위에, **pthread 외부 의존 없이**
HTTP API 서버를 얹어 외부 클라이언트가 SQL 을 실행할 수 있게 한다.

### 1.2 레이어별 변경 (커밋)

| 레이어 | 커밋 | 내용 |
|---|---|---|
| 공개 헤더 | `5bd8cbc` | `include/` 4 개 레이어 인터페이스 헤더 5 종 추가 (계약 고정) |
| Engine lock | `6bb6d73` | 테이블 RW + catalog + single mode 동시성 레이어 |
| Main 엔트리 | `68c386f` | CLI 파싱 + SIGINT + `server_run` 데몬화 |
| Build | `9694564`, `efd5c8c` | Makefile 재작성 + 4 잡 병렬 CI (build/test/tsan/valgrind) |
| Stub 링크 | `6072201` | 팀원 4 영역 stub `.c` 로 MP0 링크 통과 |
| Engine 어댑터 | `46e1e8d` | `engine_exec_sql` — raw SQL → statement 분리, JSON 직렬화, single mode 직렬화 |
| HTTP 프로토콜 | `6d902f9` | GET/POST 파서 + `Connection: close` writer + router dispatch |
| Thread pool | `471a0cf` | mutex + condvar FIFO queue, 고정 N 워커, shutdown drain |
| 테스트 + REPL | `15eb0b4` | engine_lock 단위 테스트 8 종 + ANSI REPL 클라이언트 |
| FE 통합 | `8e411b5` | `stress.html` 을 `concurrency.html` 탭 (a) 에 통합 |
| 인수인계 | `a14f4f7`, `3eb2886` | `w8_handoff.md` + 1차 mix-merge 실측 수치 반영 |

### 1.3 1차 완성 시점의 아키텍처

```
Client ──HTTP──> Thread Pool (고정 N=8) ──> Router ──> Engine (engine_lock) ──> B+Tree / Storage
```

- 캐시 없음
- Trie 없음 (prefix 검색 불가)
- 모든 읽기 경로가 engine 직행

### 1.4 1차 종료 직후 남은 문제 (→ Round 2 의 입력)

`w8_handoff.md` + `3eb2886` 커밋에서 식별된 한계:

1. **고정 풀의 부하 대응 한계.** N=8 고정은 부하 spike 에서 queue 만 늘고 latency 급등, idle 시 메모리/스레드 고정 점유.
2. **브라우저 기반 stress 왜곡.** per-origin 동시 연결 6 제한 + `Connection: close` 로 서버 멀티 이득이 희석. 서버 버그가 아닌 브라우저 제약임을 README 에 명시.
3. **사전 유스케이스의 근본 문제**: prefix 검색이 B+Tree 로 구조적 불가. autocomplete 가 전무.
4. **hot word 반복 조회 비용**: 동일 요청이 매번 파싱 + storage 탐색 + JSON 직렬화.
5. **FE 의 발표 임팩트 약함**: dark theme stress 페이지는 기술 지표는 있으나 일반 관객 설득력 부족.

---

## 2. Round 2 — 2차 리팩토링 완성

Round 1 의 5 가지 한계에 대응하는 4 개 축을 병렬 진행.

### 2.1 4 개 축의 전·후 비교

| 축 | Round 1 상태 | Round 2 완료 상태 | 핵심 커밋 |
|---|---|---|---|
| (1) Thread pool | 고정 N=8, unbounded queue | **동적 4 → 16** auto-resize + **bounded queue 256** + graceful shutdown | `5867339`, `2bb9296` |
| (2) Dict cache | 없음 | **router-level LRU + rwlock**, `/api/dict` 전용, invalidate-on-write | `f8e0b62`, `4b0b887` |
| (3) Trie prefix | 없음 (B+Tree 는 prefix 불가) | **ASCII a-z Trie** — `english` 컬럼 인덱스 + `/api/autocomplete` | `1861f3d`, `4a74de7` |
| (4) FE 재설계 | dark theme stress 페이지 | **영한 사전 중심 UI** — Dictionary / Engine Lab / Analytics Live 3 섹션 | `1097361` |

### 2.2 축별 설계 근거

#### (1) 동적 쓰레드풀 (`5867339`, `2bb9296`)

- **왜**: 고정 N 은 peak 대응과 idle 자원 절약이 양립 불가.
- **설계**:
  - core=4, cap=16. utilization ≥ 0.8 이 3 회 연속 관측되면 +4 확장.
  - 이번 라운드는 **scale-down 도 구현 포함** — utilization ≤ 0.3 이 30 초 지속 시 -4 (floor = create 초기값). 원안은 "구현 난이도 보고 결정" 이었으나 hysteresis (2 샘플 무시) 로 flapping 막으면 안전하다고 판단해 포함.
  - **슬롯 pre-alloc (TP_ABS_MAX=64)** — resize 시 realloc race 회피.
  - graceful shutdown: `submit_closed` **먼저** 세팅 → monitor 정지 → queue/active drain 대기 (timeout_ms) → 실패 시 강제 종료.
- **추가 변경 (`2bb9296`)**: unbounded queue 는 OOM 직전까지 쌓여 accept loop 가 자기강화하는 문제. **cap 256 bounded + 503 fail-fast backpressure** 로 전환. `Retry-After: 1` + `{"ok":false,"error":"queue_full"}` 명시.
- **기각한 대안**:
  - *blocking submit*: backpressure 를 TCP backlog 로 미룰 뿐, client 에는 동일 hang.
  - *drop-oldest*: 버린 fd 의 client 는 무한 대기 — HTTP 에 부적절.

#### (2) Router-level dict cache (`f8e0b62`, `4b0b887`)

- **설계 pivot (`fd85217`, 2026-04-22)**: 초안의 "엔진 내부 query cache" 를
  **router 레벨 `/api/dict` 전용 cache** 로 축소.
  - **왜**: 사전 서비스는 엔드포인트 1 개가 hot path. 범용 query cache 는 SQL
    normalize / lifetime / race window 오버헤드만 크고 이득이 없음.
  - **효과**: engine.c 핵심 경로 (parse → execute → storage) 가 캐시를 모르
    므로 동현/용 engine.c overlap 이 제거됨 (`round2_integration_map.md` 반영).
- **모듈 경계**: `src/dict_cache.c` + `src/dict_cache.h` (**private 헤더 —
  router.c 만 include**). engine.c / server.c 에서 `dict_cache_*` 호출 금지 (TEAM_RULES §10-3).
- **동시성**:
  - 초기 구현은 단일 mutex. 측정 결과 **cached 531ms > nocache 106ms 로 역전** (동일 단어 100 병렬).
  - `4b0b887` 에서 **rwlock + atomic LRU** 로 전환 — get 은 rdlock 다중 reader 병렬, LRU 타임스탬프/hit/miss/clock 은 `atomic_ulong`. **cached ~105ms ≤ nocache ~370ms 로 5× 정상화.**
- **일관성**: invalidate-on-write. `/api/admin/insert` 성공 시 해당 key invalidate (보수적 fallback 으로 `invalidate_all` 도 허용).
- **기각한 대안**: write-through — 매 INSERT 마다 캐시 prefill 이면 락 구간 길어지고 read 단축 효과 감소.

#### (3) Trie prefix (`1861f3d`, `4a74de7`)

- **왜**: B+Tree 는 prefix 쿼리 구조적 불가 (exact / range 만). autocomplete 불가능.
- **설계**:
  - 표준 trie, 노드당 **자식 26 개 고정 배열** (a-z). compressed trie 는 범위 외.
  - 대상: `dictionary.english` 만. 한글 body 는 인덱스 없음 → `?korean=` 은 선형 스캔 fallback (동작 OK / 느림).
  - 삽입 `O(k)`, 정확 매칭 `O(k)`, prefix 매칭 `O(k + m)` with `max_out`.
  - 동시성: 1 차 구현은 engine_lock 의 테이블 wrlock 에 의존. 세분화 락은 향후.
  - INSERT/UPDATE/DELETE 성공 후 trie 전체 rebuild (`dictionary_trie_refresh_after_write`). 단순 정책 — 단어 수 증가 시 증분 삽입으로 교체 검토.
- **입력 검증**: non-ASCII / 대문자 / 숫자 / 기호 prefix 는 `/api/autocomplete` 핸들러에서 400.

#### (4) FE 재설계 (`1097361`)

- **왜**: 기술 지표만 있는 dark stress 페이지는 채용담당자/일반 관객 기준 인상이 약함.
- **설계**: 영한 사전을 데모 유스케이스로. Dictionary + Engine Lab + Analytics Live 3 섹션 통합. `/api/stats` 1s 폴링으로 cache hits/misses, lock_wait_ns, total_queries 실시간 표시.
- **autocomplete 는 PR 머지 순서 때문에 mock → real 2 단 전환**: 초기엔 `USE_REAL_AUTOCOMPLETE=false`, `4a74de7` (trie endpoint) + `fee676d` (warming gate) 머지 후 `0c0280a` 에서 true flip.

### 2.3 신규 HTTP API (Round 2 추가분)

| 엔드포인트 | 캐시 | 인덱스 | 설명 |
|---|---|---|---|
| `GET /api/dict?english=<w>` | dict cache | B+Tree | 영한 primary (hot path) |
| `GET /api/dict?id=<N>` | dict cache | B+Tree | id 조회 |
| `GET /api/dict?korean=<w>` | **미적용** | 없음 | 한글 body 역방향, 선형 스캔 |
| `GET /api/autocomplete?prefix=<p>` | 미적용 | Trie | ASCII 소문자만, prefix 조합이 많아 hit rate 낮음 |
| `POST /api/admin/insert` | invalidate | B+Tree + Trie | 성공 시 `english:<w>` invalidate + trie rebuild |

---

## 3. Round 2 에서 발생한 이슈와 대응

구현 중 새로 튀어나온 문제들. 각각의 커밋은 보존 가치가 높은 "왜 이렇게 고쳤는가" 의 기록.

### 3.1 서버 warming race (`fee676d`)
- **증상**: 서버가 예열 전에 트래픽을 받으면 다운. init/shutdown race.
- **대응**: `engine_is_ready()` 도입 + 사전 엔드포인트 4 개에 **503 `warming_up` 게이트**. SIGINT 경로에 `threadpool_shutdown_graceful(5s)` drain 추가.

### 3.2 Chicken-and-egg 빈 DB 503 (`2949d4e`)
- **증상**: 빈 DB 상태에서 `/api/dict` 는 503, insert 도 503 이라 데이터로 뚫을 수 없음.
- **대응**: `engine_init` 에서 dictionary 스키마 부재 시 **빈 테이블 자동 CREATE**.

### 3.3 Cache 역전 (`4b0b887`, closes #24)
- **증상**: 단일 mutex 경합으로 **cached (531ms) > nocache (106ms)**. Round 2 통합 회의에서 P3 로 격상.
- **대응**: 위 § 2.2 (2) 의 rwlock + atomic LRU 전환. 5× 개선.

### 3.4 UI timeout 오탐 (`643c134`)
- **증상**: writer 가 wrlock 대기 중일 때 FE 8s timeout 에 걸려 UI 가 실패로 오탐. 서버 쪽은 timeout 없음.
- **대응**: `QUERY_TIMEOUT_MS` **8s → 60s** 로 정합성 회복.

### 3.5 FE 응답 파서 불일치 (`d505f1c`, `9312f5a`)
- **증상**: `extractRows` 가 `engine.columns/rows` 를 찾는데 실제 응답은 `engine.statements[0]` 구조. 검색 결과가 항상 빈 배열.
- **대응**: `engine.statements[0]` 경로 수정 + legacy fallback 추가.

### 3.6 캐시 히트 편향 (`a8f6b4e`, `7a882a1`, `8cded49`)
- **증상**: Stress 테스트가 동일 단어 N 회 호출 구조라 캐시 켜져 있으면 첫 요청 외 전부 hit. 서버 병렬 특성이 안 드러남.
- **대응 3 종**:
  - Stress UI URL 에 `&nocache=1` (`a8f6b4e`).
  - cache hit 순수 측정값 `cache_lookup_ms` 를 응답에 분리 노출 (`7a882a1`).
  - R/W Contention 테스트도 cache hit 우회 — 안 그러면 read/write lock 경쟁이 실제로 안 생김 (`8cded49`).

### 3.7 Queue OOM 자기강화 (`2bb9296`)
- **증상**: unbounded queue 가 부하 spike 에 calloc 실패 직전까지 쌓임. 그 시점에 서버 flat + accept loop 가 fd 를 계속 밀어넣음.
- **대응**: bounded queue (cap 256 = 16 워커 × 16 pending) + 503 fail-fast. 이유는 § 2.2 (1) 의 대안 기각 표와 동일.

### 3.8 Dictionary 데이터 품질 (`fc3c3b5` → `f1b0da9`)
순차적으로 데이터 품질이 개선된 경로:
- `fc3c3b5` kengdic 기반 최초 로드 (86k 행).
- `0baec9d` compound sentence 덩어리 정제 → 77k.
- `b668116`, `7eefa25` 번역 표현 정제.
- `70d3bb3` 77k + 100k 병합 + english normalize → 155k. kengdic 100k 는 Wikipedia 원본 스타일 (공백/대문자/기호/숫자 혼입) 이라 trie 규약 (a-z lowercase only) 에 맞춰 정제 필수.
- `f1b0da9` **NIKL 한국어기초사전 기반 재생성** — 출처 품질 향상 (최종 데모용).

---

## 4. Round 2 에 남긴 향후 과제

`CLAUDE.md § 알려진 이슈` 와 일치. 정리:

- **Thread pool scale-down hysteresis 세밀화**: Java `keepAliveTime` 유사 간소 모델로 동작하지만 burst 패턴 다양화 시 tuning 필요.
- **Trie 세분화 락**: 현재 engine_lock 테이블 wrlock 의존. read 다수 / write 소수라 당장 병목은 아니지만 RCU / per-node mutex 검토.
- **한글 body 역방향 인덱스 부재**: `?korean=` 은 선형 스캔. 10 만 행 기준 수십~수백 ms. 3 차 과제.
- **Unicode NFC / 초성 검색**: Trie ASCII-only 라 한글 정규화 이슈 자체 미발생. 한글 autocomplete 하려면 별도 인덱스 필요.
- **HTTP Keep-alive**: 요청당 TCP 신규. 브라우저 6 제한에 걸려 브라우저 stress 는 서버 멀티 이득 희석 — 부하 측정은 `xargs -P` / REPL 경유 필수.
- **CI apt 오버헤드**: `787965e` 로 build/test/tsan 잡 apt 제거 완료. valgrind 잡만 남음.

---

## 5. 부록: 커밋 → 섹션 매핑

빠른 추적용.

```
Round 1 (완성):
  5bd8cbc  include/ 공개 헤더 5 종
  6bb6d73  engine_lock
  68c386f  main 데몬
  9694564  Makefile
  efd5c8c  CI 4 잡
  6072201  4 영역 stub
  46e1e8d  engine 어댑터
  6d902f9  HTTP router
  471a0cf  threadpool (고정)
  15eb0b4  engine_lock 테스트 + REPL
  8e411b5  FE 탭 통합
  a14f4f7  handoff 문서
  3eb2886  실측 반영

Round 2 (설계):
  6846a26  결정사항 4 항목
  057c501  TEAM_RULES §10
  66ddde6  Integration Map
  fd85217  cache pivot (engine → router)
  06b1e9f  dict cache 분리
  36bdaa3  Trie 확정 반영

Round 2 (구현):
  1861f3d  trie engine path
  5867339  threadpool 동적 + graceful
  f8e0b62  dict cache router 연결
  4a74de7  trie router endpoints
  1097361  FE 영한 사전 재설계

Round 2 (안정화):
  787965e  CI 경량화
  fee676d  warming gate + nocache + fixture + drain
  0c0280a  autocomplete real flip
  4b0b887  dict_cache rwlock + atomic LRU (P3)
  643c134  QUERY_TIMEOUT 8s → 60s
  2949d4e  dictionary 자동 생성
  d505f1c  extractRows 수정
  9312f5a  extractRows legacy fallback
  a8f6b4e  stress nocache=1
  7a882a1  cache_lookup_ms 분리
  8cded49  R/W Contention cache 우회
  2bb9296  bounded queue + 503 backpressure
  fc3c3b5 → f1b0da9  dictionary 데이터 개선
```
