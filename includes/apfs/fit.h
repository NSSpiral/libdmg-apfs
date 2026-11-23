#ifndef APFS_FIT_H
#define APFS_FIT_H
#include <stddef.h>
#include <stdint.h>
typedef size_t (*apfs_chunk_encoder)(int, const uint8_t *, size_t, uint8_t *, size_t, int);
typedef size_t (*apfs_chunk_cache)(int, const uint8_t *, size_t, uint8_t *, size_t, int, apfs_chunk_encoder);
extern apfs_chunk_cache apfs_compress_cache;
int apfs_fit_command(int argc, char **argv);
#endif
