#include "umbra.h"
#include "EXI.h"
#include "net.h"
#include "gdb.h"
#include "debug.h"
#include "ff_utf8.h"
#include "string.h"

#define UMBRA_SETTINGS_PATH "/saves/umbracfg.bin"

#define UMBRA_STATE_BUF_SIZE 1400

static u32 umbra_cmd = 0;
static u32 umbra_last_status = 0;
static s32 umbra_net_ios_err = 0;
static u32 umbra_connect_ip = 0;
static u16 umbra_connect_port = 0;
static s32 umbra_join_res = 0;
u32 umbra_pending_read = 0;

static s32 umbra_online_sock = -1;
static volatile u32 umbra_online_active = 0;

static u8 umbra_out_buf[UMBRA_STATE_BUF_SIZE] ALIGNED(32);
static volatile u32 umbra_out_len = 0;
static volatile u32 umbra_out_ready = 0;

static u8 umbra_in_buf[UMBRA_STATE_BUF_SIZE] ALIGNED(32);
static volatile u32 umbra_in_len = 0;
static volatile u32 umbra_in_ready = 0;

static u32 umbra_write_settings(const u8 *data, u32 len)
{
	FIL fd;
	UINT wrote;
	int ret;

	ret = f_open_char(&fd, UMBRA_SETTINGS_PATH, FA_WRITE | FA_CREATE_ALWAYS);
	if (ret != FR_OK)
	{
		dbgprintf("UMBRA: Failed to open %s for write: %d\r\n", UMBRA_SETTINGS_PATH, ret);
		return UMBRA_STATUS_WRITE_ERR;
	}

	sync_before_read((void*)data, len);
	f_write(&fd, data, len, &wrote);
	f_sync(&fd);
	f_close(&fd);

	if (wrote != len)
	{
		dbgprintf("UMBRA: Write incomplete: %u/%u\r\n", wrote, len);
		return UMBRA_STATUS_WRITE_ERR;
	}

	dbgprintf("UMBRA: Wrote %u bytes to %s\r\n", wrote, UMBRA_SETTINGS_PATH);
	return UMBRA_STATUS_OK;
}

static u32 umbra_read_settings(u8 *data, u32 len)
{
	FIL fd;
	UINT readBytes;
	int ret;

	ret = f_open_char(&fd, UMBRA_SETTINGS_PATH, FA_READ | FA_OPEN_EXISTING);
	if (ret != FR_OK)
	{
		dbgprintf("UMBRA: Failed to open %s for read: %d\r\n", UMBRA_SETTINGS_PATH, ret);
		memset(data, 0, len);
		sync_after_write(data, len);
		return UMBRA_STATUS_NOT_FOUND;
	}

	f_read(&fd, data, len, &readBytes);
	f_close(&fd);

	if (readBytes == 0)
	{
		dbgprintf("UMBRA: File empty, clearing buffer\r\n");
		memset(data, 0, len);
		sync_after_write(data, len);
		return UMBRA_STATUS_NOT_FOUND;
	}

	sync_after_write(data, len);
	dbgprintf("UMBRA: Read %u bytes from %s\r\n", readBytes, UMBRA_SETTINGS_PATH);
	return UMBRA_STATUS_OK;
}

static u32 umbra_delete_settings(void)
{
	int ret = f_unlink_char(UMBRA_SETTINGS_PATH);
	if (ret != FR_OK)
	{
		dbgprintf("UMBRA: Failed to delete %s: %d\r\n", UMBRA_SETTINGS_PATH, ret);
		return UMBRA_STATUS_NOT_FOUND;
	}

	dbgprintf("UMBRA: Deleted %s\r\n", UMBRA_SETTINGS_PATH);
	return UMBRA_STATUS_OK;
}

