/*
 * Support for xc_domain_save.c
 */

#ifndef MC_MA_SL_H
#define MC_MA_SL_H
#endif

#include <pthread.h>

#define NUM_THREADS 2

static pthread_mutex_t ms_mutex; /* global mutex lock */
static pthread_cond_t queue_threshold_cv; /* queue not empty cv */
static pthread_cond_t slave_threshold_cv; /* slave next iter cv */
static pthread_cond_t master_threshold_cv; /* master next iter cv */

static int sl_cont; /* finish this iter slave number */
static int error_out; /* boolean for mem tran error */


struct slave_arg{
            int io_fd_s;
            int hvm_s;
            int debug_s;
            int live;
            uint32_t dom_s;

            unsigned long * to_send_s;
            unsigned long * to_skip_s;
            unsigned long * to_fix_s;

            xc_interface * xch_s;

	    struct sync_queue * queue;
	    struct outbuf *ob;
	    struct save_ctx *ctx;
        };

struct slave_ret {
    unsigned int send_this_batch;
    unsigned int skip_this_batch;
};

struct recieve_arg{
    //pagebuf_t *pagebuf;
    int io_fd;
//    domain_info_context *dinfo;    
    xc_interface *xch;
    struct restore_ctx *ctx;
    uint32_t dom;
    struct sync_queue *queue;
};

struct recieve_ret{ /* I don't know what else is needed currently */
    unsigned int recieved;
};

void* slave_fun(void * arg);

void* recieve_fun(void * arg);
