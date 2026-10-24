/*
 * unit.c
 * apfs-tool tests
 *
 * Library tests: checksums and file name hashes against known values (the
 * name hashes come from volumes written by macOS), trust caches of
 * synthetic signed files, then a randomized workload on fresh images. After every transaction the image is compared
 * with an in-memory model and checked with apfs_verify; snapshots are
 * compared with the model as it was when they were taken.
 *
 * usage: unit <scratch directory>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apfs/apfs.h"
#include "apfs/apfslib.h"
#include "apfs/image.h"
#include "apfs/platform.h"
#include "apfs/trustcache.h"
#include "apfs/mtree.h"
#include "lzfse.h"

static int failures = 0;

#define CHECK(cond) do { \
	if(!(cond)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while(0)

/* Known values */

static void hex(const uint8_t* p, size_t n, char* out) {
	size_t i;
	for(i = 0; i < n; i++)
		sprintf(out + i * 2, "%02x", p[i]);
}

static void test_digests(void) {
	uint8_t d[32], block[4096];
	char s[65];
	apfs_sha1_ctx sha1;
	size_t i;

	apfs_sha256("abc", 3, d);
	hex(d, 32, s);
	CHECK(!strcmp(s, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	apfs_sha1_init(&sha1);
	apfs_sha1_update(&sha1, "abc", 3);
	apfs_sha1_final(&sha1, d);
	hex(d, 20, s);
	CHECK(!strcmp(s, "a9993e364706816aba3e25717850c26c9cd0d89d"));
	CHECK((apfs_crc32c(0xffffffffU, "123456789", 9) ^ 0xffffffffU) == 0xe3069283U);

	for(i = 0; i < sizeof(block); i++)
		block[i] = (uint8_t)(i * 7 + 3);
	CHECK(apfs_fletcher64(block + 8, sizeof(block) - 8) == 0x5693ee3ff7a83be8ULL);
	apfs_obj_set_cksum(block, sizeof(block));
	CHECK(apfs_obj_verify(block, sizeof(block)));
	block[100] ^= 1;
	CHECK(!apfs_obj_verify(block, sizeof(block)));
}

static size_t unhex(const char* s, uint8_t* out) {
	size_t n = 0;
	unsigned v;
	while(s[0] && s[1] && sscanf(s, "%2x", &v) == 1) {
		out[n++] = (uint8_t)v;
		s += 2;
	}
	return n;
}

static int is_hex(const uint8_t* p, size_t n, const char* expect) {
	char s[257];
	hex(p, n, s);
	return !strcmp(s, expect);
}

/* Published test vectors (FIPS-197, SP 800-38A, RFC 2202, RFC 4231,
 * RFC 6070, RFC 5869, the classic DES example), and the key blob of an
 * image made by hdiutil with the passphrase "secret" */
static void test_crypto(void) {
	apfs_aes_ctx aes;
	uint8_t key[160], iv[16], in[64], out[64], back[64], d[64], big[200];
	size_t n;

	unhex("000102030405060708090a0b0c0d0e0f", key);
	unhex("00112233445566778899aabbccddeeff", in);
	CHECK(apfs_aes_init(&aes, key, 16) == APFS_OK);
	apfs_aes_encrypt(&aes, in, out);
	CHECK(is_hex(out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a"));
	apfs_aes_decrypt(&aes, out, back);
	CHECK(!memcmp(back, in, 16));
	unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key);
	CHECK(apfs_aes_init(&aes, key, 32) == APFS_OK);
	apfs_aes_encrypt(&aes, in, out);
	CHECK(is_hex(out, 16, "8ea2b7ca516745bfeafc49904b496089"));
	apfs_aes_decrypt(&aes, out, back);
	CHECK(!memcmp(back, in, 16));
	CHECK(apfs_aes_init(&aes, key, 20) < 0);

	unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
	unhex("000102030405060708090a0b0c0d0e0f", iv);
	unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", in);
	apfs_aes_init(&aes, key, 16);
	apfs_aes_cbc_encrypt(&aes, iv, in, out, 32);
	CHECK(is_hex(out, 32, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"));
	apfs_aes_cbc_decrypt(&aes, iv, out, out, 32);
	CHECK(!memcmp(out, in, 32));
	/* the counter carries from the last byte into the one before */
	unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", key);
	unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv);
	apfs_aes_init(&aes, key, 32);
	apfs_aes_ctr(&aes, iv, in, out, 29);
	CHECK(is_hex(out, 29, "601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990ca"));

	/* single DES as triple DES with one key; the padding is then invalid */
	unhex("133457799bbcdff1133457799bbcdff1133457799bbcdff1", key);
	memset(iv, 0, 8);
	unhex("85e813540f0ab405", d);
	CHECK(apfs_des3_cbc_decrypt(key, iv, d, 8) < 0);
	CHECK(is_hex(d, 8, "0123456789abcdef"));

	memset(key, 0x0b, 22);
	apfs_hmac_sha1(key, 20, "Hi There", 8, d);
	CHECK(is_hex(d, 20, "b617318655057264e28bc0b6fb378c8ef146be00"));
	apfs_hmac_sha256(key, 20, "Hi There", 8, d);
	CHECK(is_hex(d, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
	memset(big, 0xaa, 131);
	apfs_hmac_sha256(big, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54, d);
	CHECK(is_hex(d, 32, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));

	apfs_pbkdf2_sha1("password", 8, (const uint8_t*)"salt", 4, 1, d, 20);
	CHECK(is_hex(d, 20, "0c60c80f961f0e71f3a9b524af6012062fe037a6"));
	apfs_pbkdf2_sha1("password", 8, (const uint8_t*)"salt", 4, 4096, d, 20);
	CHECK(is_hex(d, 20, "4b007901b765489abead49d926f721d065a429c1"));
	apfs_pbkdf2_sha1("passwordPASSWORDpassword", 24, (const uint8_t*)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, d, 25);
	CHECK(is_hex(d, 25, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038"));

	n = unhex("000102030405060708090a0b0c", in);
	unhex("f0f1f2f3f4f5f6f7f8f9", iv);
	apfs_hkdf_sha256(key, 22, in, n, iv, 10, d, 42);
	CHECK(is_hex(d, 42, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));
	apfs_hkdf_sha256(key, 22, NULL, 0, NULL, 0, d, 42);
	CHECK(is_hex(d, 42, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"));

	/* PBKDF2 then triple DES unwrap the AES key, the HMAC key and "CKIE\0" */
	n = unhex("2fbec8e1dda02a9cd846861588726904a0b78190", in);
	apfs_pbkdf2_sha1("secret", 6, in, n, 200000, key, 24);
	unhex("89d0d0ff8eb92227", iv);
	n = unhex("3b0d34e243e71a6465104c65e52c115dd1e56fc8237e015da07d8087f4ac2ba8a287ba0e191dd200b4f22090bc2fa64e", d);
	CHECK(apfs_des3_cbc_decrypt(key, iv, d, n) == 41);
	CHECK(is_hex(d, 41, "8ccd692a503af0cf8d148269d8d7265d8ccd692a503af0cf8d148269d8d7265d00000000434b494500"));
}

/* Hashes of names created by macOS on case-insensitive (ci = 1) and
 * case-sensitive volumes, as found in their directory records */
static const struct {
	const char* name;
	int ci;
	uint32_t hash;
} name_hashes[] = {
	{ "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", 1, 0x01277d },
	{ "\xc5\x89", 1, 0x0252e3 },
	{ "MiXeD.Case", 1, 0x067c23 },
	{ "\xef\xac\x81le", 1, 0x0e7c61 },
	{ "\xc4\xb0stanbul", 1, 0x0f6afc },
	{ "Stra\xc3\x9f""e", 1, 0x146c83 },
	{ "\xc3\x9cn\xc3\xaf""c\xc3\xb6""d\xc3\xa9", 1, 0x14c39c },
	{ "\xc7\x84", 1, 0x1aa2ec },
	{ "e\xcc\x81", 1, 0x1d765f },
	{ "\xc3\x9f", 1, 0x201e6b },
	{ "\xc3\x85STR\xc3\x96M", 1, 0x2244ff },
	{ "\xef\xac\x83", 1, 0x2836d2 },
	{ "\xce\xa3\xce\x8a\xce\xa3\xce\xa5\xce\xa6\xce\x9f\xce\xa3", 1, 0x2acaf7 },
	{ "\xf0\x9f\x98\x80", 1, 0x2fab5c },
	{ "private-dir", 1, 0x2b29a3 },
	{ "root", 1, 0x2d9c79 },
	{ "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", 0, 0x01277d },
	{ "Stra\xc3\x9f""e", 0, 0x05e73a },
	{ "\xce\xa3\xce\x8a\xce\xa3\xce\xa5\xce\xa6\xce\x9f\xce\xa3", 0, 0x17bded },
	{ "e\xcc\x81", 0, 0x1d765f },
	{ "\xef\xac\x83", 0, 0x21b2b3 },
	{ "\xc3\x9cn\xc3\xaf""c\xc3\xb6""d\xc3\xa9", 0, 0x222830 },
	{ "\xc3\x85STR\xc3\x96M", 0, 0x2a571a },
	{ "\xef\xac\x81le", 0, 0x2e2f93 },
	{ "\xf0\x9f\x98\x80", 0, 0x2fab5c },
	{ "\xc4\xb0stanbul", 0, 0x312b5a },
	{ "\xc7\x84", 0, 0x3d816d },
	{ "MiXeD.Case", 0, 0x3dec06 },
	{ "\xc3\x9f", 0, 0x3e48f1 },
	{ "\xc5\x89", 0, 0x3eb0ff },
	{ "caf\xc3\xa9", 0, 0x2868e1 },
};

static void test_name_hashes(void) {
	size_t i;
	for(i = 0; i < sizeof(name_hashes) / sizeof(name_hashes[0]); i++) {
		uint32_t h = apfs_name_hash(name_hashes[i].name, strlen(name_hashes[i].name), name_hashes[i].ci);
		if((h >> J_DREC_HASH_SHIFT) != name_hashes[i].hash) {
			fprintf(stderr, "name hash %zu: got 0x%06x, expected 0x%06x\n", i, h >> J_DREC_HASH_SHIFT, name_hashes[i].hash);
			failures++;
		}
	}
	/* composed and decomposed forms are the same name */
	CHECK(apfs_name_equal("caf\xc3\xa9", 5, "cafe\xcc\x81", 6, 1, 0) == 0);
	CHECK(apfs_name_equal("ABC", 3, "abc", 3, 1, 1) == 0);
	CHECK(apfs_name_equal("ABC", 3, "abc", 3, 1, 0) != 0);
	CHECK(apfs_utf8_valid("\xe2\x82\xac", 3));
	CHECK(!apfs_utf8_valid("\xc3\x28", 2));
}

/* Model of the file system */

typedef struct mnode mnode_t;

typedef struct {
	char* name;
	mnode_t* node;
} mentry_t;

typedef struct {
	char* name;
	uint8_t* data;
	size_t size;
} mxattr_t;

struct mnode {
	char type;                    /* 'f'ile, 'd'irectory, 'l'ink */
	int refs;
	uint8_t* data;
	size_t size;
	char* target;
	uint16_t mode;
	mentry_t* kids;
	size_t nkids;
	mxattr_t* xa;
	size_t nxa;
};

static uint64_t rng_state;

static uint64_t rnd(void) {
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 0x2545F4914F6CDD1DULL;
}

static size_t rnd_below(size_t n) {
	return n ? (size_t)(rnd() % n) : 0;
}

static mnode_t* node_new(char type) {
	mnode_t* n = (mnode_t*)apfs_calloc(1, sizeof(mnode_t));
	n->type = type;
	n->refs = 1;
	n->mode = type == 'd' ? 0755 : (type == 'l' ? 0755 : 0644);
	return n;
}

static mnode_t* node_copy(const mnode_t* m);

static void node_put(mnode_t* n) {
	size_t i;
	if(--n->refs > 0)
		return;
	for(i = 0; i < n->nkids; i++) {
		free(n->kids[i].name);
		node_put(n->kids[i].node);
	}
	for(i = 0; i < n->nxa; i++) {
		free(n->xa[i].name);
		free(n->xa[i].data);
	}
	free(n->kids);
	free(n->xa);
	free(n->data);
	free(n->target);
	free(n);
}

/* Deep copy that keeps hard links shared within the copy */
typedef struct {
	const mnode_t* from;
	mnode_t* to;
} copymap_t;

static copymap_t* cmap;
static size_t ncmap;

static mnode_t* node_copy(const mnode_t* m) {
	mnode_t* n;
	size_t i;

	for(i = 0; i < ncmap; i++) {
		if(cmap[i].from == m) {
			cmap[i].to->refs++;
			return cmap[i].to;
		}
	}
	n = node_new(m->type);
	n->mode = m->mode;
	n->size = m->size;
	if(m->size) {
		n->data = (uint8_t*)apfs_malloc(m->size);
		memcpy(n->data, m->data, m->size);
	}
	n->target = m->target ? apfs_strdup(m->target) : NULL;
	n->nxa = m->nxa;
	n->xa = (mxattr_t*)apfs_calloc(m->nxa ? m->nxa : 1, sizeof(mxattr_t));
	for(i = 0; i < m->nxa; i++) {
		n->xa[i].name = apfs_strdup(m->xa[i].name);
		n->xa[i].size = m->xa[i].size;
		n->xa[i].data = (uint8_t*)apfs_malloc(m->xa[i].size ? m->xa[i].size : 1);
		memcpy(n->xa[i].data, m->xa[i].data, m->xa[i].size);
	}
	cmap = (copymap_t*)apfs_realloc(cmap, (ncmap + 1) * sizeof(copymap_t));
	cmap[ncmap].from = m;
	cmap[ncmap].to = n;
	ncmap++;
	n->nkids = m->nkids;
	n->kids = (mentry_t*)apfs_calloc(m->nkids ? m->nkids : 1, sizeof(mentry_t));
	for(i = 0; i < m->nkids; i++) {
		n->kids[i].name = apfs_strdup(m->kids[i].name);
		n->kids[i].node = node_copy(m->kids[i].node);
	}
	return n;
}

static mnode_t* model_copy(const mnode_t* root) {
	mnode_t* r = node_copy(root);
	free(cmap);
	cmap = NULL;
	ncmap = 0;
	return r;
}

static mentry_t* find_kid(mnode_t* dir, const char* name) {
	size_t i;
	for(i = 0; i < dir->nkids; i++)
		if(!strcmp(dir->kids[i].name, name))
			return &dir->kids[i];
	return NULL;
}

static void add_kid(mnode_t* dir, const char* name, mnode_t* node) {
	dir->kids = (mentry_t*)apfs_realloc(dir->kids, (dir->nkids + 1) * sizeof(mentry_t));
	dir->kids[dir->nkids].name = apfs_strdup(name);
	dir->kids[dir->nkids].node = node;
	dir->nkids++;
}

static mnode_t* remove_kid(mnode_t* dir, const char* name) {
	size_t i;
	for(i = 0; i < dir->nkids; i++) {
		if(!strcmp(dir->kids[i].name, name)) {
			mnode_t* n = dir->kids[i].node;
			free(dir->kids[i].name);
			dir->kids[i] = dir->kids[--dir->nkids];
			return n;
		}
	}
	return NULL;
}

/* Every path in the model, for picking operands */
typedef struct {
	char* path;
	mnode_t* node;
	mnode_t* parent;
	char* name;
} mpath_t;

static mpath_t* paths;
static size_t npaths, cappaths;

static void collect(mnode_t* dir, const char* prefix) {
	size_t i;
	for(i = 0; i < dir->nkids; i++) {
		char* p = (char*)apfs_malloc(strlen(prefix) + strlen(dir->kids[i].name) + 2);
		sprintf(p, "%s/%s", prefix, dir->kids[i].name);
		if(npaths == cappaths) {
			cappaths = cappaths ? cappaths * 2 : 64;
			paths = (mpath_t*)apfs_realloc(paths, cappaths * sizeof(mpath_t));
		}
		paths[npaths].path = p;
		paths[npaths].node = dir->kids[i].node;
		paths[npaths].parent = dir;
		paths[npaths].name = dir->kids[i].name;
		npaths++;
		if(dir->kids[i].node->type == 'd')
			collect(dir->kids[i].node, p);
	}
}

static void paths_free(void) {
	size_t i;
	for(i = 0; i < npaths; i++)
		free(paths[i].path);
	npaths = 0;
}

/* Picks a path of the given type ('d' includes the root as ""), or NULL */
static mpath_t* pick(char type) {
	size_t i, n = 0, k;
	for(i = 0; i < npaths; i++)
		if(paths[i].node->type == type)
			n++;
	if(!n)
		return NULL;
	k = rnd_below(n);
	for(i = 0; i < npaths; i++)
		if(paths[i].node->type == type && k-- == 0)
			return &paths[i];
	return NULL;
}

static const char* syllables[] = { "a", "ka", "zu", "mo", "x1", "caf\xc3\xa9", "\xe6\x97\xa5\xe6\x9c\xac", "\xc3\xb8", "\xf0\x9f\x98\x80",
	"\xc3\xb1", "long_name_segment_", "q", "7", ".hidden", "-", "_" };

static void random_name(char* out) {
	size_t parts = 1 + rnd_below(4), i;
	out[0] = 0;
	for(i = 0; i < parts; i++)
		strcat(out, syllables[rnd_below(sizeof(syllables) / sizeof(syllables[0]))]);
	if(!strcmp(out, ".") || !strcmp(out, ".."))
		strcat(out, "x");
}

static uint8_t* random_data(size_t* size) {
	static const size_t sizes[] = { 0, 1, 100, 4095, 4096, 4097, 20000, 65536, 150000, 400000 };
	size_t n = sizes[rnd_below(sizeof(sizes) / sizeof(sizes[0]))];
	uint8_t* d = (uint8_t*)apfs_malloc(n ? n : 1);
	size_t i = 0;
	int style = (int)rnd_below(3);
	while(i < n) {
		size_t run = 1 + rnd_below(9000);
		uint8_t v = (uint8_t)rnd();
		if(run > n - i)
			run = n - i;
		if(style == 0) {
			size_t j;
			for(j = 0; j < run; j++)
				d[i + j] = (uint8_t)rnd();
		} else {
			/* compressible, with runs of zeros for sparse writes */
			memset(d + i, rnd_below(3) == 0 ? 0 : v, run);
		}
		i += run;
	}
	*size = n;
	return d;
}

/* Comparing the image with the model */

static void compare_xattrs(apfs_vol_t* vol, uint64_t ino, mnode_t* m, const char* path) {
	size_t i;
	for(i = 0; i < m->nxa; i++) {
		uint8_t* data = NULL;
		size_t size = 0;
		int ret = apfs_xattr_get(vol, ino, m->xa[i].name, &data, &size);
		if(ret < 0 || size != m->xa[i].size || (size && memcmp(data, m->xa[i].data, size))) {
			fprintf(stderr, "%s: attribute %s differs (%s)\n", path, m->xa[i].name, apfs_strerror(ret));
			failures++;
		}
		free(data);
	}
}

static void compare_dir(apfs_vol_t* vol, uint64_t ino, mnode_t* m, const char* path) {
	apfs_dirent_t* ents = NULL;
	size_t n = 0, i;

	if(apfs_dir_list(vol, ino, &ents, &n) < 0) {
		fprintf(stderr, "%s: cannot list\n", path);
		failures++;
		return;
	}
	if(n != m->nkids) {
		fprintf(stderr, "%s: %zu entries, expected %zu\n", path, n, m->nkids);
		failures++;
	}
	for(i = 0; i < m->nkids; i++) {
		mnode_t* k = m->kids[i].node;
		char* p = (char*)apfs_malloc(strlen(path) + strlen(m->kids[i].name) + 2);
		apfs_inode_t inode;
		size_t j;

		sprintf(p, "%s/%s", path, m->kids[i].name);
		for(j = 0; j < n; j++)
			if(!strcmp(ents[j].name, m->kids[i].name))
				break;
		if(j == n) {
			fprintf(stderr, "%s: missing\n", p);
			failures++;
			free(p);
			continue;
		}
		if(apfs_inode_read(vol, ents[j].ino, &inode) < 0) {
			fprintf(stderr, "%s: cannot read inode\n", p);
			failures++;
			free(p);
			continue;
		}
		if((inode.mode & 07777) != k->mode) {
			fprintf(stderr, "%s: mode %o, expected %o\n", p, inode.mode & 07777, k->mode);
			failures++;
		}
		switch(k->type) {
			case 'd':
				CHECK((inode.mode & APFS_S_IFMT) == APFS_S_IFDIR);
				compare_dir(vol, ents[j].ino, k, p);
				break;
			case 'f': {
				uint64_t size = 0;
				size_t got = 0;
				uint8_t* buf;
				CHECK((inode.mode & APFS_S_IFMT) == APFS_S_IFREG);
				apfs_file_size(vol, &inode, &size);
				buf = (uint8_t*)apfs_malloc(size ? (size_t)size : 1);
				if(size != k->size || apfs_file_read(vol, &inode, 0, buf, (size_t)size, &got) < 0 || got != size ||
						memcmp(buf, k->data, (size_t)size)) {
					fprintf(stderr, "%s: contents differ (%llu bytes, expected %zu)\n", p, (unsigned long long)size, k->size);
					failures++;
				}
				free(buf);
				break;
			}
			case 'l': {
				char* target = NULL;
				CHECK((inode.mode & APFS_S_IFMT) == APFS_S_IFLNK);
				if(apfs_readlink(vol, ents[j].ino, &target) < 0 || strcmp(target, k->target)) {
					fprintf(stderr, "%s: link target differs\n", p);
					failures++;
				}
				free(target);
				break;
			}
		}
		compare_xattrs(vol, ents[j].ino, k, p);
		apfs_inode_release(&inode);
		free(p);
	}
	apfs_dirents_free(ents, n);
}

static void compare(apfs_vol_t* vol, mnode_t* root, const char* what) {
	int before = failures;
	compare_dir(vol, ROOT_DIR_INO_NUM, root, "");
	if(failures != before)
		fprintf(stderr, "  (while comparing %s)\n", what);
}

/* Random operations, applied to the image and the model */

static int is_under(const char* path, const char* dir) {
	size_t n = strlen(dir);
	return !strncmp(path, dir, n) && (path[n] == '/' || path[n] == 0);
}

static void step(apfs_vol_t* vol, mnode_t* root, int sparse_ok) {
	apfslib_opts_t o;
	char name[256], path[1024];
	mpath_t* d;
	mpath_t* f;
	int op = (int)rnd_below(100), ret;
	mnode_t* dir;
	const char* dpath;

	apfslib_default_opts(&o);
	o.quiet = 1;
	paths_free();
	collect(root, "");
	d = pick('d');
	/* the root directory is a candidate too */
	if(!d || rnd_below(4) == 0) {
		dir = root;
		dpath = "";
	} else {
		dir = d->node;
		dpath = d->path;
	}
	random_name(name);
	snprintf(path, sizeof(path), "%s/%s", dpath, name);

	if(op < 30) {
		/* new file, or overwrite of an existing one */
		mentry_t* e = find_kid(dir, name);
		size_t size;
		uint8_t* data;
		if(e && e->node->type != 'f')
			return;
		data = random_data(&size);
		o.compress = (int)rnd_below(4);
		o.sparse = sparse_ok && rnd_below(2);
		ret = apfslib_add_data(vol, data, size, path, &o);
		CHECK(ret == APFS_OK);
		if(ret == APFS_OK) {
			mnode_t* n = e ? e->node : node_new('f');
			free(n->data);
			n->data = data;
			n->size = size;
			if(!e)
				add_kid(dir, name, n);
		} else {
			free(data);
		}
	} else if(op < 40) {
		if(find_kid(dir, name))
			return;
		ret = apfslib_mkdir(vol, path, 0, &o);
		CHECK(ret == APFS_OK);
		if(ret == APFS_OK)
			add_kid(dir, name, node_new('d'));
	} else if(op < 50) {
		f = pick('f');
		if(!f)
			f = pick('l');
		if(!f)
			return;
		ret = apfslib_rm(vol, f->path, 0, 1);
		CHECK(ret == APFS_OK);
		if(ret == APFS_OK)
			node_put(remove_kid(f->parent, f->name));
	} else if(op < 53) {
		f = pick('d');
		if(!f)
			return;
		ret = apfslib_rm(vol, f->path, 1, 1);
		CHECK(ret == APFS_OK);
		if(ret == APFS_OK)
			node_put(remove_kid(f->parent, f->name));
	} else if(op < 63) {
		/* rename into the chosen directory */
		mnode_t* n;
		if(!npaths)
			return;
		f = &paths[rnd_below(npaths)];
		if(find_kid(dir, name) || (f->node->type == 'd' && is_under(dpath, f->path)))
			return;
		ret = apfslib_mv(vol, f->path, path);
		CHECK(ret == APFS_OK);
		if(ret == APFS_OK) {
			n = remove_kid(f->parent, f->name);
			add_kid(dir, name, n);
		}
	} else if(op < 70) {
		char target[256];
		if(find_kid(dir, name))
			return;
		random_name(target);
		ret = apfslib_symlink(vol, path, target, &o);
		CHECK(ret == APFS_OK);
		if(ret == APFS_OK) {
			mnode_t* n = node_new('l');
			n->target = apfs_strdup(target);
			add_kid(dir, name, n);
		}
	} else if(op < 76) {
		f = pick('f');
		if(!f || find_kid(dir, name))
			return;
		ret = apfslib_link(vol, f->path, path);
		CHECK(ret == APFS_OK);
		if(ret == APFS_OK) {
			f->node->refs++;
			add_kid(dir, name, f->node);
		}
	} else if(op < 88) {
		/* extended attributes: small ones are embedded, large ones streams */
		char xname[64];
		size_t size = rnd_below(3) ? rnd_below(200) : 3805 + rnd_below(20000), i;
		uint8_t* data;
		mnode_t* n;
		if(!npaths)
			return;
		f = &paths[rnd_below(npaths)];
		n = f->node;
		snprintf(xname, sizeof(xname), "user.test.%u", (unsigned)rnd_below(4));
		data = (uint8_t*)apfs_malloc(size ? size : 1);
		for(i = 0; i < size; i++)
			data[i] = (uint8_t)rnd();
		ret = apfslib_setattr(vol, f->path, xname, data, size);
		CHECK(ret == APFS_OK);
		if(ret == APFS_OK) {
			for(i = 0; i < n->nxa && strcmp(n->xa[i].name, xname); i++)
				;
			if(i == n->nxa) {
				n->xa = (mxattr_t*)apfs_realloc(n->xa, (n->nxa + 1) * sizeof(mxattr_t));
				n->xa[n->nxa].name = apfs_strdup(xname);
				n->nxa++;
			} else {
				free(n->xa[i].data);
			}
			n->xa[i].data = data;
			n->xa[i].size = size;
		} else {
			free(data);
		}
	} else if(op < 92) {
		mnode_t* n;
		if(!npaths)
			return;
		f = &paths[rnd_below(npaths)];
		n = f->node;
		if(!n->nxa)
			return;
		ret = apfslib_rmattr(vol, f->path, n->xa[0].name);
		CHECK(ret == APFS_OK);
		if(ret == APFS_OK) {
			free(n->xa[0].name);
			free(n->xa[0].data);
			n->xa[0] = n->xa[--n->nxa];
		}
	} else {
		uint16_t mode = (uint16_t)(rnd_below(2) ? 0600 : 0751);
		if(!npaths)
			return;
		f = &paths[rnd_below(npaths)];
		if(f->node->type == 'l')
			return;
		ret = apfslib_chmod(vol, f->path, mode, 0);
		CHECK(ret == APFS_OK);
		if(ret == APFS_OK)
			f->node->mode = mode;
	}
}

static apfs_image_t* make_image(const char* path, uint64_t size, int case_sensitive) {
	apfs_mkfs_params_t mp;
	apfs_vol_params_t vp;
	apfs_image_t* img = NULL;
	io_func* io;
	int ret;

	memset(&vp, 0, sizeof(vp));
	vp.name = "Test";
	vp.case_sensitive = case_sensitive;
	vp.norm_insensitive = case_sensitive;
	vp.root_mode = 0755;
	memset(&mp, 0, sizeof(mp));
	mp.size = size;
	mp.nvols = 1;
	mp.vols = &vp;
	io = apfs_rawfile_create(path, size);
	if(!io)
		return NULL;
	ret = apfs_mkfs(io, 0, &mp);
	CLOSE(io);
	CHECK(ret == APFS_OK);
	if(ret == APFS_OK)
		ret = apfs_image_open(path, 1, &img);
	CHECK(ret == APFS_OK);
	return ret == APFS_OK ? img : NULL;
}

/* A new container over an old image: mkfs skips blocks that are zero
 * already, but must still clear the old, newer checkpoints */
static void test_mkfs_over_old(const char* dir) {
	char path[1024], name[16];
	apfs_mkfs_params_t mp;
	apfs_vol_params_t vp;
	apfs_image_t* img;
	apfs_vol_t* vol;
	apfslib_opts_t o;
	io_func* io;
	uint64_t ino;
	int i, ret;

	printf("container over an old image\n");
	snprintf(path, sizeof(path), "%s/unit-mkfs.raw", dir);
	img = make_image(path, 16ULL << 20, 1);
	if(!img)
		return;
	apfslib_default_opts(&o);
	o.quiet = 1;
	CHECK(apfs_vol_open(img->nx, 0, &vol) == APFS_OK);
	for(i = 0; i < 4; i++) {
		snprintf(name, sizeof(name), "/old%d", i);
		CHECK(apfslib_mkdir(vol, name, 0, &o) == APFS_OK);
		CHECK(apfs_nx_commit(img->nx) == APFS_OK);
	}
	apfs_image_close(img, 1);

	memset(&vp, 0, sizeof(vp));
	vp.name = "Fresh";
	vp.case_sensitive = 1;
	vp.norm_insensitive = 1;
	vp.root_mode = 0755;
	memset(&mp, 0, sizeof(mp));
	mp.size = 16ULL << 20;
	mp.nvols = 1;
	mp.vols = &vp;
	io = apfs_rawfile_open(path, 1);
	CHECK(io != NULL);
	if(io) {
		CHECK(apfs_mkfs(io, 0, &mp) == APFS_OK);
		CLOSE(io);
	}
	ret = apfs_image_open(path, 0, &img);
	CHECK(ret == APFS_OK);
	if(ret == APFS_OK) {
		CHECK(apfs_vol_open(img->nx, 0, &vol) == APFS_OK && apfs_lookup(vol, "/old0", 0, &ino, NULL, NULL) == APFS_ENOENT);
		CHECK(apfs_verify(img->nx, 0) == APFS_OK);
		apfs_image_close(img, 0);
	}
	remove(path);
}

static void test_free_queue_footer(const char* dir) {
	char path[1024];
	apfs_image_t* img;
	apfs_eph_t* e;
	uint8_t* info;
	uint32_t longest;
	snprintf(path, sizeof(path), "%s/unit-free-queue.raw", dir);
	img = make_image(path, 16ULL << 20, 1);
	if(!img) return;
	CHECK(apfs_verify(img->nx, 0) == APFS_OK);
	e = apfs_nx_eph(img->nx, 0x402);
	CHECK(e != NULL);
	if(e) {
		info = e->data + img->nx->bs - sizeof(btree_info_t);
		longest = apfs_get32(info + 16);
		CHECK(longest == sizeof(spaceman_free_queue_key_t));
		apfs_put32(info + 16, 0);
		apfs_obj_set_cksum(e->data, img->nx->bs);
		CHECK(apfs_verify(img->nx, 0) == APFS_ECORRUPT);
		apfs_put32(info + 16, longest);
		apfs_obj_set_cksum(e->data, img->nx->bs);
		CHECK(apfs_verify(img->nx, 0) == APFS_OK);
	}
	apfs_image_close(img, 0);
	remove(path);
}

static void test_compressed_digest(const char* dir) {
	char path[1024];
	uint8_t data[4096], expected[32], digest[32];
	apfslib_opts_t options;
	apfs_image_t* img;
	apfs_vol_t* vol = NULL;
	int algorithms[] = { APFS_COMPRESS_ZLIB, APFS_COMPRESS_LZVN, APFS_COMPRESS_LZFSE };
	size_t i;
	memset(data, 'a', sizeof(data));
	apfs_sha256(data, sizeof(data), expected);
	snprintf(path, sizeof(path), "%s/unit-compressed-digest.raw", dir);
	img = make_image(path, 16ULL << 20, 1);
	if(!img) return;
	CHECK(apfs_vol_open(img->nx, 0, &vol) == APFS_OK);
	if(!vol) { apfs_image_close(img, 0); remove(path); return; }
	for(i = 0; i < sizeof(algorithms) / sizeof(algorithms[0]); i++) {
		uint64_t ino;
		uint8_t* attr = NULL;
		size_t length = 0;
		apfslib_default_opts(&options);
		options.compress = algorithms[i];
		CHECK(apfslib_add_data(vol, data, sizeof(data), "/compressed", &options) == APFS_OK);
		CHECK(apfs_lookup(vol, "/compressed", 0, &ino, NULL, NULL) == APFS_OK);
		CHECK(apfs_xattr_get(vol, ino, DECMPFS_EA_NAME, &attr, &length) == APFS_OK);
		if(attr && length > 20) {
			CHECK(apfs_decmpfs_digest(attr, length, 0, NULL, NULL, digest) == APFS_OK);
			CHECK(!memcmp(digest, expected, 32));
			CHECK(apfs_decmpfs_digest(attr, length - 4, 0, NULL, NULL, digest) == APFS_ECORRUPT);
			apfs_put64(attr + 8, sizeof(data) - 1);
			CHECK(apfs_decmpfs_digest(attr, length, 0, NULL, NULL, digest) == APFS_ECORRUPT);
		}
		free(attr);
	}
	apfs_image_close(img, 0);
	remove(path);
}

/* Data written around blocks of zeros, from an offset that is not block
 * aligned up to a last byte that is not zero, reads back unchanged */
static void test_write_sparse(const char* dir) {
	static uint8_t data[5 * 4096 + 700], back[1000 + sizeof(data)];
	char path[1024];
	FILE* f;

	printf("sparse writes\n");
	snprintf(path, sizeof(path), "%s/unit-sparse.bin", dir);
	memset(data + 1000, 0xa5, 5000);
	data[3 * 4096 + 17] = 1;
	data[sizeof(data) - 1] = 2;
	f = fopen(path, "w+b");
	CHECK(f != NULL);
	if(!f)
		return;
	CHECK(plat_sparse_file(f, sizeof(back)) == APFS_OK);
	CHECK(plat_write_sparse(f, 1000, data, sizeof(data)) == APFS_OK);
	CHECK(fseeko(f, 0, SEEK_SET) == 0 && fread(back, 1, sizeof(back), f) == sizeof(back) && fgetc(f) == EOF);
	CHECK(!memcmp(back + 1000, data, sizeof(data)));
	fclose(f);
	remove(path);
}

static void check_image(apfs_image_t* img, mnode_t* root, const char* what) {
	apfs_vol_t* vol;
	int ret = apfs_nx_commit(img->nx);
	CHECK(ret == APFS_OK);
	ret = apfs_verify(img->nx, 0);
	if(ret != APFS_OK) {
		fprintf(stderr, "  (verify failed after %s)\n", what);
		failures++;
	}
	if(apfs_vol_open(img->nx, 0, &vol) == APFS_OK)
		compare(vol, root, what);
}

static void workload(const char* dir, int case_sensitive, uint64_t seed, int rounds) {
	char path[1024], what[64];
	apfs_image_t* img;
	apfs_vol_t* vol;
	mnode_t* root = node_new('d');
	mnode_t* snap_model = NULL;
	int r, i;

	snprintf(path, sizeof(path), "%s/unit-%s.raw", dir, case_sensitive ? "cs" : "ci");
	printf("workload on a case-%s volume\n", case_sensitive ? "sensitive" : "insensitive");
	rng_state = seed;
	img = make_image(path, 96ULL << 20, case_sensitive);
	if(!img)
		return;
	CHECK(apfs_vol_open(img->nx, 0, &vol) == APFS_OK);

	for(r = 0; r < rounds && failures == 0; r++) {
		for(i = 0; i < 25; i++)
			step(vol, root, 1);
		snprintf(what, sizeof(what), "round %d", r);
		check_image(img, root, what);

		if(case_sensitive && r == rounds / 3) {
			CHECK(apfs_snapshot_create(vol, "unit-snap") == APFS_OK);
			snap_model = model_copy(root);
		}
		if(snap_model && r % 3 == 0) {
			apfs_vol_t* sv;
			CHECK(apfs_vol_open_snapshot(img->nx, 0, "unit-snap", &sv) == APFS_OK);
			if(failures == 0) {
				compare(sv, snap_model, "the snapshot");
				apfs_vol_close(sv);
			}
		}
		if(snap_model && r == 2 * rounds / 3) {
			CHECK(apfs_snapshot_delete(vol, "unit-snap") == APFS_OK);
			node_put(snap_model);
			snap_model = NULL;
			check_image(img, root, "snapshot deletion");
		}
		if(r % 4 == 3) {
			/* the image must read back the same from disk */
			CHECK(apfs_image_close(img, 1) == APFS_OK);
			CHECK(apfs_image_open(path, 1, &img) == APFS_OK);
			CHECK(apfs_vol_open(img->nx, 0, &vol) == APFS_OK);
			if(failures == 0)
				compare(vol, root, "the reopened image");
		}
	}

	paths_free();
	collect(root, "");
	{
		size_t files = 0, dirs = 0, bytes = 0, k;
		for(k = 0; k < npaths; k++) {
			if(paths[k].node->type == 'd')
				dirs++;
			else if(paths[k].node->type == 'f') {
				files++;
				bytes += paths[k].node->size;
			}
		}
		printf("  %d rounds: %zu files (%zu bytes), %zu directories, %zu entries\n", r, files, bytes, dirs, npaths);
	}

	/* empty the volume again: everything must be released */
	paths_free();
	while(root->nkids) {
		char p[600];
		snprintf(p, sizeof(p), "/%s", root->kids[0].name);
		CHECK(apfslib_rm(vol, p, 1, 1) == APFS_OK);
		node_put(remove_kid(root, root->kids[0].name));
		if(failures)
			break;
	}
	check_image(img, root, "removing everything");
	CHECK(LE64(vol->sb->apfs_num_files) == 0);

	if(snap_model)
		node_put(snap_model);
	node_put(root);
	paths_free();
	apfs_image_close(img, 1);
	remove(path);
}

/* Thousands of small records grow and shrink the trees through several levels */
static void test_many_files(const char* dir) {
	char path[1024], name[64];
	apfs_image_t* img;
	apfs_vol_t* vol;
	apfslib_opts_t o;
	int i, n = 4000, alive;
	uint64_t dino = 0;
	uint8_t data[200];

	printf("many files\n");
	snprintf(path, sizeof(path), "%s/unit-many.raw", dir);
	img = make_image(path, 64ULL << 20, 1);
	if(!img)
		return;
	apfslib_default_opts(&o);
	o.quiet = 1;
	CHECK(apfs_vol_open(img->nx, 0, &vol) == APFS_OK);
	CHECK(apfslib_mkdir(vol, "/d", 0, &o) == APFS_OK);
	CHECK(apfs_lookup(vol, "/d", 0, &dino, NULL, NULL) == APFS_OK);
	rng_state = 42;
	for(i = 0; i < n && !failures; i++) {
		snprintf(name, sizeof(name), "/d/f%05d-%llx", i, (unsigned long long)(rnd() & 0xffff));
		memset(data, i & 0xff, sizeof(data));
		CHECK(apfslib_add_data(vol, data, (size_t)(i % 200), name, &o) == APFS_OK);
		if(i % 1000 == 999) {
			CHECK(apfs_nx_commit(img->nx) == APFS_OK);
			CHECK(apfs_verify(img->nx, 0) == APFS_OK);
		}
	}
	/* remove them in a scattered order, checking along the way */
	alive = n;
	while(alive > 50 && !failures) {
		apfs_dirent_t* ents;
		size_t count, k;
		CHECK(apfs_dir_list(vol, dino, &ents, &count) == APFS_OK);
		if(count != (size_t)alive) {
			fprintf(stderr, "listing has %zu entries, expected %d\n", count, alive);
			failures++;
		}
		for(k = 0; k < count; k++) {
			if(rnd_below(3) == 0) {
				snprintf(path, sizeof(path), "/d/%s", ents[k].name);
				CHECK(apfslib_rm(vol, path, 0, 1) == APFS_OK);
				alive--;
			}
		}
		apfs_dirents_free(ents, count);
		CHECK(apfs_nx_commit(img->nx) == APFS_OK);
		CHECK(apfs_verify(img->nx, 0) == APFS_OK);
	}
	CHECK(apfslib_rm(vol, "/d", 1, 1) == APFS_OK);
	CHECK(apfs_nx_commit(img->nx) == APFS_OK);
	CHECK(apfs_verify(img->nx, 0) == APFS_OK);
	apfs_image_close(img, 1);
	snprintf(path, sizeof(path), "%s/unit-many.raw", dir);
	remove(path);
}

/* Names that differ only in case are the same file on macOS volumes */
static void test_case_insensitive(const char* dir) {
	char path[1024];
	apfs_image_t* img;
	apfs_vol_t* vol;
	apfslib_opts_t o;
	uint64_t ino1, ino2;

	snprintf(path, sizeof(path), "%s/unit-case.raw", dir);
	img = make_image(path, 32ULL << 20, 0);
	if(!img)
		return;
	apfslib_default_opts(&o);
	o.quiet = 1;
	CHECK(apfs_vol_open(img->nx, 0, &vol) == APFS_OK);
	CHECK(apfslib_add_data(vol, (const uint8_t*)"x", 1, "/Mixed\xc3\x89", &o) == APFS_OK);
	CHECK(apfs_lookup(vol, "/mIXED\xc3\xa9", 0, &ino1, NULL, NULL) == APFS_OK);
	CHECK(apfs_lookup(vol, "/MixedE\xcc\x81", 0, &ino2, NULL, NULL) == APFS_OK);
	CHECK(ino1 == ino2);
	CHECK(apfslib_mkdir(vol, "/MIXED\xc3\xa9", 0, &o) == APFS_EEXIST);
	CHECK(apfs_nx_commit(img->nx) == APFS_OK);
	CHECK(apfs_verify(img->nx, 0) == APFS_OK);
	apfs_image_close(img, 1);
	remove(path);
}

/* Trust caches */

static void put_be32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* An embedded signature with a code directory of each hash type; apart from
 * their headers, the code directories are filler */
#define CD_SIZE 64

static size_t make_sig(uint8_t* p, const uint8_t* types, int n, uint8_t fill) {
	size_t off = 12 + 8 * n;
	int i;
	put_be32(p, 0xfade0cc0);
	put_be32(p + 8, (uint32_t)n);
	for(i = 0; i < n; i++) {
		put_be32(p + 12 + 8 * i, i ? 0x1000 + i - 1 : 0);
		put_be32(p + 16 + 8 * i, (uint32_t)off);
		memset(p + off, fill + i, CD_SIZE);
		put_be32(p + off, 0xfade0c02);
		put_be32(p + off + 4, CD_SIZE);
		p[off + 37] = types[i];
		off += CD_SIZE;
	}
	put_be32(p + 4, (uint32_t)off);
	return off;
}

/* A 64-bit Mach-O executable, signed when n > 0, with the signature at 4096 */
static size_t make_macho(uint8_t* p, const uint8_t* types, int n, uint8_t fill) {
	memset(p, 0, 4096);
	apfs_put32(p, 0xfeedfacf);
	apfs_put32(p + 4, 0x0100000c);
	apfs_put32(p + 12, 2);
	if(!n)
		return 4096;
	apfs_put32(p + 16, 1);
	apfs_put32(p + 20, 16);
	apfs_put32(p + 32, 0x1d);
	apfs_put32(p + 36, 16);
	apfs_put32(p + 40, 4096);
	apfs_put32(p + 44, (uint32_t)make_sig(p + 4096, types, n, fill));
	return 4096 + apfs_get32(p + 44);
}

/* The code directory of hash type index i in a signature made by make_sig */
#define SIG_CD(sig, n, i) ((sig) + 12 + 8 * (n) + CD_SIZE * (i))

typedef struct {
	const uint8_t* p;
	size_t n;
} mem_file_t;

static int mem_read(void* ctx, uint64_t off, void* buf, size_t len) {
	mem_file_t* m = (mem_file_t*)ctx;
	if(off > m->n || len > m->n - off)
		return APFS_EIO;
	memcpy(buf, m->p + off, len);
	return APFS_OK;
}

static int hash_mem(const uint8_t* p, size_t n, apfs_tc_entry_t* e) {
	mem_file_t m;
	m.p = p;
	m.n = n;
	return apfs_tc_hash_file(mem_read, &m, n, e, APFS_TC_MAX_HASHES);
}

static int is_cdhash(const apfs_tc_entry_t* e, const uint8_t* cd) {
	uint8_t dg[32];
	if(e->hash_type == APFS_CS_SHA1) {
		apfs_sha1_ctx c;
		apfs_sha1_init(&c);
		apfs_sha1_update(&c, cd, CD_SIZE);
		apfs_sha1_final(&c, dg);
	} else {
		apfs_sha256(cd, CD_SIZE, dg);
	}
	return !memcmp(e->cdhash, dg, APFS_CDHASH_LEN);
}

static void test_trustcache(const char* dir) {
	static uint8_t thin[8192], fat[20480], dyld[8192], other[8192];
	static const uint8_t both[2] = { APFS_CS_SHA1, APFS_CS_SHA256 }, sha384[1] = { APFS_CS_SHA384 };
	apfs_tc_entry_t e[APFS_TC_MAX_HASHES];
	char path[1024], tcpath[1024];
	uint8_t uuid[16], uuid2[16], *d, *d2;
	size_t thin_len, fat_len, dyld_len, len, len2;
	apfs_tc_t tc, tc2;
	apfs_image_t* img;
	apfs_vol_t* vol;
	apfslib_opts_t o;
	int v, i;

	printf("trust caches\n");
	/* a Mach-O file counts with its strongest code directory */
	thin_len = make_macho(thin, both, 2, 1);
	CHECK(hash_mem(thin, thin_len, e) == 1);
	CHECK(e[0].hash_type == APFS_CS_SHA256 && is_cdhash(&e[0], SIG_CD(thin + 4096, 2, 1)));
	/* a standalone signature with every one */
	CHECK(hash_mem(thin + 4096, thin_len - 4096, e) == 2);
	CHECK(e[0].hash_type == APFS_CS_SHA1 && is_cdhash(&e[0], SIG_CD(thin + 4096, 2, 0)));
	CHECK(e[1].hash_type == APFS_CS_SHA256 && is_cdhash(&e[1], SIG_CD(thin + 4096, 2, 1)));
	/* a universal binary with one per architecture */
	memset(fat, 0, sizeof(fat));
	put_be32(fat, 0xcafebabe);
	put_be32(fat + 4, 2);
	for(i = 0, fat_len = 0; i < 2; i++) {
		len = make_macho(fat + 4096 + 8192 * i, both, 2, (uint8_t)(10 + i));
		put_be32(fat + 8 + 20 * i + 8, 4096 + 8192 * i);
		put_be32(fat + 8 + 20 * i + 12, (uint32_t)len);
		fat_len = 4096 + 8192 * i + len;
	}
	CHECK(hash_mem(fat, fat_len, e) == 2);
	CHECK(is_cdhash(&e[0], SIG_CD(fat + 8192, 2, 1)) && is_cdhash(&e[1], SIG_CD(fat + 16384, 2, 1)));
	/* a dyld shared cache */
	memset(dyld, 0, sizeof(dyld));
	memcpy(dyld, "dyld_v1  arm64e", 16);
	apfs_put64(dyld + 0x28, 4096);
	apfs_put64(dyld + 0x30, make_sig(dyld + 4096, both, 2, 20));
	dyld_len = 4096 + (size_t)apfs_get64(dyld + 0x30);
	CHECK(hash_mem(dyld, dyld_len, e) == 1 && is_cdhash(&e[0], SIG_CD(dyld + 4096, 2, 1)));
	/* files that are not code, or whose signature cannot be used */
	memset(other, 'x', sizeof(other));
	CHECK(hash_mem(other, sizeof(other), e) == 0);
	memcpy(other, "\xca\xfe\xba\xbe\x00\x00\x00\x34", 8);
	CHECK(hash_mem(other, sizeof(other), e) == 0);
	CHECK(hash_mem(other, make_macho(other, NULL, 0, 0), e) == APFS_ENOENT);
	CHECK(hash_mem(other, make_macho(other, sha384, 1, 0), e) == APFS_EUNSUPP);
	CHECK(hash_mem(thin, thin_len - 1, e) == APFS_ECORRUPT);

	/* the format: sorted, without duplicates */
	memset(&tc, 0, sizeof(tc));
	tc.cap = 3;
	tc.entries = (apfs_tc_entry_t*)apfs_calloc(3, sizeof(apfs_tc_entry_t));
	for(i = 0; i < 3; i++) {
		memset(tc.entries[i].cdhash, i == 1 ? 0x11 : 0x22, APFS_CDHASH_LEN);
		tc.entries[i].hash_type = APFS_CS_SHA256;
		tc.entries[i].flags = i == 1;
		tc.entries[i].category = (uint8_t)(i == 1 ? 3 : 0);
	}
	tc.count = 3;
	for(i = 0; i < 16; i++)
		uuid[i] = (uint8_t)(0xa0 + i);
	d = apfs_tc_build(&tc, 2, uuid, &len);
	CHECK(len == 24 + 2 * 24 && tc.count == 2);
	CHECK(apfs_get32(d) == 2 && !memcmp(d + 4, uuid, 16) && apfs_get32(d + 20) == 2);
	CHECK(d[24] == 0x11 && d[43] == 0x11 && d[44] == APFS_CS_SHA256 && d[45] == 1 && d[46] == 3 && d[47] == 0);
	CHECK(d[48] == 0x22 && d[68] == APFS_CS_SHA256 && d[69] == 0 && d[70] == 0);
	memset(&tc2, 0, sizeof(tc2));
	CHECK(apfs_tc_parse(d, len, &tc2, &v, uuid2) == APFS_OK && v == 2 && tc2.count == 2);
	d2 = apfs_tc_build(&tc2, v, uuid2, &len2);
	CHECK(len2 == len && !memcmp(d, d2, len));
	free(d2);
	apfs_tc_free(&tc2);
	free(d);
	d = apfs_tc_build(&tc, 1, NULL, &len);
	d2 = apfs_tc_build(&tc, 1, NULL, &len2);
	CHECK(len == 24 + 2 * 22 && apfs_get32(d) == 1 && d[24 + 22] == 0x22 && d[24 + 43] == 0);
	CHECK(len2 == len && !memcmp(d, d2, len));
	free(d2);
	free(d);
	apfs_tc_free(&tc);

	/* scanning a volume, and adding to an existing cache */
	snprintf(path, sizeof(path), "%s/unit-tc.raw", dir);
	snprintf(tcpath, sizeof(tcpath), "%s/unit.trustcache", dir);
	img = make_image(path, 32ULL << 20, 1);
	if(!img)
		return;
	apfslib_default_opts(&o);
	o.quiet = 1;
	CHECK(apfs_vol_open(img->nx, 0, &vol) == APFS_OK);
	CHECK(apfslib_mkdir(vol, "/usr/bin", 1, &o) == APFS_OK);
	CHECK(apfslib_add_data(vol, thin, thin_len, "/usr/bin/thin", &o) == APFS_OK);
	CHECK(apfslib_add_data(vol, fat, fat_len, "/usr/bin/fat", &o) == APFS_OK);
	CHECK(apfslib_add_data(vol, other, make_macho(other, NULL, 0, 0), "/usr/bin/unsigned", &o) == APFS_OK);
	CHECK(apfslib_add_data(vol, (const uint8_t*)"just some text\n", 15, "/text", &o) == APFS_OK);
	CHECK(apfslib_link(vol, "/usr/bin/thin", "/thin-link") == APFS_OK);
	memset(&tc, 0, sizeof(tc));
	CHECK(apfs_tc_scan(vol, "/", &tc, NULL) == APFS_OK && tc.count == 4);
	CHECK(apfs_tc_save(&tc, 2, NULL, tcpath) == APFS_OK && tc.count == 3);
	/* entries of an existing cache keep their flags and category; missing ones are added */
	tc.entries[0].flags = 1;
	tc.entries[0].category = 5;
	memcpy(e, &tc.entries[0], sizeof(e[0]));
	tc.count = 2;
	CHECK(apfs_tc_save(&tc, 2, "rtsc", tcpath) == APFS_OK);
	apfs_tc_free(&tc);
	CHECK(apfs_im4p_load(tcpath, &d, &len) == APFS_OK && len == 24 + 2 * 24);
	free(d);
	CHECK(apfs_tc_load(tcpath, &tc, &v) == APFS_OK && v == 2 && tc.count == 2);
	CHECK(apfs_tc_scan(vol, "/usr", &tc, NULL) == APFS_OK && tc.count == 3);
	d = apfs_tc_build(&tc, v, NULL, &len);
	CHECK(len == 24 + 3 * 24 && !memcmp(d + 24, e[0].cdhash, APFS_CDHASH_LEN) && d[24 + 21] == 1 && d[24 + 22] == 5);
	free(d);
	apfs_tc_free(&tc);
	{
		/* an IM4P with the description of IPSW trust caches */
		FILE* f = fopen(tcpath, "rb");
		uint8_t head[24];
		size_t at = 0;
		if(f && fread(head, 1, sizeof(head), f) == sizeof(head) && head[0] == 0x30)
			at = head[1] & 0x80 ? 2 + (head[1] & 0x7f) : 2;
		CHECK(at && !memcmp(head + at, "\x16\x04IM4P\x16\x04rtsc\x16\x01\x31", 15));
		if(f)
			fclose(f);
	}
	apfs_image_close(img, 0);
	remove(tcpath);
	remove(path);
}

/* mtree specifications: the layout of macOS mtree, checked against it and
 * Apple's IPSW files by tests/run.sh */

static int spec_has(const char* spec, const char* fmt, unsigned long long a, unsigned long long b) {
	char want[512];
	snprintf(want, sizeof(want), fmt, a, b);
	return strstr(spec, want) != NULL;
}

static uint64_t get_be64(const uint8_t* p) {
	uint64_t v = 0;
	int i;
	for(i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

static uint64_t ino_of(apfs_vol_t* vol, const char* path) {
	uint64_t ino = 0;
	apfs_lookup(vol, path, 0, &ino, NULL, NULL);
	return ino;
}

static void test_mtree(const char* dir) {
	static const uint8_t dir_header[70] = {
		'A', 'A', '0', '1', 70, 0, 'T', 'Y', 'P', '1', 'D', 'P', 'A', 'T', 'P', 0, 0,
		'U', 'I', 'D', '1', 0, 'G', 'I', 'D', '1', 80, 'M', 'O', 'D', '2', 0xc0, 0x01, 'F', 'L', 'G', '1', 0,
		'C', 'T', 'M', 'T', 0x00, 0x10, 0x5e, 0x5f, 0, 0, 0, 0, 0, 0, 0, 0,
		'M', 'T', 'M', 'T', 0x00, 0x10, 0x5e, 0x5f, 0, 0, 0, 0, 0, 0, 0, 0
	};
	char path[1024], out[1024];
	uint8_t big[5000], *a, *x, *raw, *d;
	size_t len, alen, dlen, got;
	apfs_image_t* img;
	apfs_vol_t* vol;
	apfs_dirent_t f, g;
	apfslib_opts_t o;
	uint64_t dino, fork = 0;
	uint16_t flags, xlen;
	char* spec;
	void* scratch;

	printf("mtree\n");
	snprintf(path, sizeof(path), "%s/unit-mtree.raw", dir);
	snprintf(out, sizeof(out), "%s/unit.mtree", dir);
	img = make_image(path, 32ULL << 20, 1);
	if(!img)
		return;
	apfslib_default_opts(&o);
	o.quiet = 1;
	memset(big, 0x5a, sizeof(big));
	CHECK(apfs_vol_open(img->nx, 0, &vol) == APFS_OK);
	CHECK(apfslib_add_data(vol, (const uint8_t*)"", 0, "/.file", &o) == APFS_OK);
	CHECK(apfslib_add_data(vol, (const uint8_t*)"x", 1, "/sp ace", &o) == APFS_OK);
	CHECK(apfslib_add_data(vol, (const uint8_t*)"x", 1, "/#*?[]\\=\xc3\xa9", &o) == APFS_OK);
	CHECK(apfslib_add_data(vol, (const uint8_t*)"x", 1, "/abcdefghijkl", &o) == APFS_OK);
	CHECK(apfslib_mkdir(vol, "/dir16charsxxxxxx", 0, &o) == APFS_OK);
	CHECK(apfslib_mkdir(vol, "/d", 0, &o) == APFS_OK);
	CHECK(apfslib_add_data(vol, (const uint8_t*)"x", 1, "/d/f", &o) == APFS_OK);
	CHECK(apfslib_link(vol, "/d/f", "/d/g") == APFS_OK);
	CHECK(apfslib_symlink(vol, "/l", "d/f", &o) == APFS_OK);
	CHECK(apfslib_add_data(vol, (const uint8_t*)"x", 1, "/r", &o) == APFS_OK);
	CHECK(apfslib_setattr(vol, "/r", RESOURCEFORK_EA_NAME, big, sizeof(big)) == APFS_OK);
	CHECK(apfslib_add_data(vol, (const uint8_t*)"x", 1, "/u", &o) == APFS_OK);
	CHECK(apfslib_setattr(vol, "/u", "user.big", big, sizeof(big)) == APFS_OK);
	CHECK(apfslib_add_data(vol, (const uint8_t*)"x", 1, "/i", &o) == APFS_OK);
	CHECK(apfslib_setattr(vol, "/i", "user.small", (const uint8_t*)"v", 1) == APFS_OK);
	if(apfs_xattr_raw(vol, ino_of(vol, "/r"), RESOURCEFORK_EA_NAME, &flags, &x, &xlen) == APFS_OK) {
		if((flags & XATTR_DATA_STREAM) && xlen >= 8)
			fork = apfs_get64(x);
		free(x);
	}
	CHECK(fork != 0);
	dino = ino_of(vol, "/d");
	memset(&f, 0, sizeof(f));
	memset(&g, 0, sizeof(g));
	CHECK(apfs_dir_lookup(vol, dino, "f", &f) == APFS_OK && apfs_dir_lookup(vol, dino, "g", &g) == APFS_OK);
	CHECK(f.sibling_id && g.sibling_id && f.sibling_id != g.sibling_id);

	CHECK(apfs_mtree_spec(vol, 1600000000, &spec, &len) == APFS_OK && len == strlen(spec));
	CHECK(strstr(spec, "#\t   tree: /Volumes/Test\n") != NULL);
	CHECK(strstr(spec, "\n\n# .\n/set type=file xattrsdigest=none.0\n"
		".               type=dir time=1600000000.000000000 \\\n"
		"                btime=1600000000.000000000 ctime=1600000000.000000000 \\\n"
		"                ptime=1600000000.000000000 inode=2 siblingid=0 nxattr=0\n") != NULL);
	CHECK(spec_has(spec, "\n    .file       atime=1600000000.000000000 inode=%llu siblingid=0 nxattr=%llu\n", ino_of(vol, "/.file"), 0));
	/* names escaped and padded; long ones push the keywords to the next line */
	CHECK(spec_has(spec, "\n    sp\\040ace   inode=%llu siblingid=0 nxattr=%llu\n", ino_of(vol, "/sp ace"), 0));
	CHECK(spec_has(spec, "\n    \\043\\052\\077\\133]\\134=\\303\\251 \\\n                inode=%llu siblingid=0 nxattr=%llu\n",
		ino_of(vol, "/#*?[]\\=\xc3\xa9"), 0));
	CHECK(spec_has(spec, "\n    abcdefghijkl \\\n                inode=%llu siblingid=0 nxattr=%llu\n", ino_of(vol, "/abcdefghijkl"), 0));
	CHECK(spec_has(spec, "\n\n# ./dir16charsxxxxxx\ndir16charsxxxxxx \\\n                type=dir inode=%llu siblingid=0 nxattr=%llu\n"
		"# ./dir16charsxxxxxx\n..\n\n..\n\n", ino_of(vol, "/dir16charsxxxxxx"), 0));
	/* hard links, symlinks (their target attribute is hidden) and digests */
	CHECK(spec_has(spec, "\n    f           inode=%llu siblingid=%llu nxattr=0\n", f.ino, f.sibling_id));
	CHECK(spec_has(spec, "\n    g           inode=%llu siblingid=%llu nxattr=0\n", g.ino, g.sibling_id));
	CHECK(spec_has(spec, "\n    l           type=link inode=%llu siblingid=0 nxattr=%llu\n", ino_of(vol, "/l"), 0));
	CHECK(spec_has(spec, "\n    r           xattrsdigest=authapfs.%llu inode=%llu siblingid=0 nxattr=1\n", fork, ino_of(vol, "/r")));
	CHECK(spec_has(spec, "\n    u           xattrsdigest=authapfs.0 inode=%llu siblingid=0 nxattr=%llu\n", ino_of(vol, "/u"), 1));
	CHECK(spec_has(spec, "\n    i           xattrsdigest=authapfs.0 inode=%llu siblingid=0 nxattr=%llu\n", ino_of(vol, "/i"), 1));
	/* files before directories */
	CHECK(strstr(spec, "\n    u ") && strstr(spec, "\n# ./d\n") && strstr(spec, "\n    u ") < strstr(spec, "\n# ./d\n"));

	/* the IPSW form: pbze blocks of an Apple Archive */
	a = apfs_mtree_archive(spec, len, 1600000000, &alen);
	CHECK(alen > 28 && !memcmp(a, "pbze\0\0\0\0\0\x40\0\0", 12));
	raw = (uint8_t*)apfs_malloc(157 + len + 1);
	scratch = apfs_malloc(lzfse_decode_scratch_size() + 1);
	got = 0;
	if(alen > 28 && get_be64(a + 12) == 157 + len) {
		size_t c = (size_t)get_be64(a + 20);
		got = c == 157 + len ? (memcpy(raw, a + 28, c), c) : lzfse_decode_buffer(raw, 157 + len + 1, a + 28, c, scratch);
	}
	CHECK(got == 157 + len);
	if(got == 157 + len) {
		CHECK(!memcmp(raw, dir_header, sizeof(dir_header)));
		CHECK(!memcmp(raw + 70, "AA01\x57\0TYP1FPATP\x09\0mtree.txt", 26) && apfs_get32(raw + 153) == len);
		CHECK(!memcmp(raw + 157, spec, len));
	}
	free(scratch);
	free(raw);
	CHECK(apfs_mtree_save(spec, len, 1600000000, "msys", out) == APFS_OK);
	CHECK(apfs_im4p_load(out, &d, &dlen) == APFS_OK && dlen == alen && !memcmp(d, a, alen));
	free(d);
	CHECK(apfs_mtree_save(spec, len, 1600000000, NULL, out) == APFS_OK);
	CHECK(apfs_im4p_load(out, &d, &dlen) == APFS_OK && dlen == len && !memcmp(d, spec, len));
	free(d);
	free(a);
	free(spec);
	apfs_dirent_release(&f);
	apfs_dirent_release(&g);
	apfs_image_close(img, 0);
	remove(out);
	remove(path);
}

/* Tar archives built by hand: PAX extended and global headers, Apple
 * metadata in PAX records and AppleDouble entries, hard links, GNU sparse
 * files */

typedef struct {
	uint8_t* p;
	size_t n;
} tbuf_t;

/* Appends data padded to a tar block */
static void tb_add(tbuf_t* b, const void* d, size_t n) {
	size_t padded = (n + 511) / 512 * 512;
	b->p = (uint8_t*)apfs_realloc(b->p, b->n + padded);
	memset(b->p + b->n, 0, padded);
	if(n)
		memcpy(b->p + b->n, d, n);
	b->n += padded;
}

static void tar_octal(uint8_t* p, size_t n, uint64_t v) {
	char tmp[24];
	snprintf(tmp, sizeof(tmp), "%0*llo", (int)n - 1, (unsigned long long)v);
	memcpy(p, tmp, n - 1);
}

static void tar_hdr(uint8_t* h, const char* name, char type, uint64_t size, const char* link) {
	memset(h, 0, 512);
	memcpy(h, name, strlen(name));
	tar_octal(h + 100, 8, 0644);
	tar_octal(h + 108, 8, 501);
	tar_octal(h + 116, 8, 20);
	tar_octal(h + 124, 12, size);
	tar_octal(h + 136, 12, 1600000000);
	h[156] = (uint8_t)type;
	if(link)
		memcpy(h + 157, link, strlen(link));
	memcpy(h + 257, "ustar\0" "00", 8);
}

static void tar_put(tbuf_t* b, uint8_t* h, const void* data, size_t n) {
	unsigned sum = 0;
	int i;
	memset(h + 148, ' ', 8);
	for(i = 0; i < 512; i++)
		sum += h[i];
	tar_octal(h + 148, 7, sum);
	tb_add(b, h, 512);
	if(n)
		tb_add(b, data, n);
}

static void tar_entry(tbuf_t* b, const char* name, char type, const void* data, size_t n, const char* link) {
	uint8_t h[512];
	tar_hdr(h, name, type, n, link);
	tar_put(b, h, data, n);
}

typedef struct {
	char d[4096];
	size_t n;
} pax_t;

/* Appends a "<length> key=value\n" record */
static void pax_rec(pax_t* r, const char* key, const void* val, size_t vlen) {
	size_t body = strlen(key) + vlen + 3, len = body + 1;
	char num[24];
	while((size_t)snprintf(num, sizeof(num), "%lu", (unsigned long)len) + body != len)
		len = strlen(num) + body;
	r->n += (size_t)sprintf(r->d + r->n, "%s %s=", num, key);
	memcpy(r->d + r->n, val, vlen);
	r->n += vlen;
	r->d[r->n++] = '\n';
}

/* Finder info, an attribute and an ACL in entry 9 and a resource fork in
 * entry 2, laid out as copyfile(3) does */
static size_t make_appledouble(uint8_t* d) {
	static const char* names[2] = { "user.ad", "com.apple.acl.text" };
	static const char* vals[2] = { "from-ad", "!#acl 1\ngroup:ABCDEFAB-CDEF-ABCD-EFAB-CDEF00000050:admin:80:allow:read,write\n" };
	size_t vlen[2], data = 120, total, off, p = 120, i;
	memset(d, 0, 1024);
	for(i = 0; i < 2; i++) {
		vlen[i] = strlen(vals[i]) + 1;
		data += (11 + strlen(names[i]) + 1 + 3) & ~(size_t)3;
	}
	total = data + vlen[0] + vlen[1];
	put_be32(d, 0x00051607);
	put_be32(d + 4, 0x00020000);
	memcpy(d + 8, "Mac OS X        ", 16);
	d[25] = 2;
	put_be32(d + 26, 9);
	put_be32(d + 30, 50);
	put_be32(d + 34, (uint32_t)(total - 50));
	put_be32(d + 38, 2);
	put_be32(d + 42, (uint32_t)total);
	put_be32(d + 46, 4);
	memcpy(d + 50, "TEXTttxt", 8);
	memcpy(d + 84, "ATTR", 4);
	put_be32(d + 92, (uint32_t)total);
	put_be32(d + 96, (uint32_t)data);
	put_be32(d + 100, (uint32_t)(vlen[0] + vlen[1]));
	d[119] = 2;
	for(i = 0, off = data; i < 2; i++) {
		put_be32(d + p, (uint32_t)off);
		put_be32(d + p + 4, (uint32_t)vlen[i]);
		d[p + 10] = (uint8_t)(strlen(names[i]) + 1);
		memcpy(d + p + 11, names[i], strlen(names[i]));
		memcpy(d + off, vals[i], vlen[i]);
		off += vlen[i];
		p += (11 + strlen(names[i]) + 1 + 3) & ~(size_t)3;
	}
	memcpy(d + total, "RSRC", 4);
	return total + 4;
}

/* An attribute's value, or NULL */
static uint8_t* xattr_of(apfs_vol_t* vol, const char* path, const char* name, size_t* len) {
	uint64_t ino;
	uint8_t* d;
	if(apfs_lookup(vol, path, 0, &ino, NULL, NULL) < 0 || apfs_xattr_get(vol, ino, name, &d, len) < 0)
		return NULL;
	return d;
}

static int read_back(apfs_vol_t* vol, const char* path, uint8_t* buf, size_t len, apfs_inode_t* inode) {
	uint64_t ino;
	size_t got;
	if(apfs_lookup(vol, path, 0, &ino, NULL, NULL) < 0 || apfs_inode_read(vol, ino, inode) < 0)
		return 0;
	return apfs_file_read(vol, inode, 0, buf, len, &got) == APFS_OK && got == len;
}

static void test_untar(const char* dir) {
	static const uint8_t bin[5] = { 0x00, '=', '\n', 'A', 0xff };
	static const uint8_t acl_user[44 + 24] = {
		0x01, 0x2c, 0xc1, 0x6d, [39] = 1, [44] = 0xff, 0xff, 0xee, 0xee, 0xdd, 0xdd, 0xcc, 0xcc, 0xbb, 0xbb, 0xaa, 0xaa,
		0, 0, 0x01, 0xf5, 0, 0, 0, 1, 0, 0, 0, 6
	};
	static const uint8_t acl_admin[44 + 24] = {
		0x01, 0x2c, 0xc1, 0x6d, [39] = 1, [44] = 0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef,
		0, 0, 0, 0x50, 0, 0, 0, 1, 0, 0, 0, 6
	};
	/* missing, later in the archive, a directory, the link itself, outside */
	static const char* bad_links[] = { "t3/missing", "t3/later", "t3/d", "t3/l", "../t/a" };
	static const uint8_t eoa[1024] = { 0 };
	char path[1024];
	uint8_t h[512], ad[1024], buf[20000], *d;
	tbuf_t b;
	pax_t r;
	apfs_image_t* img;
	apfs_vol_t* vol;
	apfs_inode_t in;
	apfslib_opts_t o;
	AbstractFile* af;
	uint64_t ino_a = 0, ino_link = 1;
	size_t len, i;

	printf("tar archives\n");
	memset(&b, 0, sizeof(b));
	memset(&r, 0, sizeof(r));
	pax_rec(&r, "uid", "1234", 4);
	tar_entry(&b, "PaxHeader", 'g', r.d, r.n, NULL);
	r.n = 0;
	pax_rec(&r, "mtime", "1234567890.123456789", 20);
	pax_rec(&r, "atime", "1300000000.25", 13);
	pax_rec(&r, "LIBARCHIVE.creationtime", "1111111111.5", 12);
	pax_rec(&r, "SCHILY.fflags", "bogus,hidden", 12);
	pax_rec(&r, "SCHILY.xattr.user.bin", bin, sizeof(bin));
	pax_rec(&r, "LIBARCHIVE.xattr.user%3Dodd", "dj0x", 4);
	pax_rec(&r, "SCHILY.acl.ace", "user:u:rw::allow:501,owner@:rwx::allow", 38);
	tar_entry(&b, "PaxHeader", 'x', r.d, r.n, NULL);
	tar_entry(&b, "t/a", '0', "A\n", 2, NULL);
	tar_entry(&b, "t/link", '1', "zzz", 3, "t/a");
	tar_entry(&b, "t/._f", '0', ad, make_appledouble(ad), NULL);
	tar_entry(&b, "t/f", '0', "F\n", 2, NULL);
	tar_entry(&b, "t/._o", '0', ad, make_appledouble(ad), NULL);
	tar_entry(&b, "t/other", '0', "O\n", 2, NULL);
	tar_hdr(h, "t/d/", '5', 0, NULL);
	tar_octal(h + 136, 12, 1500000000);
	tar_put(&b, h, NULL, 0);
	tar_entry(&b, "t/d/x", '0', "X\n", 2, NULL);
	/* old GNU sparse: 4 KiB at 0 and at 32 KiB of 64 KiB */
	memset(buf, 'A', 4096);
	memset(buf + 4096, 'B', 4096);
	tar_hdr(h, "t/sparse", 'S', 8192, NULL);
	memcpy(h + 257, "ustar  ", 8);
	tar_octal(h + 386, 12, 0);
	tar_octal(h + 398, 12, 4096);
	tar_octal(h + 410, 12, 32768);
	tar_octal(h + 422, 12, 4096);
	tar_octal(h + 483, 12, 65536);
	tar_put(&b, h, buf, 8192);
	/* PAX sparse 1.0: the map starts the data */
	r.n = 0;
	pax_rec(&r, "GNU.sparse.major", "1", 1);
	pax_rec(&r, "GNU.sparse.minor", "0", 1);
	pax_rec(&r, "GNU.sparse.name", "t/sparse1", 9);
	pax_rec(&r, "GNU.sparse.realsize", "20000", 5);
	tar_entry(&b, "PaxHeader", 'x', r.d, r.n, NULL);
	memset(buf, 0, 512);
	strcpy((char*)buf, "2\n0\n100\n10000\n50\n");
	memset(buf + 512, 'C', 100);
	memset(buf + 612, 'D', 50);
	tar_entry(&b, "t/GNUSparseFile.0/sparse1", '0', buf, 662, NULL);
	b.p = (uint8_t*)apfs_realloc(b.p, b.n + 1024);
	memset(b.p + b.n, 0, 1024);
	b.n += 1024;

	snprintf(path, sizeof(path), "%s/unit-tar.raw", dir);
	img = make_image(path, 32ULL << 20, 1);
	if(!img) {
		free(b.p);
		return;
	}
	apfslib_default_opts(&o);
	o.quiet = 1;
	CHECK(apfs_vol_open(img->nx, 0, &vol) == APFS_OK);
	af = createAbstractFileFromMemory((void**)&b.p, b.n);
	CHECK(apfslib_untar(vol, af, "/", &o) == APFS_OK);
	CLOSE(af);

	/* PAX values, the global uid included */
	CHECK(read_back(vol, "/t/a", buf, 2, &in) && !memcmp(buf, "A\n", 2));
	ino_a = in.ino;
	CHECK(in.uid == 1234 && in.mod_time == 1234567890123456789ULL && in.access_time == 1300000000250000000ULL);
	CHECK(in.create_time == 1111111111500000000ULL && in.bsd_flags == APFS_UF_HIDDEN);
	apfs_inode_release(&in);
	CHECK((d = xattr_of(vol, "/t/a", "user.bin", &len)) != NULL && len == sizeof(bin) && !memcmp(d, bin, len));
	free(d);
	CHECK((d = xattr_of(vol, "/t/a", "user=odd", &len)) != NULL && len == 3 && !memcmp(d, "v=1", 3));
	free(d);
	CHECK((d = xattr_of(vol, "/t/a", "com.apple.system.Security", &len)) != NULL && len == sizeof(acl_user) &&
		!memcmp(d, acl_user, len));
	free(d);
	/* the hard link, whose data is ignored */
	CHECK(apfs_lookup(vol, "/t/link", 0, &ino_link, NULL, NULL) == APFS_OK && ino_link == ino_a);
	/* AppleDouble entries join their file, or stay files when nothing matches */
	CHECK(apfs_lookup(vol, "/t/._f", 0, &ino_link, NULL, NULL) == APFS_ENOENT);
	CHECK((d = xattr_of(vol, "/t/f", "user.ad", &len)) != NULL && len == 8 && !memcmp(d, "from-ad", 8));
	free(d);
	CHECK((d = xattr_of(vol, "/t/f", "com.apple.FinderInfo", &len)) != NULL && len == 32 && !memcmp(d, "TEXTttxt", 8));
	free(d);
	CHECK((d = xattr_of(vol, "/t/f", "com.apple.ResourceFork", &len)) != NULL && len == 4 && !memcmp(d, "RSRC", 4));
	free(d);
	CHECK((d = xattr_of(vol, "/t/f", "com.apple.system.Security", &len)) != NULL && len == sizeof(acl_admin) &&
		!memcmp(d, acl_admin, len));
	free(d);
	CHECK(read_back(vol, "/t/._o", buf, 4, &in) && !memcmp(buf, "\0\x05\x16\x07", 4));
	apfs_inode_release(&in);
	CHECK(xattr_of(vol, "/t/other", "user.ad", &len) == NULL);
	/* directory times are set after their contents */
	CHECK(read_back(vol, "/t/d", buf, 0, &in) && in.mod_time == 1500000000000000000ULL);
	apfs_inode_release(&in);
	/* sparse files */
	CHECK(read_back(vol, "/t/sparse", buf, 20000, &in));
	for(i = 0; i < 20000 && buf[i] == (i < 4096 ? 'A' : 0); i++)
		;
	CHECK(i == 20000);
	apfs_inode_release(&in);
	CHECK(read_back(vol, "/t/sparse", buf + 4096, 4096, &in) && in.uncompressed_size == 0);
	apfs_inode_release(&in);
	CHECK(read_back(vol, "/t/sparse1", buf, 20000, &in));
	for(i = 0; i < 20000 && buf[i] == (i < 100 ? 'C' : i >= 10000 && i < 10050 ? 'D' : 0); i++)
		;
	CHECK(i == 20000);
	apfs_inode_release(&in);

	/* a path leaving the destination: nothing is written */
	b.n = 0;
	tar_entry(&b, "t2/ok", '0', "1", 1, NULL);
	tar_entry(&b, "../evil", '0', "2", 1, NULL);
	b.p = (uint8_t*)apfs_realloc(b.p, b.n + 1024);
	memset(b.p + b.n, 0, 1024);
	b.n += 1024;
	af = createAbstractFileFromMemory((void**)&b.p, b.n);
	CHECK(apfslib_untar(vol, af, "/", &o) == APFS_EINVAL);
	CLOSE(af);
	CHECK(apfs_lookup(vol, "/t2", 0, &ino_link, NULL, NULL) == APFS_ENOENT);

	/* hard links to anything but an earlier file or one in the volume:
	 * nothing is written */
	for(i = 0; i < sizeof(bad_links) / sizeof(bad_links[0]); i++) {
		b.n = 0;
		tar_entry(&b, "t3/ok", '0', "1", 1, NULL);
		tar_entry(&b, "t3/d/", '5', NULL, 0, NULL);
		tar_entry(&b, "t3/l", '0', "3", 1, NULL);
		tar_entry(&b, "t3/l", '1', NULL, 0, bad_links[i]);
		tar_entry(&b, "t3/later", '0', "2", 1, NULL);
		tb_add(&b, eoa, sizeof(eoa));
		af = createAbstractFileFromMemory((void**)&b.p, b.n);
		CHECK(apfslib_untar(vol, af, "/", &o) == APFS_EINVAL);
		CLOSE(af);
		CHECK(apfs_lookup(vol, "/t3", 0, &ino_link, NULL, NULL) == APFS_ENOENT);
	}
	/* a target already in the volume, one spelled differently (NFD), and
	 * one through a symbolic link from the archive */
	b.n = 0;
	tar_entry(&b, "t4/caf\xc3\xa9", '0', "3", 1, NULL);
	tar_entry(&b, "t4/old", '1', NULL, 0, "t/a");
	tar_entry(&b, "t4/nfd", '1', NULL, 0, "t4/cafe\xcc\x81");
	tar_entry(&b, "t4/s", '2', NULL, 0, "real");
	tar_entry(&b, "t4/real/f", '0', "4", 1, NULL);
	tar_entry(&b, "t4/via", '1', NULL, 0, "t4/s/f");
	tb_add(&b, eoa, sizeof(eoa));
	af = createAbstractFileFromMemory((void**)&b.p, b.n);
	CHECK(apfslib_untar(vol, af, "/", &o) == APFS_OK);
	CLOSE(af);
	CHECK(ino_of(vol, "/t4/old") == ino_a);
	CHECK(ino_of(vol, "/t4/nfd") != 0 && ino_of(vol, "/t4/nfd") == ino_of(vol, "/t4/caf\xc3\xa9"));
	CHECK(ino_of(vol, "/t4/via") != 0 && ino_of(vol, "/t4/via") == ino_of(vol, "/t4/real/f"));
	CHECK(apfs_nx_commit(img->nx) == APFS_OK);
	CHECK(apfs_verify(img->nx, 0) == APFS_OK);
	apfs_image_close(img, 1);
	remove(path);
	free(b.p);
}

int main(int argc, char** argv) {
	const char* dir = argc > 1 ? argv[1] : ".";

	apfs_verbosity = -1; /* expected failures would print errors */
	test_digests();
	test_crypto();
	test_name_hashes();
	test_trustcache(dir);
	test_untar(dir);
	test_mtree(dir);
	test_mkfs_over_old(dir);
	test_write_sparse(dir);
	test_free_queue_footer(dir);
	test_compressed_digest(dir);
	test_case_insensitive(dir);
	test_many_files(dir);
	workload(dir, 1, 0x1234567ULL, 30);
	workload(dir, 0, 0x89abcdefULL, 16);
	if(failures) {
		printf("unit tests: %d failure%s\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("unit tests: all passed\n");
	return 0;
}
