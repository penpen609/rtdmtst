#ifndef _RTDM_TESTXILLY_H
#define _RTDM_TESTXILLY_H
#include <rtdm/rtdm.h>
#include <rtdm/testing.h>
struct rttst_xillybus
{
	unsigned char* pbuf;
	int len;
};
struct rttst_res
{
	unsigned long write_seq;
	unsigned long read_seq;
	nanosecs_abs_t deta_time;
};
#define RTDM_CLASS_XILLYBUS		RTDM_CLASS_MISC
#define XILLYBUS_RTIOC_WRITE	_IOW(RTDM_CLASS_XILLYBUS, 0, struct rttst_xillybus)
#define XILLYBUS_RTIOC_READ		_IOR(RTDM_CLASS_XILLYBUS, 1, struct rttst_xillybus)
#define XILLYBUS_RTIOC_CURRENT_DOMAIN		_IOR(RTDM_CLASS_XILLYBUS, 2, int)
#endif