// Copyright (c) 2024 Raspberry Pi (Trading) Ltd.

// Generate DVI output using the command expander and TMDS encoder in HSTX.

// This example requires an external digital video connector connected to
// GPIOs 12 through 19 (the HSTX-capable GPIOs) with appropriate
// current-limiting resistors, e.g. 270 ohms. The pinout used in this example
// matches the Pico DVI Sock board, which can be soldered onto a Pico 2:
// https://github.com/Wren6991/Pico-DVI-Sock

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/sio.h"
#include "pico/multicore.h"
#include "pico/sem.h"
#include "stdio.h"
#include "pico/stdlib.h"
#include "hardware/vreg.h"

// Comment line below to display 640x240 RGB565
#define RBG332    // display 640x480 RGB332
// ----------------------------------------------------------------------------
#ifdef RBG332
#include "mario_640x480_rgb332.h"
#define framebuf mario_640x480_rgb332
#else
#include "mario_640x240_rgb565.h"
#define framebuf mario_640x240_rgb565
#endif

// ----------------------------------------------------------------------------
// DVI constants

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define MODE_H_SYNC_POLARITY 0
#define MODE_H_FRONT_PORCH 16
#define MODE_H_SYNC_WIDTH 96
#define MODE_H_BACK_PORCH 48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_SYNC_POLARITY 0
#define MODE_V_FRONT_PORCH 10
#define MODE_V_SYNC_WIDTH 2
#define MODE_V_BACK_PORCH 33
#define MODE_V_ACTIVE_LINES 480

#define MODE_H_TOTAL_PIXELS (                \
    MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + \
    MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL_LINES (                 \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

#define HSTX_CMD_RAW (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT (0x1u << 12)
#define HSTX_CMD_TMDS (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP (0xfu << 12)

// ----------------------------------------------------------------------------
// HSTX command lists

// Lists are padded with NOPs to be >= HSTX FIFO size, to avoid DMA rapidly
// pingponging and tripping up the IRQs.

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP};

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS};

// ----------------------------------------------------------------------------
// DMA logic

#define DMACH_PING 0
#define DMACH_PONG 1

// First we ping. Then we pong. Then... we ping again.
static bool dma_pong = false;

// A ping and a pong are cued up initially, so the first time we enter this
// handler it is to cue up the second ping after the first ping has completed.
// This is the third scanline overall (-> =2 because zero-based).
static uint v_scanline = 2;

// During the vertical active period, we take two IRQs per scanline: one to
// post the command list, and another to post the pixels.
static bool vactive_cmdlist_posted = false;

#ifndef _IMG_ASSET_SECTION
#define _IMG_ASSET_SECTION ".data"
#endif
char __attribute__((aligned(4), section(_IMG_ASSET_SECTION ".tempbuf"))) tempbuf[640]; 
void __scratch_x("") dma_irq_handler()
{
    // dma_pong indicates the channel that just finished, which is the one
    // we're about to reload.
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    if (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH))
    {
        // printf("Vsync %d\n", v_scanline);
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    }
    else if (v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH)
    {
        // printf("Vsync %d\n", v_scanline);
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    }
    else if (!vactive_cmdlist_posted)
    {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    }
    else
    {
#ifdef RBG332
        ch->read_addr = (uintptr_t)&tempbuf;
        char *ptr = (char *)&framebuf[(v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES)) * MODE_H_ACTIVE_PIXELS];
        for(int i=0; i<640; i++) {
            tempbuf[i] = *ptr++;
        }
        
        //ch->read_addr = (uintptr_t)&framebuf[(v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES)) * MODE_H_ACTIVE_PIXELS];
        // // Duplicate the upper half of the image to the lower half
        // if (v_scanline > 523 - 480 + 240)
        // {
        //     ch->read_addr = (uintptr_t)&framebuf[(v_scanline - 239 - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES)) * MODE_H_ACTIVE_PIXELS];
        // }
        // else
        // {
        //     ch->read_addr = (uintptr_t)&framebuf[(v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES)) * MODE_H_ACTIVE_PIXELS];
        // }
        ch->transfer_count = MODE_H_ACTIVE_PIXELS / sizeof(uint32_t);
#else
        // 640x480 RGB565 is too large to fit into memory. The include file is 640 x 240 pixels.
        // The image is duplicated to the lower half of the screen. 
        if (v_scanline > 523 - 480 + 240)
        {
            ch->read_addr = (uintptr_t)&framebuf[(v_scanline - 239 - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES)) * MODE_H_ACTIVE_PIXELS * 2];
        }
        else
        {
            ch->read_addr = (uintptr_t)&framebuf[(v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES)) * MODE_H_ACTIVE_PIXELS * 2];
        }
        ch->transfer_count = MODE_H_ACTIVE_PIXELS * 2 / sizeof(uint32_t);
#endif
        
        vactive_cmdlist_posted = false;
        // printf("Scanline %d\n", v_scanline);
    }

    if (!vactive_cmdlist_posted)
    {

        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
    }
}

// ----------------------------------------------------------------------------
// Main program

static __force_inline uint16_t colour_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    // printf("RGB565: %02x %02x %02x\n", r, g, b);
    return ((uint16_t)r & 0xf8) >> 3 | ((uint16_t)g & 0xfc) << 3 | ((uint16_t)b & 0xf8) << 8;
}

