#ifndef _NET_H_
#define _NET_H_

#include "global.h"

#define INADDR_ANY	0

#define IPPROTO_IP	0
#define IPPROTO_TCP	6

#define SOCK_STREAM	1
#define SOCK_DGRAM	2

#define AF_INET		2

/* IOCTL definitions from libogc network_wii.c */
enum {
	IOCTL_SO_ACCEPT = 1,
	IOCTL_SO_BIND,
	IOCTL_SO_CLOSE,
	IOCTL_SO_CONNECT,
	IOCTL_SO_FCNTL,
	IOCTL_SO_GETPEERNAME,
	IOCTL_SO_GETSOCKNAME,
	IOCTL_SO_GETSOCKOPT,
	IOCTL_SO_SETSOCKOPT,
	IOCTL_SO_LISTEN,
	IOCTL_SO_POLL,
	IOCTLV_SO_RECVFROM,
	IOCTLV_SO_SENDTO,
	IOCTL_SO_SHUTDOWN,
	IOCTL_SO_SOCKET,
	IOCTL_SO_GETHOSTID,
	IOCTL_SO_GETHOSTBYNAME,
	IOCTL_SO_GETHOSTBYADDR,
	IOCTLV_SO_GETNAMEINFO,
	IOCTL_SO_UNK14,
	IOCTL_SO_INETATON,
	IOCTL_SO_INETPTON,
	IOCTL_SO_INETNTOP,
	IOCTLV_SO_GETADDRINFO,
	IOCTL_SO_SOCKATMARK,
	IOCTLV_SO_UNK1A,
	IOCTLV_SO_UNK1B,
	IOCTLV_SO_GETINTERFACEOPT,
	IOCTLV_SO_SETINTERFACEOPT,
	IOCTL_SO_SETINTERFACE,
	IOCTL_SO_STARTUP,
};

#define IOCTL_NWC24_STARTUP	0x06

struct in_addr {
	u32 s_addr;
};

struct sockaddr_in {
	u8 sin_len;
	u8 sin_family;
	u16 sin_port;
	struct in_addr sin_addr;
	s8 sin_zero[8];
} __attribute__((packed));

struct sockaddr {
	u8 sa_len;
	u8 sa_family;
	u8 sa_data[14];
} __attribute__((packed));

struct connect_params {
	u32 socket;
	u32 has_addr;
	u8 name[28];
};

struct sendto_params {
	u32 socket;
	u32 flags;
	u32 has_destaddr;
	u8 destaddr[28];
};

extern s32 top_fd;
extern u32 NetworkStarted;
extern s32 net_init_err;

extern volatile u32 net_recv_ready;
extern volatile u32 net_recv_len;
extern u8 net_recv_buf[];

#define NET_LISTEN_PORT 52224
#define NET_RECV_BUF_SIZE 1400

int NCDInit(void);
void NetListenerStart(void);
s32 net_socket(s32 fd, u32 domain, u32 type, u32 protocol);
s32 net_close(s32 fd, s32 socket);
s32 net_bind(s32 fd, s32 socket, u32 addr, u16 port);
s32 net_connect(s32 fd, s32 socket, struct sockaddr *name);
s32 net_sendto(s32 fd, s32 socket, void *data, s32 len, u32 flags);
s32 net_recvfrom(s32 fd, s32 socket, void *mem, s32 len, u32 flags);

#endif
