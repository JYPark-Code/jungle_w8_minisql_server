# MiniSQL QA / 버그 헌팅 체크리스트

> 발표 전 수동으로 돌려보고 발견된 이슈는 우측 메모란에 기록.
> 머지 전후 상태가 다를 수 있으니 어느 시점에서 돌렸는지 함께 적기.

---

## 사용법

```bash
# 빌드 한 번
make clean && make

# 단위 테스트 한 번
make test
```

체크박스 `[ ]` → `[x]` 로 진행.
실패 시 `[!]` 로 표시하고 메모.

---

## 🅰 Parser 단독 (`--debug` / `--tokens` 로 검증)

머지 시점과 무관하게 지금 즉시 가능.

| # | 입력 / 명령 | 기대 | 결과 | 메모 |
|---|---|---|---|---|
| P1 | `echo '' > /tmp/q.sql && ./sqlparser /tmp/q.sql` | 출력 없음, exit 0 | [ ] | |
| P2 | `printf -- '-- only comment\n' > /tmp/q.sql && ./sqlparser /tmp/q.sql --debug` | 출력 없음, 에러 X | [ ] | |
| P3 | `echo 'DROP TABLE x;' > /tmp/q.sql && ./sqlparser /tmp/q.sql` | stderr `unknown keyword: DROP` | [ ] | |
| P4 | `echo 'CREATE TABLE t (id INT' > /tmp/q.sql && ./sqlparser /tmp/q.sql` | stderr `expected ')'`, 크래시 X | [ ] | |
| P5 | `echo 'INSERT INTO t (a,b) VALUES (1)' > /tmp/q.sql && ./sqlparser /tmp/q.sql --debug` | 파싱은 통과 (executor 가 거부해야 함) | [ ] | |
| P6 | 중첩 따옴표: `INSERT INTO t (a) VALUES ('it''s')` | 알려진 한계 — 동작 확인만 | [ ] | |
| P7 | `echo 'SeLecT * FrOm t' > /tmp/q.sql && ./sqlparser /tmp/q.sql --debug` | 정상 SELECT | [ ] | |
| P8 | `printf 'CREATE TABLE t(x INT); INSERT INTO t(x) VALUES(1);' > /tmp/q.sql && ./sqlparser /tmp/q.sql --debug` | 두 statement 모두 처리 | [ ] | |
| P9 | `echo "INSERT INTO t (a) VALUES ('hello;world')" > /tmp/q.sql && ./sqlparser /tmp/q.sql --debug` | 한 statement | [ ] | |
| P10 | 64자 초과 컬럼명 | strncpy 잘림, 크래시 X | [ ] | |

---

## 🅱 Executor + Storage (팀원 PR 머지 후)

| # | 시나리오 | 기대 | 결과 | 메모 |
|---|---|---|---|---|
| E1 | 같은 테이블 CREATE 두 번 | 거부 또는 덮어씀 — 일관성 | [ ] | |
| E2 | 존재하지 않는 테이블 SELECT | 친절한 에러, 크래시 X | [ ] | |
| E3 | 존재하지 않는 컬럼 WHERE | 친절한 에러 | [ ] | |
| E4 | INSERT 컬럼 개수 ≠ 값 개수 | 거부 | [ ] | |
| E5 | DELETE WHERE 없음 | 정책대로 (전체 삭제 또는 거부) | [ ] | |
| E6 | UPDATE WHERE 없음 | 정책 일관성 (DELETE 와 동일) | [ ] | |
| E7 | LIMIT 0 | 결과 0 행 | [ ] | |
| E8 | LIMIT > 행수 | 전체 결과 | [ ] | |
| E9 | LIMIT -5 | 거부 또는 0행 | [ ] | |
| E10 | WHERE 매칭 0건 SELECT | 빈 결과, 에러 X | [ ] | |
| E11 | 빈 테이블 SELECT * | 빈 결과 | [ ] | |
| E12 | INSERT 1000건 후 SELECT * | 모두 보임, 메모리 안전 | [ ] | |
| E13 | DATE 비교 `'2024-01-15' > '2023-12-31'` | 문자열 비교 작동 | [ ] | |
| E14 | NULL 값 INSERT | 정책 일관성 (지원 안 함이 정답일 수도) | [ ] | |

---

## 🅲 CLI 플래그

