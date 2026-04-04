/*

  u8x8_d_ssd1309.c

  Universal 8bit Graphics Library (https://github.com/olikraus/u8g2/)

  Copyright (c) 2017, olikraus@gmail.com
  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification,
  are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice, this list
    of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice, this
    list of conditions and the following disclaimer in the documentation and/or other
    materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
  CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
  INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
  NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "u8x8.h"

static const uint8_t u8x8_d_ssd1309_powersave0_seq[] = {
    U8X8_START_TRANSFER(), /* enable chip, delay is part of the transfer start */
    U8X8_C(0x0af), /* display on */
    U8X8_END_TRANSFER(), /* disable chip */
    U8X8_END() /* end of sequence */
};

static const uint8_t u8x8_d_ssd1309_powersave1_seq[] = {
    U8X8_START_TRANSFER(), /* enable chip, delay is part of the transfer start */
    U8X8_C(0x0ae), /* display off */
    U8X8_END_TRANSFER(), /* disable chip */
    U8X8_END() /* end of sequence */
};

static const uint8_t u8x8_d_ssd1309_flip0_seq[] = {
    U8X8_START_TRANSFER(), /* enable chip, delay is part of the transfer start */
    U8X8_C(0x0a1), /* segment remap a0/a1*/
    U8X8_C(0x0c8), /* c0: scan dir normal, c8: reverse */
    U8X8_END_TRANSFER(), /* disable chip */
    U8X8_END() /* end of sequence */
};

static const uint8_t u8x8_d_ssd1309_flip1_seq[] = {
    U8X8_START_TRANSFER(), /* enable chip, delay is part of the transfer start */
    U8X8_C(0x0a0), /* segment remap a0/a1*/
    U8X8_C(0x0c0), /* c0: scan dir normal, c8: reverse */
    U8X8_END_TRANSFER(), /* disable chip */
    U8X8_END() /* end of sequence */
};

static uint8_t u8x8_d_ssd1309_generic(u8x8_t* u8x8, uint8_t msg, uint8_t arg_int, void* arg_ptr) {
    uint8_t x;
    uint8_t c;
    uint8_t* ptr;

    switch(msg) {
    case U8X8_MSG_DISPLAY_DRAW_TILE:
        u8x8_cad_StartTransfer(u8x8);
        x = ((u8x8_tile_t*)arg_ptr)->x_pos;
        x *= 8;
        x += u8x8->x_offset;

        u8x8_cad_SendCmd(u8x8, 0x010 | (x >> 4));
        u8x8_cad_SendCmd(u8x8, 0x000 | ((x & 15)));
        u8x8_cad_SendCmd(u8x8, 0x0b0 | (((u8x8_tile_t*)arg_ptr)->y_pos));

        do {
            c = ((u8x8_tile_t*)arg_ptr)->cnt;
            ptr = ((u8x8_tile_t*)arg_ptr)->tile_ptr;
            u8x8_cad_SendData(u8x8, c * 8, ptr);
            arg_int--;
        } while(arg_int > 0);

        u8x8_cad_EndTransfer(u8x8);
        break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        if(arg_int == 0) {
            u8x8_cad_SendSequence(u8x8, u8x8_d_ssd1309_powersave0_seq);
        } else {
            u8x8_cad_SendSequence(u8x8, u8x8_d_ssd1309_powersave1_seq);
        }
        break;
#ifdef U8X8_WITH_SET_CONTRAST
    case U8X8_MSG_DISPLAY_SET_CONTRAST:
        u8x8_cad_StartTransfer(u8x8);
        u8x8_cad_SendCmd(u8x8, 0x081);
        u8x8_cad_SendArg(u8x8, arg_int);
        u8x8_cad_EndTransfer(u8x8);
        break;
#endif
    default:
        return 0;
    }
    return 1;
}

static const u8x8_display_info_t u8x8_ssd1309_128x64_noname2_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .post_chip_enable_wait_ns = 20,
    .pre_chip_disable_wait_ns = 10,
    .reset_pulse_width_ms = 100,
    .post_reset_wait_ms = 100,
    .sda_setup_time_ns = 50,
    .sck_pulse_width_ns = 50,
    .sck_clock_hz = 4000000UL,
    .spi_mode = 0,
    .i2c_bus_clock_100kHz = 4,
    .data_setup_time_ns = 40,
    .write_pulse_width_ns = 150,
    .tile_width = 16,
    .tile_height = 8,
    .default_x_offset = 2,
    .flipmode_x_offset = 2,
    .pixel_width = 128,
    .pixel_height = 64,
};

