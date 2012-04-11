#ifndef __QOS_H__
#define __QOS_H__

#define NICMAX 128
#define START_QOS 1
long nic_speed[NICMAX];
int qos_start_flag;
struct qos_arg {
	char **nic;
	int nic_num;
};

void *qos(void *arg);
#endif
