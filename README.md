# minisqld — Multi-threaded Mini DBMS in Pure C

> Week 8: W7 B+Tree Index DB 위에 **단일 C 데몬**으로 동작하는 멀티스레드
> API 서버를 얹어, 외부 클라이언트가 HTTP 로 SQL 을 실행할 수 있는
> 미니 DBMS 를 완성한다.

**핵심 차별점:**
- 순수 C (외부 HTTP/JSON 라이브러리 0)
- Thread pool 기반 동시 요청 처리
- 테이블 단위 RW lock 으로 read 동시성 극대화
- 단일 프로세스 상주 → W7 의 "subprocess 당 1.8s rebuild 고정비" 제거
- **결과: SQL end-to-end 성능이 자료구조 pure 수치(1,842×)에 근접**

---

## 빠른 시작

```bash
git clone https://github.com/JYPark-Code/jungle_w8_minisqld.git
cd jungle_w8_minisqld

# 빌드
make

# 데몬 실행 (기본 8080 포트)
./minisqld --port 8080 --workers 8 --data-dir ./data --web-root ./web

# 다른 터미널에서 테스트
curl -X POST http://localhost:8080/api/query \
     -H "Content-Type: text/plain" \
     -d "SELECT * FROM users WHERE id = 1"
```

브라우저로 <http://localhost:8080> 접속 시 동시성 데모 페이지.

---

## 아키텍처
[Client / Browser]
│ HTTP/1.1
▼
[server.c]  accept loop
│  enqueue(fd)
▼
[threadpool.c]  N workers
│
▼
[protocol.c] HTTP parse → [router.c] dispatch
│
▼
[engine.c]  RW lock per table, global catalog lock
│
▼
[parser / executor / storage / bptree]  (W7 엔진 자산 재사용)

| 레이어 | 파일 | 동시성 |
|---|---|---|
| Socket | `server.c` | accept 는 main thread, fd 는 job queue 로 |
| Thread pool | `threadpool.c` | mutex + condvar blocking queue |
| HTTP | `protocol.c` + `router.c` | stateless (worker 당 요청) |
| Engine | `engine.c` | 테이블 RW lock + 글로벌 catalog lock |
| Storage | `storage.c` (W7) | engine 레이어가 락으로 보호 |

자세한 다이어그램: [`docs/architecture.svg`](docs/architecture.svg)

---

## API

| Method | Path | 설명 |
|---|---|---|
| POST | `/api/query` | SQL 실행, body 는 raw SQL (text/plain) |
| POST | `/api/query?mode=single` | 전역 mutex 로 강제 직렬화 (비교 demo 용) |
| POST | `/api/inject` | 더미 데이터 주입 |
| GET | `/api/stats` | active workers, qps, lock wait 통계 |
| GET | `/api/explain?sql=...` | 인덱스 사용, 노드 visit 수 |
| GET | `/*` | 정적 파일 서빙 (`--web-root`) |

응답은 JSON: `{"ok":true,"rows":[...],"elapsed_ms":2.3,"index_used":true}`

---

## 빌드 타겟

```bash
make                # 데몬 빌드 (./minisqld)
make test           # 회귀 테스트 (W7 227 개 + 동시성 테스트)
make tsan           # ThreadSanitizer 빌드
make valgrind       # 메모리 누수 검사
make bench          # B+ 트리 pure 벤치 (W7 자산)
make loadtest       # 동시 N 요청 부하 테스트
make clean
```

---

## 디렉토리
/
├─ include/               공개 인터페이스 (PM 관리)
├─ src/                   구현 (W7 엔진 + Round 4 신규)
├─ tests/                 회귀 + 동시성 테스트
├─ bench/                 B+ 트리 pure 벤치 (W7)
├─ scripts/               fixture 생성, loadtest
├─ web/                   Round 4 새 데모 페이지
├─ docs/                  아키텍처 다이어그램
├─ agent/                 이전 라운드 AI 협업 컨텍스트 아카이브
├─ prev_project/          이전 라운드 웹 데모 자산 아카이브
│   ├─ sql_parser_demo/   W6
│   └─ bp_tree_demo/      W7
├─ CLAUDE.md              레포 전역 규칙 (팀원 Claude Code 세션용)
├─ agent.md               W8 라운드 설계 결정 + 4 분할 + 마일스톤
├─ claude_jiyong.md       PM 개인 컨텍스트
└─ README.md              (이 파일)

---

## 팀

| 이름 | 담당 |
|---|---|
| 지용 (PM) | `include/*.h` 인터페이스 + engine_lock + mix-merge + ANSI REPL |
| 동현 | `engine.c` thread-safe wrapping + EXPLAIN + `mode=single` |
| 용 형님 | `server.c` + `protocol.c` HTTP + `router.c` + 탭 (a) Stress 데모 |
| 승진 | `threadpool.c` + `/api/stats` + 탭 (c) RW Contention 데모 |

---

## 이전 라운드

- W6 SQL Parser: <https://github.com/JYPark-Code/jungle_w6_mini_mysql_sql_parser>
- W7 B+Tree Index DB: <https://github.com/JYPark-Code/jungle_w7_BplusTree_Index_DB>
- W7 최종본 README: [`docs/README_w7.md`](docs/README_w7.md)