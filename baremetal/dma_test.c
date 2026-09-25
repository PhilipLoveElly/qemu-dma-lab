#include <stdint.h>


/*
 * ============================================================
 * Philip DMA MMIO
 * ============================================================
 */

#define DMA_BASE            0xFE800000u

#define REG_SRC_ADDR        0x00u
#define REG_DST_ADDR        0x08u
#define REG_LENGTH          0x10u
#define REG_CONTROL         0x14u
#define REG_STATUS          0x18u
#define REG_IRQ_STATUS      0x1Cu
#define REG_IRQ_ENABLE      0x20u

#define CTRL_START          (1u << 0)

#define STATUS_BUSY         (1u << 0)
#define STATUS_DONE         (1u << 1)

#define IRQ_DONE            (1u << 0)


/*
 * ============================================================
 * Test RAM addresses
 * ============================================================
 */

#define SRC_ADDR            0x00110000u
#define DST_ADDR            0x00120000u
#define DMA_LENGTH          16u


/*
 * ============================================================
 * Interrupt configuration
 *
 * Philip DMA
 *      |
 *      v
 *    GSI 16
 *      |
 *      v
 *   IOAPIC
 *      |
 *      v
 * vector 0x2E
 *      |
 *      v
 * IDT[0x2E]
 *      |
 *      v
 * dma_irq_entry
 *      |
 *      v
 * dma_irq_handler()
 * ============================================================
 */

#define PHILIP_DMA_IRQ      16u
#define DMA_IRQ_VECTOR      0x2Eu

#define IOAPIC_BASE         0xFEC00000u


/*
 * ============================================================
 * Variables for debugging
 *
 * You can inspect these later using GDB / QEMU.
 * ============================================================
 */

volatile uint32_t dma_irq_count = 0;
volatile uint32_t dma_irq_seen = 0;
volatile uint32_t dma_test_pass = 0;
volatile uint32_t dma_test_fail = 0;


/*
 * ============================================================
 * MMIO helpers
 * ============================================================
 */

static inline void mmio_write32(uint32_t offset, uint32_t value)
{
    volatile uint32_t *addr;

    addr = (volatile uint32_t *)(DMA_BASE + offset);

    *addr = value;
}


static inline uint32_t mmio_read32(uint32_t offset)
{
    volatile uint32_t *addr;

    addr = (volatile uint32_t *)(DMA_BASE + offset);

    return *addr;
}


/*
 * ============================================================
 * IDT
 * ============================================================
 */

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attr;
    uint16_t offset_high;
} __attribute__((packed));


struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));


static struct idt_entry idt[256];


/*
 * This symbol is implemented in boot.S
 */
extern void dma_irq_entry(void);


/*
 * Install one interrupt gate into IDT.
 */
static void idt_set_gate(uint8_t vector, uint32_t handler)
{
    idt[vector].offset_low  =
        (uint16_t)(handler & 0xFFFFu);

    /*
     * 0x08 = kernel code segment selector.
     *
     * This assumes your GDT is:
     *
     * 0x00 = null
     * 0x08 = code
     * 0x10 = data
     */
    idt[vector].selector = 0x08u;

    idt[vector].zero = 0u;

    /*
     * 0x8E:
     *
     * bit 7     = Present
     * bits 6:5  = DPL 0
     * bit 4     = 0
     * bits 3:0  = 1110
     *
     * 32-bit interrupt gate
     */
    idt[vector].type_attr = 0x8Eu;

    idt[vector].offset_high =
        (uint16_t)((handler >> 16) & 0xFFFFu);
}


/*
 * Load IDT address into CPU IDTR.
 */
static void idt_load(void)
{
    struct idt_ptr idtr;

    idtr.limit = (uint16_t)(sizeof(idt) - 1u);
    idtr.base  = (uint32_t)idt;

    __asm__ volatile(
        "lidt %0"
        :
        : "m"(idtr)
    );
}


