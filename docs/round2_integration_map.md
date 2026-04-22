# Round 2 Integration Map

> 동현 (router-level dict cache) · 용 (Trie prefix index) 이 각자의 zone
> 안에서 코드를 삽입한다. 작업 **시작 전** 에 두 사람이 이 문서를 읽고
> "내가 건드릴 zone 과 상대방이 건드릴 zone 이 분리되어 있다" 를 확인한다.
>
> 2026-04-22 회의 결정에 따라 cache 위치를 **engine 내부 → router level** 로
> 이동. engine.c 핵심 경로를 오염시키지 않고, 캐시 범위를 `/api/dict`
> 엔드포인트로 제한해 invalidation 과 디버깅을 단순화했다.
>
> 사전 방향 역시 **영한** (영어 key → 한글 해석) 으로 확정. 역방향
> (한글 body → 영어) 은 **indexing 없이 선형 스캔** 으로 동작은 하되 느림
> ("option B").
>
> 참조:
> - `TEAM_RULES.md § 10 Round 2 작업 소유권 + 교차 파일 수정 규약`
> - `CLAUDE.md § 모듈 간 인터페이스 (TBD — 팀 합의 필요)`
> - `claude_jiyong.md § mix-merge 함수 단위 판정 기준`

라인 번호는 `dev` 브랜치 현재 기준. 구현 중 재배치되면 Zone 이름으로
추적 (라인 번호보다 Zone 이름이 우선).

---

## 1. `src/engine.c` — **용 (Trie) 단독**

cache 가 router 로 이전되면서 engine.c 에서 **동현 의 zone 은 제거**.
용 만 engine.c 를 건드린다.

### Zone T1 — trie prefix dispatcher (용)
**함수**: `execute_select()` (현재 line 572)
**위치**: `should_use_id_index()` / `should_use_id_range()` 판정 바로 뒤.
기존 B+Tree 분기 앞에 새 판정 함수 `should_use_trie_prefix()` 추가
**제약**: Trie 는 **ASCII 소문자 영어 only** 로 인덱스된 상태. 한글 body
조회 (역방향) 는 이 경로 타지 않고 기존 선형 탐색 fallback 유지
**삽입 예**:
```c
static int execute_select(ParsedSQL *sql, json_buf_t *out,
                          bool *out_index_used, int *out_nodes_visited) {
    ...
    if (should_use_id_index(sql)) {
        /* 기존 B+Tree exact 경로 */
        ...
    }
    if (should_use_id_range(sql)) {
        /* 기존 B+Tree range 경로 */
        ...
    }
    /* ROUND2-T1: trie prefix 경로 (용) */
    if (should_use_trie_prefix(sql)) {
        int out_rows[MAX_TRIE_OUT];
        const char *prefix = extract_prefix(sql);  /* ASCII 가정 */
        int n = trie_search_prefix(s_trie, prefix, out_rows, MAX_TRIE_OUT);
        *out_index_used = true;
        *out_nodes_visited = n;
        /* storage_select_result_by_row_indices(...) 경유 JSON 직렬화 */
        ...
        return 0;
    }
    /* 기존 선형 탐색 fallback
     * — 한글 body 조회 (WHERE korean='사과') 는 여기서 처리됨. 느리지만 동작 OK */
    ...
}
```

### Zone IT — init / teardown (용 단독)
**함수**: `engine_init()` (line 92) 끝부분, `engine_shutdown()` (line 292) 첫부분
**목적**: `s_trie` 전역 인스턴스 생성 · 해제
**삽입 예**:
```c
int engine_init(const char *data_dir) {
    (void)data_dir;
    atomic_store_explicit(&s_total_queries, 0, memory_order_relaxed);
    if (engine_lock_init() != 0) return -1;
    /* ROUND2-IT: trie 초기화 (용) */
    s_trie = trie_create();            if (!s_trie) return -1;
    /* dictionary 테이블 로드 후 각 row 의 english 컬럼을 trie 에 삽입
     * — 구현 상세는 용 담당 */
    return 0;
}

void engine_shutdown(void) {
    /* ROUND2-IT: 역순 해제 */
    if (s_trie) { trie_destroy(s_trie); s_trie = NULL; }
    engine_lock_shutdown();
}
```

