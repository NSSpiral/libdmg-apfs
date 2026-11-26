/* Native, driverless APFS interchange. Physical addresses and checkpoint
 * history are rebuilt; logical catalog records, IDs and stream layouts remain.
 * Host ownership, flags and xattrs are never authoritative. */
#include "apfs/manifest.h"
#include "apfs/apfslib.h"
#ifndef _WIN32
#include <json-c/json.h>
#endif
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <unistd.h>
#include "compat_macos.h"

#ifdef _WIN32
int apfs_manifest_inventory(apfs_vol_t *v, const char *m) { return APFS_EUNSUPP; }
int apfs_manifest_inventory_metadata(apfs_vol_t *v, const char *m) { return APFS_EUNSUPP; }
int apfs_manifest_extractfiles(apfs_vol_t *v, const char *t, const char *r) { return APFS_EUNSUPP; }
int apfs_manifest_export(apfs_vol_t *v, const char *t, const char *m) { return APFS_EUNSUPP; }
int apfs_manifest_import(apfs_vol_t *v, const char *t, const char *m) { return APFS_EUNSUPP; }
int apfs_manifest_validate(const char *t, const char *m) { return APFS_EUNSUPP; }
int apfs_metadata_import(apfs_vol_t *v, const char *t, const char *m) { return APFS_EUNSUPP; }
#else

typedef json_object J;
typedef struct Context {
    jmp_buf error;
    apfs_vol_t *vol;
    J *doc, *nodes, *streams, *records, *inodes, *stream_index, *record_index, *stream_refs;
    int tree, storage, inventory, metadata_only;
    J *inline_streams;
    char *request_line;
    size_t request_capacity;
    apfs_kv_t *kv;
    size_t count;
    size_t metadata_count;
    int status;
} Context;

static void check(Context *c, int ok, const char *why);
static void result(Context *c, int ret, const char *why);
static J *field(Context *c, J *j, const char *key, enum json_type type);
static Context *context_new(void);
static void context_free(Context *c);

static int b64value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t *base64(Context *c, const char *s, size_t *size) {
    size_t i, length = strlen(s), output = 0;
    uint8_t *data;
    check(c, !(length & 3) && length <= 64 * 1024 * 1024, "invalid base64 length");
    data = apfs_malloc(length / 4 * 3 + 1);
    for (i = 0; i < length; i += 4) {
        int a = b64value((unsigned char)s[i]);
        int b = b64value((unsigned char)s[i + 1]);
        int d = s[i + 2] == '=' ? -2 : b64value((unsigned char)s[i + 2]);
        int e = s[i + 3] == '=' ? -2 : b64value((unsigned char)s[i + 3]);
        check(c, a >= 0 && b >= 0 && (d >= 0 || d == -2) && (e >= 0 || e == -2), "invalid base64 data");
        check(c, d != -2 || e == -2, "invalid base64 padding");
        check(c, (d != -2 && e != -2) || i + 4 == length, "nonfinal base64 padding");
        data[output++] = (uint8_t)((a << 2) | (b >> 4));
        if (d != -2) {
            check(c, d >= 0, "invalid base64 data");
            data[output++] = (uint8_t)((b << 4) | (d >> 2));
            if (e != -2) {
                check(c, e >= 0, "invalid base64 data");
                data[output++] = (uint8_t)((d << 6) | e);
            } else {
                check(c, !(d & 3), "noncanonical base64 padding");
            }
        } else {
            check(c, !(b & 15), "noncanonical base64 padding");
        }
    }
    *size = output;
    return data;
}

static char *portable_path(Context *c, const char *name) {
    const char *part;
    char *path;
    size_t length;
    if (!strcmp(name, "."))
        return apfs_strdup("/");
    length = strlen(name);
    check(c, length && length < 4095 && name[0] != '/' && name[length - 1] != '/', "unsafe metadata path");
    for (part = name; *part;) {
        const char *slash = strchr(part, '/');
        size_t n = slash ? (size_t)(slash - part) : strlen(part);
        check(c, n && !(n == 1 && part[0] == '.') && !(n == 2 && part[0] == '.' && part[1] == '.'),
              "unsafe metadata path component");
        if (!slash)
            break;
        part = slash + 1;
    }
    path = apfs_malloc(length + 2);
    path[0] = '/';
    memcpy(path + 1, name, length + 1);
    return path;
}

static uint64_t optional_number(Context *c, J *j, const char *key, int required, uint64_t fallback) {
    J *v;
    if (!json_object_object_get_ex(j, key, &v)) {
        check(c, !required, key);
        return fallback;
    }
    check(c, json_object_is_type(v, json_type_int) && json_object_get_int64(v) >= 0, key);
    return json_object_get_uint64(v);
}

static int portable_layout(Context *c, J *entry, uint32_t *type, uint64_t *resource_size) {
    J *value;
    int present = json_object_object_get_ex(entry, "compression_type", &value);
    uint64_t number = optional_number(c, entry, "compression_type", 0, 0);
    *resource_size = optional_number(c, entry, "resource_fork_size", 0, UINT64_MAX);
    check(c, present || *resource_size == UINT64_MAX, "resource fork size requires compression_type");
    if (!present) { *type = 0; return 0; }
    check(c, number == 3 || number == 4 || number == 7 || number == 8 || number == 9 ||
             number == 10 || number == 11 || number == 12, "unsupported compression_type");
    check(c, optional_number(c, entry, "type", 1, 0) == APFS_S_IFREG &&
             (optional_number(c, entry, "flags", 0, 0) & APFS_UF_COMPRESSED),
             "compression layout requires a compressed regular file");
    check(c, *resource_size == UINT64_MAX || *resource_size <= INT32_MAX,
             "resource fork size exceeds 2 GiB");
    check(c, !(number & 1) || *resource_size == UINT64_MAX || *resource_size == 0,
             "inline compression cannot have a resource fork");
    *type = (uint32_t)number;
    return 1;
}

static void portable_compress(Context *c, uint64_t ino, int compression, const char *path,
        uint32_t requested_type, uint64_t requested_resource_size) {
    apfs_inode_t inode;
    uint8_t *data;
    uint64_t size;
    size_t got;
    int ret = apfs_inode_read(c->vol, ino, &inode);
    result(c, ret, "compressed metadata inode read");
    check(c, (inode.mode & APFS_S_IFMT) == APFS_S_IFREG, "compressed metadata belongs to a nonregular file");
    if (inode.bsd_flags & APFS_UF_COMPRESSED) {
        apfs_inode_release(&inode);
        if (requested_type) {
            uint8_t *data; size_t length;
            result(c, apfs_xattr_get(c->vol, ino, DECMPFS_EA_NAME, &data, &length), "compression layout header");
            int matches = length >= 16 && apfs_get32(data) == DECMPFS_MAGIC && apfs_get32(data + 4) == requested_type;
            free(data);
            check(c, matches, "compression_type disagrees across hardlink aliases");
            if (!(requested_type & 1) && requested_resource_size != UINT64_MAX) {
                uint16_t flags, size;
                result(c, apfs_xattr_raw(c->vol, ino, RESOURCEFORK_EA_NAME, &flags, &data, &size), "compression layout resource fork");
                uint64_t actual_size = size;
                if (flags & XATTR_DATA_STREAM) {
                    check(c, size == sizeof(j_xattr_dstream_t), "invalid resource fork stream");
                    actual_size = apfs_get64(data + 8);
                }
                free(data);
                check(c, actual_size == requested_resource_size, "resource_fork_size disagrees across hardlink aliases");
            }
        }
        return;
    }
    result(c, apfs_file_size(c->vol, &inode, &size), "compressed metadata file size");
    check(c, size <= ((uint64_t)1 << 31), "compressed metadata file exceeds 2 GiB");
    data = apfs_malloc((size_t)size + 1);
    ret = apfs_file_read(c->vol, &inode, 0, data, (size_t)size, &got);
    apfs_inode_release(&inode);
    result(c, ret, "compressed metadata file read");
    check(c, got == size, "short compressed metadata file read");
    ret = requested_type ? apfs_file_write_compressed_type(c->vol, ino, data, (size_t)size,
        requested_type, requested_resource_size) : apfs_file_write_compressed(c->vol, ino, data, (size_t)size, compression);
    free(data);
    if (ret != APFS_OK) fprintf(stderr, "error: compressing %s: %s (%d)\n", path, apfs_strerror(ret), ret);
    result(c, ret, "metadata compression import");
}

