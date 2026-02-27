#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "my_malloc.h"

#ifndef NUM_THREADS
#define NUM_THREADS 8
#endif

#ifndef ITERS
#define ITERS 20000
#endif

#define MAX_SIZE 2048

#ifdef LOCK_VERSION
#define MALLOC(sz) ts_malloc_lock(sz)
#define FREE(p)    ts_free_lock(p)
#else
#define MALLOC(sz) ts_malloc_nolock(sz)
#define FREE(p)    ts_free_nolock(p)
#endif

typedef struct {
  void *ptr;
} Item;

static Item *shared;
static pthread_barrier_t barrier;

static inline unsigned int mix(unsigned int x) {
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}

void *worker(void *arg) {
  long tid = (long)arg;
  unsigned int seed = (unsigned int)(time(NULL) ^ (tid * 0x9e3779b9u));

  pthread_barrier_wait(&barrier);

  // allocate: each thread fills its own stripe
  for (int i = (int)tid; i < ITERS; i += NUM_THREADS) {
    size_t sz = (rand_r(&seed) % MAX_SIZE) + 1;
    shared[i].ptr = MALLOC(sz);
  }

  pthread_barrier_wait(&barrier);

  // cross-thread free: each thread frees a different stripe (shifted by 1)
  long owner = (tid + 1) % NUM_THREADS;
  for (int i = (int)owner; i < ITERS; i += NUM_THREADS) {
    void *p = shared[i].ptr;
    if (p != NULL) {
      FREE(p);
      shared[i].ptr = NULL;
    }
  }

  return NULL;
}

int main(void) {
  pthread_t th[NUM_THREADS];

  shared = calloc(ITERS, sizeof(Item));
  if (!shared) {
    perror("calloc");
    return 1;
  }

  pthread_barrier_init(&barrier, NULL, NUM_THREADS);

  for (long i = 0; i < NUM_THREADS; i++) {
    pthread_create(&th[i], NULL, worker, (void *)i);
  }

  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_join(th[i], NULL);
  }

  // cleanup any leftovers (should be none, but safe)
  for (int i = 0; i < ITERS; i++) {
    if (shared[i].ptr != NULL) {
      FREE(shared[i].ptr);
      shared[i].ptr = NULL;
    }
  }

  pthread_barrier_destroy(&barrier);
  free(shared);

  printf("stress2 (cross-thread free) done\n");
  return 0;
}