static u32 umbra_net_send_udp(const u8 *data, u32 len)
{
	if (!NetworkStarted)
	{
		dbgprintf("UMBRA NET: network not initialized\r\n");
		return UMBRA_STATUS_NET_NO_INIT;
	}

	if (len < 8)
	{
		dbgprintf("UMBRA NET: packet too small: %u\r\n", len);
		return UMBRA_STATUS_NET_ERR;
	}

	u32 ip_addr     = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
	u16 port        = (data[4] << 8) | data[5];
	u16 payload_len = (data[6] << 8) | data[7];
	const u8 *payload = data + 8;

	if (payload_len > len - 8)
	{
		dbgprintf("UMBRA NET: payload_len %u exceeds buffer %u\r\n", payload_len, len - 8);
		return UMBRA_STATUS_NET_ERR;
	}

	dbgprintf("UMBRA NET: sending %u bytes to %d.%d.%d.%d:%u\r\n",
		payload_len,
		(ip_addr >> 24) & 0xFF, (ip_addr >> 16) & 0xFF,
		(ip_addr >> 8) & 0xFF, ip_addr & 0xFF,
		port);

	s32 sock = net_socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock < 0)
	{
		umbra_net_ios_err = sock;
		return UMBRA_STATUS_NET_SOCK_FAIL;
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
		umbra_net_ios_err = res;
		net_close(top_fd, sock);
		return UMBRA_STATUS_NET_CONN_FAIL;
	}

	res = net_sendto(top_fd, sock, (void*)payload, payload_len, 0);
	if (res < 0)
	{
		umbra_net_ios_err = res;
		net_close(top_fd, sock);
		return UMBRA_STATUS_NET_SEND_FAIL;
	}

	dbgprintf("UMBRA NET: sent %d bytes\r\n", res);
	net_close(top_fd, sock);
	return UMBRA_STATUS_OK;
}

static u32 umbra_sender_thread(void *arg)
{
	/* No dbgprintf in threads — FatFS is not thread-safe */
	while (umbra_online_active)
	{
		if (umbra_out_ready)
		{
			u32 len = umbra_out_len;
			net_sendto(top_fd, umbra_online_sock,
				   (void*)umbra_out_buf, len, 0);
			umbra_out_ready = 0;
		}
		mdelay(5);
	}

	return 0;
}

static u32 umbra_receiver_thread(void *arg)
{
	/* No dbgprintf in threads — FatFS is not thread-safe */
	u8 tmp[UMBRA_STATE_BUF_SIZE];

	while (umbra_online_active)
	{
		s32 n = net_recvfrom(top_fd, umbra_online_sock,
				     tmp, UMBRA_STATE_BUF_SIZE, 0);
		if (n > 0)
		{
			if (n > UMBRA_STATE_BUF_SIZE)
				n = UMBRA_STATE_BUF_SIZE;
			memcpy((void*)umbra_in_buf, tmp, n);
			umbra_in_len = n;
			umbra_in_ready = 1;
		}
		else if (n < 0)
		{
			if (!umbra_online_active)
				break;
			mdelay(100);
		}
	}

	return 0;
}