/*
 * ============================================================
 * IOAPIC
 * ============================================================
 */

static inline void ioapic_write(uint8_t reg, uint32_t value)
{
    volatile uint32_t *ioregsel;
    volatile uint32_t *iowin;

    ioregsel =
        (volatile uint32_t *)(IOAPIC_BASE + 0x00u);

    iowin =
        (volatile uint32_t *)(IOAPIC_BASE + 0x10u);

    *ioregsel = reg;
    *iowin = value;
}


/*
 * Route:
 *
 * GSI irq
 *    |
 *    v
 * vector
 *
 *
 * IOAPIC Redirection Table:
 *
 * IRQ 0:
 *     low  = 0x10
 *     high = 0x11
 *
 * IRQ 1:
 *     low  = 0x12
 *     high = 0x13
 *
 * ...
 *
 * IRQ 16:
 *     low  = 0x30
 *     high = 0x31
 */
static void ioapic_route_irq(uint8_t irq, uint8_t vector)
{
    uint8_t low_reg;
    uint8_t high_reg;

    low_reg =
        (uint8_t)(0x10u + (irq * 2u));

    high_reg =
        (uint8_t)(low_reg + 1u);


    /*
     * Destination field.
     *
     * APIC ID 0.
     */
    ioapic_write(
        high_reg,
        0x00000000u
    );


    /*
     * Redirection Table Low:
     *
     * bits 7:0 = vector
     *
     * bit 8  = 0
     *          fixed delivery mode
     *
     * bit 11 = 0
     *          physical destination mode
     *
     * bit 13 = 0
     *          active-high
     *
     * bit 15 = 0
     *          edge-triggered
     *
     * bit 16 = 0
     *          unmasked
     *
     *
     * Therefore for vector 0x2E:
     *
     * low = 0x0000002E
     */
    ioapic_write(
        low_reg,
        (uint32_t)vector
    );
}


/*
 * ============================================================
 * DMA interrupt handler
 * ============================================================
 */

void dma_irq_handler(void)
{
    uint32_t irq_status;

    dma_irq_count++;

    irq_status =
        mmio_read32(REG_IRQ_STATUS);


    /*
     * Make sure interrupt really came from our DMA.
     */
    if (irq_status & IRQ_DONE) {

        dma_irq_seen = 1u;


        /*
         * Clear IRQ.
         *
         * This assumes REG_IRQ_STATUS uses
         * Write-1-to-Clear semantics:
         *
         * write 1
         *    |
         *    v
         * clear IRQ_DONE
         */
        mmio_write32(
            REG_IRQ_STATUS,
            IRQ_DONE
        );
    }
}


/*
 * ============================================================
 * DMA data preparation
 * ============================================================
 */

static void prepare_test_data(void)
{
    volatile uint8_t *src;
    volatile uint8_t *dst;
    uint32_t i;

    src =
        (volatile uint8_t *)SRC_ADDR;

    dst =
        (volatile uint8_t *)DST_ADDR;


    /*
     * Source:
     *
     * 11 22 33 44
     * 55 66 77 88
     * 99 AA BB CC
     * DD EE 4A 5A
     */

    src[0]  = 0x11u;
    src[1]  = 0x22u;
    src[2]  = 0x33u;
    src[3]  = 0x44u;

    src[4]  = 0x55u;
    src[5]  = 0x66u;
    src[6]  = 0x77u;
    src[7]  = 0x88u;

    src[8]  = 0x99u;
    src[9]  = 0xAAu;
    src[10] = 0xBBu;
    src[11] = 0xCCu;

    src[12] = 0xDDu;
    src[13] = 0xEEu;
    src[14] = 0x4Au;
    src[15] = 0x5Au;


    /*
     * Clear destination first.
     */
    for (i = 0u; i < DMA_LENGTH; i++) {
        dst[i] = 0u;
    }
}


/*
 * ============================================================
 * Verify DMA data
 * ============================================================
 */

