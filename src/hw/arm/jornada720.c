/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * HP Jornada 720 (SA-1110-based Handheld PC) machine model.
 *
 * NOT intended for upstream submission to QEMU — local experiment only,
 * per qemu-src/AGENTS.md's AI-generated content policy. See
 * docs/plan.md / docs/research.md in the qemu-jornada720 project repo.
 *
 * Memory map (from Jornada 720 hardware docs):
 *   0x00000000  system Flash/ROM, 32 MiB    (SA_CS0)
 *   0x1a000000  debug board CL-CD1284 UART  -- unimplemented-device stub
 *   0x20000000  PCMCIA socket 0 I/O         -- NE2000 network card
 *   0x28000000  PCMCIA socket 0 attribute   -- the card's CIS and COR
 *   0x30000000  PCMCIA socket 1 (CF slot)   -- CompactFlash storage card
 *                                           (I/O, attribute at 0x38000000,
 *                                           common memory at 0x3c000000)
 *   0x40000000  SA-1111 companion chip      (SA_CS4) -- interrupt controller
 *                                           and PCMCIA interface; the rest
 *                                           is an unimplemented-device stub
 *   0x48000000  Epson display controller    -- registers + BitBLT engine
 *   0x48200000  Epson frame buffer, 512 KiB -- plain RAM + 640x240 RGB565 console
 *   0x80000000  SA-1110 USB device ctrl     -- read-back register stub
 *   0x90020000  SA-1110 power manager       -- POSR stub
 *   0xC0000000  system SDRAM, 32 MiB        (SA_SDCS0)
 *   on-chip SSP keyboard/touchscreen MCU    -- j720-mcu (GPIO0/9 IRQs)
 *
 * The stubs exist so boot-code probes of this not-yet-emulated hardware
 * are visible via -d unimp instead of silently spinning forever; see
 * jornada720_init().
 *
 * Boots the Windows CE (H/PC 2000) ROM to a usable desktop with display,
 * keyboard and touchscreen; needs -icount (see docs/research.md).
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "hw/core/sysbus.h"
#include "hw/core/boards.h"
#include "strongarm.h"
#include "hw/block/flash.h"
#include "hw/misc/unimp.h"
#include "hw/ssi/ssi.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/input.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/net/ne2000.h"
#include "net/net.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "migration/vmstate.h"
#include "standard-headers/linux/input-event-codes.h"
#include "system/address-spaces.h"
#include "system/runstate.h"
#include "system/reset.h"
#include "qom/object.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qemu/log.h"

#define J720_SA1111_BASE        0x40000000
#define J720_SA1111_SIZE        (16 * MiB)
#define J720_DEBUGBOARD_BASE    0x1a000000
#define J720_DEBUGBOARD_SIZE    (1 * MiB)
#define J720_EPSON_REGS_BASE    0x48000000
#define J720_EPSON_REGS_SIZE    (2 * MiB)
#define J720_EPSON_FB_BASE      0x48200000
#define J720_EPSON_FB_SIZE      (512 * KiB)
#define J720_LCD_WIDTH          640
#define J720_LCD_HEIGHT         240

#define J720_RAM_SIZE          (32 * MiB)
#define J720_FLASH_SIZE        (32 * MiB)
#define J720_FLASH_SECTOR_SIZE (64 * KiB)

/*
 * SA-1111 SSP block base is 0x0800 (confirmed against the real Linux
 * driver's device table, arch/arm/common/sa1111.c -- NOT the 0x1600/0x1800
 * values naively guessable from the simplified asm/hardware/sa1111.h
 * register-name header, which turned out to be for INTC/PCMCIA instead).
 * Boot code spins forever reading offset 0x10 within this block (SA-1111
 * base + 0x810), almost certainly the SSP Status Register polling a
 * "FIFO not full" / "not busy" flag. Not implementing real SSP semantics,
 * just returning all-1s so whatever bit is polled reads as set. See
 * docs/research.md for how this was narrowed down.
 */
#define J720_SA1111_SSP_STUB_BASE (J720_SA1111_BASE + 0x800)
#define J720_SA1111_SSP_STUB_SIZE 0x1000

static uint64_t j720_ssp_stub_read(void *opaque, hwaddr addr, unsigned size)
{
    /*
     * Guessing the SA-1111 SSP status register shares its bit layout with
     * the on-chip SA-1110 SSP (strongarm_ssp in this same tree): TNF=bit2,
     * RNE=bit3, TFS=bit5, RFS=bit6, ROR=bit7 (left clear). An all-1s stub
     * unblocked the first poll but then hung a second one -- consistent
     * with the boot code also waiting for a busy/overrun-style bit to
     * read 0, which all-1s can never satisfy. Experimental, unconfirmed
     * against the real SA-1111 datasheet chapter 8 (bitsavers copy is a
     * dead link; see docs/research.md).
     */
    return (1 << 2) | (1 << 3) | (1 << 5) | (1 << 6);
}

static void j720_ssp_stub_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
}

