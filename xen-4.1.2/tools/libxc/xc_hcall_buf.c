/*
 * Copyright (c) 2010, Citrix Systems, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdlib.h>
#include <malloc.h>
#include <pthread.h>
#include <sys/time.h>


#include "xc_private.h"
#include "xg_private.h"

xc_hypercall_buffer_t XC__HYPERCALL_BUFFER_NAME(HYPERCALL_BUFFER_NULL) = {
    .hbuf = NULL,
    .param_shadow = NULL,
    HYPERCALL_BUFFER_INIT_NO_BOUNCE
};

/*
 * Spin Lock 
 * */
#if 0
typedef signed short s16;
typedef struct {
    volatile s16 lock;
} raw_spinlock_t;
#define _RAW_SPIN_LOCK_UNLOCKED /*(raw_spinlock_t)*/ { 1 }
#define _raw_spin_is_locked(x) ((x)->lock <= 0)
raw_spinlock_t hypercall_buffer_cache_spin = _RAW_SPIN_LOCK_UNLOCKED;

static inline int _raw_spin_trylock(raw_spinlock_t *lock)
{
    s16 oldval;
    asm volatile (
        "xchgw %w0,%1"
        :"=r" (oldval), "=m" (lock->lock)
        :"0" (0) : "memory" );
    return (oldval > 0);
}

static inline void rep_nop(void)
{
    asm volatile ( "rep;nop" : : : "memory" );
}
#define cpu_relax() rep_nop()

static void spin_lock(raw_spinlock_t *l){
    while ( !_raw_spin_trylock(l) )
    {
        while ( _raw_spin_is_locked(l) )
            cpu_relax();
    }
}

static void spin_unlock(raw_spinlock_t *l){
	asm volatile (
			"movw $1,%0" 
			: "=m" (l->lock) : : "memory" );
}
#endif
/*
 * Spin Lock End
 * */

pthread_mutex_t hypercall_buffer_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static void hypercall_buffer_cache_lock(xc_interface *xch)
{
    if ( xch->flags & XC_OPENFLAG_NON_REENTRANT )
        return;
    pthread_mutex_lock(&hypercall_buffer_cache_mutex);
	//spin_lock(&hypercall_buffer_cache_spin);
}

static void hypercall_buffer_cache_unlock(xc_interface *xch)
{
    if ( xch->flags & XC_OPENFLAG_NON_REENTRANT )
        return;
    pthread_mutex_unlock(&hypercall_buffer_cache_mutex);
	//spin_unlock(&hypercall_buffer_cache_spin);
}

static void *hypercall_buffer_cache_alloc(xc_interface *xch, int nr_pages)
{
    void *p = NULL;

    hypercall_buffer_cache_lock(xch);

    xch->hypercall_buffer_total_allocations++;
    xch->hypercall_buffer_current_allocations++;
    if ( xch->hypercall_buffer_current_allocations > xch->hypercall_buffer_maximum_allocations )
        xch->hypercall_buffer_maximum_allocations = xch->hypercall_buffer_current_allocations;

    if ( nr_pages > 1 )
    {
        xch->hypercall_buffer_cache_toobig++;
    }
    else if ( xch->hypercall_buffer_cache_nr > 0 )
    {
        p = xch->hypercall_buffer_cache[--xch->hypercall_buffer_cache_nr];
        xch->hypercall_buffer_cache_hits++;
    }
    else
    {
        xch->hypercall_buffer_cache_misses++;
    }

    hypercall_buffer_cache_unlock(xch);

    return p;
}

static int hypercall_buffer_cache_free(xc_interface *xch, void *p, int nr_pages)
{
    int rc = 0;

    hypercall_buffer_cache_lock(xch);

    xch->hypercall_buffer_total_releases++;
    xch->hypercall_buffer_current_allocations--;

    if ( nr_pages == 1 && xch->hypercall_buffer_cache_nr < HYPERCALL_BUFFER_CACHE_SIZE )
    {
        xch->hypercall_buffer_cache[xch->hypercall_buffer_cache_nr++] = p;
        rc = 1;
    }

    hypercall_buffer_cache_unlock(xch);

    return rc;
}

__thread char *local_malloc_buf;
static void do_hypercall_buffer_free_pages(void *ptr, int nr_pages)
{
#ifndef __sun__
    (void) munlock(ptr, nr_pages * PAGE_SIZE);
#endif

	if ( ptr != local_malloc_buf )
		free(ptr);
}

void xc__hypercall_buffer_cache_release(xc_interface *xch)
{
    void *p;

    hypercall_buffer_cache_lock(xch);

    DBGPRINTF("hypercall buffer: total allocations:%d total releases:%d",
              xch->hypercall_buffer_total_allocations,
              xch->hypercall_buffer_total_releases);
    DBGPRINTF("hypercall buffer: current allocations:%d maximum allocations:%d",
              xch->hypercall_buffer_current_allocations,
              xch->hypercall_buffer_maximum_allocations);
    DBGPRINTF("hypercall buffer: cache current size:%d",
              xch->hypercall_buffer_cache_nr);
    DBGPRINTF("hypercall buffer: cache hits:%d misses:%d toobig:%d",
              xch->hypercall_buffer_cache_hits,
              xch->hypercall_buffer_cache_misses,
              xch->hypercall_buffer_cache_toobig);

    while ( xch->hypercall_buffer_cache_nr > 0 )
    {
        p = xch->hypercall_buffer_cache[--xch->hypercall_buffer_cache_nr];
        do_hypercall_buffer_free_pages(p, 1);
    }

    hypercall_buffer_cache_unlock(xch);
}

