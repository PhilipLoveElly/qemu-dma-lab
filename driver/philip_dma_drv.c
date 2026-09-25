#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/acpi.h>
#include <linux/jiffies.h>
#include <linux/string.h>

#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

#include "philip_dma_uapi.h"


#define DRIVER_NAME     "philip-dma"


/*
 * ------------------------------------------------------------
 * Philip DMA register map
 * ------------------------------------------------------------
 */
#define REG_SRC_ADDR        0x00
#define REG_DST_ADDR        0x08
#define REG_LENGTH          0x10
#define REG_CONTROL         0x14
#define REG_STATUS          0x18
#define REG_IRQ_STATUS      0x1C
#define REG_IRQ_ENABLE      0x20


/*
 * CONTROL bits
 */
#define CTRL_START          (1U << 0)
#define CTRL_ABORT          (1U << 1)

/*
 * STATUS bits
 */
#define STATUS_BUSY         (1U << 0)
#define STATUS_DONE         (1U << 1)


/*
 * IRQ bits
 */
#define IRQ_DONE            (1U << 0)


/*
 * DMA timeout
 */
#define DMA_TIMEOUT_MS      1000


/*
 * ------------------------------------------------------------
 * Driver private data
 * ------------------------------------------------------------
 */
struct philip_dma_dev {
    /*
     * MMIO base address.
     */
    void __iomem *regs;

    /*
     * Linux device.
     */
    struct device *dev;

    /*
     * Linux IRQ number.
     *
     * Example:
     *
     * ACPI GSI 16
     *      ↓
     * Linux IRQ domain
     *      ↓
     * Linux IRQ 49
     */
    int irq;


    /*
     * DMA coherent buffers.
     */
    void *src_buf;
    void *dst_buf;

    dma_addr_t src_dma;
    dma_addr_t dst_dma;


    /*
     * IRQ -> process context synchronization.
     */
    struct completion dma_done;


    /*
     * Protect the DMA engine.
     *
     * Only one userspace DMA transaction
     * may run at a time.
     */
    struct mutex xfer_lock;

    /*
     * Protected by xfer_lock.
     * Reject further transfers if timeout recovery fails.
     */
    bool faulted;

    /*
     * /dev/philip_dma
     */
    struct miscdevice miscdev;
};


/*
 * ------------------------------------------------------------
 * IRQ handler
 * ------------------------------------------------------------
 */
static irqreturn_t philip_dma_irq(
    int irq,
    void *dev_id
)
{
    struct philip_dma_dev *dma = dev_id;
    u32 irq_status;


    /*
     * Read interrupt status from device.
     */
    irq_status = readl(
        dma->regs + REG_IRQ_STATUS
    );


    /*
     * If IRQ_DONE is not set,
     * this interrupt is not ours.
     */
    if (!(irq_status & IRQ_DONE))
        return IRQ_NONE;


  /*  dev_info(
        dma->dev,
        "IRQ received, Linux IRQ=%d, irq_status=0x%08x\n",
        irq,
        irq_status
    );*/


    /*
     * REG_IRQ_STATUS is W1C.
     *
     * Write 1 to IRQ_DONE
     * to clear the pending interrupt.
     */
    writel(
        IRQ_DONE,
        dma->regs + REG_IRQ_STATUS
    );


    /*
     * Wake the process that is sleeping in:
     *
     * wait_for_completion_timeout()
     */
    complete(
        &dma->dma_done
    );


    return IRQ_HANDLED;
}

/*
 * Called from process context with xfer_lock held.
 *
 * Requires the QEMU device implementation with synchronous CTRL_ABORT:
 * after ABORT is processed, its pending timer is cancelled and
 * STATUS / IRQ_STATUS are cleared.
 */
static int philip_dma_recover_timeout(struct philip_dma_dev *dma)
{
    u32 status;
    u32 irq_status;

    /* Stop the device from asserting new completion interrupts. */
    writel(0, dma->regs + REG_IRQ_ENABLE);
    readl(dma->regs + REG_IRQ_ENABLE);

    /* Cancel the old transfer and clear its device-side state. */
    writel(CTRL_ABORT, dma->regs + REG_CONTROL);

    /* Read back from the device after issuing ABORT. */
    status = readl(dma->regs + REG_STATUS);

    /*
     * A handler may already have read IRQ_DONE before ABORT.
     * Wait for that handler before resetting the completion.
     */
    synchronize_irq(dma->irq);

    status = readl(dma->regs + REG_STATUS);
    irq_status = readl(dma->regs + REG_IRQ_STATUS);

    if (status != 0 || irq_status != 0) {
        dma->faulted = true;

        dev_err(dma->dev,
                "DMA recovery failed: STATUS=0x%08x IRQ_STATUS=0x%08x\n",
                status, irq_status);

        /* Leave device interrupts disabled. */
        return -EIO;
    }

    reinit_completion(&dma->dma_done);

    writel(IRQ_DONE, dma->regs + REG_IRQ_ENABLE);
    readl(dma->regs + REG_IRQ_ENABLE);

    dev_info(dma->dev,
             "DMA timeout recovery complete: engine idle\n");

    return 0;
}