> 💡 Round 1 초안의 Zone C1 / C2 / C3 (engine 내부 cache) 는 **설계 변경으로 제거**. 이력은 문서 말미 "설계 변경 이력" 참조.

---

## 2. `src/router.c` — 동현 ↔ 용 overlap (엔드포인트 단위)

cache 가 router 에 들어오면서 router 가 2 명의 공유 편집 영역이 된다.
하지만 **endpoint 단위로 분리** 되므로 실제 conflict 는 매우 작다.
각자 본인 zone 의 `if (strcmp(req->path, ...) == 0)` 블록만 추가.

### Zone R-DICT-CACHE — `/api/dict` 영한 조회 + dict cache (동현)
**위치**: `router_dispatch()` 안 라우팅 분기에 신규 추가
**파라미터**: `?english=<word>` (primary) 또는 `?id=<N>` (옵션) 또는 `?korean=<ko>` (역방향, cache 미적용)
**로직** (영한 / id 방향):
1. cache key 생성: `"english:<word>"` 또는 `"id:<N>"` prefix 분리
2. `dict_cache_get(key)` 시도
3. hit → 즉시 JSON 반환, `cache_hits++`
4. miss → cache lock 밖에서 `engine_exec_sql(...)` 호출 (cache 가 오래
   잠기지 않도록)
5. 결과 JSON 을 `dict_cache_put(key, json)` 후 반환, `cache_misses++`
6. 역방향 파라미터 (`?korean=사과`) 는 dict cache **미적용**, engine 직행 (선형 스캔)

> 같은 단어 동시 miss 시 중복 DB 조회는 허용 trade-off (결과 정합성 문제 X).

**삽입 예**:
```c
/* ROUND2-R-DICT-CACHE: 영한 사전 조회 + router-level dict cache (동현) */
if (strcmp(req->path, "/api/dict") == 0 && method_is(req, "GET")) {
    char english[128];
    char id_buf[32];
    char korean_body[128];
    int has_en = (query_param(req->query, "english", english, sizeof(english)) == 0);
    int has_id = (query_param(req->query, "id", id_buf, sizeof(id_buf)) == 0);
    int has_ko = (query_param(req->query, "korean", korean_body, sizeof(korean_body)) == 0);

    if (!has_en && !has_id && !has_ko) {
        return set_body(resp, 400, "application/json",
            "{\"ok\":false,\"error\":\"missing_param\"}");
    }

    /* 영한 / id 방향 (primary): dict cache lookup 먼저 */
    if (has_en || has_id) {
        char key[160];
        if (has_en) snprintf(key, sizeof(key), "english:%s", english);
        else        snprintf(key, sizeof(key), "id:%s", id_buf);

        char  *hit_json = NULL;
        size_t hit_len  = 0;
        if (dict_cache_get(s_dict_cache, key, &hit_json, &hit_len)) {
            resp->status = 200;
            resp->content_type = "application/json";
            resp->body = hit_json;       /* heap 소유권 이전 */
            resp->body_len = hit_len;
            return 0;
        }
        /* miss → cache lock 밖에서 engine 경유 */
        char sql[512];
        if (has_en) {
            snprintf(sql, sizeof(sql),
                     "SELECT korean FROM dictionary WHERE english='%s'", english);
        } else {
            snprintf(sql, sizeof(sql),
                     "SELECT english,korean FROM dictionary WHERE id=%s", id_buf);
        }
        engine_result_t r = engine_exec_sql(sql, false);
        if (r.ok && r.json) {
            dict_cache_put(s_dict_cache, key, r.json, strlen(r.json));
        }
        take_engine_result(resp, &r);
        return 0;
    }

    /* 역방향 (korean body → english): 인덱스 없음, 선형 스캔, dict cache 미적용 */
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT english FROM dictionary WHERE korean='%s'", korean_body);
    engine_result_t r = engine_exec_sql(sql, false);
    take_engine_result(resp, &r);
    return 0;
}
```

