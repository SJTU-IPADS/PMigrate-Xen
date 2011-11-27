/*
 * Support for mc_queue.c
 */

#ifndef MC_QUEUE_H
#define MC_QUEUE_H

#include "mc_atomic.h"

struct sync_entry {
    /*
     * fill it up with necessary variables
     */
    int last_iter;            /* boolean if last iter */ 
    int iter; 		      /* iter number */	
    int start_pfn;	      /* batch mem page start number */
    int len;                  /* length of a batch */
};

struct sync_queue {
    atomic_t total_index;
    atomic_t consume_index;

    struct sync_entry sync_list[0];
};

extern struct sync_queue *alloc_queue(int num_entries);
extern void enqueue(struct sync_queue *queue,  int last_iter, int iter, int start_pfn, int len);
extern struct sync_entry *dequeue(struct sync_queue *queue);
extern int has_obj(struct sync_queue *queue);
#endif
