#ifndef __TPGZ_H__
#define __TPGZ_H__

#include "global.h"

#define TPGZ_MAGIC      0x475A  /* "GZ" */

void EXIDeviceTPGZ(u8 *Data, u32 Length, u32 Mode);

/* Non-zero when a TPGZ DMA read is expected next. */
extern u32 tpgz_pending_read;

#endif
