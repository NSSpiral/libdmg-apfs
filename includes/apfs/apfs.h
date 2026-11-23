/*
 * apfs.h
 * apfs-tool
 *
 * In-memory structures and interfaces of the APFS library.
 */

#ifndef APFS_H
#define APFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "common.h"
#include "abstractfile.h"
#include "apfs/apfs_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CLOSE
#define CLOSE(a) ((*((a)->close))(a))
#endif

/* Errors */

enum {
	APFS_OK = 0,
	APFS_EIO = -1,
	APFS_ECORRUPT = -2,
	APFS_ENOENT = -3,
	APFS_EEXIST = -4,
	APFS_ENOSPC = -5,
	APFS_ENOTDIR = -6,
	APFS_EISDIR = -7,
	APFS_ENOTEMPTY = -8,
	APFS_EINVAL = -9,
	APFS_EROFS = -10,
	APFS_ENOMEM = -11,
	APFS_EUNSUPP = -12,
	APFS_ENAMETOOLONG = -13,
	APFS_ELOOP = -14,
	APFS_ERANGE = -15,
	APFS_EBUSY = -16,
	APFS_EXDEV = -17
};

const char* apfs_strerror(int err);

/* Logging: 0 = errors, 1 = warnings, 2 = info, 3 = debug */

extern int apfs_verbosity;
void apfs_log(int level, const char* fmt, ...);
#define APFS_ERR(...) apfs_log(0, __VA_ARGS__)
#define APFS_WARN(...) apfs_log(1, __VA_ARGS__)
#define APFS_INFO(...) apfs_log(2, __VA_ARGS__)
#define APFS_DEBUG(...) apfs_log(3, __VA_ARGS__)

/* Little-endian access. APFS stores every integer little-endian. */

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define APFS_HOST_BIG_ENDIAN 1
static inline uint16_t apfs_le16(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint32_t apfs_le32(uint32_t x) {
	return ((x >> 24) & 0xff) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24);
}
static inline uint64_t apfs_le64(uint64_t x) {
	return ((uint64_t)apfs_le32((uint32_t)x) << 32) | apfs_le32((uint32_t)(x >> 32));
}
#else
static inline uint16_t apfs_le16(uint16_t x) { return x; }
static inline uint32_t apfs_le32(uint32_t x) { return x; }
static inline uint64_t apfs_le64(uint64_t x) { return x; }
#endif

#define LE16(x) apfs_le16(x)
#define LE32(x) apfs_le32(x)
#define LE64(x) apfs_le64(x)

static inline uint16_t apfs_get16(const void* p) { uint16_t v; memcpy(&v, p, 2); return apfs_le16(v); }
static inline uint32_t apfs_get32(const void* p) { uint32_t v; memcpy(&v, p, 4); return apfs_le32(v); }
static inline uint64_t apfs_get64(const void* p) { uint64_t v; memcpy(&v, p, 8); return apfs_le64(v); }
static inline void apfs_put16(void* p, uint16_t v) { v = apfs_le16(v); memcpy(p, &v, 2); }
static inline void apfs_put32(void* p, uint32_t v) { v = apfs_le32(v); memcpy(p, &v, 4); }
static inline void apfs_put64(void* p, uint64_t v) { v = apfs_le64(v); memcpy(p, &v, 8); }

/* Utilities (util.c) */

uint64_t apfs_fletcher64(const void* data, size_t len);
int apfs_obj_verify(const void* obj, size_t size);
void apfs_obj_set_cksum(void* obj, size_t size);
uint32_t apfs_crc32c(uint32_t crc, const void* data, size_t len);
uint64_t apfs_time_now(void);
void apfs_uuid_generate(uint8_t uuid[16]);
void apfs_uuid_format(const uint8_t uuid[16], char out[37]);
int apfs_uuid_parse(const char* str, uint8_t uuid[16]);
int apfs_uuid_is_null(const uint8_t uuid[16]);
void* apfs_malloc(size_t size);
void* apfs_calloc(size_t n, size_t size);
void* apfs_realloc(void* p, size_t size);
char* apfs_strdup(const char* s);