/*
 * ------------------------------------------------------------
 * Perform one DMA transaction
 * ------------------------------------------------------------
 *
 * src and dst here are KERNEL memory pointers.
 *
 * copy_from_user() / copy_to_user() are handled
 * by the ioctl layer.
 */
static int philip_dma_transfer(
    struct philip_dma_dev *dma,
    const u8 *src,
    u8 *dst,
    u32 len
)
{
    unsigned long timeout;


    /*
     * Validate DMA length.
     */
    if (len == 0 ||
        len > PHILIP_DMA_MAX_LEN) {

        return -EINVAL;
    }

    /*
     * The caller holds xfer_lock.
     * Do not touch shared DMA buffers after failed recovery.
     */
    if (dma->faulted)
        return -EIO;

    /*
     * --------------------------------------------------------
     * 1. Prepare source buffer
     * --------------------------------------------------------
     */
    memcpy(
        dma->src_buf,
        src,
        len
    );


    /*
     * Clear destination before DMA.
     */
    memset(
        dma->dst_buf,
        0,
        len
    );


    /*
     * --------------------------------------------------------
     * 2. Clear stale pending IRQ
     * --------------------------------------------------------
     *
     * IRQ_STATUS is W1C.
     */
    writel(
        IRQ_DONE,
        dma->regs + REG_IRQ_STATUS
    );


    /*
     * --------------------------------------------------------
     * 3. Prepare completion
     * --------------------------------------------------------
     *
     * Previous DMA may already have called complete(),
     * so reset it before starting a new transfer.
     */
    reinit_completion(
        &dma->dma_done
    );


    /*
     * --------------------------------------------------------
     * 4. Program DMA registers
     * --------------------------------------------------------
     */
    writeq(
        dma->src_dma,
        dma->regs + REG_SRC_ADDR
    );


    writeq(
        dma->dst_dma,
        dma->regs + REG_DST_ADDR
    );


    writel(
        len,
        dma->regs + REG_LENGTH
    );


    /*dev_info(
        dma->dev,
        "userspace DMA start: len=%u src_dma=%pad dst_dma=%pad\n",
        len,
        &dma->src_dma,
        &dma->dst_dma
    );*/


    /*
     * --------------------------------------------------------
     * 5. Start DMA
     * --------------------------------------------------------
     */
    writel(
        CTRL_START,
        dma->regs + REG_CONTROL
    );


    /*
     * --------------------------------------------------------
     * 6. Sleep until IRQ handler calls complete()
     * --------------------------------------------------------
     */
    timeout = wait_for_completion_timeout(
        &dma->dma_done,
        msecs_to_jiffies(DMA_TIMEOUT_MS)
    );


     if (!timeout) {
        u32 status;
        int recovery_ret;

        status = readl(dma->regs + REG_STATUS);

        dev_err(dma->dev,
                "DMA timeout, STATUS=0x%08x; aborting transfer\n",
                status);

        recovery_ret = philip_dma_recover_timeout(dma);
        if (recovery_ret)
            return recovery_ret;

        /*
         * Recovery succeeded, but this request still timed out.
         * Do not report it as a successful transfer.
         */
        return -ETIMEDOUT;
    }

    /*
     * --------------------------------------------------------
     * 7. DMA completed
     * --------------------------------------------------------
     *
     * Copy coherent destination buffer into
     * ioctl's kernel-side request structure.
     */
    memcpy(
        dst,
        dma->dst_buf,
        len
    );


   /* dev_info(
        dma->dev,
        "userspace DMA complete: len=%u\n",
        len
    );*/


    return 0;
}


