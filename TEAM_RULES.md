# TEAM_RULES.md — 브랜치 전략 · 보호 규약 · CI

> 이 파일은 W8 라운드의 **팀 운영 규약**을 정의합니다.
> 모든 팀원은 작업 시작 전 이 파일을 숙지해야 하며,
> 여기 정의된 규약은 `CLAUDE.md`, `agent.md` 의 일반 규칙보다 **우선**합니다.

---

## 1. 브랜치 전략

### 1-1. 구조

```
main                                    (배포/발표 기준)
 └── dev                                 (통합 브랜치)
      ├── feature/engine-threadsafe      (동현)
      ├── feature/server-protocol        (용 형님)
      ├── feature/threadpool-stats       (승진)
      └── feature/pm-infra               (지용, 선작업 및 REPL)
```

### 1-2. 흐름

1. **팀원 작업**: `feature/*` → 본인 작업 → PR → `dev`
2. **PM mix-merge**: `dev` 에서 통합, 테스트, 충돌 해결
3. **최종 머지**: 발표 직전 `dev` → `main` 1회

### 1-3. 브랜치 생성 규칙

- 브랜치 이름은 **PM 이 사전 생성**. 팀원이 임의로 새 브랜치 만들지 말 것
- 추가 브랜치 필요시 PM 에게 DM 요청
- 이름 패턴: `feature/<영역>-<키워드>` (kebab-case)

### 1-4. 커밋/푸시 규칙

- 본인 `feature/*` 브랜치에만 직접 push 가능
- `main`, `dev` 직접 push 금지 (보호 규약 참조)
- 본인 브랜치라도 **force push 금지**. 부득이하면 `--force-with-lease` 만 허용

---

## 2. 브랜치 보호 규약

### 2-1. `main` 브랜치 (Protected)

GitHub 설정 기준:

| 항목 | 설정 |
|---|---|
| Require pull request before merging | ✅ |
| Require approvals | **1 명** (PM) |
| Dismiss stale approvals on new commits | ✅ |
| Require status checks to pass | ✅ (CI 전체 통과) |
| Require branches to be up to date | ✅ |
| Require conversation resolution before merging | ✅ |
| Require linear history | ✅ (merge commit 금지, rebase/squash 만) |
| Do not allow bypassing | ✅ (관리자도 bypass 불가) |
| Allow force pushes | ❌ |
| Allow deletions | ❌ |

**누가 merge 가능?**
- 오직 PM(지용) 만, 그리고 오직 `dev` → `main` 에 한정

### 2-2. `dev` 브랜치 (Protected)

| 항목 | 설정 |
|---|---|
| Require pull request before merging | ✅ |
| Require approvals | **1 명** (PM) |
| Require status checks to pass | ✅ (build + test + tsan + valgrind) |
| Require branches to be up to date | ✅ |
| Allow force pushes | ❌ |
| Allow deletions | ❌ |

**누가 merge 가능?**
- PM(지용). mix-merge 권한은 PM 에게만
- 팀원 PR 은 반드시 PM 리뷰/approve 후 PM 이 머지 (작성자 self-merge 금지)

### 2-3. `feature/*` 브랜치 (작업 영역)

- 본인만 push 가능 (GitHub 는 별도 제한 안 걸어도 관례로 준수)
- force push 는 `--force-with-lease` 만, 가급적 피할 것
- 머지된 브랜치는 PM 이 정리 차원에서 **삭제**

---

## 3. PR 규약

### 3-1. PR 생성

- **target**: `dev` (절대 `main` 아님)
- **source**: 본인 `feature/*`
- **제목**: Angular commit type 준수 (`feat: ...`, `fix: ...`, `test: ...`)
- **본문**: 아래 템플릿 필수

### 3-2. PR 본문 템플릿