static void portable_entry(Context *c, const char *name, J *entry, int compression) {
    apfs_inode_t inode;
    apfs_setattr_t attributes;
    J *xattrs;
    uint64_t ino, type, mode, uid, gid, mtime, flags;
    char *path = portable_path(c, name);
    int mask;
    uint32_t requested_type;
    uint64_t requested_resource_size;

    check(c, json_object_is_type(entry, json_type_object), "metadata entry");
    type = optional_number(c, entry, "type", 1, 0);
    mode = optional_number(c, entry, "mode", 1, 0);
    uid = optional_number(c, entry, "uid", 1, 0);
    gid = optional_number(c, entry, "gid", 1, 0);
    mtime = optional_number(c, entry, "mtime_ns", 1, 0);
    flags = optional_number(c, entry, "flags", 0, 0);
    check(c, type <= UINT16_MAX && mode <= 07777 && uid <= UINT32_MAX && gid <= UINT32_MAX &&
              flags <= UINT32_MAX, "metadata integer range");
    result(c, apfs_lookup(c->vol, path, 0, &ino, NULL, NULL), "metadata path missing from image");
    result(c, apfs_inode_read(c->vol, ino, &inode), "metadata inode read");
    check(c, (inode.mode & APFS_S_IFMT) == type, "metadata type differs from imported tree");
    apfs_inode_release(&inode);

    portable_layout(c, entry, &requested_type, &requested_resource_size);
    if (flags & APFS_UF_COMPRESSED)
        portable_compress(c, ino, compression, path, requested_type, requested_resource_size);

    if (json_object_object_get_ex(entry, "xattrs", &xattrs)) {
        check(c, json_object_is_type(xattrs, json_type_object), "metadata xattrs");
        json_object_object_foreach(xattrs, xname, encoded) {
            size_t size;
            uint8_t *data;
            check(c, *xname && strlen(xname) < 256 && !strchr(xname, '/'), "invalid xattr name");
            check(c, strcmp(xname, DECMPFS_EA_NAME) && strcmp(xname, SYMLINK_EA_NAME),
                  "metadata contains a filesystem-owned xattr");
            check(c, !(flags & APFS_UF_COMPRESSED) || strcmp(xname, RESOURCEFORK_EA_NAME),
                  "compressed metadata contains a conflicting resource fork");
            check(c, json_object_is_type(encoded, json_type_string), "xattr value is not base64 text");
            data = base64(c, json_object_get_string(encoded), &size);
            result(c, apfs_xattr_set(c->vol, ino, xname, data, size, 0), "xattr import");
            free(data);
        }
    }

    memset(&attributes, 0, sizeof(attributes));
    attributes.uid = (uint32_t)uid;
    attributes.gid = (uint32_t)gid;
    attributes.mtime = mtime;
    attributes.atime = optional_number(c, entry, "atime_ns", 0, mtime);
    attributes.btime = optional_number(c, entry, "birthtime_ns", 0, mtime);
    attributes.ctime = optional_number(c, entry, "ctime_ns", 0, mtime);
    attributes.bsd_flags = (uint32_t)flags & ~(uint32_t)APFS_UF_COMPRESSED;
    mask = APFS_SETATTR_UID | APFS_SETATTR_GID | APFS_SETATTR_MTIME | APFS_SETATTR_ATIME | APFS_SETATTR_FLAGS;
    if (json_object_object_get_ex(entry, "birthtime_ns", &xattrs)) mask |= APFS_SETATTR_BTIME;
    if (json_object_object_get_ex(entry, "ctime_ns", &xattrs)) mask |= APFS_SETATTR_CTIME;
    attributes.mode = (uint16_t)mode;
    mask |= APFS_SETATTR_MODE;
    result(c, apfs_setattr(c->vol, ino, mask, &attributes), "metadata attribute import");
    free(path);
}

typedef struct { J *entries; int compression; Context *context; } PortableCompression;

static int portable_compression_for_path(const char *path, void *context) {
    PortableCompression *selection = context;
    J *entry, *flags;
    while (*path == '/') path++;
    if (json_object_object_get_ex(selection->entries, path, &entry) &&
        json_object_object_get_ex(entry, "flags", &flags) &&
        (json_object_get_uint64(flags) & APFS_UF_COMPRESSED))
        return selection->compression;
    return APFS_COMPRESS_NONE;
}

static int portable_compression_layout_for_path(const char *path, void *context,
        uint32_t *type, uint64_t *resource_size) {
    PortableCompression *selection = context;
    J *entry;
    while (*path == '/') path++;
    *type = 0; *resource_size = UINT64_MAX;
    if (json_object_object_get_ex(selection->entries, path, &entry))
        portable_layout(selection->context, entry, type, resource_size);
    return APFS_OK;
}

