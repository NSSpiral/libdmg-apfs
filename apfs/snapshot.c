/*
 * snapshot.c
 * apfs-tool
 *
 * Volume snapshots. A snapshot keeps a copy of the volume superblock, takes
 * over the extent reference tree, and pins every object map entry visible at
 * its transaction. Deleting one releases what only it could see and hands its
 * physical extents to the following snapshot (or to the live volume).
 */

#include <stdlib.h>
#include <string.h>

#include "apfs/apfs.h"
#include "compat_macos.h"

#define SNAP_NAME_ID (~0ULL & OBJ_ID_MASK)
#define SNAP_NAME_MAX 255

void apfs_snapshots_free(apfs_snapshot_t* s, size_t count) {
	size_t i;
	for(i = 0; i < count; i++)
		free(s[i].name);
	free(s);
}

int apfs_snapshot_list(apfs_vol_t* vol, apfs_snapshot_t** out, size_t* count) {
	apfs_btiter_t it;
	apfs_kv_t kv;
	apfs_snapshot_t* list = NULL;
	size_t n = 0;
	uint8_t start[8];
	int ret;

	*out = NULL;
	*count = 0;
	if(!vol->snapmeta.loaded)
		return APFS_OK;

	apfs_key_hdr(start, 0, APFS_TYPE_SNAP_METADATA);
	ret = apfs_bt_iter_seek(&it, &vol->snapmeta, start, sizeof(start));
	while(ret == APFS_OK && it.valid) {
		apfs_snapshot_t* s;
		uint16_t nlen;
		apfs_bt_iter_get(&it, &kv);
		if(apfs_key_type(kv.k) == APFS_TYPE_SNAP_METADATA && kv.vlen >= 50) {
			nlen = apfs_get16(kv.v + 48);
			if(50 + (uint32_t)nlen > kv.vlen)
				nlen = (uint16_t)(kv.vlen - 50);
			list = (apfs_snapshot_t*)apfs_realloc(list, (n + 1) * sizeof(apfs_snapshot_t));
			s = &list[n++];
			memset(s, 0, sizeof(*s));
			s->xid = apfs_key_id(kv.k);
			s->extentref_tree_oid = apfs_get64(kv.v);
			s->sblock_oid = apfs_get64(kv.v + 8);
			s->create_time = apfs_get64(kv.v + 16);
			s->change_time = apfs_get64(kv.v + 24);
			s->flags = apfs_get32(kv.v + 44);
			s->name = (char*)apfs_malloc((size_t)nlen + 1);
			memcpy(s->name, kv.v + 50, nlen);
			s->name[nlen] = 0;
			s->name[strnlen(s->name, nlen)] = 0;
		}
		ret = apfs_bt_iter_next(&it);
	}
	if(ret < 0) {
		apfs_snapshots_free(list, n);
		return ret;
	}
	*out = list;
	*count = n;
	return APFS_OK;
}

static int check_name(const char* name) {
	size_t len = strlen(name);
	if(!len || !apfs_utf8_valid(name, len))
		return APFS_EINVAL;
	if(len > SNAP_NAME_MAX)
		return APFS_ENAMETOOLONG;
	return APFS_OK;
}

static uint16_t name_key(uint8_t* key, const char* name) {
	size_t len = strlen(name) + 1;
	apfs_key_hdr(key, SNAP_NAME_ID, APFS_TYPE_SNAP_NAME);
	apfs_put16(key + 8, (uint16_t)len);
	memcpy(key + 10, name, len);
	return (uint16_t)(10 + len);
}

/* Finds the transaction of a snapshot from its name */
static int find_snapshot(apfs_vol_t* vol, const char* name, xid_t* xid) {
	uint8_t key[10 + SNAP_NAME_MAX + 1];
	apfs_kv_t kv;
	int ret;

	if(!vol->snapmeta.loaded)
		return APFS_ENOENT;
	ret = apfs_bt_lookup(&vol->snapmeta, key, name_key(key, name), &kv);
	if(ret < 0)
		return ret;
	if(kv.vlen < 8)
		return APFS_ECORRUPT;
	*xid = apfs_get64(kv.v);
	return APFS_OK;
}

