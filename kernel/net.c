#include "global.h"
#include "common.h"
#include "string.h"
#include "debug.h"
#include "net.h"

s32 top_fd ALIGNED(32) = -1;
u32 NetworkStarted = 0;
s32 net_init_err ALIGNED(32) = 0;

volatile u32 net_recv_ready = 0;
volatile u32 net_recv_len = 0;
u8 net_recv_buf[NET_RECV_BUF_SIZE] ALIGNED(32);

static const char kd_name[19] ALIGNED(32) = "/dev/net/kd/request";
static const char top_name[15] ALIGNED(32) = "/dev/net/ip/top";

static u32 NetListenerThread(void *arg);

static u32 NCDInitThread(void *arg)
{
	/* No dbgprintf in threads — FatFS is not thread-safe */
	s32 res;
	int i;

	/* NWC24 startup - required to bring up the WiFi interface */
	s32 kd_fd = IOS_Open(kd_name, 0);
	if (kd_fd >= 0)
	{
		void *nwc_buf = heap_alloc_aligned(0, 32, 32);
		memset(nwc_buf, 0, 32);

		/* Retry NWC24 startup - it can return -29 while initializing */
		for (i = 0; i < 5; i++)
		{
			IOS_Ioctl(kd_fd, IOCTL_NWC24_STARTUP, 0, 0, nwc_buf, 32);
			s32 nwc_res;
			memcpy(&nwc_res, nwc_buf, sizeof(s32));
			if (nwc_res != -29)
				break;
			mdelay(200);
		}

		IOS_Close(kd_fd);
		heap_free(0, nwc_buf);
	}

	top_fd = IOS_Open(top_name, 0);
	if (top_fd < 0)
	{
		net_init_err = top_fd;
		return 1;
	}

	res = IOS_Ioctl(top_fd, IOCTL_SO_STARTUP, 0, 0, 0, 0);
	(void)res;

	u32 ip = 0;
	for (i = 0; i < 10; i++)
	{
		ip = IOS_Ioctl(top_fd, IOCTL_SO_GETHOSTID, 0, 0, 0, 0);
		if (ip != 0)
			break;
		mdelay(500);
	}

	if (ip == 0)
		net_init_err = -39;

	NetworkStarted = 1;

	/* Start the UDP listener now that the network is ready */
	u32 *listener_stack = (u32*)heap_alloc_aligned(0, 0x1000, 32);
	if (listener_stack)
	{
		u32 tid = thread_create(NetListenerThread, NULL,
					listener_stack, 0x1000 / sizeof(u32), 0x78, 1);
		thread_continue(tid);
	}

	return 0;
}

int NCDInit(void)
{
	dbgprintf("UMBRA NET: NCDInit() starting background thread\r\n");

	u32 *stack = (u32*)heap_alloc_aligned(0, 0x1000, 32);
	if (!stack)
	{
		dbgprintf("UMBRA NET: failed to alloc NCDInit stack\r\n");
		return -1;
	}

	u32 tid = thread_create(NCDInitThread, NULL, stack, 0x1000 / sizeof(u32), 0x78, 1);
	thread_continue(tid);
	dbgprintf("UMBRA NET: NCDInit thread started (tid=%u)\r\n", tid);
	return 0;
}

s32 net_socket(s32 fd, u32 domain, u32 type, u32 protocol)
{
	STACK_ALIGN(u32, params, 3, 32);

	if (fd < 0) return -62;

	params[0] = domain;
	params[1] = type;
	params[2] = protocol;

	return IOS_Ioctl(fd, IOCTL_SO_SOCKET, params, 12, 0, 0);
}

s32 net_close(s32 fd, s32 socket)
{
	STACK_ALIGN(u32, params, 1, 32);

	if (fd < 0) return -62;

	*params = socket;
	return IOS_Ioctl(fd, IOCTL_SO_CLOSE, params, 4, 0, 0);
}

