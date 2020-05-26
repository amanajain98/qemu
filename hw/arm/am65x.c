#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/arm/virt.h"
#include "sysemu/sysemu.h"
#include "hw/hw.h"
#include "exec/address-spaces.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "kvm_arm.h"
#include "standard-headers/linux/input.h"
#include "hw/arm/omap.h"
#include "exec/memory.h"

/* The Pre-Defined certain values */
#define NUM_IRQS 960 ///It has 960 shared Peripheral Interrupts
#define USART0_INT 224
#define USART1_INT 225
#define USART2_INT 226

/*The AM65x Defined Machine*/
struct am65x  {
    ARMCPU *cpu;
    MemMapEntry *memmap;
    MemoryRegion sram;
    int sram_size;
    struct omap_uart_s *uart[3];
    DeviceState *gic;
    bool secure;
};

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
};

static MemMapEntry base_memmap[] = {
    
    /* Initial Addresses */
    [PSRAMECC0_RAM] = {0x0000000000, 0x0000000400},
    [CTRL_MMR0_CFG0] = {0x0000100000, 0x0000020000},
    /* GIC AND THE GIC distributor */
    [GIC0_ITS] = {0x0001000000, 0x0000400000},
    [GIC0_DISTRIBUTOR] = {0x0001800000, 0x0000100000},
    /* UART Memory Designations */
    [UART0] = {0x0002800000, 0x0000000200},
    [UART1] = {0x0002810000, 0x0000000200},
    [UART2] = {0x0002820000, 0x0000000200},
    /* Some memory regions */
    [COMPUTE_CLUSTER0_MSMC_SRAM] = {0x0070000000, 0x0004000000},
    /* SRAM which is later initialised as well. */
    [NAVSS0_NBSS_MSMC_DDRLO_INT] = {0x0080000000, 0x0080000000},
    [NAVSS0_NBSS_MSMC1_DDRHI_INT_MEM1] = {0x0800000000, 0x0800000000},
};

/*Dummy Clocks for initialization of the OMAP_UART. Clock Rates can be from 48MHz to 192MHz */

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

static void am65x_init(MachineState *machine)
{
    struct am65x *mach;
    ARMCPU *cpu;
    Object *cpuobj;
    const char *gictype = gicv3_class_name();
    unsigned int smp_cpus;
    int type; ///Will specify the GIC Version used which here is 3
    uint32_t redist0_capacity;
    uint32_t redist0_count;
    uint32_t nb_redist_regions;
    SysBusDevice *gicbusdev;
    SysBusDevice *busdev;
    DeviceState *cpudev;
    int ppibase;
    MemoryRegion *sysmem;
    const int timer_irq[] = {
        [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
        [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
        [GTIMER_HYP] = ARCH_TIMER_NS_EL2_IRQ,
        [GTIMER_SEC] = ARCH_TIMER_S_EL1_IRQ,
    };

    mach = g_new0(struct am65x, 1);
    cpuobj = object_new(machine->cpu_type);
    cpu = ARM_CPU(cpuobj);
    mach->cpu = cpu;
    mach->memmap = base_memmap;
    mach->gic = qdev_create(NULL, gictype);
    smp_cpus = machine->smp.cpus;
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
    redist0_capacity = mach->memmap[GIC0_ITS].size / GICV3_REDIST_SIZE;
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
    sysbus_mmio_map(gicbusdev, 0, mach->memmap[GIC0_ITS].base);
    sysbus_mmio_map(gicbusdev, 1, mach->memmap[GIC0_DISTRIBUTOR].base);
    for (int i = 0; i < smp_cpus; i++)  {
        cpudev = DEVICE(qemu_get_cpu(i));
        ppibase = NUM_IRQS + i * GIC_INTERNAL + GIC_NR_SGIS;

        /* Mapping from the output timer irq lines from the CPU to the
         * GIC PPI inputs we use for the Virtual board. */

        for (int q = 0; q < ARRAY_SIZE(timer_irq); q++)  {
            qdev_connect_gpio_out(cpudev, q, qdev_get_gpio_in(mach->gic, ppibase + timer_irq[q]));
        }

        qemu_irq irq = qdev_get_gpio_in(mach->gic, ppibase + ARCH_GIC_MAINT_IRQ);
        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0, irq);
        sysbus_connect_irq(gicbusdev, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
    //GIC Initialisation is complete now.

    sysmem = get_system_memory();
    mach->sram_size = mach->memmap[COMPUTE_CLUSTER0_MSMC_SRAM].size;

    /* Memory-mapping stuff */
    memory_region_init_ram(&mach->sram, NULL, "am65x.sram", mach->sram_size, &error_fatal);
    memory_region_add_subregion(sysmem, mach->memmap[COMPUTE_CLUSTER0_MSMC_SRAM].base, &mach->sram);

    busdev = SYS_BUS_DEVICE(mach->gic);
    sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(busdev, 1, qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ));
    sysbus_mmio_map(busdev, 0, 0x00480fe000);

    mach->uart[0] = am65x_uart_init(sysmem, mach->memmap[UART0].base,
                                    qdev_get_gpio_in(mach->gic, USART0_INT),
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
    ///UART Initialisation is Complete now.
}
static void am65x_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "The TI AM65x SoC";
    mc->init = am65x_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->default_cpus = 2;
}
static const TypeInfo new_device_info = {
    .name = MACHINE_TYPE_NAME("AM65x"),
    .parent = TYPE_MACHINE,
    .class_init = am65x_class_init,
};
static void am65x_machine_init(void)
{
    type_register_static(&new_device_info);
}
type_init(am65x_machine_init)
