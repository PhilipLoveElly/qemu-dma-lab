#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "philip_dma_uapi.h"

#define REPEATS 100

static const uint32_t lengths[] = {
    1, 2, 3, 4, 7, 8, 9,
    15, 16, 17,
    31, 32, 33,
    63, 64, 65,
    127, 128, 129,
    255, 256, 257,
    511, 512, 513,
    1023, 1024, 1025,
    2047, 2048, 2049,
    4095, 4096
};

static const char *pattern_names[] = {
    "changing-fill",
    "alternating",
    "incrementing",
    "pseudorandom"
};

/* Deterministic: the same inputs generate the same data. */
static void fill_expected(
    uint8_t *buf,
    uint32_t len,
    unsigned int pattern,
    unsigned int iteration
)
{
    uint32_t state =
        0x12345678u ^ len ^
        ((uint32_t)iteration * 0x9e3779b9u);

    for (uint32_t j = 0; j < len; j++) {
        switch (pattern) {
        case 0:
            /* Iteration 0: all 00; iteration 1: all FF. */
            buf[j] = (iteration & 1u)
                ? (uint8_t)(0xffu - iteration / 2u)
                : (uint8_t)(iteration / 2u);
            break;

        case 1:
            buf[j] = (uint8_t)(
                ((j & 1u) ? 0x55u : 0xaau) ^ iteration
            );
            break;

        case 2:
            buf[j] = (uint8_t)(j + iteration);
            break;

        default:
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            buf[j] = (uint8_t)state;
            break;
        }
    }

    /*
     * Ensure consecutive iterations also differ for len == 1
     * in the pseudorandom test.
     */
    if (pattern == 3)
        buf[0] = (uint8_t)iteration;
}

int main(void)
{
    struct philip_dma_ioc_transfer req;
    uint8_t expected[PHILIP_DMA_MAX_LEN];
    unsigned int passed = 0;
    unsigned int combinations = 0;
    int fd;

    fd = open("/dev/philip_dma", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    for (size_t n = 0;
         n < sizeof(lengths) / sizeof(lengths[0]);
         n++) {
        uint32_t len = lengths[n];

        if (len > PHILIP_DMA_MAX_LEN)
            continue;

        for (unsigned int pattern = 0; pattern < 4; pattern++) {
            for (unsigned int iteration = 0;
                 iteration < REPEATS;
                 iteration++) {

                memset(&req, 0, sizeof(req));

                fill_expected(
                    expected, len, pattern, iteration
                );

                req.len = len;
                memcpy(req.src, expected, len);

                /*
                 * Every destination byte initially differs
                 * from its expected value.
                 */
                for (uint32_t j = 0; j < len; j++)
                    req.dst[j] = (uint8_t)~expected[j];

                if (ioctl(
                        fd,
                        PHILIP_DMA_IOC_TRANSFER,
                        &req
                    ) < 0) {
                    perror("transfer ioctl");
                    fprintf(
                        stderr,
                        "FAIL: len=%u pattern=%s iteration=%u\n",
                        len, pattern_names[pattern], iteration
                    );
                    goto fail;
                }

                /*
                 * Compare against a separate expected buffer,
                 * not req.src, which is part of the ioctl data.
                 */
                if (memcmp(expected, req.dst, len) != 0) {
                    for (uint32_t j = 0; j < len; j++) {
                        if (expected[j] != req.dst[j]) {
                            fprintf(
                                stderr,
                                "FAIL: len=%u pattern=%s "
                                "iteration=%u offset=%u "
                                "expected=0x%02x actual=0x%02x\n",
                                len,
                                pattern_names[pattern],
                                iteration,
                                j,
                                (unsigned int)expected[j],
                                (unsigned int)req.dst[j]
                            );
                            break;
                        }
                    }
                    goto fail;
                }

                passed++;
            }

            combinations++;

            printf(
                "PASS: len=%u pattern=%s transfers=%u\n",
                len, pattern_names[pattern], REPEATS
            );
            fflush(stdout);
        }
    }

    printf(
        "\nVERIFY PASS: combinations=%u transfers=%u\n",
        combinations, passed
    );

    close(fd);
    return 0;

fail:
    fprintf(
        stderr,
        "VERIFY FAILED after %u successful transfers\n",
        passed
    );
    close(fd);
    return 1;
}