typedef struct {
	uint32_t state[8];
	uint64_t length;
	uint8_t buf[64];
	uint32_t used;
} apfs_sha256_ctx;

void apfs_sha256_init(apfs_sha256_ctx* ctx);
void apfs_sha256_update(apfs_sha256_ctx* ctx, const void* data, size_t len);
void apfs_sha256_final(apfs_sha256_ctx* ctx, uint8_t digest[32]);
void apfs_sha256(const void* data, size_t len, uint8_t digest[32]);

typedef struct {
	uint32_t state[5];
	uint64_t length;
	uint8_t buf[64];
	uint32_t used;
} apfs_sha1_ctx;

void apfs_sha1_init(apfs_sha1_ctx* ctx);
void apfs_sha1_update(apfs_sha1_ctx* ctx, const void* data, size_t len);
void apfs_sha1_final(apfs_sha1_ctx* ctx, uint8_t digest[20]);

/* Ciphers, MACs and key derivation (crypto.c) */

typedef struct {
	uint32_t ek[60];              /* encryption round keys */
	uint32_t dk[60];              /* decryption round keys */
	int rounds;
} apfs_aes_ctx;

int apfs_aes_init(apfs_aes_ctx* ctx, const uint8_t* key, size_t len);
void apfs_aes_encrypt(const apfs_aes_ctx* ctx, const uint8_t in[16], uint8_t out[16]);
void apfs_aes_decrypt(const apfs_aes_ctx* ctx, const uint8_t in[16], uint8_t out[16]);
void apfs_aes_cbc_encrypt(const apfs_aes_ctx* ctx, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len);
void apfs_aes_cbc_decrypt(const apfs_aes_ctx* ctx, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len);
void apfs_aes_ctr(const apfs_aes_ctx* ctx, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len);
int apfs_des3_cbc_decrypt(const uint8_t key[24], const uint8_t iv[8], uint8_t* data, size_t len);

typedef struct {
	apfs_sha256_ctx inner;
	apfs_sha256_ctx outer;
} apfs_hmac_sha256_ctx;

void apfs_hmac_sha256_init(apfs_hmac_sha256_ctx* ctx, const uint8_t* key, size_t len);
void apfs_hmac_sha256_update(apfs_hmac_sha256_ctx* ctx, const void* data, size_t len);
void apfs_hmac_sha256_final(apfs_hmac_sha256_ctx* ctx, uint8_t mac[32]);
void apfs_hmac_sha256(const uint8_t* key, size_t klen, const void* data, size_t len, uint8_t mac[32]);
void apfs_hmac_sha1(const uint8_t* key, size_t klen, const void* data, size_t len, uint8_t mac[20]);
void apfs_pbkdf2_sha1(const char* pass, size_t plen, const uint8_t* salt, size_t slen, uint32_t iterations, uint8_t* out, size_t len);
void apfs_hkdf_sha256(const uint8_t* ikm, size_t ilen, const uint8_t* salt, size_t slen, const void* info, size_t nlen, uint8_t* out, size_t len);

/* Unicode (unicode.c) */

/* Hash of a filename for a hashed directory record key, already shifted
 * into the J_DREC_HASH_MASK bits. Returns 0 on invalid UTF-8. */
uint32_t apfs_name_hash(const char* name, size_t len, int case_fold);
int apfs_utf8_valid(const char* s, size_t len);
/* Compares two names as the volume would: returns 0 when they are the same
 * file name. */
int apfs_name_equal(const char* a, size_t alen, const char* b, size_t blen, int norm_insensitive, int case_fold);

struct apfs_nx;
struct apfs_vol;
struct apfs_omap;
struct apfs_btree;

/* B-trees (btree.c) */

#define APFS_BT_MAX_DEPTH 16

typedef struct {
	uint16_t klen;
	uint16_t vlen;      /* 0xffff for ghost records (free queues) */
	uint8_t* k;         /* klen bytes of key followed by vlen bytes of value */
	uint8_t* v;
} apfs_btrec_t;

typedef struct apfs_btnode {
	struct apfs_btnode* hnext;
	oid_t oid;
	paddr_t paddr;
	xid_t xid;
	uint16_t flags;
	uint16_t level;
	uint32_t nrec;
	uint32_t cap;
	apfs_btrec_t* rec;
	int dirty;
} apfs_btnode_t;

