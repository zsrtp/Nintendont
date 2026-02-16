// Nintendont (kernel): Custom EXI device for TPGZ settings persistence.
// Handles save/load/delete of tpgz settings to/from SD card.
// Dispatched from EXI.c MEMCARD_A handler when "GZ" magic is detected.

#include "TPGZ.h"
#include "EXI.h"
#include "debug.h"
#include "ff_utf8.h"
#include "string.h"

#define TPGZ_CMD_WRITE  0x01
#define TPGZ_CMD_READ   0x02
#define TPGZ_CMD_DELETE 0x03

#define TPGZ_STATUS_OK          0x00
#define TPGZ_STATUS_NOT_FOUND   0x01
#define TPGZ_STATUS_WRITE_ERR   0x02

#define TPGZ_SETTINGS_PATH "/saves/tpgzcfg.bin"

static u32 tpgz_cmd = 0;
static u32 tpgz_last_status = 0;
u32 tpgz_pending_read = 0;

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

void EXIDeviceTPGZ(u8 *Data, u32 Length, u32 Mode)
{
	sync_before_read(Data, Length);

	if (Mode == EXI_WRITE)
	{
		// Command word: [magic(16) | cmd(8) | reserved(8)]
		u32 cmdWord = *(u32*)Data;
		u32 cmd = (cmdWord >> 8) & 0xFF;

		if (Length <= 32)
		{
			// Short DMA = command-only (delete, or command setup for read)
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
			// Large DMA = command + payload (write settings)
			tpgz_cmd = cmd;
			tpgz_pending_read = 1;
			dbgprintf("TPGZ: write cmd=0x%02X len=%u\r\n", tpgz_cmd, Length);

			if (tpgz_cmd == TPGZ_CMD_WRITE)
			{
				// Skip the 4-byte command header
				tpgz_last_status = tpgz_write_settings(Data + 4, Length - 4);
			}
		}
	}
	else
	{
		// EXI_READ (DMA read)
		dbgprintf("TPGZ: read cmd=0x%02X len=%u\r\n", tpgz_cmd, Length);

		if (tpgz_cmd == TPGZ_CMD_READ)
		{
			tpgz_read_settings(Data, Length);
		}
		else if (tpgz_cmd == TPGZ_CMD_WRITE || tpgz_cmd == TPGZ_CMD_DELETE)
		{
			// Return operation status to PPC
			memset(Data, 0, Length);
			*(u32*)Data = tpgz_last_status;
			sync_after_write(Data, Length);
		}
		tpgz_pending_read = 0;
	}

	write32(EXI_CMD_0, 0);
	sync_after_write((void*)EXI_BASE, 0x20);
}