static long philip_dma_ioctl(
    struct file *file,
    unsigned int cmd,
    unsigned long arg
)
{
    struct miscdevice *misc;
    struct philip_dma_dev *dma;
    struct philip_dma_ioc_transfer *req;
    int ret;

    /*
     * Reject unsupported commands before allocating memory.
     */
    if (cmd != PHILIP_DMA_IOC_TRANSFER)
        return -ENOTTY;

    misc = file->private_data;
    dma = container_of(
        misc,
        struct philip_dma_dev,
        miscdev
    );

    /*
     * Allocate a private request for this ioctl call.
     * The full request is no longer on the kernel stack.
     */
    req = kmalloc(sizeof(*req), GFP_KERNEL_ACCOUNT);
    if (!req)
        return -ENOMEM;

    /*
     * Copy the complete request from userspace.
     */
    if (copy_from_user(
            req,
            (void __user *)arg,
            sizeof(*req)
        )) {
        ret = -EFAULT;
        goto out_free;
    }

    /*
     * Validate transfer length.
     */
    if (req->len == 0 ||
        req->len > PHILIP_DMA_MAX_LEN) {
        dev_err(
            dma->dev,
            "invalid DMA length: %u\n",
            req->len
        );

        ret = -EINVAL;
        goto out_free;
    }

    /*
     * Serialize access to the shared DMA engine and buffers.
     */
    ret = mutex_lock_interruptible(&dma->xfer_lock);
    if (ret)
        goto out_free;

    ret = philip_dma_transfer(
        dma,
        req->src,
        req->dst,
        req->len
    );

    /*
     * Always release the lock after transfer returns,
     * including when the transfer reports an error.
     */
    mutex_unlock(&dma->xfer_lock);

    if (ret)
        goto out_free;

    /*
     * req is private to this call, so copying it back does
     * not require holding the DMA engine mutex.
     */
    if (copy_to_user(
            (void __user *)arg,
            req,
            sizeof(*req)
        )) {
        ret = -EFAULT;
        goto out_free;
    }

    ret = 0;

out_free:
    kfree(req);
    return ret;
}

/*
 * ------------------------------------------------------------
 * File operations
 * ------------------------------------------------------------
 *
 * userspace:
 *
 * ioctl(fd, PHILIP_DMA_IOC_TRANSFER, ...)
 *
 *              ↓
 *
 * philip_dma_ioctl()
 */
static const struct file_operations philip_dma_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = philip_dma_ioctl,
};


/*
 * ------------------------------------------------------------
 * Probe
 * ------------------------------------------------------------
 */