static const MemoryRegionOps j720_ssp_stub_ops = {
    .read = j720_ssp_stub_read,
    .write = j720_ssp_stub_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/*
 * SA-1110 Power Manager (0x90020000) and Reset Controller (0x90030000),
 * both missing from hw/arm/strongarm.c. Register map from the SA-1110
 * Developer's Manual: PMCR, PSSR, PSPR, PWER, PCFR, PPCR, PGSR, POSR;
 * RSRR, RCSR.
 *
 * POSR bit0 (oscillator stable) must read 1 or the boot code waits
 * forever. PSPR is a scratch register kept through sleep; CE uses it.
 *
 * Sleep (CE's Suspend): writing PMCR.SF stops the machine (QEMU's
 * suspended run state) until a key press or a tap wakes it, like the
 * power button. Waking resets only the CPU, with RCSR.SMR and PSSR's
 * sleep/hold bits set, and keeps RAM, PSPR and the peripherals: the ROM
 * sees a sleep reset and resumes CE from the state it saved in RAM.
 * GPIO wake-up enables (PWER) are stored but not honoured.
 *
 * With J720_SUSPEND_QUITS=1 in the environment (run.sh's default), going
 * to sleep also asks QEMU to shut down, so run.sh saves the sleeping
 * machine and quits; it wakes when that state is loaded again.
 */
#define J720_SA1110_PM_BASE     0x90020000
#define J720_SA1110_PM_SIZE     (4 * KiB)
#define J720_SA1110_RSTC_BASE   0x90030000

#define SA_PMCR         0x00
#define SA_PSSR         0x04
#define SA_PSPR         0x08
#define SA_PWER         0x0c
#define SA_PCFR         0x10
#define SA_PPCR         0x14
#define SA_PGSR         0x18
#define SA_POSR         0x1c
#define PMCR_SF         (1 << 0)
#define PSSR_SSS        (1 << 0)    /* software sleep */
#define PSSR_DH         (1 << 3)    /* DRAM control held */
#define PSSR_PH         (1 << 4)    /* peripheral control held */
#define SA_RSRR         0x00
#define SA_RCSR         0x04
#define RSRR_SWR        (1 << 0)
#define RCSR_HWR        (1 << 0)
#define RCSR_SWR        (1 << 1)
#define RCSR_SMR        (1 << 3)    /* sleep mode reset */

typedef struct J720Power {
    uint32_t pmcr, pssr, pspr, pwer, pcfr, ppcr, pgsr;
    uint32_t rcsr;
    bool sleeping;
    bool soft_reset;            /* RSRR.SWR written: next reset reports SWR */
} J720Power;

static uint64_t j720_pm_read(void *opaque, hwaddr addr, unsigned size)
{
    J720Power *p = opaque;

    switch (addr) {
    case SA_PMCR: return p->pmcr;
    case SA_PSSR: return p->pssr;
    case SA_PSPR: return p->pspr;
    case SA_PWER: return p->pwer;
    case SA_PCFR: return p->pcfr;
    case SA_PPCR: return p->ppcr;
    case SA_PGSR: return p->pgsr;
    case SA_POSR: return 1;         /* OOK: oscillator stable */
    default:
        qemu_log_mask(LOG_UNIMP, "j720.sa1110-pm: read 0x%02" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void j720_pm_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    J720Power *p = opaque;

    switch (addr) {
    case SA_PMCR:
        p->pmcr = value;
        if (value & PMCR_SF) {
            const char *quit = getenv("J720_SUSPEND_QUITS");

            p->sleeping = true;
            qemu_system_suspend_request();
            if (quit && !strcmp(quit, "1")) {
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            }
        }
        break;
    case SA_PSSR: p->pssr &= ~value; break;     /* write 1 to clear */
    case SA_PSPR: p->pspr = value; break;
    case SA_PWER: p->pwer = value; break;
    case SA_PCFR: p->pcfr = value; break;
    case SA_PPCR: p->ppcr = value; break;
    case SA_PGSR: p->pgsr = value; break;
    default:
        qemu_log_mask(LOG_UNIMP, "j720.sa1110-pm: write 0x%02" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n", addr, value);
        break;
    }
}

static const MemoryRegionOps j720_pm_ops = {
    .read = j720_pm_read,
    .write = j720_pm_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static uint64_t j720_rstc_read(void *opaque, hwaddr addr, unsigned size)
{
    J720Power *p = opaque;

    return addr == SA_RCSR ? p->rcsr : 0;
}

static void j720_rstc_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    J720Power *p = opaque;

    if (addr == SA_RCSR) {
        p->rcsr &= ~value;                      /* write 1 to clear */
    } else if (addr == SA_RSRR && (value & RSRR_SWR)) {
        p->soft_reset = true;
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static const MemoryRegionOps j720_rstc_ops = {
    .read = j720_rstc_read,
    .write = j720_rstc_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* Wake the machine if it sleeps; true if it did (the event is used up) */
static bool j720_wake_on_input(bool press)
{
    if (!runstate_check(RUN_STATE_SUSPENDED)) {
        return false;
    }
    if (press) {
        qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER, NULL);
    }
    return true;
}

/*
 * Keyboard/touchscreen/power micro-controller (MCU) on the SA-1110's
 * on-chip SSP. Protocol from Linux (arch/arm/mach-sa1100/jornada720_ssp.c,
 * drivers/input/{keyboard,touchscreen}/jornada720_*.c), confirmed by
 * logging what the CE ROM sends: bytes go over the wire bit-reversed;
 * the MCU answers a command byte with TXDUMMY (0x11) in the same
 * transfer and then hands out data bytes, one per following transfer.
 * GPIO0 falls when key codes are waiting, GPIO9 is low while the pen is
 * down and pulses high once per new sample (Linux's driver triggers on
 * the rising edge only and treats "line still high" as pen up; CE
 * switches GPIO9 to rising edge after the first touch), GPIO10 low = MCU
 * ready (always, here).
 *
 * GETBATTERYDATA answers 3 bytes: main and backup battery 10-bit ADC
 * readings, low bytes, then a byte with main bits 9:8 in bits 1:0 and
 * backup bits 9:8 in bits 3:2 (as battdrv.dll unpacks them).
 * battdrv.dll: main < 512 is 0%, >= 665 is 100% (~12.3 mV per count, a
 * 7.4 V pack); backup > 899 is "high", > 884 "low", > 527 "critical"
 * (explorer.exe then nags "Backup Battery Very Low"), below that "no
 * battery". Report both as full.
 */
#define J720_MCU_TXDUMMY          0x11
#define J720_MCU_GETBATTERYDATA   0xc0
#define J720_MCU_GETSCANKEYCODE   0x90
#define J720_MCU_GETTOUCHSAMPLES  0xa0
#define J720_MCU_GETCONTRAST      0xd0
#define J720_MCU_SETCONTRAST      0xd1
#define J720_MCU_GETBRIGHTNESS    0xd2
#define J720_MCU_SETBRIGHTNESS    0xd3

#define J720_GPIO_KBD_IRQ   0
#define J720_GPIO_TS_IRQ    9

#define J720_MCU_KEYQ       16
/* GETSCANKEYCODE sends a count byte, then the codes, all through out[16] */
#define J720_MCU_KEYQ_USE   (J720_MCU_KEYQ - 1)
#define J720_MCU_PEND       64
#define J720_MCU_PEND_RAW   0x80000000u
#define J720_MCU_PEND_PRESS 0x40000000u     /* make + break, never split */
/* re-signal GPIO0 if CE has not read waiting codes after this long */
#define J720_MCU_KBD_WATCHDOG_MS 50
#define J720_MCU_KBD_DELAY_MS 5
#define J720_MCU_BATT_MAIN   0x2a0
#define J720_MCU_BATT_BACKUP 0x3c0
#define J720_MCU_TS_PERIOD_MS 10
/*
 * Under -icount a quick finger tap can be over before CE has read a
 * single sample; keep the pen down until it has read this many.
 */
#define J720_MCU_TS_MIN_SAMPLES 3
/* ...but never hold a released pen down longer than this many periods */
#define J720_MCU_TS_MAX_HOLD    20

static const unsigned short j720_keymap[128] = {					/* ROW */
	0, KEY_ESC, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7,		/* #1  */
	KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE,	/*  -> */
	0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,		/* #2  */
	KEY_0, KEY_MINUS, KEY_EQUAL,0, 0, 0,						/*  -> */
	0, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O,		/* #3  */
	KEY_P, KEY_BACKSLASH, KEY_BACKSPACE, 0, 0, 0,					/*  -> */
	0, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,		/* #4  */
	KEY_SEMICOLON, KEY_LEFTBRACE, KEY_RIGHTBRACE, 0, 0, 0,				/*  -> */
	0, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA,			/* #5  */
	KEY_DOT, KEY_KPMINUS, KEY_APOSTROPHE, KEY_ENTER, 0, 0,0,			/*  -> */
	0, KEY_TAB, 0, KEY_LEFTSHIFT, 0, KEY_APOSTROPHE, 0, 0, 0, 0,			/* #6  */
	KEY_UP, 0, KEY_RIGHTSHIFT, 0, 0, 0,0, 0, 0, 0, 0, KEY_LEFTALT, KEY_GRAVE,	/*  -> */
	0, 0, KEY_LEFT, KEY_DOWN, KEY_RIGHT, 0, 0, 0, 0,0, KEY_KPASTERISK,		/*  -> */
	KEY_LEFTCTRL, 0, KEY_SPACE, 0, 0, 0, KEY_SLASH, KEY_DELETE, 0, 0,		/*  -> */
	0, 0, 0, KEY_POWER,								/*  -> */
};


#define TYPE_J720_MCU "j720-mcu"
OBJECT_DECLARE_SIMPLE_TYPE(J720MCUState, J720_MCU)

struct J720MCUState {
    SSIPeripheral parent_obj;

    qemu_irq kbd_irq;           /* GPIO0, active low */
    qemu_irq ts_irq;            /* GPIO9, low while pen down */

    uint8_t out[16];            /* data bytes waiting to be clocked out */
    int out_len, out_pos;
    int expect_data;            /* SET* command: next byte is its value */
    uint8_t contrast, brightness;

    uint8_t keyq[J720_MCU_KEYQ];
    int keyq_len;
    uint8_t keydown[128 / 8];   /* modifier key codes currently held */
    /*
     * Keys and characters waiting for room in keyq (not migrated): a
     * character is typed as one Alt+digits sequence, which must not be
     * split. Entries: J720_MCU_PEND_RAW | code, or a Unicode code point.
     */
    uint32_t pend[J720_MCU_PEND];
    int pend_head, pend_len;
    QEMUTimer *kbd_timer;       /* pend after a read; GPIO0 watchdog */
    bool kbd_line_low;          /* GPIO0 low: codes waiting in keyq */

    bool pen_down;
    bool pen_up_pending;        /* released before enough samples were read */
    int pen_up_ticks;           /* periods pen_up_pending has lasted (not migrated) */
    int pen_samples;            /* GETTOUCHSAMPLES answered since pen down */
    int pen_x, pen_y;           /* 10-bit ADC values */
    int abs_x, abs_y;           /* last absolute pointer position from the UI */
    bool debug;                 /* J720_TOUCH_DEBUG set: log pen events */
    QEMUTimer *ts_timer;        /* sample pulses on GPIO9 while pen down */
};

static uint8_t j720_bitrev8(uint8_t b)
{
    b = (b & 0xf0) >> 4 | (b & 0x0f) << 4;
    b = (b & 0xcc) >> 2 | (b & 0x33) << 2;
    return (b & 0xaa) >> 1 | (b & 0x55) << 1;
}

static void j720_mcu_put(J720MCUState *s, uint8_t b)
{
    if (s->out_len < (int)sizeof(s->out)) {
        s->out[s->out_len++] = b;
    }
}

static void j720_mcu_put_samples(J720MCUState *s, int v0, int v1, int v2)
{
    j720_mcu_put(s, v0);
    j720_mcu_put(s, v1);
    j720_mcu_put(s, v2);
}

static uint8_t j720_mcu_high_bits(int v)
{
    /* high 2 bits of three identical samples, packed as the ts driver expects */
    v = (v >> 8) & 3;
    return v | v << 2 | v << 4;
}

static void j720_mcu_drain(J720MCUState *s);

static uint8_t j720_mcu_byte(J720MCUState *s, uint8_t c)
{
    int i;

    if (s->expect_data) {
        if (s->expect_data == J720_MCU_SETCONTRAST) {
            s->contrast = c;
        } else {
            s->brightness = c;
        }
        s->expect_data = 0;
        return J720_MCU_TXDUMMY;
    }

    if (s->out_pos < s->out_len) {
        uint8_t b = s->out[s->out_pos++];
        if (s->out_pos == s->out_len) {
            s->out_len = s->out_pos = 0;
        }
        return b;
    }

    s->out_len = s->out_pos = 0;
    switch (c) {
    case J720_MCU_GETSCANKEYCODE:
        j720_mcu_put(s, s->keyq_len);
        for (i = 0; i < s->keyq_len; i++) {
            j720_mcu_put(s, s->keyq[i]);
        }
        s->keyq_len = 0;
        qemu_irq_raise(s->kbd_irq);
        s->kbd_line_low = false;
        /*
         * Not right away: CE acknowledges the GPIO0 edge after reading,
         * which would swallow one made during the read.
         */
        if (s->pend_len) {
            timer_mod(s->kbd_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      J720_MCU_KBD_DELAY_MS);
        } else {
            timer_del(s->kbd_timer);
        }
        break;
    case J720_MCU_GETTOUCHSAMPLES:
        s->pen_samples++;
        j720_mcu_put_samples(s, s->pen_x & 0xff, s->pen_x & 0xff,
                             s->pen_x & 0xff);
        j720_mcu_put_samples(s, s->pen_y & 0xff, s->pen_y & 0xff,
                             s->pen_y & 0xff);
        j720_mcu_put(s, j720_mcu_high_bits(s->pen_x));
        j720_mcu_put(s, j720_mcu_high_bits(s->pen_y));
        break;
    case J720_MCU_GETBATTERYDATA:
        j720_mcu_put(s, J720_MCU_BATT_MAIN & 0xff);
        j720_mcu_put(s, J720_MCU_BATT_BACKUP & 0xff);
        j720_mcu_put(s, (J720_MCU_BATT_MAIN >> 8 & 3) |
                        (J720_MCU_BATT_BACKUP >> 8 & 3) << 2);
        break;
    case J720_MCU_GETCONTRAST:
        j720_mcu_put(s, s->contrast);
        break;
    case J720_MCU_GETBRIGHTNESS:
        j720_mcu_put(s, s->brightness);
        break;
    case J720_MCU_SETCONTRAST:
    case J720_MCU_SETBRIGHTNESS:
        s->expect_data = c;
        break;
    case J720_MCU_TXDUMMY:
    case 0x88:  /* CE clocks data out with this one */
        return J720_MCU_TXDUMMY;
    default:
        qemu_log_mask(LOG_UNIMP, "j720.mcu: command 0x%02x\n", c);
        break;
    }
    return J720_MCU_TXDUMMY;
}

static uint32_t j720_mcu_transfer(SSIPeripheral *dev, uint32_t val)
{
    J720MCUState *s = J720_MCU(dev);

    return j720_bitrev8(j720_mcu_byte(s, j720_bitrev8(val)));
}

static int j720_mcu_code(unsigned int lnx)
{
    int code;

    for (code = 1; code < 128; code++) {
        if (j720_keymap[code] == lnx) {
            return code;
        }
    }
    return 0;
}

static bool j720_mcu_held(J720MCUState *s, int code)
{
    return code && (s->keydown[code / 8] & (1 << code % 8));
}

/*
 * CE's keyboard driver (TSCkbdr.dll) turns Alt + decimal digits into the
 * character with that code once Alt goes up, Unicode included. That is
 * how characters the ROM's US layout cannot make (Cyrillic, from the host
 * layout) get typed. Shift is lifted around the sequence.
 */
static int j720_mcu_char_codes(J720MCUState *s, uint32_t cp, uint8_t *out)
{
    static const unsigned int digit[10] = {
        KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9
    };
    int alt = j720_mcu_code(KEY_LEFTALT);
    int shifts[2] = { j720_mcu_code(KEY_LEFTSHIFT),
                      j720_mcu_code(KEY_RIGHTSHIFT) };
    char dec[12];
    int n = 0, i;

    snprintf(dec, sizeof(dec), "%02u", cp);    /* at least two digits */
    for (i = 0; i < 2; i++) {
        if (j720_mcu_held(s, shifts[i])) {
            out[n++] = shifts[i] | 0x80;
        }
    }
    out[n++] = alt;
    for (i = 0; dec[i]; i++) {
        int code = j720_mcu_code(digit[dec[i] - '0']);
        out[n++] = code;
        out[n++] = code | 0x80;
    }
    out[n++] = alt | 0x80;
    for (i = 0; i < 2; i++) {
        if (j720_mcu_held(s, shifts[i])) {
            out[n++] = shifts[i];
        }
    }
    return n;
}

/* Move whatever fits from pend into keyq, keeping sequences whole */
static void j720_mcu_drain(J720MCUState *s)
{
    uint8_t seq[J720_MCU_KEYQ];
    bool added = false;

    while (s->pend_len) {
        uint32_t e = s->pend[s->pend_head];
        int n;

        if (e & J720_MCU_PEND_RAW) {
            seq[0] = e;
            n = 1;
        } else if (e & J720_MCU_PEND_PRESS) {
            seq[0] = e;
            seq[1] = (e & 0x7f) | 0x80;
            n = 2;
        } else if (e > 99999) {
            n = 0;                          /* CE takes at most 5 digits */
        } else {
            n = j720_mcu_char_codes(s, e, seq);
        }
        if (s->keyq_len + n > J720_MCU_KEYQ_USE) {
            break;
        }
        memcpy(s->keyq + s->keyq_len, seq, n);
        s->keyq_len += n;
        added |= n > 0;
        s->pend_head = (s->pend_head + 1) % J720_MCU_PEND;
        s->pend_len--;
    }
    if (added) {
        qemu_irq_lower(s->kbd_irq);
        s->kbd_line_low = true;
        timer_mod(s->kbd_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  J720_MCU_KBD_WATCHDOG_MS);
    }
}

static bool j720_mcu_pend(J720MCUState *s, uint32_t e)
{
    if (s->pend_len == J720_MCU_PEND) {
        return false;
    }
    s->pend[(s->pend_head + s->pend_len++) % J720_MCU_PEND] = e;
    if (!timer_pending(s->kbd_timer) || s->kbd_line_low) {
        j720_mcu_drain(s);
    }
    return true;
}

/*
 * After a read: hand out more codes. While codes wait unread: CE may have
 * acknowledged the GPIO0 edge without reading (a key make stuck without
 * its break then leaves a button pressed in CE), so raise the line and
 * drop it again for a fresh edge.
 */
static void j720_mcu_kbd_tick(void *opaque)
{
    J720MCUState *s = opaque;

    if (!s->keyq_len) {
        j720_mcu_drain(s);
    } else if (s->kbd_line_low) {
        qemu_irq_raise(s->kbd_irq);
        s->kbd_line_low = false;
        timer_mod(s->kbd_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  J720_MCU_KBD_DELAY_MS);
    } else {
        qemu_irq_lower(s->kbd_irq);
        s->kbd_line_low = true;
        timer_mod(s->kbd_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  J720_MCU_KBD_WATCHDOG_MS);
    }
}

static bool j720_mcu_key_queue(J720MCUState *s, uint8_t b)
{
    return j720_mcu_pend(s, J720_MCU_PEND_RAW | b);
}

static void j720_mcu_text(void *opaque, uint32_t codepoint)
{
    if (j720_wake_on_input(true)) {
        return;
    }
    j720_mcu_pend(opaque, codepoint);
}

/*
 * The real MCU reports make and break codes and CE does its own
 * autorepeat, timed by its clock. Under -icount that clock runs fast
 * (~12x with the default shift=6), so even a quick key press repeated
 * in CE. Instead, every key-down of an ordinary key, the host's own
 * autorepeat included, becomes a complete make+break press; only
 * modifiers are held for real.
 */
static void j720_mcu_key_event(DeviceState *dev, QemuConsole *src,
                               QemuInputEvent *evt)
{
    J720MCUState *s = J720_MCU(dev);
    unsigned int lnx = evt->key.key;    /* Linux key code */
    bool down = evt->key.down;

    if (j720_wake_on_input(down)) {
        return;
    }
    bool held;
    int code;

    /* the Jornada has only left Ctrl and Alt */
    if (lnx == KEY_RIGHTCTRL) {
        lnx = KEY_LEFTCTRL;
    } else if (lnx == KEY_RIGHTALT) {
        lnx = KEY_LEFTALT;
    }
    code = j720_mcu_code(lnx);
    if (!code) {
        return;
    }

    switch (lnx) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
    case KEY_LEFTCTRL:
    case KEY_LEFTALT:
        held = s->keydown[code / 8] & (1 << code % 8);
        if (held != down && j720_mcu_key_queue(s, code | (down ? 0 : 0x80))) {
            s->keydown[code / 8] ^= 1 << code % 8;
        }
        break;
    default:
        if (down) {
            j720_mcu_pend(s, J720_MCU_PEND_PRESS | code);
        }
        break;
    }
}

static void j720_mcu_pen_up(J720MCUState *s)
{
    s->pen_down = false;
    s->pen_up_pending = false;
    qemu_irq_raise(s->ts_irq);
    timer_del(s->ts_timer);
}

static void j720_mcu_ts_tick(void *opaque)
{
    J720MCUState *s = opaque;

    if (s->pen_up_pending &&
        (s->pen_samples >= J720_MCU_TS_MIN_SAMPLES ||
         ++s->pen_up_ticks >= J720_MCU_TS_MAX_HOLD)) {
        j720_mcu_pen_up(s);
        return;
    }
    if (s->pen_down) {
        qemu_irq_raise(s->ts_irq);
        qemu_irq_lower(s->ts_irq);
        timer_mod(s->ts_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  J720_MCU_TS_PERIOD_MS);
    }
}

static void j720_mcu_pointer_event(DeviceState *dev, QemuConsole *src,
                                   QemuInputEvent *evt)
{
    J720MCUState *s = J720_MCU(dev);

    if (j720_wake_on_input(evt->type == INPUT_EVENT_KIND_BTN &&
                           evt->btn.down)) {
        return;
    }
    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS: {
        InputMoveEvent *move = &evt->abs;
        /* map the whole screen onto most of the 10-bit ADC range */
        int v = qemu_input_scale_axis(MIN(MAX(move->value,
                                                  INPUT_EVENT_ABS_MIN),
                                              INPUT_EVENT_ABS_MAX),
                                      INPUT_EVENT_ABS_MIN,
                                      INPUT_EVENT_ABS_MAX, 64, 960);
        if (move->axis == INPUT_AXIS_X) {
            s->pen_x = v;
            s->abs_x = move->value;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->pen_y = v;
            s->abs_y = move->value;
        }
        break;
    }
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *btn = &evt->btn;
        if (btn->button != INPUT_BUTTON_LEFT) {
            break;
        }
        if (s->debug) {
            fprintf(stderr, "j720 touch: pen %s ui=(%d,%d) ~px=(%d,%d) "
                    "adc=(%d,%d) samples=%d\n", btn->down ? "down" : "up",
                    s->abs_x, s->abs_y,
                    (int)((int64_t)s->abs_x * J720_LCD_WIDTH /
                          (INPUT_EVENT_ABS_MAX + 1)),
                    (int)((int64_t)s->abs_y * J720_LCD_HEIGHT /
                          (INPUT_EVENT_ABS_MAX + 1)),
                    s->pen_x, s->pen_y, s->pen_samples);
        }
        if (btn->down) {
            s->pen_down = true;
            s->pen_up_pending = false;
            s->pen_up_ticks = 0;
            s->pen_samples = 0;
            qemu_irq_lower(s->ts_irq);
            timer_mod(s->ts_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      J720_MCU_TS_PERIOD_MS);
        } else if (s->pen_samples < J720_MCU_TS_MIN_SAMPLES) {
            s->pen_up_pending = true;
        } else {
            j720_mcu_pen_up(s);
        }
        break;
    }
    default:
        break;
    }
}

static const QemuInputHandler j720_mcu_kbd_handler = {
    .name  = "Jornada 720 keyboard",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = j720_mcu_key_event,
};

static const QemuInputHandler j720_mcu_ts_handler = {
    .name  = "Jornada 720 touchscreen",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = j720_mcu_pointer_event,
};

static void j720_mcu_realize(SSIPeripheral *dev, Error **errp)
{
    J720MCUState *s = J720_MCU(dev);
    QemuInputHandlerState *hs;

    s->contrast = 0x80;
    s->brightness = 0x80;
    s->ts_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, j720_mcu_ts_tick, s);
    s->kbd_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, j720_mcu_kbd_tick, s);
    s->debug = getenv("J720_TOUCH_DEBUG") != NULL;
    hs = qemu_input_handler_register(DEVICE(dev), &j720_mcu_kbd_handler);
    qemu_input_handler_activate(hs);
    hs = qemu_input_handler_register(DEVICE(dev), &j720_mcu_ts_handler);
    qemu_input_handler_activate(hs);
    qemu_input_set_text_hook(j720_mcu_text, s);
}

static int j720_mcu_post_load(void *opaque, int version_id)
{
    J720MCUState *s = opaque;

    if (s->out_len < 0 || s->out_len > (int)sizeof(s->out) ||
        s->out_pos < 0 || s->out_pos > s->out_len ||
        s->keyq_len < 0 || s->keyq_len > J720_MCU_KEYQ) {
        return -EINVAL;
    }
    /* codes left unread (possibly stuck, see j720_mcu_kbd_tick) */
    s->kbd_line_low = s->keyq_len > 0;
    if (s->kbd_line_low) {
        timer_mod(s->kbd_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  J720_MCU_KBD_WATCHDOG_MS);
    }
    return 0;
}

static bool j720_mcu_keydown_needed(void *opaque)
{
    J720MCUState *s = opaque;

    return !buffer_is_zero(s->keydown, sizeof(s->keydown));
}

static const VMStateDescription vmstate_j720_mcu_keydown = {
    .name = TYPE_J720_MCU "/keydown",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = j720_mcu_keydown_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(keydown, J720MCUState, 128 / 8),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_j720_mcu = {
    .name = TYPE_J720_MCU,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = j720_mcu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, J720MCUState),
        VMSTATE_UINT8_ARRAY(out, J720MCUState, 16),
        VMSTATE_INT32(out_len, J720MCUState),
        VMSTATE_INT32(out_pos, J720MCUState),
        VMSTATE_INT32(expect_data, J720MCUState),
        VMSTATE_UINT8(contrast, J720MCUState),
        VMSTATE_UINT8(brightness, J720MCUState),
        VMSTATE_UINT8_ARRAY(keyq, J720MCUState, J720_MCU_KEYQ),
        VMSTATE_INT32(keyq_len, J720MCUState),
        VMSTATE_BOOL(pen_down, J720MCUState),
        VMSTATE_BOOL(pen_up_pending, J720MCUState),
        VMSTATE_INT32(pen_samples, J720MCUState),
        VMSTATE_INT32(pen_x, J720MCUState),
        VMSTATE_INT32(pen_y, J720MCUState),
        VMSTATE_TIMER_PTR(ts_timer, J720MCUState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_j720_mcu_keydown,
        NULL
    }
};

static void j720_mcu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = j720_mcu_realize;
    k->transfer = j720_mcu_transfer;
    dc->vmsd = &vmstate_j720_mcu;
}

