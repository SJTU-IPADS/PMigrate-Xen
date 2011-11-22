/*
 * multi-core queue support
 */
#include <malloc.h>

#include "mc_queue.h"

/*
 * allocating a queue
 */
struct sync_queue *alloc_queue(int num_entries) {
    struct sync_queue *tmp;
    
    tmp = (struct sync_queue *)malloc(2 * sizeof(atomic_t) + sizeof(struct sync_entry) * num_entries);
    return tmp;
}

/*
 * enqueue operation
 * add any param of sync_entry if necessary
 */
void enqueue(struct sync_queue *queue, int len) {
    int index = atomic_read(&queue->total_index);

    queue->sync_list[index].len = len;
    atomic_inc(&queue->total_index);
    return;
}

/*
 * dequeue operation
 *
 * return NULL if there is no work left
 * return struct sync_entry * if we get one object
 */
struct sync_entry *dequeue(struct sync_queue *queue) {
    struct sync_entry *entry;
    int work_index = atomic_return_and_inc(&queue->consume_index);
    int total_index = atomic_read(&queue->total_index);

    if (work_index <= total_index) {
        entry = &(queue->sync_list[work_index]);
    } else {
        entry = NULL;
    }
    
    return entry;
}

/*
 * test whether the queue is empty
 */
int has_obj(struct sync_queue *queue) {
    int consume_index = atomic_read(&queue->consume_index);
    int total_index = atomic_read(&queue->total_index);

    return consume_index < total_index;
}
