/*
 * HP Jornada 720 (SA-1110-based Handheld PC) machine model.
 *
 * NOT intended for upstream submission to QEMU — local experiment only,
 * per qemu-src/AGENTS.md's AI-generated content policy. See
 * docs/plan.md / docs/research.md in the qemu-jornada720 project repo.
 *
 * Memory map (from Jornada 720 hardware docs):
 *   0x00000000  system Flash/ROM, 32 MiB   (SA_CS0)
 *   0x40000000  SA-1111 companion chip     (SA_CS4)  -- not emulated yet
 *   0x48000000  Epson display controller   -- not emulated yet
 *   0x48200000  Epson frame buffer, 512 KiB -- not emulated yet
 *   0xC0000000  system SDRAM, 32 MiB       (SA_SDCS0)
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
#include "system/address-spaces.h"
#include "qom/object.h"
#include "qemu/error-report.h"

#define J720_RAM_SIZE          (32 * MiB)
#define J720_FLASH_SIZE        (32 * MiB)
#define J720_FLASH_SECTOR_SIZE (64 * KiB)

struct Jornada720MachineState {
    MachineState parent;

    StrongARMState *sa1110;
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
    type_register_static(&jornada720_machine_typeinfo);
}
type_init(jornada720_machine_register_types);