### Zone R-STATS — `/api/stats` 응답 확장 (동현)
**위치**: 기존 `/api/stats` 핸들러 (line 264)
**추가 필드**: `"cache_hits": N`, `"cache_misses": N`
**삽입 예**:
```c
if (strcmp(req->path, "/api/stats") == 0) {
    uint64_t total = 0, lock_wait_ns = 0;
    engine_get_stats(&total, &lock_wait_ns);
    /* ROUND2-R-STATS: dict cache 카운터 (동현) */
    unsigned long ch = dict_cache_hits(s_dict_cache);
    unsigned long cm = dict_cache_misses(s_dict_cache);
    return set_jsonf(resp, 200,
        "{\"ok\":true,\"total_queries\":%llu,\"lock_wait_ns_total\":%llu,"
        "\"cache_hits\":%lu,\"cache_misses\":%lu}",
        (unsigned long long)total, (unsigned long long)lock_wait_ns,
        ch, cm);
}
```

### Zone R-AUTO — `/api/autocomplete` 신규 핸들러 (용)
**로직**: 영어 prefix 로 trie 조회 → 매칭되는 english 단어 배열 반환.
한글 입력 또는 non-ASCII prefix 는 400 반환.
**삽입 예**:
```c
/* ROUND2-R-AUTO: prefix 매칭, English ASCII only (용) */
if (strcmp(req->path, "/api/autocomplete") == 0 && method_is(req, "GET")) {
    char prefix[64];
    if (query_param(req->query, "prefix", prefix, sizeof(prefix)) != 0) {
        return set_body(resp, 400, "application/json",
            "{\"ok\":false,\"error\":\"missing_prefix\"}");
    }
    if (!is_ascii_lowercase(prefix)) {
        return set_body(resp, 400, "application/json",
            "{\"ok\":false,\"error\":\"prefix_must_be_ascii_lowercase\"}");
    }
    /* engine 경유 (Zone T1 가 처리) 또는 trie 직접 호출 — 구현자 선택 */
    ...
}
```

### Zone R-ADMIN-INSERT — `/api/admin/insert` 신규 핸들러 (용, 동현 invalidate 연계)
**로직**: POST JSON body `{"english":"apple","korean":"사과"}` 로 dictionary 테이블에 INSERT. 성공 시 dict cache 해당 key invalidate.
**삽입 예**:
```c
/* ROUND2-R-ADMIN-INSERT: 관리자 등록 (용) */
if (strcmp(req->path, "/api/admin/insert") == 0 && method_is(req, "POST")) {
    /* body 파싱 → INSERT SQL 합성 → engine_exec_sql */
    ...
    /* 성공 시 dict cache 무효화 (english: prefix key) */
    char key[160];
    snprintf(key, sizeof(key), "english:%s", new_entry.english);
    dict_cache_invalidate(s_dict_cache, key);
    /* 또는 보수적으로: dict_cache_invalidate_all(s_dict_cache); */
    ...
}
```

### Zone R-INIT — dict cache 인스턴스 생성 / 해제 (동현, router 내부)
**위치**: `router.c` 파일 상단에 `static dict_cache_t *s_dict_cache = NULL;`
초기화 방식은 구현자 선택:
- **옵션 A** (추천): `pthread_once` 로 첫 요청 시 lazy init
- **옵션 B**: `router_init_dict_cache()` 함수 신설, server.c 에서 호출
  — server.c 건드려야 하므로 **비추천** (TEAM_RULES §10-3 기각 판정과 정합)
- **옵션 C**: router.c 파일 scope 의 pthread_once_t + 첫 진입 시 init