static u32 umbra_net_connect(const u8 *data, u32 len)
{
	if (!NetworkStarted)
	{
		dbgprintf("UMBRA ONLINE: network not initialized\r\n");
		return UMBRA_STATUS_NET_NO_INIT;
	}

	if (umbra_online_sock >= 0)
	{
		dbgprintf("UMBRA ONLINE: already connected\r\n");
		return UMBRA_STATUS_NET_ALREADY;
	}

	if (len < 8)
	{
		dbgprintf("UMBRA ONLINE: connect data too short: %u\r\n", len);
		return UMBRA_STATUS_NET_ERR;
	}

	/* Parse [4B ip][2B port][2B pad] */
	u32 ip_addr = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
	u16 port    = (data[4] << 8) | data[5];

	umbra_connect_ip = ip_addr;
	umbra_connect_port = port;

	dbgprintf("UMBRA ONLINE: connecting to %d.%d.%d.%d:%u\r\n",
		(ip_addr >> 24) & 0xFF, (ip_addr >> 16) & 0xFF,
		(ip_addr >> 8) & 0xFF, ip_addr & 0xFF,
		port);

	s32 sock = net_socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock < 0)
	{
		dbgprintf("UMBRA ONLINE: socket() = %d\r\n", sock);
		umbra_net_ios_err = sock;
		return UMBRA_STATUS_NET_SOCK_FAIL;
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
		dbgprintf("UMBRA ONLINE: connect() = %d\r\n", res);
		umbra_net_ios_err = res;
		net_close(top_fd, sock);
		return UMBRA_STATUS_NET_CONN_FAIL;
	}

	/* JOIN packet so the server learns our address */
	{
		u8 join_pkt[4] ALIGNED(32);
		join_pkt[0] = 0;
		join_pkt[1] = 0x02;
		join_pkt[2] = 0;
		join_pkt[3] = 0;
		umbra_join_res = net_sendto(top_fd, sock, join_pkt, 4, 0);
		dbgprintf("UMBRA ONLINE: JOIN sendto = %d\r\n", umbra_join_res);
	}

	umbra_online_sock = sock;

	umbra_out_len = 0;
	umbra_out_ready = 0;
	umbra_in_len = 0;
	umbra_in_ready = 0;
	umbra_online_active = 1;

	u32 *send_stack = (u32*)heap_alloc_aligned(0, 0x1000, 32);
	if (send_stack)
	{
		u32 tid = thread_create(umbra_sender_thread, NULL,
					send_stack, 0x1000 / sizeof(u32), 0x78, 1);
		thread_continue(tid);
		dbgprintf("UMBRA ONLINE: sender tid=%u\r\n", tid);
	}

	u32 *recv_stack = (u32*)heap_alloc_aligned(0, 0x1000, 32);
	if (recv_stack)
	{
		u32 tid = thread_create(umbra_receiver_thread, NULL,
					recv_stack, 0x1000 / sizeof(u32), 0x78, 1);
		thread_continue(tid);
		dbgprintf("UMBRA ONLINE: receiver tid=%u\r\n", tid);
	}

	dbgprintf("UMBRA ONLINE: connected, sock=%d\r\n", sock);
	return UMBRA_STATUS_OK;
}

static u32 umbra_net_disconnect(void)
{
	if (umbra_online_sock < 0)
	{
		dbgprintf("UMBRA ONLINE: not connected\r\n");
		return UMBRA_STATUS_NET_NOT_CONN;
	}

	dbgprintf("UMBRA ONLINE: disconnecting\r\n");

	umbra_online_active = 0;

	{
		u8 leave_pkt[4] ALIGNED(32);
		leave_pkt[0] = 0;
		leave_pkt[1] = 0x03;
		leave_pkt[2] = 0;
		leave_pkt[3] = 0;
		net_sendto(top_fd, umbra_online_sock, leave_pkt, 4, 0);
	}

	/* Close socket — this also unblocks the receiver thread's recvfrom */
	net_close(top_fd, umbra_online_sock);
	umbra_online_sock = -1;

	mdelay(20);

	dbgprintf("UMBRA ONLINE: disconnected\r\n");
	return UMBRA_STATUS_OK;
}

static u32 umbra_net_state_write(const u8 *data, u32 len)
{
	if (umbra_online_sock < 0)
		return UMBRA_STATUS_NET_NOT_CONN;

	if (len > UMBRA_STATE_BUF_SIZE)
		len = UMBRA_STATE_BUF_SIZE;

	memcpy((void*)umbra_out_buf, data, len);
	umbra_out_len = len;
	umbra_out_ready = 1;

	return UMBRA_STATUS_OK;
}

