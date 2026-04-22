/* trie.h -- Round 2 dictionary prefix index.
 *
 * The stored value is a storage row_index, not the SQL id column.
 * Words are restricted to lowercase ASCII a-z.
 */

#ifndef TRIE_H
#define TRIE_H

typedef struct trie trie_t;

trie_t *trie_create(void);
void    trie_destroy(trie_t *t);

int trie_insert(trie_t *t, const char *word, int row_index);
int trie_search_exact(const trie_t *t, const char *word);
int trie_search_prefix(const trie_t *t, const char *prefix,
                       int *out_row_indices, int max_out);

#endif /* TRIE_H */
