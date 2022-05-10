// Canboot main event loop
//
// Copyright (C) 2021 Eric Callahan <arksine.code@gmail.com>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <string.h>         // memmove
#include "autoconf.h"       // CONFIG_*
#include "board/misc.h"     // delay_ms
#include "board/canbus.h"   // canbus_send
#include "board/flash.h"    // write_page
#include "board/gpio.h"     // gpio_in_setup
#include "canboot_main.h"   // canboot_main
#include "command.h"        // command_respond_ack
#include "ctr.h"            // DECL_CTR
#include "byteorder.h"      // cpu_to_le32
#include "sched.h"          // sched_run_init

#define REQUEST_SIG    0x5984E3FA6CA1589B // Random request sig

static uint8_t page_buffer[CONFIG_MAX_FLASH_PAGE_SIZE];
// Page Tracking
static uint32_t last_page_address = 0;
static uint8_t page_pending = 0;
static uint8_t is_in_transfer;
static uint8_t complete = 0;

int
flashcmd_is_in_transfer(void)
{
    return is_in_transfer;
}

static void
write_page(uint32_t page_address)
{
    flash_write_page(page_address, (uint16_t*)page_buffer);
    memset(page_buffer, 0xFF, sizeof(page_buffer));
    last_page_address = page_address;
    page_pending = 0;
}

void
command_read_block(uint32_t *data)
{
    is_in_transfer = 1;
    uint32_t block_address = le32_to_cpu(data[1]);
    uint32_t out[CONFIG_BLOCK_SIZE / 4 + 2 + 2];
    out[2] = cpu_to_le32(block_address);
    flash_read_block(block_address, &out[3]);
    command_respond_ack(CMD_REQ_BLOCK, out, ARRAY_SIZE(out));
}

void
command_write_block(uint32_t *data)
{
    is_in_transfer = 1;
    if (command_get_arg_count(data) != (CONFIG_BLOCK_SIZE / 4) + 1) {
        command_respond_command_error();
        return;
    }
    uint32_t block_address = le32_to_cpu(data[1]);
    if (block_address < CONFIG_APPLICATION_START) {
        command_respond_command_error();
        return;
    }
    uint32_t flash_page_size = flash_get_page_size();
    uint32_t page_pos = block_address % flash_page_size;
    memcpy(&page_buffer[page_pos], (uint8_t *)&data[2], CONFIG_BLOCK_SIZE);
    page_pending = 1;
    if (page_pos + CONFIG_BLOCK_SIZE == flash_page_size)
        write_page(block_address - page_pos);
    uint32_t out[4];
    out[2] = cpu_to_le32(block_address);
    command_respond_ack(CMD_RX_BLOCK, out, ARRAY_SIZE(out));
}

void
command_eof(uint32_t *data)
{
    is_in_transfer = 0;
    uint32_t flash_page_size = flash_get_page_size();
    if (page_pending) {
        write_page(last_page_address + flash_page_size);
    }
    flash_complete();
    uint32_t out[4];
    out[2] = cpu_to_le32(
        ((last_page_address - CONFIG_APPLICATION_START)
        / flash_page_size) + 1);
    command_respond_ack(CMD_RX_EOF, out, ARRAY_SIZE(out));
}

void
command_complete(uint32_t *data)
{
    uint32_t out[3];
    command_respond_ack(CMD_COMPLETE, out, ARRAY_SIZE(out));
    complete = 1;
}

void
command_connect(uint32_t *data)
{
    uint32_t mcuwords = DIV_ROUND_UP(strlen(CONFIG_MCU), 4);
    uint32_t out[6 + mcuwords];
    memset(out, 0, (6 + mcuwords) * 4);
    out[2] = cpu_to_le32(PROTO_VERSION);
    out[3] = cpu_to_le32(CONFIG_APPLICATION_START);
    out[4] = cpu_to_le32(CONFIG_BLOCK_SIZE);
    memcpy(&out[5], CONFIG_MCU, strlen(CONFIG_MCU));
    command_respond_ack(CMD_CONNECT, out, ARRAY_SIZE(out));
}

static inline uint8_t
check_application_code(void)
{
    // Read the first block of memory, if it
    // is all 0xFF then no application has been flashed
    flash_read_block(CONFIG_APPLICATION_START, (uint32_t*)page_buffer);
    for (uint8_t i = 0; i < CONFIG_BLOCK_SIZE; i++) {
        if (page_buffer[i] != 0xFF)
            return 1;
    }
    return 0;
}

// Generated by buildcommands.py
DECL_CTR("DECL_BUTTON " __stringify(CONFIG_BUTTON_PIN));
extern int32_t button_gpio, button_high, button_pullup;

// Check for a bootloader request via double tap of reset button
static int
check_button_pressed(void)
{
    if (!CONFIG_ENABLE_BUTTON)
        return 0;
    struct gpio_in button = gpio_in_setup(button_gpio, button_pullup);
    udelay(10);
    return gpio_in_read(button) == button_high;
}

// Check for a bootloader request via double tap of reset button
static void
check_double_reset(void)
{
    if (!CONFIG_ENABLE_DOUBLE_RESET)
        return;
    // set request signature and delay for two seconds.  This enters the bootloader if
    // the reset button is double clicked
    set_bootup_code(REQUEST_SIG);
    udelay(500000);
    set_bootup_code(0);
    // No reset, read the key back out to clear it
}


/****************************************************************
 * Startup
 ****************************************************************/
static void
enter_bootloader(void)
{
    sched_run_init();

    for (;;) {
        sched_run_tasks();
        if (complete && canbus_tx_clear())
            // wait until we are complete and the ack has returned
            break;
    }

    // Flash Complete, system reset
    udelay(100000);
    canbus_reboot();
}

// Main loop of program
void
canboot_main(void)
{
    // Enter the bootloader in the following conditions:
    // - The request signature is set in memory (request from app)
    // - No application code is present
    uint64_t bootup_code = get_bootup_code();
    if (bootup_code == REQUEST_SIG || !check_application_code()
        || check_button_pressed()) {
        set_bootup_code(0);
        enter_bootloader();
    }
    check_double_reset();

    // jump to app
    jump_to_application();
}
