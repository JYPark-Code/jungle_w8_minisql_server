# Round 2 Integration Map

> 동현 (cache) · 용 (trie) 이 같은 파일 (`src/engine.c`, `src/router.c`)
> 에 코드를 삽입한다. 작업 **시작 전** 에 두 사람이 이 문서를 읽고
> "내가 건드릴 zone 과 상대방이 건드릴 zone 이 분리되어 있다" 를 확인한다.
> 이렇게 하면 mix-merge 시점에 conflict 를 함수/block 단위로 가둘 수 있다.
>
> 참조:
> - `TEAM_RULES.md § 10 Round 2 작업 소유권 + 교차 파일 수정 규약`
> - `CLAUDE.md § 모듈 간 인터페이스 (TBD — 팀 합의 필요)`
> - `claude_jiyong.md § mix-merge 함수 단위 판정 기준`

라인 번호는 `dev` 브랜치 현재 기준. 구현 중 재배치되면 Zone 이름으로
추적 (라인 번호보다 Zone 이름이 우선).

---

## 1. `src/engine.c`

### Zone C1 — cache lookup (동현)
**함수**: `engine_exec_sql()` (현재 line 98)
**위치**: 함수 진입 직후, `jb_init(&out)` 호출 **전**
**목적**: 동일 SQL 반복 시 파싱/실행 생략하고 캐시된 JSON 바로 반환
**삽입 예**:
```c
engine_result_t engine_exec_sql(const char *sql, bool single_mode) {
    engine_result_t r;
    /* ROUND2-C1: cache lookup (동현) */
    if (!single_mode && !is_blank_sql(sql)) {
        char   *hit_json = NULL;
        size_t  hit_len  = 0;
        char    key[1024];
        cache_normalize_key(sql, key, sizeof(key));
        if (cache_get(s_cache, key, &hit_json, &hit_len)) {
            memset(&r, 0, sizeof(r));
            r.ok         = true;
            r.json       = hit_json;  /* heap 소유권 호출자에게 이전 */
            r.elapsed_ms = 0.0;       /* 캐시 히트 표시는 별도 필드/헤더로 검토 */
            return r;
        }
    }
    /* ── 기존 경로 진입 ── */
    json_buf_t out;
    ...
}
```

### Zone C2 — cache put (동현)
**함수**: `engine_exec_sql()` 성공 return 직전 (현재 line ~211)
**목적**: 이번 실행 결과를 캐시에 적재해 다음 동일 SQL 에 재사용
**삽입 예**:
```c
    ...
    r.ok            = all_ok;
    r.json          = jb_take(&out);
    r.index_used    = index_used;
    r.nodes_visited = nodes_visited;
    atomic_fetch_add_explicit(&s_total_queries, (uint64_t)statement_count,
                              memory_order_relaxed);
    /* ROUND2-C2: cache put (동현) */
    if (!single_mode && r.ok && r.json) {
        char   key[1024];
        cache_normalize_key(sql, key, sizeof(key));
        cache_put(s_cache, key, r.json, strlen(r.json));
    }
    return r;
```

### Zone C3 — cache invalidate on write (동현)
**함수**: `execute_statement()` (현재 line 526) 또는 그 호출자 `engine_exec_sql()` 의 per-statement loop 안
**목적**: INSERT / UPDATE / DELETE 가 한 건이라도 수행되면 관련 캐시 제거
**삽입 예**:
```c
    status = execute_statement(parsed, &out, single_mode, &stmt_index_used, &stmt_nodes);
    /* ROUND2-C3: invalidate on write (동현) */
    if (parsed && (parsed->type == QUERY_INSERT ||
                   parsed->type == QUERY_UPDATE ||
                   parsed->type == QUERY_DELETE ||
                   parsed->type == QUERY_CREATE)) {
        /* 보수적 시작값: 전체 invalidate.
         * 측정 후 table 단위 (cache_invalidate_prefix(table)) 로 좁힘 */
        cache_invalidate_all(s_cache);
    }
```

### Zone T1 — trie prefix dispatcher (용)
**함수**: `execute_select()` (현재 line 572)
**위치**: `should_use_id_index()` / `should_use_id_range()` 판정 바로 뒤.
기존 B+Tree 분기 앞에 새 판정 함수 `should_use_trie_prefix()` 추가
**목적**: `SELECT ... WHERE <text_col> LIKE 'abc%'` 형태를 Trie 로 dispatch
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
        const char *prefix = extract_prefix(sql);
        int n = trie_search_prefix(s_trie, prefix, out_rows, MAX_TRIE_OUT);
        *out_index_used = true;
        *out_nodes_visited = n;
        /* storage_select_result_by_row_indices(...) 경유 JSON 직렬화 */
        ...
        return 0;
    }
    /* 기존 선형 탐색 fallback */
    ...
}
```

### Zone IT — init / teardown (동현 + 용 공통, PM 조율)
**함수**: `engine_init()` (line 92) 끝부분, `engine_shutdown()` (line 292) 첫부분
**목적**: `s_cache` / `s_trie` 전역 인스턴스 생성 · 해제
**삽입 예**:
```c
int engine_init(const char *data_dir) {
    (void)data_dir;
    atomic_store_explicit(&s_total_queries, 0, memory_order_relaxed);
    if (engine_lock_init() != 0) return -1;
    /* ROUND2-IT: round 2 인덱스 / 캐시 초기화 */
    s_cache = cache_create(1024);       if (!s_cache) goto fail_cache;
    s_trie  = trie_create();            if (!s_trie)  goto fail_trie;
    return 0;
    ...
}