typedef int (*apfs_keycmp_fn)(const struct apfs_btree* bt, const uint8_t* k1, uint16_t l1, const uint8_t* k2, uint16_t l2);

typedef struct apfs_btree {
	struct apfs_nx* nx;
	struct apfs_vol* vol;          /* owner, for block accounting (NULL for the container) */
	struct apfs_omap* omap;        /* resolves virtual node identifiers */
	xid_t read_xid;                /* omap lookups see this transaction (snapshots), 0 = latest */
	uint32_t storage;              /* OBJ_PHYSICAL, OBJ_VIRTUAL or OBJ_EPHEMERAL */
	uint32_t subtype;
	oid_t root;
	btree_info_t info;             /* host-endian copy of the root footer */
	apfs_keycmp_fn cmp;
	int fixed;                     /* BTNODE_FIXED_KV_SIZE */
	uint16_t fixed_klen;
	uint16_t fixed_vlen;           /* leaf value size; index values are 8 bytes */
	int hashed;                    /* sealed catalog: index values carry child hashes */
	int noheader;                  /* sealed catalog: nodes have no object header */
	uint32_t hash_size;
	int loaded;
	apfs_btnode_t** htab;
	uint32_t hsize;
	uint32_t hcount;
	uint64_t ndirty;
} apfs_btree_t;

typedef struct {
	const uint8_t* k;
	uint16_t klen;
	const uint8_t* v;
	uint16_t vlen;
} apfs_kv_t;

typedef struct {
	apfs_btree_t* bt;
	int depth;
	apfs_btnode_t* node[APFS_BT_MAX_DEPTH];
	int idx[APFS_BT_MAX_DEPTH];
	int valid;
} apfs_btiter_t;

/* Key comparators */
int apfs_cmp_omap(const apfs_btree_t* bt, const uint8_t* k1, uint16_t l1, const uint8_t* k2, uint16_t l2);
int apfs_cmp_fs(const apfs_btree_t* bt, const uint8_t* k1, uint16_t l1, const uint8_t* k2, uint16_t l2);
int apfs_cmp_u64(const apfs_btree_t* bt, const uint8_t* k1, uint16_t l1, const uint8_t* k2, uint16_t l2);
int apfs_cmp_u64pair(const apfs_btree_t* bt, const uint8_t* k1, uint16_t l1, const uint8_t* k2, uint16_t l2);

void apfs_bt_init(apfs_btree_t* bt, struct apfs_nx* nx, struct apfs_vol* vol, struct apfs_omap* omap,
		uint32_t storage, uint32_t subtype, oid_t root, apfs_keycmp_fn cmp);
int apfs_bt_load(apfs_btree_t* bt);
int apfs_bt_create(apfs_btree_t* bt, uint32_t bt_flags, uint16_t key_size, uint16_t val_size);
void apfs_bt_close(apfs_btree_t* bt);
int apfs_bt_lookup(apfs_btree_t* bt, const void* key, uint16_t klen, apfs_kv_t* out);
int apfs_bt_lookup_le(apfs_btree_t* bt, const void* key, uint16_t klen, apfs_kv_t* out);
int apfs_bt_iter_seek(apfs_btiter_t* it, apfs_btree_t* bt, const void* key, uint16_t klen);
int apfs_bt_iter_get(apfs_btiter_t* it, apfs_kv_t* out);
int apfs_bt_iter_next(apfs_btiter_t* it);
int apfs_bt_insert(apfs_btree_t* bt, const void* key, uint16_t klen, const void* val, uint16_t vlen);
int apfs_bt_delete(apfs_btree_t* bt, const void* key, uint16_t klen);
int apfs_bt_flush(apfs_btree_t* bt);
int apfs_bt_get_node(apfs_btree_t* bt, oid_t oid, apfs_btnode_t** out);
int apfs_bt_free_all(apfs_btree_t* bt);
int apfs_bt_count(apfs_btree_t* bt, uint64_t* keys, uint64_t* nodes);
typedef int (*apfs_bt_node_cb)(apfs_btree_t* bt, apfs_btnode_t* node, int depth, void* ctx);
int apfs_bt_walk(apfs_btree_t* bt, apfs_bt_node_cb cb, void* ctx);
int apfs_bt_child_oid(const apfs_btree_t* bt, const apfs_btrec_t* rec, oid_t* oid);

