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
#include "migration/vmstate.h"
#include "standard-headers/linux/input-event-codes.h"
#include "system/address-spaces.h"
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
 * SA-1110 on-chip Power Manager block, 0x90020000 -- entirely missing from
 * hw/arm/strongarm.c (confirmed: no reference to it anywhere in that file).
 * Found by tracing where the ROM's OAL-style delay/poll loop (two
 * back-to-back ~100ms stalls, then re-check) reads its status word from:
 * the kernel's uncached alias 0xa3420000 resolves (via the page table
 * built at this point in boot, TTBR0 read live through the QEMU gdbstub)
 * to physical section base 0x90020000, and the polled word is at offset
 * 0x1c within it. Matches the real SA-1110 Power Manager register map
 * (PMCR=0x00, PSSR=0x04, PSPR=0x08, PWER=0x0c, PCFR=0x10, PPCR=0x14,
 * PGSR=0x18, POSR=0x1c) -- offset 0x1c is POSR, the Oscillator Status
 * Register, bit0 = "3.6864 MHz oscillator stable". Real hardware sets
 * this shortly after reset; our emulation never did, so the boot code's
 * "wait for oscillator" loop spun forever. Only bit0 of POSR is modeled;
 * everything else in this block still falls through to the
 * unimplemented-device stub below, so any further probes stay visible
 * via -d unimp. See docs/research.md.
 */
#define J720_SA1110_PM_BASE 0x90020000
#define J720_SA1110_PM_SIZE (4 * KiB)
#define J720_SA1110_PM_POSR_OFFSET 0x1c
#define J720_SA1110_PM_POSR_BASE (J720_SA1110_PM_BASE + J720_SA1110_PM_POSR_OFFSET)

static uint64_t j720_pm_posr_stub_read(void *opaque, hwaddr addr, unsigned size)
{
    return 1; /* POSR bit0 (OOK): oscillator stable */
}

static void j720_pm_stub_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
}

