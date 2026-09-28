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
 *   0x40000000  SA-1111 companion chip      (SA_CS4) -- unimplemented-device stub
 *   0x48000000  Epson display controller    -- registers + BitBLT engine
 *   0x48200000  Epson frame buffer, 512 KiB -- plain RAM + 640x240 RGB565 console
 *   0xC0000000  system SDRAM, 32 MiB        (SA_SDCS0)
 *
 * The stubs exist so boot-code probes of this not-yet-emulated hardware
 * are visible via -d unimp instead of silently spinning forever; see
 * jornada720_init().
 *
 * Step 1 goal (see docs/plan.md): boot far enough to see loader/CE output
 * on the on-chip UART. No display, no SA-1111, no input yet.
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
 * Nothing is wired up on the on-chip SSP's SSI bus (jms->sa1110->ssp_bus).
 * Boot code was found (via QEMU monitor: PC=0x4f26c, R02=0x480001fc,
 * R04/R05 = on-chip SSDR/SSSR, R06 = on-chip GPIO) to be bit-banging data
 * out over this SSP -- almost certainly the serial config interface for
 * the Epson display chip, using GPIO for chip-select -- and then waiting
 * for a response that never comes because no SSIPeripheral is attached.
 * This stub just answers every transfer with 0, enough to unblock
 * whatever "did it ack" check the boot code does. See docs/research.md.
 */
#define TYPE_J720_SSI_STUB "j720-ssi-stub"
OBJECT_DECLARE_SIMPLE_TYPE(J720SSIStubState, J720_SSI_STUB)

struct J720SSIStubState {
    SSIPeripheral parent_obj;
};

static uint32_t j720_ssi_stub_transfer(SSIPeripheral *dev, uint32_t val)
{
    return 0;
}

static void j720_ssi_stub_realize(SSIPeripheral *dev, Error **errp)
{
    /* Nothing to do; SSIPeripheralClass.realize is called unconditionally
     * by ssi_peripheral_realize() with no NULL check, so this must exist. */
}

static void j720_ssi_stub_class_init(ObjectClass *klass, const void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = j720_ssi_stub_realize;
    k->transfer = j720_ssi_stub_transfer;
}

static const TypeInfo j720_ssi_stub_typeinfo = {
    .name = TYPE_J720_SSI_STUB,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(J720SSIStubState),
    .class_init = j720_ssi_stub_class_init,
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

    /* BitBLT operation in progress (write/read/color expansion blits) */
    int blt_op;
    uint32_t blt_x, blt_y, blt_w, blt_h;
    uint32_t blt_bit;       /* color expansion: next bit in current word */
} J720Display;

static bool j720_display_update(void *opaque)
{
    J720Display *d = opaque;

    if (!d->surface_set) {
        DisplaySurface *ds = qemu_create_displaysurface_from(
            J720_LCD_WIDTH, J720_LCD_HEIGHT, PIXMAN_r5g6b5,
            J720_LCD_WIDTH * 2, memory_region_get_ram_ptr(&d->fb));
        qemu_console_set_surface(d->con, ds);
        d->surface_set = true;
    }
    qemu_console_update_full(d->con);
    return true;
}

static void j720_display_invalidate(void *opaque)
{
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
    uint16_t *p = epson_px(d, epson_rect_addr(d, EPSON_BLT_DST,
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
        uint16_t *p = epson_px(d, epson_rect_addr(d, EPSON_BLT_DST,
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
                uint16_t *dp = epson_px(d, dst + y * stride + x * 2);
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
                uint16_t *dp = epson_px(d, dst - y * stride - x * 2);
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
                uint16_t *dp = epson_px(d, dst + y * stride + x * 2);
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
                *epson_px(d, dst + y * stride + x * 2) = fgc;
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

struct Jornada720MachineState {
    MachineState parent;

    StrongARMState *sa1110;
    J720Display display;
};

#define TYPE_JORNADA720_MACHINE MACHINE_TYPE_NAME("jornada720")
OBJECT_DECLARE_SIMPLE_TYPE(Jornada720MachineState, JORNADA720_MACHINE)

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
        MemoryRegion *pm_posr_stub = g_new(MemoryRegion, 1);
        memory_region_init_io(pm_posr_stub, NULL, &j720_pm_posr_stub_ops, NULL,
                               "j720.sa1110-pm-posr-stub", 4);
        memory_region_add_subregion_overlap(get_system_memory(),
                                             J720_SA1110_PM_POSR_BASE,
                                             pm_posr_stub, 1);
    }

    ssi_create_peripheral(jms->sa1110->ssp_bus, TYPE_J720_SSI_STUB);

    jms->display.blt_op = -1;
    memory_region_init_ram(&jms->display.fb, NULL, "j720.epson-fb",
                           J720_EPSON_FB_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), J720_EPSON_FB_BASE,
                                &jms->display.fb);
    memory_region_init_io(&jms->display.regs, NULL, &j720_epson_regs_ops,
                          &jms->display, "j720.epson-regs",
                          J720_EPSON_REGS_SIZE);
    memory_region_add_subregion(get_system_memory(), J720_EPSON_REGS_BASE,
                                &jms->display.regs);
    jms->display.con = qemu_graphic_console_create(NULL, 0, &j720_display_ops,
                                                   &jms->display);

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
    type_register_static(&j720_ssi_stub_typeinfo);
    type_register_static(&jornada720_machine_typeinfo);
}
type_init(jornada720_machine_register_types);
