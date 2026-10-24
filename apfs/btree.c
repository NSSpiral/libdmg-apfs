/*
 * btree.c
 * apfs-tool
 *
 * Generic APFS B-tree engine. Nodes are parsed into arrays of records when
 * loaded and serialized again when written. Modifications follow APFS
 * copy-on-write rules: a node from an older transaction is relocated (or, for
 * virtual trees, remapped through the object map) before it is changed.
 */

#include <stdlib.h>
#include <string.h>

#include "apfs/apfs.h"

#define NODE_HDR_SIZE 56
#define NODE_INFO_SIZE 40

typedef struct {
	int depth;
	apfs_btnode_t* node[APFS_BT_MAX_DEPTH];
	int idx[APFS_BT_MAX_DEPTH];
} bt_path_t;

/* Comparators */

static int cmp_u64(uint64_t a, uint64_t b) {
	return a < b ? -1 : (a > b ? 1 : 0);
}

int apfs_cmp_omap(const apfs_btree_t* bt, const uint8_t* k1, uint16_t l1, const uint8_t* k2, uint16_t l2) {
	int c;
	(void)bt; (void)l1; (void)l2;
	c = cmp_u64(apfs_get64(k1), apfs_get64(k2));
	if(c)
		return c;
	return cmp_u64(apfs_get64(k1 + 8), apfs_get64(k2 + 8));
}

int apfs_cmp_u64(const apfs_btree_t* bt, const uint8_t* k1, uint16_t l1, const uint8_t* k2, uint16_t l2) {
	(void)bt; (void)l1; (void)l2;
	return cmp_u64(apfs_get64(k1), apfs_get64(k2));
}

int apfs_cmp_u64pair(const apfs_btree_t* bt, const uint8_t* k1, uint16_t l1, const uint8_t* k2, uint16_t l2) {
	return apfs_cmp_omap(bt, k1, l1, k2, l2);
}

/* Compares two NUL-terminated names stored inside keys, bounded by the key length */
static int cmp_names(const uint8_t* n1, int len1, const uint8_t* n2, int len2) {
	int i = 0;
	for(;;) {
		int c1 = i < len1 ? n1[i] : 0;
		int c2 = i < len2 ? n2[i] : 0;
		if(c1 != c2)
			return c1 < c2 ? -1 : 1;
		if(c1 == 0)
			return 0;
		i++;
	}
}

int apfs_cmp_fs(const apfs_btree_t* bt, const uint8_t* k1, uint16_t l1, const uint8_t* k2, uint16_t l2) {
	uint64_t h1, h2;
	int c;
	uint8_t t;

	if(l1 < 8 || l2 < 8)
		return cmp_u64(l1, l2);

	h1 = apfs_get64(k1);
	h2 = apfs_get64(k2);
	c = cmp_u64(h1 & OBJ_ID_MASK, h2 & OBJ_ID_MASK);
	if(c)
		return c;
	c = cmp_u64(h1 >> OBJ_TYPE_SHIFT, h2 >> OBJ_TYPE_SHIFT);
	if(c)
		return c;

	t = (uint8_t)(h1 >> OBJ_TYPE_SHIFT);
	switch(t) {
		case APFS_TYPE_DIR_REC:
			if(bt && bt->vol && bt->vol->hashed_names) {
				if(l1 < 12 || l2 < 12)
					return cmp_u64(l1, l2);
				c = cmp_u64(apfs_get32(k1 + 8) & J_DREC_HASH_MASK, apfs_get32(k2 + 8) & J_DREC_HASH_MASK);
				if(c)
					return c;
				return cmp_names(k1 + 12, l1 - 12, k2 + 12, l2 - 12);
			}
			if(l1 < 10 || l2 < 10)
				return cmp_u64(l1, l2);
			return cmp_names(k1 + 10, l1 - 10, k2 + 10, l2 - 10);
		case APFS_TYPE_XATTR:
		case APFS_TYPE_SNAP_NAME:
			if(l1 < 10 || l2 < 10)
				return cmp_u64(l1, l2);
			return cmp_names(k1 + 10, l1 - 10, k2 + 10, l2 - 10);
		case APFS_TYPE_FILE_EXTENT:
		case APFS_TYPE_SIBLING_LINK:
			if(l1 < 16 || l2 < 16)
				return cmp_u64(l1, l2);
			return cmp_u64(apfs_get64(k1 + 8), apfs_get64(k2 + 8));
		case APFS_TYPE_FILE_INFO:
			if(l1 < 16 || l2 < 16)
				return cmp_u64(l1, l2);
			return cmp_u64(apfs_get64(k1 + 8) & J_FILE_INFO_LBA_MASK, apfs_get64(k2 + 8) & J_FILE_INFO_LBA_MASK);
		default:
			return 0;
	}
}

/* Node cache */

static uint32_t hslot(const apfs_btree_t* bt, oid_t oid) {
	return (uint32_t)((oid * 0x9E3779B97F4A7C15ULL) >> 32) & (bt->hsize - 1);
}

static apfs_btnode_t* cache_find(apfs_btree_t* bt, oid_t oid) {
	apfs_btnode_t* n;
	if(!bt->htab)
		return NULL;
	for(n = bt->htab[hslot(bt, oid)]; n; n = n->hnext)
		if(n->oid == oid)
			return n;
	return NULL;
}

static void cache_insert(apfs_btree_t* bt, apfs_btnode_t* node) {
	uint32_t s;
	if(!bt->htab) {
		bt->hsize = 256;
		bt->htab = (apfs_btnode_t**)apfs_calloc(bt->hsize, sizeof(apfs_btnode_t*));
	} else if(bt->hcount > bt->hsize * 2) {
		uint32_t oldsize = bt->hsize, i;
		apfs_btnode_t** old = bt->htab;
		bt->hsize *= 4;
		bt->htab = (apfs_btnode_t**)apfs_calloc(bt->hsize, sizeof(apfs_btnode_t*));
		for(i = 0; i < oldsize; i++) {
			apfs_btnode_t* n = old[i];
			while(n) {
				apfs_btnode_t* next = n->hnext;
				s = hslot(bt, n->oid);
				n->hnext = bt->htab[s];
				bt->htab[s] = n;
				n = next;
			}
		}
		free(old);
	}
	s = hslot(bt, node->oid);
	node->hnext = bt->htab[s];
	bt->htab[s] = node;
	bt->hcount++;
}

