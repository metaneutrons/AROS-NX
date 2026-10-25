#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define min(a, b) ((a) < (b) ? (a) : (b))
#define ESP_LOGE(...) ((void)0)
#define FAILURE 1
#define ESP_OK 0

struct bytes { uint8_t *data; size_t len; };
struct response { int resp; };

static int encode(struct bytes *dest, const char *src, size_t width)
{
    struct response response = {0};
    struct response *resp_payload = &response;
    RPC_RESP_COPY_STR((*dest), src, width);
    return response.resp;
}

int main(void)
{
    const size_t widths[] = {32, 64};
    unsigned checks = 0;
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        size_t width = widths[i];
        /* Exact-size heap allocation: ASan catches reads past full fields. */
        char *src = malloc(width);
        assert(src);
        for (size_t length = 0; length <= width; ++length) {
            memset(src, 'A', width);
            if (length < width) src[length] = 0;
            struct bytes dest = {0};
            assert(encode(&dest, src, width) == 0);
            assert(dest.data);
            assert(dest.len == min(width, length + 1));
            assert(memcmp(dest.data, src, length) == 0);
            assert(dest.data[length] == 0);
            free(dest.data);
            checks += 5;
        }
        free(src);
    }
    printf("c6-string-response: %u checks, 0 failures\n", checks);
    return 0;
}