/* Writes the metadata record of a snapshot, given its fixed fields */
static int put_metadata(apfs_vol_t* vol, xid_t xid, const uint8_t* fixed, const char* name) {
	uint8_t key[8];
	uint8_t* val;
	size_t nlen = strlen(name) + 1;
	int ret;

	val = (uint8_t*)apfs_calloc(1, 50 + nlen);
	memcpy(val, fixed, 48);
	apfs_put16(val + 48, (uint16_t)nlen);
	memcpy(val + 50, name, nlen);
	apfs_key_hdr(key, xid, APFS_TYPE_SNAP_METADATA);
	ret = apfs_bt_insert(&vol->snapmeta, key, sizeof(key), val, (uint16_t)(50 + nlen));
	free(val);
	return ret;
}

static int get_metadata(apfs_vol_t* vol, xid_t xid, uint8_t* fixed) {
	uint8_t key[8];
	apfs_kv_t kv;
	int ret;

	apfs_key_hdr(key, xid, APFS_TYPE_SNAP_METADATA);
	ret = apfs_bt_lookup(&vol->snapmeta, key, sizeof(key), &kv);
	if(ret < 0)
		return ret == APFS_ENOENT ? APFS_ECORRUPT : ret;
	if(kv.vlen < 50)
		return APFS_ECORRUPT;
	memcpy(fixed, kv.v, 48);
	return APFS_OK;
}

/* The object map's snapshot tree lists the transactions of all snapshots */
static int open_omap_snap_tree(apfs_vol_t* vol, apfs_btree_t* bt) {
	omap_phys_t* om = APFS_OMAP_PHYS(&vol->omap);
	apfs_bt_init(bt, vol->nx, vol, NULL, OBJ_PHYSICAL, OBJECT_TYPE_OMAP_SNAPSHOT,
			LE64(om->om_snapshot_tree_oid), apfs_cmp_u64);
	if(!bt->root)
		return apfs_bt_create(bt, BTREE_PHYSICAL, 8, sizeof(omap_snapshot_t));
	return apfs_bt_load(bt);
}

static int close_omap_snap_tree(apfs_vol_t* vol, apfs_btree_t* bt) {
	omap_phys_t* om = APFS_OMAP_PHYS(&vol->omap);
	int ret = APFS_OK;

	if(bt->info.bt_key_count == 0) {
		/* no snapshots left: drop the tree, as on a volume that never had one */
		ret = apfs_bt_free_all(bt);
		om->om_snapshot_tree_oid = 0;
	} else {
		ret = apfs_bt_flush(bt);
		om->om_snapshot_tree_oid = LE64(bt->root);
		apfs_bt_close(bt);
	}
	vol->omap.dirty = 1;
	return ret;
}

static int new_extref_tree(apfs_vol_t* vol) {
	apfs_bt_init(&vol->extref, vol->nx, vol, NULL, OBJ_PHYSICAL, OBJECT_TYPE_BLOCKREFTREE, 0, apfs_cmp_fs);
	return apfs_bt_create(&vol->extref, BTREE_SEQUENTIAL_INSERT | BTREE_PHYSICAL | BTREE_KV_NONALIGNED, 0, 0);
}