```markdown
## 변경 요약
- 한 줄 요약

## 변경 내역
- 무엇을 추가/수정/삭제했는지 bullet

## 검증
- [ ] `make` 빌드 경고 0
- [ ] `make test` 통과 (회귀 + 본인 파트)
- [ ] `make tsan` 통과 (동시성 건드린 경우)
- [ ] `make valgrind` 통과 (메모리 건드린 경우)
- [ ] `include/*.h` 인터페이스 계약 준수 (임의 수정 없음)
- [ ] 본인 파트 단위 테스트 추가됨

## 참고 / 질문
- PM 에게 확인받고 싶은 설계 결정 (있으면)
```

### 3-3. 리뷰 프로세스

1. 팀원: PR 올리고 PM 을 reviewer 지정
2. PM: CI 결과 확인 → 코드 리뷰 → approve 또는 change request
3. change request 시 팀원이 수정 후 다시 push (같은 PR 에)
4. approve 후 PM 이 squash merge (linear history 유지)

---

## 4. CI (GitHub Actions)

### 4-1. 잡 구성

`.github/workflows/ci.yml` 에 다음 잡을 **병렬 실행**:

| 잡 이름 | 트리거 | 내용 |
|---|---|---|
| `build` | PR, push to main/dev | `make` — 빌드 경고 0 확인 |
| `test` | PR, push to main/dev | `make test` — W7 회귀 227 + 본인 테스트 |
| `tsan` | PR, push to main/dev | `make tsan && ./run_tsan_tests.sh` |
| `valgrind` | PR, push to main/dev | `make valgrind` — 누수/잘못된 접근 0 |

### 4-2. 각 잡 필수 통과 조건

**`build` 잡**
- `gcc -Wall -Wextra -Wpedantic` 경고 0
- `minisqld` 바이너리 생성 성공
- 모든 테스트 바이너리 생성 성공

**`test` 잡**
- W7 회귀 테스트 227 개 전부 통과
- `test_threadpool`, `test_engine_concurrent`, `test_protocol` 통과

**`tsan` 잡** (⚠️ 이번에 신규)
- `CFLAGS += -fsanitize=thread -g -O1` 로 전체 재빌드
- 동시성 테스트 바이너리 실행
- TSan 이 race condition / deadlock 검출 시 **실패**
- 구체적 시나리오:
  - 동시 SELECT 100 × 2 초
  - 동시 INSERT 50 × 2 초
  - SELECT ↔ INSERT 혼합 50:50 × 2 초
  - `engine_shutdown` 중 in-flight 쿼리 존재

**`valgrind` 잡**
- `valgrind --leak-check=full --error-exitcode=1`
- definitely lost / indirectly lost 0
- invalid read/write 0

### 4-3. CI 실패 시 정책

- **어떤 잡 하나라도 실패하면 `dev` 머지 불가**
- CI 수정을 위해서만 FIX 커밋 허용, 무관한 변경 섞지 말 것
- 빈번한 flaky test 는 PM 에게 즉시 보고 (수정 우선순위 최상)

---

## 5. 로컬 검증 체크리스트

PR 올리기 전 반드시 로컬에서 실행:

```bash
# 1. 빌드 경고 0
make clean && make 2>&1 | grep -E "warning|error" && echo "❌ FAIL" || echo "✅ PASS"

# 2. 전체 테스트
make test

# 3. 동시성 건드린 PR 이면
make tsan && ./tsan_tests

# 4. 메모리 할당 건드린 PR 이면
make valgrind

# 5. 본인 브랜치가 dev 최신을 반영하는지
git fetch origin
git rebase origin/dev    # conflict 나면 해결 후 계속
```

로컬에서 실패한 상태로 PR 올리지 말 것. CI 낭비.

---

## 6. Makefile 빌드 타겟 요약

PM 이 MP0 에서 다음 타겟을 제공:

| 타겟 | 용도 |
|---|---|
| `make` | 기본 빌드 (`./minisqld` + 엔진) |
| `make test` | 회귀 + 동시성 단위 테스트 |
| `make tsan` | ThreadSanitizer 빌드 |
| `make valgrind` | valgrind 회귀 |
| `make bench` | B+Tree pure 벤치 (W7 자산) |
| `make loadtest` | 동시 N 요청 부하 테스트 |
| `make clean` | 빌드 산출물 삭제 |

---

## 7. 긴급 상황 대응

### 7-1. CI 가 flaky 하게 실패할 때
- TSan 은 타이밍 민감. 일시적 실패 시 `re-run jobs` 먼저
- 2 회 연속 실패 시 진짜 race 일 가능성 → PM 에게 즉시 보고

### 7-2. `dev` 머지 후 `main` 상태 불안정
- PM 이 `dev` 에서 hotfix 후 재머지
- 발표 30 분 전에는 새 PR 받지 않음 (코드 프리즈)

### 7-3. 본인 브랜치 꼬였을 때
- 임의로 force push 하지 말 것
- PM 에게 DM 후 같이 해결
- 최악의 경우 브랜치 리셋: `git reset --hard origin/dev` 후 본인 변경 cherry-pick

---

## 8. 금지 사항 요약

- ❌ `main`, `dev` 에 직접 push
- ❌ force push (`--force-with-lease` 예외)
- ❌ self-merge — **단서**: Round 2 현재 PM (admin) 은 `dev` 의 `enforce_admins=false` 덕분에 본인 PR self-merge 가능. 일반 팀원 PR 은 여전히 PM approve 필요
- ❌ CI 실패 상태로 merge
- ❌ `include/*.h` PM 승인 없이 수정 (단, **새 모듈 헤더** (예: Round 2 의 `include/cache.h`, `include/trie.h`) 는 해당 담당자가 생성. 기존 5 개 헤더 수정은 여전히 PM 승인)
- ❌ 외부 HTTP/JSON 라이브러리 추가
- ❌ W7 엔진 코드 수정 (동현만 가능)
- ❌ `prev_project/`, `agent/` 수정
- ❌ **Round 2 소유권 매트릭스 (§10-1) 의 "건드리면 안 됨" 열 위반**.
  교차 수정 필요 시 `§10-3` 승인 절차 경유

---

## 9. 체크포인트 (PM 용)

MP0 완료 전에 PM 이 확인:

- [x] GitHub 에서 `main` 브랜치 보호 규약 설정 완료
- [x] GitHub 에서 `dev` 브랜치 생성 및 보호 규약 설정 완료
- [x] `.github/workflows/ci.yml` 에 4 개 잡(build/test/tsan/valgrind) 정의
- [x] `dev` 보호 규약에 "Require status checks: build, test, tsan, valgrind" 등록
- [x] 팀원 4 명 feature 브랜치 생성 완료
- [x] 이 `TEAM_RULES.md` 를 슬랙에 공유

---

## 10. Round 2 작업 소유권 + 교차 파일 수정 규약

> Round 2 는 4 개 신기능 (동적 TP / LRU 캐시 / Trie / FE 재디자인) 이 병렬
> 진행. 파일 경계 규칙을 명확히 해서 mix-merge 충돌과 설계 권한 분쟁을
> 사전에 차단한다. Round 1 의 `§1 브랜치 전략` 과 `§8 금지 사항 요약` 을
> 우선하고, 본 섹션이 Round 2 에 한해 덧붙이는 규칙이다.

### 10-1. 소유권 매트릭스 (2026-04-22 pivot 반영)

> 동현 의 cache 가 engine 내부 → router 레벨로 이동하면서 매트릭스 갱신.
> engine.c 는 용 단독 편집 영역이 되고, router.c 는 동현 ↔ 용 endpoint
> 단위 overlap 으로 남는다.

| 담당 | 브랜치 | 주 소유 파일 | 교차 허용 (PM review 필수) | 건드리면 안 됨 |
|---|---|---|---|---|
| 지용 (Lead, TP) | `feature/dynamic-threadpool` | `src/threadpool.c`, `include/threadpool.h` (API 추가), `src/server.c` (graceful shutdown 연계), `tests/test_threadpool.c` 보강 | `src/main.c` (signal handler 보강) | `src/cache.c`, `src/trie.c`, `src/engine.c`, `src/router.c`, `web/` |
| 동현 (Cache) | `feature/lru-cache` | `src/cache.c` (신규), `include/cache.h` (신규), `src/router.c` 의 Zone R-DICT-CACHE / R-STATS / R-INIT, `tests/test_cache.c` (신규) | — (engine.c 는 **건드리지 않음**, server.c 는 금지) | `src/server.c`, `src/engine.c`, `src/threadpool.c`, `src/trie.c`, `web/` |
| 용 (Trie) | `feature/trie-prefix` | `src/trie.c` (신규), `include/trie.h` (신규), `src/engine.c` 의 Zone T1 + IT (prefix dispatcher + trie init/teardown), `src/router.c` 의 Zone R-AUTO / R-ADMIN-INSERT, `tests/test_trie.c` (신규) | (없음) | `src/server.c`, `src/threadpool.c`, `src/cache.c`, `web/` |
| 승진 (FE) | `feature/ui-redesign` | `web/` 전체 | — (백엔드 무수정) | `src/`, `include/`, `Makefile`, `.github/` |

Zone 이름은 `docs/round2_integration_map.md` 기준. 본 문서와 불일치하면
통합 맵 문서가 우선.

### 10-2. 병렬 진행 허가 (승진 FE)

- 승진의 FE 작업은 **백엔드 구현 완료를 기다릴 필요가 없다**. 다음 3 가지
  근거:
  1. `web/` 만 건드리므로 백엔드 파일 경계 침범 0
  2. `CLAUDE.md § 모듈 간 인터페이스 (d) FE ↔ Server (HTTP API)` 의
     엔드포인트 스펙이 TBD 초안으로 확정되어 있음 → **계약** 으로 간주하고
     mock fetch 로 시작 가능
  3. 기존 `/api/query` 는 Round 1 머지 결과 정상 동작 → dictionary 기본
     조회는 즉시 E2E 테스트 가능
- 자동완성 (`/autocomplete`) 과 관리자 등록 (`/admin/insert`) 은 구현 전
  stub 응답으로 UI 만 먼저 완성. 엔드포인트 실제 연결은 mix-merge 시점
  에서 fetch URL 1 줄 수정으로 끝난다

### 10-3. 교차 파일 수정 승인 절차

- **원칙**: "본인 담당 외 파일은 수정 금지" (Round 1 `§8` 유지). 아래는
  Round 2 에서 미리 **합의 완료** 된 예외.
- **허용된 교차 수정** (위 매트릭스 "교차 허용" 열):
  - PR 본문에 *어느 파일의 어느 함수를 몇 줄 수정했는지* 명시
  - PM 이 리뷰에서 범위 확인 (허용 범위 초과 시 request changes)
- **사전 합의 안 된 추가 교차 수정이 필요해지면**:
  1. DM 으로 PM 에게 요청 (Slack / 이 레포 이슈 중 택 1)
  2. **구현 시작 전** 승인 받을 것 (PR 올리고 나서 승인 요청 지양)
  3. 승인 내용은 PR 본문 "참고 / 질문" 섹션에 기록
- **거절 / 확정 예시 (Round 2 회의 결정)**:
  - ❌ 동현 가 `src/server.c` 에 cache 라이프사이클 훅을 직접 추가하는 것
    → 기각. cache 는 router 소유이며 `pthread_once` 기반 lazy init 또는
    router.c 파일 scope static 으로 초기화. server.c 의 SIGINT 경로
    (`claude_jiyong.md § mix-merge Zone 4`) 에 race 도입 방지 목적
  - ❌ 동현 가 `src/engine.c` 에 cache_get / cache_put 을 직접 삽입하는 것
    → 기각 (2026-04-22 회의). 엔진 핵심 경로를 캐시 로직으로 오염시키지
    않기 위함. 캐시는 router 의 `/api/dict` 핸들러 안에만 존재
  - ✅ 동현 가 `src/router.c` 의 `/api/stats` 핸들러에 `cache_hits/misses`
    필드를 추가하는 것 → 허용 (Zone R-STATS). endpoint 1 개 응답 확장이라
    용 의 신규 endpoint 추가와 conflict 낮음

### 10-4. 의도된 overlap (mix-merge 포인트)

2026-04-22 cache pivot 으로 `src/engine.c` overlap 은 **소멸**. 남은
overlap 은 `src/router.c` 만.

| 지점 | 겹치는 두 사람 | 실제 충돌 크기 |
|---|---|---|
| `src/router.c` | 동현 (Zone R-DICT-CACHE / R-STATS / R-INIT) ↔ 용 (Zone R-AUTO / R-ADMIN-INSERT) | **낮음** — 같은 함수 `router_dispatch` 안에 서로 다른 `if (strcmp(path, ...))` 블록을 추가. git 대부분 자동 병합 |
| `src/engine.c` (Zone IT) | 용 단독 | overlap 없음. trie 초기화만 |
| `CLAUDE.md § 모듈 간 인터페이스` 의 TBD 시그니처 | 4 명 전원 | 구현 시작 전 각자 리뷰 → 동의 시 PR 에 `interface-signed-off: <name>` 태그 본문에 기재 |

mix-merge 순서는 `docs/round2_integration_map.md § 3` 참조. 핵심:
PM TP → 용 trie (engine.c 변경) → 동현 cache (router.c 변경) → 용
router 신규 endpoint → 승진 UI fetch 교체.

### 10-5. Round 2 brach 이름 규약

- 패턴: `feature/<역할>-<핵심키워드>` (kebab-case)
- 제안:
  - `feature/dynamic-threadpool` (지용)
  - `feature/lru-cache` (동현)
  - `feature/trie-prefix` (용)
  - `feature/ui-redesign` (승진)
- 실제 생성은 Round 1 과 동일하게 PM 이 사전 생성 후 공유

### 10-6. Round 2 PR 본문 추가 체크리스트

Round 1 `§3-2` 의 기본 템플릿에 **Round 2 전용 항목** 추가:

```markdown
## Round 2 해당 체크
- [ ] `CLAUDE.md § 모듈 간 인터페이스` 의 내 모듈 TBD 시그니처가 실제
      구현과 일치 (불일치 시 이 PR 에서 CLAUDE.md 같이 수정)
- [ ] 본 PR 에서 교차 수정한 파일이 `TEAM_RULES.md § 10-1` 의 "교차
      허용" 범위 안에 있음. 초과 시 사전 DM 승인 기록 첨부
- [ ] 의도된 overlap 지점 (`§ 10-4`) 에 해당하면, 상대 담당자의 PR
      상태 확인 후 본인 PR 본문에 링크
```