static const TypeInfo j720_mcu_typeinfo = {
    .name = TYPE_J720_MCU,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(J720MCUState),
    .class_init = j720_mcu_class_init,
};

/*
 * Epson S1D13806 display controller (as on the Jornada 720, per Linux's
 * s1d13xxxfb board setup). The frame buffer is plain RAM, scanned out as
 * a fixed 640x240, 16bpp RGB565 panel from offset 0. Display start
 * address / stride / bpp registers are NOT decoded; a CE mode that
 * differs from this guess will look garbled.
 *
 * Registers 0x000-0x1ff are a plain register file, except for the 2D
 * BitBLT engine (0x100-0x119 + data port at +0x100000) that CE's ddi.dll
 * draws everything through. Implemented, 16bpp only, rectangular
 * addressing only:
 *   op 0x0/0x4 write blit (+transparent), 0x1 read blit,
 *   0x2/0x3 move blit positive/negative, 0x5 transparent move,
 *   0x6/0x7 pattern fill (+transparent), 0x8/0x9 color expansion
 *   (+transparent), 0xc solid fill.
 * Not implemented (logged under -d unimp): move with color expansion
 * (0xa/0xb), linear addressing, 8bpp.
 * Semantics are reconstructed from the S1D13806 register map and from
 * what ddi.dll was observed to do; not verified against the datasheet.
 */
#define J720_EPSON_NREGS        0x200
#define J720_EPSON_BLT_DATA     0x100000

#define EPSON_BLT_CTL0          0x100   /* b7 start/active, b6 FIFO not empty */
#define EPSON_BLT_CTL1          0x101   /* b0 16bpp */
#define EPSON_BLT_ROP           0x102   /* ROP code / color expansion start bit */
#define EPSON_BLT_OP            0x103
#define EPSON_BLT_SRC           0x104   /* 3 bytes, byte address */
#define EPSON_BLT_DST           0x108   /* 3 bytes, byte address */
#define EPSON_BLT_MEM_OFF       0x10c   /* 2 bytes, stride in 16-bit words */
#define EPSON_BLT_WIDTH         0x110   /* 2 bytes, pixels - 1 */
#define EPSON_BLT_HEIGHT        0x112   /* 2 bytes, lines - 1 */
#define EPSON_BLT_BGC           0x114   /* 2 bytes */
#define EPSON_BLT_FGC           0x118   /* 2 bytes */

typedef struct J720Display {
    MemoryRegion fb;
    MemoryRegion regs;
    uint8_t reg[J720_EPSON_NREGS];
    QemuConsole *con;
    bool surface_set;
    bool invalidate;
    /*
     * Lines touched by the BitBLT engine since the last refresh: it
     * writes the frame buffer through the RAM pointer, which the dirty
     * log used for CPU writes does not see.
     */
    int blt_dirty_lo, blt_dirty_hi;

    /* BitBLT operation in progress (write/read/color expansion blits) */
    int blt_op;
    uint32_t blt_x, blt_y, blt_w, blt_h;
    uint32_t blt_bit;       /* color expansion: next bit in current word */
} J720Display;

#define J720_LCD_LINE_BYTES     (J720_LCD_WIDTH * 2)

/* Only push the lines that changed: a full refresh every frame is costly
 * on a slow host, especially when SDL scales it up to a phone screen. */
static bool j720_display_update(void *opaque)
{
    J720Display *d = opaque;
    DirtyBitmapSnapshot *snap;
    int y, lo = J720_LCD_HEIGHT, hi = -1;

    if (!d->surface_set) {
        DisplaySurface *ds = qemu_create_displaysurface_from(
            J720_LCD_WIDTH, J720_LCD_HEIGHT, PIXMAN_r5g6b5,
            J720_LCD_LINE_BYTES, memory_region_get_ram_ptr(&d->fb));
        qemu_console_set_surface(d->con, ds);
        d->surface_set = true;
        d->invalidate = true;
    }

    snap = memory_region_snapshot_and_clear_dirty(&d->fb, 0,
                J720_LCD_LINE_BYTES * J720_LCD_HEIGHT, DIRTY_MEMORY_VGA);
    for (y = 0; y < J720_LCD_HEIGHT; y++) {
        if (d->invalidate ||
            (y >= d->blt_dirty_lo && y <= d->blt_dirty_hi) ||
            memory_region_snapshot_get_dirty(&d->fb, snap,
                                             y * J720_LCD_LINE_BYTES,
                                             J720_LCD_LINE_BYTES)) {
            lo = MIN(lo, y);
            hi = y;
        }
    }
    g_free(snap);
    d->invalidate = false;
    d->blt_dirty_lo = INT_MAX;
    d->blt_dirty_hi = -1;

    if (hi >= lo) {
        qemu_console_update(d->con, 0, lo, J720_LCD_WIDTH, hi - lo + 1);
    }
    return true;
}

static void j720_display_invalidate(void *opaque)
{
    J720Display *d = opaque;

    d->invalidate = true;
}

static uint32_t epson_reg16(J720Display *d, unsigned r)
{
    return d->reg[r] | (d->reg[r + 1] << 8);
}

static uint32_t epson_reg24(J720Display *d, unsigned r)
{
    return d->reg[r] | (d->reg[r + 1] << 8) | (d->reg[r + 2] << 16);
}

static uint16_t *epson_px(J720Display *d, uint32_t addr)
{
    uint8_t *fb = memory_region_get_ram_ptr(&d->fb);

    return (uint16_t *)(fb + (addr & (J720_EPSON_FB_SIZE - 2)));
}

/* Pointer to a pixel the engine is about to write; records the line */
static uint16_t *epson_px_w(J720Display *d, uint32_t addr)
{
    int y = (addr & (J720_EPSON_FB_SIZE - 2)) / J720_LCD_LINE_BYTES;

    d->blt_dirty_lo = MIN(d->blt_dirty_lo, y);
    d->blt_dirty_hi = MAX(d->blt_dirty_hi, y);
    return epson_px(d, addr);
}

/* S1D13806 ROP codes: 16 boolean functions of source S and destination D */
static uint16_t epson_rop(unsigned rop, uint16_t s, uint16_t dst)
{
    switch (rop & 0xf) {
    case 0x0: return 0;
    case 0x1: return ~(s | dst);
    case 0x2: return ~s & dst;
    case 0x3: return ~s;
    case 0x4: return s & ~dst;
    case 0x5: return ~dst;
    case 0x6: return s ^ dst;
    case 0x7: return ~(s & dst);
    case 0x8: return s & dst;
    case 0x9: return ~(s ^ dst);
    case 0xa: return dst;
    case 0xb: return ~s | dst;
    case 0xc: return s;
    case 0xd: return s | ~dst;
    case 0xe: return s | dst;
    default:  return 0xffff;
    }
}

static uint32_t epson_stride(J720Display *d)
{
    return (epson_reg16(d, EPSON_BLT_MEM_OFF) & 0x7ff) * 2;
}

/* Address of pixel (x, y) of the current rectangle, relative to reg r */
static uint32_t epson_rect_addr(J720Display *d, unsigned r, uint32_t x,
                                uint32_t y)
{
    return epson_reg24(d, r) + y * epson_stride(d) + x * 2;
}

static void epson_blt_advance(J720Display *d)
{
    if (++d->blt_x >= d->blt_w) {
        d->blt_x = 0;
        d->blt_y++;
        d->blt_bit = d->reg[EPSON_BLT_ROP] & 0xf;
    }
    if (d->blt_y >= d->blt_h) {
        d->blt_op = -1;
    }
}

/* One pixel pushed by the CPU through the data port (write blits) */
static void epson_blt_push_pixel(J720Display *d, uint16_t s)
{
    uint16_t *p = epson_px_w(d, epson_rect_addr(d, EPSON_BLT_DST,
                                                d->blt_x, d->blt_y));

    if (d->blt_op != 0x4 || s != epson_reg16(d, EPSON_BLT_BGC)) {
        *p = epson_rop(d->reg[EPSON_BLT_ROP], s, *p);
    }
    epson_blt_advance(d);
}