/* Object maps (omap.c) */

typedef struct apfs_omap {
	struct apfs_nx* nx;
	struct apfs_vol* vol;
	paddr_t paddr;
	uint8_t* raw;
	int dirty;
	int loaded;
	apfs_btree_t tree;
} apfs_omap_t;

#define APFS_OMAP_PHYS(om) ((omap_phys_t*)(om)->raw)

int apfs_omap_open(apfs_omap_t* om, struct apfs_nx* nx, struct apfs_vol* vol, paddr_t paddr);
int apfs_omap_create(apfs_omap_t* om, struct apfs_nx* nx, struct apfs_vol* vol, uint32_t flags);
int apfs_omap_lookup(apfs_omap_t* om, oid_t oid, xid_t max_xid, paddr_t* paddr, xid_t* xid, uint32_t* flags);
int apfs_omap_set(apfs_omap_t* om, oid_t oid, xid_t xid, uint32_t flags, paddr_t paddr);
int apfs_omap_remove(apfs_omap_t* om, oid_t oid, xid_t xid);
int apfs_omap_cow(apfs_omap_t* om, oid_t oid, xid_t old_xid, paddr_t old_paddr, paddr_t* new_paddr, uint32_t vflags);
int apfs_omap_delete_object(apfs_omap_t* om, oid_t oid, xid_t xid, paddr_t paddr);
int apfs_omap_flush(apfs_omap_t* om);
void apfs_omap_close(apfs_omap_t* om);

typedef struct {
	oid_t oid;
	xid_t xid;
	uint32_t flags;
	uint32_t size;
	paddr_t paddr;
} apfs_omap_rec_t;

/* Every mapping of the object map, sorted by object identifier, then transaction */
int apfs_omap_list(apfs_omap_t* om, apfs_omap_rec_t** out, size_t* count);

/* Space manager (spaceman.c) */

typedef struct {
	paddr_t start;
	uint64_t count;
	struct apfs_vol* owner;
} apfs_extent_t;

typedef struct apfs_sm {
	int loaded;
	uint64_t nblocks;
	uint64_t nchunks;
	uint32_t blocks_per_chunk;
	uint32_t chunks_per_cib;
	uint32_t cibs_per_cab;
	uint32_t ncib;
	uint32_t ncab;
	uint8_t* bitmap;              /* one bit per block of the main device */
	uint8_t* chunk_dirty;
	paddr_t* cib_addr;
	uint8_t** cib;
	paddr_t* cab_addr;
	uint8_t** cab;
	uint64_t free_count;
	paddr_t ip_base;
	uint64_t ip_blocks;
	uint8_t* ip_bitmap;
	int ip_dirty;
	apfs_extent_t* pend;          /* frees applied at the end of the transaction */
	size_t npend;
	size_t cappend;
	paddr_t hint;
	paddr_t meta_hint;
	uint64_t reserved_for_commit;
} apfs_sm_t;

int apfs_sm_load(struct apfs_nx* nx);
void apfs_sm_close(struct apfs_nx* nx);
int apfs_sm_alloc(struct apfs_nx* nx, struct apfs_vol* owner, uint64_t want, int metadata, paddr_t* start, uint64_t* got);
int apfs_sm_alloc_block(struct apfs_nx* nx, struct apfs_vol* owner, paddr_t* paddr);
int apfs_sm_free(struct apfs_nx* nx, struct apfs_vol* owner, paddr_t start, uint64_t count);
int apfs_sm_is_free(struct apfs_nx* nx, paddr_t start, uint64_t count);
int apfs_sm_mark_used(struct apfs_nx* nx, paddr_t start, uint64_t count);
int apfs_sm_flush(struct apfs_nx* nx);
uint64_t apfs_sm_free_blocks(struct apfs_nx* nx);

/* Containers (container.c) */

