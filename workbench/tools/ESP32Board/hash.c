/* Reuse AROS's existing BSD/public-domain SHA-256 implementation rather than
 * introducing a second crypto implementation. Its original notices remain
 * in the included source. The three include directories are in mmakefile. */
#include "../../network/WirelessManager/src/crypto/sha256-internal.c"
#include "image.h"

int board_sha256(const uint8_t *data, size_t size, uint8_t digest[32])
{
    const u8 *vector[1] = {data};
    size_t lengths[1] = {size};
    if ((!data && size) || !digest) return 0;
    return sha256_vector(1, vector, lengths, digest) == 0;
}
