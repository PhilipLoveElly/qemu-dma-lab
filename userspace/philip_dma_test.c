#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>

#include "philip_dma_uapi.h"


#define WARMUP_ITERATIONS     100
#define BENCH_ITERATIONS      1000


/*
 * Calculate:
 *
 * end - start
 *
 * and return nanoseconds.
 */
static uint64_t timespec_diff_ns(
    const struct timespec *start,
    const struct timespec *end
)
{
    return
        (uint64_t)(end->tv_sec - start->tv_sec)
        * 1000000000ULL
        +
        (uint64_t)(end->tv_nsec - start->tv_nsec);
}


/*
 * qsort() comparison function.
 *
 * Sort uint64_t values from small to large.
 */
static int compare_u64(
    const void *a,
    const void *b
)
{
    uint64_t va;
    uint64_t vb;

    va = *(const uint64_t *)a;
    vb = *(const uint64_t *)b;


    if (va < vb)
        return -1;

    if (va > vb)
        return 1;

    return 0;
}


/*
 * Calculate percentile using nearest-rank method.
 *
 * Example:
 *
 * 1000 samples
 *
 * P50 -> 500th sample
 * P95 -> 950th sample
 * P99 -> 990th sample
 *
 * The array must already be sorted.
 */
static uint64_t get_percentile_ns(
    const uint64_t *sorted,
    size_t count,
    unsigned int percentile
)
{
    size_t rank;


    if (count == 0)
        return 0;


    if (percentile < 1 ||
        percentile > 100)
        return 0;


    /*
     * ceil(percentile * count / 100)
     *
     * Integer-only implementation.
     */
    rank =
        (
            (size_t)percentile * count
            + 99
        )
        / 100;


    /*
     * rank starts from 1,
     * C array index starts from 0.
     */
    return sorted[rank - 1];
}