static int philip_dma_probe(
    struct platform_device *pdev
)
{
    struct philip_dma_dev *dma;
    struct resource *res;

    u32 status;
    u32 irq_enable;

    int ret;


   /* dev_info(
        &pdev->dev,
        "probe entered\n"
    );*/


    /*
     * --------------------------------------------------------
     * 1. Allocate private driver structure
     * --------------------------------------------------------
     */
    dma = devm_kzalloc(
        &pdev->dev,
        sizeof(*dma),
        GFP_KERNEL
    );


    if (!dma)
        return -ENOMEM;


    /*
     * Save Linux device pointer.
     */
    dma->dev = &pdev->dev;


    /*
     * --------------------------------------------------------
     * 2. Initialize synchronization objects
     * --------------------------------------------------------
     */
    init_completion(
        &dma->dma_done
    );


    mutex_init(
        &dma->xfer_lock
    );


    /*
     * Save private data in platform_device.
     *
     * remove() can later retrieve it with:
     *
     * platform_get_drvdata()
     */
    platform_set_drvdata(
        pdev,
        dma
    );


    /*
     * --------------------------------------------------------
     * 3. Obtain MMIO resource
     * --------------------------------------------------------
     *
     * This resource came from ACPI _CRS.
     */
    res = platform_get_resource(
        pdev,
        IORESOURCE_MEM,
        0
    );


    if (!res) {
        dev_err(
            &pdev->dev,
            "failed to get MMIO resource\n"
        );

        return -ENODEV;
    }


   /* dev_info(
        &pdev->dev,
        "MMIO resource: start=%pa end=%pa\n",
        &res->start,
        &res->end
    );*/


    /*
     * --------------------------------------------------------
     * 4. Map device registers
     * --------------------------------------------------------
     */
    dma->regs = devm_ioremap_resource(
        &pdev->dev,
        res
    );


    if (IS_ERR(dma->regs))
        return PTR_ERR(dma->regs);


  /*  dev_info(
        &pdev->dev,
        "MMIO mapped successfully\n"
    ); */


    /*
     * Read initial status.
     */
    status = readl(
        dma->regs + REG_STATUS
    );


   /* dev_info(
        &pdev->dev,
        "STATUS = 0x%08x\n",
        status
    );*/


    /*
     * --------------------------------------------------------
     * 5. Allocate coherent source DMA buffer
     * --------------------------------------------------------
     */
    dma->src_buf = dma_alloc_coherent(
        &pdev->dev,
        PHILIP_DMA_MAX_LEN,
        &dma->src_dma,
        GFP_KERNEL
    );


    if (!dma->src_buf) {
        dev_err(
            &pdev->dev,
            "failed to allocate source DMA buffer\n"
        );

        return -ENOMEM;
    }


    /*
     * --------------------------------------------------------
     * 6. Allocate coherent destination DMA buffer
     * --------------------------------------------------------
     */
    dma->dst_buf = dma_alloc_coherent(
        &pdev->dev,
        PHILIP_DMA_MAX_LEN,
        &dma->dst_dma,
        GFP_KERNEL
    );


    if (!dma->dst_buf) {
        dev_err(
            &pdev->dev,
            "failed to allocate destination DMA buffer\n"
        );

        ret = -ENOMEM;

        goto err_free_src;
    }


   /* dev_info(
        &pdev->dev,
        "src_dma = %pad\n",
        &dma->src_dma
    );*/


   /* dev_info(
        &pdev->dev,
        "dst_dma = %pad\n",
        &dma->dst_dma
    );*/


    /*
     * --------------------------------------------------------
     * 7. Get Linux IRQ number
     * --------------------------------------------------------
     */
    dma->irq = platform_get_irq(
        pdev,
        0
    );


    if (dma->irq < 0) {
        ret = dma->irq;

        dev_err(
            &pdev->dev,
            "failed to get IRQ: %d\n",
            ret
        );

        goto err_free_dst;
    }


   /* dev_info(
        &pdev->dev,
        "Linux IRQ = %d\n",
        dma->irq
    );*/


    /*
     * --------------------------------------------------------
     * 8. Clear stale pending interrupt
     * --------------------------------------------------------
     */
    writel(
        IRQ_DONE,
        dma->regs + REG_IRQ_STATUS
    );


    /*
     * Disable device interrupt while
     * Linux IRQ handler is being registered.
     */
    writel(
        0,
        dma->regs + REG_IRQ_ENABLE
    );


    /*
     * --------------------------------------------------------
     * 9. Register Linux IRQ handler
     * --------------------------------------------------------
     */
    ret = devm_request_irq(
        &pdev->dev,
        dma->irq,
        philip_dma_irq,
        0,
        DRIVER_NAME,
        dma
    );


    if (ret) {
        dev_err(
            &pdev->dev,
            "failed to request IRQ %d: %d\n",
            dma->irq,
            ret
        );

        goto err_free_dst;
    }


   /* dev_info(
        &pdev->dev,
        "IRQ %d registered successfully\n",
        dma->irq
    );*/


    /*
     * --------------------------------------------------------
     * 10. Enable DMA DONE interrupt
     * --------------------------------------------------------
     */
    writel(
        IRQ_DONE,
        dma->regs + REG_IRQ_ENABLE
    );


    irq_enable = readl(
        dma->regs + REG_IRQ_ENABLE
    );


   /* dev_info(
        &pdev->dev,
        "IRQ_ENABLE = 0x%08x\n",
        irq_enable
    );*/


    /*
     * --------------------------------------------------------
     * 11. Configure miscdevice
     * --------------------------------------------------------
     *
     * This creates the userspace interface:
     *
     * /dev/philip_dma
     */
    dma->miscdev.minor =
        MISC_DYNAMIC_MINOR;


    dma->miscdev.name =
        "philip_dma";


    dma->miscdev.fops =
        &philip_dma_fops;


    dma->miscdev.parent =
        &pdev->dev;


    /*
     * --------------------------------------------------------
     * 12. Register miscdevice
     * --------------------------------------------------------
     */
    ret = misc_register(
        &dma->miscdev
    );


    if (ret) {
        dev_err(
            &pdev->dev,
            "failed to register misc device: %d\n",
            ret
        );

        goto err_disable_irq;
    }


   /* dev_info(
        &pdev->dev,
        "/dev/philip_dma registered\n"
    );*/


    /*
     * IMPORTANT:
     *
     * We do NOT start DMA here anymore.
     *
     * probe() only initializes the driver.
     *
     * DMA is started later by userspace:
     *
     * ioctl()
     *   ↓
     * philip_dma_ioctl()
     *   ↓
     * philip_dma_transfer()
     */
   /* dev_info(
        &pdev->dev,
        "driver ready\n"
    );*/


    return 0;


/*
 * ------------------------------------------------------------
 * Error paths
 * ------------------------------------------------------------
 */

err_disable_irq:

    /*
     * Stop device from generating interrupts.
     */
    writel(
        0,
        dma->regs + REG_IRQ_ENABLE
    );


    /*
     * Clear pending IRQ.
     */
    writel(
        IRQ_DONE,
        dma->regs + REG_IRQ_STATUS
    );


err_free_dst:

    if (dma->dst_buf) {

        dma_free_coherent(
            &pdev->dev,
            PHILIP_DMA_MAX_LEN,
            dma->dst_buf,
            dma->dst_dma
        );


        dma->dst_buf = NULL;
    }


err_free_src:

    if (dma->src_buf) {

        dma_free_coherent(
            &pdev->dev,
            PHILIP_DMA_MAX_LEN,
            dma->src_buf,
            dma->src_dma
        );


        dma->src_buf = NULL;
    }


    return ret;
}


