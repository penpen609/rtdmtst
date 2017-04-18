/*
 * Copyright (C) 2010 Jan Kiszka <jan.kiszka@web.de>.
 *
 * Xenomai is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/module.h>
#include <rtdm/driver.h>
#include <rtdm/testing.h>
#define RTDM_SUBCLASS_RTDMTST		4
MODULE_DESCRIPTION("RTDM test helper module");
MODULE_AUTHOR("Jan Kiszka <jan.kiszka@web.de>");
MODULE_VERSION("0.1.0");
MODULE_LICENSE("GPL");

struct rtdm_tst_context {
	rtdm_mutex_t  mutex;
	nanosecs_abs_t rtt_time;
	unsigned long write_seq;
	unsigned long read_seq;
};

static int rtdm_tst_open(struct rtdm_fd *fd, int oflags)
{
	struct rtdm_tst_context *ctx = rtdm_fd_to_private(fd);
	rtdm_mutex_init(&ctx->mutex);
	ctx->rtt_time = rtdm_clock_read();
	ctx->write_seq = 0;
	ctx->read_seq = 0;

	return 0;
}

static void rtdm_tst_close(struct rtdm_fd *fd)
{
	struct rtdm_tst_context *ctx = rtdm_fd_to_private(fd);
	rtdm_mutex_destroy(&ctx->mutex);
}

static int rtdm_tst_ioctl_rt(struct rtdm_fd *fd,
			    unsigned int request, void __user *arg)
{
	struct rtdm_tst_context *ctx = rtdm_fd_to_private(fd);
	rtdm_mutex_lock(&ctx->mutex);
	switch (request) {
	case XILLYBUS_RTIOC_WRITE:
		printk(XENO_INFO "rt_xillybus_ioctl write\n");
		ctx->rtt_time = rtdm_clock_read();
		++ctx->write_seq;
		ret = 1;
		break;
	case XILLYBUS_RTIOC_READ:
	{
								printk(XENO_INFO "rt_xillybus_ioctl read\n");
								if (ctx->write_seq > ctx->read_seq)
								{
									struct rttst_xillybus* userArg = (struct rttst_xillybus*)(arg);
									struct rttst_res* pRes = (struct rttst_res*)(userArg->pbuf);
									nanosecs_abs_t curTime = rtdm_clock_read() - ctx->rtt_time;
									pRes->write_seq = ctx->write_seq;
									pRes->read_seq = ctx->read_seq;
									pRes->deta_time = curTime - ctx->rtt_time;
									++ctx->read_seq;
									ret = sizeof(struct rttst_res);
								}
								else
									ret = 0;
	}
		break;
	case XILLYBUS_RTIOC_CURRENT_DOMAIN:
		printk(XENO_INFO "rt_xillybus_ioctl current domain\n");
		int* pUser = (int*)arg;
		if (ipipe_root_p)
		{
			*pUser = 1;
		}
		else
		{
			*pUser = 0;
		}
		ret = sizeof(int);
		break;
	default:
		ret = -ENOTTY;
	}
	rtdm_mutex_unlock(&ctx->mutex);
	return ret;
}

static int rtdm_tst_ioctl_nrt(struct rtdm_fd *fd,
			    unsigned int request, void __user *arg)
{
	struct rtdm_tst_context *ctx = rtdm_fd_to_private(fd);
	int ret = 0, magic = RTTST_RTDM_MAGIC_SECONDARY;
	rtdm_mutex_lock(&ctx->mutex);
	switch (request) {
	case XILLYBUS_RTIOC_WRITE:
		printk(XENO_INFO "nrt_xillybus_ioctl write\n");
		ctx->rtt_time = rtdm_clock_read();
		++ctx->write_seq;
		ret = 1;
		break;
	case XILLYBUS_RTIOC_READ:
	{
								printk(XENO_INFO "nrt_xillybus_ioctl read\n");
								if (ctx->write_seq > ctx->read_seq)
								{
									struct rttst_xillybus* userArg = (struct rttst_xillybus*)(arg);
									struct rttst_res* pRes = (struct rttst_res*)(userArg->pbuf);
									nanosecs_abs_t curTime = rtdm_clock_read() - ctx->rtt_time;
									pRes->write_seq = ctx->write_seq;
									pRes->read_seq = ctx->read_seq;
									pRes->deta_time = curTime - ctx->rtt_time;
									++ctx->read_seq;
									ret = sizeof(struct rttst_res);
								}
								else
									ret = 0;
	}
		break;
	case XILLYBUS_RTIOC_CURRENT_DOMAIN:
		printk(XENO_INFO "nrt_xillybus_ioctl current domain\n");
		int* pUser = (int*)arg;
		if (ipipe_root_p)
		{
			*pUser = 1;
		}
		else
		{
			*pUser = 0;
		}
		ret = sizeof(int);
		break;
	default:
		ret = -ENOTTY;
	}
	rtdm_mutex_unlock(&ctx->mutex);
	return ret;
}
      
static struct rtdm_driver rtdm_tst_driver = {
	.profile_info		= RTDM_PROFILE_INFO(rtdmtst,
						    RTDM_CLASS_TESTING,
							RTDM_SUBCLASS_RTDMTST,
						    RTTST_PROFILE_VER),
	.device_flags		= RTDM_NAMED_DEVICE,
	.device_count		= 1,
	.context_size		= sizeof(struct rtdm_tst_context),
	.ops = {
		.open		= rtdm_tst_open,
		.close		= rtdm_tst_close,
		.ioctl_rt	= rtdm_tst_ioctl_rt,
		.ioctl_nrt	= rtdm_tst_ioctl_nrt,
	},
};

static struct rtdm_device device = {
	.driver = &rtdm_tst_driver,
	.label = "xillypcie",
};

static int __init rtdm_test_init(void)
{
	int  ret;

	if (!realtime_core_enabled())
		return -ENODEV;

	ret = rtdm_dev_register(&device);

	return ret;
}

static void __exit rtdm_test_exit(void)
{
	rtdm_dev_unregister(&device);
}

module_init(rtdm_test_init);
module_exit(rtdm_test_exit);
