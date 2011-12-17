#ifndef __MC_MIGRATION_HELPER_H__
#define __MC_MIGRATION_HELPER_H__
#include <pthread.h>

#define BUFFER_INIT_SIZE 10
#define DEFAULT_PORT 3344

enum {mc_migrate_debug = 0};
enum {mc_migrate_hint = 1};

typedef struct {
	int cnt;
	pthread_mutex_t mutex;
	
} banner_t;

/* Waiting for slave ready */
banner_t slave_ready_banner;

int parse_dest_file(char* dest_file, char*** dests, int* dest_cnt); 
int rune_add_ips(char** rune, char** dests, int dest_cnt);
int mc_net_server(char* ip);
int mc_net_client(char* ip);
void init_slave_ready_banner(void);
#endif