/*
 * ------------------------------------------------------------
 * Remove
 * ------------------------------------------------------------
 */
static void philip_dma_remove(struct platform_device *pdev)
{
    struct philip_dma_dev *dma = platform_get_drvdata(pdev);
    u32 status;
    u32 irq_status;

    dev_info(&pdev->dev, "remove entered\n");

    /*
     * Stop new opens through the misc device.
     * This alone does not invalidate existing file descriptors.
     */
    misc_deregister(&dma->miscdev);

    mutex_lock(&dma->xfer_lock);
    dma->faulted = true;

    /*
     * Stop device IRQ generation before stopping the engine.
     */
    writel(0, dma->regs + REG_IRQ_ENABLE);
    readl(dma->regs + REG_IRQ_ENABLE);

    /*
     * Our QEMU device processes CTRL_ABORT synchronously:
     * cancel its timer and clear STATUS / IRQ_STATUS.
     */
    writel(CTRL_ABORT, dma->regs + REG_CONTROL);
    readl(dma->regs + REG_STATUS);

    /*
     * Remove the managed IRQ handler now, before freeing buffers.
     * This also waits for executing handlers to finish.
     */
    devm_free_irq(&pdev->dev, dma->irq, dma);

    status = readl(dma->regs + REG_STATUS);
    irq_status = readl(dma->regs + REG_IRQ_STATUS);

    if (status != 0 || irq_status != 0) {
        /*
         * Cannot confirm the device is quiescent.
         * Deliberately retain the manually allocated DMA buffers
         * rather than return potentially DMA-active memory for reuse.
         */
        dev_err(&pdev->dev,
                "remove: ABORT failed, STATUS=0x%08x "
                "IRQ_STATUS=0x%08x; DMA buffers retained\n",
                status, irq_status);

        mutex_unlock(&dma->xfer_lock);
        return;
    }

    dev_info(&pdev->dev,
             "remove: engine idle, IRQ handler released\n");

    if (dma->dst_buf) {
        dma_free_coherent(&pdev->dev,
                          PHILIP_DMA_MAX_LEN,
                          dma->dst_buf,
                          dma->dst_dma);
        dma->dst_buf = NULL;
    }

    if (dma->src_buf) {
        dma_free_coherent(&pdev->dev,
                          PHILIP_DMA_MAX_LEN,
                          dma->src_buf,
                          dma->src_dma);
        dma->src_buf = NULL;
    }

    mutex_unlock(&dma->xfer_lock);

    dev_info(&pdev->dev, "driver removed\n");
}
/*
 * ------------------------------------------------------------
 * ACPI match table
 * ------------------------------------------------------------
 *
 * Matches:
 *
 * Device (PDMA)
 * {
 *     Name (_HID, "PHL0001")
 *     ...
 * }
 */
static const struct acpi_device_id philip_dma_acpi_ids[] = {
    { "PHL0001", 0 },
    { }
};


MODULE_DEVICE_TABLE(
    acpi,
    philip_dma_acpi_ids
);


/*
 * ------------------------------------------------------------
 * Platform driver
 * ------------------------------------------------------------
 */
static struct platform_driver philip_dma_driver = {

    .probe =
        philip_dma_probe,

    .remove =
        philip_dma_remove,

    .driver = {

        .name =
            DRIVER_NAME,

        .acpi_match_table =
            ACPI_PTR(philip_dma_acpi_ids),

        .suppress_bind_attrs = true,
  },
};


/*
 * Equivalent to module init / exit registration.
 */
module_platform_driver(
    philip_dma_driver
);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Philip");
MODULE_DESCRIPTION(
    "Philip QEMU DMA platform driver with userspace ioctl interface"
);