static void cache_remove(apfs_btree_t* bt, apfs_btnode_t* node) {
	apfs_btnode_t** pp;
	if(!bt->htab)
		return;
	for(pp = &bt->htab[hslot(bt, node->oid)]; *pp; pp = &(*pp)->hnext) {
		if(*pp == node) {
			*pp = node->hnext;
			node->hnext = NULL;
			bt->hcount--;
			return;
		}
	}
}

static void node_free_mem(apfs_btnode_t* n) {
	uint32_t i;
	for(i = 0; i < n->nrec; i++)
		free(n->rec[i].k);
	free(n->rec);
	free(n);
}

/* Records */

static void node_reserve(apfs_btnode_t* n, uint32_t count) {
	if(count <= n->cap)
		return;
	n->cap = count < 16 ? 16 : count * 2;
	n->rec = (apfs_btrec_t*)apfs_realloc(n->rec, n->cap * sizeof(apfs_btrec_t));
}

static void rec_set(apfs_btrec_t* r, const void* k, uint16_t klen, const void* v, uint16_t vlen) {
	uint16_t vbytes = (vlen == 0xffff) ? 0 : vlen;
	r->k = (uint8_t*)apfs_malloc((size_t)klen + vbytes);
	memcpy(r->k, k, klen);
	if(vbytes)
		memcpy(r->k + klen, v, vbytes);
	r->v = r->k + klen;
	r->klen = klen;
	r->vlen = vlen;
}

static void node_insert_rec(apfs_btnode_t* n, uint32_t pos, const void* k, uint16_t klen, const void* v, uint16_t vlen) {
	node_reserve(n, n->nrec + 1);
	memmove(&n->rec[pos + 1], &n->rec[pos], (n->nrec - pos) * sizeof(apfs_btrec_t));
	rec_set(&n->rec[pos], k, klen, v, vlen);
	n->nrec++;
}

static void node_remove_rec(apfs_btnode_t* n, uint32_t pos) {
	free(n->rec[pos].k);
	memmove(&n->rec[pos], &n->rec[pos + 1], (n->nrec - pos - 1) * sizeof(apfs_btrec_t));
	n->nrec--;
}

static void node_replace_rec(apfs_btnode_t* n, uint32_t pos, const void* k, uint16_t klen, const void* v, uint16_t vlen) {
	apfs_btrec_t r;
	rec_set(&r, k, klen, v, vlen);
	free(n->rec[pos].k);
	n->rec[pos] = r;
}

/* Layout helpers */

static uint32_t node_bs(const apfs_btree_t* bt) {
	return bt->nx->bs;
}

static uint16_t index_vlen(const apfs_btree_t* bt) {
	return (uint16_t)(bt->hashed ? 8 + bt->hash_size : 8);
}

static uint32_t fixed_entry_cap(const apfs_btree_t* bt, int leaf) {
	uint32_t vlen = leaf ? bt->fixed_vlen : 8;
	return (node_bs(bt) - NODE_HDR_SIZE) / (bt->fixed_klen + vlen + 4);
}

static uint32_t var_toc_bytes(uint32_t nrec) {
	uint32_t entries = (nrec + 7) & ~7U;
	if(entries == 0)
		entries = 8;
	return entries * 8;
}

static uint32_t node_toc_bytes(const apfs_btree_t* bt, const apfs_btnode_t* n) {
	if(bt->fixed)
		return fixed_entry_cap(bt, n->level == 0) * 4;
	return var_toc_bytes(n->nrec);
}

static uint32_t node_payload(const apfs_btnode_t* n) {
	uint32_t i, total = 0;
	for(i = 0; i < n->nrec; i++)
		total += n->rec[i].klen + (n->rec[i].vlen == 0xffff ? 0 : n->rec[i].vlen);
	return total;
}

static int node_fits(const apfs_btree_t* bt, const apfs_btnode_t* n, int is_root) {
	uint32_t avail = node_bs(bt) - NODE_HDR_SIZE - (is_root ? NODE_INFO_SIZE : 0);
	uint32_t toc = node_toc_bytes(bt, n);
	if(bt->fixed && n->nrec > fixed_entry_cap(bt, n->level == 0))
		return 0;
	if(toc > avail)
		return 0;
	return node_payload(n) <= avail - toc;
}

/* Parse a raw node block into records */
static int node_parse(apfs_btree_t* bt, apfs_btnode_t* n, const uint8_t* raw) {
	uint32_t bs = node_bs(bt);
	uint16_t flags = apfs_get16(raw + 32);
	uint16_t level = apfs_get16(raw + 34);
	uint32_t nkeys = apfs_get32(raw + 36);
	uint16_t toc_off = apfs_get16(raw + 40);
	uint16_t toc_len = apfs_get16(raw + 42);
	uint32_t toc_start = NODE_HDR_SIZE + toc_off;
	uint32_t key_start = toc_start + toc_len;
	uint32_t val_end = bs - ((flags & BTNODE_ROOT) ? NODE_INFO_SIZE : 0);
	int fixed = (flags & BTNODE_FIXED_KV_SIZE) != 0;
	uint32_t i;

	n->flags = flags;
	n->level = level;
	n->nrec = 0;

	if(key_start > val_end || nkeys > bs / 4) {
		APFS_ERR("B-tree node 0x%llx has an invalid layout", (unsigned long long)n->paddr);
		return APFS_ECORRUPT;
	}
	if(fixed && (!bt->fixed_klen || !bt->fixed_vlen)) {
		APFS_ERR("B-tree node 0x%llx has fixed-size records but the tree has no sizes", (unsigned long long)n->paddr);
		return APFS_ECORRUPT;
	}

	node_reserve(n, nkeys);
	for(i = 0; i < nkeys; i++) {
		uint32_t koff, klen, voff, vlen;
		const uint8_t* kp;
		const uint8_t* vp;

		if(fixed) {
			const uint8_t* e = raw + toc_start + i * 4;
			if(toc_start + i * 4 + 4 > key_start)
				return APFS_ECORRUPT;
			koff = apfs_get16(e);
			voff = apfs_get16(e + 2);
			klen = bt->fixed_klen;
			vlen = (level == 0) ? bt->fixed_vlen : 8;
			if(voff == BTOFF_INVALID) {
				vlen = 0xffff;
				voff = 0;
			}
		} else {
			const uint8_t* e = raw + toc_start + i * 8;
			if(toc_start + i * 8 + 8 > key_start)
				return APFS_ECORRUPT;
			koff = apfs_get16(e);
			klen = apfs_get16(e + 2);
			voff = apfs_get16(e + 4);
			vlen = apfs_get16(e + 6);
		}

		if(key_start + koff + klen > val_end) {
			APFS_ERR("B-tree node 0x%llx: key %u out of bounds", (unsigned long long)n->paddr, i);
			return APFS_ECORRUPT;
		}
		kp = raw + key_start + koff;
		if(vlen == 0xffff) {
			vp = NULL;
		} else {
			if(voff > val_end - key_start || vlen > voff) {
				APFS_ERR("B-tree node 0x%llx: value %u out of bounds", (unsigned long long)n->paddr, i);
				return APFS_ECORRUPT;
			}
			vp = raw + val_end - voff;
		}
		rec_set(&n->rec[i], kp, (uint16_t)klen, vp, (uint16_t)vlen);
		n->nrec++;
	}
	return APFS_OK;
}

