#ifndef __DDP_DITHER_H__
#define __DDP_DITHER_H__

#include "ddp_info.h"

typedef enum {
	DISP_DITHER0,
	DISP_DITHER1
} disp_dither_id_t;

void dither_test(const char *cmd, char *debug_output);
extern DDP_MODULE_DRIVER ddp_driver_dither;
#endif