__thread struct timeval malloc_time;
__thread struct timeval malloc_time_end;
__thread unsigned long long total_malloc_time;

static unsigned long
time_between(struct timeval begin, struct timeval end)
{
	    return (end.tv_sec - begin.tv_sec) * 1000000 + (end.tv_usec - begin.tv_usec);
}

int is_migrate = 0;

void *xc__hypercall_buffer_alloc_pages(xc_interface *xch, xc_hypercall_buffer_t *b, int nr_pages)
{
    size_t size = nr_pages * PAGE_SIZE;
    void *p = hypercall_buffer_cache_alloc(xch, nr_pages);
	
	if (!p && nr_pages <= 10 && is_migrate) {
		p = local_malloc_buf;
	}

	gettimeofday(&malloc_time, NULL);
    if ( !p ) {
#if defined(_POSIX_C_SOURCE) && !defined(__sun__)
        int ret;
        ret = posix_memalign(&p, PAGE_SIZE, size);
        if (ret != 0)
            return NULL;
#elif defined(__NetBSD__) || defined(__OpenBSD__)
        p = valloc(size);
#else
        p = memalign(PAGE_SIZE, size);
#endif

        if (!p)
            return NULL;

#ifndef __sun__
        if ( mlock(p, size) < 0 )
        {
            free(p);
            return NULL;
        }
#endif
    }
	gettimeofday(&malloc_time_end, NULL);
	total_malloc_time += time_between(malloc_time, malloc_time_end);

    b->hbuf = p;

    memset(p, 0, size);
    return b->hbuf;
}

void xc__hypercall_buffer_free_pages(xc_interface *xch, xc_hypercall_buffer_t *b, int nr_pages)
{
    if ( b->hbuf == NULL )
        return;

	if ( b->hbuf == local_malloc_buf )
		return;
    if ( !hypercall_buffer_cache_free(xch, b->hbuf, nr_pages) )
        do_hypercall_buffer_free_pages(b->hbuf, nr_pages);
}

struct allocation_header {
    int nr_pages;
};

void *xc__hypercall_buffer_alloc(xc_interface *xch, xc_hypercall_buffer_t *b, size_t size)
{
    size_t actual_size = ROUNDUP(size + sizeof(struct allocation_header), PAGE_SHIFT);
    int nr_pages = actual_size >> PAGE_SHIFT;
    struct allocation_header *hdr;

    hdr = xc__hypercall_buffer_alloc_pages(xch, b, nr_pages);
    if ( hdr == NULL )
        return NULL;

    b->hbuf = (void *)(hdr+1);

    hdr->nr_pages = nr_pages;
    return b->hbuf;
}

void xc__hypercall_buffer_free(xc_interface *xch, xc_hypercall_buffer_t *b)
{
    struct allocation_header *hdr;

    if (b->hbuf == NULL)
        return;

    hdr = b->hbuf;
    b->hbuf = --hdr;

    xc__hypercall_buffer_free_pages(xch, b, hdr->nr_pages);
}

int xc__hypercall_bounce_pre(xc_interface *xch, xc_hypercall_buffer_t *b)
{
    void *p;

    /*
     * Catch hypercall buffer declared other than with DECLARE_HYPERCALL_BOUNCE.
     */
    if ( b->ubuf == (void *)-1 || b->dir == XC_HYPERCALL_BUFFER_BOUNCE_NONE )
        abort();

    /*
     * Do need to bounce a NULL buffer.
     */
    if ( b->ubuf == NULL )
    {
        b->hbuf = NULL;
        return 0;
    }

	//fprintf(stderr, "b->sz = %lu", b->sz);
    p = xc__hypercall_buffer_alloc(xch, b, b->sz);
    if ( p == NULL )
        return -1;

    if ( b->dir == XC_HYPERCALL_BUFFER_BOUNCE_IN || b->dir == XC_HYPERCALL_BUFFER_BOUNCE_BOTH )
        memcpy(b->hbuf, b->ubuf, b->sz);

    return 0;
}

void xc__hypercall_bounce_post(xc_interface *xch, xc_hypercall_buffer_t *b)
{
    /*
     * Catch hypercall buffer declared other than with DECLARE_HYPERCALL_BOUNCE.
     */
    if ( b->ubuf == (void *)-1 || b->dir == XC_HYPERCALL_BUFFER_BOUNCE_NONE )
        abort();

    if ( b->hbuf == NULL )
        return;

    if ( b->dir == XC_HYPERCALL_BUFFER_BOUNCE_OUT || b->dir == XC_HYPERCALL_BUFFER_BOUNCE_BOTH )
        memcpy(b->ubuf, b->hbuf, b->sz);

    xc__hypercall_buffer_free(xch, b);
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
