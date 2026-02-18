// Nintendont (kernel): Custom EXI device for TPGZ.
// Handles settings persistence, one-shot network sends, and persistent
// UDP game-state streaming (connect/disconnect/state_write/state_read).
// Dispatched from EXI.c MEMCARD_A handler when "GZ" magic is detected.

#include "TPGZ.h"
#include "EXI.h"
#include "net.h"
#include "debug.h"
#include "ff_utf8.h"
#include "string.h"

#define TPGZ_SETTINGS_PATH "/saves/tpgzcfg.bin"

/* Max payload that fits in a single UDP datagram without fragmentation */
#define TPGZ_STATE_BUF_SIZE 1400

/* ── General state ─────────────────────────────────────────────────── */

static u32 tpgz_cmd = 0;
static u32 tpgz_last_status = 0;
static s32 tpgz_net_ios_err = 0;
static u32 tpgz_connect_ip = 0;
static u16 tpgz_connect_port = 0;
static s32 tpgz_join_res = 0;
u32 tpgz_pending_read = 0;

/* ── Persistent online socket state ────────────────────────────────── */

static s32 tpgz_online_sock = -1;
static volatile u32 tpgz_online_active = 0;

/* Outgoing state buffer (game writes via STATE_WRITE, sender thread reads) */
static u8 tpgz_out_buf[TPGZ_STATE_BUF_SIZE] ALIGNED(32);
static volatile u32 tpgz_out_len = 0;
static volatile u32 tpgz_out_ready = 0;

/* Incoming state buffer (receiver thread writes, game reads via STATE_READ) */
static u8 tpgz_in_buf[TPGZ_STATE_BUF_SIZE] ALIGNED(32);
static volatile u32 tpgz_in_len = 0;
static volatile u32 tpgz_in_ready = 0;

/* ── Settings persistence ──────────────────────────────────────────── */

static u32 tpgz_write_settings(const u8 *data, u32 len)
{
	FIL fd;
	UINT wrote;
	int ret;

	ret = f_open_char(&fd, TPGZ_SETTINGS_PATH, FA_WRITE | FA_CREATE_ALWAYS);
	if (ret != FR_OK)
	{
		dbgprintf("TPGZ: Failed to open %s for write: %d\r\n", TPGZ_SETTINGS_PATH, ret);
		return TPGZ_STATUS_WRITE_ERR;
	}

	sync_before_read((void*)data, len);
	f_write(&fd, data, len, &wrote);
	f_sync(&fd);
	f_close(&fd);

	if (wrote != len)
	{
		dbgprintf("TPGZ: Write incomplete: %u/%u\r\n", wrote, len);
		return TPGZ_STATUS_WRITE_ERR;
	}

	dbgprintf("TPGZ: Wrote %u bytes to %s\r\n", wrote, TPGZ_SETTINGS_PATH);
	return TPGZ_STATUS_OK;
}

static u32 tpgz_read_settings(u8 *data, u32 len)
{
	FIL fd;
	UINT readBytes;
	int ret;

	ret = f_open_char(&fd, TPGZ_SETTINGS_PATH, FA_READ | FA_OPEN_EXISTING);
	if (ret != FR_OK)
	{
		dbgprintf("TPGZ: Failed to open %s for read: %d\r\n", TPGZ_SETTINGS_PATH, ret);
		memset(data, 0, len);
		sync_after_write(data, len);
		return TPGZ_STATUS_NOT_FOUND;
	}

	f_read(&fd, data, len, &readBytes);
	f_close(&fd);

	if (readBytes == 0)
	{
		dbgprintf("TPGZ: File empty, clearing buffer\r\n");
		memset(data, 0, len);
		sync_after_write(data, len);
		return TPGZ_STATUS_NOT_FOUND;
	}

	sync_after_write(data, len);
	dbgprintf("TPGZ: Read %u bytes from %s\r\n", readBytes, TPGZ_SETTINGS_PATH);
	return TPGZ_STATUS_OK;
}