/* One 16-bit mono word pushed by the CPU (color expansion blits) */
static void epson_blt_push_mono(J720Display *d, uint16_t w)
{
    uint32_t y = d->blt_y;
    int bit;

    for (bit = d->blt_bit; bit >= 0 && d->blt_op >= 0 && d->blt_y == y;
         bit--) {
        uint16_t *p = epson_px_w(d, epson_rect_addr(d, EPSON_BLT_DST,
                                                    d->blt_x, d->blt_y));
        if (w & (1 << bit)) {
            *p = epson_reg16(d, EPSON_BLT_FGC);
        } else if (d->blt_op == 0x8) {
            *p = epson_reg16(d, EPSON_BLT_BGC);
        }
        epson_blt_advance(d);
    }
    if (d->blt_op >= 0 && d->blt_y == y) {
        d->blt_bit = 15;
    }
}

static void epson_blt_start(J720Display *d)
{
    unsigned op = d->reg[EPSON_BLT_OP] & 0xf;
    unsigned rop = d->reg[EPSON_BLT_ROP];
    uint32_t w = (epson_reg16(d, EPSON_BLT_WIDTH) & 0x3ff) + 1;
    uint32_t h = (epson_reg16(d, EPSON_BLT_HEIGHT) & 0x3ff) + 1;
    uint16_t fgc = epson_reg16(d, EPSON_BLT_FGC);
    uint16_t bgc = epson_reg16(d, EPSON_BLT_BGC);
    uint32_t stride = epson_stride(d);
    uint32_t src = epson_reg24(d, EPSON_BLT_SRC);
    uint32_t dst = epson_reg24(d, EPSON_BLT_DST);
    uint32_t x, y;

    if (!(d->reg[EPSON_BLT_CTL1] & 1) || (d->reg[EPSON_BLT_CTL0] & 3)) {
        qemu_log_mask(LOG_UNIMP, "j720.epson: BitBLT mode ctl0=0x%02x "
                      "ctl1=0x%02x not implemented\n",
                      d->reg[EPSON_BLT_CTL0], d->reg[EPSON_BLT_CTL1]);
    }

    d->blt_op = -1;
    d->blt_x = d->blt_y = 0;
    d->blt_w = w;
    d->blt_h = h;
    d->blt_bit = rop & 0xf;

    switch (op) {
    case 0x0: /* write blit with ROP */
    case 0x1: /* read blit */
    case 0x4: /* transparent write blit */
    case 0x8: /* color expansion */
    case 0x9: /* transparent color expansion */
        d->blt_op = op;
        break;
    case 0x2: /* move blit, positive direction, with ROP */
    case 0x5: /* transparent move blit, positive direction */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                uint16_t sp = *epson_px(d, src + y * stride + x * 2);
                uint16_t *dp = epson_px_w(d, dst + y * stride + x * 2);
                if (op == 0x2) {
                    *dp = epson_rop(rop, sp, *dp);
                } else if (sp != bgc) {
                    *dp = sp;
                }
            }
        }
        break;
    case 0x3: /* move blit, negative direction: addresses of last pixel */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                uint16_t sp = *epson_px(d, src - y * stride - x * 2);
                uint16_t *dp = epson_px_w(d, dst - y * stride - x * 2);
                *dp = epson_rop(rop, sp, *dp);
            }
        }
        break;
    case 0x6: /* pattern fill with ROP: 8x8 pattern at src */
    case 0x7: /* pattern fill with transparency */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                uint32_t px = ((src >> 1) + x) & 7;
                uint32_t py = ((src >> 4) + y) & 7;
                uint16_t pp = *epson_px(d, (src & ~0x7f) + py * 16 + px * 2);
                uint16_t *dp = epson_px_w(d, dst + y * stride + x * 2);
                if (op == 0x6) {
                    *dp = epson_rop(rop, pp, *dp);
                } else if (pp != bgc) {
                    *dp = pp;
                }
            }
        }
        break;
    case 0xc: /* solid fill */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                *epson_px_w(d, dst + y * stride + x * 2) = fgc;
            }
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "j720.epson: BitBLT op 0x%x not implemented\n",
                      op);
        break;
    }
}

static uint64_t j720_epson_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    J720Display *d = opaque;
    uint64_t v = 0;
    unsigned i;

    if (addr >= J720_EPSON_BLT_DATA) {
        /* read blit: hand out source pixels, 16 bits at a time */
        for (i = 0; i < size; i += 2) {
            if (d->blt_op == 0x1) {
                v |= (uint64_t)*epson_px(d, epson_rect_addr(d, EPSON_BLT_SRC,
                                         d->blt_x, d->blt_y)) << (8 * i);
                epson_blt_advance(d);
            }
        }
        return v;
    }
    for (i = 0; i < size && addr + i < J720_EPSON_NREGS; i++) {
        uint8_t b = d->reg[addr + i];
        if (addr + i == EPSON_BLT_CTL0) {
            b &= ~0xf0;
            if (d->blt_op >= 0) {
                b |= 0x80;
            }
            if (d->blt_op == 0x1) {
                b |= 0x40;
            }
        }
        v |= (uint64_t)b << (8 * i);
    }
    return v;
}

static void j720_epson_regs_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    J720Display *d = opaque;
    unsigned i;

    if (addr >= J720_EPSON_BLT_DATA) {
        for (i = 0; i < size; i += 2) {
            uint16_t w = value >> (8 * i);
            switch (d->blt_op) {
            case 0x0:
            case 0x4:
                epson_blt_push_pixel(d, w);
                break;
            case 0x8:
            case 0x9:
                epson_blt_push_mono(d, w);
                break;
            default:
                break;
            }
        }
        return;
    }
    for (i = 0; i < size && addr + i < J720_EPSON_NREGS; i++) {
        d->reg[addr + i] = value >> (8 * i);
    }
    if (addr <= EPSON_BLT_CTL0 && EPSON_BLT_CTL0 < addr + size &&
        (d->reg[EPSON_BLT_CTL0] & 0x80)) {
        epson_blt_start(d);
    }
}

static const MemoryRegionOps j720_epson_regs_ops = {
    .read = j720_epson_regs_read,
    .write = j720_epson_regs_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const GraphicHwOps j720_display_ops = {
    .invalidate = j720_display_invalidate,
    .gfx_update = j720_display_update,
};

/*
 * SA-1110 on-chip USB device controller (0x80000000), missing from
 * strongarm.c. CE's udcser.dll (ActiveSync over USB, running inside
 * device.exe) sets UDCCR bit 0 (UDC disable) and spins until it reads
 * back as 1 -- forever, on an empty bus, starving the GUI. This is only
 * a read-back register file (no USB), logged under -d unimp.
 */
#define J720_SA1110_UDC_BASE 0x80000000
#define J720_SA1110_UDC_SIZE 0x100

static uint64_t j720_udc_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *reg = opaque;

    qemu_log_mask(LOG_UNIMP, "j720.sa1110-udc: read 0x%02" HWADDR_PRIx "\n",
                  addr);
    return reg[addr >> 2];
}

static void j720_udc_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    uint32_t *reg = opaque;

    qemu_log_mask(LOG_UNIMP, "j720.sa1110-udc: write 0x%02" HWADDR_PRIx
                  " value 0x%" PRIx64 "\n", addr, value);
    reg[addr >> 2] = value;
}

static const MemoryRegionOps j720_udc_ops = {
    .read = j720_udc_read,
    .write = j720_udc_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/*
 * SA-1111 companion chip: interrupt controller and PCMCIA interface,
 * with an NE2000-compatible network PC Card in socket 0 and a
 * CompactFlash storage card in socket 1, the CF slot (see "CF card"
 * below). Register layout from Linux
 * (arch/arm/common/sa1111.c, drivers/pcmcia/sa1111_generic.c); the rest
 * of the chip is still the unimplemented-device stub underneath.
 *
 * The CE ROM has NE2000.DLL and a Drivers\PCMCIA\Detect entry
 * (DetectNE2000) for NE2000 cards, so the card only needs a plausible
 * CIS: network function, one I/O configuration at 0x300-0x31f with an
 * interrupt. The card's interrupt (nIREQ) drives socket 0's READY line,
 * which the SA-1111 interrupt controller sees as IRQ 49 (S0_READY_NINT);
 * the controller's output goes to SA-1110 GPIO1, as on the real board.
 *
 * SA-1110 static memory for socket 0: I/O at 0x20000000, attribute
 * memory at 0x28000000 (CIS on even bytes, configuration registers at
 * 0x3f8), common memory at 0x2c000000 (unused).
 */
#define J720_SA1111_INTC_BASE   (J720_SA1111_BASE + 0x1600)
#define J720_SA1111_PCMCIA_BASE (J720_SA1111_BASE + 0x1800)
#define J720_PCMCIA_S0_IO       0x20000000
#define J720_PCMCIA_S0_ATTR     0x28000000
#define J720_PCMCIA_WINDOW      (64 * MiB)
#define J720_GPIO_SA1111_IRQ    1

#define SA1111_INTEN0           0x08
#define SA1111_INTEN1           0x0c
#define SA1111_INTPOL0          0x10
#define SA1111_INTPOL1          0x14
#define SA1111_INTSTATCLR0      0x1c
#define SA1111_INTSTATCLR1      0x20
#define SA1111_INTSET0          0x24
#define SA1111_INTSET1          0x28
#define SA1111_WAKEEN0          0x2c
#define SA1111_WAKEEN1          0x30
#define SA1111_WAKEPOL0         0x34
#define SA1111_WAKEPOL1         0x38

/* interrupt numbers 32..63 live in bank 1 */
#define SA1111_IRQ_S0_READY_NINT (49 - 32)
#define SA1111_IRQ_S1_READY_NINT (50 - 32)
#define SA1111_IRQ_S0_CD_VALID   (51 - 32)
#define SA1111_IRQ_S1_CD_VALID   (52 - 32)
/* bank 1 inputs this model drives; the others' levels are unknown */
#define SA1111_INPUTS1          (1u << SA1111_IRQ_S0_READY_NINT | \
                                 1u << SA1111_IRQ_S1_READY_NINT | \
                                 1u << SA1111_IRQ_S0_CD_VALID | \
                                 1u << SA1111_IRQ_S1_CD_VALID)

#define SA1111_PCCR             0x00
#define SA1111_PCSSR            0x04
#define SA1111_PCSR             0x08
#define PCSR_S0_READY           (1 << 0)
#define PCSR_S1_READY           (1 << 1)
#define PCSR_S0_DETECT          (1 << 2)    /* 1 = socket empty */
#define PCSR_S1_DETECT          (1 << 3)
#define PCSR_S0_VS1             (1 << 4)    /* 0 = 3.3 V card */
#define PCSR_S0_VS2             (1 << 5)
#define PCSR_S1_VS1             (1 << 6)
#define PCSR_S1_VS2             (1 << 7)
#define PCSR_S0_BVD1            (1 << 10)
#define PCSR_S0_BVD2            (1 << 11)
#define PCSR_S1_BVD1            (1 << 12)
#define PCSR_S1_BVD2            (1 << 13)
#define PCCR_S0_RST             (1 << 0)
#define PCCR_S1_RST             (1 << 1)

#define J720_CARD_COR           0x3f8       /* attribute memory offsets */
#define J720_CARD_CCSR          0x3fa
#define J720_CARD_CFG_INDEX     0x20
#define COR_SRESET              0x80
#define COR_INDEX_MASK          0x3f
#define CCSR_INTR               0x02

/*
 * CF card: a CompactFlash storage card (PC Card ATA) in socket 1. The
 * ROM has ATADISK.DLL and FATFS.DLL, which mount it as \Storage Card.
 * Its medium is the block backend "cf", which may be empty: QEMU's
 * eject/change commands (HMP "change cf FILE raw", "eject cf"; QMP
 * blockdev-change-medium, eject) take the card out of the slot and put
 * one in, and the SA-1111 card-detect interrupt (IRQ 52, S1_CD_VALID,
 * low while a card is in) tells CE. A read-only medium, such as a host
 * directory as "fat:16:DIR" with read-only=on, makes a card that
 * fails writes.
 *
 * The card follows the CompactFlash spec: CIS with a fixed-disk FUNCID
 * and four configurations (0: memory mapped, the state after reset;
 * 1: 16 I/O ports anywhere; 2/3: the primary/secondary ATA ports), the
 * configuration registers at 0x200 of attribute memory, and the ATA
 * task file. Commands run synchronously, PIO only, one sector per DRQ
 * block. In an I/O configuration the card's INTRQ drives READY, which
 * the SA-1111 sees as IRQ 50 (S1_READY_NINT), like the NE2000 above.
 */
#define J720_PCMCIA_S1_IO       0x30000000
#define J720_PCMCIA_S1_ATTR     0x38000000
#define J720_PCMCIA_S1_MEM      0x3c000000

#define J720_CF_COR             0x200       /* attribute memory offsets */
#define J720_CF_CCSR            0x202
#define J720_CF_SECTOR          512

#define ATA_SR_ERR              0x01
#define ATA_SR_DRQ              0x08
#define ATA_SR_DSC              0x10
#define ATA_SR_DRDY             0x40
#define ATA_SR_BSY              0x80
#define ATA_ER_ABRT             0x04
#define ATA_ER_IDNF             0x10
#define ATA_ER_UNC              0x40
#define ATA_DC_NIEN             0x02
#define ATA_DC_SRST             0x04
#define ATA_DH_DEV              0x10
#define ATA_DH_LBA              0x40

/* how long a card resumed in the slot stays out before going in again */
#define J720_CF_REPLUG_MS       3000