typedef struct {
	oid_t oid;
	uint32_t type;
	uint32_t subtype;
	uint32_t size;
	oid_t fs_oid;
	uint8_t* data;
} apfs_eph_t;

typedef struct apfs_nx {
	io_func* io;
	uint64_t base;                /* byte offset of the container within io */
	uint32_t bs;
	int writable;
	uint8_t* sbraw;               /* superblock of the mounted checkpoint */
	nx_superblock_t* sb;
	xid_t xid;                    /* transaction of the mounted checkpoint */
	xid_t txn;                    /* transaction being built, 0 if none */
	apfs_eph_t* eph;
	uint32_t neph;
	apfs_sm_t sm;
	apfs_omap_t omap;
	struct apfs_vol* vols[NX_MAX_FILE_SYSTEMS];
	int dirty;
	uint64_t blocks_written;
} apfs_nx_t;

int apfs_nx_open(apfs_nx_t** out, io_func* io, uint64_t base, int writable);
int apfs_nx_begin(apfs_nx_t* nx);
int apfs_nx_commit(apfs_nx_t* nx);
int apfs_nx_close(apfs_nx_t* nx);
oid_t apfs_nx_new_oid(apfs_nx_t* nx);
apfs_eph_t* apfs_nx_eph(apfs_nx_t* nx, oid_t oid);
int apfs_nx_volume_count(apfs_nx_t* nx);
int apfs_read_blocks(apfs_nx_t* nx, paddr_t bno, uint64_t count, void* buf);
int apfs_write_blocks(apfs_nx_t* nx, paddr_t bno, uint64_t count, const void* buf);
int apfs_read_obj(apfs_nx_t* nx, paddr_t bno, void* buf, uint32_t* type, uint32_t* subtype, oid_t* oid, xid_t* xid);
int apfs_write_obj(apfs_nx_t* nx, paddr_t bno, void* buf);
int apfs_dev_size_blocks(apfs_nx_t* nx, uint64_t* blocks);

/* Volumes (volume.c) */

typedef struct apfs_vol {
	apfs_nx_t* nx;
	int index;
	oid_t oid;
	paddr_t paddr;
	xid_t xid;
	uint8_t* raw;
	apfs_superblock_t* sb;
	apfs_omap_t omap;
	apfs_btree_t fs;
	apfs_btree_t extref;
	apfs_btree_t snapmeta;
	apfs_btree_t fext;
	int hashed_names;             /* directory records use hashed keys */
	int case_fold;                /* case-insensitive names */
	int sibling_maps;             /* write the APFS_TYPE_SIBLING_MAP index */
	int sealed;
	int readonly;
	xid_t snap_xid;               /* nonzero when mounted from a snapshot */
	char* snap_name;
	int dirty;
	uint64_t alloc_delta;
} apfs_vol_t;

#define APFS_VSB(v) ((v)->sb)

int apfs_vol_open(apfs_nx_t* nx, int index, apfs_vol_t** out);
int apfs_vol_open_snapshot(apfs_nx_t* nx, int index, const char* snapname, apfs_vol_t** out);
int apfs_vol_mark_dirty(apfs_vol_t* vol);
int apfs_vol_flush(apfs_vol_t* vol);
void apfs_vol_close(apfs_vol_t* vol);
uint64_t apfs_vol_next_id(apfs_vol_t* vol);
int apfs_vol_find(apfs_nx_t* nx, const char* name_or_index, int* index);
const char* apfs_role_name(uint16_t role);
int apfs_role_parse(const char* s, uint16_t* role);

typedef struct {
	const char* name;
	uint16_t role;
	int case_sensitive;           /* 1: case-sensitive (iOS), 0: case-insensitive (macOS) */
	int norm_insensitive;         /* set APFS_INCOMPAT_NORMALIZATION_INSENSITIVE (case-sensitive volumes) */
	uint64_t reserve_blocks;
	uint64_t quota_blocks;
	const uint8_t* uuid;          /* NULL to generate */
	uint32_t root_mode;           /* defaults to 0755 */
	uint32_t root_uid;
	uint32_t root_gid;
} apfs_vol_params_t;