int apfs_snapshot_create(apfs_vol_t* vol, const char* name) {
	apfs_nx_t* nx = vol->nx;
	apfs_superblock_t* copy;
	omap_phys_t* om;
	apfs_btree_t ost;
	uint8_t key[8], val[sizeof(omap_snapshot_t)], fixed[48];
	paddr_t sblock;
	oid_t extref;
	xid_t xid;
	uint64_t now = apfs_time_now();
	int ret;

	ret = check_name(name);
	if(ret < 0)
		return ret;
	if(vol->sealed)
		return APFS_EROFS;
	ret = find_snapshot(vol, name, &xid);
	if(ret == APFS_OK)
		return APFS_EEXIST;
	if(ret != APFS_ENOENT)
		return ret;
	ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;
	xid = nx->txn;

	if(!vol->snapmeta.loaded) {
		apfs_bt_init(&vol->snapmeta, nx, vol, NULL, OBJ_PHYSICAL, OBJECT_TYPE_SNAPMETATREE, 0, apfs_cmp_fs);
		ret = apfs_bt_create(&vol->snapmeta, BTREE_SEQUENTIAL_INSERT | BTREE_PHYSICAL | BTREE_KV_NONALIGNED, 0, 0);
		if(ret < 0)
			return ret;
	}

	/* the superblock as of this transaction; snapshot superblocks are
	 * physical and don't count towards the volume's allocations */
	ret = apfs_sm_alloc_block(nx, NULL, &sblock);
	if(ret < 0)
		return ret;
	copy = (apfs_superblock_t*)apfs_malloc(nx->bs);
	memcpy(copy, vol->raw, nx->bs);
	copy->apfs_o.o_oid = LE64(sblock);
	copy->apfs_o.o_xid = LE64(xid);
	copy->apfs_o.o_type = LE32(OBJ_PHYSICAL | OBJECT_TYPE_FS);
	copy->apfs_o.o_subtype = 0;
	copy->apfs_omap_oid = 0;              /* shared with the live volume */
	copy->apfs_extentref_tree_oid = 0;    /* given by the snapshot metadata */
	copy->apfs_snap_meta_tree_oid = 0;
	ret = apfs_write_obj(nx, sblock, copy);
	free(copy);
	if(ret < 0)
		return ret;

	/* the snapshot takes the extent reference tree; the volume starts a new one */
	if(!vol->extref.loaded) {
		ret = new_extref_tree(vol);
		if(ret < 0)
			return ret;
	}
	ret = apfs_bt_flush(&vol->extref);
	if(ret < 0)
		return ret;
	extref = vol->extref.root;
	apfs_bt_close(&vol->extref);
	ret = new_extref_tree(vol);
	if(ret < 0)
		return ret;

	memset(fixed, 0, sizeof(fixed));
	apfs_put64(fixed, extref);
	apfs_put64(fixed + 8, sblock);
	apfs_put64(fixed + 16, now);
	apfs_put64(fixed + 24, now);
	apfs_put64(fixed + 32, apfs_vol_next_id(vol));
	apfs_put32(fixed + 40, OBJ_PHYSICAL | OBJECT_TYPE_BTREE);
	ret = put_metadata(vol, xid, fixed, name);
	if(ret == APFS_OK) {
		uint8_t nkey[10 + SNAP_NAME_MAX + 1], nval[8];
		apfs_put64(nval, xid);
		ret = apfs_bt_insert(&vol->snapmeta, nkey, name_key(nkey, name), nval, sizeof(nval));
	}
	if(ret < 0)
		return ret;

	/* from now on the object map keeps whatever this transaction can see */
	ret = open_omap_snap_tree(vol, &ost);
	if(ret < 0)
		return ret;
	apfs_put64(key, xid);
	memset(val, 0, sizeof(val));
	ret = apfs_bt_insert(&ost, key, sizeof(key), val, sizeof(val));
	if(ret < 0) {
		apfs_bt_close(&ost);
		return ret;
	}
	ret = close_omap_snap_tree(vol, &ost);
	if(ret < 0)
		return ret;
	om = APFS_OMAP_PHYS(&vol->omap);
	om->om_snap_count = LE32(LE32(om->om_snap_count) + 1);
	om->om_most_recent_snap = LE64(xid);

	vol->sb->apfs_num_snapshots = LE64(LE64(vol->sb->apfs_num_snapshots) + 1);
	vol->dirty = 1;

	/* later changes must not land in the snapshot's transaction */
	return apfs_nx_commit(nx);
}

int apfs_snapshot_rename(apfs_vol_t* vol, const char* oldname, const char* newname) {
	uint8_t key[10 + SNAP_NAME_MAX + 1], val[8], fixed[48];
	xid_t xid, other;
	int ret;

	ret = check_name(newname);
	if(ret < 0)
		return ret;
	ret = find_snapshot(vol, oldname, &xid);
	if(ret < 0)
		return ret;
	if(strcmp(oldname, newname) == 0)
		return APFS_OK;
	ret = find_snapshot(vol, newname, &other);
	if(ret == APFS_OK)
		return APFS_EEXIST;
	if(ret != APFS_ENOENT)
		return ret;
	ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;

	ret = apfs_bt_delete(&vol->snapmeta, key, name_key(key, oldname));
	if(ret < 0)
		return ret;
	apfs_put64(val, xid);
	ret = apfs_bt_insert(&vol->snapmeta, key, name_key(key, newname), val, sizeof(val));
	if(ret < 0)
		return ret;
	ret = get_metadata(vol, xid, fixed);
	if(ret < 0)
		return ret;
	apfs_put64(fixed + 24, apfs_time_now());
	return put_metadata(vol, xid, fixed, newname);
}

