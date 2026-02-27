#ifndef MY_MALLOC_H
#define MY_MALLOC_H

#include <stddef.h>

void * ff_malloc(size_t size);
void   ff_free(void * ptr);

void * bf_malloc(size_t size);
void   bf_free(void * ptr);

void *ts_malloc_lock(size_t size);
void ts_free_lock(void *ptr);

void *ts_malloc_nolock(size_t size);
void ts_free_nolock(void *ptr);

unsigned long get_data_segment_size();
unsigned long get_data_segment_free_space_size();

#endif
