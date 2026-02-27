#include "my_malloc.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef NUM_THREADS
#define NUM_THREADS 8
#endif
#ifndef ITERS
#define ITERS 200000
#endif
#ifndef MAXSZ
#define MAXSZ 4096
#endif

#ifdef LOCK_VERSION
#define MALLOC(sz) ts_malloc_lock(sz)
#define FREE(p) ts_free_lock(p)
#else
#define MALLOC(sz) ts_malloc_nolock(sz)
#define FREE(p) ts_free_nolock(p)
#endif

static inline uint32_t xorshift32(uint32_t *s) {
  uint32_t x = *s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *s = x;
  return x;
}

void *worker(void *arg) {
  uintptr_t tid = (uintptr_t)arg;
  uint32_t seed = (uint32_t)(time(NULL) ^ (tid * 0x9e3779b9u));

  void *p1 = NULL, *p2 = NULL, *p3 = NULL;

  for (int i = 0; i < ITERS; i++) {
    size_t sz = (xorshift32(&seed) % MAXSZ) + 1;
    void *p = MALLOC(sz);
    if (!p) continue;

    uint8_t v = (uint8_t)(tid ^ i);
    for (size_t k = 0; k < sz; k += 64) ((uint8_t *)p)[k] = v;

    uint32_t r = xorshift32(&seed);
    if ((r & 3u) == 0 && p1) { FREE(p1); p1 = NULL; }
    if ((r & 3u) == 1 && p2) { FREE(p2); p2 = NULL; }
    if ((r & 3u) == 2 && p3) { FREE(p3); p3 = NULL; }

    if (!p1) p1 = p;
    else if (!p2) p2 = p;
    else if (!p3) p3 = p;
    else FREE(p);
  }

  if (p1) FREE(p1);
  if (p2) FREE(p2);
  if (p3) FREE(p3);
  return NULL;
}

int main() {
  pthread_t th[NUM_THREADS];
  for (uintptr_t i = 0; i < NUM_THREADS; i++) {
    pthread_create(&th[i], NULL, worker, (void *)i);
  }
  for (int i = 0; i < NUM_THREADS; i++) pthread_join(th[i], NULL);
  printf("stress1 done\n");
  return 0;
}
