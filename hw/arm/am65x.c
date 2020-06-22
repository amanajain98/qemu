#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/arm/boot.h"
#include "hw/loader.h"
#include "hw/arm/virt.h"
#include "sysemu/sysemu.h"
#include "sysemu/blockdev.h"
#include "sysemu/tpm.h"
#include "hw/hw.h"
#include "hw/vfio/vfio-calxeda-xgmac.h"
#include "hw/vfio/vfio-amd-xgbe.h"
#include "target/arm/internals.h"
#include "hw/core/cpu.h"
#include "exec/address-spaces.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "kvm_arm.h"
#include "standard-headers/linux/input.h"
#include "hw/arm/omap.h"
#include "exec/memory.h"
#include "qemu/units.h"
#include "hw/char/pl011.h"
#include "sysemu/reset.h"
#include "hw/char/serial.h"

/*The AM65x Defined Machine*/
struct AM65x  {
    MachineState parent;
    MemMapEntry *memmap;
    ///struct omap_uart_s *uart;
    DeviceState *gic;
    int gic_version;
    bool secure;
    struct arm_boot_info bootinfo;
    int psci_conduit;
    PFlashCFI01 *flash;
    FWCfgState *fw_cfg;
};

struct AM65xClass {
    MachineClass parent;
};

/* The Pre-Defined certain values */
#define NUM_IRQS 960 ///It has 960 shared Peripheral Interrupts
#define USART0_INT 224

#define LEGACY_RAMLIMIT_GB 255
#define LEGACY_RAMLIMIT_BYTES (LEGACY_RAMLIMIT_GB * GiB)

#define AM65x_FLASH_SECTOR_SIZE (1 * (KiB))

#define TYPE_AM65x_MACHINE   MACHINE_TYPE_NAME("AM65x")
#define AM65x_MACHINE(obj) \
    OBJECT_CHECK(struct AM65x, (obj), TYPE_AM65x_MACHINE)
#define AM65x_MACHINE_GET_CLASS(obj) \
    OBJECT_GET_CLASS(struct AM65xClass, obj, TYPE_AM65x_MACHINE)

enum {
    MEM,
    CPUPERIPHS,
    GIC0_DISTRIBUTOR,
    GIC0_ITS, ///Without the ITS Functionality
    UART0,
    FW_CFG,
    SECURE_MEM,
    FLASH,
};

static MemMapEntry base_memmap[] = {
    /* Space up to 0x00020000 is reserved for a boot ROM */
    [MEM] =                { 0x00000000, 0x00020000 },  
    [CPUPERIPHS] =         { 0x01800000, 0x00400000 },
    /* GIC distributor and CPU interfaces sit inside the CPU peripheral space */
    [GIC0_DISTRIBUTOR] =   { 0x01800000, 0x00200000 },
    [GIC0_ITS] =           { 0x01a00000, 0x00200000 },
    [UART0] =              { 0x02800000, 0x00001000 },
    /* This redistributor space allows up to 2*64kB*123 CPUs */
    [FW_CFG] =             { 0x09020000, 0x00000018 },
    [SECURE_MEM] =         { 0x0e000000, 0x01000000 },
    /* Actual RAM size depends on initial RAM and device memory settings */
    [FLASH] =              { GiB       , 0x00020000 },
};

/*
enum {
    PSRAMECC0_RAM,
    CTRL_MMR0_CFG0,
    GIC0_ITS, ///Without the ITS Functionality
    GIC0_DISTRIBUTOR,
    UART0,
    UART1,
    UART2,
    COMPUTE_CLUSTER0_MSMC_SRAM,
    NAVSS0_NBSS_MSMC_DDRLO_INT,
    NAVSS0_NBSS_MSMC1_DDRHI_INT_MEM1,
    FLASH,
    FW_CFG,
};
*/

unsigned int smp_cpus;

/*
static MemMapEntry base_memmap[] = {
    
    /// Initial Addresses 
    [PSRAMECC0_RAM] = {0x0000000000, 0x0000000400},
    [CTRL_MMR0_CFG0] = {0x0000100000, 0x0000020000},
    /// GIC AND THE GIC distributor
    [GIC0_ITS] = {0x0001000000, 0x0000400000},
    [GIC0_DISTRIBUTOR] = {0x0001800000, 0x0000100000},
    /// UART Memory Designations 
    [UART0] = {0x0002800000, 0x0000000200},
    [UART1] = {0x0002810000, 0x0000000200},
    [UART2] = {0x0002820000, 0x0000000200},
    /// Some memory regions 
    [COMPUTE_CLUSTER0_MSMC_SRAM] = {0x0007000000, 0x0004000000},
    /// SRAM which is later initialised as well. 
    [NAVSS0_NBSS_MSMC_DDRLO_INT] = {0x0008000000, 0x0080000000},
    [NAVSS0_NBSS_MSMC1_DDRHI_INT_MEM1] = {0x0080000000, 0x0800000000},
    [FLASH] = {0x0000200000, 0x0008000000},
    [FW_CFG] = {0x0001000000,0x0000000018},
};
*/