static u32 tpgz_delete_settings(void)
{
	int ret = f_unlink_char(TPGZ_SETTINGS_PATH);
	if (ret != FR_OK)
	{
		dbgprintf("TPGZ: Failed to delete %s: %d\r\n", TPGZ_SETTINGS_PATH, ret);
		return TPGZ_STATUS_NOT_FOUND;
	}

	dbgprintf("TPGZ: Deleted %s\r\n", TPGZ_SETTINGS_PATH);
	return TPGZ_STATUS_OK;
}

/* ── One-shot UDP send (legacy CMD_NET_SEND) ───────────────────────── */

static u32 tpgz_net_send_udp(const u8 *data, u32 len)
{
	if (!NetworkStarted)
	{
		dbgprintf("TPGZ NET: network not initialized\r\n");
		return TPGZ_STATUS_NET_NO_INIT;
	}

	if (len < 8)
	{
		dbgprintf("TPGZ NET: packet too small: %u\r\n", len);
		return TPGZ_STATUS_NET_ERR;
	}

	u32 ip_addr     = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
	u16 port        = (data[4] << 8) | data[5];
	u16 payload_len = (data[6] << 8) | data[7];
	const u8 *payload = data + 8;

	if (payload_len > len - 8)
	{
		dbgprintf("TPGZ NET: payload_len %u exceeds buffer %u\r\n", payload_len, len - 8);
		return TPGZ_STATUS_NET_ERR;
	}

	dbgprintf("TPGZ NET: sending %u bytes to %d.%d.%d.%d:%u\r\n",
		payload_len,
		(ip_addr >> 24) & 0xFF, (ip_addr >> 16) & 0xFF,
		(ip_addr >> 8) & 0xFF, ip_addr & 0xFF,
		port);

	s32 sock = net_socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock < 0)
	{
		tpgz_net_ios_err = sock;
		return TPGZ_STATUS_NET_SOCK_FAIL;
	}

	STACK_ALIGN(struct sockaddr_in, dest, 1, 32);
	memset(dest, 0, sizeof(struct sockaddr_in));
	dest->sin_len    = 8;
	dest->sin_family = AF_INET;
	dest->sin_port   = port;
	dest->sin_addr.s_addr = ip_addr;

	s32 res = net_connect(top_fd, sock, (struct sockaddr*)dest);
	if (res < 0)
	{
		tpgz_net_ios_err = res;
		net_close(top_fd, sock);
		return TPGZ_STATUS_NET_CONN_FAIL;
	}

	res = net_sendto(top_fd, sock, (void*)payload, payload_len, 0);
	if (res < 0)
	{
		tpgz_net_ios_err = res;
		net_close(top_fd, sock);
		return TPGZ_STATUS_NET_SEND_FAIL;
	}

	dbgprintf("TPGZ NET: sent %d bytes\r\n", res);
	net_close(top_fd, sock);
	return TPGZ_STATUS_OK;
}

/* ── Persistent online: sender thread ──────────────────────────────── */

static u32 tpgz_sender_thread(void *arg)
{
	/* No dbgprintf in threads — FatFS is not thread-safe */
	while (tpgz_online_active)
	{
		if (tpgz_out_ready)
		{
			u32 len = tpgz_out_len;
			net_sendto(top_fd, tpgz_online_sock,
				   (void*)tpgz_out_buf, len, 0);
			tpgz_out_ready = 0;
		}
		mdelay(5);
	}

	return 0;
}

/* ── Persistent online: receiver thread ────────────────────────────── */

static u32 tpgz_receiver_thread(void *arg)
{
	/* No dbgprintf in threads — FatFS is not thread-safe */
	u8 tmp[TPGZ_STATE_BUF_SIZE];

	while (tpgz_online_active)
	{
		s32 n = net_recvfrom(top_fd, tpgz_online_sock,
				     tmp, TPGZ_STATE_BUF_SIZE, 0);
		if (n > 0)
		{
			if (n > TPGZ_STATE_BUF_SIZE)
				n = TPGZ_STATE_BUF_SIZE;
			memcpy((void*)tpgz_in_buf, tmp, n);
			tpgz_in_len = n;
			tpgz_in_ready = 1;
		}
		else if (n < 0)
		{
			if (!tpgz_online_active)
				break;
			mdelay(100);
		}
	}

	return 0;
}

