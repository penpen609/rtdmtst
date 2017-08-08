/*
 * Copyright (C) 2009 Philippe Gerum <rpm@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 *
 *
 * IDDP-based client/server demo, using the sendto(2)/recvfrom(2)
 * system calls to exchange data over a socket.
 *
 * In this example, two sockets are created.  A server thread (reader)
 * is bound to a real-time port and receives datagrams sent to this
 * port from a client thread (writer). The client socket is bound to a
 * different port, only to provide a valid peer name; this is
 * optional.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <error.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <xeno_config.h>

#define START_FLAG_LEN 4
#define MAX_DATA_NUM_PER_FRAME 64
#define RECV_BUF_SIZE 10240
pthread_t svtid;

struct rtt_stat {
	long long rtt;
	long long rtt_min;
	long long rtt_max;
	long long rtt_max_last;
	unsigned int index_min;
	unsigned int index_max;
	unsigned int index_lastmax;
};
struct rttst_res
{
	unsigned long write_seq;
	struct timespec write_time;
};
struct frame_format
{
	unsigned char start_flag[START_FLAG_LEN]; //4字节的起始标志eb90
	unsigned int send_index; //发送序号
	unsigned long long send_timespec; //发送数据时间戳，精度纳秒
	unsigned int data_num; //发送数据个数，最多为64个，单个数据长度为4字节可能是float、也可能是int
	char datas[MAX_DATA_NUM_PER_FRAME * 4  + 4]; //数据值缓冲区+4字节的校验 
};
static int testdev = -1;
static int writeindex = 0;
//static int readindex = 0;
static int data_value_num = MAX_DATA_NUM_PER_FRAME;
unsigned int write_begin_seq = 0;
float write_value = 0.0f;
static int needTrans = 0; //是否进行字节序转换
#define TIME_BUF_SIZE 50
#define TIME_SEQ_SUC 1
#define TIME_SEQ_ERR 0
struct time_seq_buf
{
	unsigned long long write_time_buf[TIME_BUF_SIZE];
	int size_num;
	int header_index;
	int tail_index;	
};

void time_seq_buf_init(struct time_seq_buf* p)
{
	if (p == NULL)
	{
		return;
	}
	memset(p->write_time_buf, 0, sizeof(unsigned long long)*TIME_BUF_SIZE);
	p->size_num = 0;
	p->header_index = 0;
	p->tail_index = 0;
}
int time_seq_buf_push(struct time_seq_buf* p, unsigned long long time_seq)
{
	if (p == NULL)
	{
		return TIME_SEQ_ERR;
	}
	if (p->size_num >= TIME_BUF_SIZE)
	{
		return TIME_SEQ_ERR;
	}
	p->write_time_buf[p->tail_index++] = time_seq;
	if (p->tail_index >= TIME_BUF_SIZE)
	{
		p->tail_index = 0;
	}
	++p->size_num;
	return TIME_SEQ_SUC;
}
int time_seq_buf_isEmpty(struct time_seq_buf* p)
{
	if (p == NULL)
	{
		return TIME_SEQ_ERR;
	}
	if (p->size_num == 0)
	{
		return TIME_SEQ_SUC;
	}
	return TIME_SEQ_ERR;
}
unsigned long long time_seq_buf_pop(struct time_seq_buf* p)
{
	unsigned long long time_seq = 0;
	if (p == NULL)
	{
		return time_seq;
	}
	if (p->size_num > 0)
	{
		time_seq = p->write_time_buf[p->header_index++];
		if (p->header_index >= TIME_BUF_SIZE)
		{
			p->header_index = 0;
		}
		--p->size_num;
	}
	return time_seq;
}

#ifdef CONFIG_XENO_COBALT

#include "cobalt-signal.h"

static const char *reason_str[] = {
	[SIGDEBUG_UNDEFINED] = "received SIGDEBUG for unknown reason",
	[SIGDEBUG_MIGRATE_SIGNAL] = "received signal",
	[SIGDEBUG_MIGRATE_SYSCALL] = "invoked syscall",
	[SIGDEBUG_MIGRATE_FAULT] = "triggered fault",
	[SIGDEBUG_MIGRATE_PRIOINV] = "affected by priority inversion",
	[SIGDEBUG_NOMLOCK] = "process memory not locked",
	[SIGDEBUG_WATCHDOG] = "watchdog triggered (period too short?)",
	[SIGDEBUG_RESCNT_IMBALANCE] = "rescnt imbalance",
	[SIGDEBUG_LOCK_BREAK] = "scheduler lock break",
	[SIGDEBUG_MUTEX_SLEEP] = "mutex sleep",
};

static void sigdebug(int sig, siginfo_t *si, void *context)
{
	const char fmt[] = "%s, aborting.\n"
		"(enabling CONFIG_XENO_OPT_DEBUG_TRACE_RELAX may help)\n";
	unsigned int reason = si->si_value.sival_int;
	int n __attribute__((unused));
	static char buffer[256];

	if (reason > SIGDEBUG_MUTEX_SLEEP)
		reason = SIGDEBUG_UNDEFINED;

// 	switch (reason) {
// 	case SIGDEBUG_UNDEFINED:
// 	case SIGDEBUG_NOMLOCK:
// 	case SIGDEBUG_WATCHDOG:
// 		n = snprintf(buffer, sizeof(buffer), "altency: %s\n",
// 			reason_str[reason]);
// 		n = write(STDERR_FILENO, buffer, n);
// 		exit(EXIT_FAILURE);
// 	}

	n = snprintf(buffer, sizeof(buffer), fmt, reason_str[reason]);
	n = write(STDERR_FILENO, buffer, n);
//  	signal(sig, SIG_DFL);
//  	__STD(kill(getpid(), sig));
}

#endif /* CONFIG_XENO_COBALT */
static void fail(const char *reason)
{
	perror(reason);
	exit(EXIT_FAILURE);
}
static void  hosttonetwork(void* pData, unsigned int DataLen)
{
	int i = 0, count = 0;
	unsigned char tmp;
	unsigned char *tmpData = (unsigned char*)pData;

	count = DataLen / 2;
	for (i = 0; i < count; i++)
	{
		tmp = tmpData[i];
		tmpData[i] = tmpData[DataLen - i - 1];
		tmpData[DataLen - i - 1] = tmp;
	}
}