/// Dummy Clocks for initialization of the OMAP_UART. Clock Rates can be from 48MHz to 192MHz 
/*
static struct clk dummy_fclk0 = {
    .name = "uart0_fclk",
    .rate = 48000000,
};

static struct clk dummy_fclk1 = {
    .name = "uart1_fclk",
    .rate = 192000000,
};
static struct clk dummy_fclk2 = {
    .name = "uart2_fclk",
    .rate = 48000000,
};
*/
/*
static void create_uart(struct AM65x *mach,
                        MemoryRegion *mem, Chardev *chr)
{
    char *nodename;
    hwaddr base = mach->memmap[UART0].base;
    int irq = 1;
    DeviceState *dev = qdev_create(NULL, TYPE_PL011);
    SysBusDevice *s = SYS_BUS_DEVICE(dev);
    qdev_prop_set_chr(dev, "chardev", chr);
    qdev_init_nofail(dev);
    memory_region_add_subregion(mem, base,
                                sysbus_mmio_get_region(s, 0));
    sysbus_mmio_map(s, 0, mach->memmap[UART0].base);
    sysbus_connect_irq(s, 0, qdev_get_gpio_in(mach->gic, irq));
    nodename = g_strdup_printf("/pl011@%" PRIx64, base);
    g_free(nodename);
}
static void create_serial_uart()
{
    SerialMM *smm = SERIAL_MM(qdev_create(NULL, TYPE_SERIAL_MM));
    MemoryRegion *mr;
    qdev_prop_set_uint8(DEVICE(smm), "regshift", 2);
    qdev_prop_set_uint32(DEVICE(smm), "baudbase", omap_clk_getrate(&dummy_fclk0)/16);
    qdev_prop_set_chr(DEVICE(smm), "chardev", serial_hd(0));
    qdev_set_legacy_instance_id(DEVICE(smm), mach->memmap[UART0].base, 2);
    qdev_prop_set_uint8(DEVICE(smm), "endianness", DEVICE_NATIVE_ENDIAN);
    qdev_init_nofail(DEVICE(smm));
    ///sysbus_mmio_map(SYS_BUS_DEVICE(smm), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(smm), 0, ir);
    mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(smm), 0);
    memory_region_add_subregion(sysmem, mach->memmap[UART0].base, mr);
}
*/

static bool am65x_firmware_init(MachineState *machine , struct AM65x *mach,MemoryRegion *sysmem)
{
    pflash_cfi01_legacy_drive(mach->flash , drive_get(IF_PFLASH, 0, 0));
    DeviceState *dev=DEVICE(mach->flash);
    hwaddr base=mach->memmap[FLASH].base;
    hwaddr size=mach->memmap[FLASH].size;
    qdev_prop_set_uint32(dev, "num-blocks", size / AM65x_FLASH_SECTOR_SIZE);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0,base);
    /*
    memory_region_add_subregion(sysmem,base,sysbus_mmio_get_region(SYS_BUS_DEVICE(dev),
                                                                      0));
    */
    if (bios_name) {
        char *fname;
        MemoryRegion *mr;
        int image_size;
        /*
        SysBusDevice *sbd = SYS_BUS_DEVICE(machine);
        sysbus_init_mmio(sbd, &s->ram);
	*/
        fname = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
        if (!fname) {
            error_report("Could not find ROM image '%s'", bios_name);
            exit(1);
        }
        ///mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(mach->flash), 0);
        mr=machine->ram;
        image_size = load_image_mr(fname, mr);
        g_free(fname);
        if (image_size < 0) {
            error_report("Could not load ROM image '%s'", bios_name);
            exit(1);
        }
    }
    return bios_name;
}

static void create_secure_ram(struct AM65x *mach,
                              MemoryRegion *secure_sysmem)
{
    MemoryRegion *secram = g_new(MemoryRegion, 1);
    char *nodename;
    hwaddr base = mach->memmap[SECURE_MEM].base;
    hwaddr size = mach->memmap[SECURE_MEM].size;

    memory_region_init_ram(secram, NULL, "am65x.secure-ram", size,
                           &error_fatal);
    memory_region_add_subregion(secure_sysmem, base, secram);

    nodename = g_strdup_printf("/secram@%" PRIx64, base);
    g_free(nodename);
}