/* ── NET_CONNECT: open persistent socket + start threads ───────────── */

static u32 tpgz_net_connect(const u8 *data, u32 len)
{
	if (!NetworkStarted)
	{
		dbgprintf("TPGZ ONLINE: network not initialized\r\n");
		return TPGZ_STATUS_NET_NO_INIT;
	}

	if (tpgz_online_sock >= 0)
	{
		dbgprintf("TPGZ ONLINE: already connected\r\n");
		return TPGZ_STATUS_NET_ALREADY;
	}

	if (len < 8)
	{
		dbgprintf("TPGZ ONLINE: connect data too short: %u\r\n", len);
		return TPGZ_STATUS_NET_ERR;
	}

	/* Parse [4B ip][2B port][2B pad] */
	u32 ip_addr = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
	u16 port    = (data[4] << 8) | data[5];

	tpgz_connect_ip = ip_addr;
	tpgz_connect_port = port;

	dbgprintf("TPGZ ONLINE: connecting to %d.%d.%d.%d:%u\r\n",
		(ip_addr >> 24) & 0xFF, (ip_addr >> 16) & 0xFF,
		(ip_addr >> 8) & 0xFF, ip_addr & 0xFF,
		port);

	/* Create UDP socket */
	s32 sock = net_socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock < 0)
	{
		dbgprintf("TPGZ ONLINE: socket() = %d\r\n", sock);
		tpgz_net_ios_err = sock;
		return TPGZ_STATUS_NET_SOCK_FAIL;
	}

	/* Connect socket to server — sets default destination, OS assigns local port */
	STACK_ALIGN(struct sockaddr_in, dest, 1, 32);
	memset(dest, 0, sizeof(struct sockaddr_in));
	dest->sin_len    = 8;
	dest->sin_family = AF_INET;
	dest->sin_port   = port;
	dest->sin_addr.s_addr = ip_addr;

	s32 res = net_connect(top_fd, sock, (struct sockaddr*)dest);
	if (res < 0)
	{
		dbgprintf("TPGZ ONLINE: connect() = %d\r\n", res);
		tpgz_net_ios_err = res;
		net_close(top_fd, sock);
		return TPGZ_STATUS_NET_CONN_FAIL;
	}

	/* Send an initial JOIN packet so the server learns our address.
	 * Format: [player_id=0][msg_type=0x02 JOIN][len=0][no payload] */
	{
		u8 join_pkt[4] ALIGNED(32);
		join_pkt[0] = 0;    /* player_id (assigned by server) */
		join_pkt[1] = 0x02; /* MSG_JOIN */
		join_pkt[2] = 0;    /* payload len high */
		join_pkt[3] = 0;    /* payload len low */
		tpgz_join_res = net_sendto(top_fd, sock, join_pkt, 4, 0);
		dbgprintf("TPGZ ONLINE: JOIN sendto = %d\r\n", tpgz_join_res);
	}

	tpgz_online_sock = sock;

	/* Reset buffers */
	tpgz_out_len = 0;
	tpgz_out_ready = 0;
	tpgz_in_len = 0;
	tpgz_in_ready = 0;
	tpgz_online_active = 1;

	/* Spawn sender thread */
	u32 *send_stack = (u32*)heap_alloc_aligned(0, 0x1000, 32);
	if (send_stack)
	{
		u32 tid = thread_create(tpgz_sender_thread, NULL,
					send_stack, 0x1000 / sizeof(u32), 0x78, 1);
		thread_continue(tid);
		dbgprintf("TPGZ ONLINE: sender tid=%u\r\n", tid);
	}

	/* Spawn receiver thread */
	u32 *recv_stack = (u32*)heap_alloc_aligned(0, 0x1000, 32);
	if (recv_stack)
	{
		u32 tid = thread_create(tpgz_receiver_thread, NULL,
					recv_stack, 0x1000 / sizeof(u32), 0x78, 1);
		thread_continue(tid);
		dbgprintf("TPGZ ONLINE: receiver tid=%u\r\n", tid);
	}

	dbgprintf("TPGZ ONLINE: connected, sock=%d\r\n", sock);
	return TPGZ_STATUS_OK;
}

