#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "qos.h"
#include "mc_migration_helper.h"

#define RECV(A) fetchcur(A, 2)
#define SEND(A) fetchcur(A, 10)
#define MB 1048576

static inline long fetchcur(char *eth, int i)
{
	char *buf = NULL;
	size_t len;
	FILE *fd = fopen("/proc/net/dev", "r");
	int cnt;
	char *tok;
	while((cnt = getline(&buf, &len, fd)) > 0) {
		tok = strtok(buf, " :");
		if(!strcmp(eth, tok)) {
			int j;
			for ( j = 1; j < i; j ++ ) {
				tok = strtok(NULL, " :");
			}
			free(buf);
			goto end;
		}
		free(buf);
		buf = NULL;
	}
end:
	fclose(fd);
	return atol(tok);
}

void *qos(void *arg)
{
	struct qos_arg *qos_arg= (struct qos_arg*) arg;
	int nicnum = qos_arg->nic_num;
	char **nic = qos_arg->nic;
	int j;

	//long total[NICMAX],
	long lrecv[NICMAX],
		 lsend[NICMAX],
		 recv[NICMAX],
		 send[NICMAX];

	free(arg);

	pthread_mutex_lock(&qos_pause_mutex);
	for(j = 0; j < nicnum; j++){
		//total[j] = fetchtotal(nic[j]);
		lrecv[j] = RECV(nic[j]);
		lsend[j] = SEND(nic[j]);
	}
	pthread_mutex_unlock(&qos_pause_mutex);

	while(1){
		sleep(1);
		pthread_mutex_lock(&qos_pause_mutex);
		for (j = 0; j < nicnum; j++){
			recv[j] = RECV(nic[j]);
			send[j] = SEND(nic[j]);
			//recv[j] = lrecv[j];
			//send[j] = lsend[j];
			nic_speed[j] = ((send[j] - lsend[j] + recv[j] - lrecv[j]) + MB - 1)/MB;
			//fprintf(stderr, "recv[%d] = %ld, send[%d] = %ld\n", j, recv[j], j, send[j]);
			lrecv[j] = recv[j];
			lsend[j] = send[j];
		}

		pthread_mutex_unlock(&qos_pause_mutex);
	}
}