static unsigned int frame_len(unsigned int data_num)
{
	unsigned int len = sizeof(struct frame_format);
	if (data_num < MAX_DATA_NUM_PER_FRAME)
	{
		len -= (MAX_DATA_NUM_PER_FRAME - data_num) * 4;
	}	
	return len;
}

static void *write_proc(void *arg)
{
	struct timespec write_time;	
	struct timespec ts;
	
	int ret = 0;
	int write_num = 100;

	int i = 0;
	struct frame_format txbuf;
	int send_bytes = frame_len(data_value_num);
	int hasWrite = 0;
	int toWrite = 0;
	struct rtt_stat rttstat = { 0, 1000000000000000000LL, 0,
		0, 0, 0, 0 };
	int nCnt = 0;
	unsigned long long cost_sum = 0;
	unsigned long long last_write_time = 0;
	unsigned long long last_recv_val_time = 0;
	struct time_seq_buf write_seqs;
	int last_ack_index = -1;
	int last_rcv_write_index = -1;
	unsigned int sum = 0;
	unsigned char rxbuf[RECV_BUF_SIZE];
	float last_recv_val = 0.0f;
	struct timespec read_time;
	long long deta_time = 0;
	int toRead = RECV_BUF_SIZE;
	int hasRead = 0;
	int nPos = 0; //解析开始位置
	int tEnd = 0;
	int bFind = 0;
	int notEnough = 0;
	//unsigned int last_recv_index = 0;
	long long loop_waste = 0;
	float fval = -1.0f;
	unsigned int readok_num = 0;
// 	struct itimerspec timer_conf;
// 	int tfd = -1;
// 	unsigned long long ticks;

	if (arg != NULL)
	{
		write_num = *(int*)(arg);
		printf("write num =%d, send bytes=%d\n", write_num, send_bytes);
	}
#ifdef CONFIG_XENO_COBALT
	ret = pthread_setmode_np(0, PTHREAD_WARNSW, NULL);
#endif
	txbuf.start_flag[0] = 0x90;
	txbuf.start_flag[1] = 0xeb;
	txbuf.start_flag[3] = 0xeb;
	txbuf.start_flag[2] = 0x90;
// 	tfd = timerfd_create(CLOCK_MONOTONIC, 0);
// 	if (tfd == -1)
// 		error(1, errno, "timerfd_create()");
// 
// 	clock_gettime(CLOCK_MONOTONIC, &write_time);
// 	timer_conf.it_value = write_time;
// 	timer_conf.it_interval.tv_sec = 0;
// 	timer_conf.it_interval.tv_nsec = 2000;
// 
// 	ret = timerfd_settime(tfd, TFD_TIMER_ABSTIME, &timer_conf, NULL);
// 	if (ret)
// 		error(1, errno, "timerfd_settime()");
	time_seq_buf_init(&write_seqs);
	while ((ret = read(testdev, rxbuf, RECV_BUF_SIZE)) > 0)
	{
		++readok_num;
		//printf("read ok.....%d\n", ret);
		ts.tv_sec = 0;
		ts.tv_nsec = 5000; /*10 us */
		clock_nanosleep(CLOCK_REALTIME, 0, &ts, NULL);
	}
	printf("read old data over.............%d\n", readok_num);
	readok_num = 0;
	while (1)
	{		
		int* pData = NULL;
		float* pValue = NULL;
		//clock_gettime(CLOCK_MONOTONIC, &begin_time);
			//printf("==========write wait time %d....\n", txbuf.send_index);
// 		if (toRead > 0)
// 		{
// 			memset(rxbuf + hasRead, 0, toRead);
// 		}
			ret = read(testdev, rxbuf + hasRead, toRead);
			if (ret > 0)
			{
				//printf("read ok.....%d\n", ret);
				++readok_num;
				toRead -= ret;
				hasRead += ret;
				notEnough = 0;
			}
			//printf("read=====%d\n", ret);
			if (hasRead >= START_FLAG_LEN && (notEnough != 1))
			{
				tEnd = hasRead - 3;
				bFind = 0;
				//找起始位置，开始解析包内容
				while (nPos < tEnd)
				{
					if ((rxbuf[nPos + 1] == 0xeb && rxbuf[nPos] == 0x90 &&
						rxbuf[nPos + 3] == 0xeb && rxbuf[nPos + 2] == 0x90))						
					{//找到起始位置
						bFind = 1;
						break;
					}
					else
					{
						++nPos;
					}
				}
				//printf("parse pos=%d hasRead=%d\n", nPos, hasRead);
				if (bFind)
				{//解析内容
					struct frame_format* rcv = (struct frame_format*)(rxbuf + nPos);
					int nDataIndex = 0;
					int* pData = (int*)(rcv->datas);	
					unsigned int data_num = rcv->data_num;
					//printf("parse 0x%0x 0x%0x 0x%0x 0x%0x\n", rxbuf[nPos], rxbuf[nPos + 1], rxbuf[nPos + 2], rxbuf[nPos + 3]);
// 					if (needTrans)
// 						hosttonetwork((void*)&data_num, sizeof(data_num));						
					if (data_num > MAX_DATA_NUM_PER_FRAME)
					{
						nPos += 4; //数据个数非法，丢弃包头，继续找下一个起始
						printf("data num error %d, drop\n", data_num);
						break;
					}
					else
					{
						unsigned int len = frame_len(data_num);						
						//printf("frame_len=%d\n", len);
						if ((nPos + len) <= hasRead)
						{//读取完成，才能解析						
							clock_gettime(CLOCK_MONOTONIC, &read_time);
// 							if (needTrans)
// 							{
// 								hosttonetwork((void*)&rcv->send_index, sizeof(rcv->send_index));
// 								hosttonetwork((void*)&rcv->send_timespec, sizeof(rcv->send_timespec));
// 							}						

							// 				if ((last_recv_index + 1) != rcv->send_index)
							// 				{
							// 					printf("lost data, readindex=%d writeseq=%d\n", last_recv_index, rcv->send_index);
							// 				}
							++nCnt;
							cost_sum += deta_time;
							//todo,数据处理 
							//printf("====%d readindex=%d send_time=%llu data_num=%d Value:\n",nCnt, rcv->send_index, rcv->send_timespec, data_num);
							pValue = (float*)rcv->datas;
							//pData = (int*)rcv->datas;
							for (nDataIndex = 0; nDataIndex < data_num; ++nDataIndex, ++pValue)
							{
// 								if (needTrans)
// 								{
// 									hosttonetwork((void*)pValue, 4);
//  								}
// 								
								if (nDataIndex == 0)
								{
									// 约定第一个数与当前序号一致
									//int nIndex = (int)(*pValue);
									unsigned long long send_timespec = 0;
									
									if (fabs(last_recv_val - *pValue) > 0.1f)
									{		
										struct timespec cur_time;
										clock_gettime(CLOCK_MONOTONIC, &cur_time);
										if (last_recv_val_time != 0)
										{
											printf("last_recv_val=%f(%f) %d %d readnum=%d %llu\n",
												last_recv_val, *pValue, last_ack_index, rcv->send_index, rcv->send_index - last_ack_index,
												cur_time.tv_sec * 1000000000 + cur_time.tv_nsec - last_recv_val_time );
										}
										else
										{
											printf("last_recv_val=%f(%f) %d %d readnum=%d\n", last_recv_val, *pValue, last_ack_index, rcv->send_index, rcv->send_index - last_ack_index);
										}
										last_recv_val_time = cur_time.tv_sec * 1000000000 + cur_time.tv_nsec;
										last_recv_val = *pValue;
										last_ack_index = rcv->send_index;
									}
									// 暂时封掉，不管序号对不对都获取到发送时间
// 									if (nIndex == last_rcv_write_index + 1)
// 									{
// 										send_timespec = time_seq_buf_pop(&write_seqs);
// 										printf("get write time %d %d\n", nIndex, last_ack_index);
// 									}
// 									if (nIndex != 0 && last_rcv_write_index == -1)
// 									{
// 									}
// 									else
// 										last_rcv_write_index = nIndex;
									send_timespec = time_seq_buf_pop(&write_seqs);
									++last_rcv_write_index;

									if (send_timespec > 0)
									{
										deta_time = (read_time.tv_sec * 1000000000 + read_time.tv_nsec) - send_timespec;
										if (deta_time > rttstat.rtt_max)
										{
											if (rttstat.rtt_max <= 0)
											{
												rttstat.rtt_max_last = deta_time;
												rttstat.index_lastmax = rcv->send_index;
											}
											else
											{
												rttstat.rtt_max_last = rttstat.rtt_max;
												rttstat.index_lastmax = rttstat.index_max;
											}
											rttstat.rtt_max = deta_time;
											rttstat.index_max = rcv->send_index;

										}
										if (deta_time < rttstat.rtt_min)
										{
											rttstat.rtt_min = deta_time;
											rttstat.index_min = rcv->send_index;
										}
									}
								}	
								else if (nDataIndex == 62)
								{
									printf("%d=%f\n", nDataIndex, *pValue);
								}
								//printf("%d=%d\t", nDataIndex, *pData);
								//++pData;
							}
							//printf("\n");
							nPos = nPos + len;							
							if (nPos < hasRead)
							{
								memmove(rxbuf, rxbuf + nPos, hasRead - nPos);
							}
							hasRead = hasRead - nPos;
							nPos = 0;
							toRead = RECV_BUF_SIZE - hasRead;
							//printf("=====parse pos=%d hasRead=%d toRead=%d\n", nPos, hasRead, toRead);

							if (nCnt % 100 == 0)
							{
								if (rttstat.rtt == 0)
									rttstat.rtt = cost_sum / 100;
								else
								{
									rttstat.rtt += (cost_sum / 100);
									rttstat.rtt /= 2;
								}
								cost_sum = 0;
								printf("*****************Total num %d ======== last avr_rtt %lld, min %d:%lld, max %d:%lld, last max %d:%lld\n", nCnt, rttstat.rtt, rttstat.index_min, rttstat.rtt_min,
									rttstat.index_max, rttstat.rtt_max, rttstat.index_lastmax, rttstat.rtt_max_last);
							} 	
// 							if (nCnt >= write_num)
// 							{
// 								break;
// 							}
						}
						else
						{
							printf("recv not enough..\n");
							notEnough = 1;
						}
							
					}					
				}
				else
				{//丢弃					
					if (nPos < hasRead)
					{
						memmove(rxbuf, rxbuf + nPos, hasRead - nPos);
					}
					hasRead = hasRead - nPos;
					nPos = 0;
					toRead = RECV_BUF_SIZE - hasRead;
					printf(" not found begin, drop hasRead=%d toRead=%d\n", hasRead, toRead);
				}
			}
			
			if (hasWrite == 0 && toWrite == 0 && (writeindex < write_num))
			{
				clock_gettime(CLOCK_MONOTONIC, &write_time);
				txbuf.send_timespec = write_time.tv_sec * 1000000000 + write_time.tv_nsec;
				if (txbuf.send_timespec - last_write_time >= 50000)
				{
					memcpy(&sum, txbuf.start_flag, 4);
					txbuf.send_index = writeindex++;
					sum += txbuf.send_index;

					pData = (int*)&(txbuf.send_timespec);
					sum += *pData;
					pData++;
					sum += *pData;
					txbuf.data_num = data_value_num;
					sum += txbuf.data_num;
					last_write_time = txbuf.send_timespec;
					if (TIME_SEQ_ERR == time_seq_buf_push(&write_seqs, last_write_time))
						printf("write seq full\n");
//  					if (needTrans)
//  					{
//  						hosttonetwork((void*)&txbuf.send_index, sizeof(txbuf.send_index));
//  						hosttonetwork((void*)&txbuf.send_timespec, sizeof(txbuf.send_timespec));
//  						hosttonetwork((void*)&txbuf.data_num, sizeof(txbuf.data_num));
//  					}
					//fval = (float)(txbuf.send_index + write_begin_seq);
					pValue = (float*)txbuf.datas;
					for (i = 0; i < data_value_num; ++i)
					{
						*pValue = /*fval*/write_value;
						//fval += 1.0f;
						//sum += writeindex + i;
//  						if (needTrans)
//  						{
//  							hosttonetwork((void*)pValue, 4);
//  						}
						pValue++;
						
					}
//  					if (needTrans)
//  					{
//  						hosttonetwork((void*)&sum, 4);
//  					}
					*(unsigned int*)pValue = sum;
					toWrite = send_bytes;
				}
			}
			//printf("write wait time %d....\n", txbuf.send_index);
			if (toWrite > 0)
			{
				ret = write(testdev, txbuf.start_flag + hasWrite, toWrite);
				if (ret > 0)
				{
					hasWrite += ret;
					toWrite -= ret;
				}
				if (toWrite == 0)
				{
					printf("write ok.....%d %d %f\n", txbuf.send_index, hasWrite, *(float*)txbuf.datas);
					ret = write(testdev, NULL, 0); //flush
					hasWrite = 0;
				}
			}
			//clock_gettime(CLOCK_MONOTONIC, &write_time);
			//loop_waste = 50000 - ((write_time.tv_sec * 1000000000 + write_time.tv_nsec) - last_write_time);
			ts.tv_sec = 0;
			//if (loop_waste > 5000)
 			{
				//printf("loop left %lld\n", loop_waste);				
				ts.tv_nsec = 2500; /*10 us */				
 			}
// 			else
// 			{
// 				if (loop_waste < 2000)
// 				{
// 					ts.tv_nsec = 2000;
// 				}
// 				else
// 					ts.tv_nsec = loop_waste;
// 			}
			clock_nanosleep(CLOCK_REALTIME, 0, &ts, NULL);
			
			//read(tfd, &ticks, sizeof(ticks));
	}
	return NULL;
}