int apfs_vol_create(apfs_nx_t* nx, const apfs_vol_params_t* params, int* index_out);
int apfs_vol_delete(apfs_nx_t* nx, int index);
int apfs_vol_rename(apfs_vol_t* vol, const char* name);
int apfs_vol_set_role(apfs_vol_t* vol, uint16_t role);

/* File-system records (fs.c) */

#define APFS_MAX_XF 20

typedef struct {
	uint8_t type;
	uint8_t flags;
	uint16_t size;
	uint8_t* data;
} apfs_xf_t;

typedef struct {
	uint64_t ino;
	uint64_t parent_id;
	uint64_t private_id;
	uint64_t create_time;
	uint64_t mod_time;
	uint64_t change_time;
	uint64_t access_time;
	uint64_t internal_flags;
	uint32_t nlink;               /* nchildren for directories */
	uint32_t prot_class;
	uint32_t write_gen;
	uint32_t bsd_flags;
	uint32_t uid;
	uint32_t gid;
	uint16_t mode;
	uint16_t pad1;
	uint64_t uncompressed_size;
	int nxf;
	apfs_xf_t xf[APFS_MAX_XF];
} apfs_inode_t;

typedef struct {
	char* name;
	uint64_t ino;
	uint64_t date_added;
	uint16_t flags;
	uint64_t sibling_id;
} apfs_dirent_t;

typedef struct {
	uint64_t size;
	uint64_t alloced_size;
	uint64_t crypto_id;
	uint64_t bytes_written;
	uint64_t bytes_read;
} apfs_dstream_t;

static inline uint64_t apfs_key_id(const uint8_t* k) { return apfs_get64(k) & OBJ_ID_MASK; }
static inline uint8_t apfs_key_type(const uint8_t* k) { return (uint8_t)(apfs_get64(k) >> OBJ_TYPE_SHIFT); }
static inline void apfs_key_hdr(uint8_t* k, uint64_t id, uint8_t type) {
	apfs_put64(k, (id & OBJ_ID_MASK) | ((uint64_t)type << OBJ_TYPE_SHIFT));
}

int apfs_inode_read(apfs_vol_t* vol, uint64_t ino, apfs_inode_t* out);
int apfs_inode_parse(uint64_t ino, const uint8_t* val, uint16_t vlen, apfs_inode_t* out);
int apfs_inode_write(apfs_vol_t* vol, apfs_inode_t* inode);
void apfs_inode_release(apfs_inode_t* inode);
int apfs_inode_copy(apfs_inode_t* dst, const apfs_inode_t* src);
const uint8_t* apfs_inode_xf(const apfs_inode_t* inode, uint8_t type, uint16_t* size);
int apfs_inode_xf_set(apfs_inode_t* inode, uint8_t type, uint8_t flags, const void* data, uint16_t size);
void apfs_inode_xf_del(apfs_inode_t* inode, uint8_t type);
int apfs_inode_get_dstream(const apfs_inode_t* inode, apfs_dstream_t* ds);
void apfs_inode_set_dstream(apfs_inode_t* inode, const apfs_dstream_t* ds);
const char* apfs_inode_name(const apfs_inode_t* inode);
int apfs_inode_set_name(apfs_inode_t* inode, const char* name);
uint8_t apfs_mode_to_dtype(uint16_t mode);

int apfs_drec_key(apfs_vol_t* vol, uint64_t dir, const char* name, uint8_t* key);
int apfs_xattr_raw(apfs_vol_t* vol, uint64_t ino, const char* name, uint16_t* flags, uint8_t** xdata, uint16_t* xlen);
int apfs_dir_lookup(apfs_vol_t* vol, uint64_t dir, const char* name, apfs_dirent_t* out);
int apfs_dir_list(apfs_vol_t* vol, uint64_t dir, apfs_dirent_t** out, size_t* count);
void apfs_dirent_release(apfs_dirent_t* d);
void apfs_dirents_free(apfs_dirent_t* d, size_t count);
int apfs_lookup(apfs_vol_t* vol, const char* path, int follow, uint64_t* ino, uint64_t* parent, char** basename);

