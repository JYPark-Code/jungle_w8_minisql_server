# Phase 1 — 후속 리팩토링 보고서

> 1차 완성 직후 같은 날 같은 팀이 발견한 한계를 해결하기 위해 진행한
> 후속 리팩토링의 작업 기록 + 협업 패턴 학습 자료.

---

## 0. 한 줄 요약

**1차 완성 (201 테스트, main 머지) → 사용/리뷰에서 6 가지 한계 발견 → 3 명이 dev2 에서 4 시간 후속 작업 → 옵션 B Mixed Merge 로 통합 → main 머지 (227 테스트).**

---

## 1. 1차 완성에서 발견된 한계 6 가지

오전 1차 완성 후 코드를 다시 들여다보면서 드러난 약점:

| # | 발견 | 원인 | 영향 |
|---|---|---|---|
| 1 | `storage_select` 가 결과를 stdout 에만 print | 다른 함수가 결과를 받아 쓸 수 없음 | JOIN/집계/subquery 모두 불가능 |
| 2 | WHERE 가 1~2 조건 + 단일 결합자만 | 1주차 구현이 단순함을 우선 | `WHERE a=1 AND b=2 AND c=3` 미지원 |
| 3 | `storage_delete` / `storage_update` 시그니처 비대칭 | SELECT 는 `ParsedSQL*`, DELETE/UPDATE 는 `WhereClause*` + count | API 일관성 없음, N-ary 결합자 전달 불가 |
| 4 | 집계 함수 COUNT(*) 한 종류만 | 1주차 발표 우선순위 | SUM/AVG/MIN/MAX 미지원 |
| 5 | silent error 다수 | 에러 메시지 삽입 시간 부족 | "존재하지 않는 테이블 SELECT" 가 빈 결과 |
| 6 | server.py 의 `--json` 응답에 `{"raw": ...}` 객체 다수 | storage 가 `--json` 모드에서도 표를 stdout 에 같이 찍음 → JSON stream 오염 | 브라우저 뷰어에 UNKNOWN 카드 표시 |

---

## 2. 작업 분배 (3 명, dev2)

```
[D. 인터페이스 계약 정리 — 지용]  ← MP1, 가장 먼저
   • RowSet 구조체
   • where_links 결합자 배열
   • 새 함수 선언 (storage_select_result, print_rowset, rowset_free)
              ↓ 머지 후 셋이 동시 진행
   ┌──────────┼──────────────────────┐
   ↓          ↓                      ↓
[A. 지용]   [B. 석제]              [C. 원우]
RowSet      Parser stop set         storage_delete/update
인프라      + N-ary WHERE            시그니처 통일
+ 집계      + 출력 모듈              + N-ary 평가
함수 5종    갱신                     + executor.c 호출부
   └──────────┬──────────────────────┘
              ↓
        [통합 + 회귀 검증]
              ↓
        [dev2 → main]
```

| 영역 | 담당 | PR | 핵심 |
|---|---|---|---|
| MP1 인터페이스 계약 | 지용 | #29 | RowSet, where_links, 새 함수 선언 |
| A RowSet + 집계 함수 | 지용 | #30 | storage_select_result, COUNT/SUM/AVG/MIN/MAX, silent error 친절 메시지 |
| B Parser N-ary + stop set | 석제 | #33 | parse_where N-ary, parse_select stop set, ast inline 결합자 |
| C UPDATE/DELETE 시그니처 | 원우 | #32 | storage_delete/update(table, ParsedSQL*), N-ary 평가 |
| dev2 → main | PM | #34 | 최종 통합 |

---

## 3. 인터페이스 변경

### 3-1. 시그니처 통일

```c
// 1주차
int storage_delete(const char *table, WhereClause *where, int where_count);
int storage_update(const char *table, SetClause *set, int set_count,
                   WhereClause *where, int where_count);

// Phase 1 후 — SELECT 와 동일 패턴
int storage_delete(const char *table, ParsedSQL *sql);
int storage_update(const char *table, ParsedSQL *sql);
```

호출부 (`executor.c`) 도 한 줄씩 갱신:
```c
case QUERY_DELETE: storage_delete(sql->table, sql); break;
case QUERY_UPDATE: storage_update(sql->table, sql); break;
```

### 3-2. RowSet 신설

```c
typedef struct {
    int     row_count;
    int     col_count;
    char  **col_names;     // ["id", "name", "age"]
    char ***rows;          // rows[i][j] = i 번째 행의 j 번째 컬럼 값
} RowSet;

int  storage_select_result(const char *table, ParsedSQL *sql, RowSet **out);
void print_rowset(FILE *out, const RowSet *rs);
void rowset_free(RowSet *rs);
```