/*
 * Object map: an entry with a transaction in (prev, xid] could only be seen
 * by the deleted snapshot, unless the next snapshot (or the live volume, when
 * next is 0) still sees it because nothing replaced it before then.
 * Placeholders for deleted objects go away once there is nothing older left
 * for them to hide.
 */
static int release_omap_entries(apfs_vol_t* vol, xid_t prev, xid_t xid, xid_t next) {
	apfs_omap_rec_t* r;
	uint8_t* drop;
	size_t n, i, j, k, m;
	int ret;

	ret = apfs_omap_list(&vol->omap, &r, &n);
	if(ret < 0)
		return ret;
	drop = (uint8_t*)apfs_calloc(n ? n : 1, 1);
	for(i = 0; i < n; i = j) {
		for(j = i; j < n && r[j].oid == r[i].oid; j++)
			;
		for(k = i; k < j; k++) {
			if(r[k].xid <= prev || r[k].xid > xid)
				continue;
			if(k + 1 == j || (next && r[k + 1].xid > next))
				continue;
			drop[k] = 1;
		}
		for(k = i; k < j; k++) {
			if(drop[k] || !(r[k].flags & OMAP_VAL_DELETED))
				continue;
			for(m = i; m < k && drop[m]; m++)
				;
			if(m == k)
				drop[k] = 1;
		}
	}
	for(k = 0; k < n && ret == APFS_OK; k++) {
		if(!drop[k])
			continue;
		ret = apfs_omap_remove(&vol->omap, r[k].oid, r[k].xid);
		if(ret == APFS_OK && !(r[k].flags & OMAP_VAL_DELETED) && r[k].paddr) {
			uint64_t blocks = (r[k].size + vol->nx->bs - 1) / vol->nx->bs;
			ret = apfs_sm_free(vol->nx, vol, r[k].paddr, blocks ? blocks : 1);
		}
	}
	free(drop);
	free(r);
	return ret;
}

/* Physical extent records */

typedef struct {
	paddr_t start;
	uint64_t len;
	uint8_t kind;
	uint64_t owner;
	int32_t refcnt;
} pext_t;

static void pext_get(const apfs_kv_t* kv, pext_t* p) {
	p->start = apfs_key_id(kv->k);
	p->len = apfs_get64(kv->v) & PEXT_LEN_MASK;
	p->kind = (uint8_t)(apfs_get64(kv->v) >> PEXT_KIND_SHIFT);
	p->owner = apfs_get64(kv->v + 8);
	p->refcnt = (int32_t)apfs_get32(kv->v + 16);
}

static int pext_put(apfs_btree_t* bt, const pext_t* p) {
	uint8_t key[8], val[20];
	apfs_key_hdr(key, p->start, APFS_TYPE_EXTENT);
	apfs_put64(val, (p->len & PEXT_LEN_MASK) | ((uint64_t)p->kind << PEXT_KIND_SHIFT));
	apfs_put64(val + 8, p->owner);
	apfs_put32(val + 16, (uint32_t)p->refcnt);
	return apfs_bt_insert(bt, key, sizeof(key), val, sizeof(val));
}

static int pext_del(apfs_btree_t* bt, paddr_t start) {
	uint8_t key[8];
	apfs_key_hdr(key, start, APFS_TYPE_EXTENT);
	return apfs_bt_delete(bt, key, sizeof(key));
}

static int pext_valid(const apfs_kv_t* kv) {
	return kv->klen == 8 && kv->vlen >= 20 && apfs_key_type(kv->k) == APFS_TYPE_EXTENT;
}

