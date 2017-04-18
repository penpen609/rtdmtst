/*
* Round-Trip-Time Test - sends and receives messages and measures the *                        time in between.
*
* Copyright (C) 2006 Wolfgang Grandegger <wg@grandegger.com>
*
* Based on RTnet's examples/xenomai/posix/rtt-sender.c.
*
* Copyright (C) 2002 Ulrich Marx <marx@kammer.uni-hannover.de>
*               2002 Marc Kleine-Budde <kleine-budde@gmx.de>
*               2006 Jan Kiszka <jan.kiszka@web.de>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
*
* The program sends out CAN messages periodically and copies the current
* time-stamp to the payload. At reception, that time-stamp is compared
* with the current time to determine the round-trip time. The jitter
* values are printer out regularly. Concurrent tests can be carried out
* by starting the program with different message identifiers. It is also
* possible to use this program on a remote system as simple repeater to
* loopback messages.
*/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <error.h>
#include <signal.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/timerfd.h>
#include <xeno_config.h>
#include <rtdm/testing.h>
#include <boilerplate/trace.h>
#include <xenomai/init.h>

#include "rtdm_testxilly_h.h"

#define ONE_BILLION	1000000000
#define TEN_MILLIONS	10000000

#define HIPRIO 99
#define LOPRIO 0

static int testdev = -1;

static unsigned char txbuf[1024];
static unsigned char rxbuf[1024];
struct rtt_stat {
	long long rtt;
	long long rtt_min;
	long long rtt_max;
	long long rtt_max_last;
};
static void *latency(void *cookie)
{
	char task_name[16];
	int tfd = 0;
	struct timespec expected;
	struct itimerspec timer_conf;
	long long period_ns = 10;
	int count = 0;
	int err = 0;
	int sig;
	struct rttst_xillybus writearg;
	struct rttst_xillybus readarg;
	struct rttst_res* pRes = (struct rttst_res*)rxbuf;
	struct rtt_stat* rttstat = (struct rtt_stat* )cookie;
	struct timespec beg_time;
	struct timespec end_time;
	long long deta_time;
	writearg.len = 1024;
    writearg.pbuf = txbuf;
	readarg.len = 1024;
    readarg.pbuf = rxbuf;

	memset(rxbuf, 0, 1024);
	snprintf(task_name, sizeof(task_name), "sampling-%d", getpid());
	err = pthread_setname_np(pthread_self(), task_name);
	if (err)
		error(1, err, "pthread_setname_np(latency)");

	tfd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (tfd == -1)
		error(1, errno, "timerfd_create()");

#ifdef CONFIG_XENO_COBALT
	err = pthread_setmode_np(0, PTHREAD_WARNSW, NULL);
	if (err)
		error(1, err, "pthread_setmode_np()");
#endif

	err = clock_gettime(CLOCK_MONOTONIC, &expected);
	if (err)
		error(1, errno, "clock_gettime()");

	//fault_threshold = CONFIG_XENO_DEFAULT_PERIOD;
	/* start time: one millisecond from now. */
	expected.tv_nsec += 1000000;
	if (expected.tv_nsec > ONE_BILLION) {
		expected.tv_nsec -= ONE_BILLION;
		expected.tv_sec++;
	}
	timer_conf.it_value = expected;
	timer_conf.it_interval.tv_sec = period_ns / ONE_BILLION;
	timer_conf.it_interval.tv_nsec = period_ns % ONE_BILLION;

	//err = timerfd_settime(tfd, TFD_TIMER_ABSTIME, &timer_conf, NULL);
	//if (err)
	//	error(1, errno, "timerfd_settime()");
	for (count = 0; count < 10; count++) {
		//uint64_t ticks;

		//err = read(tfd, &ticks, sizeof(ticks));
		clock_gettime(CLOCK_MONOTONIC, &beg_time);
		err = ioctl(testdev, XILLYBUS_RTIOC_WRITE, &writearg);
		if (err < 0)
			printf("write err=%d\n", err);
		err = ioctl(testdev, XILLYBUS_RTIOC_READ, &readarg);
		if (err <0)
			printf("read err=%d\n", err);
		else
		{
			clock_gettime(CLOCK_MONOTONIC, &end_time);
			deta_time = (end_time.tv_sec * 1000000000 + end_time.tv_nsec) - (beg_time.tv_sec * 1000000000 + beg_time.tv_nsec);
			printf("read: write index %lu, %lu  %lu %lu\n", pRes->write_seq,
				pRes->read_seq, pRes->deta_time, deta_time);
			if (deta_time > rttstat->rtt_max)
			{
				if (rttstat->rtt_max <= 0)
					rttstat->rtt_max_last = deta_time;
				else
					rttstat->rtt_max_last = rttstat->rtt_max;
				rttstat->rtt_max = deta_time;
			}
			if (deta_time < rttstat->rtt_min)
				rttstat->rtt_min = deta_time;
			rttstat->rtt = deta_time;
		}
		//expected.tv_nsec += (ticks * period_ns) % ONE_BILLION;
		//expected.tv_sec += (ticks * period_ns) / ONE_BILLION;
		//if (expected.tv_nsec > ONE_BILLION) {
		//	expected.tv_nsec -= ONE_BILLION;
		//	expected.tv_sec++;
		//}
		
	}
	printf("last rtt %lld, min %lld, max %lld, last max %lld\n", rttstat->rtt, rttstat->rtt_min, rttstat->rtt_max, rttstat->rtt_max_last);
	signal(sig, SIGTERM);
	kill(getpid(), sig);
}

