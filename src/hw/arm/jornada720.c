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
 *   0x48000000  Epson display controller    -- unimplemented-device stub
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
 * Epson display controller -- first cut. The frame buffer is backed by
 * plain RAM (so whatever CE draws is kept instead of being dropped by an
 * unimplemented-device stub) and scanned out as a fixed 640x240, 16bpp
 * RGB565 panel starting at frame buffer offset 0, which is the Jornada
 * 720's native panel and the mode Linux's s1d13xxxfb uses on this board.
 * Display start address / stride / bpp registers of the Epson chip are
 * NOT decoded yet (its register block is still an unimplemented-device
 * stub), so a CE mode that differs from this guess will look garbled.
 */
typedef struct J720Display {
    MemoryRegion fb;
    QemuConsole *con;
    bool surface_set;
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
    create_unimplemented_device("j720.epson-regs", J720_EPSON_REGS_BASE,
                                 J720_EPSON_REGS_SIZE);
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

    memory_region_init_ram(&jms->display.fb, NULL, "j720.epson-fb",
                           J720_EPSON_FB_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), J720_EPSON_FB_BASE,
                                &jms->display.fb);
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
