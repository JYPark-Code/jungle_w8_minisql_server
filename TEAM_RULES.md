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
- ❌ self-merge (본인 PR 을 본인이 approve/merge)
- ❌ CI 실패 상태로 merge
- ❌ `include/*.h` PM 승인 없이 수정
- ❌ 외부 HTTP/JSON 라이브러리 추가
- ❌ W7 엔진 코드 수정 (동현만 가능)
- ❌ `prev_project/`, `agent/` 수정

---

## 9. 체크포인트 (PM 용)

MP0 완료 전에 PM 이 확인:

- [ ] GitHub 에서 `main` 브랜치 보호 규약 설정 완료
- [ ] GitHub 에서 `dev` 브랜치 생성 및 보호 규약 설정 완료
- [ ] `.github/workflows/ci.yml` 에 4 개 잡(build/test/tsan/valgrind) 정의
- [ ] `dev` 보호 규약에 "Require status checks: build, test, tsan, valgrind" 등록
- [ ] 팀원 4 명 feature 브랜치 생성 완료
- [ ] 이 `TEAM_RULES.md` 를 슬랙에 공유
