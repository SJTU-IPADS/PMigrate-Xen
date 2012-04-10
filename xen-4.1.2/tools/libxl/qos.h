#ifndef __QOS_H__
#define __QOS_H__

#define NICMAX 128
long nic_speed[NICMAX];
struct qos_arg {
	char **nic;
	int nic_num;
};

void *qos(void *arg);
#endif