static const MemoryRegionOps j720_pm_posr_stub_ops = {
    .read = j720_pm_posr_stub_read,
    .write = j720_pm_stub_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

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
#define J720_MCU_BATT_MAIN   0x2a0
#define J720_MCU_BATT_BACKUP 0x3c0
#define J720_MCU_TS_PERIOD_MS 10
/*
 * Under -icount a quick finger tap can be over before CE has read a
 * single sample; keep the pen down until it has read this many.
 */
#define J720_MCU_TS_MIN_SAMPLES 3

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

    bool pen_down;
    bool pen_up_pending;        /* released before enough samples were read */
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

static bool j720_mcu_key_queue(J720MCUState *s, uint8_t b)
{
    if (s->keyq_len == J720_MCU_KEYQ) {
        return false;
    }
    s->keyq[s->keyq_len++] = b;
    qemu_irq_lower(s->kbd_irq);
    return true;
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
    bool held;
    int code;

    /* the Jornada has only left Ctrl and Alt */
    if (lnx == KEY_RIGHTCTRL) {
        lnx = KEY_LEFTCTRL;
    } else if (lnx == KEY_RIGHTALT) {
        lnx = KEY_LEFTALT;
    }
    for (code = 1; code < 128; code++) {
        if (j720_keymap[code] == lnx) {
            break;
        }
    }
    if (code == 128) {
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
        if (down && s->keyq_len + 2 <= J720_MCU_KEYQ) {
            j720_mcu_key_queue(s, code);
            j720_mcu_key_queue(s, code | 0x80);
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

    if (s->pen_up_pending && s->pen_samples >= J720_MCU_TS_MIN_SAMPLES) {
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
    s->debug = getenv("J720_TOUCH_DEBUG") != NULL;
    hs = qemu_input_handler_register(DEVICE(dev), &j720_mcu_kbd_handler);
    qemu_input_handler_activate(hs);
    hs = qemu_input_handler_register(DEVICE(dev), &j720_mcu_ts_handler);
    qemu_input_handler_activate(hs);
}

static int j720_mcu_post_load(void *opaque, int version_id)
{
    J720MCUState *s = opaque;

    if (s->out_len < 0 || s->out_len > (int)sizeof(s->out) ||
        s->out_pos < 0 || s->out_pos > s->out_len ||
        s->keyq_len < 0 || s->keyq_len > J720_MCU_KEYQ) {
        return -EINVAL;
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
 * with an NE2000-compatible network PC Card in socket 0 (the CF slot,
 * socket 1, stays empty). Register layout from Linux
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

#define J720_CARD_COR           0x3f8       /* attribute memory offsets */
#define J720_CARD_CCSR          0x3fa
#define J720_CARD_CFG_INDEX     0x20
#define COR_SRESET              0x80
#define COR_INDEX_MASK          0x3f
#define CCSR_INTR               0x02

#define TYPE_J720_SA1111 "j720-sa1111"
OBJECT_DECLARE_SIMPLE_TYPE(J720SA1111State, J720_SA1111)

struct J720SA1111State {
    SysBusDevice parent_obj;

    MemoryRegion intc_io;
    MemoryRegion pcmcia_io;
    MemoryRegion card_io;
    MemoryRegion card_attr;
    qemu_irq irq;               /* to SA-1110 GPIO1 */

    uint32_t inten[2], intpol[2], intstat[2], wakeen[2], wakepol[2];
    uint32_t intin[2];          /* current level of each interrupt input */
    uint32_t pccr, pcssr;

    uint8_t cor, ccsr;
    bool card_irq;
    uint8_t cis[128];
    int cis_len;
    NE2000State ne2000;
};

static void j720_sa1111_update(J720SA1111State *s)
{
    qemu_set_irq(s->irq, (s->intstat[0] & s->inten[0]) ||
                         (s->intstat[1] & s->inten[1]));
}

/* Interrupt inputs are edge-detected; INTPOL set = falling edge */
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
    case SA1111_INTPOL0:     s->intpol[0] = value; break;
    case SA1111_INTPOL1:     s->intpol[1] = value; break;
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
        /* socket 0: 3.3 V card present; socket 1: empty */
        return (j720_card_ready(s) ? PCSR_S0_READY : 0) | PCSR_S0_VS2 |
               PCSR_S0_BVD1 | PCSR_S0_BVD2 |
               PCSR_S1_DETECT | PCSR_S1_VS1 | PCSR_S1_VS2 |
               PCSR_S1_BVD1 | PCSR_S1_BVD2;
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
        s->pccr = value;
        j720_card_update(s);
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

    s->intin[1] = 1u << SA1111_IRQ_S0_READY_NINT;  /* card ready */
}

static int j720_sa1111_post_load(void *opaque, int version_id)
{
    J720SA1111State *s = opaque;

    j720_sa1111_update(s);
    return 0;
}

static const VMStateDescription vmstate_j720_sa1111 = {
    .name = TYPE_J720_SA1111,
    .version_id = 1,
    .minimum_version_id = 1,
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
    }
};

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

        qemu_configure_nic_device(dev, true, NULL);
        sysbus_realize_and_unref(sbd, &error_fatal);
        memory_region_add_subregion_overlap(get_system_memory(),
                J720_SA1111_INTC_BASE, sysbus_mmio_get_region(sbd, 0), 1);
        memory_region_add_subregion_overlap(get_system_memory(),
                J720_SA1111_PCMCIA_BASE, sysbus_mmio_get_region(sbd, 1), 1);
        sysbus_mmio_map(sbd, 2, J720_PCMCIA_S0_IO);
        sysbus_mmio_map(sbd, 3, J720_PCMCIA_S0_ATTR);
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
        MemoryRegion *pm_posr_stub = g_new(MemoryRegion, 1);
        memory_region_init_io(pm_posr_stub, NULL, &j720_pm_posr_stub_ops, NULL,
                               "j720.sa1110-pm-posr-stub", 4);
        memory_region_add_subregion_overlap(get_system_memory(),
                                             J720_SA1110_PM_POSR_BASE,
                                             pm_posr_stub, 1);
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