기존 `storage_select` 는 wrapper 가 됨 (외부 동작 동일):
```c
int storage_select(const char *table, ParsedSQL *sql) {
    RowSet *rs = NULL;
    int status = storage_select_result(table, sql, &rs);
    if (status == 0 && rs != NULL) print_rowset(stdout, rs);
    rowset_free(rs);
    return status;
}
```

### 3-3. ParsedSQL 확장

```c
typedef struct {
    /* ... 1주차 필드 ... */
    WhereClause *where;
    int          where_count;       // 1주차: 1~2 → Phase 1: N
    char         where_logic[8];    // (deprecated) 1주차 호환
    char       **where_links;       // NEW: N-1 개의 결합자 ("AND"/"OR")
} ParsedSQL;
```

---

## 4. 옵션 B Mixed Merge 패턴 ⭐

이 Phase 1 의 가장 중요한 협업 학습.

### 4-1. 발생한 상황

원우 PR #32 가 본인 N-ary WHERE 평가를 검증하기 위해 **석제 영역** (parser, ast, json, format, test_parser) 까지 만들어버림. 그 사이 석제 PR #33 도 같은 영역에서 작업 중. 두 PR 이 같은 함수들을 다르게 구현.

### 4-2. 두 가지 옵션

**옵션 A — 평범한 머지 패턴 (한 명 채택)**
- 한 PR 머지 후 다른 PR 닫음
- 1주차 PR #20 vs #21 (B vs C 경쟁) 과 같은 패턴
- **문제:** 한 명의 작업이 통째로 사라짐, 학습 가치 ↓, 작업자 사기 ↓

**옵션 B — 함수 단위 비교 후 베스트 통합 (Mixed Merge)**
- 두 구현의 같은 함수들을 옆으로 비교
- 함수마다 더 나은 쪽 채택
- 결과 commit 에 양쪽 Co-Authored-By
- **장점:** 두 사람 작업 모두 살아남음, 학습 효과 ↑, 코드 품질 ↑

### 4-3. 실제 적용 — PR #33 처리

**1단계: 함수 단위 비교**

원우 PR #32 머지된 dev2 vs 석제 PR #33 의 같은 영역을 함수마다 옆으로 비교:

| 함수 | 원우 | 석제 | 채택 | 사유 |
|---|---|---|---|---|
| `parse_where` | 외부 dead while + #if 0 보존 | 단일 while, 결합자 정규화, NULL 처리 명확 | **석제** | 가독성 ↑, 견고성 ↑ |
| `is_select_stop_token` | 없음 | 신규 helper | **석제** | 신규 기능 |
| `parse_select stop set` | FROM 만 체크 | FROM/WHERE/ORDER/LIMIT/`)`/`;` 모두 처리 | **석제** | 차별 가치 |
| `ast_print` WHERE 출력 | `links: AND OR` 헤더 | 각 조건 옆 inline | **석제** | 직관적 |
| `json_out` WHERE | `emit_str_array` 헬퍼 재사용 | 직접 루프 | **원우** | DRY 원칙 |
| `sql_format where_link_at` | NULL 안 반환, 정규화 | NULL 반환 가능 | **원우** | robust |
| `test_parser.c` | 자체 케이스 | 자체 케이스 | **합집합** | 모든 케이스 살림 |
| `test_executor.c` | dev2 새 경로 정합 | 동상 | **원우** | 더 정교 |

**2단계: 머지 commit 작성**

석제 브랜치에서 dev2 를 머지 → 5개 충돌 발생 → 함수마다 결정에 따라 해결:

```bash
git checkout 석제-브랜치
git merge dev2
# 충돌 해결:
git checkout --ours src/parser.c        # 석제
git checkout --ours src/ast_print.c     # 석제
git checkout --theirs src/sql_format.c  # 원우
git checkout --theirs tests/test_executor.c  # 원우
# test_parser.c 는 수동 합집합
git add -A && git commit -m "Merge dev2 into ... (mixed-merge)"
```

**3단계: Attribution**

