#include "my_malloc.h"
#include <unistd.h>

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

static pthread_mutex_t sbrk_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct BlockHeader {
  size_t size;                // payload size (bytes)
  int    free;                // 1 free, 0 used
  struct BlockHeader * next;  // next free block (free list)
} BlockHeader;

static __thread BlockHeader * tls_free_list = NULL; //free list per thread
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; //Only for sbrk()
static BlockHeader * volatile global_reclaim = NULL; // lock-free stack (CAS)
static BlockHeader * free_list = NULL;
static void insert_free_sorted_list(BlockHeader ** head, BlockHeader * b);

// track data segment size from first sbrk we call
static void * heap_start = NULL;
static void * heap_end   = NULL;

//take the upper multiple of eight
static size_t align8(size_t n) {
  return (n + 7u) & ~(size_t)7u;
}

static void record_heap_bounds(void * old_brk, size_t inc) {
  if (heap_start == NULL) heap_start = old_brk;
  void * new_brk = (char *)old_brk + inc;
  heap_end = new_brk;
}

static BlockHeader * request_from_os(size_t payload) {
  size_t total = sizeof(BlockHeader) + payload;
  void * old = sbrk(0);
  if (old == (void *)-1) return NULL;//sbrk fail

  void * res = sbrk((intptr_t)total);
  if (res == (void *)-1) return NULL;//sbrk fail

  record_heap_bounds(old, total);

  BlockHeader * b = (BlockHeader *)res;
  b->size = payload;
  b->free = 0;
  b->next = NULL;
  return b;
}

static void reclaim_push(BlockHeader * b){
  //push b to the global_reclaim stack
  BlockHeader * old;
  //only set head to b if head is still 'old'
  do{
    old = global_reclaim;
    b -> next = old;
  } while(!__sync_bool_compare_and_swap(&global_reclaim, old, b));
}

static BlockHeader * reclaim_pop(void){
  // pop from global_reclaim stack using CAS
  BlockHeader * old;
  BlockHeader * next;
  do{
    old = global_reclaim;
    if (old == NULL){
      return NULL;
    }
    next = old -> next;
  } while(!__sync_bool_compare_and_swap(&global_reclaim, old, next));
  old -> next = NULL;
  return old;

}

static void reclaim_drain_to_tls(int max_take) {
  for (int i = 0; i < max_take; i++) {
    BlockHeader * b = reclaim_pop();
    if (b == NULL) break;
    // b is a freed block; insert into THIS thread's TLS list
    insert_free_sorted_list(&tls_free_list, b);
  }
}

static void remove_from_list(BlockHeader ** head, BlockHeader * b) {
  //adjust from project1, not using free_list.
  if (*head == b) {
    *head = b->next;
    b->next = NULL;
    return;
  }
  for (BlockHeader * p = *head; p != NULL; p = p->next) {
    if (p->next == b) {
      p->next = b->next;
      b->next = NULL;
      return;
    }
  }
}

// coalesce with next free neighbors in the free list (free list kept sorted by addr)
static void coalesce_around_list(BlockHeader * b) {
  // merge with next if adjacent in memory
  while (b->next != NULL) {
    char * b_end = (char *)(b + 1) + b->size;
    if (b_end == (char *)b->next) {
      BlockHeader * n = b->next;
      b->size += sizeof(BlockHeader) + n->size;
      b->next = n->next;
    } else {
      break;
    }
  }
}

static void insert_free_sorted_list(BlockHeader ** head, BlockHeader * b) {
  b->free = 1;
  b->next = NULL;

  //set b to *head if there is no head/b is sorted before *head
  if (*head == NULL || b < *head) {
    b->next = *head;
    *head = b;
    coalesce_around_list(b);
    return;
  }

  BlockHeader * p = *head;
  while (p->next != NULL && p->next < b) {
    p = p->next;
  }
  b->next = p->next;
  p->next = b;

  // coalesce p with b if adjacent
  coalesce_around_list(b);  
  coalesce_around_list(p);
}

static void maybe_split_into_list(BlockHeader * b, size_t need, BlockHeader ** head) {
  if (b->size < need + sizeof(BlockHeader) + 8) return;

  char * base = (char *)(b + 1);
  BlockHeader * nb = (BlockHeader *)(base + need);
  nb->size = b->size - need - sizeof(BlockHeader);
  nb->free = 1;
  nb->next = NULL;

  b->size = need;

  insert_free_sorted_list(head, nb);
}

static BlockHeader * find_best_fit_list(BlockHeader * head, size_t need) {
  BlockHeader * best = NULL;
  size_t best_size = SIZE_MAX;

  for (BlockHeader * b = head; b != NULL; b = b->next) {
    if (b->free && b->size >= need && b->size < best_size) {
      best = b;
      best_size = b->size;
      if (best_size == need) break;
    }
  }
  return best;
}

void ts_free_nolock(void * ptr) {
  if (ptr == NULL) return;
  BlockHeader * b = ((BlockHeader *)ptr) - 1;
  b->free = 1;
  b->next = NULL;
  reclaim_push(b);
}

void * ts_malloc_nolock(size_t size) {
  if (size == 0) return NULL;
  size_t need = align8(size);

  // pull some globally freed blocks into this thread
  reclaim_drain_to_tls(32);

  // best-fit in this thread's TLS list
  BlockHeader * best = find_best_fit_list(tls_free_list, need);
  if (best != NULL) {
    remove_from_list(&tls_free_list, best);
    best->free = 0;
    maybe_split_into_list(best, need, &tls_free_list);
    return (void *)(best + 1);
  }

  // no suitable block -> request from OS (ONLY sbrk section may lock)
  pthread_mutex_lock(&sbrk_lock);
  BlockHeader * nb = request_from_os(need);
  pthread_mutex_unlock(&sbrk_lock);

  if (nb == NULL) return NULL;
  return (void *)(nb + 1);
}

unsigned long get_data_segment_size() {
  if (heap_start == NULL || heap_end == NULL) return 0;
  return (unsigned long)((char *)heap_end - (char *)heap_start);
}

unsigned long get_data_segment_free_space_size() {
  unsigned long sum = 0;
  for (BlockHeader * b = free_list; b != NULL; b = b->next) {
    if (b->free) sum += (unsigned long)b->size;
  }
  return sum;
}

void *ts_malloc_lock(size_t size) {
  if (size == 0) return NULL;
  size_t need = align8(size);

  pthread_mutex_lock(&g_lock);

  // best-fit on global free_list
  BlockHeader * best = find_best_fit_list(free_list, need);
  if (best != NULL) {
    remove_from_list(&free_list, best);
    best->free = 0;
    maybe_split_into_list(best, need, &free_list);
    pthread_mutex_unlock(&g_lock);
    return (void *)(best + 1);
  }

  // request from OS (safe because we still hold g_lock)
  BlockHeader * nb = request_from_os(need);
  pthread_mutex_unlock(&g_lock);

  if (nb == NULL) return NULL;
  return (void *)(nb + 1);
}

void ts_free_lock(void *ptr) {
  if (ptr == NULL) return;
  BlockHeader * b = ((BlockHeader *)ptr) - 1;

  pthread_mutex_lock(&g_lock);
  insert_free_sorted_list(&free_list, b);
  pthread_mutex_unlock(&g_lock);
}