/* ── NET_DISCONNECT: tear down persistent socket ───────────────────── */

static u32 tpgz_net_disconnect(void)
{
	if (tpgz_online_sock < 0)
	{
		dbgprintf("TPGZ ONLINE: not connected\r\n");
		return TPGZ_STATUS_NET_NOT_CONN;
	}

	dbgprintf("TPGZ ONLINE: disconnecting\r\n");

	/* Signal threads to stop */
	tpgz_online_active = 0;

	/* Send LEAVE packet */
	{
		u8 leave_pkt[4] ALIGNED(32);
		leave_pkt[0] = 0;    /* player_id */
		leave_pkt[1] = 0x03; /* MSG_LEAVE */
		leave_pkt[2] = 0;
		leave_pkt[3] = 0;
		net_sendto(top_fd, tpgz_online_sock, leave_pkt, 4, 0);
	}

	/* Close socket — this also unblocks the receiver thread's recvfrom */
	net_close(top_fd, tpgz_online_sock);
	tpgz_online_sock = -1;

	/* Give threads a moment to notice and exit */
	mdelay(20);

	dbgprintf("TPGZ ONLINE: disconnected\r\n");
	return TPGZ_STATUS_OK;
}

/* ── NET_STATE_WRITE: copy outgoing state into send buffer ─────────── */

static u32 tpgz_net_state_write(const u8 *data, u32 len)
{
	if (tpgz_online_sock < 0)
		return TPGZ_STATUS_NET_NOT_CONN;

	if (len > TPGZ_STATE_BUF_SIZE)
		len = TPGZ_STATE_BUF_SIZE;

	memcpy((void*)tpgz_out_buf, data, len);
	tpgz_out_len = len;
	tpgz_out_ready = 1;

	return TPGZ_STATUS_OK;
}

/* ── EXI dispatch ──────────────────────────────────────────────────── */

