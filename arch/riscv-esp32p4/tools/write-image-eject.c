/*
 * Write a complete image prefix to a raw macOS disk and eject the medium
 * before closing the exclusive device descriptor.  Keeping the descriptor
 * open across DKIOCEJECT closes the Disk Arbitration automount window that
 * otherwise lets macOS create .fseventsd between dd(1) and diskutil eject.
 *
 * The caller must first unmount the exact, independently identified medium.
 * A required expected-capacity argument makes a stale /dev/rdiskN name fail
 * closed when it resolves to a differently sized device.
 */

#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COPY_CHUNK (4U * 1024U * 1024U)

static void fail(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}

static uint64_t parse_capacity(const char *text)
{
    char *end = NULL;
    uintmax_t value;

    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT64_MAX) {
        fprintf(stderr, "invalid expected capacity: %s\n", text);
        exit(2);
    }
    return (uint64_t)value;
}

static void write_full(int fd, const void *buffer, size_t length,
    uint64_t offset)
{
    const unsigned char *cursor = buffer;

    while (length != 0) {
        ssize_t written = pwrite(fd, cursor, length, (off_t)offset);
        if (written < 0)
            fail("write raw device");
        if (written == 0) {
            errno = EIO;
            fail("short write to raw device");
        }
        cursor += (size_t)written;
        length -= (size_t)written;
        offset += (uint64_t)written;
    }
}

int main(int argc, char **argv)
{
    struct stat image_stat;
    struct stat target_stat;
    uint64_t expected_capacity;
    uint64_t block_count;
    uint64_t device_capacity;
    uint64_t offset = 0;
    uint32_t block_size;
    uint32_t writable;
    unsigned char *buffer;
    int image_fd;
    int target_fd;

    if (argc != 4) {
        fprintf(stderr,
            "usage: %s IMAGE /dev/rdiskN EXPECTED_DEVICE_BYTES\n",
            argv[0]);
        return 2;
    }
    if (strncmp(argv[2], "/dev/rdisk", 10) != 0) {
        fprintf(stderr, "target must be an explicit /dev/rdiskN path\n");
        return 2;
    }
    expected_capacity = parse_capacity(argv[3]);

    image_fd = open(argv[1], O_RDONLY);
    if (image_fd < 0)
        fail("open image");
    if (fstat(image_fd, &image_stat) != 0)
        fail("stat image");
    if (!S_ISREG(image_stat.st_mode) || image_stat.st_size <= 0) {
        fprintf(stderr, "image must be a non-empty regular file\n");
        return 2;
    }

    target_fd = open(argv[2], O_WRONLY | O_EXCL);
    if (target_fd < 0)
        fail("open raw device exclusively");
    if (fstat(target_fd, &target_stat) != 0)
        fail("stat raw device");
    if (!S_ISCHR(target_stat.st_mode)) {
        fprintf(stderr, "target is not a raw character device\n");
        return 2;
    }
    if (ioctl(target_fd, DKIOCGETBLOCKSIZE, &block_size) != 0)
        fail("get device block size");
    if (ioctl(target_fd, DKIOCGETBLOCKCOUNT, &block_count) != 0)
        fail("get device block count");
    if (ioctl(target_fd, DKIOCISWRITABLE, &writable) != 0)
        fail("get device write state");

    if (block_size == 0 || block_count > UINT64_MAX / block_size) {
        fprintf(stderr, "invalid device geometry\n");
        return 2;
    }
    device_capacity = block_count * block_size;
    if (device_capacity != expected_capacity) {
        fprintf(stderr,
            "device capacity mismatch: got %" PRIu64 ", expected %" PRIu64
            "\n", device_capacity, expected_capacity);
        return 2;
    }
    if (writable == 0) {
        fprintf(stderr, "device reports write protected\n");
        return 2;
    }
    if ((uint64_t)image_stat.st_size > device_capacity ||
        (uint64_t)image_stat.st_size % block_size != 0) {
        fprintf(stderr,
            "image size %" PRIu64 " is invalid for %" PRIu32
            "-byte blocks and %" PRIu64 "-byte device\n",
            (uint64_t)image_stat.st_size, block_size, device_capacity);
        return 2;
    }

#ifdef F_NOCACHE
    if (fcntl(target_fd, F_NOCACHE, 1) != 0)
        fail("disable device caching");
#endif
    buffer = malloc(COPY_CHUNK);
    if (buffer == NULL)
        fail("allocate copy buffer");

    while (offset < (uint64_t)image_stat.st_size) {
        size_t wanted = COPY_CHUNK;
        ssize_t got;

        if (wanted > (uint64_t)image_stat.st_size - offset)
            wanted = (size_t)((uint64_t)image_stat.st_size - offset);
        got = read(image_fd, buffer, wanted);
        if (got < 0)
            fail("read image");
        if (got == 0) {
            errno = EIO;
            fail("short read from image");
        }
        write_full(target_fd, buffer, (size_t)got, offset);
        offset += (uint64_t)got;
    }

    if (fsync(target_fd) != 0)
        fail("fsync raw device");
    if (ioctl(target_fd, DKIOCSYNCHRONIZECACHE) != 0)
        fail("synchronize device cache");
    if (ioctl(target_fd, DKIOCEJECT) != 0)
        fail("eject raw device");

    printf("wrote and ejected %" PRIu64 " bytes on %" PRIu64
        "-byte device\n", offset, device_capacity);
    free(buffer);
    close(target_fd);
    close(image_fd);
    return 0;
}