| # | 명령 | 기대 | 결과 | 메모 |
|---|---|---|---|---|
| C1 | `./sqlparser` | usage + exit 1 | [ ] | |
| C2 | `./sqlparser nonexistent.sql` | perror + exit 1 | [ ] | |
| C3 | `./sqlparser --help` | 도움말, exit 0 | [ ] | |
| C4 | `./sqlparser -h` | 도움말 | [ ] | |
| C5 | `./sqlparser --version` | `MiniSQL 0.1.0` | [ ] | |
| C6 | `./sqlparser query.sql --debug --json` | 둘 다 출력 | [ ] | |
| C7 | `./sqlparser query.sql --debug --json --format` | 셋 다 출력 | [ ] | |
| C8 | `./sqlparser query.sql --tokens` | 토큰만, execute 안 함 | [ ] | |
| C9 | `./sqlparser query.sql --unknown` | 동작 확인 (현재: 무시) | [ ] | |

---

## 🅳 server.py / HTML 뷰어

서버 시작:
```bash
python3 server.py 8000 &
SERVER_PID=$!
# ... 테스트 ...
kill $SERVER_PID
```

| # | 시나리오 / 명령 | 기대 | 결과 | 메모 |
|---|---|---|---|---|
| S1 | sqlparser 바이너리 없는 상태로 server 시작 후 /query | 친절 에러 ("먼저 make 로 빌드하세요") | [ ] | |
| S2 | `curl -s http://localhost:8000/health` | `{"ok":true,"binary_exists":true}` | [ ] | |
| S3 | `curl -s http://localhost:8000/` | HTML 본문 | [ ] | |
| S4 | `curl -X POST http://localhost:8000/query -H "Content-Type: application/json" -d '{"sql":"SELECT * FROM t"}'` | statements 배열 | [ ] | |
| S5 | `curl -X POST http://localhost:8000/query -H "Content-Type: application/json" -d '{}'` | 400 + missing sql | [ ] | |
| S6 | `curl -X POST http://localhost:8000/query -H "Content-Type: application/json" -d 'not json'` | 400 + invalid JSON | [ ] | |
| S7 | 5초 timeout 시뮬 (큰 입력) | timeout 처리 | [ ] | |
| S8 | 동시 5개 요청 (`for i in 1 2 3 4 5; do curl ... & done`) | 모두 정상 | [ ] | |
| S9 | 브라우저에서 한국어 SQL (`SELECT 이름 FROM 테이블`) 입력 | UTF-8 정상 | [ ] | |
| S10 | 브라우저에서 `<script>alert(1)</script>` 입력 | escapeHtml 적용, JS 실행 X | [ ] | |

---

## 🅴 메모리 / 안정성

| # | 명령 | 기대 | 결과 | 메모 |
|---|---|---|---|---|
| M1 | `valgrind --leak-check=full -q ./sqlparser query.sql` | 누수 0 | [ ] | |
| M2 | `valgrind --leak-check=full -q ./sqlparser query.sql --debug --json --format` | 누수 0 | [ ] | |
| M3 | `valgrind --leak-check=full -q ./test_runner` | 누수 0 | [ ] | |
| M4 | `for i in $(seq 1 100); do ./sqlparser query.sql > /dev/null; done; echo OK` | 100회 OK | [ ] | |
| M5 | 1MB SQL 파일 처리 (`yes 'SELECT * FROM t;' \| head -100000 > /tmp/big.sql && ./sqlparser /tmp/big.sql > /dev/null`) | 크래시 X | [ ] | |

---

## 🅵 통합 시나리오 (가장 중요)

| # | 흐름 | 기대 | 결과 | 메모 |
|---|---|---|---|---|
| I1 | `./run_demo.sh` 한 번에 끝까지 | 모든 단계 통과 + 서버 시작 | [ ] | |
| I2 | 브라우저에서 query.sql 내용 그대로 입력 → Run | CLI 와 동일 결과 | [ ] | |
| I3 | `./sqlparser query.sql --json` 출력을 수동으로 server /query 에 보내기 | 동일 결과 | [ ] | |
| I4 | dev → main 머지 시뮬레이션 (`git checkout main && git merge --no-commit dev && git merge --abort`) | 충돌 0 | [ ] | |

---

## 발견된 버그 / TODO

| 일시 | 카테고리 | 내용 | 우선순위 | 처리 |
|---|---|---|---|---|
| | | | | |

---

## 발표 직전 최종 점검 (Smoke Test)

발표 시작 30분 전 무조건 한 번 더:

1. [ ] `git pull origin dev` (최신)
2. [ ] `make clean && make` (무경고)
3. [ ] `make test` (138+ passed / 0 failed)
4. [ ] `./run_demo.sh` (브라우저까지 정상)
5. [ ] 노트북 충전 / 외부 모니터 연결 / 인터넷 / Wi-Fi
6. [ ] 발표 슬라이드 / 대본 / 백업 영상 위치 확인