void EXIDeviceUmbra(u8 *Data, u32 Length, u32 Mode)
{
	sync_before_read(Data, Length);

	if (Mode == EXI_WRITE)
	{
		/* Command word: [magic(16) | cmd(8) | reserved(8)] */
		u32 cmdWord = *(u32*)Data;
		u32 cmd = (cmdWord >> 8) & 0xFF;

		if (cmd == UMBRA_CMD_NET_SEND)
		{
			umbra_cmd = cmd;
			umbra_pending_read = 1;
			dbgprintf("UMBRA: net_send len=%u\r\n", Length);
			umbra_last_status = umbra_net_send_udp(Data + 4, Length - 4);
		}
		else if (cmd == UMBRA_CMD_NET_CONNECT)
		{
			umbra_cmd = cmd;
			umbra_pending_read = 1;
			dbgprintf("UMBRA: net_connect len=%u\r\n", Length);
			umbra_last_status = umbra_net_connect(Data + 4, Length - 4);
		}
		else if (cmd == UMBRA_CMD_NET_STATE_WRITE)
		{
			umbra_cmd = cmd;
			umbra_pending_read = 1;
			umbra_last_status = umbra_net_state_write(Data + 4, Length - 4);
		}
		else if (cmd == UMBRA_CMD_NET_DISCONNECT)
		{
			umbra_cmd = cmd;
			umbra_pending_read = 1;
			umbra_last_status = umbra_net_disconnect();
		}
		else if (cmd == UMBRA_CMD_GDB_START)
		{
			umbra_cmd = cmd;
			umbra_pending_read = 1;
			u16 port = (Data[4] << 8) | Data[5];
			dbgprintf("UMBRA: gdb_start port=%u\r\n", port);
			s32 gdb_err = gdb_start(port);
			dbgprintf("UMBRA: gdb_start result=%d\r\n", gdb_err);
			umbra_last_status = (gdb_err == 0) ? UMBRA_STATUS_OK : UMBRA_STATUS_NET_ERR;
		}
		else if (cmd == UMBRA_CMD_NET_STATE_READ || cmd == UMBRA_CMD_NET_RECV)
		{
			umbra_cmd = cmd;
			umbra_pending_read = 1;
		}
		else if (Length <= 32)
		{
			umbra_cmd = cmd;
			umbra_pending_read = 1;
			dbgprintf("UMBRA: cmd=0x%02X\r\n", umbra_cmd);

			if (umbra_cmd == UMBRA_CMD_DELETE)
			{
				umbra_last_status = umbra_delete_settings();
			}
		}
		else
		{
			umbra_cmd = cmd;
			umbra_pending_read = 1;
			dbgprintf("UMBRA: write cmd=0x%02X len=%u\r\n", umbra_cmd, Length);

			if (umbra_cmd == UMBRA_CMD_WRITE)
			{
				umbra_last_status = umbra_write_settings(Data + 4, Length - 4);
			}
		}
	}
	else
	{
		dbgprintf("UMBRA: read cmd=0x%02X len=%u\r\n", umbra_cmd, Length);

		if (umbra_cmd == UMBRA_CMD_READ)
		{
			umbra_read_settings(Data, Length);
		}
		else if (umbra_cmd == UMBRA_CMD_NET_SEND)
		{
			memset(Data, 0, Length);
			*(u32*)(Data + 0)  = umbra_last_status;
			*(s32*)(Data + 4)  = top_fd;
			*(s32*)(Data + 8)  = umbra_net_ios_err;
			*(u32*)(Data + 12) = NetworkStarted;
			*(s32*)(Data + 16) = net_init_err;
			sync_after_write(Data, Length);
		}
		else if (umbra_cmd == UMBRA_CMD_NET_RECV)
		{
			memset(Data, 0, Length);
			if (net_recv_ready)
			{
				u32 copyLen = net_recv_len;
				if (copyLen > Length - 8) copyLen = Length - 8;
				*(u32*)(Data + 0) = UMBRA_STATUS_OK;
				*(u32*)(Data + 4) = copyLen;
				memcpy(Data + 8, (void*)net_recv_buf, copyLen);
				net_recv_ready = 0;
				net_recv_len = 0;
			}
			else
			{
				*(u32*)(Data + 0) = UMBRA_STATUS_OK;
				*(u32*)(Data + 4) = 0;
			}
			sync_after_write(Data, Length);
		}
		else if (umbra_cmd == UMBRA_CMD_NET_CONNECT ||
			 umbra_cmd == UMBRA_CMD_NET_DISCONNECT ||
			 umbra_cmd == UMBRA_CMD_NET_STATE_WRITE)
		{
			memset(Data, 0, Length);
			*(u32*)(Data + 0) = umbra_last_status;
			sync_after_write(Data, Length);
		}
		else if (umbra_cmd == UMBRA_CMD_NET_STATE_READ)
		{
			memset(Data, 0, Length);
			if (umbra_online_sock < 0)
			{
				*(u32*)(Data + 0) = UMBRA_STATUS_NET_NOT_CONN;
				*(u32*)(Data + 4) = 0;
			}
			else if (umbra_in_ready)
			{
				u32 copyLen = umbra_in_len;
				if (copyLen > Length - 8) copyLen = Length - 8;
				*(u32*)(Data + 0) = UMBRA_STATUS_OK;
				*(u32*)(Data + 4) = copyLen;
				memcpy(Data + 8, (void*)umbra_in_buf, copyLen);
				umbra_in_ready = 0;
			}
			else
			{
				*(u32*)(Data + 0) = UMBRA_STATUS_OK;
				*(u32*)(Data + 4) = 0;
			}
			sync_after_write(Data, Length);
		}
		else if (umbra_cmd == UMBRA_CMD_GDB_START)
		{
			/* Read live values from SHM, not cached debug vars */
			{
				u32 live_hb, live_seen, live_halt, live_state;
				sync_before_read((void*)GDB_SHM_ADDR_ARM, GDB_SHM_SIZE);
				live_hb   = read32(GDB_SHM_ADDR_ARM + GDB_SHM_OFF_PPC_HEARTBEAT);
				live_seen = read32(GDB_SHM_ADDR_ARM + GDB_SHM_OFF_PPC_HALT_SEEN);
				live_halt = read32(GDB_SHM_ADDR_ARM + GDB_SHM_OFF_HALT_REQ);
				live_state = read32(GDB_SHM_ADDR_ARM + GDB_SHM_OFF_STATE);

				memset(Data, 0, Length);
				*(u32*)(Data + 0) = umbra_last_status;
				if (Length >= 40)
				{
					*(u32*)(Data + 4) = gdb_dbg_state;
					*(s32*)(Data + 8) = gdb_dbg_err;
					*(u32*)(Data + 12) = gdb_dbg_polls;
					*(s32*)(Data + 16) = gdb_dbg_last_poll;
					*(u32*)(Data + 20) = live_hb;
					*(u32*)(Data + 24) = live_seen;
					*(u32*)(Data + 28) = live_halt;
					*(s32*)(Data + 32) = gdb_dbg_client_err;
					*(u32*)(Data + 36) = gdb_dbg_client_polls;
				}
				sync_after_write(Data, Length);
				dbgprintf("UMBRA: gdb_dbg st=%u err=%d polls=%u lpoll=%d hb=%u seen=%u halt=%u state=%u cerr=%d cpolls=%u cmds=0x%08X\r\n",
					gdb_dbg_state, gdb_dbg_err, gdb_dbg_polls, gdb_dbg_last_poll,
					live_hb, live_seen, live_halt, live_state,
					gdb_dbg_client_err, gdb_dbg_client_polls,
					gdb_dbg_shm_halt);
			}
		}
		else if (umbra_cmd == UMBRA_CMD_WRITE || umbra_cmd == UMBRA_CMD_DELETE)
		{
			memset(Data, 0, Length);
			*(u32*)Data = umbra_last_status;
			sync_after_write(Data, Length);
		}
		umbra_pending_read = 0;
	}

	write32(EXI_CMD_0, 0);
	sync_after_write((void*)EXI_BASE, 0x20);
}