s32 net_connect(s32 fd, s32 socket, struct sockaddr *name)
{
	STACK_ALIGN(struct connect_params, params, 1, 32);

	if (fd < 0) return -62;

	name->sa_len = 8;

	memset(params, 0, sizeof(struct connect_params));
	params->socket = socket;
	params->has_addr = 1;
	memcpy(params->name, name, 8);

	return IOS_Ioctl(fd, IOCTL_SO_CONNECT, params,
			sizeof(struct connect_params), 0, 0);
}

s32 net_sendto(s32 fd, s32 socket, void *data, s32 len, u32 flags)
{
	STACK_ALIGN(struct sendto_params, params, 1, 32);
	STACK_ALIGN(ioctlv, vec, 2, 32);

	if (fd < 0) return -62;

	u8 *message_buf = (u8*)heap_alloc_aligned(0, len, 32);
	if (message_buf == NULL)
		return -1;

	memset(params, 0, sizeof(struct sendto_params));
	memcpy(message_buf, data, len);

	params->socket = socket;
	params->flags = flags;
	params->has_destaddr = 0;

	vec[0].data = message_buf;
	vec[0].len = len;
	vec[1].data = params;
	vec[1].len = sizeof(struct sendto_params);

	sync_after_write(message_buf, len);
	sync_after_write(params, sizeof(struct sendto_params));

	s32 res = IOS_Ioctlv(fd, IOCTLV_SO_SENDTO, 2, 0, vec);

	heap_free(0, message_buf);

	return res;
}

s32 net_bind(s32 fd, s32 socket, u32 addr, u16 port)
{
	STACK_ALIGN(struct connect_params, params, 1, 32);

	if (fd < 0) return -62;

	memset(params, 0, sizeof(struct connect_params));
	params->socket = socket;
	params->has_addr = 1;

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = 8;
	sin.sin_family = AF_INET;
	sin.sin_port = port;
	sin.sin_addr.s_addr = addr;
	memcpy(params->name, &sin, 8);

	return IOS_Ioctl(fd, IOCTL_SO_BIND, params, sizeof(struct connect_params), 0, 0);
}

s32 net_recvfrom(s32 fd, s32 socket, void *mem, s32 len, u32 flags)
{
	STACK_ALIGN(u32, params, 2, 32);
	STACK_ALIGN(ioctlv, vec, 3, 32);

	if (fd < 0) return -62;

	params[0] = socket;
	params[1] = flags;

	vec[0].data = params;
	vec[0].len = 8;
	vec[1].data = mem;
	vec[1].len = len;
	vec[2].data = NULL;
	vec[2].len = 0;

	sync_after_write(params, 8);

	/* 1 input, 2 outputs (data buf, src addr) */
	s32 ret = IOS_Ioctlv(fd, IOCTLV_SO_RECVFROM, 1, 2, vec);

	if (ret > 0)
		sync_before_read(mem, ret);

	return ret;
}

s32 net_listen(s32 fd, s32 socket, u32 backlog)
{
	STACK_ALIGN(u32, params, 2, 32);

	if (fd < 0) return -62;

	params[0] = socket;
	params[1] = backlog;

	return IOS_Ioctl(fd, IOCTL_SO_LISTEN, params, 8, 0, 0);
}

s32 net_accept(s32 fd, s32 socket)
{
	STACK_ALIGN(u32, params, 1, 32);
	STACK_ALIGN(struct sockaddr, addr, 1, 32);

	if (fd < 0) return -62;

	*params = socket;
	addr->sa_len = 8;
	addr->sa_family = AF_INET;

	return IOS_Ioctl(fd, IOCTL_SO_ACCEPT, params, 4, addr, 8);
}