int main(int argc, char **argv)
{	
	struct sigaction sa __attribute__((unused));	
	char inputbuf[1024];
	int write_num = 100;
	struct sched_param wrparam = { .sched_priority = 71 };

	sigset_t set;
	pthread_attr_t svattr;
	int sig;
#ifdef CONFIG_XENO_COBALT
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = sigdebug;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGDEBUG, &sa, NULL);
#endif	
	
	testdev = open("/dev/rtdm/xillypcie", O_RDWR);
	if (testdev < 0)
	{
		perror("open /dev/rtdm/xillypcie failure!");
		return 1;
	}
	
	printf("\n=============********** xillybus test ********============================\n");
	while (1)
	{
		printf("start write proc[y|n]:");
		memset(inputbuf, 0, 1024);
		fgets(inputbuf, 1023, stdin);
		if (inputbuf[0] == 'y')
		{
			printf("\n input write num :");
			memset(inputbuf, 0, 1024);
			fgets(inputbuf, 1023, stdin);
			write_num = atoi(inputbuf);
// 			printf("\n input write begin seq :");
// 			memset(inputbuf, 0, 1024);
// 			fgets(inputbuf, 1023, stdin);
// 			write_begin_seq = atoi(inputbuf);
			printf("\n input write data value(float) :");
			memset(inputbuf, 0, 1024);
			fgets(inputbuf, 1023, stdin);
			write_value = atof(inputbuf);
			printf("\n input data value num :");
			memset(inputbuf, 0, 1024);
			fgets(inputbuf, 1023, stdin);
			data_value_num = atoi(inputbuf);
// 			printf("\n need translat to big-endian[y|n] :");
// 			memset(inputbuf, 0, 1024);
// 			fgets(inputbuf, 1023, stdin);
// 			if (inputbuf[0] == 'y')
// 			{
// 				needTrans = 1;
// 			}
			break;
		}
	}
	printf("\n ============= xillybus test begin============================\n");
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGHUP);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	pthread_attr_init(&svattr);
	pthread_attr_setdetachstate(&svattr, PTHREAD_CREATE_JOINABLE);
	pthread_attr_setinheritsched(&svattr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&svattr, SCHED_FIFO);
	pthread_attr_setschedparam(&svattr, &wrparam);
	errno = pthread_create(&svtid, &svattr, &write_proc, &write_num);
	if (errno)
		fail("pthread write create");

	sigwait(&set, &sig);
	pthread_cancel(svtid);
	pthread_join(svtid, NULL);
	if (testdev >= 0)
	{
		close(testdev);
	}
	return 0;
}
