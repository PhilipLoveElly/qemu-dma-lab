#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "philip_dma_uapi.h"

static struct philip_dma_ioc_transfer req;

static int normal_transfer(int fd, unsigned int seed)
{
    unsigned int i;

    memset(&req, 0, sizeof(req));
    req.len = PHILIP_DMA_MAX_LEN;

    for (i = 0; i < req.len; i++)
        req.src[i] = (unsigned char)(i * 37u + seed);

    memset(req.dst, 0xa5, sizeof(req.dst));

    if (ioctl(fd, PHILIP_DMA_IOC_TRANSFER, &req) == -1) {
        perror("FAIL: normal transfer");
        return -1;
    }

    /* Check against the original pattern, independently of returned src. */
    for (i = 0; i < PHILIP_DMA_MAX_LEN; i++) {
        unsigned char expected =
            (unsigned char)(i * 37u + seed);

        if (req.dst[i] != expected) {
            fprintf(stderr,
                    "FAIL: byte %u: expected=%02x actual=%02x\n",
                    i, (unsigned int)expected,
                    (unsigned int)req.dst[i]);
            return -1;
        }
    }

    puts("PASS: normal 4096-byte transfer and data verification");
    return 0;
}

static int expect_error(int fd, const char *name,
                        unsigned long cmd, void *arg,
                        int expected_errno)
{
    int ret;
    int saved_errno;

    errno = 0;
    ret = ioctl(fd, cmd, arg);
    saved_errno = errno;

    if (ret != -1 || saved_errno != expected_errno) {
        fprintf(stderr,
                "FAIL: %s: ret=%d errno=%d; expected -1/%d\n",
                name, ret, saved_errno, expected_errno);
        return -1;
    }

    printf("PASS: %s: errno=%d (%s)\n",
           name, saved_errno, strerror(saved_errno));
    return 0;
}

int main(void)
{
    int fd;
    int result = 1;

    setvbuf(stdout, NULL, _IONBF, 0);

    fd = open("/dev/philip_dma", O_RDWR);
    if (fd == -1) {
        perror("open /dev/philip_dma");
        return 1;
    }

    puts("Baseline:");
    if (normal_transfer(fd, 1) != 0)
        goto out;

    puts("\nCase 1: zero length");
    memset(&req, 0, sizeof(req));
    req.len = 0;
    if (expect_error(fd, "zero length",
                     PHILIP_DMA_IOC_TRANSFER, &req, EINVAL) != 0)
        goto out;
    if (normal_transfer(fd, 2) != 0)
        goto out;

    puts("\nCase 2: oversized length");
    memset(&req, 0, sizeof(req));
    req.len = PHILIP_DMA_MAX_LEN + 1;
    if (expect_error(fd, "oversized length",
                     PHILIP_DMA_IOC_TRANSFER, &req, EINVAL) != 0)
        goto out;
    if (normal_transfer(fd, 3) != 0)
        goto out;

    puts("\nCase 3: unknown ioctl");
    if (expect_error(fd, "unknown ioctl",
                     _IO(PHILIP_DMA_IOC_MAGIC, 0x7f),
                     &req, ENOTTY) != 0)
        goto out;
    if (normal_transfer(fd, 4) != 0)
        goto out;

    puts("\nCase 4: NULL request pointer");
    if (expect_error(fd, "NULL request pointer",
                     PHILIP_DMA_IOC_TRANSFER, NULL, EFAULT) != 0)
        goto out;
    if (normal_transfer(fd, 5) != 0)
        goto out;

    puts("\nALL PASS: 4 negative cases + 5 normal transfers");
    result = 0;

out:
    close(fd);
    return result;
}
