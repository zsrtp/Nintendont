/* kernel/net.c
 * IOS socket wrappers for TPGZ networking.
 * Based on Slippi Nintendont and libogc network_wii.c.
 */

#include "global.h"
#include "common.h"
#include "string.h"
#include "debug.h"
#include "net.h"

/* Cached fd for /dev/net/ip/top. */
s32 top_fd ALIGNED(32) = -1;
u32 NetworkStarted = 0;
s32 net_init_err ALIGNED(32) = 0;

/* Shared receive buffer for listener thread */
volatile u32 net_recv_ready = 0;
volatile u32 net_recv_len = 0;
u8 net_recv_buf[NET_RECV_BUF_SIZE] ALIGNED(32);

static const char kd_name[19] ALIGNED(32) = "/dev/net/kd/request";
static const char top_name[15] ALIGNED(32) = "/dev/net/ip/top";

int NCDInit(void)
{
	s32 res;
	int i;

	dbgprintf("TPGZ NET: NCDInit()\r\n");

	/* NWC24 startup - required to bring up the WiFi interface */
	s32 kd_fd = IOS_Open(kd_name, 0);
	dbgprintf("TPGZ NET: kd_fd: %d\r\n", kd_fd);
	if (kd_fd >= 0)
	{
		void *nwc_buf = heap_alloc_aligned(0, 32, 32);
		memset(nwc_buf, 0, 32);

		/* Retry NWC24 startup - it can return -29 while initializing */
		for (i = 0; i < 5; i++)
		{
			res = IOS_Ioctl(kd_fd, IOCTL_NWC24_STARTUP, 0, 0, nwc_buf, 32);
			dbgprintf("TPGZ NET: NWC24_STARTUP[%d]: %d\r\n", i, res);
			s32 nwc_res;
			memcpy(&nwc_res, nwc_buf, sizeof(s32));
			dbgprintf("TPGZ NET: NWC24 result: %d\r\n", nwc_res);
			if (nwc_res != -29)
				break;
			mdelay(200);
		}

		IOS_Close(kd_fd);
		heap_free(0, nwc_buf);
	}
	else
	{
		dbgprintf("TPGZ NET: failed to open kd: %d\r\n", kd_fd);
	}

	/* Open socket driver */
	top_fd = IOS_Open(top_name, 0);
	dbgprintf("TPGZ NET: top_fd: %d\r\n", top_fd);
	if (top_fd < 0)
	{
		net_init_err = top_fd;
		return -1;
	}

	res = IOS_Ioctl(top_fd, IOCTL_SO_STARTUP, 0, 0, 0, 0);
	dbgprintf("TPGZ NET: SO_STARTUP: %d\r\n", res);

	/* Wait for the network interface to come up (poll for valid IP) */
	u32 ip = 0;
	for (i = 0; i < 10; i++)
	{
		ip = IOS_Ioctl(top_fd, IOCTL_SO_GETHOSTID, 0, 0, 0, 0);
		dbgprintf("TPGZ NET: GETHOSTID[%d]: 0x%08x\r\n", i, ip);
		if (ip != 0)
			break;
		mdelay(500);
	}

	dbgprintf("TPGZ NET: IP: %d.%d.%d.%d\r\n",
		(ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
		(ip >> 8) & 0xFF, ip & 0xFF);

	if (ip == 0)
	{
		net_init_err = -39;
		dbgprintf("TPGZ NET: no IP after retries, WiFi not connected?\r\n");
		/* Still mark as started - socket ops will fail with clear errors */
	}

	NetworkStarted = 1;
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

	return IOS_Ioctlv(fd, IOCTLV_SO_RECVFROM, 2, 0, vec);
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

	u8 tmp[NET_RECV_BUF_SIZE];
	while (1)
	{
		s32 n = net_recvfrom(top_fd, sock, tmp, NET_RECV_BUF_SIZE, 0);
		if (n > 0)
		{
			if (n > NET_RECV_BUF_SIZE)
				n = NET_RECV_BUF_SIZE;
			memcpy((void*)net_recv_buf, tmp, n);
			net_recv_len = n;
			net_recv_ready = 1;
			/* no dbgprintf — FatFS not thread-safe */
		}
		else if (n < 0)
		{
			/* no dbgprintf — FatFS not thread-safe */
			mdelay(1000);
		}
	}

	return 0;
}

void NetListenerStart(void)
{
	if (!NetworkStarted)
	{
		dbgprintf("TPGZ NET: can't start listener, network not ready\r\n");
		return;
	}

	u32 *stack = (u32*)heap_alloc_aligned(0, 0x1000, 32);
	if (!stack)
	{
		dbgprintf("TPGZ NET: failed to alloc listener stack\r\n");
		return;
	}

	u32 tid = thread_create(NetListenerThread, NULL, stack, 0x1000 / sizeof(u32), 0x78, 1);
	thread_continue(tid);
	dbgprintf("TPGZ NET: listener thread started (tid=%u)\r\n", tid);
}
