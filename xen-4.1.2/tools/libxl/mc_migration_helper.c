#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
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
			buf[strlen(buf) - 1] = '\0';
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

/* Change the Rune Command */
int rune_add_ips(char** rune, char** dests, int dest_cnt)
{
	int i, len = BUFFER_INIT_SIZE, current;
	char *a = malloc(BUFFER_INIT_SIZE);
	bzero(a, BUFFER_INIT_SIZE);

	/* Ignore the first */
	for (i = 1; i < dest_cnt; i++) {
		if ((current = strlen(dests[i]) + strlen(a) + 1) > len){
			len = current * 2;
			a = realloc(a, len);
		}
		a = strcat(a, " ");
		a = strcat(a, dests[i]);
	}

	asprintf(rune, "%s %s", *rune, a);
	return 0;
}

/* Network Socket Connection */
int mc_net_server(char* ip) 
{
	int sock, connect, addr_len;
	struct sockaddr_in server_addr,client_addr;
	struct hostent *host;

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		fprintf(stderr, "Create Socket Error\n");
		return -1;
	}

	if ((host = gethostbyname(ip)) == NULL) {
		fprintf(stderr, "Get Host By Ip Error\n");
		return -1;
	}
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = DEFAULT_PORT;
	server_addr.sin_addr.s_addr = *((unsigned long *) host->h_addr_list[0]);

	if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
		fprintf(stderr, "Bind Error\n");
		return -1;
	}

	if (listen(sock, 10) == -1) {
		fprintf(stderr, "Listen Error\n");
		return -1;
	}

	addr_len = sizeof(client_addr);
	connect = accept(sock, (struct sockaddr *) &client_addr, (socklen_t*) &client_addr);
	return connect;
}
