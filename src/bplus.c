#include <stdlib.h> /* malloc */
#include <string.h> /* strlen */

#include "bplus.h"
#include "private/utils.h"


int bp__default_compare_cb(const bp_key_t* a, const bp_key_t* b);
int bp__default_filter_cb(const bp_key_t* key);


int bp_open(bp_tree_t* tree, const char* filename) {
  int ret;
  ret = bp__writer_create((bp__writer_t*) tree, filename);
  if (ret != BP_OK) return ret;

  tree->head.page = NULL;

  /*
   * Load head.
   * Writer will not compress data chunk smaller than head,
   * that's why we're passing head size as compressed size here
   */
  ret = bp__writer_find((bp__writer_t*) tree,
                        kNotCompressed,
                        BP__HEAD_SIZE,
                        &tree->head,
                        bp__tree_read_head,
                        bp__tree_write_head);
  if (ret != BP_OK) return ret;

  /* set default compare function */
  bp_set_compare_cb(tree, bp__default_compare_cb);

  return BP_OK;
}


int bp_close(bp_tree_t* tree) {
  int ret;
  ret = bp__writer_destroy((bp__writer_t*) tree);

  if (ret != BP_OK) return ret;
  if (tree->head.page != NULL) {
    bp__page_destroy(tree, tree->head.page);
    tree->head.page = NULL;
  }

  return BP_OK;
}


int bp_get(bp_tree_t* tree, const bp_key_t* key, bp_value_t* value) {
  return bp__page_get(tree, tree->head.page, (bp__kv_t*) key, value);
}


int bp_set(bp_tree_t* tree, const bp_key_t* key, const bp_value_t* value) {
  int ret;

  ret = bp__page_insert(tree, tree->head.page, (bp__kv_t*) key, value);
  if (ret != BP_OK) return ret;

  return bp__tree_write_head((bp__writer_t*) tree, &tree->head);
}


int bp_remove(bp_tree_t* tree, const bp_key_t* key) {
  int ret;
  ret = bp__page_remove(tree, tree->head.page, (bp__kv_t*) key);
  if (ret != BP_OK) return ret;

  return bp__tree_write_head((bp__writer_t*) tree, &tree->head);
}


int bp_compact(bp_tree_t* tree) {
  int ret;
  char* compacted_name;
  bp_tree_t compacted;
  bp__page_t* head_page;
  bp__page_t* head_copy;

  /* get name of compacted database (prefixed with .compact) */
  ret = bp__writer_compact_name((bp__writer_t*) tree, &compacted_name);
  if (ret != BP_OK) return ret;

  /* open it */
  ret = bp_open(&compacted, compacted_name);
  free(compacted_name);
  if (ret != BP_OK) return ret;

  /* for multi-threaded env */
  head_page = tree->head.page;

  /* clone head for thread safety */
  ret = bp__page_load(tree,
                      head_page->offset,
                      head_page->config,
                      &head_copy);
  if (ret != BP_OK) return ret;

  /* copy all pages starting from root */
  ret = bp__page_copy(tree, &compacted, head_copy);
  if (ret != BP_OK) return ret;

  /* compacted tree already has a head page, free it first */
  free(compacted.head.page);
  compacted.head.page = head_copy;

  ret = bp__tree_write_head((bp__writer_t*) &compacted, &compacted.head);
  if (ret != BP_OK) return ret;

  return bp__writer_compact_finalize((bp__writer_t*) tree,
                                     (bp__writer_t*) &compacted);
}


/* Wrappers to allow string to string set/get/remove */


int bp_gets(bp_tree_t* tree, const char* key, char** value) {
  int ret;
  bp_key_t bkey;
  bp_value_t bvalue;

  bkey.value = (char*) key;
  bkey.length = strlen(key) + 1;

  ret = bp_get(tree, &bkey, &bvalue);
  if (ret != BP_OK) return ret;

  *value = bvalue.value;

  return BP_OK;
}


int bp_sets(bp_tree_t* tree, const char* key, const char* value) {
  bp_key_t bkey;
  bp_value_t bvalue;

  bkey.value = (char*) key;
  bkey.length = strlen(key) + 1;

  bvalue.value = (char*) value;
  bvalue.length = strlen(value) + 1;

  return bp_set(tree, &bkey, &bvalue);
}


