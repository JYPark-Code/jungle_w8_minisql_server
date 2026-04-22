#include "trie.h"

#include <stdlib.h>
#include <string.h>

typedef struct trie_node {
    struct trie_node *children[26];
    int row_index;
} trie_node_t;

struct trie {
    trie_node_t *root;
};

static trie_node_t *trie_node_create(void);
static void trie_node_destroy(trie_node_t *node);
static int char_index(char c);
static int word_is_valid(const char *word);
static int collect_prefix_rows(const trie_node_t *node, int *out, int max_out, int *filled);

trie_t *trie_create(void)
{
    trie_t *t = calloc(1U, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }

    t->root = trie_node_create();
    if (t->root == NULL) {
        free(t);
        return NULL;
    }

    return t;
}

void trie_destroy(trie_t *t)
{
    if (t == NULL) {
        return;
    }
    trie_node_destroy(t->root);
    free(t);
}

int trie_insert(trie_t *t, const char *word, int row_index)
{
    trie_node_t *node;
    const char *p;

    if (t == NULL || t->root == NULL || row_index < 0 || !word_is_valid(word)) {
        return -1;
    }

    node = t->root;
    for (p = word; *p != '\0'; ++p) {
        int idx = char_index(*p);
        if (node->children[idx] == NULL) {
            node->children[idx] = trie_node_create();
            if (node->children[idx] == NULL) {
                return -1;
            }
        }
        node = node->children[idx];
    }

    node->row_index = row_index;
    return 0;
}

int trie_search_exact(const trie_t *t, const char *word)
{
    const trie_node_t *node;
    const char *p;

    if (t == NULL || t->root == NULL || !word_is_valid(word)) {
        return -1;
    }

    node = t->root;
    for (p = word; *p != '\0'; ++p) {
        int idx = char_index(*p);
        node = node->children[idx];
        if (node == NULL) {
            return -1;
        }
    }

    return node->row_index;
}

int trie_search_prefix(const trie_t *t, const char *prefix,
                       int *out_row_indices, int max_out)
{
    const trie_node_t *node;
    const char *p;
    int filled = 0;

    if (t == NULL || t->root == NULL || out_row_indices == NULL ||
        max_out <= 0 || !word_is_valid(prefix)) {
        return 0;
    }

    node = t->root;
    for (p = prefix; *p != '\0'; ++p) {
        int idx = char_index(*p);
        node = node->children[idx];
        if (node == NULL) {
            return 0;
        }
    }

    if (collect_prefix_rows(node, out_row_indices, max_out, &filled) != 0) {
        return filled;
    }
    return filled;
}

static trie_node_t *trie_node_create(void)
{
    trie_node_t *node = calloc(1U, sizeof(*node));
    if (node != NULL) {
        node->row_index = -1;
    }
    return node;
}

static void trie_node_destroy(trie_node_t *node)
{
    int i;

    if (node == NULL) {
        return;
    }

    for (i = 0; i < 26; ++i) {
        trie_node_destroy(node->children[i]);
    }
    free(node);
}

static int char_index(char c)
{
    return c - 'a';
}

static int word_is_valid(const char *word)
{
    const char *p;

    if (word == NULL || word[0] == '\0') {
        return 0;
    }

    for (p = word; *p != '\0'; ++p) {
        if (*p < 'a' || *p > 'z') {
            return 0;
        }
    }
    return 1;
}

static int collect_prefix_rows(const trie_node_t *node, int *out, int max_out, int *filled)
{
    int i;

    if (node == NULL || out == NULL || filled == NULL) {
        return -1;
    }
    if (*filled >= max_out) {
        return 1;
    }

    if (node->row_index >= 0) {
        out[*filled] = node->row_index;
        (*filled)++;
        if (*filled >= max_out) {
            return 1;
        }
    }

    for (i = 0; i < 26; ++i) {
        if (node->children[i] != NULL &&
            collect_prefix_rows(node->children[i], out, max_out, filled) != 0) {
            return 1;
        }
    }

    return 0;
}
