#ifndef __MC_MIGRATION_HELPER_H__
#define __MC_MIGRATION_HELPER_H__

#define BUFFER_INIT_SIZE 10
#define DEFAULT_PORT 3344

enum {mc_migrate_debug = 0};
enum {mc_migrate_hint = 1};

int parse_dest_file(char* dest_file, char*** dests, int* dest_cnt); 
int rune_add_ips(char** rune, char** dests, int dest_cnt);
int mc_net_server(char* ip);

#endif
