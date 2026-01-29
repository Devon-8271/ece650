#include "my_malloc.h"
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>

typedef struct BlockHeader {
  size_t size;                // payload size (bytes)
  int    free;                // 1 free, 0 used
  struct BlockHeader * next;  // next free block (free list)
} BlockHeader;

static BlockHeader * free_list = NULL;

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

// split b into [b payload size = need] + [new free block] if enough space
static void maybe_split(BlockHeader * b, size_t need) {
  // require room for a header + at least 8 bytes payload in remainder
  if (b->size < need + sizeof(BlockHeader) + 8) return;

  char * base = (char *)(b + 1);               // start of payload
  BlockHeader * nb = (BlockHeader *)(base + need);
  nb->size = b->size - need - sizeof(BlockHeader);
  nb->free = 1;
  nb->next = NULL;

  b->size = need;

  // insert nb into free list (by address)
  void * nb_payload = (void *)(nb + 1);
  ff_free(nb_payload);
}

static void remove_from_free_list(BlockHeader * b) {
  if (free_list == b) {
    // if b is the head of the list
    free_list = b->next;
    b->next = NULL;
    return;
  }
  for (BlockHeader * p = free_list; p != NULL; p = p->next) {
    if (p->next == b) {
      p->next = b->next;
      b->next = NULL;
      return;
    }
  }
}

// coalesce with next free neighbors in the free list (free list kept sorted by addr)
static void coalesce_around(BlockHeader * b) {
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

void * ff_malloc(size_t size) {
  if (size == 0) return NULL;
  size_t need = align8(size);

  // first-fit search
  for (BlockHeader * b = free_list; b != NULL; b = b->next) {
    if (b->free && b->size >= need) {
      remove_from_free_list(b);
      b->free = 0;
      maybe_split(b, need);
      return (void *)(b + 1);
    }
  }

  BlockHeader * nb = request_from_os(need);
  //directly give user, do not add to freelist
  if (nb == NULL) return NULL;
  return (void *)(nb + 1);
}

void ff_free(void * ptr) {
  if (ptr == NULL) return;
  BlockHeader * b = ((BlockHeader *)ptr) - 1;
  b->free = 1;
  b->next = NULL;

  // insert into free list sorted by address
  if (free_list == NULL || b < free_list) {
    b->next = free_list;
    free_list = b;
    coalesce_around(b);
    return;
  }

  BlockHeader * p = free_list;
  while (p->next != NULL && p->next < b) {
    p = p->next;
  }
  //now p->next >b, insert to have p-> b->(p->nxt)
  b->next = p->next;
  p->next = b;

  // coalesce p with b if adjacent
  coalesce_around(b);
  coalesce_around(p); // p might now be adjacent to b
}


void * bf_malloc(size_t size) {
  if (size == 0) return NULL;
  size_t need = align8(size);

  //best-fit search
  BlockHeader * best = NULL;
  size_t best_size = SIZE_MAX;
  for (BlockHeader * b = free_list; b!= NULL; b = b->next){
    if (b->free && b->size >= need){
        if (best_size > b->size){
          best = b;
          best_size = b->size;
          if (best_size == need){
            break;
          }
        }
    }
  }
  if (best_size < SIZE_MAX){
    remove_from_free_list(best);
    best->free = 0;
    maybe_split(best, need);
    return (void *)(best+1);
  }

  BlockHeader * nb = request_from_os(need);
  if (nb == NULL) return NULL;
  return (void *)(nb + 1);
}

void bf_free(void * ptr) {
  ff_free(ptr);
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
