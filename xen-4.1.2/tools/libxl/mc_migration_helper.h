#ifndef __MC_MIGRATION_HELPER_H__
#define __MC_MIGRATION_HELPER_H__

#define BUFFER_INIT_SIZE 10

enum {mc_migrate_debug = 0};
enum {mc_migrate_hint = 1};

int parse_dest_file(char* dest_file, char*** dests, int* dest_cnt); 

#endif