typedef struct J720CFCard {
    BlockBackend *blk;
    QEMUTimer *resume_timer;
    bool inserted;
    bool writable;              /* the medium takes writes */
    uint64_t nb_sectors;        /* from the medium, not migrated */
    uint16_t cyls, heads, secs; /* current CHS translation */

    uint8_t cor, ccsr;
    bool intrq;

    /* ATA task file */
    uint8_t feature, error, nsector, sector, lcyl, hcyl, select;
    uint8_t status, devctl, cmd;

    /* PIO transfer: buf[pos..end), then xfer_left more sectors */
    uint8_t buf[J720_CF_SECTOR];
    uint32_t pos, end;
    uint32_t xfer_left;
    uint64_t lba;               /* sector in buf (write) / next (read) */
} J720CFCard;

#define TYPE_J720_SA1111 "j720-sa1111"
OBJECT_DECLARE_SIMPLE_TYPE(J720SA1111State, J720_SA1111)

struct J720SA1111State {
    SysBusDevice parent_obj;

    MemoryRegion intc_io;
    MemoryRegion pcmcia_io;
    MemoryRegion card_io;
    MemoryRegion card_attr;
    MemoryRegion cf_io;
    MemoryRegion cf_attr;
    MemoryRegion cf_mem;
    qemu_irq irq;               /* to SA-1110 GPIO1 */

    uint32_t inten[2], intpol[2], intstat[2], wakeen[2], wakepol[2];
    uint32_t intin[2];          /* current level of each interrupt input */
    uint32_t pccr, pcssr;

    uint8_t cor, ccsr;
    bool card_irq;
    uint8_t cis[128];
    int cis_len;
    NE2000State ne2000;

    J720CFCard cf;
};

static void j720_sa1111_update(J720SA1111State *s)
{
    qemu_set_irq(s->irq, (s->intstat[0] & s->inten[0]) ||
                         (s->intstat[1] & s->inten[1]));
}

/*
 * Interrupt inputs are edge-detected: a rising edge of the input XOR its
 * INTPOL bit (set = falling edge) latches it. So flipping INTPOL is an
 * edge too, and CE relies on that for card detect, as Linux does in
 * sa1111_retrigger_irq().
 */
static void j720_sa1111_set_intpol(J720SA1111State *s, int bank,
                                   uint32_t pol)
{
    uint32_t known = bank ? SA1111_INPUTS1 : 0;
    uint32_t old = s->intin[bank] ^ s->intpol[bank];
    uint32_t new = s->intin[bank] ^ pol;

    s->intpol[bank] = pol;
    s->intstat[bank] |= ~old & new & known;
}

static void j720_sa1111_set_input(J720SA1111State *s, int bank, int bit,
                                  bool level)
{
    uint32_t mask = 1u << bit;
    bool old = s->intin[bank] & mask;

    if (old == level) {
        return;
    }
    s->intin[bank] ^= mask;
    if (level == !(s->intpol[bank] & mask)) {
        s->intstat[bank] |= mask;
        j720_sa1111_update(s);
    }
}

static bool j720_card_ready(J720SA1111State *s)
{
    if (s->pccr & PCCR_S0_RST) {
        return false;
    }
    /* once configured as an I/O card, READY carries nIREQ */
    return !((s->cor & COR_INDEX_MASK) && s->card_irq);
}

static void j720_card_update(J720SA1111State *s)
{
    j720_sa1111_set_input(s, 1, SA1111_IRQ_S0_READY_NINT,
                          j720_card_ready(s));
}

static void j720_card_ne2000_irq(void *opaque, int n, int level)
{
    J720SA1111State *s = opaque;

    s->card_irq = level;
    s->ccsr = level ? s->ccsr | CCSR_INTR : s->ccsr & ~CCSR_INTR;
    j720_card_update(s);
}

/* CF card */

static bool j720_cf_ready(J720SA1111State *s)
{
    J720CFCard *cf = &s->cf;

    if (!cf->inserted || (s->pccr & PCCR_S1_RST)) {
        return false;
    }
    /* once configured as an I/O card, READY carries nIREQ */
    return !((cf->cor & COR_INDEX_MASK) && cf->intrq &&
             !(cf->devctl & ATA_DC_NIEN));
}

static void j720_cf_update(J720SA1111State *s)
{
    j720_sa1111_set_input(s, 1, SA1111_IRQ_S1_READY_NINT, j720_cf_ready(s));
}

static void j720_cf_set_intrq(J720SA1111State *s, bool level)
{
    J720CFCard *cf = &s->cf;

    cf->intrq = level;
    cf->ccsr = level ? cf->ccsr | CCSR_INTR : cf->ccsr & ~CCSR_INTR;
    j720_cf_update(s);
}

static void j720_cf_default_geometry(J720CFCard *cf)
{
    uint64_t cyls;

    cf->heads = 16;
    cf->secs = 63;
    cyls = cf->nb_sectors / (cf->heads * cf->secs);
    cf->cyls = MAX(1, MIN(cyls, 16383));
}

/* the ATA reset signature, after a hardware or software reset */
static void j720_cf_ata_reset(J720SA1111State *s)
{
    J720CFCard *cf = &s->cf;

    cf->error = 0x01;               /* diagnostics passed */
    cf->nsector = cf->sector = 1;
    cf->lcyl = cf->hcyl = 0;
    cf->select = 0xa0;
    cf->feature = cf->cmd = 0;
    cf->status = ATA_SR_DRDY | ATA_SR_DSC;
    cf->pos = cf->end = cf->xfer_left = 0;
    j720_cf_default_geometry(cf);
    j720_cf_set_intrq(s, false);
}

static void j720_cf_reset(J720SA1111State *s)
{
    s->cf.cor = s->cf.ccsr = 0;
    s->cf.devctl = 0;
    j720_cf_ata_reset(s);
}

static void j720_cf_set_inserted(J720SA1111State *s, bool inserted)
{
    J720CFCard *cf = &s->cf;
    int64_t len = inserted ? blk_getlength(cf->blk) : 0;

    cf->inserted = inserted;
    cf->nb_sectors = MAX(len, 0) / J720_CF_SECTOR;
    j720_cf_reset(s);
    /* card detect is active low */
    j720_sa1111_set_input(s, 1, SA1111_IRQ_S1_CD_VALID, !inserted);
}

/* write access when the medium allows it; a read-only card rejects writes */
static void j720_cf_set_perm(J720CFCard *cf)
{
    cf->writable = blk_is_inserted(cf->blk) &&
                   blk_supports_write_perm(cf->blk) &&
                   blk_set_perm(cf->blk,
                                BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                                BLK_PERM_ALL, NULL) == 0;
    if (!cf->writable) {
        blk_set_perm(cf->blk, BLK_PERM_CONSISTENT_READ, BLK_PERM_ALL, NULL);
    }
}

static void j720_cf_change_media_cb(void *opaque, bool load, Error **errp)
{
    J720SA1111State *s = opaque;

    j720_cf_set_perm(&s->cf);
    if (load != s->cf.inserted) {
        j720_cf_set_inserted(s, load);
    }
}

static void j720_cf_resume_timer(void *opaque)
{
    J720SA1111State *s = opaque;
    bool medium = blk_is_inserted(s->cf.blk);

    if (s->cf.inserted) {
        j720_cf_set_inserted(s, false);
        if (medium) {
            timer_mod(s->cf.resume_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      J720_CF_REPLUG_MS);
        }
    } else if (medium) {
        j720_cf_set_inserted(s, true);
    }
}

static const BlockDevOps j720_cf_block_ops = {
    .change_media_cb = j720_cf_change_media_cb,
};

static void j720_cf_put_string(uint16_t *id, const char *str, int words)
{
    int i;

    for (i = 0; i < words * 2; i++) {
        uint8_t c = *str ? *str++ : ' ';
        id[i / 2] |= (i & 1) ? c : c << 8;
    }
}

static void j720_cf_identify(J720CFCard *cf)
{
    uint16_t id[256] = { 0 };
    uint32_t cur = cf->cyls * cf->heads * cf->secs;
    uint32_t total = MIN(cf->nb_sectors, 0x0fffffff);
    int i;

    id[0] = 0x848a;                 /* CFA */
    id[1] = cf->cyls;
    id[3] = cf->heads;
    id[6] = cf->secs;
    id[7] = total >> 16;            /* sectors per card, MSW first */
    id[8] = total;
    j720_cf_put_string(&id[10], "QEMU0720CF", 10);
    id[20] = 2;                     /* dual-ported buffer */
    id[21] = 2;                     /* buffer size in sectors */
    id[22] = 4;                     /* ECC bytes */
    j720_cf_put_string(&id[23], "1.0", 4);
    j720_cf_put_string(&id[27], "QEMU CompactFlash Card", 20);
    id[47] = 0x8001;                /* READ/WRITE MULTIPLE: 1 sector */
    id[49] = 0x0200;                /* LBA */
    id[51] = 0x0200;                /* PIO mode 2 */
    id[53] = 0x0001;                /* words 54-58 valid */
    id[54] = cf->cyls;
    id[55] = cf->heads;
    id[56] = cf->secs;
    id[57] = cur;
    id[58] = cur >> 16;
    id[60] = total;
    id[61] = total >> 16;
    for (i = 0; i < 256; i++) {
        stw_le_p(&cf->buf[i * 2], id[i]);
    }
}

/* the sector the task file addresses, or -1 if it is not on the card */
static int64_t j720_cf_task_lba(J720CFCard *cf)
{
    uint64_t lba;
    unsigned cyl = cf->hcyl << 8 | cf->lcyl, head = cf->select & 0x0f;

    if (cf->select & ATA_DH_LBA) {
        lba = (uint64_t)head << 24 | cf->hcyl << 16 | cf->lcyl << 8 |
              cf->sector;
    } else {
        if (!cf->sector || cf->sector > cf->secs || head >= cf->heads ||
            cyl >= cf->cyls) {
            return -1;
        }
        lba = ((uint64_t)cyl * cf->heads + head) * cf->secs +
              cf->sector - 1;
    }
    return lba < cf->nb_sectors ? lba : -1;
}

/* point the task file at a sector, as ATA leaves it after a transfer */
static void j720_cf_set_task_lba(J720CFCard *cf, uint64_t lba)
{
    unsigned cyl, head;

    if (cf->select & ATA_DH_LBA) {
        cf->select = (cf->select & 0xf0) | ((lba >> 24) & 0x0f);
        cf->hcyl = lba >> 16;
        cf->lcyl = lba >> 8;
        cf->sector = lba;
        return;
    }
    cyl = lba / (cf->heads * cf->secs);
    head = (lba / cf->secs) % cf->heads;
    cf->select = (cf->select & 0xf0) | head;
    cf->hcyl = cyl >> 8;
    cf->lcyl = cyl;
    cf->sector = lba % cf->secs + 1;
}

static void j720_cf_done(J720SA1111State *s, uint8_t error)
{
    J720CFCard *cf = &s->cf;

    cf->error = error;
    cf->status = ATA_SR_DRDY | ATA_SR_DSC | (error ? ATA_SR_ERR : 0);
    cf->pos = cf->end = cf->xfer_left = 0;
    j720_cf_set_intrq(s, true);
}

/* PIO in: the next sector into the buffer, DRQ and an interrupt */
static void j720_cf_read_sector(J720SA1111State *s)
{
    J720CFCard *cf = &s->cf;

    if (blk_pread(cf->blk, cf->lba * J720_CF_SECTOR, J720_CF_SECTOR,
                  cf->buf, 0) < 0) {
        j720_cf_done(s, ATA_ER_UNC);
        return;
    }
    j720_cf_set_task_lba(cf, cf->lba);
    cf->lba++;
    cf->nsector--;
    cf->xfer_left--;
    cf->pos = 0;
    cf->end = J720_CF_SECTOR;
    cf->status = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    j720_cf_set_intrq(s, true);
}

/* PIO out: the host has filled the buffer */
static void j720_cf_write_sector(J720SA1111State *s)
{
    J720CFCard *cf = &s->cf;

    if (blk_pwrite(cf->blk, cf->lba * J720_CF_SECTOR, J720_CF_SECTOR,
                   cf->buf, 0) < 0) {
        j720_cf_done(s, ATA_ER_UNC);
        return;
    }
    j720_cf_set_task_lba(cf, cf->lba);
    cf->lba++;
    cf->nsector--;
    if (--cf->xfer_left == 0) {
        j720_cf_done(s, 0);
        return;
    }
    cf->pos = 0;
    cf->status = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    j720_cf_set_intrq(s, true);
}

/* buffer fully read or written by the host */
static void j720_cf_buffer_done(J720SA1111State *s)
{
    J720CFCard *cf = &s->cf;

    switch (cf->cmd) {
    case 0x20: case 0x21: case 0xc4:
        if (cf->xfer_left) {
            j720_cf_read_sector(s);
            return;
        }
        break;
    case 0x30: case 0x31: case 0x38: case 0xc5:
        j720_cf_write_sector(s);
        return;
    }
    cf->pos = cf->end = 0;
    cf->status = ATA_SR_DRDY | ATA_SR_DSC;
}