static FWCfgState *create_fw_cfg(const struct AM65x *mach, AddressSpace *as)
{
    hwaddr base = mach->memmap[FW_CFG].base;
    FWCfgState *fw_cfg;
    fw_cfg = fw_cfg_init_mem_wide(base + 8, base, 8, base + 16, as);
    fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, (uint16_t)smp_cpus);
    return fw_cfg;
}

static const char *valid_cpus[] = {
    ARM_CPU_TYPE_NAME("cortex-a53"),
    ARM_CPU_TYPE_NAME("cortex-a57"),
    ARM_CPU_TYPE_NAME("cortex-a72"),
    ARM_CPU_TYPE_NAME("max"),
};

static bool cpu_type_valid(const char *cpu)
{
    for (int i = 0; i < ARRAY_SIZE(valid_cpus); i++) {
        if (strcmp(cpu, valid_cpus[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool am65x_get_secure(Object *obj, Error **errp)
{
    struct AM65x *mach = AM65x_MACHINE(obj);
    return mach->secure;
}
static void am65x_set_secure(Object *obj, bool value, Error **errp)
{
    struct AM65x *mach = AM65x_MACHINE(obj);
    mach->secure = value;
}

static char *am65x_get_gic_version(Object *obj, Error **errp)
{
    const char *val = "3";
    return g_strdup(val);
}
static void am65x_set_gic_version(Object *obj, const char *value, Error **errp)
{
    struct AM65x *mach = AM65x_MACHINE(obj);
    mach->gic_version = VIRT_GIC_VERSION_3;
}

static void flash_create(struct AM65x *mach)
{
    DeviceState *dev = qdev_create(NULL, TYPE_PFLASH_CFI01);
    qdev_prop_set_uint64(dev, "sector-length", AM65x_FLASH_SECTOR_SIZE);
    qdev_prop_set_uint8(dev, "width", 4);
    qdev_prop_set_uint8(dev, "device-width", 2);
    qdev_prop_set_bit(dev, "big-endian", false);
    qdev_prop_set_uint16(dev, "id0", 0x89);
    qdev_prop_set_uint16(dev, "id1", 0x18);
    qdev_prop_set_uint16(dev, "id2", 0x00);
    qdev_prop_set_uint16(dev, "id3", 0x00);
    qdev_prop_set_string(dev, "name", "Flash");
    object_property_add_child(OBJECT(mach), "Flash", OBJECT(dev),
                              &error_abort);
    mach->flash=PFLASH_CFI01(dev);    
}

static void create_omap_uart(struct AM65x *mach,MemoryRegion *mem, Chardev *chr)
{
    hwaddr base = mach->memmap[UART0].base;
    DeviceState *dev = qdev_create(NULL, TYPE_OMAP_UART);
    SysBusDevice *s = SYS_BUS_DEVICE(dev);
    qdev_prop_set_chr(dev, "chardev", chr);
    
    qdev_init_nofail(dev);
    memory_region_add_subregion(mem, base,
                                sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, 0, qdev_get_gpio_in(mach->gic, USART0_INT));
} 

/*
static void am65x_reset(void *opaque)
{
    struct AM65x *mach = (struct AM65x *) opaque;
    omap_uart_reset(mach->uart);
}
*/

static void AM65x_init(MachineState *machine)
{
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    struct AM65x *mach = AM65x_MACHINE(machine);
    ///struct AM65xClass *machc = AM65x_MACHINE_GET_CLASS(machine);
    const char *gictype = gicv3_class_name();
    const CPUArchIdList *possible_cpus;
    int type; ///Will specify the GIC Version used which here is 3
    uint32_t redist0_capacity;
    uint32_t redist0_count;
    uint32_t nb_redist_regions;
    SysBusDevice *gicbusdev;
    DeviceState *cpudev;
    int ppibase;
    MemoryRegion *sysmem=get_system_memory();
    bool firmware_loaded;
    const int timer_irq[] = {
        [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
        [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
        [GTIMER_HYP] = ARCH_TIMER_NS_EL2_IRQ,
        [GTIMER_SEC] = ARCH_TIMER_S_EL1_IRQ,
    };   
    
    if (!cpu_type_valid(machine->cpu_type)) {
        error_report("CPU type %s not supported", machine->cpu_type);
        exit(1);
    }
    MemoryRegion *secure_sysmem = NULL;
    secure_sysmem = g_new(MemoryRegion, 1);
    memory_region_init(secure_sysmem, OBJECT(machine), "secure-memory",
                           UINT64_MAX);
    memory_region_add_subregion_overlap(secure_sysmem, 0, sysmem, -1);
    mach->secure=true;
    mach->psci_conduit = QEMU_PSCI_CONDUIT_DISABLED;
    mach->memmap = base_memmap;
    smp_cpus = machine->smp.cpus;
    possible_cpus = mc->possible_cpu_arch_ids(machine);
    for (int n = 0; n < possible_cpus->len; n++) {
        Object *cpuobj;
        CPUState *cs;
        if (n >= smp_cpus) {
            break;
        }
        cpuobj = object_new(possible_cpus->cpus[n].type);
        object_property_set_int(cpuobj, possible_cpus->cpus[n].arch_id,
                                "mp-affinity", NULL);
	cs = CPU(cpuobj);
        cs->cpu_index = n;
        numa_cpu_pre_plug(&possible_cpus->cpus[cs->cpu_index], DEVICE(cpuobj),
                          &error_fatal);
        ///object_property_set_bool(cpuobj, false, "has_el3", NULL);
        object_property_set_int(cpuobj, mach->psci_conduit,
                                    "psci-conduit", NULL);
        if (object_property_find(cpuobj, "reset-cbar", NULL)) {
            object_property_set_int(cpuobj, mach->memmap[CPUPERIPHS].base,
                                    "reset-cbar", &error_abort);
        }
        object_property_set_link(cpuobj, OBJECT(sysmem), "memory",
                                 &error_abort);
        object_property_set_link(cpuobj, OBJECT(secure_sysmem),
                                     "secure-memory", &error_abort);
        object_property_set_bool(cpuobj, true, "realized", &error_fatal);
        object_unref(cpuobj);
    }

    ///GIC Part Starts now.
    mach->gic = qdev_create(NULL, gictype);
    type = 3;
    ///Initialisation of the device so created by using the qdev_create().
    qdev_prop_set_uint32(mach->gic, "revision", type);
    qdev_prop_set_uint32(mach->gic, "num-cpu", smp_cpus);
    ///The number of internal and external interrupts of which internal interrupts is always 32.
    ///16 Software Generated Interrupts and 16 Private Peripheral Interrupts
    qdev_prop_set_uint32(mach->gic, "num-irq", 32 + NUM_IRQS);

    if (!kvm_irqchip_in_kernel())  {
        qdev_prop_set_bit(mach->gic, "has-security-extensions", mach->secure);
    }
    redist0_capacity = mach->memmap[GIC0_DISTRIBUTOR].size / GICV3_REDIST_SIZE;
    redist0_count = MIN(smp_cpus, redist0_capacity);
    if (smp_cpus > redist0_capacity) {
        nb_redist_regions = 2;
    }
    else
        nb_redist_regions = 1;

    qdev_prop_set_uint32(mach->gic, "len-redist-region-count",
                         nb_redist_regions);
    qdev_prop_set_uint32(mach->gic, "redist-region-count[0]", redist0_count);
    qdev_init_nofail(mach->gic);
    
    gicbusdev = SYS_BUS_DEVICE(mach->gic);
    sysbus_mmio_map(gicbusdev, 0, mach->memmap[GIC0_DISTRIBUTOR].base);
    sysbus_mmio_map(gicbusdev, 1, mach->memmap[GIC0_ITS].base);
    for (int i = 0; i < smp_cpus; i++)  {
        cpudev = DEVICE(qemu_get_cpu(i));
        ppibase = NUM_IRQS + i * GIC_INTERNAL + GIC_NR_SGIS;  ///GIC_INTERNAL is 32 and GIC_NR_SGIS is 16,SGI's define the 
							      ///number of Software Generated interrupts

        /* Mapping from the output timer irq lines from the CPU to the
         * GIC PPI inputs we use for the Virtual board. */
        for (int q = 0; q < ARRAY_SIZE(timer_irq); q++)  {
            qdev_connect_gpio_out(cpudev, q, qdev_get_gpio_in(DEVICE(mach->gic), ppibase + timer_irq[q]));
        }

        qemu_irq irq = qdev_get_gpio_in(mach->gic, ppibase + ARCH_GIC_MAINT_IRQ);
        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0, irq);
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(mach->gic, ppibase + VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicbusdev, i , qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
    //GIC Initialisation is complete now.

    create_omap_uart(mach,sysmem,serial_hd(0));
    ///create_uart(mach, sysmem, serial_hd(0));
    create_secure_ram(mach,secure_sysmem);
    ///qemu_irq ir=qdev_get_gpio_in(DEVICE(mach->gic), USART0_INT);
    
    /*
    mach->uart = am65x_uart_init(sysmem, mach->memmap[UART0].base,ir,
                                    &dummy_fclk0, NULL, NULL, NULL, "UART0",
                                    serial_hd(0));   
    
    mach->uart[1] = am65x_uart_init(sysmem, mach->memmap[UART1].base,
                                    qdev_get_gpio_in(mach->gic, USART1_INT),
                                    &dummy_fclk1, NULL, NULL, NULL, "UART1",
                                    serial_hd(1));
    mach->uart[2] = am65x_uart_init(sysmem, mach->memmap[UART2].base,
                                    qdev_get_gpio_in(mach->gic, USART2_INT),
                                    &dummy_fclk2, NULL, NULL, NULL, "UART2",
                                    serial_hd(2));
    ///UART Initialisation is Complete now.*/

    mach->fw_cfg = create_fw_cfg(mach, &address_space_memory);
    rom_set_fw(mach->fw_cfg);
    
    ///mach->sram_size = mach->memmap[COMPUTE_CLUSTER0_MSMC_SRAM].size;  /// 64MB RAM

    /* RAM Memory-mapping 
    memory_region_init_ram(machine->ram, NULL, "MACHINE_RAM", mach->memmap[MEM].size,
                           &error_fatal);*/
    memory_region_add_subregion(sysmem, mach->memmap[MEM].base,
                                machine->ram);
    /* Bootload Info structure update */
    firmware_loaded = am65x_firmware_init(machine,mach, sysmem);
    mach->bootinfo.ram_size = machine->ram_size;
    mach->bootinfo.nb_cpus = smp_cpus;
    mach->bootinfo.board_id = -1;
    mach->bootinfo.firmware_loaded = firmware_loaded;

    ///qemu_register_reset(am65x_reset, mach);
}

static const CPUArchIdList *am65x_possible_cpu_arch_ids(MachineState *ms)
{
    unsigned int max_cpus = ms->smp.max_cpus;
    if (ms->possible_cpus) {
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }
    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (int n = 0; n < ms->possible_cpus->len; n++) {
        ms->possible_cpus->cpus[n].type = ms->cpu_type;
        ms->possible_cpus->cpus[n].props.has_thread_id = true;
        ms->possible_cpus->cpus[n].props.thread_id = n;
    }
    return ms->possible_cpus;
}

static CpuInstanceProperties am65x_cpu_index_to_props(MachineState *ms, unsigned cpu_index)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

static int64_t am65x_get_default_cpu_node_id(const MachineState *ms, int idx)
{
    return idx % ms->numa_state->num_nodes;
}

static void am65x_instance_init(Object *obj)
{
    struct AM65x *mach = AM65x_MACHINE(obj);
    ///struct AM65xClass *machc = AM65x_MACHINE_GET_CLASS(mach);
    mach->secure = true;
    object_property_add_bool(obj, "secure", am65x_get_secure,
                             am65x_set_secure, NULL);
    object_property_set_description(obj, "secure",
                                    "Set on/off to enable/disable the ARM "
                                    "Security Extensions (TrustZone)",
                                    NULL);

    mach->gic_version = 3;
    object_property_add_str(obj, "gic-version", am65x_get_gic_version,
                        am65x_set_gic_version, NULL);
    object_property_set_description(obj, "gic-version",
                                    "Set GIC version. "
                                    "Valid value is 3",
                                    NULL);
    flash_create(mach);
}

static void am65x_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->init = AM65x_init;
    mc->desc = "The TI AM65x SoC";
    mc->possible_cpu_arch_ids = am65x_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = am65x_cpu_index_to_props;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->get_default_cpu_node_id = am65x_get_default_cpu_node_id;
    mc->default_cpus = 1;
    mc->max_cpus=4;
    mc->default_ram_id = "am65x.ram";
    mc->numa_mem_supported = true;
    mc->auto_enable_numa_with_memhp = true;
}

static const TypeInfo new_device_info = {
    .name = MACHINE_TYPE_NAME("AM65x"),
    .parent = TYPE_MACHINE,
    .class_init = am65x_class_init,
    .class_size    = sizeof(struct AM65xClass),
    .instance_init = am65x_instance_init,
    .instance_size = sizeof(struct AM65x),
    .interfaces = (InterfaceInfo[]) {
         { TYPE_HOTPLUG_HANDLER },
         { }
     },
};
static void am65x_machine_init(void)
{
    type_register_static(&new_device_info);
}
type_init(am65x_machine_init)