s32 net_poll(s32 fd, struct pollsd *sds, u32 nsds, s32 timeout)
{
	STACK_ALIGN(u64, params, 1, 32);

	if (fd < 0) return -62;

	u32 sz = nsds * sizeof(struct pollsd);
	struct pollsd *aligned = (struct pollsd*)heap_alloc_aligned(0, sz, 32);
	if (!aligned)
		return -1;

	memcpy(aligned, sds, sz);

	/* IOS expects 8 bytes: first 4 unused, second 4 = timeout */
	union ullc outv;
	outv.ul[0] = 0;
	outv.ul[1] = timeout;
	params[0] = outv.ull;

	sync_after_write(params, 8);
	sync_after_write(aligned, sz);

	s32 res = IOS_Ioctl(fd, IOCTL_SO_POLL, params, 8, aligned, sz);

	sync_before_read(aligned, sz);
	memcpy(sds, aligned, sz);
	heap_free(0, aligned);

	return res;
}

s32 net_setsockopt(s32 fd, s32 socket, u32 level, u32 optname,
		   void *optval, u32 optlen)
{
	STACK_ALIGN(struct setsockopt_params, params, 1, 32);

	if (fd < 0) return -62;

	memset(params, 0, sizeof(struct setsockopt_params));
	params->socket = socket;
	params->level = level;
	params->optname = optname;
	params->optlen = optlen;

	if (optval && optlen)
		memcpy(params->optval, optval, optlen);

	return IOS_Ioctl(fd, IOCTL_SO_SETSOCKOPT, params,
			 sizeof(struct setsockopt_params), NULL, 0);
}

s32 net_fcntl(s32 fd, s32 socket, u32 cmd, u32 arg)
{
	STACK_ALIGN(u32, params, 3, 32);

	if (fd < 0) return -62;

	params[0] = socket;
	params[1] = cmd;
	params[2] = arg;

	return IOS_Ioctl(fd, IOCTL_SO_FCNTL, params, 12, 0, 0);
}

static u32 NetListenerThread(void *arg)
{
	/* No dbgprintf in threads — FatFS is not thread-safe */

	s32 sock = net_socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock < 0)
		return 1;

	s32 res = net_bind(top_fd, sock, INADDR_ANY, NET_LISTEN_PORT);
	if (res < 0)
	{
		net_close(top_fd, sock);
		return 1;
	}

	/* Use poll (IOS_Ioctl) + recv (IOS_Ioctlv) instead of blocking recv.
	 * A blocking IOS_Ioctlv holds the IOS network module's single
	 * processing slot, preventing ALL other Ioctlv calls (including
	 * TCP recv/send from other threads like GDB). */
	u8 tmp[NET_RECV_BUF_SIZE];
	struct pollsd psd;
	while (1)
	{
		psd.socket = sock;
		psd.events = POLLIN;
		psd.revents = 0;

		s32 pr = net_poll(top_fd, &psd, 1, 100);
		if (pr <= 0 || !(psd.revents & POLLIN))
			continue;

		s32 n = net_recvfrom(top_fd, sock, tmp, NET_RECV_BUF_SIZE, 0);
		if (n > 0)
		{
			if (n > NET_RECV_BUF_SIZE)
				n = NET_RECV_BUF_SIZE;
			memcpy((void*)net_recv_buf, tmp, n);
			net_recv_len = n;
			net_recv_ready = 1;
		}
		else if (n < 0)
		{
			mdelay(100);
		}
	}

	return 0;
}

void NetListenerStart(void)
{
	if (!NetworkStarted)
	{
		dbgprintf("UMBRA NET: can't start listener, network not ready\r\n");
		return;
	}

	u32 *stack = (u32*)heap_alloc_aligned(0, 0x1000, 32);
	if (!stack)
	{
		dbgprintf("UMBRA NET: failed to alloc listener stack\r\n");
		return;
	}

	u32 tid = thread_create(NetListenerThread, NULL, stack, 0x1000 / sizeof(u32), 0x78, 1);
	thread_continue(tid);
	dbgprintf("UMBRA NET: listener thread started (tid=%u)\r\n", tid);
}
