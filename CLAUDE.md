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