int apfs_metadata_import(apfs_vol_t *vol, const char *tree, const char *manifest) {
    Context *c = context_new();
    struct stat st;
    J *schema, *entries, *algorithm;
    apfslib_opts_t options;
    int compression = APFS_COMPRESS_ZLIB;
    int failed;

    c->vol = vol;
    if (setjmp(c->error)) {
        int status = c->status ? c->status : APFS_EINVAL;
        context_free(c);
        return status;
    }
    check(c, !stat(manifest, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 512 * 1024 * 1024,
          "portable metadata file");
    c->doc = json_object_from_file(manifest);
    check(c, c->doc && json_object_is_type(c->doc, json_type_object), "portable metadata JSON");
    schema = field(c, c->doc, "schema", json_type_string);
    check(c, !strcmp(json_object_get_string(schema), APFS_PORTABLE_SCHEMA), "portable metadata schema");
    entries = field(c, c->doc, "entries", json_type_object);
    check(c, json_object_object_length(entries) > 0 && json_object_object_length(entries) <= 2000000,
          "portable metadata entry count");
    if (json_object_object_get_ex(c->doc, "compression", &algorithm)) {
        check(c, json_object_is_type(algorithm, json_type_string) &&
                  apfs_compress_parse(json_object_get_string(algorithm), &compression) == APFS_OK &&
                  compression != APFS_COMPRESS_NONE, "portable metadata compression");
    }

    apfslib_default_opts(&options);
    PortableCompression selection = { entries, compression, c };
    /* Reject layout errors before any source files are written. */
    json_object_object_foreach(entries, layout_path, layout_entry) {
        uint32_t type; uint64_t resource_size;
        (void)layout_path;
        check(c, json_object_is_type(layout_entry, json_type_object), "metadata entry");
        portable_layout(c, layout_entry, &type, &resource_size);
    }
    options.compression_for_path = portable_compression_for_path;
    options.compression_layout_for_path = portable_compression_layout_for_path;
    options.compression_context = &selection;
    options.xattrs = 0;
    options.quiet = 1;
    result(c, apfslib_addall(vol, tree, "/", &options), "portable tree import");
    json_object_object_foreach(entries, name, entry)
        portable_entry(c, name, entry, compression);
    failed = apfs_vol_flush(vol);
    context_free(c);
    return failed;
}

typedef struct {
    uint8_t *bytes;
    uint64_t size;
    int fd;
} Blob;

static void bad(Context *c, const char *why) {
    APFS_ERR("native manifest: %s", why);
    longjmp(c->error, 1);
}
static void check(Context *c, int ok, const char *why) {
    if (!ok)
        bad(c, why);
}
static void result(Context *c, int ret, const char *why) {
    if (ret < 0) {
        c->status = ret;
        bad(c, why);
    }
}
static J *field(Context *c, J *j, const char *key, enum json_type type) {
    J *v;
    check(c, j && json_object_object_get_ex(j, key, &v) && json_object_is_type(v, type), key);
    return v;
}
static const char *string(Context *c, J *j, const char *key) {
    J *v = field(c, j, key, json_type_string);
    const char *s = json_object_get_string(v);
    check(c, strlen(s) == (size_t)json_object_get_string_len(v), "embedded NUL in string");
    return s;
}
static uint64_t number(Context *c, J *j, const char *key) {
    J *v = field(c, j, key, json_type_int);
    check(c, json_object_get_int64(v) >= 0, "negative or excessive integer");
    return json_object_get_uint64(v);
}
static void puts_(J *j, const char *k, const char *v) {
    json_object_object_add(j, k, json_object_new_string(v));
}
static void putn(J *j, const char *k, uint64_t v) { json_object_object_add(j, k, json_object_new_uint64(v)); }
static char *hex(const void *p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    const uint8_t *b = p;
    char *s = apfs_malloc(n * 2 + 1);
    size_t i;
    for (i = 0; i < n; i++) {
        s[2 * i] = digits[b[i] >> 4];
        s[2 * i + 1] = digits[b[i] & 15];
    }
    s[n * 2] = 0;
    return s;
}
static uint8_t *unhex(Context *c, const char *s, size_t *n) {
    static const char digits[] = "0123456789abcdef";
    size_t i, len = strlen(s);
    uint8_t *p;
    check(c, !(len & 1) && len <= 32 * 1024 * 1024, "invalid hex length");
    p = apfs_malloc(len / 2 + 1);
    for (i = 0; i < len; i += 2) {
        const char *a = strchr(digits, s[i]), *b = strchr(digits, s[i + 1]);
        if (!a || !b) {
            free(p);
            bad(c, "invalid hex digit");
        }
        p[i / 2] = (uint8_t)((a - digits) * 16 + (b - digits));
    }
    *n = len / 2;
    return p;
}
static void puth(J *j, const char *k, const void *b, size_t n) {
    char *s = hex(b, n);
    puts_(j, k, s);
    free(s);
}
static void idkey(uint64_t id, char key[24]) { snprintf(key, 24, "%" PRIu64, id); }
static J *indexed(Context *c, J *index, uint64_t id) {
    char key[24];
    J *j;
    idkey(id, key);
    check(c, json_object_object_get_ex(index, key, &j), "missing native inode or stream");
    return j;
}
static void index_add(Context *c, J *index, uint64_t id, J *value) {
    char key[24];
    J *old;
    idkey(id, key);
    check(c, !json_object_object_get_ex(index, key, &old), "duplicate inode/stream id");
    json_object_object_add(index, key, json_object_get(value));
}
static int safe_open(Context *c, int base, const char *path, int flags, int parent, char **leaf) {
    char *copy, *part, *next, *save = NULL;
    int dir, fd = -1;
    check(c, *path && *path != '/' && strlen(path) < 4096, "unsafe host path");
    copy = apfs_strdup(path);
    dir = dup(base);
    check(c, dir >= 0, "dup host directory");
    part = strtok_r(copy, "/", &save);
    while (part) {
        check(c, strcmp(part, ".") && strcmp(part, ".."), "unsafe host path component");
        next = strtok_r(NULL, "/", &save);
        if (!next) {
            if (parent) {
                *leaf = apfs_strdup(part);
                fd = dir;
            } else {
                fd = openat(dir, part, flags | O_NOFOLLOW | O_CLOEXEC, 0600);
                close(dir);
            }
            break;
        }
        fd = openat(dir, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dir);
        check(c, fd >= 0, "host parent missing or a symlink");
        dir = fd;
        part = next;
    }
    free(copy);
    check(c, fd >= 0, "cannot open host file");
    return fd;
}
static void write_all(Context *c, int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t got = write(fd, b, n);
        check(c, got > 0, "host write failed");
        b += got;
        n -= (size_t)got;
    }
}
static void hashfd(Context *c, int fd, uint8_t digest[32]) {
    uint8_t buf[65536];
    ssize_t n;
    apfs_sha256_ctx hash;
    check(c, lseek(fd, 0, SEEK_SET) == 0, "host seek");
    apfs_sha256_init(&hash);
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        apfs_sha256_update(&hash, buf, (size_t)n);
    check(c, n == 0, "host hash read");
    apfs_sha256_final(&hash, digest);
    check(c, lseek(fd, 0, SEEK_SET) == 0, "host seek");
}
static J *descriptor(Context *c, const char *location, const char *path) {
    int fd = safe_open(c, !strcmp(location, "tree") ? c->tree : c->storage, path, O_RDONLY, 0, NULL);
    struct stat st;
    uint8_t digest[32];
    J *j = json_object_new_object();
    check(c, !fstat(fd, &st) && S_ISREG(st.st_mode), "content is not a regular file");
    hashfd(c, fd, digest);
    close(fd);
    puts_(j, "location", location);
    puts_(j, "path", path);
    putn(j, "size", (uint64_t)st.st_size);
    puth(j, "sha256", digest, 32);
    return j;
}
static int contentfd(Context *c, J *desc) {
    const char *location = string(c, desc, "location");
    struct stat st;
    uint8_t digest[32];
    char *encoded;
    int fd;
    check(c, !strcmp(location, "tree") || !strcmp(location, "storage"), "invalid content location");
    fd = safe_open(c, !strcmp(location, "tree") ? c->tree : c->storage, string(c, desc, "path"), O_RDONLY, 0,
                   NULL);
    check(c, !fstat(fd, &st) && S_ISREG(st.st_mode) && (uint64_t)st.st_size == number(c, desc, "size"),
          "content size/type changed");
    hashfd(c, fd, digest);
    encoded = hex(digest, 32);
    check(c, !strcmp(encoded, string(c, desc, "sha256")), "content hash changed; regenerate the manifest");
    free(encoded);
    return fd;
}
static int blob_read(void *ctx, uint64_t off, void *buf, size_t len) {
    Blob *b = ctx;
    if (off > b->size || len > b->size - off)
        return APFS_ECORRUPT;
    if (b->bytes)
        memcpy(buf, b->bytes + off, len);
    else {
        size_t done = 0;
        while (done < len) {
            ssize_t n = pread(b->fd, (uint8_t *)buf + done, len - done, (off_t)(off + done));
            if (n <= 0)
                return APFS_EIO;
            done += (size_t)n;
        }
    }
    return APFS_OK;
}
static void blob_close(Blob *b) {
    free(b->bytes);
    if (b->fd >= 0)
        close(b->fd);
}
static void kv_add(Context *c, const uint8_t *k, size_t kl, const uint8_t *v, size_t vl) {
    uint8_t *key, *value;
    apfs_kv_t *kv;
    check(c, kl >= 8 && kl <= 65535 && vl < 65535, "native record size");
    key = apfs_malloc(kl);
    value = apfs_malloc(vl + 1);
    memcpy(key, k, kl);
    memcpy(value, v, vl);
    c->kv = apfs_realloc(c->kv, (c->count + 1) * sizeof(*c->kv));
    kv = &c->kv[c->count++];
    kv->k = key;
    kv->v = value;
    kv->klen = (uint16_t)kl;
    kv->vlen = (uint16_t)vl;
}
static void stream_add(Context *c, uint64_t id, uint64_t size) {
    char key[24];
    J *existing;
    idkey(id, key);
    if (json_object_object_get_ex(c->stream_index, key, &existing)) {
        check(c, number(c, existing, "size") == size, "inconsistent stream size");
        return;
    }
    existing = json_object_new_object();
    putn(existing, "id", id);
    putn(existing, "size", size);
    json_object_array_add(c->streams, existing);
    index_add(c, c->stream_index, id, existing);
}
static void record_add(Context *c, const apfs_kv_t *kv) {
    J *j = json_object_new_object();
    char *key = hex(kv->k, kv->klen);
    J *old;
    check(c, !json_object_object_get_ex(c->record_index, key, &old), "duplicate native record key");
    puts_(j, "key", key);
    puth(j, "value", kv->v, kv->vlen);
    json_object_array_add(c->records, j);
    json_object_object_add(c->record_index, key, json_object_get(j));
    free(key);
    if (apfs_key_type(kv->k) == APFS_TYPE_INODE) {
        apfs_inode_t inode;
        apfs_dstream_t ds;
        J *node = json_object_new_object();
        uint64_t id = apfs_key_id(kv->k);
        result(c, apfs_inode_parse(id, kv->v, kv->vlen, &inode), "invalid inode record");
        putn(node, "id", id);
        json_object_object_add(node, "paths", json_object_new_array());
        json_object_array_add(c->nodes, node);
        index_add(c, c->inodes, id, node);
        if (apfs_inode_get_dstream(&inode, &ds) == APFS_OK)
            stream_add(c, inode.private_id, ds.size);
        apfs_inode_release(&inode);
    } else if (apfs_key_type(kv->k) == APFS_TYPE_XATTR) {
        check(c, kv->vlen >= 4 && 4U + apfs_get16(kv->v + 2) == kv->vlen, "invalid xattr record length");
        if (apfs_get16(kv->v) & XATTR_DATA_STREAM) {
            check(c, kv->vlen == 52, "invalid external xattr stream record");
            uint64_t stream = apfs_get64(kv->v + 4);
            stream_add(c, stream, apfs_get64(kv->v + 12));
            if (c->inventory) {
                apfs_inode_t inode;
                result(c, apfs_inode_read(c->vol, apfs_key_id(kv->k), &inode), "xattr owner");
                /* Expanded imports regenerate compressed resource forks. */
                if (!(inode.bsd_flags & APFS_UF_COMPRESSED) ||
                    kv->klen != 10 + sizeof("com.apple.ResourceFork") ||
                    memcmp(kv->k + 10, "com.apple.ResourceFork",
                           sizeof("com.apple.ResourceFork"))) {
                    char key[32];
                    snprintf(key, sizeof(key), "%" PRIu64, stream);
                    json_object_object_add(c->inline_streams, key, json_object_new_boolean(1));
                }
                apfs_inode_release(&inode);
            }
        }
    }
}
static void supported(Context *c, apfs_vol_t *v) {
    check(c, apfs_nx_volume_count(v->nx) == 1,
          "native interchange currently requires a single-volume container");
    check(c,
          !v->sealed && !v->snap_xid && !LE64(v->sb->apfs_num_snapshots) &&
              (LE64(v->sb->apfs_fs_flags) & APFS_FS_UNENCRYPTED) && !v->sb->apfs_er_state_oid &&
              !v->sb->apfs_integrity_meta_oid && !v->sb->apfs_doc_id_tree_oid &&
              !v->sb->apfs_sec_root_tree_oid,
          "native interchange requires an unencrypted, unsealed volume without snapshots or auxiliary index "
          "trees");
}
static int supported_type(uint8_t t) {
    return t == 3 || t == 4 || t == 5 || t == 6 || t == 9 || t == 10 || t == 12 || t == 13;
}
static J *volume_export(apfs_vol_t *v) {
    J *j = json_object_new_object();
    apfs_superblock_t *s = v->sb;
#define EXPORT64(name) putn(j, #name, LE64(s->name))
    EXPORT64(apfs_features);
    EXPORT64(apfs_readonly_compatible_features);
    EXPORT64(apfs_incompatible_features);
    EXPORT64(apfs_fs_flags);
    EXPORT64(apfs_next_obj_id);
    EXPORT64(apfs_num_files);
    EXPORT64(apfs_num_directories);
    EXPORT64(apfs_num_symlinks);
    EXPORT64(apfs_num_other_fsobjects);
    EXPORT64(apfs_cloneinfo_id_epoch);
#undef EXPORT64
    putn(j, "apfs_next_doc_id", LE32(s->apfs_next_doc_id));
    putn(j, "apfs_role", LE16(s->apfs_role));
    puth(j, "apfs_volname", s->apfs_volname, sizeof(s->apfs_volname));
    puth(j, "apfs_vol_uuid", &s->apfs_vol_uuid, 16);
    puth(j, "apfs_volume_group_id", &s->apfs_volume_group_id, 16);
    puth(j, "apfs_meta_crypto", &s->apfs_meta_crypto, sizeof(s->apfs_meta_crypto));
    return j;
}
static void volume_import(Context *c, apfs_superblock_t *s) {
    J *j = field(c, c->doc, "volume", json_type_object);
    size_t n;
    uint8_t *b;
#define IMPORT64(name) s->name = LE64(number(c, j, #name))
    IMPORT64(apfs_features);
    IMPORT64(apfs_readonly_compatible_features);
    IMPORT64(apfs_incompatible_features);
    IMPORT64(apfs_fs_flags);
    IMPORT64(apfs_next_obj_id);
    IMPORT64(apfs_num_files);
    IMPORT64(apfs_num_directories);
    IMPORT64(apfs_num_symlinks);
    IMPORT64(apfs_num_other_fsobjects);
    IMPORT64(apfs_cloneinfo_id_epoch);
#undef IMPORT64
    check(c, number(c, j, "apfs_next_doc_id") <= UINT32_MAX && number(c, j, "apfs_role") <= UINT16_MAX,
          "volume integer range");
    s->apfs_next_doc_id = LE32((uint32_t)number(c, j, "apfs_next_doc_id"));
    s->apfs_role = LE16((uint16_t)number(c, j, "apfs_role"));
#define IMPORTHEX(name)                                                                                      \
    b = unhex(c, string(c, j, #name), &n);                                                                   \
    check(c, n == sizeof(s->name), "volume field size");                                                     \
    memcpy(&s->name, b, n);                                                                                  \
    free(b)
    IMPORTHEX(apfs_volname);
    IMPORTHEX(apfs_vol_uuid);
    IMPORTHEX(apfs_volume_group_id);
    IMPORTHEX(apfs_meta_crypto);
#undef IMPORTHEX
    check(c, memchr(s->apfs_volname, 0, sizeof(s->apfs_volname)) != NULL, "unterminated volume name");
    check(c,
          (LE64(s->apfs_fs_flags) & APFS_FS_UNENCRYPTED) && !LE64(s->apfs_readonly_compatible_features) &&
              !(LE64(s->apfs_incompatible_features) &
                ~(uint64_t)(APFS_INCOMPAT_CASE_INSENSITIVE | APFS_INCOMPAT_NORMALIZATION_INSENSITIVE)),
          "unsupported native volume features");
}
static void host_mkdir(Context *c, const char *path) {
    char *leaf;
    int dir = safe_open(c, c->tree, path, 0, 1, &leaf);
    check(c, mkdirat(dir, leaf, 0700) == 0, "cannot create tree directory");
    free(leaf);
    close(dir);
}
static void walk_paths(Context *c, uint64_t dir, const char *path, unsigned depth) {
    apfs_dirent_t *entries;
    size_t count, i;
    check(c, depth < 256, "directory cycle or excessive depth");
    result(c, apfs_dir_list(c->vol, dir, &entries, &count), "cannot read native directory");
    for (i = 0; i < count; i++) {
        J *node = indexed(c, c->inodes, entries[i].ino);
        J *paths = field(c, node, "paths", json_type_array);
        char *child;
        apfs_inode_t inode;
        check(c,
              *entries[i].name && !strchr(entries[i].name, '/') && strcmp(entries[i].name, ".") &&
                  strcmp(entries[i].name, ".."),
              "unsafe native filename");
        child = apfs_malloc(strlen(path) + strlen(entries[i].name) + 2);
        sprintf(child, "%s%s%s", path, *path ? "/" : "", entries[i].name);
        result(c, apfs_inode_read(c->vol, entries[i].ino, &inode), "cannot read directory inode");
        if ((inode.mode & APFS_S_IFMT) == APFS_S_IFDIR)
            check(c, json_object_array_length(paths) == 0, "directory hardlink or cycle");
        json_object_array_add(paths, json_object_new_string(child));
        if ((inode.mode & APFS_S_IFMT) == APFS_S_IFDIR) {
            if (!c->inventory)
                host_mkdir(c, child);
            walk_paths(c, inode.ino, child, depth + 1);
        }
        apfs_inode_release(&inode);
        free(child);
    }
    apfs_dirents_free(entries, count);
}
static void export_contents(Context *c) {
    size_t i;
    for (i = 0; i < json_object_array_length(c->nodes); i++) {
        J *node = json_object_array_get_idx(c->nodes, i);
        J *paths = field(c, node, "paths", json_type_array);
        apfs_inode_t inode;
        apfs_dstream_t ds;
        uint64_t size, pos;
        uint8_t buf[65536];
        char saved[64];
        const char *path;
        const char *location;
        int fd;
        size_t k;
        result(c, apfs_inode_read(c->vol, number(c, node, "id"), &inode), "read inode for contents");
        if ((inode.mode & APFS_S_IFMT) != APFS_S_IFREG) {
            apfs_inode_release(&inode);
            continue;
        }
        snprintf(saved, sizeof(saved), "logical-%" PRIu64, inode.ino);
        location = json_object_array_length(paths) ? "tree" : "storage";
        path = json_object_array_length(paths) ? json_object_get_string(json_object_array_get_idx(paths, 0))
                                               : saved;
        fd = c->inventory ? -1
                          : safe_open(c, !strcmp(location, "tree") ? c->tree : c->storage, path,
                                      O_WRONLY | O_CREAT | O_EXCL, 0, NULL);
        apfs_sha256_ctx hash;
        uint8_t digest[32];
        apfs_sha256_init(&hash);
        result(c, apfs_file_size(c->vol, &inode, &size), "logical file size");
        for (pos = 0; !c->metadata_only && pos < size;) {
            size_t want = size - pos > sizeof(buf) ? sizeof(buf) : (size_t)(size - pos), got = 0;
            result(c, apfs_file_read(c->vol, &inode, pos, buf, want, &got), "decode file contents");
            check(c, got == want, "short logical file read");
            if (c->inventory)
                apfs_sha256_update(&hash, buf, got);
            else
                write_all(c, fd, buf, got);
            pos += got;
        }
        if (c->inventory) {
            J *desc = json_object_new_object();
            apfs_sha256_final(&hash, digest);
            puts_(desc, "location", "image");
            putn(desc, "size", size);
            if (!c->metadata_only)
                puth(desc, "sha256", digest, 32);
            json_object_object_add(node, "content", desc);
        } else {
            close(fd);
            json_object_object_add(node, "content", descriptor(c, location, path));
        }
        char stream_key[32];
        J *needed;
        snprintf(stream_key, sizeof(stream_key), "%" PRIu64, inode.private_id);
        if (!(inode.bsd_flags & APFS_UF_COMPRESSED) &&
            apfs_inode_get_dstream(&inode, &ds) == APFS_OK &&
            !(c->inventory && json_object_object_get_ex(c->inline_streams, stream_key, &needed))) {
            check(c, ds.size == size, "uncompressed stream size mismatch");
            json_object_object_add(indexed(c, c->stream_index, inode.private_id), "content",
                                   json_object_get(field(c, node, "content", json_type_object)));
        }
        for (k = 1; !c->inventory && k < json_object_array_length(paths); k++) {
            const char *alias = json_object_get_string(json_object_array_get_idx(paths, k));
            char *leaf;
            int dir = safe_open(c, c->tree, alias, 0, 1, &leaf);
            check(c, linkat(c->tree, path, dir, leaf, 0) == 0, "cannot export host hardlink");
            free(leaf);
            close(dir);
        }
        apfs_inode_release(&inode);
    }
    /* Symlinks are created after all host directories and regular contents. */
    for (i = 0; i < json_object_array_length(c->nodes); i++) {
        J *node = json_object_array_get_idx(c->nodes, i);
        J *paths = field(c, node, "paths", json_type_array);
        apfs_inode_t inode;
        size_t k;
        result(c, apfs_inode_read(c->vol, number(c, node, "id"), &inode), "read symlink inode");
        if ((inode.mode & APFS_S_IFMT) == APFS_S_IFLNK) {
            char *target;
            result(c, apfs_readlink(c->vol, inode.ino, &target), "read native symlink");
            puts_(node, "symlink", target);
            for (k = 0; !c->inventory && k < json_object_array_length(paths); k++) {
                char *leaf;
                int dir = safe_open(c, c->tree,
                                    json_object_get_string(json_object_array_get_idx(paths, k)), 0,
                                    1, &leaf);
                check(c, symlinkat(target, dir, leaf) == 0, "export symlink");
                free(leaf);
                close(dir);
            }
            free(target);
        }
        apfs_inode_release(&inode);
    }
}
static void export_streams(Context *c) {
    size_t i;
    for (i = 0; i < json_object_array_length(c->streams); i++) {
        J *stream = json_object_array_get_idx(c->streams, i), *desc, *extents = json_object_new_array();
        uint64_t id = number(c, stream, "id"), size = number(c, stream, "size"), pos;
        uint8_t buf[65536], key[16];
        apfs_btiter_t it;
        apfs_kv_t kv;
        int ret;
        if (!json_object_object_get_ex(stream, "content", &desc)) {
            char path[64];
            int fd;
            snprintf(path, sizeof(path), "stream-%" PRIu64, id);
            fd = c->inventory
                     ? -1
                     : safe_open(c, c->storage, path, O_CREAT | O_EXCL | O_WRONLY, 0, NULL);
            apfs_sha256_ctx hash;
            uint8_t digest[32], *inline_data = NULL;
            char stream_key[32];
            J *needed;
            snprintf(stream_key, sizeof(stream_key), "%" PRIu64, id);
            if (c->inventory && json_object_object_get_ex(c->inline_streams, stream_key, &needed)) {
                check(c, size <= 64 * 1024 * 1024, "external xattr too large for inventory");
                inline_data = apfs_malloc((size_t)size + 1);
            }
            apfs_sha256_init(&hash);
            for (pos = 0; (!c->metadata_only || inline_data) && pos < size;) {
                size_t n = size - pos > sizeof(buf) ? sizeof(buf) : (size_t)(size - pos);
                result(c, apfs_dstream_read(c->vol, id, size, pos, buf, n), "read raw stream");
                if (c->inventory) {
                    apfs_sha256_update(&hash, buf, n);
                    if (inline_data)
                        memcpy(inline_data + pos, buf, n);
                } else
                    write_all(c, fd, buf, n);
                pos += n;
            }
            if (c->inventory) {
                J *desc = json_object_new_object();
                apfs_sha256_final(&hash, digest);
                puts_(desc, "location", "image");
                putn(desc, "size", size);
                if (!c->metadata_only || inline_data)
                    puth(desc, "sha256", digest, 32);
                if (inline_data) {
                    puth(desc, "inline_hex", inline_data, (size_t)size);
                    free(inline_data);
                }
                json_object_object_add(stream, "content", desc);
            } else {
                close(fd);
                json_object_object_add(stream, "content", descriptor(c, "storage", path));
            }
        }
        apfs_key_hdr(key, id, APFS_TYPE_FILE_EXTENT);
        apfs_put64(key + 8, 0);
        ret = apfs_bt_iter_seek(&it, &c->vol->fs, key, 16);
        while (ret == APFS_OK && it.valid) {
            J *extent;
            uint64_t length, off, crypto;
            unsigned flags, hole;
            J *last = NULL;
            apfs_bt_iter_get(&it, &kv);
            if (apfs_key_id(kv.k) != id || apfs_key_type(kv.k) != APFS_TYPE_FILE_EXTENT)
                break;
            check(c, kv.klen == 16 && kv.vlen == 24, "raw extent size");
            length = apfs_get64(kv.v) & J_FILE_EXTENT_LEN_MASK;
            flags = (unsigned)(apfs_get64(kv.v) >> 56);
            off = apfs_get64(kv.k + 8);
            crypto = apfs_get64(kv.v + 16);
            hole = apfs_get64(kv.v + 8) == 0;
            if (json_object_array_length(extents))
                last = json_object_array_get_idx(extents, json_object_array_length(extents) - 1);
            if (last && number(c, last, "offset") + number(c, last, "length") == off &&
                number(c, last, "flags") == flags && number(c, last, "crypto_id") == crypto &&
                number(c, last, "hole") == hole)
                putn(last, "length", number(c, last, "length") + length);
            else {
                extent = json_object_new_object();
                putn(extent, "offset", off);
                putn(extent, "length", length);
                putn(extent, "flags", flags);
                putn(extent, "crypto_id", crypto);
                putn(extent, "hole", hole);
                json_object_array_add(extents, extent);
            }
            ret = apfs_bt_iter_next(&it);
        }
        result(c, ret, "read stream extents");
        json_object_object_add(stream, "extents", extents);
    }
}
static Context *context_new(void) {
    Context *c = apfs_calloc(1, sizeof(*c));
    c->tree = c->storage = -1;
    c->inodes = json_object_new_object();
    c->stream_index = json_object_new_object();
    c->record_index = json_object_new_object();
    c->stream_refs = json_object_new_object();
    return c;
}
static void context_free(Context *c) {
    size_t i;
    for (i = 0; i < c->count; i++) {
        free((void *)c->kv[i].k);
        free((void *)c->kv[i].v);
    }
    free(c->kv);
    if (c->tree >= 0)
        close(c->tree);
    if (c->storage >= 0)
        close(c->storage);
    json_object_put(c->doc);
    json_object_put(c->inodes);
    json_object_put(c->stream_index);
    json_object_put(c->record_index);
    json_object_put(c->stream_refs);
    free(c->request_line);
    free(c);
}
static char *storage_name(const char *path) {
    char *s = apfs_malloc(strlen(path) + 7);
    sprintf(s, "%s.files", path);
    return s;
}

static int manifest_export(apfs_vol_t *vol, const char *tree, const char *manifest, int inventory,
                           int metadata_only) {
    Context *c = context_new();
    int ret = APFS_ECORRUPT;
    if (setjmp(c->error) == 0) {
        apfs_btiter_t it;
        apfs_kv_t kv;
        char *storage;
        int fd, status;
        c->vol = vol;
        c->inventory = inventory;
        c->metadata_only = metadata_only;
        c->inline_streams = json_object_new_object();
        supported(c, vol);
        if (!inventory) {
            check(c, mkdir(tree, 0700) == 0, "export tree must be new");
            c->tree = open(tree, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            check(c, c->tree >= 0, "open export tree");
            storage = storage_name(manifest);
            check(c, mkdir(storage, 0700) == 0, "manifest storage must be new");
            c->storage = open(storage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            free(storage);
            check(c, c->storage >= 0, "open manifest storage");
        }
        c->doc = json_object_new_object();
        puts_(c->doc, "schema",
              metadata_only ? "apple-firmware.apfs-inventory-metadata/1"
              : inventory   ? "apple-firmware.apfs-inventory/1"
                            : APFS_NATIVE_SCHEMA);
        putn(c->doc, "block_size", vol->nx->bs);
        json_object_object_add(c->doc, "volume", volume_export(vol));
        c->nodes = json_object_new_array();
        c->streams = json_object_new_array();
        c->records = json_object_new_array();
        json_object_object_add(c->doc, "nodes", c->nodes);
        json_object_object_add(c->doc, "streams", c->streams);
        json_object_object_add(c->doc, "records", c->records);
        status = apfs_bt_iter_seek(&it, &vol->fs, NULL, 0);
        while (status == APFS_OK && it.valid) {
            apfs_bt_iter_get(&it, &kv);
            if (apfs_key_type(kv.k) != APFS_TYPE_FILE_EXTENT) {
                check(c, supported_type(apfs_key_type(kv.k)), "unsupported native record type");
                record_add(c, &kv);
            }
            status = apfs_bt_iter_next(&it);
        }
        result(c, status, "read native records");
        json_object_array_add(
            field(c, indexed(c, c->inodes, ROOT_DIR_INO_NUM), "paths", json_type_array),
            json_object_new_string(""));
        walk_paths(c, ROOT_DIR_INO_NUM, "", 0);
        export_contents(c);
        export_streams(c);
        fd = open(manifest, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
        check(c, fd >= 0, "manifest output must be new");
        check(c, json_object_to_fd(fd, c->doc, JSON_C_TO_STRING_PRETTY) == 0 && fsync(fd) == 0,
              "write native manifest");
        close(fd);
        ret = APFS_OK;
    }
    if (c->inline_streams)
        json_object_put(c->inline_streams);
    context_free(c);
    return ret;
}

int apfs_manifest_export(apfs_vol_t *v, const char *t, const char *m) {
    return manifest_export(v, t, m, 0, 0);
}
int apfs_manifest_inventory(apfs_vol_t *v, const char *m) {
    return manifest_export(v, NULL, m, 1, 0);
}
int apfs_manifest_inventory_metadata(apfs_vol_t *v, const char *m) {
    return manifest_export(v, NULL, m, 1, 1);
}
int apfs_manifest_extractfiles(apfs_vol_t *vol, const char *tree, const char *request) {
    Context *c = context_new();
    int ret = APFS_ECORRUPT, streaming = !strcmp(request, "-");
    if (setjmp(c->error) == 0) {
        c->tree = open(tree, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        check(c, c->tree >= 0, "open extraction tree");
        do {
            if (streaming) {
                ssize_t length = getline(&c->request_line, &c->request_capacity, stdin);
                if (length < 0) {
                    check(c, feof(stdin), "read extraction request");
                    break;
                }
                check(c, length <= 16 * 1024 * 1024, "request too large");
                c->doc = json_tokener_parse(c->request_line);
            } else {
                struct stat st;
                int fd = open(request, O_RDONLY | O_NOFOLLOW);
                check(c,
                      fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
                          st.st_size <= 16 * 1024 * 1024,
                      "request size/type");
                c->doc = json_object_from_fd(fd);
                close(fd);
            }
            J *files = field(c, c->doc, "files", json_type_array);
            /* Reuse the request as the response, adding hashes of native bytes
             * while they are extracted, before rereading the host copy. */
            for (size_t i = 0; i < json_object_array_length(files); i++) {
                J *entry = json_object_array_get_idx(files, i);
                apfs_inode_t inode;
                apfs_sha256_ctx hash;
                uint8_t digest[32], buf[65536];
                uint64_t size, pos = 0;
                result(c, apfs_inode_read(vol, number(c, entry, "id"), &inode),
                       "read extraction inode");
                check(c, (inode.mode & APFS_S_IFMT) == APFS_S_IFREG,
                      "extraction requires regular inode");
                result(c, apfs_file_size(vol, &inode, &size), "extraction size");
                int fd = safe_open(c, c->tree, string(c, entry, "path"),
                                   O_WRONLY | O_CREAT | O_EXCL, 0, NULL);
                apfs_sha256_init(&hash);
                while (pos < size) {
                    size_t n = size - pos > sizeof(buf) ? sizeof(buf) : (size_t)(size - pos),
                           got = 0;
                    result(c, apfs_file_read(vol, &inode, pos, buf, n, &got),
                           "extract logical data");
                    check(c, got == n, "short extraction read");
                    apfs_sha256_update(&hash, buf, n);
                    write_all(c, fd, buf, n);
                    pos += n;
                }
                check(c, close(fd) == 0, "close extracted file");
                apfs_sha256_final(&hash, digest);
                puth(entry, "sha256", digest, 32);
                putn(entry, "size", size);
                apfs_inode_release(&inode);
            }
            check(c,
                  puts(json_object_to_json_string_ext(c->doc, JSON_C_TO_STRING_PLAIN)) >= 0 &&
                      fflush(stdout) == 0,
                  "write extraction receipt");
            json_object_put(c->doc);
            c->doc = NULL;
        } while (streaming);
        ret = APFS_OK;
    }
    context_free(c);
    return ret;
}

static void load(Context *c, const char *tree, const char *manifest) {
    char *storage;
    int fd;
    struct stat st;
    size_t i;
    apfs_kv_t previous = {0};
    apfs_superblock_t sb = {0};
    apfs_vol_t vol = {0};
    apfs_btree_t bt = {0};
    fd = open(manifest, O_RDONLY | O_NOFOLLOW);
    check(c, fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size <= 256 * 1024 * 1024,
          "manifest input size/type");
    c->doc = json_object_from_fd(fd);
    close(fd);
    check(c, c->doc != NULL, "invalid JSON manifest");
    check(c, !strcmp(string(c, c->doc, "schema"), APFS_NATIVE_SCHEMA), "unsupported native manifest schema");
    c->nodes = field(c, c->doc, "nodes", json_type_array);
    c->streams = field(c, c->doc, "streams", json_type_array);
    c->records = field(c, c->doc, "records", json_type_array);
    c->tree = open(tree, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    storage = storage_name(manifest);
    c->storage = open(storage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    free(storage);
    check(c, c->tree >= 0 && c->storage >= 0, "tree or native storage directory missing/symlink");
    volume_import(c, &sb);
    vol.sb = &sb;
    vol.hashed_names = (LE64(sb.apfs_incompatible_features) & 9) != 0;
    vol.case_fold = (LE64(sb.apfs_incompatible_features) & 1) != 0;
    bt.vol = &vol;
    for (i = 0; i < json_object_array_length(c->records); i++) {
        J *j = json_object_array_get_idx(c->records, i);
        size_t kl, vl;
        uint8_t *k = unhex(c, string(c, j, "key"), &kl), *v = unhex(c, string(c, j, "value"), &vl);
        apfs_kv_t kv;
        check(c, kl >= 8 && kl <= 65535 && vl < 65535 && supported_type(apfs_key_type(k)),
              "unsupported native record");
        kv_add(c, k, kl, v, vl);
        free(k);
        free(v);
        kv = c->kv[c->count - 1];
        check(c, !previous.k || apfs_cmp_fs(&bt, previous.k, previous.klen, kv.k, kv.klen) < 0,
              "native record ordering/duplicate key");
        previous = kv;
        json_object_object_add(c->record_index, string(c, j, "key"), json_object_get(j));
    }
    c->metadata_count = c->count;
    for (i = 0; i < json_object_array_length(c->nodes); i++)
        index_add(c, c->inodes, number(c, json_object_array_get_idx(c->nodes, i), "id"),
                  json_object_array_get_idx(c->nodes, i));
    for (i = 0; i < json_object_array_length(c->streams); i++)
        index_add(c, c->stream_index, number(c, json_object_array_get_idx(c->streams, i), "id"),
                  json_object_array_get_idx(c->streams, i));
}
static Blob attribute(Context *c, uint64_t id, const char *name) {
    Blob blob = {NULL, 0, -1};
    uint8_t key[1024];
    char *hexkey;
    J *record;
    size_t n;
    uint8_t *value;
    check(c, strlen(name) + 11 <= sizeof(key), "attribute name too long");
    apfs_key_hdr(key, id, APFS_TYPE_XATTR);
    apfs_put16(key + 8, (uint16_t)(strlen(name) + 1));
    memcpy(key + 10, name, strlen(name) + 1);
    hexkey = hex(key, strlen(name) + 11);
    check(c, json_object_object_get_ex(c->record_index, hexkey, &record), "missing native attribute");
    free(hexkey);
    value = unhex(c, string(c, record, "value"), &n);
    check(c, n >= 4 && n == 4U + apfs_get16(value + 2), "attribute record size");
    if (apfs_get16(value) & XATTR_DATA_STREAM) {
        J *stream;
        check(c, n == 52, "external attribute size");
        stream = indexed(c, c->stream_index, apfs_get64(value + 4));
        blob.size = apfs_get64(value + 12);
        check(c, blob.size == number(c, stream, "size"), "attribute stream size disagreement");
        blob.fd = contentfd(c, field(c, stream, "content", json_type_object));
        free(value);
    } else {
        blob.size = n - 4;
        blob.bytes = apfs_malloc((size_t)blob.size + 1);
        memcpy(blob.bytes, value + 4, (size_t)blob.size);
        free(value);
    }
    return blob;
}
/* Bind the exported namespace to the catalog, including every hardlink alias.
 * Special nodes remain manifest-only; they do not need host device privileges. */
static void namespace_check(Context *c, apfs_vol_t *vol) {
    J *paths = json_object_new_object();
    J *counts = json_object_new_object();
    size_t i, j, inodes = 0;
    for (i = 0; i < json_object_array_length(c->nodes); i++) {
        J *node = json_object_array_get_idx(c->nodes, i);
        J *list = field(c, node, "paths", json_type_array);
        for (j = 0; j < json_object_array_length(list); j++) {
            J *item = json_object_array_get_idx(list, j);
            J *old;
            const char *path;
            check(c, json_object_is_type(item, json_type_string), "invalid namespace path");
            path = json_object_get_string(item);
            check(c,
                  strlen(path) == (size_t)json_object_get_string_len(item) &&
                      !json_object_object_get_ex(paths, path, &old),
                  "duplicate or invalid namespace path");
            json_object_object_add(paths, path, json_object_get(node));
        }
    }
    for (i = 0; i < c->metadata_count; i++) {
        apfs_kv_t *kv = &c->kv[i];
        if (apfs_key_type(kv->k) == APFS_TYPE_INODE)
            inodes++;
        if (apfs_key_type(kv->k) == APFS_TYPE_DIR_REC && apfs_key_id(kv->k) != ROOT_DIR_PARENT &&
            apfs_key_id(kv->k) != PRIV_DIR_INO_NUM) {
            char key[24];
            J *n;
            idkey(apfs_get64(kv->v), key);
            if (!json_object_object_get_ex(counts, key, &n))
                json_object_object_add(counts, key, json_object_new_int(1));
            else
                json_object_set_int(n, json_object_get_int(n) + 1);
        }
    }
    check(c, inodes == json_object_array_length(c->nodes), "native node list differs from inode catalog");
    for (i = 0; i < json_object_array_length(c->nodes); i++) {
        J *node = json_object_array_get_idx(c->nodes, i);
        J *list = field(c, node, "paths", json_type_array);
        J *count;
        uint64_t id = number(c, node, "id");
        char idstr[24];
        uint8_t inodekey[8];
        char *encoded;
        J *record;
        size_t len;
        uint8_t *bytes;
        unsigned mode;
        idkey(id, idstr);
        apfs_key_hdr(inodekey, id, APFS_TYPE_INODE);
        encoded = hex(inodekey, 8);
        check(c, json_object_object_get_ex(c->record_index, encoded, &record), "node has no inode");
        free(encoded);
        bytes = unhex(c, string(c, record, "value"), &len);
        check(c, len >= 92, "short inode");
        mode = apfs_get16(bytes + 80) & APFS_S_IFMT;
        free(bytes);
        check(c,
              json_object_array_length(list) == (id == ROOT_DIR_INO_NUM ? 1
                                                 : json_object_object_get_ex(counts, idstr, &count)
                                                     ? (size_t)json_object_get_int(count)
                                                     : 0),
              "missing or extra namespace alias");
        for (j = 0; j < json_object_array_length(list); j++) {
            const char *path = json_object_get_string(json_object_array_get_idx(list, j));
            struct stat st;
            int fd;
            if (!*path) {
                check(c, id == ROOT_DIR_INO_NUM, "empty non-root path");
                continue;
            }
            {
                char *copy = apfs_strdup(path), *name = strrchr(copy, '/');
                J *parent;
                uint8_t key[4096];
                int kl;
                if (name) {
                    *name++ = 0;
                } else {
                    name = copy;
                }
                check(c, strlen(name) < 1000 && *name && strcmp(name, ".") && strcmp(name, ".."),
                      "unsafe native filename");
                check(c, json_object_object_get_ex(paths, name == copy ? "" : copy, &parent),
                      "namespace parent missing");
                kl = apfs_drec_key(vol, number(c, parent, "id"), name, key);
                check(c, kl > 0 && kl < (int)sizeof(key), "directory key too long");
                encoded = hex(key, (size_t)kl);
                check(c, json_object_object_get_ex(c->record_index, encoded, &record),
                      "namespace path differs from catalog");
                free(encoded);
                free(copy);
                bytes = unhex(c, string(c, record, "value"), &len);
                check(c, len >= 18 && apfs_get64(bytes) == id, "namespace inode differs from catalog");
                free(bytes);
            }
            if (mode == APFS_S_IFREG) {
                J *desc = field(c, node, "content", json_type_object);
                char *leaf;
                int dir;
                fd = contentfd(c, desc);
                check(c, !fstat(fd, &st), "stat logical file");
                close(fd);
                dir = safe_open(c, c->tree, path, 0, 1, &leaf);
                {
                    struct stat alias;
                    check(c,
                          !fstatat(dir, leaf, &alias, AT_SYMLINK_NOFOLLOW) && S_ISREG(alias.st_mode) &&
                              alias.st_dev == st.st_dev && alias.st_ino == st.st_ino,
                          "logical file hardlink alias changed");
                }
                free(leaf);
                close(dir);
            } else if (mode == APFS_S_IFDIR) {
                fd = safe_open(c, c->tree, path, O_RDONLY | O_DIRECTORY, 0, NULL);
                close(fd);
            } else if (mode == APFS_S_IFLNK) {
                Blob target = attribute(c, id, SYMLINK_EA_NAME);
                char actual[4096], expected[4096], *leaf;
                int dir = safe_open(c, c->tree, path, 0, 1, &leaf);
                ssize_t n;
                check(c, target.size > 0 && target.size <= sizeof(expected), "symlink target size");
                result(c, blob_read(&target, 0, expected, (size_t)target.size), "read symlink target");
                check(c,
                      expected[target.size - 1] == 0 && strlen(expected) == target.size - 1 &&
                          !strcmp(string(c, node, "symlink"), expected),
                      "symlink differs from native attribute");
                n = readlinkat(dir, leaf, actual, sizeof(actual));
                check(c, n == (ssize_t)target.size - 1 && !memcmp(actual, expected, (size_t)n),
                      "host symlink target changed");
                blob_close(&target);
                free(leaf);
                close(dir);
            }
        }
    }
    json_object_put(paths);
    json_object_put(counts);
}
static void preflight(Context *c) {
    size_t i;
    uint64_t bs = number(c, c->doc, "block_size");
    apfs_nx_t nx = {0};
    apfs_vol_t vol = {0};
    apfs_superblock_t sb = {0};
    check(c, bs >= 4096 && bs <= 65536 && !(bs & (bs - 1)), "invalid native block size");
    nx.bs = (uint32_t)bs;
    vol.nx = &nx;
    vol.sb = &sb;
    volume_import(c, &sb);
    vol.hashed_names = (LE64(sb.apfs_incompatible_features) & 9) != 0;
    vol.case_fold = (LE64(sb.apfs_incompatible_features) & 1) != 0;
    for (i = 0; i < c->metadata_count; i++) {
        apfs_kv_t *kv = &c->kv[i];
        uint64_t id = 0, size = 0;
        int referenced = 0;
        if (apfs_key_type(kv->k) == APFS_TYPE_XATTR && kv->vlen == 52 &&
            (apfs_get16(kv->v) & XATTR_DATA_STREAM)) {
            id = apfs_get64(kv->v + 4);
            size = apfs_get64(kv->v + 12);
            referenced = 1;
        } else if (apfs_key_type(kv->k) == APFS_TYPE_INODE) {
            apfs_inode_t inode;
            apfs_dstream_t ds;
            result(c, apfs_inode_parse(apfs_key_id(kv->k), kv->v, kv->vlen, &inode), "parse native inode");
            if (apfs_inode_get_dstream(&inode, &ds) == APFS_OK) {
                id = inode.private_id;
                size = ds.size;
                referenced = 1;
            }
            apfs_inode_release(&inode);
        }
        if (referenced) {
            char key[24];
            check(c, number(c, indexed(c, c->stream_index, id), "size") == size, "catalog stream size mismatch");
            idkey(id, key);
            json_object_object_add(c->stream_refs, key, json_object_new_boolean(1));
        }
    }
    for (i = 0; i < json_object_array_length(c->streams); i++) {
        J *stream = json_object_array_get_idx(c->streams, i);
        J *extents = field(c, stream, "extents", json_type_array);
        uint64_t pos = 0, size = number(c, stream, "size"), id = number(c, stream, "id");
        size_t j;
        char key[24]; J *reference;
        idkey(id, key);
        check(c, json_object_object_get_ex(c->stream_refs, key, &reference), "unreferenced native stream");
        int fd = contentfd(c, field(c, stream, "content", json_type_object));
        check(c, number(c, field(c, stream, "content", json_type_object), "size") == size,
              "stream descriptor size");
        for (j = 0; j < json_object_array_length(extents); j++) {
            J *e = json_object_array_get_idx(extents, j);
            uint64_t length = number(c, e, "length"), flags = number(c, e, "flags"),
                     hole = number(c, e, "hole");
            uint8_t key[16], value[24];
            check(c,
                  number(c, e, "offset") == pos && length && !(length % bs) &&
                      length <= J_FILE_EXTENT_LEN_MASK && flags <= 255 && hole <= 1 &&
                      length <= INT64_MAX - pos,
                  "invalid logical extent layout");
            if (hole && pos < size) {
                uint8_t buf[65536];
                uint64_t end = pos + length < size ? pos + length : size, at = pos;
                Blob blob = {NULL, size, fd};
                while (at < end) {
                    size_t n = end - at > sizeof(buf) ? sizeof(buf) : (size_t)(end - at), k;
                    result(c, blob_read(&blob, at, buf, n), "read sparse hole");
                    for (k = 0; k < n; k++)
                        check(c, buf[k] == 0, "nonzero content in declared sparse hole");
                    at += n;
                }
            }
            apfs_key_hdr(key, id, APFS_TYPE_FILE_EXTENT);
            apfs_put64(key + 8, pos);
            apfs_put64(value, length | (flags << 56));
            apfs_put64(value + 8, hole ? 0 : 1);
            apfs_put64(value + 16, number(c, e, "crypto_id"));
            kv_add(c, key, 16, value, 24);
            pos += length;
        }
        close(fd);
        check(c, pos >= size, "missing stream extents");
    }
    result(c, apfs_verify_fs_records(&vol, c->kv, c->count),
           "native filesystem graph or stream layout failed validation");
    namespace_check(c, &vol);
    for (i = 0; i < c->metadata_count; i++) {
        apfs_kv_t *kv = &c->kv[i];
        if (apfs_key_type(kv->k) == APFS_TYPE_INODE) {
            apfs_inode_t inode;
            J *node = indexed(c, c->inodes, apfs_key_id(kv->k));
            J *desc;
            apfs_dstream_t ds;
            result(c, apfs_inode_parse(apfs_key_id(kv->k), kv->v, kv->vlen, &inode), "parse native inode");
            field(c, node, "paths", json_type_array);
            if ((inode.mode & APFS_S_IFMT) == APFS_S_IFREG) {
                int fd;
                desc = field(c, node, "content", json_type_object);
                fd = contentfd(c, desc);
                close(fd);
                if (inode.bsd_flags & APFS_UF_COMPRESSED) {
                    Blob header = attribute(c, inode.ino, DECMPFS_EA_NAME), resource = {NULL, 0, -1};
                    uint8_t *bytes;
                    uint8_t digest[32];
                    char *encoded;
                    uint32_t type;
                    check(c, header.size >= 16 && header.size <= 16 * 1024 * 1024,
                          "invalid compression attribute size");
                    bytes = apfs_malloc((size_t)header.size);
                    result(c, blob_read(&header, 0, bytes, (size_t)header.size),
                           "read compression attribute");
                    type = apfs_get32(bytes + 4);
                    if (type == 4 || type == 8 || type == 10 || type == 12)
                        resource = attribute(c, inode.ino, RESOURCEFORK_EA_NAME);
                    check(c, apfs_get64(bytes + 8) == number(c, desc, "size"),
                          "compressed logical size changed");
                    result(c,
                           apfs_decmpfs_digest(bytes, (size_t)header.size, resource.size, blob_read,
                                               &resource, digest),
                           "stored compression does not decode");
                    encoded = hex(digest, 32);
                    check(c, !strcmp(encoded, string(c, desc, "sha256")),
                          "compressed storage does not match expanded content");
                    free(encoded);
                    free(bytes);
                    blob_close(&header);
                    blob_close(&resource);
                } else if (apfs_inode_get_dstream(&inode, &ds) == APFS_OK) {
                    J *stream = indexed(c, c->stream_index, inode.private_id);
                    J *raw = field(c, stream, "content", json_type_object);
                    check(c,
                          ds.size == number(c, desc, "size") &&
                              !strcmp(string(c, raw, "sha256"), string(c, desc, "sha256")),
                          "raw stream differs from logical file");
                } else
                    check(c, number(c, desc, "size") == 0, "file without data stream is not empty");
            }
            apfs_inode_release(&inode);
        }
    }
}
int apfs_manifest_validate(const char *tree, const char *manifest) {
    Context *c = context_new();
    int ret = APFS_ECORRUPT;
    if (setjmp(c->error) == 0) {
        load(c, tree, manifest);
        preflight(c);
        ret = APFS_OK;
    }
    context_free(c);
    return ret;
}
static void write_stream(Context *c, apfs_vol_t *vol, J *stream) {
    J *extents = field(c, stream, "extents", json_type_array);
    size_t i;
    uint64_t id = number(c, stream, "id"), size = number(c, stream, "size");
    int fd = contentfd(c, field(c, stream, "content", json_type_object));
    Blob blob = {NULL, size, fd};
    uint8_t *buf = apfs_malloc(vol->nx->bs * 256U);
    for (i = 0; i < json_object_array_length(extents); i++) {
        J *e = json_object_array_get_idx(extents, i);
        uint64_t at = number(c, e, "offset"), remaining = number(c, e, "length"),
                 flags = number(c, e, "flags"), crypto = number(c, e, "crypto_id");
        while (remaining) {
            uint64_t blocks = remaining / vol->nx->bs, got;
            paddr_t physical = 0;
            uint8_t key[16], value[24];
            size_t n;
            if (blocks > 256)
                blocks = 256;
            got = blocks;
            if (!number(c, e, "hole")) {
                result(c, apfs_sm_alloc(vol->nx, vol, blocks, 0, &physical, &got),
                       "allocate native stream blocks");
                n = (size_t)(got * vol->nx->bs);
                memset(buf, 0, n);
                if (at < size)
                    result(c, blob_read(&blob, at, buf, size - at > n ? n : (size_t)(size - at)),
                           "read native stream contents");
                result(c, apfs_write_blocks(vol->nx, physical, got, buf), "write native stream blocks");
                result(c, apfs_phys_extent_add(vol, physical, got, id), "write native extent reference");
            }
            apfs_key_hdr(key, id, APFS_TYPE_FILE_EXTENT);
            apfs_put64(key + 8, at);
            apfs_put64(value, got * vol->nx->bs | (flags << 56));
            apfs_put64(value + 8, physical);
            apfs_put64(value + 16, crypto);
            result(c, apfs_bt_insert(&vol->fs, key, 16, value, 24), "insert native stream extent");
            remaining -= got * vol->nx->bs;
            at += got * vol->nx->bs;
        }
    }
    free(buf);
    close(fd);
}
int apfs_manifest_import(apfs_vol_t *vol, const char *tree, const char *manifest) {
    Context *c = context_new();
    int ret = APFS_ECORRUPT;
    if (setjmp(c->error) == 0) {
        size_t i;
        apfs_btiter_t it;
        apfs_kv_t kv;
        J *initial = json_object_new_array();
        apfs_superblock_t desired = {0};
        c->vol = vol;
        load(c, tree, manifest);
        preflight(c);
        supported(c, vol);
        volume_import(c, &desired);
        check(c,
              number(c, c->doc, "block_size") == vol->nx->bs &&
                  LE64(desired.apfs_incompatible_features) == LE64(vol->sb->apfs_incompatible_features),
              "destination block size or name comparison mode differs");
        result(c, apfs_bt_iter_seek(&it, &vol->fs, NULL, 0), "read destination catalog");
        while (it.valid) {
            char *key;
            apfs_bt_iter_get(&it, &kv);
            check(c,
                  apfs_key_id(kv.k) <= PRIV_DIR_INO_NUM &&
                      (apfs_key_type(kv.k) == APFS_TYPE_INODE || apfs_key_type(kv.k) == APFS_TYPE_DIR_REC),
                  "native import requires a fresh empty volume");
            key = hex(kv.k, kv.klen);
            json_object_array_add(initial, json_object_new_string(key));
            free(key);
            result(c, apfs_bt_iter_next(&it), "read destination catalog");
        }
        /* Every host byte and compressed logical hash passed before mutation. */
        result(c, apfs_vol_mark_dirty(vol), "begin native import");
        for (i = 0; i < json_object_array_length(initial); i++) {
            size_t n;
            uint8_t *key = unhex(c, json_object_get_string(json_object_array_get_idx(initial, i)), &n);
            result(c, apfs_bt_delete(&vol->fs, key, (uint16_t)n), "clear formatter catalog");
            free(key);
        }
        json_object_put(initial);
        volume_import(c, vol->sb);
        for (i = 0; i < c->metadata_count; i++)
            result(c, apfs_bt_insert(&vol->fs, c->kv[i].k, c->kv[i].klen, c->kv[i].v, c->kv[i].vlen),
                   "import native metadata record");
        for (i = 0; i < json_object_array_length(c->streams); i++)
            write_stream(c, vol, json_object_array_get_idx(c->streams, i));
        ret = APFS_OK;
    }
    if (ret < 0 && c->status)
        ret = c->status;
    context_free(c);
    return ret;
}
#endif
