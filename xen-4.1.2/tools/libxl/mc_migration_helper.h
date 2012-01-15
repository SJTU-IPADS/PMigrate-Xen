#ifndef __MC_MIGRATION_HELPER_H__
#define __MC_MIGRATION_HELPER_H__
#include <pthread.h>
#include <stdio.h>
#include "../libxc/xenctrl.h"

#define BUFFER_INIT_SIZE 10
#define DEFAULT_PORT 3344
#define PAUSE while(1)
#define MULTI_TRY 10

#define SLEEP_SHORT_TIME 1000
#define SLEEP_LONG_TIME 1000000

enum {mc_migrate_debug = 0};
enum {mc_migrate_hint = 1};

typedef struct {
	int cnt;
	pthread_mutex_t mutex;
	pthread_barrier_t barr;
} banner_t;

struct outbuf {
    void* buf;
    size_t size;
    size_t pos;
};

typedef struct {
	unsigned int batch;
	unsigned long *pfn_batch;
	int *pfn_err;
	//xen_pfn_t *pfn_type;
	unsigned long *pfn_type;
	int hvm;
    struct domain_info_context* dinfo;
	struct save_ctx *ctx;
	xc_interface *xch;
	int debug;
	int iter;
    unsigned char *region_base;
	int last_iter;
    struct outbuf ob;
	int live;
	char *page;
} send_argu_t;

struct list_item {
	void* item;
	struct list_item* next;
	struct list_item* prev;
};

typedef struct {
    void* pages;
    /* pages is of length nr_physpages, pfn_types is of length nr_pages */
    unsigned int nr_physpages, nr_pages;

    /* Types of the pfns in the current region */
    unsigned long* pfn_types;

    int verify;

    int new_ctxt_format;
    int max_vcpu_id;
    uint64_t vcpumap;
    uint64_t identpt;
    uint64_t vm86_tss;
    uint64_t console_pfn;
    uint64_t acpi_ioport_location;
} pagebuf_t;

/* Global Variable */
// Receive Waiting for slave ready
banner_t receive_ready_banner;
// Sender Waiting for slave ready
banner_t sender_iter_banner;
// Argument in Send
struct list_item *send_argu_head;
pthread_mutex_t send_argu_head_mutex;
// Pagebuf in Recv
struct list_item *recv_pagebuf_head;
pthread_mutex_t recv_pagebuf_head_mutex;
// Receive Finish Count
int recv_finish_cnt;
pthread_mutex_t recv_finish_cnt_mutex;
int recv_slave_cnt;


int parse_dest_file(char* dest_file, char*** dests, int* dest_cnt); 
int rune_add_ips(char** rune, char** dests, int dest_cnt);
int mc_net_server(char* ip);
int mc_net_client(char* ip);
void init_banner(banner_t *banner, int count);
void* send_patch(void* args);
void* receive_patch(void* args);
int init_list_head(struct list_item *list_head);
int send_argu_enqueue(send_argu_t* argu);
int send_argu_dequeue(send_argu_t **argu);
int recv_pagebuf_enqueue(pagebuf_t *pagebuf);
int recv_pagebuf_dequeue(pagebuf_t **pagebuf);
#endif