**삽입 예 (옵션 A)**:
```c
/* router.c 상단 */
#include "dict_cache.h"          /* src/ 안 private 헤더 */
static dict_cache_t *s_dict_cache = NULL;
static pthread_once_t s_cache_once = PTHREAD_ONCE_INIT;

static void init_dict_cache(void) {
    s_dict_cache = dict_cache_create(1024);
}

/* /api/dict 핸들러 진입 시 1 회 보장 */
pthread_once(&s_cache_once, init_dict_cache);
```

shutdown 시 `dict_cache_destroy` 는 현 라운드에서는 생략 가능 (프로세스
종료 시 OS 가 메모리 회수). 깔끔하게 하려면 atexit 등록 1 줄.

---

## 3. mix-merge 순서 (PM 권장)

`claude_jiyong.md § mix-merge 머지 순서` 연장선. cache pivot 으로
engine.c overlap 이 사라져 순서가 더 단순해짐:

1. **PM (동적 threadpool)** 먼저 → dev
2. **용 (trie)** — engine.c 에 Zone T1 + IT 삽입. router.c 는 건드리지 않음
3. **동현 (dict cache + router)** — `src/dict_cache.c` + `src/dict_cache.h` 신규 + `src/router.c` 에 Zone R-DICT-CACHE / R-STATS / R-INIT 삽입
4. **용 (router 신규 엔드포인트)** — 동현 PR 위에 Zone R-AUTO / R-ADMIN-INSERT 추가
5. **승진 (UI)** — fetch URL 교체 (mock → 실연결)

각 단계 뒤 dev CI 녹색 확인 후 다음 PR 진행.

> 📌 **router.c 의 동현 ↔ 용 overlap** 은 PR 단계에서 endpoint 단위 conflict 만 발생. 같은 함수 (`router_dispatch`) 안에 서로 다른 `if (strcmp(path, ...))` 블록을 추가하므로 git 이 대부분 자동 병합. PM rebase 책임은 최소화.

---

## 4. PR 본문 체크리스트 (동현 / 용)

```markdown
### Round 2 Integration (docs/round2_integration_map.md 확인)
- [ ] 내가 건드린 Zone 이름 모두 명시 (예: T1 + IT / R-DICT-CACHE + R-STATS + R-INIT)
- [ ] 위 Zone 외 다른 파일/함수는 건드리지 않음 (TEAM_RULES §10-1)
- [ ] 상대 담당자 (동현↔용) PR 이 아직 안 올라왔으면 그대로 PR, 이미
      머지되어 있으면 리베이스 후 Zone 위치 재확인
- [ ] mix-merge 시 내 Zone 라인이 식별 가능하도록 `/* ROUND2-<ZoneID> */`
      마커 주석 유지
- [ ] 한글 입력 / 비 ASCII 처리 경로가 trie 를 타지 않는지 확인
      (trie 는 ASCII lowercase only, 한글 body 조회는 선형 스캔 fallback)
```

---

## 5. 설계 변경 이력

- **2026-04-22 초안**: Zone C1 / C2 / C3 (engine 내부 query cache, 동현)
- **2026-04-22 수정**: engine-level cache 제거, **router-level dict cache** 로 pivot
  - 근거: 사전 서비스는 엔드포인트 단위 캐싱으로 충분. 범용 SQL normalize /
    lifetime / race window 회피. engine 핵심 경로 무오염
  - 영향: 동현 ↔ 용 의 engine.c overlap 소멸. router.c overlap 은 엔드포인트
    단위로 남음 (conflict 작음)
- **2026-04-22 추가 결정**: **영한 단방향** 확정. 역방향 (한글 body) 조회는
  인덱스 없이 선형 스캔으로 동작 유지 ("option B — slow but works")
  - Trie 는 ASCII 영어만 저장. 노드 자식 26 개 고정 배열로 구현 단순화
  - 한글 Unicode NFC 정규화 / 초성 검색 범위 외
