/*
 * verify.c
 * apfs-tool
 *
 * Consistency check of a container, for platforms without fsck_apfs and for
 * the tests: every block reachable from the checkpoint is used once and
 * agrees with the allocation bitmap, B-trees are ordered and their counts
 * right, every object map entry is used, each volume owns as many blocks as
 * it claims, and the file-system records agree with one another and with the
 * volume's counters. The rules follow apfsck and what fsck_apfs accepts.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apfs/apfs.h"

#define MAX_REPORTS 100

typedef struct {
	oid_t oid;
	xid_t xid;
} vobj_t;

typedef struct {
	apfs_nx_t* nx;
	int verbose;
	uint64_t problems;
	uint64_t nblocks;
	uint8_t* used;                /* blocks reachable from the checkpoint */
	uint8_t* pending;             /* blocks waiting in a free queue */
	uint64_t charged;             /* blocks owned by the volume being checked */
	char where[320];
	vobj_t* seen;                 /* virtual objects reached in the volume */
	size_t nseen;
	size_t capseen;
} vctx_t;

static void problem(vctx_t* c, const char* fmt, ...) {
	char msg[512];
	va_list ap;

	if(++c->problems > MAX_REPORTS) {
		if(c->problems == MAX_REPORTS + 1)
			APFS_ERR("too many problems, the rest are not listed");
		return;
	}
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if(c->where[0])
		APFS_ERR("%s: %s", c->where, msg);
	else
		APFS_ERR("%s", msg);
}

