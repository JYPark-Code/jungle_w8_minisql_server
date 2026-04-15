/* bptree.c — MiniSQL Week 7 B+ Tree 인덱스 구현 (지용 담당).
 *
 * 설계:
 *   - key = int (id 컬럼), value = int row_index (storage CSV 행 번호).
 *   - 모든 값(row_index) 은 리프에만 저장 (B+ 트리 기본 성질).
 *   - 리프끼리 next 포인터로 연결 → range 쿼리 O(log n + k) 에 유리.
 *   - 한 노드의 최대 자식 수 = order, 최대 키 수 = order - 1.
 *
 * 노드 표현:
 *   Node 하나로 리프/내부 양쪽 다 표현. is_leaf 플래그로 구분하고
 *   leaf / internal 전용 데이터는 union 으로 둔다.
 *
 * Phase 2: 구조체 + create/destroy + search.
 * Phase 3 이후: insert + split.
 */

#include "bptree.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/* 노드 공용 구조.
 * keys[]        : 크기 order (내부노드는 최대 order-1 개 사용, 삽입 중 오버플로 확인용으로 +1 여유).
 * leaf.row_indices[] : 크기 order.
 * internal.children[] : 크기 order+1. */
typedef struct Node {
    int is_leaf;
    int num_keys;
    int *keys;
    union {
        struct {
            int *row_indices;
            struct Node *next;
        } leaf;
        struct {
            struct Node **children;
        } internal;
    } u;
} Node;

struct BPTree {
    int order;
    Node *root;
};

/* 새 리프 노드 할당. 모든 배열은 order 크기로 잡아 overflow 처리에 여유를 둔다. */
static Node *leaf_new(int order) {
    Node *n = malloc(sizeof *n);
    if (!n) return NULL;
    n->is_leaf = 1;
    n->num_keys = 0;
    n->keys = malloc(sizeof(int) * (size_t)order);
    n->u.leaf.row_indices = malloc(sizeof(int) * (size_t)order);
    n->u.leaf.next = NULL;
    if (!n->keys || !n->u.leaf.row_indices) {
        free(n->keys);
        free(n->u.leaf.row_indices);
        free(n);
        return NULL;
    }
    return n;
}

/* 노드 + 하위 트리 재귀 해제. 리프의 next 링크는 "같은 레벨 이웃" 이므로
 * 따라가지 않는다 (부모에서 children 배열로 모두 도달 가능하기 때문). */
static void node_free(Node *n) {
    if (!n) return;
    if (!n->is_leaf) {
        for (int i = 0; i <= n->num_keys; ++i) {
            node_free(n->u.internal.children[i]);
        }
        free(n->u.internal.children);
    } else {
        free(n->u.leaf.row_indices);
    }
    free(n->keys);
    free(n);
}

BPTree *bptree_create(int order) {
    if (order < 3) return NULL; /* 최소 차수 제한 — split 로직이 성립하려면 3 이상. */
    BPTree *t = malloc(sizeof *t);
    if (!t) return NULL;
    t->order = order;
    t->root = leaf_new(order);
    if (!t->root) {
        free(t);
        return NULL;
    }
    return t;
}

void bptree_destroy(BPTree *tree) {
    if (!tree) return;
    node_free(tree->root);
    free(tree);
}

/* 내부 노드에서 id 키가 속할 자식 인덱스 반환.
 *
 * 규약: keys[i-1] <= id < keys[i] 인 i 를 찾아 children[i] 로 내려간다. */
static int internal_child_idx(const Node *n, int id) {
    assert(!n->is_leaf);
    int i = 0;
    while (i < n->num_keys && id >= n->keys[i]) {
        ++i;
    }
    return i;
}

/* 리프 노드에 정렬 상태를 유지하며 (id, row_index) 삽입.
 *
 * 규약:
 *   - 이미 동일 id 가 있으면 row_index 를 덮어쓴다 (중복 방지).
 *   - 리프 용량 (order - 1) 에 도달하지 않은 경우에만 삽입.
 *     full 이면 0 반환 — 호출자가 split 을 수행해야 한다 (Phase 4).
 *
 * 반환: 1=삽입/덮어쓰기 성공, 0=공간 부족. */
static int leaf_insert_nonfull(Node *leaf, int order, int id, int row_index) {
    assert(leaf->is_leaf);
    int i = 0;
    while (i < leaf->num_keys && leaf->keys[i] < id) ++i;
    if (i < leaf->num_keys && leaf->keys[i] == id) {
        leaf->u.leaf.row_indices[i] = row_index;
        return 1;
    }
    if (leaf->num_keys >= order - 1) return 0;
    for (int j = leaf->num_keys; j > i; --j) {
        leaf->keys[j] = leaf->keys[j - 1];
        leaf->u.leaf.row_indices[j] = leaf->u.leaf.row_indices[j - 1];
    }
    leaf->keys[i] = id;
    leaf->u.leaf.row_indices[i] = row_index;
    ++leaf->num_keys;
    return 1;
}

void bptree_insert(BPTree *tree, int id, int row_index) {
    if (!tree || !tree->root) return;
    Node *cur = tree->root;
    while (!cur->is_leaf) {
        cur = cur->u.internal.children[internal_child_idx(cur, id)];
    }
    /* Phase 3: split 없는 삽입. 리프가 가득 차면 현재 insert 는 무시.
     * Phase 4 에서 leaf split 추가 시 full 분기를 대체. */
    (void)leaf_insert_nonfull(cur, tree->order, id, row_index);
}

int bptree_search(BPTree *tree, int id) {
    if (!tree || !tree->root) return -1;
    Node *cur = tree->root;
    while (!cur->is_leaf) {
        cur = cur->u.internal.children[internal_child_idx(cur, id)];
    }
    for (int i = 0; i < cur->num_keys; ++i) {
        if (cur->keys[i] == id) {
            return cur->u.leaf.row_indices[i];
        }
    }
    return -1;
}

int bptree_range(BPTree *tree, int from, int to, int *out, int max_out) {
    (void)tree;
    (void)from;
    (void)to;
    (void)out;
    (void)max_out;
    return 0; /* 선택 구현. */
}

void bptree_print(BPTree *tree) {
    (void)tree;
    /* 디버그용, 선택 구현. */
}