int bp_removes(bp_tree_t* tree, const char* key) {
  bp_key_t bkey;
  bkey.value = (char*) key;
  bkey.length = strlen(key) + 1;

  return bp_remove(tree, &bkey);
}


int bp_get_filtered_range(bp_tree_t* tree,
                          const bp_key_t* start,
                          const bp_key_t* end,
                          bp_filter_cb filter,
                          bp_range_cb cb) {
  return bp__page_get_range(tree,
                            tree->head.page,
                            (bp__kv_t*) start,
                            (bp__kv_t*) end,
                            filter,
                            cb);
}


int bp_get_filtered_ranges(bp_tree_t* tree,
                           const char* start,
                           const char* end,
                           bp_filter_cb filter,
                           bp_range_cb cb) {
  bp_key_t bstart;
  bp_key_t bend;

  bstart.value = (char*) start;
  bstart.length = strlen(start) + 1;

  bend.value = (char*) end;
  bend.length = strlen(end) + 1;

  return bp_get_filtered_range(tree, &bstart, &bend, filter, cb);
}


int bp_get_range(bp_tree_t* tree,
                 const bp_key_t* start,
                 const bp_key_t* end,
                 bp_range_cb cb) {
  return bp_get_filtered_range(tree, start, end, bp__default_filter_cb, cb);
}


int bp_get_ranges(bp_tree_t* tree,
                  const char* start,
                  const char* end,
                  bp_range_cb cb) {
  return bp_get_filtered_ranges(tree, start, end, bp__default_filter_cb, cb);
}


void bp_set_compare_cb(bp_tree_t* tree, bp_compare_cb cb) {
  tree->compare_cb = cb;
}


int bp__tree_read_head(bp__writer_t* w, void* data) {
  bp_tree_t* t = (bp_tree_t*) w;
  bp__tree_head_t* head = (bp__tree_head_t*) data;

  t->head.offset = ntohll(head->offset);
  t->head.config = ntohll(head->config);
  t->head.page_size = ntohll(head->page_size);
  t->head.hash = ntohll(head->hash);

  /* we've copied all data - free it */
  free(data);

  /* Check hash first */
  if (bp__compute_hashl(t->head.offset) != t->head.hash) return 1;

  return bp__page_load(t, t->head.offset, t->head.config, &t->head.page);
}


int bp__tree_write_head(bp__writer_t* w, void* data) {
  int ret;
  bp_tree_t* t = (bp_tree_t*) w;
  bp__tree_head_t nhead;
  uint64_t offset;
  uint64_t size;

  if (t->head.page == NULL) {
    /* TODO: page size should be configurable */
    t->head.page_size = 64;

    /* Create empty leaf page */
    ret = bp__page_create(t, kLeaf, 0, 0, &t->head.page);
    if (ret != BP_OK) return ret;
  }

  /* Update head's position */
  t->head.offset = t->head.page->offset;
  t->head.config = t->head.page->config;

  t->head.hash = bp__compute_hashl(t->head.offset);

  /* Create temporary head with fields in network byte order */
  nhead.offset = htonll(t->head.offset);
  nhead.config = htonll(t->head.config);
  nhead.page_size = htonll(t->head.page_size);
  nhead.hash = htonll(t->head.hash);

  size = BP__HEAD_SIZE;
  ret = bp__writer_write(w,
                         kNotCompressed,
                         &nhead,
                         &offset,
                         &size);

  return ret;
}


int bp__default_compare_cb(const bp_key_t* a, const bp_key_t* b) {
  uint32_t i, len = a->length < b->length ? a->length : b->length;

  for (i = 0; i < len; i++) {
    if (a->value[i] != b->value[i]) {
      return (uint8_t) a->value[i] > (uint8_t) b->value[i] ? 1 : -1;
    }
  }

  return a->length - b->length;
}


int bp__default_filter_cb(const bp_key_t* key) {
  /* default filter accepts all keys */
  return 1;
}