int apfs_xattr_list(apfs_vol_t* vol, uint64_t ino, char*** names, size_t* count);
int apfs_xattr_get(apfs_vol_t* vol, uint64_t ino, const char* name, uint8_t** data, size_t* size);
int apfs_xattr_set(apfs_vol_t* vol, uint64_t ino, const char* name, const void* data, size_t size, int fs_owned);
int apfs_xattr_remove(apfs_vol_t* vol, uint64_t ino, const char* name);
void apfs_names_free(char** names, size_t count);

/* Data streams (extents.c) */

typedef size_t (*apfs_source_fn)(void* ctx, void* buf, size_t len);

int apfs_dstream_read(apfs_vol_t* vol, uint64_t id, uint64_t size, uint64_t off, void* buf, size_t len);
int apfs_dstream_write(apfs_vol_t* vol, uint64_t id, uint64_t size, apfs_source_fn src, void* ctx, apfs_dstream_t* ds);
int apfs_dstream_write_sparse(apfs_vol_t* vol, uint64_t id, uint64_t size, apfs_source_fn src, void* ctx,
		apfs_dstream_t* ds, uint64_t* sparse_bytes);
int apfs_dstream_id_ensure(apfs_vol_t* vol, uint64_t id);
int apfs_inode_drop_dstream(apfs_vol_t* vol, apfs_inode_t* inode);
int apfs_dstream_truncate(apfs_vol_t* vol, uint64_t id, uint64_t new_alloced, apfs_dstream_t* ds);
int apfs_dstream_release(apfs_vol_t* vol, uint64_t id);
int apfs_dstream_extents(apfs_vol_t* vol, uint64_t id, apfs_extent_t** out, size_t* count, uint64_t** logical);
int apfs_phys_extent_add(apfs_vol_t* vol, paddr_t start, uint64_t count, uint64_t owner);
int apfs_phys_extent_put(apfs_vol_t* vol, paddr_t start, uint64_t count);

/* Files (file.c) */

typedef struct {
	uint16_t mode;
	uint32_t uid;
	uint32_t gid;
	uint32_t rdev;
	uint32_t bsd_flags;
	uint64_t time;                /* 0 = now */
} apfs_create_t;

int apfs_file_size(apfs_vol_t* vol, const apfs_inode_t* inode, uint64_t* size);
int apfs_file_read(apfs_vol_t* vol, const apfs_inode_t* inode, uint64_t off, void* buf, size_t len, size_t* got);
#define APFS_WRITE_SPARSE 0x1

int apfs_file_write(apfs_vol_t* vol, uint64_t ino, apfs_source_fn src, void* ctx, uint64_t size);
int apfs_file_write_ex(apfs_vol_t* vol, uint64_t ino, apfs_source_fn src, void* ctx, uint64_t size, int flags);
int apfs_file_truncate(apfs_vol_t* vol, uint64_t ino, uint64_t size);
int apfs_readlink(apfs_vol_t* vol, uint64_t ino, char** target);

int apfs_create(apfs_vol_t* vol, uint64_t parent, const char* name, const apfs_create_t* attr, uint64_t* ino_out);
int apfs_symlink(apfs_vol_t* vol, uint64_t parent, const char* name, const char* target, const apfs_create_t* attr, uint64_t* ino_out);
int apfs_link(apfs_vol_t* vol, uint64_t ino, uint64_t newparent, const char* newname);
int apfs_unlink(apfs_vol_t* vol, uint64_t parent, const char* name);
int apfs_rename(apfs_vol_t* vol, uint64_t oldparent, const char* oldname, uint64_t newparent, const char* newname);

#define APFS_SETATTR_MODE 0x01
#define APFS_SETATTR_UID 0x02
#define APFS_SETATTR_GID 0x04
#define APFS_SETATTR_FLAGS 0x08
#define APFS_SETATTR_MTIME 0x10
#define APFS_SETATTR_ATIME 0x20
#define APFS_SETATTR_CTIME 0x40
#define APFS_SETATTR_BTIME 0x80

typedef struct {
	uint16_t mode;                /* permission bits only */
	uint32_t uid;
	uint32_t gid;
	uint32_t bsd_flags;
	uint64_t mtime;
	uint64_t atime;
	uint64_t ctime;
	uint64_t btime;
} apfs_setattr_t;