static int run_benchmark(
    int fd,
    uint32_t len
)
{
    struct philip_dma_ioc_transfer req;

    struct timespec start;
    struct timespec end;

    uint64_t elapsed_ns;

    uint64_t total_ns = 0;
    uint64_t min_ns = UINT64_MAX;
    uint64_t max_ns = 0;

    /*
     * Store every measured latency.
     *
     * 1000 * 8 bytes = 8000 bytes.
     */
    uint64_t latencies[BENCH_ITERATIONS];

    uint64_t p50_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;

    double avg_ns;
    double avg_us;

    double ops_per_sec;

    double throughput_bytes_per_sec;
    double throughput_mib_per_sec;

    int i;
    int ret;


    /*
     * Validate requested DMA length.
     */
    if (len == 0 ||
        len > PHILIP_DMA_MAX_LEN) {

        fprintf(
            stderr,
            "Invalid length: %u\n",
            len
        );

        return -1;
    }


    memset(
        &req,
        0,
        sizeof(req)
    );


    req.len = len;


    /*
     * Prepare deterministic source data.
     *
     * Example:
     *
     * 00 01 02 03 ...
     */
    for (i = 0; i < (int)len; i++) {

        req.src[i] =
            (uint8_t)(i & 0xff);
    }


    /*
     * ========================================================
     * Warm-up
     * ========================================================
     *
     * These requests are NOT included in the benchmark.
     */
    for (i = 0;
         i < WARMUP_ITERATIONS;
         i++) {

        ret = ioctl(
            fd,
            PHILIP_DMA_IOC_TRANSFER,
            &req
        );


        if (ret < 0) {

            perror(
                "warmup ioctl"
            );

            return -1;
        }


        /*
         * Make sure warm-up DMA is also correct.
         */
        if (memcmp(
                req.src,
                req.dst,
                len
            ) != 0) {

            fprintf(
                stderr,
                "Warm-up DMA verification failed "
                "at iteration %d\n",
                i
            );

            return -1;
        }
    }


    /*
     * ========================================================
     * Benchmark
     * ========================================================
     */
    for (i = 0;
         i < BENCH_ITERATIONS;
         i++) {

        /*
         * Start timestamp.
         */
        if (clock_gettime(
                CLOCK_MONOTONIC_RAW,
                &start
            ) != 0) {

            perror(
                "clock_gettime start"
            );

            return -1;
        }


        /*
         * One complete DMA transaction:
         *
         * userspace
         *   ↓
         * ioctl
         *   ↓
         * driver
         *   ↓
         * DMA
         *   ↓
         * IRQ
         *   ↓
         * completion
         *   ↓
         * ioctl returns
         */
        ret = ioctl(
            fd,
            PHILIP_DMA_IOC_TRANSFER,
            &req
        );


        /*
         * End timestamp.
         */
        if (clock_gettime(
                CLOCK_MONOTONIC_RAW,
                &end
            ) != 0) {

            perror(
                "clock_gettime end"
            );

            return -1;
        }


        if (ret < 0) {

            perror(
                "ioctl"
            );

            return -1;
        }


        /*
         * Verify DMA data.
         */
        if (memcmp(
                req.src,
                req.dst,
                len
            ) != 0) {

            fprintf(
                stderr,
                "DMA verification failed "
                "at iteration %d\n",
                i
            );

            return -1;
        }


        /*
         * Calculate this transaction latency.
         */
        elapsed_ns =
            timespec_diff_ns(
                &start,
                &end
            );


        /*
         * Store every sample.
         *
         * This is new in Benchmark v3.
         */
        latencies[i] =
            elapsed_ns;


        /*
         * Accumulate for average.
         */
        total_ns +=
            elapsed_ns;


        /*
         * Minimum.
         */
        if (elapsed_ns < min_ns)
            min_ns =
                elapsed_ns;


        /*
         * Maximum.
         */
        if (elapsed_ns > max_ns)
            max_ns =
                elapsed_ns;
    }


    /*
     * ========================================================
     * Sort latency samples
     * ========================================================
     *
     * Before:
     *
     * 3000
     * 1200
     * 8000
     * 1500
     *
     * After:
     *
     * 1200
     * 1500
     * 3000
     * 8000
     */
    qsort(
        latencies,
        BENCH_ITERATIONS,
        sizeof(latencies[0]),
        compare_u64
    );


    /*
     * ========================================================
     * Calculate percentiles
     * ========================================================
     */
    p50_ns =
        get_percentile_ns(
            latencies,
            BENCH_ITERATIONS,
            50
        );


    p95_ns =
        get_percentile_ns(
            latencies,
            BENCH_ITERATIONS,
            95
        );


    p99_ns =
        get_percentile_ns(
            latencies,
            BENCH_ITERATIONS,
            99
        );


    /*
     * ========================================================
     * Average latency
     * ========================================================
     */
    avg_ns =
        (double)total_ns
        /
        BENCH_ITERATIONS;


    avg_us =
        avg_ns
        /
        1000.0;


    /*
     * ========================================================
     * Transactions per second
     * ========================================================
     */
    ops_per_sec =
        1000000000.0
        /
        avg_ns;


    /*
     * ========================================================
     * Effective payload throughput
     * ========================================================
     *
     * Each operation transfers:
     *
     * len bytes
     */
    throughput_bytes_per_sec =
        (double)len
        *
        ops_per_sec;


    throughput_mib_per_sec =
        throughput_bytes_per_sec
        /
        (1024.0 * 1024.0);


    /*
     * ========================================================
     * Print benchmark result
     * ========================================================
     */
    printf(
        "\n"
        "========================================\n"
        " Philip DMA Benchmark\n"
        "========================================\n"
    );


    printf(
        "Transfer size : %u bytes\n",
        len
    );


    printf(
        "Iterations    : %d\n",
        BENCH_ITERATIONS
    );


    printf(
        "Average       : %.3f us\n",
        avg_us
    );


    printf(
        "Minimum       : %.3f us\n",
        (double)min_ns
        /
        1000.0
    );


    printf(
        "P50           : %.3f us\n",
        (double)p50_ns
        /
        1000.0
    );


    printf(
        "P95           : %.3f us\n",
        (double)p95_ns
        /
        1000.0
    );


    printf(
        "P99           : %.3f us\n",
        (double)p99_ns
        /
        1000.0
    );


    printf(
        "Maximum       : %.3f us\n",
        (double)max_ns
        /
        1000.0
    );


    printf(
        "Operations/s  : %.2f\n",
        ops_per_sec
    );


    printf(
        "Throughput    : %.3f MiB/s\n",
        throughput_mib_per_sec
    );


    printf(
        "========================================\n"
    );


    return 0;
}


int main(
    int argc,
    char *argv[]
)
{
    int fd;

    uint32_t len;


    /*
     * Default DMA length.
     */
    len = 16;


    /*
     * Example:
     *
     * ./philip_dma_test 64
     */
    if (argc >= 2) {

        len =
            (uint32_t)strtoul(
                argv[1],
                NULL,
                0
            );
    }


    /*
     * Validate length before opening device.
     */
    if (len == 0 ||
        len > PHILIP_DMA_MAX_LEN) {

        fprintf(
            stderr,
            "Transfer length must be 1..%d\n",
            PHILIP_DMA_MAX_LEN
        );

        return 1;
    }


    /*
     * Open DMA character device.
     */
    fd = open(
        "/dev/philip_dma",
        O_RDWR
    );


    if (fd < 0) {

        perror(
            "open"
        );

        return 1;
    }


    /*
     * Run benchmark.
     */
    if (run_benchmark(
            fd,
            len
        ) != 0) {

        close(fd);

        return 1;
    }


    close(fd);


    return 0;
}
