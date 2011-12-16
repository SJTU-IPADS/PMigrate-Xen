#include <stdio.h>
#include <stdlib.h>
#include "mc_migration_helper.h"

/* Parse the Migration Destination IP File */
int parse_dest_file(char* dest_file, char*** dests, int* dest_cnt) 
{
	int store_cnt = BUFFER_INIT_SIZE, i = 0;
	char **ip_store = (char **)malloc(sizeof(char*) * BUFFER_INIT_SIZE);
	FILE *fd = fopen(dest_file, "r");
	if (!fd)
		return -1;

	while (1) {
		/* Break Only realloc the buffer */
		while (i < store_cnt) {
			char *buf = NULL;
			size_t buf_size = 0;
			if( getline(&buf, &buf_size, fd) < 0 ) {
				store_cnt = i;
				goto out;
			}
			ip_store[i] = buf;
			i++;
		}
		ip_store = realloc(ip_store, store_cnt + BUFFER_INIT_SIZE);
		store_cnt += BUFFER_INIT_SIZE;
	}

out:
	*dests = ip_store;
	*dest_cnt = store_cnt;
	return 0;
}