int apfs_setattr(apfs_vol_t* vol, uint64_t ino, int mask, const apfs_setattr_t* attr);

/* Compression (compress.c) */

enum {
	APFS_COMPRESS_NONE = 0,
	APFS_COMPRESS_ZLIB = 1,
	APFS_COMPRESS_LZVN = 2,
	APFS_COMPRESS_LZFSE = 3
};

int apfs_decmpfs_size(apfs_vol_t* vol, const apfs_inode_t* inode, uint64_t* size);
int apfs_decmpfs_read(apfs_vol_t* vol, const apfs_inode_t* inode, uint64_t off, void* buf, size_t len, size_t* got);
/* Hash expanded compressed storage without opening or modifying an image. */
typedef int (*apfs_read_at_fn)(void* ctx, uint64_t off, void* buf, size_t len);
int apfs_decmpfs_digest(const uint8_t* attr, size_t len, uint64_t resource_size,
        apfs_read_at_fn read_resource, void* ctx, uint8_t digest[32]);
int apfs_file_write_compressed(apfs_vol_t* vol, uint64_t ino, const uint8_t* data, size_t size, int algo);
/* Preserve a decmpfs type; UINT64_MAX leaves the resource fork length automatic. */
int apfs_file_write_compressed_type(apfs_vol_t* vol, uint64_t ino, const uint8_t* data,
        size_t size, uint32_t type, uint64_t resource_size);
int apfs_compress_parse(const char* name, int* algo);
const char* apfs_decmpfs_type_name(uint32_t type);

/* Snapshots (snapshot.c) */

typedef struct {
	char* name;
	xid_t xid;
	uint64_t create_time;
	uint64_t change_time;
	oid_t sblock_oid;
	oid_t extentref_tree_oid;
	uint32_t flags;
} apfs_snapshot_t;

int apfs_snapshot_list(apfs_vol_t* vol, apfs_snapshot_t** out, size_t* count);
void apfs_snapshots_free(apfs_snapshot_t* s, size_t count);
int apfs_snapshot_create(apfs_vol_t* vol, const char* name);
int apfs_snapshot_delete(apfs_vol_t* vol, const char* name);
int apfs_snapshot_rename(apfs_vol_t* vol, const char* oldname, const char* newname);

/* Container creation and resizing (mkfs.c, resize.c) */

typedef struct {
	uint64_t size;                /* bytes */
	uint32_t block_size;          /* defaults to 4096 */
	const uint8_t* uuid;          /* NULL to generate */
	int nvols;
	const apfs_vol_params_t* vols;
} apfs_mkfs_params_t;

uint64_t apfs_mkfs_reserved_blocks(uint64_t size, uint32_t block_size);
int apfs_mkfs(io_func* io, uint64_t base, const apfs_mkfs_params_t* params);
int apfs_nx_resize(apfs_nx_t* nx, uint64_t new_size);

/* Sealed volumes (seal.c) */

int apfs_vol_unseal(apfs_vol_t* vol);
int apfs_vol_seal_info(apfs_vol_t* vol, uint32_t* hash_type, uint8_t* root_hash, uint32_t* hash_size, xid_t* broken_xid);
#define APFS_ROOT_HASH_METADATA_SIZE (16 + 3 * APFS_HASH_MAX_SIZE)
/* Builds the payload of an IPSW .root_hash file: the sealed catalog hashes
 * for 4 KiB, 8 KiB and 16 KiB APFS block sizes. */
int apfs_vol_root_hash_metadata(apfs_vol_t* vol, uint8_t out[APFS_ROOT_HASH_METADATA_SIZE]);

/* Consistency checks (verify.c) */

int apfs_verify(apfs_nx_t* nx, int verbose);
/* Caller owns the reachable-block and pending-free bitmaps, even on error. */
int apfs_reachable_blocks(apfs_nx_t* nx, uint8_t** used, uint8_t** pending);
/* Check native logical records before importing any blocks. */
int apfs_verify_fs_records(apfs_vol_t* vol, const apfs_kv_t* records, size_t count);

#ifdef __cplusplus
}
#endif

#endif
