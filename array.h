#include <pthread.h>
#include <stdbool.h>

#ifndef ARRAY_H
#define ARRAY_H

typedef struct ARRAY_T {
    int capacity;
    int count;
    pthread_mutex_t mtx;
    void **items;
} array_t;

typedef void (* free_func_t)(void *);
typedef void (* freep_func_t)(void **);
typedef  int (*cfunc_t)(const void *, const void *, array_t *);


extern array_t *array_create(void);
extern void array_destroy(array_t ** s, free_func_t free_func, freep_func_t freep_func);
extern void array_clean(array_t * s, free_func_t free_func, freep_func_t freep_func);
extern void array_append(array_t * s, void *obj);
extern void array_insert(array_t * s, void *obj, int position);
extern bool array_del(array_t * s, void *obj);
void array_pop(array_t * a);
//extern void array_del_no_lock(array_t * a, void *obj);
extern void array_resize(array_t * s, int count);
extern void array_preappend(array_t * s, const int app_count);
extern int  array_getcount(array_t * a);
extern void*  array_getitem(array_t * a, const int position);
extern void array_lock(array_t * a);
extern void array_unlock(array_t * a);

extern void array_qsort( array_t * a, cfunc_t cfunc);

#define array_for_each(array, tmp_int) \
	for (tmp_int=0; tmp_int<array_getcount(array); tmp_int++)

#endif
