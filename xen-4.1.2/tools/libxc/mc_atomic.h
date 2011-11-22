/*
 * atomic operation support for mc_queue.c
 */

#ifndef MC_ATOMIC_H
#define MC_ATOMIC_H

/*
 * The volatile qualifier is in the operations
 */
typedef struct { int counter; } atomic_t;

/**
 * atomic_read - read atomic variable
 * @v: pointer of type atomic_t
 * 
 * Atomically reads the value of @v.
 */
static inline int atomic_read(const atomic_t *v)
{
	return (*(volatile int *)&(v)->counter);
}

/**
 * atomic_set - set atomic variable
 * @v: pointer of type atomic_t
 * @i: required value
 * 
 * Atomically sets the value of @v to @i.
 */ 
static inline void atomic_set(atomic_t *v, int i)
{
	v->counter = i;
}

/**
 * atomic_inc - increment atomic variable
 * @v: pointer of type atomic_t
 *
 * Atomically increments @v by 1.
 */
static inline void atomic_inc(atomic_t *v)
{
	asm volatile("lock; incl %0"
                 : "+m" (*(volatile int *)&v->counter));
}

/**
 * atomic_inc_and_return - increment and read 
 * @v: pointer of type atomic_t
 * 
 * Atomically increments @v by 1
 * and returns the increased value 
 */ 
static inline int atomic_return_and_inc(atomic_t *v)
{
    int i = 1;

    asm volatile (
        "lock; xaddl %0, %1"
        : "+r" (i), "+m" (*(volatile int *)&v->counter)
        :
        : "memory" );
    return i;
}

#endif