/* Serialize records into a raw node block */
static void node_serialize(apfs_btree_t* bt, apfs_btnode_t* n, uint8_t* raw) {
	uint32_t bs = node_bs(bt);
	int is_root = (n->oid == bt->root);
	uint32_t toc = node_toc_bytes(bt, n);
	uint32_t key_start = NODE_HDR_SIZE + toc;
	uint32_t val_end = bs - (is_root ? NODE_INFO_SIZE : 0);
	uint32_t koff = 0, voff = 0;
	uint16_t flags = 0;
	uint32_t i;

	memset(raw, 0, bs);
	if(is_root)
		flags |= BTNODE_ROOT;
	if(n->level == 0)
		flags |= BTNODE_LEAF;
	if(bt->fixed)
		flags |= BTNODE_FIXED_KV_SIZE;
	if(bt->hashed)
		flags |= BTNODE_HASHED;
	if(bt->noheader)
		flags |= BTNODE_NOHEADER;
	n->flags = flags;

	apfs_put16(raw + 32, flags);
	apfs_put16(raw + 34, n->level);
	apfs_put32(raw + 36, n->nrec);
	apfs_put16(raw + 40, 0);
	apfs_put16(raw + 42, (uint16_t)toc);

	for(i = 0; i < n->nrec; i++) {
		apfs_btrec_t* r = &n->rec[i];
		uint16_t vbytes = r->vlen == 0xffff ? 0 : r->vlen;

		memcpy(raw + key_start + koff, r->k, r->klen);
		if(r->vlen != 0xffff) {
			voff += vbytes;
			memcpy(raw + val_end - voff, r->v, vbytes);
		}
		if(bt->fixed) {
			uint8_t* e = raw + NODE_HDR_SIZE + i * 4;
			apfs_put16(e, (uint16_t)koff);
			apfs_put16(e + 2, r->vlen == 0xffff ? BTOFF_INVALID : (uint16_t)voff);
		} else {
			uint8_t* e = raw + NODE_HDR_SIZE + i * 8;
			apfs_put16(e, (uint16_t)koff);
			apfs_put16(e + 2, r->klen);
			apfs_put16(e + 4, (uint16_t)voff);
			apfs_put16(e + 6, r->vlen);
		}
		koff += r->klen;
	}

	/* free space between the keys and the values; free lists are empty */
	apfs_put16(raw + 44, (uint16_t)koff);
	apfs_put16(raw + 46, (uint16_t)((val_end - voff) - (key_start + koff)));
	apfs_put16(raw + 48, BTOFF_INVALID);
	apfs_put16(raw + 50, 0);
	apfs_put16(raw + 52, BTOFF_INVALID);
	apfs_put16(raw + 54, 0);

	if(is_root) {
		uint8_t* info = raw + bs - NODE_INFO_SIZE;
		apfs_put32(info, bt->info.bt_fixed.bt_flags);
		apfs_put32(info + 4, bt->info.bt_fixed.bt_node_size);
		apfs_put32(info + 8, bt->info.bt_fixed.bt_key_size);
		apfs_put32(info + 12, bt->info.bt_fixed.bt_val_size);
		apfs_put32(info + 16, bt->info.bt_longest_key);
		apfs_put32(info + 20, bt->info.bt_longest_val);
		apfs_put64(info + 24, bt->info.bt_key_count);
		apfs_put64(info + 32, bt->info.bt_node_count);
	}

	if(!bt->noheader) {
		uint32_t otype = (is_root ? OBJECT_TYPE_BTREE : OBJECT_TYPE_BTREE_NODE) | bt->storage;
		apfs_put64(raw + 8, n->oid);
		apfs_put64(raw + 16, n->xid);
		apfs_put32(raw + 24, otype);
		apfs_put32(raw + 28, bt->subtype);
		apfs_obj_set_cksum(raw, bs);
	}
}

void apfs_bt_init(apfs_btree_t* bt, apfs_nx_t* nx, apfs_vol_t* vol, apfs_omap_t* omap,
		uint32_t storage, uint32_t subtype, oid_t root, apfs_keycmp_fn cmp) {
	memset(bt, 0, sizeof(*bt));
	bt->nx = nx;
	bt->vol = vol;
	bt->omap = omap;
	bt->storage = storage;
	bt->subtype = subtype;
	bt->root = root;
	bt->cmp = cmp;
}

static void bt_apply_info(apfs_btree_t* bt) {
	uint32_t f = bt->info.bt_fixed.bt_flags;
	bt->hashed = (f & BTREE_HASHED) != 0;
	bt->noheader = (f & BTREE_NOHEADER) != 0;
	if(bt->hashed && !bt->hash_size)
		bt->hash_size = APFS_HASH_CCSHA256_SIZE;
	if(bt->info.bt_fixed.bt_key_size && bt->info.bt_fixed.bt_val_size) {
		bt->fixed = 1;
		bt->fixed_klen = (uint16_t)bt->info.bt_fixed.bt_key_size;
		bt->fixed_vlen = (uint16_t)bt->info.bt_fixed.bt_val_size;
	}
}

