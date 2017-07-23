/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program
 * If not, see <http://www.gnu.org/licenses/>.
 */
/*******************************************************************************
 *
 * Filename:
 * ---------
 *  mt_sco_digital_type.h
 *
 * Project:
 * --------
 *   MT6583  Audio Driver Kernel Function
 *
 * Description:
 * ------------
 *   Audio register
 *
 * Author:
 * -------
 * Chipeng Chang
 *
 *------------------------------------------------------------------------------
 *
 *
 *******************************************************************************/

#ifndef _AUDIO_HDMI_TYPE_H
#define _AUDIO_HDMI_TYPE_H

#include <linux/list.h>

/*****************************************************************************
 *                ENUM DEFINITION
 ******************************************************************************/
#define _ANX7805_SLIMPORT_CHANNEL 2
#define HDMI_USE_CHANNELS_MIN     1
#define HDMI_USE_CHANNELS_MAX     8


void mtk_Hdmi_Configuration_Init(void *hdmi_stream_format);

void mtk_Hdmi_Clock_Set(void *hdmi_stream_format);

void mtk_Hdmi_Configuration_Set(void *hdmi_stream_format, struct snd_pcm_substream *substream, int displaytype);

void mtk_Hdmi_Set_Interconnection(void *hdmi_stream_format);

void mtk_Hdmi_Set_Sdata(void *hdmi_stream_format);

void SetHDMIClockInverse(void);

void SetHDMIDebugEnable(bool enable);

void SetHDMIDumpReg(void);

bool SetHDMIMCLK(void);

bool SetHDMIBCLK(void);

uint32 GetHDMIApLLSource(void);

bool SetHDMIApLL(uint32 ApllSource);

bool SetHDMIdatalength(uint32 length);

bool SetHDMIsamplerate(uint32 samplerate);

bool SetHDMIChannels(uint32 Channels);

bool SetHDMIEnable(bool bEnable);

bool SetHDMIConnection(uint32 ConnectionState, uint32 Input, uint32 Output);

bool SetTDMLrckWidth(uint32 cycles);

bool SetTDMbckcycle(uint32 cycles);

bool SetTDMChannelsSdata(uint32 channels);

bool SetTDMDatalength(uint32 length);

bool SetTDMI2Smode(uint32 mode);

bool SetTDMLrckInverse(bool enable);

bool SetTDMBckInverse(bool enable);

bool SetTDMEnable(bool enable);

#endif
