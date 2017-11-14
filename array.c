#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "dlog.h"
#include "array.h"
#include "dmem.h"

static void array_grow(array_t * a, int min_capacity);
static void array_init(array_t * a);

array_t *array_create(void) {
    array_t *a = xmalloc(sizeof(array_t));
    if (!a) daemon_log(LOG_ERR, "virtual memory exhausted in function \"%s\" al line %d", __FUNCTION__, __LINE__);
    array_init(a);
    return a;
}

static void array_init(array_t * a) {
    if (a == NULL)
        return;
    memset(a, 0, sizeof(array_t));
    pthread_mutex_init(&a->mtx, NULL);
}

void array_lock(array_t * a) {
    if (a == NULL)
        return;
    pthread_mutex_lock(&a->mtx);
}

void array_unlock(array_t * a) {
    if (a == NULL)
        return;
    pthread_mutex_unlock(&a->mtx);
}

static void _array_del_last(array_t * a) {
    if (!a) return;
    if (a->count) {
        a->items[a->count - 1] = NULL;
        a->count--;
    }
}

void array_clean(array_t * a, free_func_t free_func, freep_func_t freep_func) {
    void * p;
    int count;
    if (a == NULL)
        return;
    /* could also warn if some objects are left */
    pthread_mutex_lock(&a->mtx);
    while ((count = array_getcount(a)) > 0) {
        p = array_getitem(a, count - 1);
        _array_del_last(a);
        if (p) {
            if (free_func) {
                free_func(p);
            } else {
                if (freep_func) {
                    freep_func(&p);
                } else {
                    FREE(p);
                }
            }
        }
    }
    FREE(a->items);
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
    pthread_mutex_unlock(&a->mtx);
}

void array_destroy(array_t ** a, free_func_t free_func, freep_func_t freep_func) {

    if ((a == NULL) || (*a == NULL))
        return;

    array_t *tmp = *a;

    pthread_mutex_lock(&tmp->mtx);
    *a = NULL;
    pthread_mutex_unlock(&tmp->mtx);
    array_clean(tmp, free_func, freep_func);
    pthread_mutex_destroy(&tmp->mtx);
    FREE(tmp);
}

void array_append(array_t * a, void *obj) {
    if (a == NULL)
        return;

    pthread_mutex_lock(&a->mtx);
    if (a->count >= a->capacity)
        array_grow(a, a->count + 1);
    a->items[a->count++] = obj;
    pthread_mutex_unlock(&a->mtx);
}

void array_insert(array_t *a, void *obj, int position) {
    if (a == NULL)
        return;
    pthread_mutex_lock(&a->mtx);

    if (a->count >= a->capacity)
        array_grow(a, a->count + 1);
    if (position > a->count)
        position = a->count;
    if (position < a->count)
        memmove(&a->items[position + 1], &a->items[position], (size_t)(a->count - position) * sizeof(void *));
    a->items[position] = obj;
    (a->count)++;

    pthread_mutex_unlock(&a->mtx);
}

bool array_del(array_t * a, void *obj) {
    bool ret = false;

    if (a == NULL)
        return(ret);

    int pos = 0;

    pthread_mutex_lock(&a->mtx);
    for (pos = 0; pos < a->count; ++pos) {
        if (obj == a->items[pos]) {
            ret = true;
            if (pos == (a->count - 1)) {
                a->items[pos] = NULL;
            } else {
                memmove(&a->items[pos], &a->items[pos + 1], (size_t)(a->count - pos) * sizeof(void *));
            }
            a->count--;
        }
    }
    pthread_mutex_unlock(&a->mtx);
    return(ret);
}

void array_pop(array_t * a) {
    if (a == NULL)
        return;

    pthread_mutex_lock(&a->mtx);

    if (a->count > 0) {
        a->count--;
    }
    pthread_mutex_unlock(&a->mtx);
}

static void array_del_no_lock(array_t * a, void *obj) {
    if (a == NULL)
        return;
    int pos = 0;
    for (pos = 0; pos < a->count; ++pos) {
        if (obj == a->items[pos]) {
            if (pos == (a->count - 1)) {
                a->items[pos] = NULL;
            } else {
                memmove(&a->items[pos], &a->items[pos + 1], (size_t)(a->count - pos) * sizeof(void *));
            }

            a->count--;
        }
    }
}

/* if you are going to append a known and large number of items, call this first */
void array_preappend(array_t * a, const int app_count) {
    if (a == NULL)
        return;
    pthread_mutex_lock(&a->mtx);
    if (a->count + app_count > a->capacity)
        array_grow(a, a->count + app_count);
    pthread_mutex_unlock(&a->mtx);
}

void array_resize(array_t * a, int count) {
    if (a == NULL)
        return;

    pthread_mutex_lock(&a->mtx);
    if (count >= a->capacity) {
        array_grow(a, a->count + 1);
    } else if (count < a->count) {
        memset(a, 0, (size_t)(a->count - count));
    }

    a->count = count;
    pthread_mutex_unlock(&a->mtx);
}

int  array_getcount(array_t *  a) {
    if (a == NULL)
        return -1;
    return a->count;
}

void*  array_getitem( array_t * a, const int position) {
    if (a == NULL)
        return NULL;
    if (position >= a->count || position < 0)
        return NULL;
    else
        return a->items[position];
}

typedef  int(*cfunc_std_t)(const void *, const void *, void *);

void array_qsort( array_t * a, cfunc_t cfunc) {
    if (a == NULL)
        return;

    pthread_mutex_lock(&a->mtx);
    qsort_r(a->items, a->count, sizeof(a->items[0]), (cfunc_std_t)cfunc, a);
    pthread_mutex_unlock(&a->mtx);
}

/* grows internal buffer to satisfy required minimal capacity */
static void array_grow(array_t * a, int min_capacity) {
    const int min_delta = 16;
    int delta;

    if (a->capacity >= min_capacity) {
        daemon_log(LOG_ERR, "a->capacity(%d) >= min_capacity(%d)", a->capacity, min_capacity);
    }

    delta = min_capacity;
    /* make delta a multiple of min_delta */
    delta += min_delta - 1;
    delta /= min_delta;
    delta *= min_delta;

    /* actual grow */
    if (delta <= 0) {
        daemon_log(LOG_ERR, "delta(%d) <= 0", delta);
    }

    a->capacity += delta;
    a->items = a->items ?
               xrealloc(a->items, (size_t)a->capacity * sizeof(void *)) :
               xmalloc((size_t)a->capacity * sizeof(void *));
    if (!a->items) daemon_log(LOG_ERR, "virtual memory exhausted in function \"%s\" al line %d", __FUNCTION__, __LINE__);

    /* reset, just in case */
    if (a->items) {
        memset(a->items + a->count, 0, (size_t) (a->capacity - a->count) * sizeof(void *));
    }
}



