/* Memory-backed native allocation planning and checked offline shrinking. */
#include "apfs/fit.h"
#include "apfs/apfs.h"
#include "apfs/image.h"
#include "apfs/manifest.h"
#ifdef _WIN32
int apfs_fit_command(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 1;
}
#else
#include "../common/image_fit.h"
#include <json-c/json.h>
/* Shared across sequential planning children. Content hashes bind cached
 * compressed chunks to their source bytes and exact encoding parameters. */
#define CACHE_SLOTS 16384
#define CACHE_BYTES (128U * 1024 * 1024)
typedef struct {
    uint8_t digest[32];
    uint32_t offset, stored, input, capacity;
    int algo, exact, valid;
} chunk_entry;
typedef struct {
    size_t used;
    chunk_entry entries[CACHE_SLOTS];
    uint8_t bytes[CACHE_BYTES];
} chunk_cache;
static chunk_cache *cache;
static size_t cached_chunk(int algo, const uint8_t *src, size_t len, uint8_t *dst, size_t cap, int exact,
                           apfs_chunk_encoder compute) {
    uint8_t digest[32];
    uint32_t hash;
    size_t i, n;
    chunk_entry *slot = NULL;
    apfs_sha256(src, len, digest);
    memcpy(&hash, digest, sizeof(hash));
    for (i = 0; i < CACHE_SLOTS; i++) {
        chunk_entry *entry = &cache->entries[(hash + i) % CACHE_SLOTS];
        if (!entry->valid) {
            slot = entry;
            break;
        }
        if (entry->algo == algo && entry->exact == exact && entry->input == len && entry->capacity == cap &&
            !memcmp(entry->digest, digest, 32)) {
            memcpy(dst, cache->bytes + entry->offset, entry->stored);
            return entry->stored;
        }
    }
    n = compute(algo, src, len, dst, cap, exact);
    if (slot && n && n <= CACHE_BYTES - cache->used) {
        memcpy(slot->digest, digest, 32);
        slot->offset = (uint32_t)cache->used;
        slot->stored = (uint32_t)n;
        slot->input = (uint32_t)len;
        slot->capacity = (uint32_t)cap;
        slot->algo = algo;
        slot->exact = exact;
        memcpy(cache->bytes + cache->used, dst, n);
        cache->used += n;
        slot->valid = 1;
    }
    return n;
}
typedef struct {
    const char *tree, *manifest;
    int portable;
    apfs_mkfs_params_t mkfs;
    apfs_vol_params_t volume;
    char name[256];
    uint8_t uuid[16];
} plan_context;
static int build_image(void *data, uint64_t size, void *opaque, uint64_t *free_bytes) {
    plan_context *p = opaque;
    io_func *io = fit_io(data, size);
    apfs_nx_t *nx = NULL;
    apfs_vol_t *vol = NULL;
    int ret;
    if (!io)
        return 1;
    p->mkfs.size = size;
    p->mkfs.nvols = 1;
    p->mkfs.vols = &p->volume;
    ret = apfs_mkfs(io, 0, &p->mkfs);
    if (!ret)
        ret = apfs_nx_open(&nx, io, 0, 1);
    if (!ret)
        ret = apfs_vol_open(nx, 0, &vol);
    if (!ret)
        ret = p->portable ? apfs_metadata_import(vol, p->tree, p->manifest)
                          : apfs_manifest_import(vol, p->tree, p->manifest);
    if (!ret)
        ret = apfs_nx_commit(nx);
    if (nx) {
        nx->writable = 0;
        apfs_nx_close(nx);
        nx = NULL;
    }
    if (!ret)
        ret = apfs_nx_open(&nx, io, 0, 0);
    if (!ret)
        ret = apfs_sm_load(nx);
    if (!ret)
        *free_bytes = apfs_sm_free_blocks(nx);
    if (!ret)
        *free_bytes *= nx->bs;
    if (nx)
        apfs_nx_close(nx);
    CLOSE(io);
    return ret == APFS_ENOSPC ? 2 : ret < 0 ? 1 : 0;
}
static int verify_image(fit_image *image) {
    io_func *io = fit_io(image->data, image->size);
    apfs_nx_t *nx = NULL;
    int ret;
    if (!io)
        return 1;
    ret = apfs_nx_open(&nx, io, 0, 0);
    if (!ret)
        ret = apfs_verify(nx, 0);
    if (nx)
        apfs_nx_close(nx);
    CLOSE(io);
    return ret < 0;
}
static int getnum(json_object *object, const char *key, uint64_t *out) {
    json_object *v;
    if (!json_object_object_get_ex(object, key, &v) || !json_object_is_type(v, json_type_int) ||
        json_object_get_int64(v) < 0)
        return 1;
    *out = json_object_get_uint64(v);
    return 0;
}
static int load_config(plan_context *p, const char *path) {
    json_object *doc = json_object_from_file(path), *v, *name;
    uint64_t bs, flags, role;
    const char *hex;
    size_t i;
    if (!doc)
        return 1;
    if (getnum(doc, "block_size", &bs) || bs < NX_MINIMUM_BLOCK_SIZE || bs > NX_MAXIMUM_BLOCK_SIZE ||
        (bs & (bs - 1)) || !json_object_object_get_ex(doc, "volume", &v) ||
        getnum(v, "apfs_incompatible_features", &flags) || getnum(v, "apfs_role", &role) ||
        role > UINT16_MAX || !json_object_object_get_ex(v, "apfs_volname", &name))
        goto bad;
    if (flags & ~(uint64_t)(APFS_INCOMPAT_CASE_INSENSITIVE | APFS_INCOMPAT_NORMALIZATION_INSENSITIVE))
        goto bad;
    hex = json_object_get_string(name);
    if (!hex || strlen(hex) != 512)
        goto bad;
    for (i = 0; i < 256; i++) {
        unsigned n;
        char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        if (strspn(pair, "0123456789abcdefABCDEF") != 2 || sscanf(pair, "%x", &n) != 1)
            goto bad;
        p->name[i] = (char)n;
    }
    if (!memchr(p->name, 0, sizeof(p->name)))
        goto bad;
    p->mkfs.block_size = (uint32_t)bs;
    p->volume.name = p->name;
    p->volume.role = (uint16_t)role;
    p->volume.case_sensitive = !(flags & APFS_INCOMPAT_CASE_INSENSITIVE);
    p->volume.norm_insensitive = !!(flags & APFS_INCOMPAT_NORMALIZATION_INSENSITIVE);
    json_object_put(doc);
    return 0;
bad:
    json_object_put(doc);
    fprintf(stderr, "invalid native APFS format parameters\n");
    return 1;
}
static int shrink_image(int argc, char **argv) {
    const char *path = argv[1];
    uint64_t size;
    struct stat original;
    apfs_image_t *source = NULL;
    apfs_vol_t *volume = NULL;
    char *workspace = NULL, *tree = NULL, *manifest = NULL;
    plan_context p = {0};
    fit_image image = {0};
    int ret = 1;
    if (argc != 4 || !fit_size(argv[3], &size) || size < 64 * FIT_MIB || size > FIT_LIMIT ||
        lstat(path, &original) || (!S_ISREG(original.st_mode) || original.st_nlink != 1) ||
        size >= (uint64_t)original.st_size)
        goto done;
    if (apfs_image_open(path, 0, &source) < 0)
        goto done;
    if (source->format != APFS_IMG_RAW || source->base || source->length != (uint64_t)original.st_size ||
        apfs_nx_volume_count(source->nx) != 1 || size % source->nx->bs || apfs_verify(source->nx, 0) < 0)
        goto done;
    if (apfs_vol_open(source->nx, 0, &volume) < 0)
        goto done;
    workspace = fit_workspace("apfs-shrink");
    if (!workspace)
        goto done;
    tree = malloc(strlen(workspace) + 16);
    manifest = malloc(strlen(workspace) + 24);
    if (!tree || !manifest)
        goto done;
    sprintf(tree, "%s/tree", workspace);
    sprintf(manifest, "%s/manifest.json", workspace);
    if (apfs_manifest_export(volume, tree, manifest) < 0)
        goto done;
    p.tree = tree;
    p.manifest = manifest;
    if (load_config(&p, manifest))
        goto done;
    memcpy(p.uuid, source->nx->sb->nx_uuid, 16);
    p.mkfs.uuid = p.uuid;
    if (fit_probe(size, build_image, &p, &image) || verify_image(&image))
        goto done;
    /* Export again and compare the complete allocation-independent native
     * graph, stream hashes, volume properties and hardlink topology. */
    {
        io_func *io = fit_io(image.data, image.size);
        apfs_nx_t *nx = NULL;
        apfs_vol_t *v = NULL;
        char *checktree = malloc(strlen(workspace) + 24), *checkjson = malloc(strlen(workspace) + 32);
        json_object *a = NULL, *b = NULL;
        int same = 0;
        if (io && checktree && checkjson && !apfs_nx_open(&nx, io, 0, 0) && !apfs_vol_open(nx, 0, &v)) {
            sprintf(checktree, "%s/verification", workspace);
            sprintf(checkjson, "%s/verification.json", workspace);
            if (!apfs_manifest_export(v, checktree, checkjson)) {
                a = json_object_from_file(manifest);
                b = json_object_from_file(checkjson);
                same = a && b && json_object_equal(a, b);
            }
        }
        if (a)
            json_object_put(a);
        if (b)
            json_object_put(b);
        if (nx)
            apfs_nx_close(nx);
        if (io)
            CLOSE(io);
        free(checktree);
        free(checkjson);
        if (!same) {
            fprintf(stderr, "shrink changed native filesystem metadata or contents\n");
            goto done;
        }
    }
    ret = fit_publish(path, &image, &original);
    if (!ret)
        fit_report(&image);
done:
    if (source)
        apfs_image_close(source, 0);
    fit_release(&image);
    if (workspace) {
        if (fit_remove_tree(workspace))
            fprintf(stderr, "cannot remove shrink workspace %s\n", workspace);
        free(workspace);
    }
    free(tree);
    free(manifest);
    if (ret)
        fprintf(stderr, "shrink failed; source image was not replaced (requires a bare, unencrypted, "
                        "unsealed single volume without snapshots; 64 MiB..2 GiB)\n");
    return ret;
}
static int fit_usage(void) {
    fprintf(stderr, "usage: apfs-tool IMAGE fit TREE --manifest JSON [--size N] [--reserve N] [--maximum N]\n"
                    "       apfs-tool IMAGE fit TREE --metadata JSON --like NATIVE [--size N] [--reserve N] "
                    "[--maximum N]\n");
    return 1;
}
int apfs_fit_command(int argc, char **argv) {
    plan_context p = {0};
    fit_image image = {0};
    const char *like = NULL;
    fit_options options = {2 * FIT_MIB, 0, FIT_LIMIT, 0};
    int i, ret = 1;
    if (argc >= 3 && !strcmp(argv[2], "shrink"))
        return shrink_image(argc, argv);
    struct stat destination;
    if (argc < 6)
        return fit_usage();
    if (!lstat(argv[1], &destination) || errno != ENOENT) {
        fprintf(stderr, "fit output must be a new path\n");
        return 1;
    }
    p.tree = argv[3];
    for (i = 4; i < argc; i += 2) {
        if (i + 1 >= argc)
            return fit_usage();
        if (!strcmp(argv[i], "--manifest")) {
            if (p.manifest)
                return fit_usage();
            p.manifest = argv[i + 1];
            p.portable = 0;
        } else if (!strcmp(argv[i], "--metadata")) {
            if (p.manifest)
                return fit_usage();
            p.manifest = argv[i + 1];
            p.portable = 1;
        } else if (!strcmp(argv[i], "--like")) {
            if (like)
                return fit_usage();
            like = argv[i + 1];
        } else if (fit_option(&options, argv[i], argv[i + 1]) != 1)
            return fit_usage();
    }
    if (!p.manifest || (p.portable != (like != NULL)))
        return fit_usage();
    if (!fit_options_valid(&options, 64 * FIT_MIB) || load_config(&p, like ? like : p.manifest))
        return 1;
    uint64_t reserve = options.reserve, exact = options.exact, maximum = options.maximum;
    if (p.portable) {
        cache = mmap(NULL, sizeof(*cache), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
        if (cache == MAP_FAILED) {
            cache = NULL;
            perror("allocate compression cache");
            return 1;
        }
        apfs_compress_cache = cached_chunk;
    }
    int planned = 1;
    uint64_t start = 64 * FIT_MIB, end = maximum < FIT_LIMIT ? maximum : FIT_LIMIT;
    end -= end % FIT_MIB;
    if (exact)
        planned = fit_plan(build_image, &p, start, maximum, exact, reserve, &image);
    else if (end >= start) {
        uint64_t previous = start / p.mkfs.block_size - apfs_mkfs_reserved_blocks(start, p.mkfs.block_size);
        for (uint64_t size = start + FIT_MIB;; size += FIT_MIB) {
            uint64_t available =
                size / p.mkfs.block_size - apfs_mkfs_reserved_blocks(size, p.mkfs.block_size);
            if (size > end || available < previous) {
                planned = fit_plan(build_image, &p, start, size - FIT_MIB, 0, reserve, &image);
                if (planned != 2 || size > end)
                    break;
                fit_release(&image);
                start = size;
            }
            previous = available;
        }
    }
    if (planned == 2)
        fprintf(stderr, "payload cannot fit with the required free-space reserve\n");
    if (!planned && !verify_image(&image)) {
        ret = fit_publish(argv[1], &image, NULL);
        if (!ret)
            fit_report(&image);
    }
    fit_release(&image);
    apfs_compress_cache = NULL;
    if (cache) {
        munmap(cache, sizeof(*cache));
        cache = NULL;
    }
    return ret;
}
#endif