static void verify_dma_data(void)
{
    volatile uint8_t *src;
    volatile uint8_t *dst;
    uint32_t i;

    src =
        (volatile uint8_t *)SRC_ADDR;

    dst =
        (volatile uint8_t *)DST_ADDR;


    dma_test_pass = 1u;
    dma_test_fail = 0u;


    for (i = 0u; i < DMA_LENGTH; i++) {

        if (src[i] != dst[i]) {

            dma_test_pass = 0u;
            dma_test_fail = 1u;

            break;
        }
    }
}


/*
 * ============================================================
 * Main
 * ============================================================
 */

void dma_test_main(void)
{
    volatile uint32_t timeout;


    /*
     * --------------------------------------------------------
     * 1. Disable CPU interrupts while configuring IDT/IOAPIC
     * --------------------------------------------------------
     */

    __asm__ volatile("cli");


    /*
     * --------------------------------------------------------
     * 2. Install DMA ISR into IDT[0x2E]
     * --------------------------------------------------------
     */

    idt_set_gate(
        DMA_IRQ_VECTOR,
        (uint32_t)dma_irq_entry
    );


    /*
     * --------------------------------------------------------
     * 3. Load IDT
     * --------------------------------------------------------
     */

    idt_load();


    /*
     * --------------------------------------------------------
     * 4. Route GSI 16 -> vector 0x2E
     * --------------------------------------------------------
     */

    ioapic_route_irq(
        PHILIP_DMA_IRQ,
        DMA_IRQ_VECTOR
    );


    /*
     * --------------------------------------------------------
     * 5. Prepare RAM
     * --------------------------------------------------------
     */

    prepare_test_data();


    /*
     * --------------------------------------------------------
     * 6. Program DMA registers
     * --------------------------------------------------------
     */

    mmio_write32(
        REG_SRC_ADDR,
        SRC_ADDR
    );

    mmio_write32(
        REG_DST_ADDR,
        DST_ADDR
    );

    mmio_write32(
        REG_LENGTH,
        DMA_LENGTH
    );


    /*
     * --------------------------------------------------------
     * 7. Clear old pending DMA interrupt
     * --------------------------------------------------------
     */

    mmio_write32(
        REG_IRQ_STATUS,
        IRQ_DONE
    );


    /*
     * --------------------------------------------------------
     * 8. Enable DMA DONE interrupt
     * --------------------------------------------------------
     */

    mmio_write32(
        REG_IRQ_ENABLE,
        IRQ_DONE
    );


    /*
     * --------------------------------------------------------
     * 9. Enable CPU maskable interrupts
     * --------------------------------------------------------
     */

    __asm__ volatile("sti");


    /*
     * --------------------------------------------------------
     * 10. Start DMA
     * --------------------------------------------------------
     */

    mmio_write32(
        REG_CONTROL,
        CTRL_START
    );


    /*
     * --------------------------------------------------------
     * 11. Wait for interrupt
     *
     * dma_irq_handler() will set:
     *
     * dma_irq_seen = 1
     *
     * We use HLT instead of continuously polling STATUS.
     * CPU sleeps until interrupt arrives.
     * --------------------------------------------------------
     */

    timeout = 1000000u;

    while ((dma_irq_seen == 0u) &&
           (timeout != 0u)) {

        __asm__ volatile("hlt");

        timeout--;
    }


    /*
     * --------------------------------------------------------
     * 12. Verify copied data
     * --------------------------------------------------------
     */

    if (dma_irq_seen != 0u) {

        verify_dma_data();

    } else {

        dma_test_pass = 0u;
        dma_test_fail = 1u;
    }


    /*
     * --------------------------------------------------------
     * 13. Stop here forever
     *
     * You can inspect RAM / variables from QEMU monitor or GDB.
     * --------------------------------------------------------
     */

    for (;;) {
        __asm__ volatile("hlt");
    }
}
