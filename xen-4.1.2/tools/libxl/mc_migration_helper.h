#ifndef __MC_MIGRATION_HELPER_H__
#define __MC_MIGRATION_HELPER_H__
#include <pthread.h>
#include <stdio.h>
#include "../libxc/xenctrl.h"

#define BUFFER_INIT_SIZE 10
#define DEFAULT_PORT 3344
#define PAUSE while(1)
#define LOGFILE "log"
#define MULTI_TRY 10

#define SLEEP_SHORT_TIME 1000

enum {mc_migrate_debug = 0};
enum {mc_migrate_hint = 1};

typedef struct {
	int cnt;
	pthread_mutex_t mutex;
	
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

struct list_item *send_argu_head;
pthread_mutex_t send_argu_head_mutex;

/* Waiting for slave ready */
banner_t slave_ready_banner;

int parse_dest_file(char* dest_file, char*** dests, int* dest_cnt); 
int rune_add_ips(char** rune, char** dests, int dest_cnt);
int mc_net_server(char* ip);
int mc_net_client(char* ip);
void init_slave_ready_banner(void);
void* send_patch(void* args);
void* receive_patch(void* args);
int init_list_head(struct list_item *list_head);
int send_argu_enqueue(send_argu_t* argu);
int send_argu_dequeue(send_argu_t **argu);
#endif
