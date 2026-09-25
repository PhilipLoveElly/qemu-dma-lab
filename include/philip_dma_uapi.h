#ifndef PHILIP_DMA_UAPI_H
#define PHILIP_DMA_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>


#define PHILIP_DMA_MAX_LEN    4096

#define PHILIP_DMA_IOC_MAGIC  'p'


struct philip_dma_ioc_transfer {
    __u32 len;
    __u32 reserved;

    __u8 src[PHILIP_DMA_MAX_LEN];
    __u8 dst[PHILIP_DMA_MAX_LEN];
};


#define PHILIP_DMA_IOC_TRANSFER \
    _IOWR( \
        PHILIP_DMA_IOC_MAGIC, \
        0x01, \
        struct philip_dma_ioc_transfer \
    )


#endif