static void progress(vctx_t* c, const char* fmt, ...) {
	va_list ap;
	if(!c->verbose)
		return;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

static void* grow(void* p, size_t* cap, size_t n, size_t size) {
	if(n < *cap)
		return p;
	*cap = *cap ? *cap * 2 : 64;
	return apfs_realloc(p, *cap * size);
}

static int bit_get(const uint8_t* bm, uint64_t i) {
	return (bm[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(uint8_t* bm, uint64_t i) {
	bm[i >> 3] |= (uint8_t)(1 << (i & 7));
}

/* Records blocks as used; `charge` counts them for the current volume */
static void mark(vctx_t* c, paddr_t start, uint64_t count, const char* what, int charge) {
	uint64_t i;

	if(start >= c->nblocks || count > c->nblocks - start) {
		problem(c, "%s at block 0x%llx+%llu lies outside the container", what, (unsigned long long)start,
			(unsigned long long)count);
		return;
	}
	for(i = 0; i < count; i++) {
		if(bit_get(c->used, start + i))
			problem(c, "block 0x%llx (%s) is used twice", (unsigned long long)(start + i), what);
		bit_set(c->used, start + i);
	}
	if(charge)
		c->charged += count;
}

static void see(vctx_t* c, oid_t oid, xid_t xid) {
	c->seen = (vobj_t*)grow(c->seen, &c->capseen, c->nseen, sizeof(vobj_t));
	c->seen[c->nseen].oid = oid;
	c->seen[c->nseen].xid = xid;
	c->nseen++;
}

static int vobj_cmp(const void* a, const void* b) {
	const vobj_t* x = (const vobj_t*)a;
	const vobj_t* y = (const vobj_t*)b;
	if(x->oid != y->oid)
		return x->oid < y->oid ? -1 : 1;
	if(x->xid != y->xid)
		return x->xid < y->xid ? -1 : 1;
	return 0;
}

/* Checks that the block holds the expected object */
static void check_object(vctx_t* c, paddr_t paddr, oid_t oid, xid_t xid, const char* what) {
	uint8_t* buf = (uint8_t*)apfs_malloc(c->nx->bs);
	const obj_phys_t* o = (const obj_phys_t*)buf;

	if(apfs_read_blocks(c->nx, paddr, 1, buf) < 0)
		problem(c, "cannot read %s at block 0x%llx", what, (unsigned long long)paddr);
	else if(!apfs_obj_verify(buf, c->nx->bs))
		problem(c, "%s at block 0x%llx has a bad checksum", what, (unsigned long long)paddr);
	else if(LE64(o->o_oid) != oid)
		problem(c, "%s at block 0x%llx has object id 0x%llx, expected 0x%llx", what, (unsigned long long)paddr,
			(unsigned long long)LE64(o->o_oid), (unsigned long long)oid);
	else if(xid && LE64(o->o_xid) != xid)
		problem(c, "%s at block 0x%llx has transaction %llu, expected %llu", what, (unsigned long long)paddr,
			(unsigned long long)LE64(o->o_xid), (unsigned long long)xid);
	else if(LE64(o->o_xid) > c->nx->xid)
		problem(c, "%s at block 0x%llx comes from a future transaction", what, (unsigned long long)paddr);
	free(buf);
}

/* B-trees */

typedef struct {
	vctx_t* c;
	apfs_btree_t* bt;
	const char* name;
	int charge;                   /* account physical nodes to the volume */
	int ordered;                  /* the comparator knows the key format */
	uint64_t keys;
	uint64_t nodes;
	uint8_t* last;
	uint16_t last_len;
} walk_t;

static int walk_node(walk_t* w, oid_t oid, int level, const apfs_btrec_t* sep, int depth) {
	vctx_t* c = w->c;
	apfs_btree_t* bt = w->bt;
	apfs_btnode_t* n;
	uint32_t i;
	int ret;

	if(depth >= APFS_BT_MAX_DEPTH) {
		problem(c, "the %s is too deep", w->name);
		return APFS_ECORRUPT;
	}
	ret = apfs_bt_get_node(bt, oid, &n);
	if(ret < 0) {
		problem(c, "cannot read node 0x%llx of the %s", (unsigned long long)oid, w->name);
		return ret;
	}
	w->nodes++;
	if(bt->storage == OBJ_PHYSICAL)
		mark(c, n->paddr, 1, w->name, w->charge);
	else if(bt->storage == OBJ_VIRTUAL)
		see(c, n->oid, n->xid);
	if(level >= 0 && n->level != level)
		problem(c, "node 0x%llx of the %s has level %u instead of %d", (unsigned long long)oid, w->name, n->level, level);
	if(!n->nrec && depth > 0)
		problem(c, "node 0x%llx of the %s is empty", (unsigned long long)oid, w->name);

	if(w->ordered && n->nrec) {
		if(sep && bt->cmp(bt, sep->k, sep->klen, n->rec[0].k, n->rec[0].klen) != 0)
			problem(c, "node 0x%llx of the %s does not start with its index key", (unsigned long long)oid, w->name);
		for(i = 1; i < n->nrec; i++) {
			if(bt->cmp(bt, n->rec[i - 1].k, n->rec[i - 1].klen, n->rec[i].k, n->rec[i].klen) >= 0) {
				problem(c, "keys are out of order in node 0x%llx of the %s", (unsigned long long)oid, w->name);
				break;
			}
		}
	}

	if(n->level == 0) {
		w->keys += n->nrec;
		if(w->ordered && n->nrec) {
			const apfs_btrec_t* r = &n->rec[n->nrec - 1];
			if(w->last && bt->cmp(bt, w->last, w->last_len, n->rec[0].k, n->rec[0].klen) >= 0)
				problem(c, "leaves of the %s are out of order", w->name);
			free(w->last);
			w->last = (uint8_t*)apfs_malloc(r->klen ? r->klen : 1);
			memcpy(w->last, r->k, r->klen);
			w->last_len = r->klen;
		}
		return APFS_OK;
	}

	for(i = 0; i < n->nrec; i++) {
		apfs_btrec_t sepi = n->rec[i];
		oid_t child;
		ret = apfs_bt_child_oid(bt, &sepi, &child);
		if(ret < 0) {
			problem(c, "bad index record in node 0x%llx of the %s", (unsigned long long)oid, w->name);
			return ret;
		}
		ret = walk_node(w, child, n->level - 1, &sepi, depth + 1);
		if(ret < 0)
			return ret;
	}
	return APFS_OK;
}

/* Walks a whole tree: physical nodes are marked as used, virtual ones seen */
static int check_tree(vctx_t* c, apfs_btree_t* bt, const char* name, int charge, int ordered) {
	walk_t w;
	int ret;

	memset(&w, 0, sizeof(w));
	w.c = c;
	w.bt = bt;
	w.name = name;
	w.charge = charge;
	w.ordered = ordered;
	ret = apfs_bt_load(bt);
	if(ret < 0) {
		problem(c, "cannot load the %s", name);
		return ret;
	}
	ret = walk_node(&w, bt->root, -1, NULL, 0);
	free(w.last);
	if(ret < 0)
		return ret;
	if(w.keys != bt->info.bt_key_count)
		problem(c, "the %s has %llu records, but its footer says %llu", name, (unsigned long long)w.keys,
			(unsigned long long)bt->info.bt_key_count);
	if(w.nodes != bt->info.bt_node_count)
		problem(c, "the %s has %llu nodes, but its footer says %llu", name, (unsigned long long)w.nodes,
			(unsigned long long)bt->info.bt_node_count);
	return APFS_OK;
}

static int check_phys_tree(vctx_t* c, apfs_vol_t* vol, oid_t root, uint32_t subtype, apfs_keycmp_fn cmp,
		const char* name, int ordered) {
	apfs_btree_t bt;
	int ret;
	if(!root)
		return APFS_OK;
	apfs_bt_init(&bt, c->nx, vol, NULL, OBJ_PHYSICAL, subtype, root, cmp);
	ret = check_tree(c, &bt, name, vol != NULL, ordered);
	apfs_bt_close(&bt);
	return ret;
}

/* Extent reference trees */

typedef struct {
	paddr_t start;
	uint64_t len;
	int32_t refcnt;
	uint64_t refs;                /* blocks referenced by file extents */
} vpext_t;

typedef struct {
	vpext_t* list;
	size_t count;
	size_t cap;
	int updates;                  /* reference count updates for snapshots */
} vpexts_t;

/* Marks the extents an extent reference tree owns and remembers them */
static int check_extref(vctx_t* c, apfs_btree_t* bt, const char* name, vpexts_t* out) {
	apfs_btiter_t it;
	apfs_kv_t kv;
	int ret;

	ret = check_tree(c, bt, name, 1, 1);
	if(ret < 0)
		return ret;
	ret = apfs_bt_iter_seek(&it, bt, NULL, 0);
	while(ret == APFS_OK && it.valid) {
		apfs_bt_iter_get(&it, &kv);
		if(kv.klen != 8 || kv.vlen != 20 || apfs_key_type(kv.k) != APFS_TYPE_EXTENT) {
			problem(c, "bad record in the %s", name);
		} else {
			uint8_t kind = (uint8_t)(apfs_get64(kv.v) >> PEXT_KIND_SHIFT);
			paddr_t start = apfs_key_id(kv.k);
			uint64_t len = apfs_get64(kv.v) & PEXT_LEN_MASK;
			if(!len)
				problem(c, "empty extent 0x%llx in the %s", (unsigned long long)start, name);
			if(kind == APFS_KIND_NEW) {
				mark(c, start, len, "file data", 1);
				out->list = (vpext_t*)grow(out->list, &out->cap, out->count, sizeof(vpext_t));
				out->list[out->count].start = start;
				out->list[out->count].len = len;
				out->list[out->count].refcnt = (int32_t)apfs_get32(kv.v + 16);
				out->list[out->count].refs = 0;
				if(out->list[out->count].refcnt <= 0)
					problem(c, "extent 0x%llx in the %s has reference count %d", (unsigned long long)start, name,
						out->list[out->count].refcnt);
				out->count++;
			} else if(kind == APFS_KIND_UPDATE) {
				out->updates++;
			} else {
				problem(c, "extent 0x%llx in the %s has unknown kind %u", (unsigned long long)start, name, kind);
			}
		}
		ret = apfs_bt_iter_next(&it);
	}
	return ret;
}

static int pext_cmp(const void* a, const void* b) {
	const vpext_t* x = (const vpext_t*)a;
	const vpext_t* y = (const vpext_t*)b;
	return x->start < y->start ? -1 : (x->start > y->start ? 1 : 0);
}

/* File-system records */

typedef struct {
	uint64_t ino;
	uint64_t parent;
	uint64_t private_id;
	uint64_t size;
	uint64_t alloced;
	uint64_t sparse;
	uint32_t nlink;
	uint32_t links;
	uint32_t children;
	uint16_t mode;
	uint8_t has_dstream;
	uint8_t orphan;
} vinode_t;

typedef struct {
	uint64_t id;
	uint32_t refcnt;
	uint8_t has_rec;              /* a dstream id record exists */
	uint8_t xattr;
	uint8_t orphan;
	uint32_t refs;
	uint64_t size;
	uint64_t alloced;
	uint64_t start;               /* logical address of the first extent */
	uint64_t bytes;
	uint64_t sparse;
} vstream_t;

typedef struct {
	uint64_t parent;
	uint64_t ino;
	uint16_t dtype;
} vdrec_t;

typedef struct {
	paddr_t phys;
	uint64_t blocks;
} vref_t;

typedef struct {
	vinode_t* inodes;
	size_t ninodes, capinodes;
	vstream_t* streams;
	size_t nstreams, capstreams;
	vdrec_t* drecs;
	size_t ndrecs, capdrecs;
	vref_t* refs;
	size_t nrefs, caprefs;
	uint64_t max_id;
} vfs_t;

static vinode_t* find_inode(vfs_t* f, uint64_t ino) {
	size_t lo = 0, hi = f->ninodes;
	while(lo < hi) {
		size_t mid = (lo + hi) / 2;
		if(f->inodes[mid].ino == ino)
			return &f->inodes[mid];
		if(f->inodes[mid].ino < ino)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

/* External xattrs can introduce stream IDs far ahead of the catalog walk.
 * Insert at the binary-search position rather than re-sorting the full array
 * for each such reference. */
static vstream_t* stream(vfs_t* f, uint64_t id, int create) {
	size_t lo = 0, hi = f->nstreams;
	if(f->nstreams && f->streams[f->nstreams - 1].id == id)
		return &f->streams[f->nstreams - 1];
	while(lo < hi) {
		size_t mid = (lo + hi) / 2;
		if(f->streams[mid].id == id)
			return &f->streams[mid];
		if(f->streams[mid].id < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	if(!create)
		return NULL;
	f->streams = (vstream_t*)grow(f->streams, &f->capstreams, f->nstreams, sizeof(vstream_t));
	memmove(f->streams + lo + 1, f->streams + lo, (f->nstreams - lo) * sizeof(vstream_t));
	memset(&f->streams[lo], 0, sizeof(vstream_t));
	f->streams[lo].id = id;
	f->nstreams++;
	return &f->streams[lo];
}

static void add_extent(vctx_t* c, vfs_t* f, uint64_t id, uint64_t logical, uint64_t len_flags, paddr_t phys) {
	vstream_t* s = stream(f, id, 1);
	uint64_t len = len_flags & J_FILE_EXTENT_LEN_MASK;

	if(len % c->nx->bs)
		problem(c, "extent of stream 0x%llx at 0x%llx is not a whole number of blocks", (unsigned long long)id,
			(unsigned long long)logical);
	if(s->bytes == 0 && logical != 0)
		s->start = logical;
	if(s->start + s->bytes != logical)
		problem(c, "extents of stream 0x%llx are not consecutive at 0x%llx", (unsigned long long)id,
			(unsigned long long)logical);
	s->bytes += len;
	if(phys) {
		f->refs = (vref_t*)grow(f->refs, &f->caprefs, f->nrefs, sizeof(vref_t));
		f->refs[f->nrefs].phys = phys;
		f->refs[f->nrefs].blocks = (len + c->nx->bs - 1) / c->nx->bs;
		f->nrefs++;
	} else {
		s->sparse += len;
	}
}

static void parse_drec(vctx_t* c, apfs_vol_t* vol, vfs_t* f, const apfs_kv_t* kv) {
	uint64_t parent = apfs_key_id(kv->k);
	const char* name;
	uint32_t nlen, hash = 0;

	if(vol->hashed_names) {
		if(kv->klen < 13) {
			problem(c, "directory record in 0x%llx is too short", (unsigned long long)parent);
			return;
		}
		nlen = apfs_get32(kv->k + 8) & J_DREC_LEN_MASK;
		hash = apfs_get32(kv->k + 8) & J_DREC_HASH_MASK;
		name = (const char*)kv->k + 12;
		if(12 + nlen != kv->klen) {
			problem(c, "directory record in 0x%llx has a bad name length", (unsigned long long)parent);
			return;
		}
	} else {
		if(kv->klen < 11) {
			problem(c, "directory record in 0x%llx is too short", (unsigned long long)parent);
			return;
		}
		nlen = apfs_get16(kv->k + 8);
		name = (const char*)kv->k + 10;
		if(10 + nlen != kv->klen) {
			problem(c, "directory record in 0x%llx has a bad name length", (unsigned long long)parent);
			return;
		}
	}
	if(!nlen || name[nlen - 1] != 0 || memchr(name, 0, nlen - 1) != NULL) {
		problem(c, "directory record in 0x%llx has a bad name", (unsigned long long)parent);
		return;
	}
	if(vol->hashed_names && apfs_name_hash(name, nlen - 1, vol->case_fold) != hash)
		problem(c, "directory record \"%s\" in 0x%llx has the wrong hash", name, (unsigned long long)parent);
	if(kv->vlen < 18) {
		problem(c, "directory record \"%s\" in 0x%llx is too short", name, (unsigned long long)parent);
		return;
	}
	f->drecs = (vdrec_t*)grow(f->drecs, &f->capdrecs, f->ndrecs, sizeof(vdrec_t));
	f->drecs[f->ndrecs].parent = parent;
	f->drecs[f->ndrecs].ino = apfs_get64(kv->v);
	f->drecs[f->ndrecs].dtype = apfs_get16(kv->v + 16) & DREC_TYPE_MASK;
	f->ndrecs++;
}

static void parse_inode(vctx_t* c, vfs_t* f, const apfs_kv_t* kv) {
	apfs_inode_t inode;
	vinode_t* vi;
	apfs_dstream_t ds;
	const uint8_t* xf;
	uint16_t xsize;
	uint64_t ino = apfs_key_id(kv->k);

	if(apfs_inode_parse(ino, kv->v, kv->vlen, &inode) < 0) {
		problem(c, "inode 0x%llx cannot be parsed", (unsigned long long)ino);
		return;
	}
	f->inodes = (vinode_t*)grow(f->inodes, &f->capinodes, f->ninodes, sizeof(vinode_t));
	vi = &f->inodes[f->ninodes++];
	memset(vi, 0, sizeof(*vi));
	vi->ino = ino;
	vi->parent = inode.parent_id;
	vi->private_id = inode.private_id;
	vi->nlink = inode.nlink;
	vi->mode = inode.mode;
	if(apfs_inode_get_dstream(&inode, &ds) == APFS_OK) {
		vi->has_dstream = 1;
		vi->size = ds.size;
		vi->alloced = ds.alloced_size;
	}
	xf = apfs_inode_xf(&inode, INO_EXT_TYPE_SPARSE_BYTES, &xsize);
	if(xf && xsize >= 8)
		vi->sparse = apfs_get64(xf);
	if(inode.private_id > f->max_id)
		f->max_id = inode.private_id;
	apfs_inode_release(&inode);
}

static void parse_xattr(vctx_t* c, vfs_t* f, const apfs_kv_t* kv) {
	uint16_t flags;
	vstream_t* s;
	uint64_t id;

	if(kv->vlen < 4) {
		problem(c, "extended attribute of 0x%llx is too short", (unsigned long long)apfs_key_id(kv->k));
		return;
	}
	flags = apfs_get16(kv->v);
	if(!(flags & XATTR_DATA_STREAM))
		return;
	if(kv->vlen < 4 + 48) {
		problem(c, "extended attribute stream of 0x%llx is too short", (unsigned long long)apfs_key_id(kv->k));
		return;
	}
	id = apfs_get64(kv->v + 4);
	s = stream(f, id, 1);
	s->xattr = 1;
	s->refs++;
	s->size = apfs_get64(kv->v + 12);
	s->alloced = apfs_get64(kv->v + 20);
	if(id > f->max_id)
		f->max_id = id;
}

static void check_links(vctx_t* c, apfs_vol_t* vol, vfs_t* f) {
	apfs_superblock_t* vsb = vol->sb;
	uint64_t files = 0, dirs = 0, links = 0, other = 0;
	size_t i;

	for(i = 0; i < f->ndrecs; i++) {
		vdrec_t* d = &f->drecs[i];
		vinode_t* child = find_inode(f, d->ino);
		if(!child) {
			problem(c, "directory 0x%llx names inode 0x%llx, which does not exist", (unsigned long long)d->parent,
				(unsigned long long)d->ino);
			continue;
		}
		child->links++;
		if((child->mode >> 12) != d->dtype)
			problem(c, "directory record for inode 0x%llx has the wrong type", (unsigned long long)d->ino);
		if(d->parent == PRIV_DIR_INO_NUM)
			child->orphan = 1;
		if(d->parent != ROOT_DIR_PARENT) {
			vinode_t* parent = find_inode(f, d->parent);
			if(!parent)
				problem(c, "inode 0x%llx is named in directory 0x%llx, which does not exist", (unsigned long long)d->ino,
					(unsigned long long)d->parent);
			else if((parent->mode & APFS_S_IFMT) != APFS_S_IFDIR)
				problem(c, "inode 0x%llx is named in 0x%llx, which is not a directory", (unsigned long long)d->ino,
					(unsigned long long)d->parent);
			else
				parent->children++;
		}
	}

	for(i = 0; i < f->ninodes; i++) {
		vinode_t* vi = &f->inodes[i];
		uint16_t type = vi->mode & APFS_S_IFMT;

		if(!vi->links)
			problem(c, "inode 0x%llx has no directory record", (unsigned long long)vi->ino);
		if(type == APFS_S_IFDIR) {
			if(vi->links > 1)
				problem(c, "directory 0x%llx has hard links", (unsigned long long)vi->ino);
			if(vi->nlink != vi->children)
				problem(c, "directory 0x%llx counts %u children but holds %u", (unsigned long long)vi->ino, vi->nlink,
					vi->children);
		} else if(!vi->orphan && vi->nlink != vi->links) {
			problem(c, "inode 0x%llx counts %u links but has %u", (unsigned long long)vi->ino, vi->nlink, vi->links);
		}
		if(vi->ino > f->max_id)
			f->max_id = vi->ino;

		if(vi->has_dstream) {
			vstream_t* s = stream(f, vi->private_id, 1);
			s->refs++;
			s->size = vi->size;
			s->alloced = vi->alloced;
			if(vi->orphan)
				s->orphan = 1;
			else if(s->sparse != vi->sparse)
				problem(c, "inode 0x%llx counts %llu sparse bytes but has %llu", (unsigned long long)vi->ino,
					(unsigned long long)vi->sparse, (unsigned long long)s->sparse);
		}

		if(vi->orphan)
			continue;
		switch(type) {
			case APFS_S_IFREG: files++; break;
			case APFS_S_IFDIR: if(vi->ino >= MIN_USER_INO_NUM) dirs++; break;
			case APFS_S_IFLNK: links++; break;
			case APFS_S_IFSOCK: case APFS_S_IFBLK: case APFS_S_IFCHR: case APFS_S_IFIFO: other++; break;
			default: problem(c, "inode 0x%llx has an invalid mode 0%o", (unsigned long long)vi->ino, vi->mode); break;
		}
	}

	if(LE64(vsb->apfs_num_files) != files)
		problem(c, "the volume counts %llu files but holds %llu", (unsigned long long)LE64(vsb->apfs_num_files),
			(unsigned long long)files);
	if(LE64(vsb->apfs_num_directories) != dirs)
		problem(c, "the volume counts %llu directories but holds %llu", (unsigned long long)LE64(vsb->apfs_num_directories),
			(unsigned long long)dirs);
	if(LE64(vsb->apfs_num_symlinks) != links)
		problem(c, "the volume counts %llu symbolic links but holds %llu", (unsigned long long)LE64(vsb->apfs_num_symlinks),
			(unsigned long long)links);
	if(LE64(vsb->apfs_num_other_fsobjects) != other)
		problem(c, "the volume counts %llu special files but holds %llu",
			(unsigned long long)LE64(vsb->apfs_num_other_fsobjects), (unsigned long long)other);
}

static void check_streams(vctx_t* c, vfs_t* f) {
	size_t i;
	for(i = 0; i < f->nstreams; i++) {
		vstream_t* s = &f->streams[i];
		unsigned long long id = (unsigned long long)s->id;
		if(!s->refs) {
			problem(c, "data stream 0x%llx is not used", id);
			continue;
		}
		if(s->xattr) {
			if(s->has_rec || s->refs != 1)
				problem(c, "attribute stream 0x%llx is shared", id);
		} else {
			if(!s->has_rec)
				problem(c, "data stream 0x%llx has no reference count", id);
			else if(s->refcnt != s->refs)
				problem(c, "data stream 0x%llx counts %u references but has %u", id, s->refcnt, s->refs);
		}
		if(s->orphan)
			continue;
		if(s->start != 0)
			problem(c, "data stream 0x%llx is missing its first extents", id);
		if(s->bytes < s->size)
			problem(c, "data stream 0x%llx is missing extents", id);
		if(s->bytes != s->alloced)
			problem(c, "data stream 0x%llx has %llu bytes of extents but claims %llu", id,
				(unsigned long long)s->bytes, (unsigned long long)s->alloced);
	}
}

static int ref_cmp(const void* a, const void* b) {
	const vref_t* x = (const vref_t*)a;
	const vref_t* y = (const vref_t*)b;
	return x->phys < y->phys ? -1 : (x->phys > y->phys ? 1 : 0);
}

/* File extents must lie in extents some reference tree owns; without
 * snapshots, the reference counts must also match the file extents */
static void check_refs(vctx_t* c, vfs_t* f, vpexts_t* p, int exact) {
	size_t i, j;

	qsort(f->refs, f->nrefs, sizeof(vref_t), ref_cmp);
	qsort(p->list, p->count, sizeof(vpext_t), pext_cmp);
	j = 0;
	for(i = 0; i < f->nrefs; i++) {
		paddr_t start = f->refs[i].phys, end = start + f->refs[i].blocks;
		uint64_t covered = 0;
		size_t k;
		while(j < p->count && p->list[j].start + p->list[j].len <= start)
			j++;
		for(k = j; k < p->count && p->list[k].start < end; k++) {
			paddr_t lo = p->list[k].start > start ? p->list[k].start : start;
			paddr_t hi = p->list[k].start + p->list[k].len < end ? p->list[k].start + p->list[k].len : end;
			covered += hi - lo;
			p->list[k].refs += hi - lo;
		}
		if(covered != f->refs[i].blocks)
			problem(c, "file data at block 0x%llx has no extent reference", (unsigned long long)start);
	}
	if(!exact)
		return;
	for(i = 0; i < p->count; i++)
		if(p->list[i].refs != (uint64_t)p->list[i].refcnt * p->list[i].len)
			problem(c, "extent 0x%llx counts %d references, but file extents use it %llu times",
				(unsigned long long)p->list[i].start, p->list[i].refcnt,
				(unsigned long long)(p->list[i].refs / (p->list[i].len ? p->list[i].len : 1)));
}

static int check_fs(vctx_t* c, apfs_vol_t* vol, vpexts_t* pexts, int nsnaps) {
	vfs_t f;
	apfs_btiter_t it;
	apfs_kv_t kv;
	int ret;

	memset(&f, 0, sizeof(f));
	ret = apfs_bt_iter_seek(&it, &vol->fs, NULL, 0);
	while(ret == APFS_OK && it.valid) {
		uint64_t id;
		apfs_bt_iter_get(&it, &kv);
		if(kv.klen < 8) {
			problem(c, "file-system record with a short key");
			ret = apfs_bt_iter_next(&it);
			continue;
		}
		id = apfs_key_id(kv.k);
		if(id > f.max_id)
			f.max_id = id;
		switch(apfs_key_type(kv.k)) {
			case APFS_TYPE_INODE:
				parse_inode(c, &f, &kv);
				break;
			case APFS_TYPE_DIR_REC:
				parse_drec(c, vol, &f, &kv);
				break;
			case APFS_TYPE_XATTR:
				parse_xattr(c, &f, &kv);
				break;
			case APFS_TYPE_DSTREAM_ID:
				if(kv.vlen >= 4) {
					vstream_t* s = stream(&f, id, 1);
					s->has_rec = 1;
					s->refcnt = apfs_get32(kv.v);
				}
				break;
			case APFS_TYPE_FILE_EXTENT:
				if(kv.klen != 16 || kv.vlen < 24)
					problem(c, "file extent of 0x%llx has a bad size", (unsigned long long)id);
				else
					add_extent(c, &f, id, apfs_get64(kv.k + 8), apfs_get64(kv.v), apfs_get64(kv.v + 8));
				break;
			case APFS_TYPE_SIBLING_LINK:
				if(kv.klen >= 16 && apfs_get64(kv.k + 8) > f.max_id)
					f.max_id = apfs_get64(kv.k + 8);
				break;
			default:
				break;
		}
		ret = apfs_bt_iter_next(&it);
	}
	if(ret == APFS_OK && vol->fext.loaded) {
		ret = apfs_bt_iter_seek(&it, &vol->fext, NULL, 0);
		while(ret == APFS_OK && it.valid) {
			apfs_bt_iter_get(&it, &kv);
			if(kv.klen == 16 && kv.vlen == 16)
				add_extent(c, &f, apfs_get64(kv.k), apfs_get64(kv.k + 8), apfs_get64(kv.v), apfs_get64(kv.v + 8));
			ret = apfs_bt_iter_next(&it);
		}
	}
	if(ret < 0) {
		problem(c, "cannot read the file-system tree");
	} else {
		check_links(c, vol, &f);
		check_streams(c, &f);
		check_refs(c, &f, pexts, nsnaps == 0);
		if(f.max_id >= LE64(vol->sb->apfs_next_obj_id))
			problem(c, "object id 0x%llx is in use but not yet allocated", (unsigned long long)f.max_id);
	}
	free(f.inodes);
	free(f.streams);
	free(f.drecs);
	free(f.refs);
	return ret;
}

int apfs_verify_fs_records(apfs_vol_t* vol, const apfs_kv_t* records, size_t count) {
    vctx_t c;
    vfs_t f;
    size_t i;
    memset(&c, 0, sizeof(c)); memset(&f, 0, sizeof(f)); c.nx = vol->nx;
    for(i = 0; i < count; i++) {
        const apfs_kv_t* kv = &records[i];
        uint64_t id;
        if(kv->klen < 8) { problem(&c, "short native key"); continue; }
        id = apfs_key_id(kv->k);
        if(id > f.max_id) f.max_id = id;
        switch(apfs_key_type(kv->k)) {
            case APFS_TYPE_INODE:
                if(kv->klen != 8) problem(&c, "bad native inode key");
                else parse_inode(&c, &f, kv);
                break;
            case APFS_TYPE_DIR_REC: parse_drec(&c, vol, &f, kv); break;
            case APFS_TYPE_XATTR:
                if(kv->klen < 12 || apfs_get16(kv->k + 8) != kv->klen - 10 ||
                   kv->k[kv->klen - 1] || memchr(kv->k + 10, 0, kv->klen - 11) ||
                   kv->vlen < 4 || apfs_get16(kv->v + 2) != kv->vlen - 4 ||
                   ((apfs_get16(kv->v) & 3) != XATTR_DATA_STREAM &&
                    (apfs_get16(kv->v) & 3) != XATTR_DATA_EMBEDDED))
                    problem(&c, "bad native extended attribute");
                else parse_xattr(&c, &f, kv);
                break;
            case APFS_TYPE_DSTREAM_ID:
                if(kv->klen != 8 || kv->vlen != 4) problem(&c, "bad native stream reference");
                else { vstream_t* s = stream(&f, id, 1); s->has_rec = 1; s->refcnt = apfs_get32(kv->v); }
                break;
            case APFS_TYPE_FILE_EXTENT:
                if(kv->klen != 16 || kv->vlen != 24) problem(&c, "bad native extent");
                else add_extent(&c, &f, id, apfs_get64(kv->k + 8), apfs_get64(kv->v), apfs_get64(kv->v + 8));
                break;
            case APFS_TYPE_SIBLING_LINK:
                if(kv->klen != 16 || kv->vlen < 12 || apfs_get16(kv->v + 8) != kv->vlen - 10 ||
                   kv->v[kv->vlen - 1] || memchr(kv->v + 10, 0, kv->vlen - 11))
                    problem(&c, "bad native sibling record");
                else if(apfs_get64(kv->k + 8) > f.max_id) f.max_id = apfs_get64(kv->k + 8);
                break;
            case APFS_TYPE_SIBLING_MAP:
                if(kv->klen != 8 || kv->vlen != 8) problem(&c, "bad native sibling map");
                break;
            case APFS_TYPE_DIR_STATS:
                if(kv->klen != 8 || kv->vlen != 32) problem(&c, "bad native directory statistics");
                break;
            case APFS_TYPE_FILE_INFO:
                if(kv->klen != 16 || kv->vlen < 3 || kv->vlen != 3U + kv->v[2])
                    problem(&c, "bad native file information");
                break;
            default: problem(&c, "unsupported native record"); break;
        }
    }
    vinode_t* root = find_inode(&f, ROOT_DIR_INO_NUM);
    if(!root || (root->mode & APFS_S_IFMT) != APFS_S_IFDIR)
        problem(&c, "native root directory is missing");
    check_links(&c, vol, &f); check_streams(&c, &f);
    if(f.max_id >= LE64(vol->sb->apfs_next_obj_id)) problem(&c, "native next object id is too low");
    free(f.inodes); free(f.streams); free(f.drecs); free(f.refs);
    return c.problems ? APFS_ECORRUPT : APFS_OK;
}

/* Volumes */

static int is_listed(const oid_t* list, size_t n, oid_t oid) {
	size_t i;
	for(i = 0; i < n; i++)
		if(list[i] == oid)
			return 1;
	return 0;
}

static void check_volume(vctx_t* c, int index) {
	apfs_nx_t* nx = c->nx;
	apfs_vol_t* vol;
	apfs_superblock_t* vsb;
	apfs_omap_rec_t* om = NULL;
	apfs_snapshot_t* snaps = NULL;
	vpexts_t pexts;
	oid_t singles[64];
	size_t nom = 0, nsnaps = 0, nsingles = 0, i;
	char name[APFS_VOLNAME_LEN + 1];
	int ret;

	memset(&pexts, 0, sizeof(pexts));
	c->nseen = 0;
	c->charged = 0;
	snprintf(c->where, sizeof(c->where), "volume %d", index);
	ret = apfs_vol_open(nx, index, &vol);
	if(ret < 0) {
		problem(c, "cannot open the volume: %s", apfs_strerror(ret));
		return;
	}
	vsb = vol->sb;
	memcpy(name, vsb->apfs_volname, APFS_VOLNAME_LEN);
	name[APFS_VOLNAME_LEN] = 0;
	snprintf(c->where, sizeof(c->where), "volume %d (%s)", index, name);
	progress(c, "Checking volume %d (%s)", index, name);

	if(LE32(vsb->apfs_fs_index) != (uint32_t)index)
		problem(c, "the superblock says it is volume %u", LE32(vsb->apfs_fs_index));
	if(!(LE64(vsb->apfs_fs_flags) & APFS_FS_UNENCRYPTED))
		progress(c, "  the volume is encrypted; file contents are not checked");

	/* object map */
	mark(c, vol->omap.paddr, 1, "volume object map", 1);
	check_object(c, vol->omap.paddr, vol->omap.paddr, 0, "volume object map");
	check_tree(c, &vol->omap.tree, "volume object map tree", 1, 1);
	check_phys_tree(c, vol, LE64(APFS_OMAP_PHYS(&vol->omap)->om_snapshot_tree_oid), OBJECT_TYPE_OMAP_SNAPSHOT, apfs_cmp_u64,
		"object map snapshot tree", 1);
	ret = apfs_omap_list(&vol->omap, &om, &nom);
	if(ret < 0)
		problem(c, "cannot read the object map");
	for(i = 0; i < nom; i++) {
		uint64_t blocks = om[i].size > nx->bs ? (om[i].size + nx->bs - 1) / nx->bs : 1;
		if(om[i].flags & OMAP_VAL_DELETED)
			continue;
		mark(c, om[i].paddr, blocks, "virtual object", 1);
		if(!(om[i].flags & OMAP_VAL_NOHEADER) && !(om[i].flags & OMAP_VAL_ENCRYPTED))
			check_object(c, om[i].paddr, om[i].oid, om[i].xid, "virtual object");
	}

	/* trees */
	progress(c, "  file-system tree");
	check_tree(c, &vol->fs, "file-system tree", 1, 1);
	if(vol->extref.loaded)
		check_extref(c, &vol->extref, "extent reference tree", &pexts);
	if(vol->snapmeta.loaded)
		check_tree(c, &vol->snapmeta, "snapshot metadata tree", 1, 1);
	if(vol->fext.loaded)
		check_tree(c, &vol->fext, "file extent tree", 1, 1);
	if(vsb->apfs_doc_id_tree_oid) {
		if((LE32(vsb->apfs_doc_id_tree_type) & OBJ_STORAGETYPE_MASK) == OBJ_PHYSICAL) {
			check_phys_tree(c, vol, LE64(vsb->apfs_doc_id_tree_oid), 0, apfs_cmp_u64, "document id tree", 0);
		} else {
			apfs_btree_t bt;
			apfs_bt_init(&bt, nx, vol, &vol->omap, OBJ_VIRTUAL, 0, LE64(vsb->apfs_doc_id_tree_oid), apfs_cmp_u64);
			check_tree(c, &bt, "document id tree", 1, 0);
			apfs_bt_close(&bt);
		}
	}
	if(vsb->apfs_sec_root_tree_oid) {
		if((LE32(vsb->apfs_sec_root_tree_type) & OBJ_STORAGETYPE_MASK) == OBJ_PHYSICAL) {
			check_phys_tree(c, vol, LE64(vsb->apfs_sec_root_tree_oid), 0, apfs_cmp_u64, "secondary root tree", 0);
		} else {
			apfs_btree_t bt;
			apfs_bt_init(&bt, nx, vol, &vol->omap, OBJ_VIRTUAL, 0, LE64(vsb->apfs_sec_root_tree_oid), apfs_cmp_u64);
			check_tree(c, &bt, "secondary root tree", 1, 0);
			apfs_bt_close(&bt);
		}
	}
	singles[nsingles++] = LE64(vsb->apfs_integrity_meta_oid);
	singles[nsingles++] = LE64(vsb->apfs_er_state_oid);
	singles[nsingles++] = LE64(vsb->apfs_snap_meta_ext_oid);

	/* snapshots */
	if(apfs_snapshot_list(vol, &snaps, &nsnaps) < 0)
		problem(c, "cannot list the snapshots");
	if(LE64(vsb->apfs_num_snapshots) != nsnaps)
		problem(c, "the volume counts %llu snapshots but has %zu", (unsigned long long)LE64(vsb->apfs_num_snapshots), nsnaps);
	if(nsnaps && LE64(APFS_OMAP_PHYS(&vol->omap)->om_most_recent_snap) != snaps[nsnaps - 1].xid)
		problem(c, "the object map's latest snapshot is not the volume's");
	if(LE32(APFS_OMAP_PHYS(&vol->omap)->om_snap_count) != nsnaps)
		problem(c, "the object map counts %u snapshots, the volume has %zu", LE32(APFS_OMAP_PHYS(&vol->omap)->om_snap_count),
			nsnaps);
	for(i = 0; i < nsnaps; i++) {
		apfs_vol_t* sv;
		apfs_btree_t bt;
		char what[300];

		snprintf(what, sizeof(what), "snapshot \"%s\"", snaps[i].name);
		mark(c, snaps[i].sblock_oid, 1, what, 0);
		check_object(c, snaps[i].sblock_oid, snaps[i].sblock_oid, 0, what);
		snprintf(what, sizeof(what), "extent reference tree of snapshot \"%s\"", snaps[i].name);
		apfs_bt_init(&bt, nx, vol, NULL, OBJ_PHYSICAL, OBJECT_TYPE_BLOCKREFTREE, snaps[i].extentref_tree_oid, apfs_cmp_fs);
		check_extref(c, &bt, what, &pexts);
		apfs_bt_close(&bt);
		if(apfs_vol_open_snapshot(nx, index, snaps[i].name, &sv) < 0) {
			problem(c, "cannot open snapshot \"%s\"", snaps[i].name);
			continue;
		}
		snprintf(what, sizeof(what), "file-system tree of snapshot \"%s\"", snaps[i].name);
		check_tree(c, &sv->fs, what, 1, 1);
		if(nsingles + 3 <= sizeof(singles) / sizeof(singles[0])) {
			singles[nsingles++] = LE64(sv->sb->apfs_integrity_meta_oid);
			singles[nsingles++] = LE64(sv->sb->apfs_er_state_oid);
			singles[nsingles++] = LE64(sv->sb->apfs_snap_meta_ext_oid);
		}
		apfs_vol_close(sv);
	}

	/* every object map entry must be reachable */
	qsort(c->seen, c->nseen, sizeof(vobj_t), vobj_cmp);
	for(i = 0; i < nom; i++) {
		vobj_t key;
		if(om[i].flags & OMAP_VAL_DELETED)
			continue;
		key.oid = om[i].oid;
		key.xid = om[i].xid;
		if(!bsearch(&key, c->seen, c->nseen, sizeof(vobj_t), vobj_cmp) && !is_listed(singles, nsingles, om[i].oid))
			problem(c, "object map entry 0x%llx (transaction %llu) is not used by anything", (unsigned long long)om[i].oid,
				(unsigned long long)om[i].xid);
	}

	progress(c, "  file-system records");
	check_fs(c, vol, &pexts, (int)nsnaps);

	if(c->charged != LE64(vsb->apfs_fs_alloc_count))
		problem(c, "the volume counts %llu allocated blocks but uses %llu", (unsigned long long)LE64(vsb->apfs_fs_alloc_count),
			(unsigned long long)c->charged);

	apfs_snapshots_free(snaps, nsnaps);
	free(om);
	free(pexts.list);
	c->where[0] = 0;
}

/* Container */

static void check_free_queues(vctx_t* c, spaceman_phys_t* sm) {
	int q;
	for(q = 0; q < SFQ_COUNT; q++) {
		apfs_btree_t bt;
		apfs_btiter_t it;
		apfs_kv_t kv;
		oid_t root = LE64(sm->sm_fq[q].sfq_tree_oid);
		uint64_t count = 0;
		int ret;
		if(!root)
			continue;
		apfs_bt_init(&bt, c->nx, NULL, NULL, OBJ_EPHEMERAL, OBJECT_TYPE_SPACEMAN_FREE_QUEUE, root, apfs_cmp_u64pair);
		ret = apfs_bt_load(&bt);
		if(ret == APFS_OK && (bt.info.bt_fixed.bt_key_size != 16 || bt.info.bt_fixed.bt_val_size != 8 ||
				bt.info.bt_longest_key != 16 || bt.info.bt_longest_val > 8))
			problem(c, "free queue %d has invalid fixed record widths or maximum key/value sizes", q);
		if(ret == APFS_OK)
			ret = apfs_bt_iter_seek(&it, &bt, NULL, 0);
		while(ret == APFS_OK && it.valid) {
			paddr_t start;
			uint64_t len, b;
			apfs_bt_iter_get(&it, &kv);
			start = apfs_get64(kv.k + 8);
			len = (kv.vlen == 8) ? apfs_get64(kv.v) : 1;
			for(b = 0; b < len && start + b < c->nblocks; b++)
				bit_set(c->pending, start + b);
			count += len;
			ret = apfs_bt_iter_next(&it);
		}
		if(ret < 0)
			problem(c, "cannot read free queue %d", q);
		else if(count != LE64(sm->sm_fq[q].sfq_count))
			problem(c, "free queue %d holds %llu blocks but counts %llu", q, (unsigned long long)count,
				(unsigned long long)LE64(sm->sm_fq[q].sfq_count));
		apfs_bt_close(&bt);
	}
}

static void check_spaceman(vctx_t* c) {
	apfs_nx_t* nx = c->nx;
	apfs_sm_t* sm = &nx->sm;
	apfs_eph_t* e = apfs_nx_eph(nx, LE64(nx->sb->nx_spaceman_oid));
	spaceman_phys_t* p;
	uint64_t free_blocks = 0, leaked = 0, i;
	paddr_t first_leak = 0;
	uint32_t k;

	if(!e) {
		problem(c, "the space manager is missing");
		return;
	}
	p = (spaceman_phys_t*)e->data;
	mark(c, LE64(p->sm_ip_bm_base), LE32(p->sm_ip_bm_block_count), "internal pool bitmaps", 0);
	mark(c, LE64(p->sm_ip_base), LE64(p->sm_ip_block_count), "internal pool", 0);
	if(apfs_sm_load(nx) < 0) {
		problem(c, "cannot load the space manager");
		return;
	}
	for(k = 0; k < sm->ncib; k++) {
		chunk_info_block_t* cib = (chunk_info_block_t*)sm->cib[k];
		uint32_t n = LE32(cib->cib_chunk_info_count), j;
		if(sm->cib_addr[k] < sm->ip_base || sm->cib_addr[k] >= sm->ip_base + sm->ip_blocks)
			problem(c, "chunk-info block 0x%llx is outside the internal pool", (unsigned long long)sm->cib_addr[k]);
		for(j = 0; j < n; j++) {
			chunk_info_t* ci = &cib->cib_chunk_info[j];
			uint64_t chunk = LE64(ci->ci_addr) / sm->blocks_per_chunk;
			uint32_t blocks = LE32(ci->ci_block_count) & CI_COUNT_MASK, zero = 0, b;
			paddr_t bm = LE64(ci->ci_bitmap_addr);
			if(bm && (bm < sm->ip_base || bm >= sm->ip_base + sm->ip_blocks))
				problem(c, "bitmap block 0x%llx is outside the internal pool", (unsigned long long)bm);
			if(chunk >= sm->nchunks)
				continue;
			for(b = 0; b < blocks; b++)
				if(!bit_get(sm->bitmap, chunk * sm->blocks_per_chunk + b))
					zero++;
			if(zero != LE32(ci->ci_free_count))
				problem(c, "chunk %llu counts %u free blocks but has %u", (unsigned long long)chunk, LE32(ci->ci_free_count), zero);
		}
	}
	for(k = 0; k < sm->ncab; k++)
		if(sm->cab_addr[k] < sm->ip_base || sm->cab_addr[k] >= sm->ip_base + sm->ip_blocks)
			problem(c, "chunk-info address block 0x%llx is outside the internal pool", (unsigned long long)sm->cab_addr[k]);

	check_free_queues(c, p);

	for(i = 0; i < c->nblocks; i++) {
		int allocated = bit_get(sm->bitmap, i);
		if(!allocated)
			free_blocks++;
		if(bit_get(c->used, i) && !allocated) {
			problem(c, "block 0x%llx is in use but marked free", (unsigned long long)i);
		} else if(allocated && !bit_get(c->used, i) && !bit_get(c->pending, i)) {
			if(!leaked)
				first_leak = i;
			leaked++;
		}
	}
	if(leaked)
		problem(c, "%llu blocks are allocated but not used (the first is 0x%llx)", (unsigned long long)leaked,
			(unsigned long long)first_leak);
	if(free_blocks != LE64(p->sm_dev[SD_MAIN].sm_free_count))
		problem(c, "the space manager counts %llu free blocks but the bitmap has %llu",
			(unsigned long long)LE64(p->sm_dev[SD_MAIN].sm_free_count), (unsigned long long)free_blocks);
}

int apfs_verify(apfs_nx_t* nx, int verbose) {
	nx_superblock_t* sb = nx->sb;
	vctx_t c;
	apfs_omap_rec_t* om = NULL;
	size_t nom = 0, i;
	int v;

	memset(&c, 0, sizeof(c));
	c.nx = nx;
	c.verbose = verbose;
	c.nblocks = LE64(sb->nx_block_count);
	c.used = (uint8_t*)apfs_calloc((size_t)(c.nblocks / 8 + 1), 1);
	c.pending = (uint8_t*)apfs_calloc((size_t)(c.nblocks / 8 + 1), 1);

	progress(&c, "Checking the container (transaction %llu)", (unsigned long long)nx->xid);
	mark(&c, 0, 1, "container superblock", 0);
	if((LE32(sb->nx_xp_desc_blocks) | LE32(sb->nx_xp_data_blocks)) & 0x80000000U)
		problem(&c, "checkpoint areas that are not contiguous are not supported");
	else {
		mark(&c, LE64(sb->nx_xp_desc_base), LE32(sb->nx_xp_desc_blocks), "checkpoint descriptors", 0);
		mark(&c, LE64(sb->nx_xp_data_base), LE32(sb->nx_xp_data_blocks), "checkpoint data", 0);
	}
	if(LE64(sb->nx_efi_jumpstart)) {
		uint8_t* buf = (uint8_t*)apfs_malloc(nx->bs);
		nx_efi_jumpstart_t* js = (nx_efi_jumpstart_t*)buf;
		paddr_t jp = LE64(sb->nx_efi_jumpstart);
		mark(&c, jp, 1, "EFI jumpstart", 0);
		if(apfs_read_obj(nx, jp, buf, NULL, NULL, NULL, NULL) < 0 || LE32(js->nej_magic) != NX_EFI_JUMPSTART_MAGIC) {
			problem(&c, "the EFI jumpstart is invalid");
		} else {
			uint32_t n = LE32(js->nej_num_extents), j;
			for(j = 0; j < n && sizeof(nx_efi_jumpstart_t) + (j + 1) * sizeof(prange_t) <= nx->bs; j++)
				mark(&c, LE64(js->nej_rec_extents[j].pr_start_paddr), LE64(js->nej_rec_extents[j].pr_block_count), "EFI driver", 0);
		}
		free(buf);
	}
	if(LE64(sb->nx_keylocker.pr_block_count))
		mark(&c, LE64(sb->nx_keylocker.pr_start_paddr), LE64(sb->nx_keylocker.pr_block_count), "keybag", 0);
	if(LE64(sb->nx_mkb_locker.pr_block_count))
		mark(&c, LE64(sb->nx_mkb_locker.pr_start_paddr), LE64(sb->nx_mkb_locker.pr_block_count), "media keybag", 0);

	mark(&c, nx->omap.paddr, 1, "container object map", 0);
	check_object(&c, nx->omap.paddr, nx->omap.paddr, 0, "container object map");
	check_tree(&c, &nx->omap.tree, "container object map tree", 0, 1);
	if(apfs_omap_list(&nx->omap, &om, &nom) < 0)
		problem(&c, "cannot read the container object map");
	for(i = 0; i < nom; i++) {
		int used = 0;
		for(v = 0; v < NX_MAX_FILE_SYSTEMS; v++)
			if(LE64(sb->nx_fs_oid[v]) == om[i].oid)
				used = 1;
		if(om[i].flags & OMAP_VAL_DELETED)
			continue;
		mark(&c, om[i].paddr, 1, "volume superblock", 0);
		check_object(&c, om[i].paddr, om[i].oid, om[i].xid, "volume superblock");
		if(!used)
			problem(&c, "container object map entry 0x%llx is not a volume", (unsigned long long)om[i].oid);
	}
	free(om);

	for(v = 0; v < NX_MAX_FILE_SYSTEMS; v++)
		if(sb->nx_fs_oid[v])
			check_volume(&c, v);

	progress(&c, "Checking the space manager");
	check_spaceman(&c);

	free(c.used);
	free(c.pending);
	free(c.seen);
	if(c.problems)
		APFS_ERR("%llu problem%s found", (unsigned long long)c.problems, c.problems == 1 ? "" : "s");
	return c.problems ? APFS_ECORRUPT : APFS_OK;
}