static __force_inline uint8_t colour_rgb332(uint8_t r, uint8_t g, uint8_t b)
{
    // printf("RGB332: %02x %02x %02x\n", r, g, b);
    return (r & 0xc0) >> 6 | (g & 0xe0) >> 3 | (b & 0xe0) >> 0;
}

void scroll_framebuffer(void);

void core1_main()
{
    printf("DVI output example\n");

#ifdef RBG332
    printf("640x480 RGB332\n");
    // Configure HSTX's TMDS encoder for RGB332
    hstx_ctrl_hw->expand_tmds =
        2 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        2 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        1 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
#else
    // This should be RGB565
    printf("640x240 RGB565\n");
    hstx_ctrl_hw->expand_tmds =
        5 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | // 5 bits for red
        0 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |   // No rotation for red
        6 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | // 6 bits for green
        29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |  // Rotation for green
        5 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | // 5 bits for blue
        26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;   // Rotation for blue
#endif
    // Pixels (TMDS) come in 4 8-bit chunks. Control symbols (RAW) are an
    // entire 32-bit word.
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Serial output config: clock period of 5 cycles, pop from command
    // expander every 5 cycles, shift the output shiftreg by 2 every cycle.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Note we are leaving the HSTX clock at the SDK default of 125 MHz; since
    // we shift out two bits per HSTX clock cycle, this gives us an output of
    // 250 Mbps, which is very close to the bit clock for 480p 60Hz (252 MHz).
    // If we want the exact rate then we'll have to reconfigure PLLs.

    // HSTX outputs 0 through 7 appear on GPIO 12 through 19.
    // Pinout on Pico DVI sock:
    //
    //   GP12 D0+  GP13 D0-
    //   GP14 CK+  GP15 CK-
    //   GP16 D2+  GP17 D2-
    //   GP18 D1+  GP19 D1-

    // Assign clock pair to two neighbouring pins:
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (uint lane = 0; lane < 3; ++lane)
    {
        // For each TMDS lane, assign it to the correct GPIO pair based on the
        // desired pinout:
        // HSTX Output Bits and GPIO Pins
        // The HSTX hardware outputs data on 8 "output bits" (0 through 7), which are mapped to specific GPIO pins. The mapping is fixed as follows:

        // HSTX Output Bit 0 → GPIO12
        // HSTX Output Bit 1 → GPIO13
        // HSTX Output Bit 2 → GPIO14
        // HSTX Output Bit 3 → GPIO15
        // HSTX Output Bit 4 → GPIO16
        // HSTX Output Bit 5 → GPIO17
        // HSTX Output Bit 6 → GPIO18
        // HSTX Output Bit 7 → GPIO19
        // lane_to_output_bit Array
        // The lane_to_output_bit array specifies which HSTX output bits are used for each TMDS lane:

        // Index 0: TMDS lane D0 (data lane 0)
        // Index 1: TMDS lane D1 (data lane 1)
        // Index 2: TMDS lane D2 (data lane 2)

        // Hardcoded mapping for now using Adafruit Metro RP2350
        // The mapping is fixed as follows:
        // D0+ = CPIO18, D0-=GPIO19, D1+=GPIO16, D1-=GPIO17, D2+-GPIO12, D2-=GPIO13 
        // For the array {6, 4, 0}:
        
        // D0 (Index 0) is assigned to HSTX output bit 6 → GPIO18 (D0+) and GPIO19 (D0-).
        // D1 (Index 1) is assigned to HSTX output bit 4 → GPIO16 (D1+) and GPIO17 (D1-).
        // D2 (Index 2) is assigned to HSTX output bit 0 → GPIO12 (D2+) and GPIO13 (D2-).
        // https://learn.adafruit.com/adafruit-metro-rp2350/pinouts#hstx-connector-3193107
        static const int lane_to_output_bit[3] = {6, 4, 0}; // was {0, 6, 4};
        int bit = lane_to_output_bit[lane];
        // Output even bits during first half of each HSTX cycle, and odd bits
        // during second half. The shifter advances by two bits each cycle.
        uint32_t lane_data_sel_bits =
            (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        // The two halves of each pair get identical data, but one pin is inverted.
        hstx_ctrl_hw->bit[bit] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (int i = 12; i <= 19; ++i)
    {
        gpio_set_function(i, 0); // HSTX
    }

    // Both channels are set up identically, to transfer a whole scanline and
    // then chain to the opposite channel. Each time a channel finishes, we
    // reconfigure the one that just finished, meanwhile the opposite channel
    // is already making progress.
    dma_channel_config c;
    c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PING,
        &c,
        &hstx_fifo_hw->fifo,
        vblank_line_vsync_off,
        count_of(vblank_line_vsync_off),
        false);
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PONG,
        &c,
        &hstx_fifo_hw->fifo,
        vblank_line_vsync_off,
        count_of(vblank_line_vsync_off),
        false);

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(DMACH_PING);

    while (1)
        __wfi();
}

int main(void)
{
    stdio_init_all();
    sleep_ms(1000);
    printf("DVI output example on Core1\n");
    int teller = 0;
    multicore_launch_core1(core1_main);
    while (1)
    {
        sleep_ms(1000);
        printf("Running random on core 0: %d\n", teller++);
    }
}
