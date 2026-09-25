#include <stdint.h>

#define DMA_BASE       0xfe800000ULL

#define REG_SRC_ADDR   0x00
#define REG_DST_ADDR   0x08
#define REG_LENGTH     0x10
#define REG_CONTROL    0x14
#define REG_STATUS     0x18

#define CTRL_START     (1U << 0)

#define STATUS_BUSY    (1U << 0)
#define STATUS_DONE    (1U << 1)

static inline void mmio_write64(uint64_t addr, uint64_t value)
{
    *(volatile uint64_t *)addr = value;
}

static inline void mmio_write32(uint64_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t mmio_read32(uint64_t addr)
{
    return *(volatile uint32_t *)addr;
}

volatile uint8_t src_buffer[16] = {
    0x11, 0x22, 0x33, 0x44,
    0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc,
    0xdd, 0xee, 0xff, 0x5a
};

volatile uint8_t dst_buffer[16] = {0};

void main(void)
{
    uint64_t src = (uint64_t)src_buffer;
    uint64_t dst = (uint64_t)dst_buffer;

    mmio_write64(DMA_BASE + REG_SRC_ADDR, src);
    mmio_write64(DMA_BASE + REG_DST_ADDR, dst);

    mmio_write32(DMA_BASE + REG_LENGTH,
                 sizeof(src_buffer));

    mmio_write32(DMA_BASE + REG_CONTROL,
                 CTRL_START);

    while (!(mmio_read32(DMA_BASE + REG_STATUS)
             & STATUS_DONE)) {
    }

    while (1) {
        __asm__ volatile("hlt");
    }
}