/* Finds the record covering bno (*covers = 1), or else the first one after it */
static int pext_find(apfs_btree_t* bt, paddr_t bno, pext_t* p, int* covers) {
	uint8_t key[8];
	apfs_btiter_t it;
	apfs_kv_t kv;
	int ret;

	apfs_key_hdr(key, bno, APFS_TYPE_EXTENT);
	ret = apfs_bt_lookup_le(bt, key, sizeof(key), &kv);
	if(ret == APFS_OK && pext_valid(&kv)) {
		pext_get(&kv, p);
		if(p->start <= bno && bno < p->start + p->len) {
			*covers = 1;
			return APFS_OK;
		}
	} else if(ret < 0 && ret != APFS_ENOENT) {
		return ret;
	}
	*covers = 0;
	ret = apfs_bt_iter_seek(&it, bt, key, sizeof(key));
	while(ret == APFS_OK && it.valid) {
		apfs_bt_iter_get(&it, &kv);
		if(pext_valid(&kv)) {
			pext_get(&kv, p);
			if(p->start > bno)
				return APFS_OK;
		}
		ret = apfs_bt_iter_next(&it);
	}
	return ret < 0 ? ret : APFS_ENOENT;
}

static int read_pexts(apfs_btree_t* bt, pext_t** out, size_t* count) {
	pext_t* list = NULL;
	size_t n = 0, cap = 0;
	apfs_btiter_t it;
	apfs_kv_t kv;
	int ret;

	ret = apfs_bt_iter_seek(&it, bt, NULL, 0);
	while(ret == APFS_OK && it.valid) {
		apfs_bt_iter_get(&it, &kv);
		if(pext_valid(&kv)) {
			if(n == cap) {
				cap = cap ? cap * 2 : 64;
				list = (pext_t*)apfs_realloc(list, cap * sizeof(pext_t));
			}
			pext_get(&kv, &list[n++]);
		}
		ret = apfs_bt_iter_next(&it);
	}
	if(ret < 0) {
		free(list);
		return ret;
	}
	*out = list;
	*count = n;
	return APFS_OK;
}

/*
 * Moves the extent records of a deleted snapshot into the next tree. Its own
 * extents combine with the reference updates recorded after it, and those
 * whose count drops to zero are freed; its updates to older snapshots'
 * extents add to the next tree's.
 */
static int merge_extrefs(apfs_vol_t* vol, apfs_btree_t* from, apfs_btree_t* to) {
	pext_t* a;
	size_t n, i;
	int ret;

	ret = read_pexts(from, &a, &n);
	if(ret < 0)
		return ret;
	for(i = 0; i < n && ret == APFS_OK; i++) {
		paddr_t pos = a[i].start, end = a[i].start + a[i].len;
		while(pos < end && ret == APFS_OK) {
			pext_t t, m;
			paddr_t sub_end;
			int covers = 0;

			ret = pext_find(to, pos, &t, &covers);
			if(ret == APFS_ENOENT) {
				ret = APFS_OK;
				t.start = end;
			} else if(ret < 0) {
				break;
			}
			if(!covers) {
				m = a[i];
				m.start = pos;
				m.len = (t.start < end ? t.start : end) - pos;
				ret = pext_put(to, &m);
				pos += m.len;
				continue;
			}

			sub_end = t.start + t.len < end ? t.start + t.len : end;
			ret = pext_del(to, t.start);
			if(ret == APFS_OK && t.start < pos) {
				m = t;
				m.len = pos - t.start;
				ret = pext_put(to, &m);
			}
			if(ret == APFS_OK && t.start + t.len > sub_end) {
				m = t;
				m.start = sub_end;
				m.len = t.start + t.len - sub_end;
				ret = pext_put(to, &m);
			}
			if(ret < 0)
				break;

			m.start = pos;
			m.len = sub_end - pos;
			m.refcnt = a[i].refcnt + t.refcnt;
			if(a[i].kind == APFS_KIND_NEW || t.kind == APFS_KIND_NEW) {
				m.kind = APFS_KIND_NEW;
				m.owner = a[i].kind == APFS_KIND_NEW ? a[i].owner : t.owner;
			} else {
				m.kind = t.kind;
				m.owner = t.owner;
			}
			if(m.refcnt != 0)
				ret = pext_put(to, &m);
			else if(m.kind == APFS_KIND_NEW)
				ret = apfs_sm_free(vol->nx, vol, m.start, m.len);
			pos = sub_end;
		}
	}
	free(a);
	return ret;
}