static void setup_sched_parameters(pthread_attr_t *attr, int prio)
{
	struct sched_param p;
	int ret;
	
	ret = pthread_attr_init(attr);
	if (ret)
		error(1, ret, "pthread_attr_init()");

	ret = pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
	if (ret)
		error(1, ret, "pthread_attr_setinheritsched()");

	ret = pthread_attr_setschedpolicy(attr, prio ? SCHED_FIFO : SCHED_OTHER);
	if (ret)
		error(1, ret, "pthread_attr_setschedpolicy()");

	p.sched_priority = prio;
	ret = pthread_attr_setschedparam(attr, &p);
	if (ret)
		error(1, ret, "pthread_attr_setschedparam()");
}
int main(int argc, char *argv[])
{
	struct sigaction sa __attribute__((unused));
	int cpu = 0;
	int sig = 0;
	pthread_t latency_task;
	pthread_attr_t tattr;
	cpu_set_t cpus;
	struct rtt_stat rttstat = { 0, 1000000000000000000LL, 0,
		0 };
	int ret = 0;
	int priority = HIPRIO;
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);

	testdev = open("/dev/rtdm/xillypcie", O_RDWR);
	if (testdev < 0)
	{
		perror("open /dev/rtdm/xillypcie failure!");
		return 1;
	}
	if (ioctl(testdev, XILLYBUS_RTIOC_CURRENT_DOMAIN, &ret) >= 0)
	{
		printf("XILLYBUS_RTIOC_CURRENT_DOMAIN ok %d\n", ret);
	}
	else
	{
		printf("XILLYBUS_RTIOC_CURRENT_DOMAIN failure %d\n", errno);
	}
	setup_sched_parameters(&tattr, priority);
	CPU_ZERO(&cpus);
	CPU_SET(cpu, &cpus);

	ret = pthread_attr_setaffinity_np(&tattr, sizeof(cpus), &cpus);
	if (ret)
		error(1, ret, "pthread_attr_setaffinity_np()");

	ret = pthread_create(&latency_task, &tattr, latency, &rttstat);
	if (ret)
		error(1, ret, "pthread_create(latency)");

	pthread_attr_destroy(&tattr);
	__STD(sigwait(&mask, &sig));

	pthread_cancel(latency_task);
		pthread_join(latency_task, NULL);
	if (testdev >= 0)
		close(testdev);
/* This call also leaves primary mode, required for socket cleanup. */
	printf("last rtt %lld, min %lld, max %lld, last max %lld\n", rttstat.rtt, rttstat.rtt_min, rttstat.rtt_max, rttstat.rtt_max_last);
	return 1;
}
