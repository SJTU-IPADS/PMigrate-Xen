#include <sys/types.h>
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

/*static long fetchtotal(char *eth)
{
	long result = -1;
	FILE *stream;
	char buf[128];
	char ans[16];
	char *p = NULL; 

	memset(buf, '\0', sizeof(buf));
	sprintf(buf, "ethtool %s | grep \"Speed\" | awk {'split( $2, tmp, \":\"); print tolower(tmp[1])'}", eth);
	stream = popen(buf, "r");
	fscanf(stream, "%s", ans);
	pclose(stream);
	p = ans;
	if (*p >= '0' && *p <= '9'){
		result = *p - '0';
		p++;
		while (*p >= '0' && *p <= '9'){
			result = result * 10 + (*p - '0');
			p++;
		}
	}
	return result;
}*/

static inline long fetchcur(char *eth, int i)
{
	FILE *stream;
	char buf[128];
	char ans[16];

	memset(buf, '\0', sizeof(buf));
	sprintf(buf, "cat /proc/net/dev | grep \"%s\" | awk {'print $%d'}", eth, i);
	stream = popen(buf, "r");
	fscanf(stream, "%s", ans);
	pclose(stream);
	return atol(ans);
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
	}

	while(1){
		sleep(1);
		pthread_mutex_lock(&qos_pause_mutex);
		for (j = 0; j < nicnum; j++){
			recv[j] = RECV(nic[j]);
			send[j] = SEND(nic[j]);
			nic_speed[j] = ((send[j] - lsend[j] + recv[j] - lrecv[j]) + MB - 1)/MB;
			fprintf(stderr, "%ld\t", nic_speed[j]);
			lrecv[j] = recv[j];
			lsend[j] = send[j];
		}
		pthread_mutex_unlock(&qos_pause_mutex);
		fprintf(stderr, "\n");
	}
}
