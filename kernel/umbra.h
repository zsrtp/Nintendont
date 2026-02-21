#ifndef __UMBRA_H__
#define __UMBRA_H__

#include "global.h"

#define UMBRA_MAGIC      0x475A  /* "GZ" */

/* EXI command IDs (bits [15:8] of the command word) */
#define UMBRA_CMD_WRITE            0x01
#define UMBRA_CMD_READ             0x02
#define UMBRA_CMD_DELETE           0x03
#define UMBRA_CMD_NET_SEND         0x04
#define UMBRA_CMD_NET_RECV         0x05
#define UMBRA_CMD_NET_CONNECT      0x06
#define UMBRA_CMD_NET_STATE_WRITE  0x07
#define UMBRA_CMD_NET_STATE_READ   0x08
#define UMBRA_CMD_NET_DISCONNECT   0x09

/* Status codes returned to PPC */
#define UMBRA_STATUS_OK              0x00
#define UMBRA_STATUS_NOT_FOUND       0x01
#define UMBRA_STATUS_WRITE_ERR       0x02
#define UMBRA_STATUS_NET_ERR         0x03
#define UMBRA_STATUS_NET_NO_INIT     0x04
#define UMBRA_STATUS_NET_SOCK_FAIL   0x05
#define UMBRA_STATUS_NET_CONN_FAIL   0x06
#define UMBRA_STATUS_NET_SEND_FAIL   0x07
#define UMBRA_STATUS_NET_ALREADY     0x08
#define UMBRA_STATUS_NET_NOT_CONN    0x09

void EXIDeviceUmbra(u8 *Data, u32 Length, u32 Mode);

/* Non-zero when an umbra DMA read is expected next. */
extern u32 umbra_pending_read;

#endif
