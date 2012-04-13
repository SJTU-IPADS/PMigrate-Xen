#ifndef __QOS_H__
#define __QOS_H__

#define NICMAX 128
#define START_QOS 1
long nic_speed[NICMAX];
struct qos_arg {
	char **nic;
	int nic_num;
};

//pthread_mutex_t qos_pause_mutex;

void *qos(void *arg);
#endif