static int read_node_raw(apfs_btree_t* bt, oid_t oid, uint8_t* buf, paddr_t* paddr_out, xid_t* xid_out) {
	apfs_nx_t* nx = bt->nx;
	paddr_t paddr = oid;
	xid_t xid = 0;
	int noheader = bt->noheader;
	int ret;

	if(bt->storage == OBJ_EPHEMERAL) {
		apfs_eph_t* e = apfs_nx_eph(nx, oid);
		if(!e) {
			APFS_ERR("ephemeral B-tree node 0x%llx is missing", (unsigned long long)oid);
			return APFS_ECORRUPT;
		}
		memcpy(buf, e->data, nx->bs);
		*paddr_out = 0;
		*xid_out = apfs_get64(buf + 16);
		return APFS_OK;
	}

	if(bt->storage == OBJ_VIRTUAL) {
		uint32_t vflags = 0;
		ret = apfs_omap_lookup(bt->omap, oid, bt->read_xid, &paddr, &xid, &vflags);
		if(ret < 0) {
			APFS_ERR("object map has no mapping for B-tree node 0x%llx", (unsigned long long)oid);
			return ret == APFS_ENOENT ? APFS_ECORRUPT : ret;
		}
		if(vflags & OMAP_VAL_DELETED)
			return APFS_ECORRUPT;
		/* the root of a sealed tree is known to lack a header before the tree is loaded */
		if(vflags & OMAP_VAL_NOHEADER)
			noheader = 1;
	}

	ret = apfs_read_blocks(nx, paddr, 1, buf);
	if(ret < 0)
		return ret;

	if(!noheader) {
		uint32_t type;
		if(!apfs_obj_verify(buf, nx->bs)) {
			APFS_ERR("bad checksum in B-tree node 0x%llx (block 0x%llx)", (unsigned long long)oid, (unsigned long long)paddr);
			return APFS_ECORRUPT;
		}
		type = apfs_get32(buf + 24) & OBJECT_TYPE_MASK;
		if((type != OBJECT_TYPE_BTREE && type != OBJECT_TYPE_BTREE_NODE) || apfs_get64(buf + 8) != oid) {
			APFS_ERR("block 0x%llx is not B-tree node 0x%llx", (unsigned long long)paddr, (unsigned long long)oid);
			return APFS_ECORRUPT;
		}
		xid = apfs_get64(buf + 16);
	}

	*paddr_out = paddr;
	*xid_out = xid;
	return APFS_OK;
}

int apfs_bt_get_node(apfs_btree_t* bt, oid_t oid, apfs_btnode_t** out) {
	apfs_btnode_t* n = cache_find(bt, oid);
	uint8_t* buf;
	int ret;

	if(n) {
		*out = n;
		return APFS_OK;
	}

	buf = (uint8_t*)apfs_malloc(bt->nx->bs);
	n = (apfs_btnode_t*)apfs_calloc(1, sizeof(apfs_btnode_t));
	n->oid = oid;
	ret = read_node_raw(bt, oid, buf, &n->paddr, &n->xid);
	if(ret == APFS_OK) {
		uint16_t flags = apfs_get16(buf + 32);
		if((flags & BTNODE_ROOT) && !bt->loaded) {
			const uint8_t* info = buf + bt->nx->bs - NODE_INFO_SIZE;
			bt->info.bt_fixed.bt_flags = apfs_get32(info);
			bt->info.bt_fixed.bt_node_size = apfs_get32(info + 4);
			bt->info.bt_fixed.bt_key_size = apfs_get32(info + 8);
			bt->info.bt_fixed.bt_val_size = apfs_get32(info + 12);
			bt->info.bt_longest_key = apfs_get32(info + 16);
			bt->info.bt_longest_val = apfs_get32(info + 20);
			bt->info.bt_key_count = apfs_get64(info + 24);
			bt->info.bt_node_count = apfs_get64(info + 32);
			bt_apply_info(bt);
			bt->loaded = 1;
		}
		ret = node_parse(bt, n, buf);
	}
	free(buf);
	if(ret < 0) {
		node_free_mem(n);
		return ret;
	}
	cache_insert(bt, n);
	*out = n;
	return APFS_OK;
}

int apfs_bt_load(apfs_btree_t* bt) {
	apfs_btnode_t* root;
	int ret;
	if(bt->loaded)
		return APFS_OK;
	ret = apfs_bt_get_node(bt, bt->root, &root);
	if(ret < 0)
		return ret;
	if(!(root->flags & BTNODE_ROOT)) {
		APFS_ERR("B-tree root 0x%llx is not flagged as root", (unsigned long long)bt->root);
		return APFS_ECORRUPT;
	}
	return APFS_OK;
}

int apfs_bt_child_oid(const apfs_btree_t* bt, const apfs_btrec_t* rec, oid_t* oid) {
	if(rec->vlen == 0xffff || rec->vlen < 8)
		return APFS_ECORRUPT;
	*oid = apfs_get64(rec->v);
	if(bt->hashed)
		*oid += bt->root;
	return APFS_OK;
}

static void make_child_val(const apfs_btree_t* bt, oid_t oid, uint8_t* val, const uint8_t* hash) {
	if(bt->hashed) {
		apfs_put64(val, oid - bt->root);
		if(hash)
			memcpy(val + 8, hash, bt->hash_size);
		else
			memset(val + 8, 0, bt->hash_size);
	} else {
		apfs_put64(val, oid);
	}
}

/* Search helpers */

/* First record >= key */
static uint32_t leaf_search(const apfs_btree_t* bt, const apfs_btnode_t* n, const uint8_t* key, uint16_t klen, int* exact) {
	uint32_t lo = 0, hi = n->nrec;
	*exact = 0;
	while(lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		int c = bt->cmp(bt, n->rec[mid].k, n->rec[mid].klen, key, klen);
		if(c < 0) {
			lo = mid + 1;
		} else {
			if(c == 0)
				*exact = 1;
			hi = mid;
		}
	}
	if(lo < n->nrec && bt->cmp(bt, n->rec[lo].k, n->rec[lo].klen, key, klen) == 0)
		*exact = 1;
	else
		*exact = 0;
	return lo;
}