int apfs_snapshot_delete(apfs_vol_t* vol, const char* name) {
	apfs_nx_t* nx = vol->nx;
	apfs_snapshot_t* snaps;
	apfs_btree_t from, next_tree, ost;
	omap_phys_t* om;
	uint8_t key[10 + SNAP_NAME_MAX + 1];
	size_t count, i;
	xid_t xid, prev, next;
	int ret;

	ret = find_snapshot(vol, name, &xid);
	if(ret < 0)
		return ret;
	ret = apfs_snapshot_list(vol, &snaps, &count);
	if(ret < 0)
		return ret;
	for(i = 0; i < count && snaps[i].xid != xid; i++)
		;
	if(i == count) {
		apfs_snapshots_free(snaps, count);
		return APFS_ECORRUPT;
	}
	prev = i > 0 ? snaps[i - 1].xid : 0;
	next = i + 1 < count ? snaps[i + 1].xid : 0;

	ret = apfs_vol_mark_dirty(vol);
	if(ret == APFS_OK)
		ret = release_omap_entries(vol, prev, xid, next);

	/* extents */
	if(ret == APFS_OK && snaps[i].extentref_tree_oid) {
		apfs_bt_init(&from, nx, vol, NULL, OBJ_PHYSICAL, OBJECT_TYPE_BLOCKREFTREE, snaps[i].extentref_tree_oid, apfs_cmp_fs);
		ret = apfs_bt_load(&from);
		if(ret == APFS_OK && next) {
			uint8_t fixed[48];
			apfs_bt_init(&next_tree, nx, vol, NULL, OBJ_PHYSICAL, OBJECT_TYPE_BLOCKREFTREE,
					snaps[i + 1].extentref_tree_oid, apfs_cmp_fs);
			ret = apfs_bt_load(&next_tree);
			if(ret == APFS_OK)
				ret = merge_extrefs(vol, &from, &next_tree);
			if(ret == APFS_OK)
				ret = apfs_bt_flush(&next_tree);
			if(ret == APFS_OK)
				ret = get_metadata(vol, next, fixed);
			if(ret == APFS_OK && apfs_get64(fixed) != next_tree.root) {
				apfs_put64(fixed, next_tree.root);
				ret = put_metadata(vol, next, fixed, snaps[i + 1].name);
			}
			apfs_bt_close(&next_tree);
		} else if(ret == APFS_OK) {
			if(!vol->extref.loaded)
				ret = new_extref_tree(vol);
			if(ret == APFS_OK)
				ret = merge_extrefs(vol, &from, &vol->extref);
		}
		if(ret == APFS_OK)
			ret = apfs_bt_free_all(&from);
		else
			apfs_bt_close(&from);
	}

	if(ret == APFS_OK)
		ret = apfs_sm_free(nx, NULL, snaps[i].sblock_oid, 1);
	if(ret == APFS_OK) {
		uint8_t mkey[8];
		apfs_key_hdr(mkey, xid, APFS_TYPE_SNAP_METADATA);
		ret = apfs_bt_delete(&vol->snapmeta, mkey, sizeof(mkey));
	}
	if(ret == APFS_OK)
		ret = apfs_bt_delete(&vol->snapmeta, key, name_key(key, name));

	if(ret == APFS_OK)
		ret = open_omap_snap_tree(vol, &ost);
	if(ret == APFS_OK) {
		uint8_t okey[8];
		apfs_put64(okey, xid);
		ret = apfs_bt_delete(&ost, okey, sizeof(okey));
		if(ret == APFS_OK || ret == APFS_ENOENT)
			ret = close_omap_snap_tree(vol, &ost);
		else
			apfs_bt_close(&ost);
	}
	if(ret == APFS_OK) {
		om = APFS_OMAP_PHYS(&vol->omap);
		if(LE32(om->om_snap_count))
			om->om_snap_count = LE32(LE32(om->om_snap_count) - 1);
		if(!next)
			om->om_most_recent_snap = LE64(prev);
		if(LE64(vol->sb->apfs_num_snapshots))
			vol->sb->apfs_num_snapshots = LE64(LE64(vol->sb->apfs_num_snapshots) - 1);
		vol->dirty = 1;
	}
	apfs_snapshots_free(snaps, count);
	return ret;
}
