/*
 * Copyright (C) 2014-2015  Zubax Robotics  <info@zubax.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Pavel Kirienko <pavel.kirienko@zubax.com>
 */

#include "board.hpp"
#include <cstring>
#include <ch.hpp>
#include <hal.h>
#include <unistd.h>

/**
 * GPIO config for ChibiOS PAL driver
 */
const PALConfig pal_default_config =
{
    { VAL_GPIOAODR, VAL_GPIOACRL, VAL_GPIOACRH },
    { VAL_GPIOBODR, VAL_GPIOBCRL, VAL_GPIOBCRH },
    { VAL_GPIOCODR, VAL_GPIOCCRL, VAL_GPIOCCRH },
    { VAL_GPIODODR, VAL_GPIODCRL, VAL_GPIODCRH },
    { VAL_GPIOEODR, VAL_GPIOECRL, VAL_GPIOECRH }
};

/// Provided by linker
const extern std::uint8_t DeviceSignatureStorage[];

namespace board
{

os::watchdog::Timer init(unsigned wdt_timeout_ms)
{
    /*
     * OS
     */
    halInit();
    chSysInit();
    sdStart(&STDOUT_SD, NULL);

    /*
     * Watchdog
     */
    os::watchdog::init();
    os::watchdog::Timer wdt;
    wdt.startMSec(wdt_timeout_ms);

    /*
     * Version check
     */
    const auto hw_ver = detectHardwareVersion();

    if (hw_ver.major != 2)
    {
        chibios_rt::System::halt("BAD HW");
    }

    if (hw_ver.minor != 2)
    {
        chibios_rt::System::halt("BAD HW");
    }

    /*
     * Prompt
     */
    os::lowsyslog(PRODUCT_NAME_STRING " Bootloader %d.%d %d.%d.%08x / %s\n",
                  hw_ver.major, hw_ver.minor,
                  BL_VERSION_MAJOR, BL_VERSION_MINOR, GIT_HASH,
                  watchdogTriggeredLastReset() ? "WDTRESET" : "OK");

    return wdt;
}

void die(int error)
{
    os::lowsyslog("ERR %i\n", error);
    while (1)
    {
        setStatusLed(false);
        ::usleep(25000);
        setStatusLed(true);
        ::usleep(25000);
    }
}

void setCANLed(unsigned iface_index, bool state)
{
    switch (iface_index)
    {
    case 0:
    {
        palWritePad(GPIO_PORT_LED_CAN1, GPIO_PIN_LED_CAN1, state);
        break;
    }
    case 1:
    {
        palWritePad(GPIO_PORT_LED_CAN2, GPIO_PIN_LED_CAN2, state);
        break;
    }
    default:
    {
        break;
    }
    }
}

void setStatusLed(bool state)
{
    palWritePad(GPIO_PORT_LED_STATUS, GPIO_PIN_LED_STATUS, state);
}

void restart()
{
    NVIC_SystemReset();

    for (;;) { }    // No return
}

void bootApplication()
{
    // We cordially extend our thanks to David Sidrane and Ben Dyer, whose ideas have somewhat inspired this thing.
    chSysLock();

    // Deinit all peripherals that may have been used
    RCC->APB1RSTR |= RCC_APB1RSTR_CAN1RST | RCC_APB1RSTR_CAN2RST | RCC_APB1RSTR_USART3RST;

    // Kill the sys tick
    SysTick->CTRL = 0;

    // Update the vector table location
    __asm volatile("dsb");
    __asm volatile("isb");
    SCB->VTOR = APPLICATION_OFFSET;
    __asm volatile("dsb");

    // Let's roll!
    const unsigned stacktop = *reinterpret_cast<unsigned*>(APPLICATION_OFFSET);
    const unsigned entrypoint = *reinterpret_cast<unsigned*>(APPLICATION_OFFSET + 4);
    asm volatile("msr msp, %[stacktop]          \n"
                 "bx       %[entrypoint]        \n"
                 :: [stacktop] "r"(stacktop), [entrypoint] "r"(entrypoint):);

    for (;;) { }        // Noreturn
}

UniqueID readUniqueID()
{
    UniqueID out_bytes;
    std::memcpy(out_bytes.data(), reinterpret_cast<const void*>(0x1FFFF7E8), std::tuple_size<UniqueID>::value);
    return out_bytes;
}

bool tryReadDeviceSignature(DeviceSignature& out_sign)
{
    std::memcpy(out_sign.data(), &DeviceSignatureStorage[0], std::tuple_size<DeviceSignature>::value);

    bool valid = false;
    for (auto x : out_sign)
    {
        if (x != 0xFF && x != 0x00)          // All 0xFF/0x00 is not a valid signature, it's empty storage
        {
            valid = true;
            break;
        }
    }

    return valid;
}

HardwareVersion detectHardwareVersion()
{
    auto v = HardwareVersion();

    v.major = HW_VERSION_MAJOR;

    if (palReadPad(GPIO_PORT_HWID, GPIO_PIN_HWID_BIT0))
    {
        v.minor += 1;
    }

    if (!palReadPad(GPIO_PORT_HWID, GPIO_PIN_HWID_BIT1_INVERSE))
    {
        v.minor += 2;
    }

    if (palReadPad(GPIO_PORT_HWID, GPIO_PIN_HWID_BIT2))
    {
        v.minor += 4;
    }

    return v;
}

} // namespace board


namespace os
{

void applicationHaltHook()
{
    // Panic indication
    board::setStatusLed(true);
    board::setCANLed(0, true);
    board::setCANLed(1, true);
}

}

/*
 * Early init from ChibiOS
 */
extern "C"
{

void __early_init(void)
{
    stm32_clock_init();
}

void boardInit(void)
{
    uint32_t mapr = AFIO->MAPR;
    mapr &= ~AFIO_MAPR_SWJ_CFG; // these bits are write-only

    // Enable SWJ only, JTAG is not needed at all:
    mapr |= AFIO_MAPR_SWJ_CFG_JTAGDISABLE;

    AFIO->MAPR = mapr | AFIO_MAPR_CAN_REMAP_REMAP2;

    /*
     * Enabling the CAN controllers, then configuring GPIO functions for CAN_TX.
     * Order matters, otherwise the CAN_TX pins will twitch, disturbing the CAN bus.
     * This is why we can't perform this initialization using ChibiOS GPIO configuration.
     *
     * NOTE: Check this - the problem may only appear when CAN pin remapping is used,
     *       because ChibiOS initializes AFIO after GPIO.
     */
    RCC->APB1ENR |= RCC_APB1ENR_CAN1EN;
    palSetPadMode(GPIOB, 9, PAL_MODE_STM32_ALTERNATE_PUSHPULL);
    // Enabling CAN2 too
    RCC->APB1ENR |= RCC_APB1ENR_CAN2EN;
    palSetPadMode(GPIOB, 13, PAL_MODE_STM32_ALTERNATE_PUSHPULL);
}

}