/* Last record <= key, or -1 */
static int index_search(const apfs_btree_t* bt, const apfs_btnode_t* n, const uint8_t* key, uint16_t klen) {
	int lo = 0, hi = (int)n->nrec - 1, ans = -1;
	while(lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = bt->cmp(bt, n->rec[mid].k, n->rec[mid].klen, key, klen);
		if(c <= 0) {
			ans = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return ans;
}

/* Copy-on-write */

static int node_cow(apfs_btree_t* bt, apfs_btnode_t* n, apfs_btnode_t* parent, int pidx) {
	apfs_nx_t* nx = bt->nx;
	int ret;

	if(n->xid == nx->txn) {
		if(!n->dirty) {
			n->dirty = 1;
			bt->ndirty++;
		}
		return APFS_OK;
	}
	if(bt->hashed || bt->noheader) {
		APFS_ERR("sealed volumes must be unsealed before they can be modified");
		return APFS_EROFS;
	}

	switch(bt->storage) {
		case OBJ_PHYSICAL: {
			paddr_t newp;
			oid_t old = n->oid;
			ret = apfs_sm_alloc_block(nx, bt->vol, &newp);
			if(ret < 0)
				return ret;
			ret = apfs_sm_free(nx, bt->vol, n->paddr, 1);
			if(ret < 0)
				return ret;
			cache_remove(bt, n);
			n->oid = newp;
			n->paddr = newp;
			cache_insert(bt, n);
			if(parent) {
				uint8_t v[8 + APFS_HASH_MAX_SIZE];
				make_child_val(bt, newp, v, NULL);
				memcpy(parent->rec[pidx].v, v, 8);
			} else if(bt->root == old) {
				bt->root = newp;
			}
			break;
		}
		case OBJ_VIRTUAL: {
			paddr_t newp;
			ret = apfs_omap_cow(bt->omap, n->oid, n->xid, n->paddr, &newp, 0);
			if(ret < 0)
				return ret;
			n->paddr = newp;
			break;
		}
		case OBJ_EPHEMERAL:
			break;
	}

	n->xid = nx->txn;
	if(!n->dirty) {
		n->dirty = 1;
		bt->ndirty++;
	}
	return APFS_OK;
}

static int node_new(apfs_btree_t* bt, uint16_t level, apfs_btnode_t** out) {
	apfs_nx_t* nx = bt->nx;
	apfs_btnode_t* n = (apfs_btnode_t*)apfs_calloc(1, sizeof(apfs_btnode_t));
	int ret;

	n->level = level;
	n->xid = nx->txn;
	switch(bt->storage) {
		case OBJ_PHYSICAL:
			ret = apfs_sm_alloc_block(nx, bt->vol, &n->paddr);
			if(ret < 0) {
				free(n);
				return ret;
			}
			n->oid = n->paddr;
			break;
		case OBJ_VIRTUAL:
			ret = apfs_sm_alloc_block(nx, bt->vol, &n->paddr);
			if(ret < 0) {
				free(n);
				return ret;
			}
			n->oid = apfs_nx_new_oid(nx);
			ret = apfs_omap_set(bt->omap, n->oid, nx->txn, 0, n->paddr);
			if(ret < 0) {
				free(n);
				return ret;
			}
			break;
		default:
			n->oid = apfs_nx_new_oid(nx);
			break;
	}
	n->dirty = 1;
	bt->ndirty++;
	cache_insert(bt, n);
	bt->info.bt_node_count++;
	*out = n;
	return APFS_OK;
}

static int node_destroy(apfs_btree_t* bt, apfs_btnode_t* n) {
	apfs_nx_t* nx = bt->nx;
	int ret = APFS_OK;

	switch(bt->storage) {
		case OBJ_PHYSICAL:
			ret = apfs_sm_free(nx, bt->vol, n->paddr, 1);
			break;
		case OBJ_VIRTUAL:
			ret = apfs_omap_delete_object(bt->omap, n->oid, n->xid, n->paddr);
			break;
		default:
			break;
	}
	if(n->dirty)
		bt->ndirty--;
	cache_remove(bt, n);
	node_free_mem(n);
	if(bt->info.bt_node_count)
		bt->info.bt_node_count--;
	return ret;
}

static int descend(apfs_btree_t* bt, const uint8_t* key, uint16_t klen, bt_path_t* p, int write, int* exact) {
	apfs_btnode_t* n;
	int ret;

	ret = apfs_bt_get_node(bt, bt->root, &n);
	if(ret < 0)
		return ret;
	if(write) {
		ret = node_cow(bt, n, NULL, 0);
		if(ret < 0)
			return ret;
	}

	p->depth = 0;
	for(;;) {
		if(p->depth >= APFS_BT_MAX_DEPTH) {
			APFS_ERR("B-tree is too deep");
			return APFS_ECORRUPT;
		}
		p->node[p->depth] = n;
		if(n->level == 0) {
			p->idx[p->depth] = (int)leaf_search(bt, n, key, klen, exact);
			p->depth++;
			return APFS_OK;
		} else {
			int c = index_search(bt, n, key, klen);
			apfs_btnode_t* child;
			oid_t coid;
			if(c < 0)
				c = 0;
			if(n->nrec == 0) {
				APFS_ERR("empty index node 0x%llx", (unsigned long long)n->oid);
				return APFS_ECORRUPT;
			}
			p->idx[p->depth] = c;
			p->depth++;
			ret = apfs_bt_child_oid(bt, &n->rec[c], &coid);
			if(ret < 0)
				return ret;
			ret = apfs_bt_get_node(bt, coid, &child);
			if(ret < 0)
				return ret;
			if(child->level + 1 != n->level) {
				APFS_ERR("B-tree node levels are inconsistent (node 0x%llx)", (unsigned long long)coid);
				return APFS_ECORRUPT;
			}
			if(write) {
				ret = node_cow(bt, child, n, c);
				if(ret < 0)
					return ret;
			}
			n = child;
		}
	}
}

int apfs_bt_lookup(apfs_btree_t* bt, const void* key, uint16_t klen, apfs_kv_t* out) {
	bt_path_t p;
	int exact = 0, ret;
	apfs_btnode_t* leaf;
	apfs_btrec_t* r;

	ret = descend(bt, (const uint8_t*)key, klen, &p, 0, &exact);
	if(ret < 0)
		return ret;
	if(!exact)
		return APFS_ENOENT;
	leaf = p.node[p.depth - 1];
	r = &leaf->rec[p.idx[p.depth - 1]];
	if(out) {
		out->k = r->k;
		out->klen = r->klen;
		out->v = r->v;
		out->vlen = r->vlen == 0xffff ? 0 : r->vlen;
	}
	return APFS_OK;
}

int apfs_bt_lookup_le(apfs_btree_t* bt, const void* key, uint16_t klen, apfs_kv_t* out) {
	apfs_btnode_t* n;
	int ret, depth = 0;

	ret = apfs_bt_get_node(bt, bt->root, &n);
	if(ret < 0)
		return ret;
	for(;;) {
		int c = index_search(bt, n, (const uint8_t*)key, klen);
		if(c < 0)
			return APFS_ENOENT;
		if(n->level == 0) {
			apfs_btrec_t* r = &n->rec[c];
			out->k = r->k;
			out->klen = r->klen;
			out->v = r->v;
			out->vlen = r->vlen == 0xffff ? 0 : r->vlen;
			return APFS_OK;
		} else {
			oid_t coid;
			ret = apfs_bt_child_oid(bt, &n->rec[c], &coid);
			if(ret < 0)
				return ret;
			ret = apfs_bt_get_node(bt, coid, &n);
			if(ret < 0)
				return ret;
			if(++depth >= APFS_BT_MAX_DEPTH)
				return APFS_ECORRUPT;
		}
	}
}

/* Iteration */

static int iter_descend_left(apfs_btiter_t* it) {
	for(;;) {
		apfs_btnode_t* n = it->node[it->depth - 1];
		apfs_btnode_t* child;
		oid_t coid;
		int ret;

		if(n->level == 0)
			return APFS_OK;
		if(it->depth >= APFS_BT_MAX_DEPTH)
			return APFS_ECORRUPT;
		ret = apfs_bt_child_oid(it->bt, &n->rec[it->idx[it->depth - 1]], &coid);
		if(ret < 0)
			return ret;
		ret = apfs_bt_get_node(it->bt, coid, &child);
		if(ret < 0)
			return ret;
		it->node[it->depth] = child;
		it->idx[it->depth] = 0;
		it->depth++;
	}
}

/* Moves up until a node has a next entry, then down to the leftmost leaf */
static int iter_advance_node(apfs_btiter_t* it) {
	int ret;
	while(it->depth > 1) {
		apfs_btnode_t* parent;
		it->depth--;
		parent = it->node[it->depth - 1];
		if((uint32_t)(it->idx[it->depth - 1] + 1) < parent->nrec) {
			it->idx[it->depth - 1]++;
			ret = iter_descend_left(it);
			if(ret < 0)
				return ret;
			if(it->node[it->depth - 1]->nrec == 0)
				continue;
			it->valid = 1;
			return APFS_OK;
		}
	}
	it->valid = 0;
	return APFS_OK;
}

int apfs_bt_iter_seek(apfs_btiter_t* it, apfs_btree_t* bt, const void* key, uint16_t klen) {
	apfs_btnode_t* n;
	int ret;

	memset(it, 0, sizeof(*it));
	it->bt = bt;
	ret = apfs_bt_get_node(bt, bt->root, &n);
	if(ret < 0)
		return ret;
	it->node[0] = n;
	it->depth = 1;
	for(;;) {
		n = it->node[it->depth - 1];
		if(n->level == 0) {
			int exact;
			uint32_t pos = key ? leaf_search(bt, n, (const uint8_t*)key, klen, &exact) : 0;
			it->idx[it->depth - 1] = (int)pos;
			if(pos >= n->nrec)
				return iter_advance_node(it);
			it->valid = 1;
			return APFS_OK;
		} else {
			int c = key ? index_search(bt, n, (const uint8_t*)key, klen) : 0;
			apfs_btnode_t* child;
			oid_t coid;
			if(c < 0)
				c = 0;
			if(n->nrec == 0)
				return APFS_ECORRUPT;
			it->idx[it->depth - 1] = c;
			if(it->depth >= APFS_BT_MAX_DEPTH)
				return APFS_ECORRUPT;
			ret = apfs_bt_child_oid(bt, &n->rec[c], &coid);
			if(ret < 0)
				return ret;
			ret = apfs_bt_get_node(bt, coid, &child);
			if(ret < 0)
				return ret;
			it->node[it->depth] = child;
			it->idx[it->depth] = 0;
			it->depth++;
		}
	}
}

int apfs_bt_iter_get(apfs_btiter_t* it, apfs_kv_t* out) {
	apfs_btnode_t* leaf;
	apfs_btrec_t* r;
	if(!it->valid)
		return APFS_ENOENT;
	leaf = it->node[it->depth - 1];
	r = &leaf->rec[it->idx[it->depth - 1]];
	out->k = r->k;
	out->klen = r->klen;
	out->v = r->v;
	out->vlen = r->vlen == 0xffff ? 0 : r->vlen;
	return APFS_OK;
}

int apfs_bt_iter_next(apfs_btiter_t* it) {
	apfs_btnode_t* leaf;
	if(!it->valid)
		return APFS_ENOENT;
	leaf = it->node[it->depth - 1];
	if((uint32_t)(it->idx[it->depth - 1] + 1) < leaf->nrec) {
		it->idx[it->depth - 1]++;
		return APFS_OK;
	}
	return iter_advance_node(it);
}

/* Modification */

static int choose_split(const apfs_btree_t* bt, const apfs_btnode_t* n, int append) {
	uint32_t total = node_payload(n);
	uint32_t acc = 0, i;
	int m;

	if(n->nrec < 2)
		return -1;

	if(append) {
		/* Sequential insertions: keep the left node as full as possible */
		apfs_btnode_t tmp;
		tmp = *n;
		for(m = (int)n->nrec - 1; m >= 1; m--) {
			tmp.nrec = (uint32_t)m;
			if(node_fits(bt, &tmp, 0))
				return m;
		}
		return 1;
	}

	for(i = 0; i < n->nrec; i++) {
		acc += n->rec[i].klen + (n->rec[i].vlen == 0xffff ? 0 : n->rec[i].vlen);
		if(acc * 2 >= total)
			break;
	}
	m = (int)i + 1;
	if(m >= (int)n->nrec)
		m = (int)n->nrec - 1;
	if(m < 1)
		m = 1;
	return m;
}

/* Splits n (child pidx of parent) until every part fits */
static int split_node(apfs_btree_t* bt, apfs_btnode_t* parent, int pidx, apfs_btnode_t* n, int append) {
	int ret;
	while(!node_fits(bt, n, 0)) {
		apfs_btnode_t* r;
		uint8_t cval[8 + APFS_HASH_MAX_SIZE];
		int m = choose_split(bt, n, append);
		uint32_t i, moved;

		if(m < 0) {
			APFS_ERR("B-tree record too large for a node");
			return APFS_ECORRUPT;
		}
		ret = node_new(bt, n->level, &r);
		if(ret < 0)
			return ret;
		moved = n->nrec - (uint32_t)m;
		node_reserve(r, moved);
		for(i = 0; i < moved; i++)
			r->rec[i] = n->rec[m + i];
		r->nrec = moved;
		n->nrec = (uint32_t)m;

		make_child_val(bt, r->oid, cval, NULL);
		node_insert_rec(parent, (uint32_t)pidx + 1, r->rec[0].k, r->rec[0].klen, cval, index_vlen(bt));

		/* the left part fits now when the split point was chosen by size */
		if(!node_fits(bt, n, 0)) {
			/* keep splitting the left part */
			continue;
		}
		n = r;
		pidx++;
	}
	return APFS_OK;
}

/* Turns the root into an index node above a new child holding its records */
static int grow_root(apfs_btree_t* bt, apfs_btnode_t* root, apfs_btnode_t** child_out) {
	apfs_btnode_t* c;
	uint8_t cval[8 + APFS_HASH_MAX_SIZE];
	int ret;

	ret = node_new(bt, root->level, &c);
	if(ret < 0)
		return ret;
	c->rec = root->rec;
	c->nrec = root->nrec;
	c->cap = root->cap;
	root->rec = NULL;
	root->nrec = 0;
	root->cap = 0;
	root->level++;
	make_child_val(bt, c->oid, cval, NULL);
	node_insert_rec(root, 0, c->rec[0].k, c->rec[0].klen, cval, index_vlen(bt));
	*child_out = c;
	return APFS_OK;
}

/* Pulls the only child of an index root up into the root */
static int collapse_root(apfs_btree_t* bt, apfs_btnode_t* root) {
	while(root->level > 0 && root->nrec == 1) {
		apfs_btnode_t* c;
		oid_t coid;
		int ret;
		ret = apfs_bt_child_oid(bt, &root->rec[0], &coid);
		if(ret < 0)
			return ret;
		ret = apfs_bt_get_node(bt, coid, &c);
		if(ret < 0)
			return ret;
		if(c->nrec && !node_fits(bt, c, 1))
			return APFS_OK; /* the child's records would not fit in the root */
		node_remove_rec(root, 0);
		free(root->rec);
		root->rec = c->rec;
		root->nrec = c->nrec;
		root->cap = c->cap;
		root->level = c->level;
		c->rec = NULL;
		c->nrec = 0;
		c->cap = 0;
		ret = node_destroy(bt, c);
		if(ret < 0)
			return ret;
	}
	if(root->level > 0 && root->nrec == 0)
		root->level = 0;
	return APFS_OK;
}

static int same_key(const apfs_btrec_t* a, const apfs_btrec_t* b) {
	return a->klen == b->klen && memcmp(a->k, b->k, a->klen) == 0;
}

static int bt_fix(apfs_btree_t* bt, bt_path_t* p, int append) {
	int lvl = p->depth - 1;
	int ret;

	while(lvl > 0) {
		apfs_btnode_t* n = p->node[lvl];
		apfs_btnode_t* parent = p->node[lvl - 1];
		int pidx = p->idx[lvl - 1];
		int changed = 0;

		if(n->nrec == 0) {
			ret = node_destroy(bt, n);
			if(ret < 0)
				return ret;
			node_remove_rec(parent, (uint32_t)pidx);
			changed = 1;
		} else {
			if(!same_key(&parent->rec[pidx], &n->rec[0])) {
				node_replace_rec(parent, (uint32_t)pidx, n->rec[0].k, n->rec[0].klen,
						parent->rec[pidx].v, parent->rec[pidx].vlen);
				changed = 1;
			}
			if(!node_fits(bt, n, 0)) {
				ret = split_node(bt, parent, pidx, n, append);
				if(ret < 0)
					return ret;
				changed = 1;
			}
		}
		if(!changed)
			return APFS_OK;
		lvl--;
	}

	/* root */
	{
		apfs_btnode_t* root = p->node[0];
		while(!node_fits(bt, root, 1)) {
			apfs_btnode_t* c;
			ret = grow_root(bt, root, &c);
			if(ret < 0)
				return ret;
			ret = split_node(bt, root, 0, c, append);
			if(ret < 0)
				return ret;
		}
		ret = collapse_root(bt, root);
		if(ret < 0)
			return ret;
	}
	return APFS_OK;
}

int apfs_bt_insert(apfs_btree_t* bt, const void* key, uint16_t klen, const void* val, uint16_t vlen) {
	bt_path_t p;
	apfs_btnode_t* leaf;
	int exact = 0, ret, pos, append;
	uint16_t vbytes = vlen == 0xffff ? 0 : vlen;

	ret = apfs_nx_begin(bt->nx);
	if(ret < 0)
		return ret;
	ret = apfs_bt_load(bt);
	if(ret < 0)
		return ret;
	if(bt->fixed && (klen != bt->fixed_klen || (vlen != 0xffff && vlen != bt->fixed_vlen))) {
		APFS_ERR("record size does not match fixed-size B-tree");
		return APFS_EINVAL;
	}

	ret = descend(bt, (const uint8_t*)key, klen, &p, 1, &exact);
	if(ret < 0)
		return ret;
	leaf = p.node[p.depth - 1];
	pos = p.idx[p.depth - 1];
	append = !exact && (uint32_t)pos == leaf->nrec;

	if(exact) {
		node_replace_rec(leaf, (uint32_t)pos, key, klen, val, vlen);
	} else {
		node_insert_rec(leaf, (uint32_t)pos, key, klen, val, vlen);
		bt->info.bt_key_count++;
	}
	if(klen > bt->info.bt_longest_key)
		bt->info.bt_longest_key = klen;
	if(vbytes > bt->info.bt_longest_val)
		bt->info.bt_longest_val = vbytes;

	return bt_fix(bt, &p, append);
}

int apfs_bt_delete(apfs_btree_t* bt, const void* key, uint16_t klen) {
	bt_path_t p;
	apfs_btnode_t* leaf;
	int exact = 0, ret;

	ret = apfs_bt_lookup(bt, key, klen, NULL);
	if(ret < 0)
		return ret;
	ret = apfs_nx_begin(bt->nx);
	if(ret < 0)
		return ret;
	ret = descend(bt, (const uint8_t*)key, klen, &p, 1, &exact);
	if(ret < 0)
		return ret;
	if(!exact)
		return APFS_ENOENT;
	leaf = p.node[p.depth - 1];
	node_remove_rec(leaf, (uint32_t)p.idx[p.depth - 1]);
	if(bt->info.bt_key_count)
		bt->info.bt_key_count--;
	return bt_fix(bt, &p, 0);
}

int apfs_bt_create(apfs_btree_t* bt, uint32_t bt_flags, uint16_t key_size, uint16_t val_size) {
	apfs_btnode_t* root;
	int ret;

	ret = apfs_nx_begin(bt->nx);
	if(ret < 0)
		return ret;
	memset(&bt->info, 0, sizeof(bt->info));
	bt->info.bt_fixed.bt_flags = bt_flags;
	bt->info.bt_fixed.bt_node_size = bt->nx->bs;
	bt->info.bt_fixed.bt_key_size = key_size;
	bt->info.bt_fixed.bt_val_size = val_size;
	bt->info.bt_longest_key = key_size;
	bt->info.bt_longest_val = val_size;
	bt_apply_info(bt);
	bt->loaded = 1;

	ret = node_new(bt, 0, &root);
	if(ret < 0)
		return ret;
	bt->info.bt_node_count = 1;
	bt->root = root->oid;
	return APFS_OK;
}

int apfs_bt_flush(apfs_btree_t* bt) {
	uint8_t* buf;
	uint32_t i;
	int ret = APFS_OK;

	if(!bt->htab || !bt->ndirty)
		return APFS_OK;
	buf = (uint8_t*)apfs_malloc(bt->nx->bs);
	for(i = 0; i < bt->hsize && ret == APFS_OK; i++) {
		apfs_btnode_t* n;
		for(n = bt->htab[i]; n; n = n->hnext) {
			if(!n->dirty)
				continue;
			node_serialize(bt, n, buf);
			if(bt->storage == OBJ_EPHEMERAL) {
				apfs_eph_t* e = apfs_nx_eph(bt->nx, n->oid);
				if(!e) {
					ret = APFS_EINVAL;
					break;
				}
				memcpy(e->data, buf, bt->nx->bs);
			} else {
				ret = apfs_write_blocks(bt->nx, n->paddr, 1, buf);
				if(ret < 0)
					break;
			}
			n->dirty = 0;
			bt->ndirty--;
		}
	}
	free(buf);
	return ret;
}

void apfs_bt_close(apfs_btree_t* bt) {
	uint32_t i;
	if(bt->htab) {
		for(i = 0; i < bt->hsize; i++) {
			apfs_btnode_t* n = bt->htab[i];
			while(n) {
				apfs_btnode_t* next = n->hnext;
				node_free_mem(n);
				n = next;
			}
		}
		free(bt->htab);
	}
	bt->htab = NULL;
	bt->hsize = 0;
	bt->hcount = 0;
	bt->ndirty = 0;
	bt->loaded = 0;
}

static int walk_node(apfs_btree_t* bt, oid_t oid, int depth, apfs_bt_node_cb cb, void* ctx) {
	apfs_btnode_t* n;
	uint32_t i, count;
	oid_t* children;
	int ret;

	if(depth >= APFS_BT_MAX_DEPTH)
		return APFS_ECORRUPT;
	ret = apfs_bt_get_node(bt, oid, &n);
	if(ret < 0)
		return ret;
	if(n->level == 0)
		return cb(bt, n, depth, ctx);

	/* the callback may drop the node, so collect the children first */
	count = n->nrec;
	children = (oid_t*)apfs_malloc(sizeof(oid_t) * (count ? count : 1));
	for(i = 0; i < count; i++) {
		ret = apfs_bt_child_oid(bt, &n->rec[i], &children[i]);
		if(ret < 0) {
			free(children);
			return ret;
		}
	}
	ret = cb(bt, n, depth, ctx);
	for(i = 0; i < count && ret >= 0; i++)
		ret = walk_node(bt, children[i], depth + 1, cb, ctx);
	free(children);
	return ret;
}

int apfs_bt_walk(apfs_btree_t* bt, apfs_bt_node_cb cb, void* ctx) {
	return walk_node(bt, bt->root, 0, cb, ctx);
}

static int count_cb(apfs_btree_t* bt, apfs_btnode_t* n, int depth, void* ctx) {
	uint64_t* c = (uint64_t*)ctx;
	(void)bt; (void)depth;
	c[1]++;
	if(n->level == 0)
		c[0] += n->nrec;
	return APFS_OK;
}

int apfs_bt_count(apfs_btree_t* bt, uint64_t* keys, uint64_t* nodes) {
	uint64_t c[2] = { 0, 0 };
	int ret = apfs_bt_walk(bt, count_cb, c);
	if(keys)
		*keys = c[0];
	if(nodes)
		*nodes = c[1];
	return ret;
}

typedef struct {
	oid_t* list;
	size_t count;
} oid_list_t;

static int free_cb(apfs_btree_t* bt, apfs_btnode_t* n, int depth, void* ctx) {
	oid_list_t* l = (oid_list_t*)ctx;
	(void)depth;
	(void)bt;
	l->list = (oid_t*)apfs_realloc(l->list, (l->count + 1) * sizeof(oid_t));
	l->list[l->count++] = n->oid;
	return APFS_OK;
}

/* Frees every node of the tree */
int apfs_bt_free_all(apfs_btree_t* bt) {
	oid_list_t ctx = { NULL, 0 };
	size_t i;
	int ret;

	ret = apfs_nx_begin(bt->nx);
	if(ret < 0)
		return ret;
	ret = apfs_bt_walk(bt, free_cb, &ctx);
	for(i = 0; i < ctx.count && ret >= 0; i++) {
		apfs_btnode_t* n;
		ret = apfs_bt_get_node(bt, ctx.list[i], &n);
		if(ret >= 0)
			ret = node_destroy(bt, n);
	}
	free(ctx.list);
	apfs_bt_close(bt);
	bt->root = 0;
	return ret;
}