void engine_shutdown(void) {
    /* ROUND2-IT: 역순 해제 */
    if (s_trie)  { trie_destroy(s_trie);   s_trie  = NULL; }
    if (s_cache) { cache_destroy(s_cache); s_cache = NULL; }
    engine_lock_shutdown();
}
```
> ⚠ 두 담당자의 PR 모두 이 Zone 을 건드려야 함. 먼저 머지되는 쪽이
> 본인 라인만 추가하고, 뒤에 머지되는 쪽은 상대 라인 사이에 본인 것을 끼움.
> PM 이 mix-merge 시 순서 (cache → trie) 를 확정.

---

## 2. `src/router.c`

### Zone R-STATS — `/api/stats` 응답 확장 (동현)
**함수**: `router_dispatch()` 안 `/api/stats` 분기 (현재 line 264)
**추가 필드**: `"cache_hits": N`, `"cache_misses": N`, (선택) `"cache_capacity": N`
**삽입 예**:
```c
    if (strcmp(req->path, "/api/stats") == 0) {
        uint64_t total = 0, lock_wait_ns = 0;
        engine_get_stats(&total, &lock_wait_ns);
        /* ROUND2-R-STATS: cache 카운터 (동현) */
        unsigned long ch = cache_hits(s_cache);
        unsigned long cm = cache_misses(s_cache);
        return set_jsonf(resp, 200,
            "{\"ok\":true,\"total_queries\":%llu,\"lock_wait_ns_total\":%llu,"
            "\"cache_hits\":%lu,\"cache_misses\":%lu}",
            (unsigned long long)total, (unsigned long long)lock_wait_ns,
            ch, cm);
    }
```

### Zone R-SEARCH — `/search` 신규 핸들러 (용)
**위치**: `router_dispatch()` 안, `/api/` 분기 **밑에**. 이 엔드포인트는 `/api/` prefix 가 없음 (FE 계약 기준)
**로직**: `?q=word` → `engine_exec_sql("SELECT ... WHERE key='word'")` 경유 **OR** 직접 trie exact 호출
**삽입 예**:
```c
    /* ROUND2-R-SEARCH: 정확 매칭 (용) */
    if (strcmp(req->path, "/search") == 0 && method_is(req, "GET")) {
        char q[256];
        if (query_param(req->query, "q", q, sizeof(q)) != 0) {
            return set_body(resp, 400, "application/json",
                "{\"ok\":false,\"error\":\"missing_q\"}");
        }
        /* engine 경유 — cache 효과도 함께 검증 가능 */
        char sql[512];
        snprintf(sql, sizeof(sql), "SELECT * FROM words WHERE spelling='%s'", q);
        engine_result_t r = engine_exec_sql(sql, false);
        take_engine_result(resp, &r);
        return 0;
    }
```

### Zone R-AUTO — `/autocomplete` 신규 핸들러 (용)
**삽입 예**:
```c
    /* ROUND2-R-AUTO: prefix 매칭 (용) */
    if (strcmp(req->path, "/autocomplete") == 0 && method_is(req, "GET")) {
        char prefix[128];
        if (query_param(req->query, "prefix", prefix, sizeof(prefix)) != 0) {
            return set_body(resp, 400, "application/json",
                "{\"ok\":false,\"error\":\"missing_prefix\"}");
        }
        /* TODO: engine 경유 (prefix dispatcher) 또는 trie 직접 호출 */
        ...
    }
```

### Zone R-INSERT — `/admin/insert` 신규 핸들러 (용, 동현과 일부 연계)
**삽입 예**:
```c
    /* ROUND2-R-INSERT: 관리자 쓰기 (용) */
    if (strcmp(req->path, "/admin/insert") == 0 && method_is(req, "POST")) {
        /* body 파싱 → INSERT SQL 합성 → engine_exec_sql(..., false) */
        /* engine.c 의 Zone C3 가 자동으로 cache_invalidate_all 호출 */
        ...
    }
```

### Zone `/api/inject` Round 2 완성 — 별건 (담당 미확정)
**위치**: 현재 501 반환 (line 279~). 2차 끝까지 구현 필요.
현재 라운드에서는 이 Zone 을 **누가** 완성할지 회의에서 별도 합의 필요.
기본값: `/admin/insert` 와 로직이 유사 (더미 주입용) → 용 이 함께 처리.

---

## 3. mix-merge 순서 (PM 권장)

`claude_jiyong.md § mix-merge 머지 순서` 연장선:

1. **PM 자체 작업 (동적 threadpool)** 먼저 → dev
2. **동현 (cache)** — 기존 engine.c 에 C1/C2/C3/IT 삽입
3. **용 (trie)** — 2 가 들어간 engine.c 위에 T1/IT 삽입. IT Zone 은 cache 라인 다음에 끼움
4. **용 (router.c 신규 엔드포인트)** — 동현 PR 이 R-STATS 확장한 위에 R-SEARCH / R-AUTO / R-INSERT 추가
5. **승진 (UI)** — 백엔드 엔드포인트가 dev 에 있는 상태에서 fetch URL 교체. mock → 실연결 1 줄 수정

각 단계 뒤 dev CI 녹색 확인 후 다음 PR 진행.

---

## 4. PR 본문 체크리스트 (동현 / 용)

```markdown
### Round 2 Integration (docs/round2_integration_map.md 확인)
- [ ] 내가 건드린 Zone 이름 모두 명시 (예: C1, C2, C3, IT)
- [ ] 위 Zone 외 다른 파일/함수는 건드리지 않음 (TEAM_RULES §10-1)
- [ ] 상대 담당자 (동현↔용) PR 이 아직 안 올라왔으면 그대로 PR, 이미
      머지되어 있으면 리베이스 후 Zone 위치 재확인
- [ ] mix-merge 시 내 Zone 라인이 식별 가능하도록 `/* ROUND2-<ZoneID> */`
      마커 주석 유지
```