static const uint8_t u8x8_d_ssd1309_128x64_noname_init_seq[] = {
    U8X8_START_TRANSFER(),
    U8X8_C(0x0ae),
    U8X8_CA(0x0d5, 0x0a0),
    U8X8_C(0x040),
    U8X8_CA(0x020, 0x002),
    U8X8_C(0x0a1),
    U8X8_C(0x0c8),
    U8X8_CA(0x0da, 0x012),
    U8X8_CA(0x081, 0x06f),
    U8X8_CA(0x0d9, 0x0d3),
    U8X8_CA(0x0db, 0x020),
    U8X8_C(0x02e),
    U8X8_C(0x0a4),
    U8X8_C(0x0a6),
    U8X8_END_TRANSFER(),
    U8X8_END()
};

uint8_t u8x8_d_ssd1309_128x64_noname2(u8x8_t* u8x8, uint8_t msg, uint8_t arg_int, void* arg_ptr) {
    if(u8x8_d_ssd1309_generic(u8x8, msg, arg_int, arg_ptr) != 0) return 1;

    switch(msg) {
    case U8X8_MSG_DISPLAY_SET_FLIP_MODE:
        if(arg_int == 0) {
            u8x8_cad_SendSequence(u8x8, u8x8_d_ssd1309_flip0_seq);
            u8x8->x_offset = u8x8->display_info->default_x_offset;
        } else {
            u8x8_cad_SendSequence(u8x8, u8x8_d_ssd1309_flip1_seq);
            u8x8->x_offset = u8x8->display_info->flipmode_x_offset;
        }
        break;
    case U8X8_MSG_DISPLAY_INIT:
        u8x8_d_helper_display_init(u8x8);
        u8x8_cad_SendSequence(u8x8, u8x8_d_ssd1309_128x64_noname_init_seq);
        break;
    case U8X8_MSG_DISPLAY_SETUP_MEMORY:
        u8x8_d_helper_display_setup_memory(u8x8, &u8x8_ssd1309_128x64_noname2_display_info);
        break;
    default:
        return 0;
    }
    return 1;
}

static const u8x8_display_info_t u8x8_ssd1309_128x64_noname0_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .post_chip_enable_wait_ns = 20,
    .pre_chip_disable_wait_ns = 10,
    .reset_pulse_width_ms = 100,
    .post_reset_wait_ms = 100,
    .sda_setup_time_ns = 50,
    .sck_pulse_width_ns = 50,
    .sck_clock_hz = 4000000UL,
    .spi_mode = 0,
    .i2c_bus_clock_100kHz = 4,
    .data_setup_time_ns = 40,
    .write_pulse_width_ns = 150,
    .tile_width = 16,
    .tile_height = 8,
    .default_x_offset = 0,
    .flipmode_x_offset = 0,
    .pixel_width = 128,
    .pixel_height = 64,
};

uint8_t u8x8_d_ssd1309_128x64_noname0(u8x8_t* u8x8, uint8_t msg, uint8_t arg_int, void* arg_ptr) {
    if(u8x8_d_ssd1309_generic(u8x8, msg, arg_int, arg_ptr) != 0) return 1;

    switch(msg) {
    case U8X8_MSG_DISPLAY_SET_FLIP_MODE:
        if(arg_int == 0) {
            u8x8_cad_SendSequence(u8x8, u8x8_d_ssd1309_flip0_seq);
            u8x8->x_offset = u8x8->display_info->default_x_offset;
        } else {
            u8x8_cad_SendSequence(u8x8, u8x8_d_ssd1309_flip1_seq);
            u8x8->x_offset = u8x8->display_info->flipmode_x_offset;
        }
        break;
    case U8X8_MSG_DISPLAY_INIT:
        u8x8_d_helper_display_init(u8x8);
        u8x8_cad_SendSequence(u8x8, u8x8_d_ssd1309_128x64_noname_init_seq);
        break;
    case U8X8_MSG_DISPLAY_SETUP_MEMORY:
        u8x8_d_helper_display_setup_memory(u8x8, &u8x8_ssd1309_128x64_noname0_display_info);
        break;
    default:
        return 0;
    }
    return 1;
}
