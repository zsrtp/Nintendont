#ifndef __TPGZ_H__
#define __TPGZ_H__

#include "global.h"

#define TPGZ_MAGIC      0x475A  /* "GZ" */

/* EXI command IDs (bits [15:8] of the command word) */
#define TPGZ_CMD_WRITE            0x01
#define TPGZ_CMD_READ             0x02
#define TPGZ_CMD_DELETE           0x03
#define TPGZ_CMD_NET_SEND         0x04
#define TPGZ_CMD_NET_RECV         0x05
#define TPGZ_CMD_NET_CONNECT      0x06
#define TPGZ_CMD_NET_STATE_WRITE  0x07
#define TPGZ_CMD_NET_STATE_READ   0x08
#define TPGZ_CMD_NET_DISCONNECT   0x09

/* Status codes returned to PPC */
#define TPGZ_STATUS_OK              0x00
#define TPGZ_STATUS_NOT_FOUND       0x01
#define TPGZ_STATUS_WRITE_ERR       0x02
#define TPGZ_STATUS_NET_ERR         0x03
#define TPGZ_STATUS_NET_NO_INIT     0x04
#define TPGZ_STATUS_NET_SOCK_FAIL   0x05
#define TPGZ_STATUS_NET_CONN_FAIL   0x06
#define TPGZ_STATUS_NET_SEND_FAIL   0x07
#define TPGZ_STATUS_NET_ALREADY     0x08
#define TPGZ_STATUS_NET_NOT_CONN    0x09

void EXIDeviceTPGZ(u8 *Data, u32 Length, u32 Mode);

/* Non-zero when a TPGZ DMA read is expected next. */
extern u32 tpgz_pending_read;

#endif