void EXIDeviceTPGZ(u8 *Data, u32 Length, u32 Mode)
{
	sync_before_read(Data, Length);

	if (Mode == EXI_WRITE)
	{
		/* Command word: [magic(16) | cmd(8) | reserved(8)] */
		u32 cmdWord = *(u32*)Data;
		u32 cmd = (cmdWord >> 8) & 0xFF;

		if (cmd == TPGZ_CMD_NET_SEND)
		{
			/* Legacy one-shot send: large DMA with IP/port/payload */
			tpgz_cmd = cmd;
			tpgz_pending_read = 1;
			dbgprintf("TPGZ: net_send len=%u\r\n", Length);
			tpgz_last_status = tpgz_net_send_udp(Data + 4, Length - 4);
		}
		else if (cmd == TPGZ_CMD_NET_CONNECT)
		{
			/* Persistent connect: [4B cmd][4B ip][2B port][2B pad] */
			tpgz_cmd = cmd;
			tpgz_pending_read = 1;
			dbgprintf("TPGZ: net_connect len=%u\r\n", Length);
			tpgz_last_status = tpgz_net_connect(Data + 4, Length - 4);
		}
		else if (cmd == TPGZ_CMD_NET_STATE_WRITE)
		{
			/* State write: [4B cmd][payload...] */
			tpgz_cmd = cmd;
			tpgz_pending_read = 1;
			tpgz_last_status = tpgz_net_state_write(Data + 4, Length - 4);
		}
		else if (cmd == TPGZ_CMD_NET_DISCONNECT)
		{
			/* Disconnect: command-only */
			tpgz_cmd = cmd;
			tpgz_pending_read = 1;
			tpgz_last_status = tpgz_net_disconnect();
		}
		else if (cmd == TPGZ_CMD_NET_STATE_READ || cmd == TPGZ_CMD_NET_RECV)
		{
			/* Read commands: just set up for the DMA read phase */
			tpgz_cmd = cmd;
			tpgz_pending_read = 1;
		}
		else if (Length <= 32)
		{
			/* Short DMA = command-only (delete, or command setup for read) */
			tpgz_cmd = cmd;
			tpgz_pending_read = 1;
			dbgprintf("TPGZ: cmd=0x%02X\r\n", tpgz_cmd);

			if (tpgz_cmd == TPGZ_CMD_DELETE)
			{
				tpgz_last_status = tpgz_delete_settings();
			}
		}
		else
		{
			/* Large DMA = command + payload (write settings) */
			tpgz_cmd = cmd;
			tpgz_pending_read = 1;
			dbgprintf("TPGZ: write cmd=0x%02X len=%u\r\n", tpgz_cmd, Length);

			if (tpgz_cmd == TPGZ_CMD_WRITE)
			{
				tpgz_last_status = tpgz_write_settings(Data + 4, Length - 4);
			}
		}
	}
	else
	{
		/* EXI_READ (DMA read) */
		dbgprintf("TPGZ: read cmd=0x%02X len=%u\r\n", tpgz_cmd, Length);

		if (tpgz_cmd == TPGZ_CMD_READ)
		{
			tpgz_read_settings(Data, Length);
		}
		else if (tpgz_cmd == TPGZ_CMD_NET_SEND)
		{
			/* Return status + diagnostic info to PPC */
			memset(Data, 0, Length);
			*(u32*)(Data + 0)  = tpgz_last_status;
			*(s32*)(Data + 4)  = top_fd;
			*(s32*)(Data + 8)  = tpgz_net_ios_err;
			*(u32*)(Data + 12) = NetworkStarted;
			*(s32*)(Data + 16) = net_init_err;
			sync_after_write(Data, Length);
		}
		else if (tpgz_cmd == TPGZ_CMD_NET_RECV)
		{
			/* Legacy recv: return data from listener thread */
			memset(Data, 0, Length);
			if (net_recv_ready)
			{
				u32 copyLen = net_recv_len;
				if (copyLen > Length - 8) copyLen = Length - 8;
				*(u32*)(Data + 0) = TPGZ_STATUS_OK;
				*(u32*)(Data + 4) = copyLen;
				memcpy(Data + 8, (void*)net_recv_buf, copyLen);
				net_recv_ready = 0;
				net_recv_len = 0;
			}
			else
			{
				*(u32*)(Data + 0) = TPGZ_STATUS_OK;
				*(u32*)(Data + 4) = 0;
			}
			sync_after_write(Data, Length);
		}
		else if (tpgz_cmd == TPGZ_CMD_NET_CONNECT ||
			 tpgz_cmd == TPGZ_CMD_NET_DISCONNECT ||
			 tpgz_cmd == TPGZ_CMD_NET_STATE_WRITE)
		{
			/* Return [4B status] */
			memset(Data, 0, Length);
			*(u32*)(Data + 0) = tpgz_last_status;
			sync_after_write(Data, Length);
		}
		else if (tpgz_cmd == TPGZ_CMD_NET_STATE_READ)
		{
			/* Return [4B status][4B len][data...] from receiver buffer */
			memset(Data, 0, Length);
			if (tpgz_online_sock < 0)
			{
				*(u32*)(Data + 0) = TPGZ_STATUS_NET_NOT_CONN;
				*(u32*)(Data + 4) = 0;
			}
			else if (tpgz_in_ready)
			{
				u32 copyLen = tpgz_in_len;
				if (copyLen > Length - 8) copyLen = Length - 8;
				*(u32*)(Data + 0) = TPGZ_STATUS_OK;
				*(u32*)(Data + 4) = copyLen;
				memcpy(Data + 8, (void*)tpgz_in_buf, copyLen);
				tpgz_in_ready = 0;
			}
			else
			{
				*(u32*)(Data + 0) = TPGZ_STATUS_OK;
				*(u32*)(Data + 4) = 0;
			}
			sync_after_write(Data, Length);
		}
		else if (tpgz_cmd == TPGZ_CMD_WRITE || tpgz_cmd == TPGZ_CMD_DELETE)
		{
			/* Return operation status to PPC */
			memset(Data, 0, Length);
			*(u32*)Data = tpgz_last_status;
			sync_after_write(Data, Length);
		}
		tpgz_pending_read = 0;
	}

	write32(EXI_CMD_0, 0);
	sync_after_write((void*)EXI_BASE, 0x20);
}