```
Merge dev2 into feature/p1-parser-stop-set (mixed-merge)

함수 단위 채택:
- parse_where           → 석제 (단일 while, 결합자 정규화)
- parse_select stop set → 석제 (신규 기능)
- ast_print WHERE       → 석제 (inline 결합자)
- json_out WHERE        → 원우 (헬퍼 재사용)
- sql_format            → 원우 (NULL 안 반환)
- test_parser.c         → 합집합

Co-Authored-By: Kim Seok Je <hwarang97@...>
Co-Authored-By: wonu1 <wonu1@...>
Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

### 4-4. 결과

- 석제 작업의 약 **70% 가 dev2 에 그대로 살아남음**
- 원우 작업의 **JSON / format 부분이 유지** 됨
- 두 분 모두 attribution
- 27 새 단위 테스트 (양쪽 합집합) 통과

---

## 5. 옵션 B 채택 가이드

언제 옵션 B 를 써야 하나? 우리 케이스 기준:

### ✅ 옵션 B 가 좋은 상황
- 두 사람이 같은 영역을 다르게 구현
- 두 구현 모두 동작하고 검증됨
- 함수 단위로 명확히 분리 가능
- 양쪽 모두 학습 가치 있음
- 작업자 사기/심리적 영향 고려 필요

### ❌ 옵션 A (한 명 채택) 가 좋은 상황
- 한쪽이 명백히 잘못되었거나 동작 안 함
- 영역 격리가 정말 중요해서 향후 비슷한 상황 방지해야 함
- 시간 제약이 매우 심함 (mixed merge 는 시간 ↑)
- 두 구현이 너무 달라서 함수 단위 비교가 의미 없음

### 옵션 B 워크플로 체크리스트

```
1. [ ] 두 PR 다 빌드 + 테스트 + valgrind 통과 확인
2. [ ] 같은 영역의 함수들 옆으로 정렬해서 비교
3. [ ] 각 함수마다 평가 (가독성, 견고성, 엣지 케이스, 메모리)
4. [ ] 채택 결정 표 작성 (Claude 추천 + PM 결정)
5. [ ] 한 PR 의 브랜치를 base 로 dev2 머지
6. [ ] 충돌 해결 — 결정에 따라 ours/theirs/manual
7. [ ] 빌드 + 테스트 + valgrind 재검증
8. [ ] commit 에 양쪽 Co-Authored-By
9. [ ] 두 PR 모두에 결과 코멘트 (어느 부분이 채택됐는지)
10. [ ] 머지
```

---

## 6. 1차 vs 후속 결과 비교

| 지표 | 1차 완성 | 후속 리팩토링 후 | 변화 |
|---|---|---|---|
| 단위 테스트 | 201 | **227** | +26 |
| 빌드 경고 | 0 | **0** | — |
| valgrind 누수 | 0 (3 바이너리) | **0 (5 바이너리)** | +2 |
| storage 함수 | 5 | **8** | + RowSet 신설 |
| WHERE 조건 수 | 최대 2 | **N개** | 무한 |
| WHERE 결합자 | 단일 (AND or OR) | **혼합 가능** | |
| 집계 함수 | 1 (COUNT) | **5 (COUNT/SUM/AVG/MIN/MAX)** | +4 |
| 친절 에러 케이스 | 부분적 | **대부분 stderr** | +6 곳 |
| Pull Request 수 | 24 | **34** | +10 |
| 머지 commit | 60+ | **80+** | +20 |
| C 소스 라인 | ~5000 | **~6500** | +1500 |
| 작업 시간 | ~7 시간 | **+ ~4 시간 (같은 날)** | |

---

## 7. 다음 단계 (Phase 2 후보)

Phase 1 의 RowSet 인프라가 모든 미래 작업의 기반:

| 기능 | RowSet 의존도 | 난이도 |
|---|---|---|
| **B+트리 도입** (storage 내부 교체) | ⭐⭐ | 높음 |
| **JOIN** (RowSet 두 개 결합) | ⭐⭐⭐⭐⭐ | 높음 |
| **GROUP BY + 집계** (현재는 단일 집계만) | ⭐⭐⭐⭐ | 중간 |
| **Subquery** (스칼라 → IN → FROM) | ⭐⭐⭐⭐⭐ | 중간~높음 |
| **DATETIME 타입 실제 비교** | ⭐ | 낮음 |
| **트랜잭션 / 로그** | ⭐⭐ | 매우 높음 |

Phase 1 의 가장 큰 가치는 **"storage 가 데이터를 반환할 수 있게 됨"** — 위 모든 기능의 prerequisite.

---

## 8. 핵심 학습

1. **인터페이스 격리의 가치** — 1주차의 storage 시그니처 계약 덕분에 후속 작업에서 안전하게 시그니처를 갱신할 수 있었음. 격리가 없었다면 후속 리팩토링은 불가능.

2. **데이터 / 표시 분리** — `print_*` 와 `*_result` 의 분리가 모든 미래 기능의 기반. 1차 완성 시 이 부분을 안 한 게 가장 큰 약점이었음.

3. **CI 자동 검증** — 후속 리팩토링에서 회귀 0 을 보장한 핵심. dev2 trigger 누락 fix 는 1줄 수정이지만 효과는 큼.

4. **옵션 B Mixed Merge** — 같은 영역에 두 구현이 있을 때, 한 명을 탈락시키는 게 아니라 함수 단위로 베스트 통합. 인간적이면서 코드 품질 ↑.

5. **친절한 에러 메시지의 중요성** — silent error 는 사용자에게 "왜 안 됐지?" 만 남김. 한 줄짜리 stderr 추가가 사용자 경험을 크게 바꿈.

6. **Co-Authored-By 의 의미** — git history 에 attribution 이 남으면 작업자가 사라지지 않는다. 통합 commit 에서 특히 중요.