static void j720_cf_command(J720SA1111State *s, uint8_t cmd)
{
    J720CFCard *cf = &s->cf;
    unsigned count = cf->nsector ? cf->nsector : 256;
    int64_t lba;

    if (cf->select & ATA_DH_DEV) {
        return;                     /* no drive 1 */
    }
    cf->cmd = cmd;
    cf->pos = cf->end = cf->xfer_left = 0;
    j720_cf_set_intrq(s, false);

    switch (cmd) {
    case 0xec:                      /* IDENTIFY DEVICE */
        j720_cf_identify(cf);
        cf->error = 0;
        cf->end = J720_CF_SECTOR;
        cf->status = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        j720_cf_set_intrq(s, true);
        break;
    case 0x20: case 0x21:           /* READ SECTORS */
    case 0xc4:                      /* READ MULTIPLE */
    case 0x30: case 0x31:           /* WRITE SECTORS */
    case 0x38:                      /* CFA WRITE WITHOUT ERASE */
    case 0xc5:                      /* WRITE MULTIPLE */
    case 0x40: case 0x41:           /* READ VERIFY SECTORS */
        lba = j720_cf_task_lba(cf);
        if (lba < 0 || lba + count > cf->nb_sectors) {
            j720_cf_done(s, ATA_ER_IDNF | ATA_ER_ABRT);
            break;
        }
        if (!cf->writable && (cmd == 0x30 || cmd == 0x31 || cmd == 0x38 ||
                              cmd == 0xc5)) {
            j720_cf_done(s, ATA_ER_ABRT);
            break;
        }
        cf->error = 0;
        cf->lba = lba;
        cf->xfer_left = count;
        if (cmd == 0x40 || cmd == 0x41) {
            j720_cf_set_task_lba(cf, lba + count - 1);
            cf->nsector = 0;
            j720_cf_done(s, 0);
        } else if (cmd == 0x30 || cmd == 0x31 || cmd == 0x38 ||
                   cmd == 0xc5) {
            /* the first block of a PIO out command has no interrupt */
            cf->end = J720_CF_SECTOR;
            cf->status = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        } else {
            j720_cf_read_sector(s);
        }
        break;
    case 0xc6:                      /* SET MULTIPLE MODE */
        j720_cf_done(s, cf->nsector > 1 ? ATA_ER_ABRT : 0);
        break;
    case 0x91:                      /* INITIALIZE DEVICE PARAMETERS */
        if (!cf->nsector) {
            j720_cf_done(s, ATA_ER_ABRT);
            break;
        }
        cf->heads = (cf->select & 0x0f) + 1;
        cf->secs = cf->nsector;
        cf->cyls = MAX(1, MIN(cf->nb_sectors / (cf->heads * cf->secs),
                              65535));
        j720_cf_done(s, 0);
        break;
    case 0x90:                      /* EXECUTE DEVICE DIAGNOSTIC */
        j720_cf_ata_reset(s);
        j720_cf_set_intrq(s, true);
        break;
    case 0xe7:                      /* FLUSH CACHE */
        j720_cf_done(s, cf->writable && blk_flush(cf->blk) < 0 ?
                        ATA_ER_ABRT : 0);
        break;
    case 0xe5: case 0x98:           /* CHECK POWER MODE: active */
        cf->nsector = 0xff;
        j720_cf_done(s, 0);
        break;
    case 0x10 ... 0x1f:             /* RECALIBRATE */
    case 0x70:                      /* SEEK */
    case 0x03:                      /* CFA REQUEST EXTENDED ERROR */
    case 0xc0:                      /* CFA ERASE SECTORS */
    case 0xef:                      /* SET FEATURES */
    case 0xe0 ... 0xe4: case 0xe6:  /* standby, idle, sleep */
    case 0x94 ... 0x97: case 0x99:
        j720_cf_done(s, 0);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "j720.cf: ATA command 0x%02x\n", cmd);
        j720_cf_done(s, ATA_ER_ABRT);
        break;
    }
}

static uint32_t j720_cf_data_read(J720SA1111State *s, unsigned size)
{
    J720CFCard *cf = &s->cf;
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < size && cf->pos < cf->end; i++) {
        v |= cf->buf[cf->pos++] << (8 * i);
        if (cf->pos == cf->end) {
            j720_cf_buffer_done(s);
        }
    }
    return v;
}

static void j720_cf_data_write(J720SA1111State *s, uint32_t value,
                               unsigned size)
{
    J720CFCard *cf = &s->cf;
    unsigned i;

    for (i = 0; i < size && cf->pos < cf->end; i++) {
        cf->buf[cf->pos++] = value >> (8 * i);
        if (cf->pos == cf->end) {
            j720_cf_buffer_done(s);
        }
    }
}

/* one 8-bit register of the CF register map, other than data */
static uint8_t j720_cf_reg_read(J720SA1111State *s, int reg)
{
    J720CFCard *cf = &s->cf;

    switch (reg) {
    case 0x1: case 0xd: return cf->error;
    case 0x2: return cf->nsector;
    case 0x3: return cf->sector;
    case 0x4: return cf->lcyl;
    case 0x5: return cf->hcyl;
    case 0x6: return cf->select;
    case 0x7:
        if (cf->select & ATA_DH_DEV) {
            return 0;
        }
        j720_cf_set_intrq(s, false);
        return cf->status;
    case 0xe:
        return cf->select & ATA_DH_DEV ? 0 : cf->status;
    default:
        return 0xff;
    }
}

static void j720_cf_reg_write(J720SA1111State *s, int reg, uint8_t value)
{
    J720CFCard *cf = &s->cf;

    switch (reg) {
    case 0x1: case 0xd: cf->feature = value; break;
    case 0x2: cf->nsector = value; break;
    case 0x3: cf->sector = value; break;
    case 0x4: cf->lcyl = value; break;
    case 0x5: cf->hcyl = value; break;
    case 0x6: cf->select = value; break;
    case 0x7: j720_cf_command(s, value); break;
    case 0xe:
        if ((cf->devctl & ATA_DC_SRST) && !(value & ATA_DC_SRST)) {
            j720_cf_ata_reset(s);
        } else if (value & ATA_DC_SRST) {
            cf->status = ATA_SR_BSY;
        }
        cf->devctl = value;
        j720_cf_update(s);
        break;
    }
}

/*
 * Data at 0 and 8 (odd byte at 9); a 16-bit access elsewhere covers two
 * adjacent registers.
 */
static uint64_t j720_cf_access_read(J720SA1111State *s, int reg,
                                    unsigned size)
{
    if (reg == 0 || reg == 8 || (reg == 9 && size == 1)) {
        return j720_cf_data_read(s, size);
    }
    if (size == 2) {
        return j720_cf_reg_read(s, reg) | j720_cf_reg_read(s, reg + 1) << 8;
    }
    return j720_cf_reg_read(s, reg);
}

static void j720_cf_access_write(J720SA1111State *s, int reg,
                                 uint64_t value, unsigned size)
{
    if (reg == 0 || reg == 8 || (reg == 9 && size == 1)) {
        j720_cf_data_write(s, value, size);
        return;
    }
    if (size == 2) {
        j720_cf_reg_write(s, reg, value);
        j720_cf_reg_write(s, reg + 1, value >> 8);
        return;
    }
    j720_cf_reg_write(s, reg, value);
}

/* the register an I/O address selects in the current configuration */
static int j720_cf_io_reg(J720CFCard *cf, hwaddr addr)
{
    switch (cf->cor & COR_INDEX_MASK) {
    case 0:
        return -1;                  /* memory mapped */
    case 1:
        return addr & 0xf;
    default:                        /* 0x1f0-0x1f7/0x3f6-0x3f7 and 0x17x */
        if ((addr & 0x20e) == 0x206) {
            return 0xe + (addr & 1);
        }
        return addr & 0x7;
    }
}

static uint64_t j720_cf_io_read(void *opaque, hwaddr addr, unsigned size)
{
    J720SA1111State *s = opaque;
    int reg = s->cf.inserted ? j720_cf_io_reg(&s->cf, addr) : -1;

    return reg < 0 ? (1ULL << (size * 8)) - 1
                   : j720_cf_access_read(s, reg, size);
}

static void j720_cf_io_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    J720SA1111State *s = opaque;
    int reg = s->cf.inserted ? j720_cf_io_reg(&s->cf, addr) : -1;

    if (reg >= 0) {
        j720_cf_access_write(s, reg, value, size);
    }
}

/* common memory: the registers in configuration 0, data at 0x400-0x7ff */
static int j720_cf_mem_reg(J720CFCard *cf, hwaddr addr)
{
    if (!cf->inserted || (cf->cor & COR_INDEX_MASK)) {
        return -1;
    }
    addr &= 0x7ff;
    return addr >= 0x400 ? (addr & 1 ? 9 : 8) : addr & 0xf;
}

static uint64_t j720_cf_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    J720SA1111State *s = opaque;
    int reg = j720_cf_mem_reg(&s->cf, addr);

    return reg < 0 ? (1ULL << (size * 8)) - 1
                   : j720_cf_access_read(s, reg, size);
}

static void j720_cf_mem_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    J720SA1111State *s = opaque;
    int reg = j720_cf_mem_reg(&s->cf, addr);

    if (reg >= 0) {
        j720_cf_access_write(s, reg, value, size);
    }
}

/*
 * CIS of a CF storage card: fixed disk, ATA interface; configuration
 * registers at 0x200; four configurations (see "CF card" above).
 */
static const uint8_t j720_cf_cis[] = {
    0x01, 0x03, 0xd9, 0x01, 0xff,               /* DEVICE: 2 KiB */
    0x15, 0x1a, 0x04, 0x01,                     /* VERS_1 4.1 */
    'Q', 'E', 'M', 'U', 0,
    'C', 'o', 'm', 'p', 'a', 'c', 't', 'F', 'l', 'a', 's', 'h', ' ',
    'C', 'a', 'r', 'd', 0, 0xff,
    0x21, 0x02, 0x04, 0x01,                     /* FUNCID: fixed disk */
    0x22, 0x02, 0x01, 0x01,                     /* FUNCE: ATA interface */
    0x22, 0x03, 0x02, 0x0c, 0x0f,               /* FUNCE: ATA features */
    0x1a, 0x05, 0x01, 0x03, 0x00, 0x02, 0x0f,   /* CONFIG: 0x200, last 3 */
    /*
     * Each entry: Vcc 3.3 V (ATADISK.DLL picks an entry by nominal Vcc),
     * then the interface it describes.
     * 0: memory mapped, 2 KiB; default
     */
    0x1b, 0x08, 0xc0, 0xc0, 0x21, 0x01, 0xb5, 0x1e, 0x08, 0x00,
    /* 1: 16 I/O ports anywhere, any IRQ */
    0x1b, 0x0a, 0x81, 0x41, 0x19, 0x01, 0xb5, 0x1e, 0x64, 0x30, 0xff, 0xff,
    /* 2: 0x1f0-0x1f7 and 0x3f6-0x3f7, IRQ 14 */
    0x1b, 0x0f, 0x82, 0x41, 0x19, 0x01, 0xb5, 0x1e, 0xea, 0x61,
    0xf0, 0x01, 0x07, 0xf6, 0x03, 0x01, 0x2e,
    /* 3: 0x170-0x177 and 0x376-0x377, IRQ 15 */
    0x1b, 0x0f, 0x83, 0x41, 0x19, 0x01, 0xb5, 0x1e, 0xea, 0x61,
    0x70, 0x01, 0x07, 0x76, 0x03, 0x01, 0x2f,
    0x14, 0x00,                                 /* NO_LINK */
    0xff,                                       /* END */
};

static uint64_t j720_cf_attr_read(void *opaque, hwaddr addr, unsigned size)
{
    J720SA1111State *s = opaque;
    uint32_t off = addr & ~1;
    uint8_t b = 0xff;

    if (!s->cf.inserted || (addr & 1)) {
        return (1ULL << (size * 8)) - 1;
    }
    if (off / 2 < sizeof(j720_cf_cis)) {
        b = j720_cf_cis[off / 2];
    } else if (off == J720_CF_COR) {
        b = s->cf.cor;
    } else if (off == J720_CF_CCSR) {
        b = s->cf.ccsr;
    }
    return size == 2 ? b | 0xff00 : b;
}

