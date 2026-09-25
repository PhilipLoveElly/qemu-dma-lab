#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "philip_dma_uapi.h"

#define TEST_LEN    4096U
#define TEST_ROUNDS 1000U

/*
 * 每 16 bytes 包含：
 *   process 識別值、完整輪次編號，以及隨位置改變的資料。
 */
static uint8_t pattern(unsigned int id,
                       unsigned int round,
                       unsigned int offset)
{
    unsigned int slot = offset % 16U;

    if (slot == 0)
        return id == 0 ? 0xA5 : 0x5A;

    if (slot >= 1 && slot <= 4)
        return (uint8_t)(round >> ((slot - 1U) * 8U));

    return (uint8_t)(
        offset * 37U + round * 13U + id * 101U
    );
}

/*
 * 雙向交換一個 byte：
 * 兩個 process 都到達這裡後，才能繼續本輪。
 */
static int rendezvous(int sock)
{
    char token = 'R';
    ssize_t n;

    do {
        n = write(sock, &token, 1);
    } while (n < 0 && errno == EINTR);

    if (n != 1) {
        perror("barrier write");
        return -1;
    }

    do {
        n = read(sock, &token, 1);
    } while (n < 0 && errno == EINTR);

    if (n != 1) {
        if (n == 0)
            fprintf(stderr, "barrier: peer exited\n");
        else
            perror("barrier read");

        return -1;
    }

    return 0;
}

static int worker(unsigned int id, int sock)
{
    const char *name = id == 0 ? "A" : "B";
    struct philip_dma_ioc_transfer *req;
    uint8_t expected[TEST_LEN];
    struct timespec start, end;
    int fd;
    int result = 1;

    /*
     * 避免測試因 barrier 等待而無限掛住。
     * 超過 120 秒會由 SIGALRM 結束，父程序判定失敗。
     */
    alarm(120);

    req = calloc(1, sizeof(*req));
    if (!req) {
        perror("calloc");
        return 1;
    }

    fd = open("/dev/philip_dma", O_RDWR);
    if (fd < 0) {
        perror("open /dev/philip_dma");
        free(req);
        return 1;
    }

    printf("[%s] ready: PID=%ld, len=%u, rounds=%u\n",
           name, (long)getpid(), TEST_LEN, TEST_ROUNDS);

    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
        perror("clock_gettime");
        goto out;
    }

    for (unsigned int round = 0; round < TEST_ROUNDS; round++) {
        memset(req, 0, sizeof(*req));
        req->len = TEST_LEN;

        for (unsigned int i = 0; i < TEST_LEN; i++) {
            expected[i] = pattern(id, round, i);
            req->src[i] = expected[i];

            /* 預填與預期不同的值，避免未更新資料誤判通過。 */
            req->dst[i] = (uint8_t)~expected[i];
        }

        if (rendezvous(sock) < 0) {
            fprintf(stderr, "[%s] FAIL: barrier, round=%u\n",
                    name, round);
            goto out;
        }

        if (ioctl(fd, PHILIP_DMA_IOC_TRANSFER, req) < 0) {
            int saved_errno = errno;

            fprintf(stderr,
                    "[%s] FAIL: ioctl, round=%u, errno=%d (%s)\n",
                    name, round, saved_errno,
                    strerror(saved_errno));
            goto out;
        }

        /*
         * 與獨立保存的 expected 比對，
         * 不依賴 ioctl 回傳後的 req->src。
         */
        for (unsigned int i = 0; i < TEST_LEN; i++) {
            if (req->dst[i] != expected[i]) {
                fprintf(stderr,
                        "[%s] FAIL: round=%u offset=%u "
                        "expected=0x%02x actual=0x%02x\n",
                        name, round, i,
                        (unsigned int)expected[i],
                        (unsigned int)req->dst[i]);
                goto out;
            }
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &end) < 0) {
        perror("clock_gettime");
        goto out;
    }

    double elapsed =
        (double)(end.tv_sec - start.tv_sec) +
        (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;

    printf("[%s] PASS: %u/%u transfers verified, "
           "%u bytes each, elapsed=%.3f s\n",
           name, TEST_ROUNDS, TEST_ROUNDS, TEST_LEN, elapsed);

    result = 0;

out:
    close(fd);
    free(req);
    alarm(0);
    return result;
}

int main(void)
{
    int sockets[2];
    int status;
    int result_a;
    pid_t child;
    pid_t waited;

    setvbuf(stdout, NULL, _IONBF, 0);

    /*
     * 對方失敗關閉 socket 時，
     * write() 回傳 EPIPE，讓程式能印出錯誤。
     */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
        perror("socketpair");
        return 1;
    }

    child = fork();
    if (child < 0) {
        perror("fork");
        close(sockets[0]);
        close(sockets[1]);
        return 1;
    }

    if (child == 0) {
        int result_b;

        close(sockets[0]);
        result_b = worker(1, sockets[1]);
        close(sockets[1]);
        _exit(result_b);
    }

    close(sockets[1]);
    result_a = worker(0, sockets[0]);
    close(sockets[0]);

    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0) {
        perror("waitpid");
        return 1;
    }

    if (!WIFEXITED(status)) {
        if (WIFSIGNALED(status))
            fprintf(stderr, "B terminated by signal %d\n",
                    WTERMSIG(status));

        fprintf(stderr, "OVERALL FAIL\n");
        return 1;
    }

    if (result_a != 0 || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "OVERALL FAIL: A=%d B=%d\n",
                result_a, WEXITSTATUS(status));
        return 1;
    }

    printf("OVERALL PASS: two processes, "
           "2000 transfers, distinct patterns, zero mismatches\n");
    return 0;
}