static void j720_cf_attr_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    J720SA1111State *s = opaque;
    J720CFCard *cf = &s->cf;

    if (!cf->inserted) {
        return;
    }
    switch (addr) {
    case J720_CF_COR:
        if (value & COR_SRESET) {
            j720_cf_reset(s);
            break;
        }
        cf->cor = value;
        j720_cf_update(s);
        break;
    case J720_CF_CCSR:
        cf->ccsr = (cf->ccsr & CCSR_INTR) | (value & ~CCSR_INTR);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps j720_cf_io_ops = {
    .read = j720_cf_io_read,
    .write = j720_cf_io_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps j720_cf_mem_ops = {
    .read = j720_cf_mem_read,
    .write = j720_cf_mem_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps j720_cf_attr_ops = {
    .read = j720_cf_attr_read,
    .write = j720_cf_attr_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t j720_sa1111_intc_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    J720SA1111State *s = opaque;

    switch (addr) {
    case SA1111_INTEN0:      return s->inten[0];
    case SA1111_INTEN1:      return s->inten[1];
    case SA1111_INTPOL0:     return s->intpol[0];
    case SA1111_INTPOL1:     return s->intpol[1];
    case SA1111_INTSTATCLR0: return s->intstat[0];
    case SA1111_INTSTATCLR1: return s->intstat[1];
    case SA1111_WAKEEN0:     return s->wakeen[0];
    case SA1111_WAKEEN1:     return s->wakeen[1];
    case SA1111_WAKEPOL0:    return s->wakepol[0];
    case SA1111_WAKEPOL1:    return s->wakepol[1];
    default:
        qemu_log_mask(LOG_UNIMP, "j720.sa1111-intc: read 0x%02" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void j720_sa1111_intc_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    J720SA1111State *s = opaque;

    switch (addr) {
    case SA1111_INTEN0:      s->inten[0] = value; break;
    case SA1111_INTEN1:      s->inten[1] = value; break;
    case SA1111_INTPOL0:     j720_sa1111_set_intpol(s, 0, value); break;
    case SA1111_INTPOL1:     j720_sa1111_set_intpol(s, 1, value); break;
    case SA1111_INTSTATCLR0: s->intstat[0] &= ~value; break;
    case SA1111_INTSTATCLR1: s->intstat[1] &= ~value; break;
    case SA1111_INTSET0:     s->intstat[0] |= value; break;
    case SA1111_INTSET1:     s->intstat[1] |= value; break;
    case SA1111_WAKEEN0:     s->wakeen[0] = value; break;
    case SA1111_WAKEEN1:     s->wakeen[1] = value; break;
    case SA1111_WAKEPOL0:    s->wakepol[0] = value; break;
    case SA1111_WAKEPOL1:    s->wakepol[1] = value; break;
    default:
        qemu_log_mask(LOG_UNIMP, "j720.sa1111-intc: write 0x%02" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n", addr, value);
        break;
    }
    j720_sa1111_update(s);
}

static const MemoryRegionOps j720_sa1111_intc_ops = {
    .read = j720_sa1111_intc_read,
    .write = j720_sa1111_intc_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t j720_sa1111_pcmcia_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    J720SA1111State *s = opaque;

    switch (addr) {
    case SA1111_PCCR:
        return s->pccr;
    case SA1111_PCSSR:
        return s->pcssr;
    case SA1111_PCSR:
        /* socket 0: 3.3 V card present; socket 1: 3.3 V card or empty */
        return (j720_card_ready(s) ? PCSR_S0_READY : 0) | PCSR_S0_VS2 |
               PCSR_S0_BVD1 | PCSR_S0_BVD2 |
               (j720_cf_ready(s) ? PCSR_S1_READY : 0) |
               (s->cf.inserted ? 0 : PCSR_S1_DETECT | PCSR_S1_VS1) |
               PCSR_S1_VS2 | PCSR_S1_BVD1 | PCSR_S1_BVD2;
    default:
        qemu_log_mask(LOG_UNIMP, "j720.sa1111-pcmcia: read 0x%02" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void j720_sa1111_pcmcia_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size)
{
    J720SA1111State *s = opaque;

    switch (addr) {
    case SA1111_PCCR:
        if ((value & PCCR_S0_RST) && !(s->pccr & PCCR_S0_RST)) {
            s->cor = 0;
            ne2000_reset(&s->ne2000);
        }
        if ((value & PCCR_S1_RST) && !(s->pccr & PCCR_S1_RST)) {
            j720_cf_reset(s);
        }
        s->pccr = value;
        j720_card_update(s);
        j720_cf_update(s);
        break;
    case SA1111_PCSSR:
        s->pcssr = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "j720.sa1111-pcmcia: write 0x%02"
                      HWADDR_PRIx " value 0x%" PRIx64 "\n", addr, value);
        break;
    }
}

static const MemoryRegionOps j720_sa1111_pcmcia_ops = {
    .read = j720_sa1111_pcmcia_read,
    .write = j720_sa1111_pcmcia_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* Attribute memory: CIS on even bytes, then COR and CCSR */
static uint64_t j720_card_attr_read(void *opaque, hwaddr addr, unsigned size)
{
    J720SA1111State *s = opaque;
    uint32_t off = addr & ~1;
    uint8_t b = 0xff;

    if (off / 2 < s->cis_len) {
        b = s->cis[off / 2];
    } else if (off == J720_CARD_COR) {
        b = s->cor;
    } else if (off == J720_CARD_CCSR) {
        b = s->ccsr;
    }
    if (addr & 1) {
        return 0xff;    /* odd bytes of attribute memory are undefined */
    }
    return size == 2 ? b | 0xff00 : b;
}

static void j720_card_attr_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    J720SA1111State *s = opaque;

    switch (addr) {
    case J720_CARD_COR:
        if (value & COR_SRESET) {
            ne2000_reset(&s->ne2000);
            value = 0;
        }
        s->cor = value;
        j720_card_update(s);
        break;
    case J720_CARD_CCSR:
        s->ccsr = (s->ccsr & CCSR_INTR) | (value & ~CCSR_INTR);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps j720_card_attr_ops = {
    .read = j720_card_attr_read,
    .write = j720_card_attr_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * I/O space: the card decodes 5 address lines (32 NE2000 ports). A
 * 16-bit access outside the data port reads/writes two adjacent 8-bit
 * registers, as a 16-bit PC Card does.
 */
static uint64_t j720_card_io_read(void *opaque, hwaddr addr, unsigned size)
{
    J720SA1111State *s = opaque;
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    uint64_t v = 0, b;
    unsigned port = addr & 0x1f, i;

    if (!(s->cor & COR_INDEX_MASK)) {
        return (1ULL << (size * 8)) - 1;
    }
    if (port == 0x10 || size == 1) {
        memory_region_dispatch_read(&s->ne2000.io, port, &v,
                                    size_memop(size), attrs);
        return v;
    }
    for (i = 0; i < size; i++) {
        memory_region_dispatch_read(&s->ne2000.io, (port + i) & 0x1f, &b,
                                    MO_8, attrs);
        v |= b << (8 * i);
    }
    return v;
}

static void j720_card_io_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    J720SA1111State *s = opaque;
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    unsigned port = addr & 0x1f, i;

    if (!(s->cor & COR_INDEX_MASK)) {
        return;
    }
    if (port == 0x10 || size == 1) {
        memory_region_dispatch_write(&s->ne2000.io, port, value,
                                     size_memop(size), attrs);
        return;
    }
    for (i = 0; i < size; i++) {
        memory_region_dispatch_write(&s->ne2000.io, (port + i) & 0x1f,
                                     (value >> (8 * i)) & 0xff, MO_8, attrs);
    }
}

static const MemoryRegionOps j720_card_io_ops = {
    .read = j720_card_io_read,
    .write = j720_card_io_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void j720_card_build_cis(J720SA1111State *s)
{
    static const char vers[] = "QEMU\0NE2000 Compatible PC Card\0";
    uint8_t *p = s->cis;
    int i;

    /* CISTPL_DEVICE: no common memory */
    *p++ = 0x01; *p++ = 0x02; *p++ = 0x00; *p++ = 0xff;
    /* CISTPL_VERS_1 4.1 */
    *p++ = 0x15; *p++ = 2 + sizeof(vers) - 1 + 1;
    *p++ = 0x04; *p++ = 0x01;
    memcpy(p, vers, sizeof(vers) - 1);
    p += sizeof(vers) - 1;
    *p++ = 0xff;
    /* CISTPL_FUNCID: network adapter */
    *p++ = 0x21; *p++ = 0x02; *p++ = 0x06; *p++ = 0x00;
    /* CISTPL_FUNCE: LAN node ID */
    *p++ = 0x22; *p++ = 0x08; *p++ = 0x04; *p++ = 0x06;
    for (i = 0; i < 6; i++) {
        *p++ = s->ne2000.c.macaddr.a[i];
    }
    /* CISTPL_CONFIG: 2-byte register base 0x3f8, COR+CCSR present */
    *p++ = 0x1a; *p++ = 0x05; *p++ = 0x01; *p++ = J720_CARD_CFG_INDEX;
    *p++ = J720_CARD_COR & 0xff; *p++ = J720_CARD_COR >> 8; *p++ = 0x03;
    /*
     * CISTPL_CFTABLE_ENTRY: index 0x20 (default, interface byte), I/O
     * interface; features: I/O space and IRQ; 8/16-bit I/O, 5 address
     * lines, one range 0x300 + 32; level IRQ, any of 0-15.
     */
    *p++ = 0x1b; *p++ = 0x0b;
    *p++ = 0xc0 | J720_CARD_CFG_INDEX; *p++ = 0x01; *p++ = 0x18;
    *p++ = 0xe5; *p++ = 0x60; *p++ = 0x00; *p++ = 0x03; *p++ = 0x1f;
    *p++ = 0x30; *p++ = 0xff; *p++ = 0xff;
    /* CISTPL_END */
    *p++ = 0xff;
    s->cis_len = p - s->cis;
    assert(s->cis_len <= sizeof(s->cis));
}

static NetClientInfo j720_ne2000_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = ne2000_receive,
};

static void j720_sa1111_realize(DeviceState *dev, Error **errp)
{
    J720SA1111State *s = J720_SA1111(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    NE2000State *ne = &s->ne2000;

    memory_region_init_io(&s->intc_io, OBJECT(dev), &j720_sa1111_intc_ops, s,
                          "j720.sa1111-intc", 0x100);
    memory_region_init_io(&s->pcmcia_io, OBJECT(dev), &j720_sa1111_pcmcia_ops,
                          s, "j720.sa1111-pcmcia", 0x100);
    memory_region_init_io(&s->card_io, OBJECT(dev), &j720_card_io_ops, s,
                          "j720.pcmcia0-io", J720_PCMCIA_WINDOW);
    memory_region_init_io(&s->card_attr, OBJECT(dev), &j720_card_attr_ops, s,
                          "j720.pcmcia0-attr", J720_PCMCIA_WINDOW);
    sysbus_init_mmio(sbd, &s->intc_io);
    sysbus_init_mmio(sbd, &s->pcmcia_io);
    sysbus_init_mmio(sbd, &s->card_io);
    sysbus_init_mmio(sbd, &s->card_attr);
    memory_region_init_io(&s->cf_io, OBJECT(dev), &j720_cf_io_ops, s,
                          "j720.pcmcia1-io", J720_PCMCIA_WINDOW);
    memory_region_init_io(&s->cf_attr, OBJECT(dev), &j720_cf_attr_ops, s,
                          "j720.pcmcia1-attr", J720_PCMCIA_WINDOW);
    memory_region_init_io(&s->cf_mem, OBJECT(dev), &j720_cf_mem_ops, s,
                          "j720.pcmcia1-mem", J720_PCMCIA_WINDOW);
    sysbus_init_mmio(sbd, &s->cf_io);
    sysbus_init_mmio(sbd, &s->cf_attr);
    sysbus_init_mmio(sbd, &s->cf_mem);
    sysbus_init_irq(sbd, &s->irq);

    ne2000_setup_io(ne, dev, 0x20);
    /*
     * The I/O window forwards into this region from its own handler; the
     * guard against re-entrant I/O on one device would drop every access.
     */
    ne->io.disable_reentrancy_guard = true;
    ne->irq = qemu_allocate_irq(j720_card_ne2000_irq, s, 0);
    qemu_macaddr_default_if_unset(&ne->c.macaddr);
    ne2000_reset(ne);
    ne->nic = qemu_new_nic(&j720_ne2000_info, &ne->c,
                           object_get_typename(OBJECT(dev)), dev->id,
                           &dev->mem_reentrancy_guard, ne);
    qemu_format_nic_info_str(qemu_get_queue(ne->nic), ne->c.macaddr.a);
    j720_card_build_cis(s);

    /* socket 0: card present and ready; socket 1 empty for now */
    s->intin[1] = 1u << SA1111_IRQ_S0_READY_NINT |
                  1u << SA1111_IRQ_S1_CD_VALID;

    if (s->cf.blk) {
        if (blk_attach_dev(s->cf.blk, dev) < 0) {
            error_setg(errp, "CF card drive is already in use");
            return;
        }
        j720_cf_set_perm(&s->cf);
        blk_set_dev_ops(s->cf.blk, &j720_cf_block_ops, s);
        s->cf.resume_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                          j720_cf_resume_timer, s);
        if (blk_is_inserted(s->cf.blk)) {
            j720_cf_set_inserted(s, true);
        }
    }
    j720_cf_reset(s);
}

/* a state saved before the CF card existed has the slot empty */
static int j720_sa1111_pre_load(void *opaque)
{
    J720SA1111State *s = opaque;

    s->cf.inserted = false;
    return 0;
}

static int j720_sa1111_post_load(void *opaque, int version_id)
{
    J720SA1111State *s = opaque;
    J720CFCard *cf = &s->cf;
    bool medium = cf->blk && blk_is_inserted(cf->blk);

    /* card detect is active low; older states have both inputs low */
    s->intin[1] &= ~(1u << SA1111_IRQ_S0_CD_VALID);
    s->intin[1] = deposit32(s->intin[1], SA1111_IRQ_S1_CD_VALID, 1,
                            !cf->inserted);
    /*
     * The CF medium is whatever this run was started with, and it may
     * have been changed on the host meanwhile. So CE sees the card in the
     * slot pulled out and the medium, if any, put in afresh: it drops
     * what it cached of the old one. Once the machine runs, not while
     * devices loaded after this one could still overwrite the interrupt.
     */
    if (cf->inserted || medium) {
        timer_mod(cf->resume_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
    if (medium) {
        cf->nb_sectors = MAX(blk_getlength(cf->blk), 0) / J720_CF_SECTOR;
    }
    j720_sa1111_update(s);
    return 0;
}

static bool j720_cf_needed(void *opaque)
{
    return true;
}

/* a subsection, so states saved before the CF card still load */
static const VMStateDescription vmstate_j720_cf = {
    .name = TYPE_J720_SA1111 "/cf",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = j720_cf_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(cf.inserted, J720SA1111State),
        VMSTATE_UINT16(cf.cyls, J720SA1111State),
        VMSTATE_UINT16(cf.heads, J720SA1111State),
        VMSTATE_UINT16(cf.secs, J720SA1111State),
        VMSTATE_UINT8(cf.cor, J720SA1111State),
        VMSTATE_UINT8(cf.ccsr, J720SA1111State),
        VMSTATE_BOOL(cf.intrq, J720SA1111State),
        VMSTATE_UINT8(cf.feature, J720SA1111State),
        VMSTATE_UINT8(cf.error, J720SA1111State),
        VMSTATE_UINT8(cf.nsector, J720SA1111State),
        VMSTATE_UINT8(cf.sector, J720SA1111State),
        VMSTATE_UINT8(cf.lcyl, J720SA1111State),
        VMSTATE_UINT8(cf.hcyl, J720SA1111State),
        VMSTATE_UINT8(cf.select, J720SA1111State),
        VMSTATE_UINT8(cf.status, J720SA1111State),
        VMSTATE_UINT8(cf.devctl, J720SA1111State),
        VMSTATE_UINT8(cf.cmd, J720SA1111State),
        VMSTATE_UINT8_ARRAY(cf.buf, J720SA1111State, J720_CF_SECTOR),
        VMSTATE_UINT32(cf.pos, J720SA1111State),
        VMSTATE_UINT32(cf.end, J720SA1111State),
        VMSTATE_UINT32(cf.xfer_left, J720SA1111State),
        VMSTATE_UINT64(cf.lba, J720SA1111State),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_j720_sa1111 = {
    .name = TYPE_J720_SA1111,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = j720_sa1111_pre_load,
    .post_load = j720_sa1111_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(inten, J720SA1111State, 2),
        VMSTATE_UINT32_ARRAY(intpol, J720SA1111State, 2),
        VMSTATE_UINT32_ARRAY(intstat, J720SA1111State, 2),
        VMSTATE_UINT32_ARRAY(wakeen, J720SA1111State, 2),
        VMSTATE_UINT32_ARRAY(wakepol, J720SA1111State, 2),
        VMSTATE_UINT32_ARRAY(intin, J720SA1111State, 2),
        VMSTATE_UINT32(pccr, J720SA1111State),
        VMSTATE_UINT32(pcssr, J720SA1111State),
        VMSTATE_UINT8(cor, J720SA1111State),
        VMSTATE_UINT8(ccsr, J720SA1111State),
        VMSTATE_BOOL(card_irq, J720SA1111State),
        VMSTATE_STRUCT(ne2000, J720SA1111State, 0, vmstate_ne2000,
                       NE2000State),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_j720_cf,
        NULL
    }
};

static const Property j720_sa1111_properties[] = {
    DEFINE_NIC_PROPERTIES(J720SA1111State, ne2000.c),
};

static void j720_sa1111_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = j720_sa1111_realize;
    dc->vmsd = &vmstate_j720_sa1111;
    device_class_set_props(dc, j720_sa1111_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo j720_sa1111_typeinfo = {
    .name = TYPE_J720_SA1111,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(J720SA1111State),
    .class_init = j720_sa1111_class_init,
};

#define TYPE_JORNADA720_MACHINE MACHINE_TYPE_NAME("jornada720")
OBJECT_DECLARE_SIMPLE_TYPE(Jornada720MachineState, JORNADA720_MACHINE)

struct Jornada720MachineState {
    MachineState parent;

    StrongARMState *sa1110;
    J720Display display;
    uint32_t udc_reg[J720_SA1110_UDC_SIZE / 4];
    J720Power power;
};

/*
 * State of the board-level models above (the frame buffer RAM and the
 * SA-1110 devices save themselves), so run.sh can suspend the machine
 * to a file and resume it: CE keeps everything, registry included, in
 * RAM, like the real battery-backed device.
 */
static int j720_display_post_load(void *opaque, int version_id)
{
    J720Display *d = opaque;

    d->invalidate = true;
    d->blt_dirty_lo = INT_MAX;
    d->blt_dirty_hi = -1;
    return 0;
}

static const VMStateDescription vmstate_j720_display = {
    .name = "j720-epson",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = j720_display_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, J720Display, J720_EPSON_NREGS),
        VMSTATE_INT32(blt_op, J720Display),
        VMSTATE_UINT32(blt_x, J720Display),
        VMSTATE_UINT32(blt_y, J720Display),
        VMSTATE_UINT32(blt_w, J720Display),
        VMSTATE_UINT32(blt_h, J720Display),
        VMSTATE_UINT32(blt_bit, J720Display),
        VMSTATE_END_OF_LIST()
    }
};

static bool j720_power_needed(void *opaque)
{
    return true;    /* a subsection, so states saved before it still load */
}

static const VMStateDescription vmstate_j720_power = {
    .name = "jornada720/power",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = j720_power_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(power.pmcr, Jornada720MachineState),
        VMSTATE_UINT32(power.pssr, Jornada720MachineState),
        VMSTATE_UINT32(power.pspr, Jornada720MachineState),
        VMSTATE_UINT32(power.pwer, Jornada720MachineState),
        VMSTATE_UINT32(power.pcfr, Jornada720MachineState),
        VMSTATE_UINT32(power.ppcr, Jornada720MachineState),
        VMSTATE_UINT32(power.pgsr, Jornada720MachineState),
        VMSTATE_UINT32(power.rcsr, Jornada720MachineState),
        VMSTATE_BOOL(power.sleeping, Jornada720MachineState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_jornada720 = {
    .name = "jornada720",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(display, Jornada720MachineState, 1,
                       vmstate_j720_display, J720Display),
        VMSTATE_UINT32_ARRAY(udc_reg, Jornada720MachineState,
                             J720_SA1110_UDC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_j720_power,
        NULL
    }
};

/* Leave sleep: a sleep-mode reset of the CPU alone (see j720_pm_write) */
static void jornada720_wake(Jornada720MachineState *jms)
{
    J720Power *p = &jms->power;

    p->sleeping = false;
    p->pmcr = 0;
    p->pssr |= PSSR_SSS | PSSR_DH | PSSR_PH;
    p->rcsr = RCSR_SMR;
    cpu_reset(CPU(jms->sa1110->cpu));
}

static void jornada720_wakeup(MachineState *machine)
{
    jornada720_wake(JORNADA720_MACHINE(machine));
}

/*
 * A state saved while CE slept loads as a running VM; wake it before the
 * CPU runs, as if the power button had been pressed on start.
 */
static void jornada720_vm_state_change(void *opaque, bool running,
                                       RunState state)
{
    Jornada720MachineState *jms = opaque;

    if (running && jms->power.sleeping) {
        jornada720_wake(jms);
    }
}

static void jornada720_reset(void *opaque)
{
    J720Power *p = opaque;

    p->rcsr = p->soft_reset ? RCSR_SWR : RCSR_HWR;
    p->soft_reset = false;
    p->sleeping = false;
    p->pmcr = p->pssr = 0;
}

static void jornada720_init(MachineState *machine)
{
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    Jornada720MachineState *jms = JORNADA720_MACHINE(machine);
    DriveInfo *dinfo;

    if (machine->ram_size != mc->default_ram_size) {
        char *sz = size_to_str(mc->default_ram_size);
        error_report("Invalid RAM size, should be %s", sz);
        g_free(sz);
        exit(EXIT_FAILURE);
    }

    jms->sa1110 = sa1110_init(machine->cpu_type);

    memory_region_add_subregion(get_system_memory(), SA_SDCS0, machine->ram);

    dinfo = drive_get(IF_PFLASH, 0, 0);
    pflash_cfi01_register(SA_CS0, "jornada720.rom", J720_FLASH_SIZE,
                           dinfo ? blk_by_legacy_dinfo(dinfo) : NULL,
                           J720_FLASH_SECTOR_SIZE, 4, 0x00, 0x00, 0x00, 0x00, 0);

    /*
     * Stubs so boot-code probes of not-yet-emulated hardware show up in
     * -d unimp logs (address, size, read/write) instead of either silently
     * reading -1 forever (ignore_memory_transaction_failures=true) or
     * raising a real Data Abort the ROM's exception handling can't yet
     * deal with (=false). This is how real unpopulated chip-select space
     * behaves anyway: reads something, doesn't fault the bus.
     */
    create_unimplemented_device("j720.sa1111", J720_SA1111_BASE,
                                 J720_SA1111_SIZE);
    create_unimplemented_device("j720.debugboard", J720_DEBUGBOARD_BASE,
                                 J720_DEBUGBOARD_SIZE);
    create_unimplemented_device("j720.sa1110-pm", J720_SA1110_PM_BASE,
                                 J720_SA1110_PM_SIZE);

    {
        MemoryRegion *ssp_stub = g_new(MemoryRegion, 1);
        memory_region_init_io(ssp_stub, NULL, &j720_ssp_stub_ops, NULL,
                               "j720.sa1111-ssp-stub",
                               J720_SA1111_SSP_STUB_SIZE);
        memory_region_add_subregion_overlap(get_system_memory(),
                                             J720_SA1111_SSP_STUB_BASE,
                                             ssp_stub, 1);
    }

    {
        DeviceState *dev = qdev_new(TYPE_J720_SA1111);
        SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
        BlockBackend *cf = blk_by_name("cf");

        /*
         * The CF slot's medium: -drive if=none,id=cf,file=...,format=raw,
         * or an empty drive "cf" for the monitor's change command.
         */
        if (!cf) {
            cf = blk_by_legacy_dinfo(drive_new(drive_add(IF_NONE, -1, NULL,
                                                         "id=cf"),
                                               IF_NONE, &error_fatal));
        }
        J720_SA1111(dev)->cf.blk = cf;
        qemu_configure_nic_device(dev, true, NULL);
        sysbus_realize_and_unref(sbd, &error_fatal);
        memory_region_add_subregion_overlap(get_system_memory(),
                J720_SA1111_INTC_BASE, sysbus_mmio_get_region(sbd, 0), 1);
        memory_region_add_subregion_overlap(get_system_memory(),
                J720_SA1111_PCMCIA_BASE, sysbus_mmio_get_region(sbd, 1), 1);
        sysbus_mmio_map(sbd, 2, J720_PCMCIA_S0_IO);
        sysbus_mmio_map(sbd, 3, J720_PCMCIA_S0_ATTR);
        sysbus_mmio_map(sbd, 4, J720_PCMCIA_S1_IO);
        sysbus_mmio_map(sbd, 5, J720_PCMCIA_S1_ATTR);
        sysbus_mmio_map(sbd, 6, J720_PCMCIA_S1_MEM);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(jms->sa1110->gpio,
                                                    J720_GPIO_SA1111_IRQ));
    }

    {
        MemoryRegion *udc = g_new(MemoryRegion, 1);
        memory_region_init_io(udc, NULL, &j720_udc_ops, jms->udc_reg,
                              "j720.sa1110-udc", J720_SA1110_UDC_SIZE);
        memory_region_add_subregion(get_system_memory(), J720_SA1110_UDC_BASE,
                                    udc);
    }

    {
        MemoryRegion *pm = g_new(MemoryRegion, 1);
        MemoryRegion *rstc = g_new(MemoryRegion, 1);

        memory_region_init_io(pm, NULL, &j720_pm_ops, &jms->power,
                              "j720.sa1110-pm", 0x20);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            J720_SA1110_PM_BASE, pm, 1);
        memory_region_init_io(rstc, NULL, &j720_rstc_ops, &jms->power,
                              "j720.sa1110-rstc", 0x8);
        memory_region_add_subregion(get_system_memory(),
                                    J720_SA1110_RSTC_BASE, rstc);
        jms->power.rcsr = RCSR_HWR;
        qemu_register_reset(jornada720_reset, &jms->power);
        qemu_register_wakeup_support();
        qemu_system_wakeup_enable(QEMU_WAKEUP_REASON_OTHER, true);
        qemu_add_vm_change_state_handler(jornada720_vm_state_change, jms);
    }

    {
        J720MCUState *mcu = J720_MCU(ssi_create_peripheral(
                                jms->sa1110->ssp_bus, TYPE_J720_MCU));
        mcu->kbd_irq = qdev_get_gpio_in(jms->sa1110->gpio, J720_GPIO_KBD_IRQ);
        mcu->ts_irq = qdev_get_gpio_in(jms->sa1110->gpio, J720_GPIO_TS_IRQ);
        qemu_irq_raise(mcu->kbd_irq);   /* no key codes waiting */
        qemu_irq_raise(mcu->ts_irq);    /* pen up */
    }

    jms->display.blt_op = -1;
    memory_region_init_ram(&jms->display.fb, NULL, "j720.epson-fb",
                           J720_EPSON_FB_SIZE, &error_fatal);
    memory_region_set_log(&jms->display.fb, true, DIRTY_MEMORY_VGA);
    jms->display.blt_dirty_lo = INT_MAX;
    jms->display.blt_dirty_hi = -1;
    memory_region_add_subregion(get_system_memory(), J720_EPSON_FB_BASE,
                                &jms->display.fb);
    memory_region_init_io(&jms->display.regs, NULL, &j720_epson_regs_ops,
                          &jms->display, "j720.epson-regs",
                          J720_EPSON_REGS_SIZE);
    memory_region_add_subregion(get_system_memory(), J720_EPSON_REGS_BASE,
                                &jms->display.regs);
    jms->display.con = qemu_graphic_console_create(NULL, 0, &j720_display_ops,
                                                   &jms->display);
    vmstate_register(NULL, 0, &vmstate_jornada720, jms);

    /*
     * No arm_load_kernel() call here on purpose: we are not booting a
     * Linux zImage via ATAGS. The Windows CE ROM contains its own boot
     * code and is expected to run straight from the reset vector inside
     * the flash mapped above, same as on real hardware.
     */
}

static void jornada720_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "HP Jornada 720 Handheld PC (SA-1110)";
    mc->init = jornada720_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("sa1110");
    mc->default_ram_size = J720_RAM_SIZE;
    mc->default_ram_id = "strongarm.sdram";
    mc->wakeup = jornada720_wakeup;
}

static const TypeInfo jornada720_machine_typeinfo = {
    .name = TYPE_JORNADA720_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = jornada720_machine_class_init,
    .instance_size = sizeof(Jornada720MachineState),
};

static void jornada720_machine_register_types(void)
{
    type_register_static(&j720_mcu_typeinfo);
    type_register_static(&j720_sa1111_typeinfo);
    type_register_static(&jornada720_machine_typeinfo);
}
type_init(jornada720_machine_register_types);
