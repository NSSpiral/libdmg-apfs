/*
 * volume.c
 * apfs-tool
 *
 * Volume superblocks: mounting (optionally from a snapshot), flushing and
 * bookkeeping.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apfs/apfs.h"

#define APFS_TOOL_ID "apfs-tool (1.0)"

static const struct {
	uint16_t role;
	const char* name;
} roles[] = {
	{ APFS_VOL_ROLE_NONE, "none" },
	{ APFS_VOL_ROLE_SYSTEM, "system" },
	{ APFS_VOL_ROLE_USER, "user" },
	{ APFS_VOL_ROLE_RECOVERY, "recovery" },
	{ APFS_VOL_ROLE_VM, "vm" },
	{ APFS_VOL_ROLE_PREBOOT, "preboot" },
	{ APFS_VOL_ROLE_INSTALLER, "installer" },
	{ APFS_VOL_ROLE_DATA, "data" },
	{ APFS_VOL_ROLE_BASEBAND, "baseband" },
	{ APFS_VOL_ROLE_UPDATE, "update" },
	{ APFS_VOL_ROLE_XART, "xart" },
	{ APFS_VOL_ROLE_HARDWARE, "hardware" },
	{ APFS_VOL_ROLE_BACKUP, "backup" },
	{ APFS_VOL_ROLE_ENTERPRISE, "enterprise" },
	{ APFS_VOL_ROLE_PRELOGIN, "prelogin" },
	{ 0, NULL }
};

const char* apfs_role_name(uint16_t role) {
	int i;
	for(i = 0; roles[i].name; i++)
		if(roles[i].role == role)
			return roles[i].name;
	return "unknown";
}

int apfs_role_parse(const char* s, uint16_t* role) {
	int i;
	char* end;
	unsigned long v;
	for(i = 0; roles[i].name; i++) {
		const char* a = roles[i].name;
		const char* b = s;
		while(*a && *b && (*a == *b || *a == (*b | 0x20)))
			a++, b++;
		if(!*a && !*b) {
			*role = roles[i].role;
			return APFS_OK;
		}
	}
	v = strtoul(s, &end, 0);
	if(*s && !*end && v <= 0xffff) {
		*role = (uint16_t)v;
		return APFS_OK;
	}
	return APFS_EINVAL;
}

static void vol_set_flags(apfs_vol_t* vol) {
	uint64_t incompat = LE64(vol->sb->apfs_incompatible_features);
	vol->case_fold = (incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;
	vol->hashed_names = (incompat & (APFS_INCOMPAT_CASE_INSENSITIVE | APFS_INCOMPAT_NORMALIZATION_INSENSITIVE)) != 0;
	vol->sibling_maps = 1;
	vol->sealed = (incompat & APFS_INCOMPAT_SEALED_VOLUME) != 0;
}

static int vol_open_trees(apfs_vol_t* vol, apfs_omap_t* omap, oid_t root_tree, oid_t extref, oid_t snapmeta, xid_t read_xid) {
	apfs_nx_t* nx = vol->nx;
	apfs_superblock_t* vsb = vol->sb;
	int ret;

	apfs_bt_init(&vol->fs, nx, vol, omap, LE32(vsb->apfs_root_tree_type) & OBJ_STORAGETYPE_MASK,
			OBJECT_TYPE_FSTREE, root_tree, apfs_cmp_fs);
	vol->fs.read_xid = read_xid;
	ret = apfs_bt_load(&vol->fs);
	if(ret < 0) {
		APFS_ERR("cannot load the file-system tree of volume %d", vol->index);
		return ret;
	}

	if(extref) {
		apfs_bt_init(&vol->extref, nx, vol, NULL, OBJ_PHYSICAL, OBJECT_TYPE_BLOCKREFTREE, extref, apfs_cmp_fs);
		ret = apfs_bt_load(&vol->extref);
		if(ret < 0)
			return ret;
	}
	if(snapmeta) {
		apfs_bt_init(&vol->snapmeta, nx, vol, NULL, OBJ_PHYSICAL, OBJECT_TYPE_SNAPMETATREE, snapmeta, apfs_cmp_fs);
		ret = apfs_bt_load(&vol->snapmeta);
		if(ret < 0)
			return ret;
	}
	if(vol->sealed && vsb->apfs_fext_tree_oid) {
		apfs_bt_init(&vol->fext, nx, vol, NULL, OBJ_PHYSICAL, OBJECT_TYPE_FEXT_TREE,
				LE64(vsb->apfs_fext_tree_oid), apfs_cmp_u64pair);
		ret = apfs_bt_load(&vol->fext);
		if(ret < 0)
			return ret;
	}
	return APFS_OK;
}

static void vol_free(apfs_vol_t* vol) {
	if(vol->fs.htab || vol->fs.loaded)
		apfs_bt_close(&vol->fs);
	apfs_bt_close(&vol->extref);
	apfs_bt_close(&vol->snapmeta);
	apfs_bt_close(&vol->fext);
	if(vol->snap_xid == 0)
		apfs_omap_close(&vol->omap);
	free(vol->raw);
	free(vol->snap_name);
	free(vol);
}


int apfs_vol_open(apfs_nx_t* nx, int index, apfs_vol_t** out) {
	apfs_vol_t* vol;
	oid_t oid;
	uint32_t type, vflags;
	int ret;

	if(index < 0 || index >= NX_MAX_FILE_SYSTEMS)
		return APFS_EINVAL;
	if(nx->vols[index]) {
		*out = nx->vols[index];
		return APFS_OK;
	}
	oid = LE64(nx->sb->nx_fs_oid[index]);
	if(!oid)
		return APFS_ENOENT;

	vol = (apfs_vol_t*)apfs_calloc(1, sizeof(apfs_vol_t));
	vol->nx = nx;
	vol->index = index;
	vol->oid = oid;
	vol->raw = (uint8_t*)apfs_malloc(nx->bs);
	vol->sb = (apfs_superblock_t*)vol->raw;
	vol->readonly = !nx->writable;

	ret = apfs_omap_lookup(&nx->omap, oid, 0, &vol->paddr, &vol->xid, &vflags);
	if(ret < 0) {
		APFS_ERR("volume %d (oid 0x%llx) is missing from the container object map", index, (unsigned long long)oid);
		free(vol->raw);
		free(vol);
		return ret == APFS_ENOENT ? APFS_ECORRUPT : ret;
	}
	ret = apfs_read_obj(nx, vol->paddr, vol->raw, &type, NULL, NULL, NULL);
	if(ret < 0 || (type & OBJECT_TYPE_MASK) != OBJECT_TYPE_FS || LE32(vol->sb->apfs_magic) != APFS_MAGIC) {
		APFS_ERR("block 0x%llx is not a volume superblock", (unsigned long long)vol->paddr);
		free(vol->raw);
		free(vol);
		return ret < 0 ? ret : APFS_ECORRUPT;
	}
	vol_set_flags(vol);

	if(!(LE64(vol->sb->apfs_fs_flags) & APFS_FS_UNENCRYPTED))
		APFS_WARN("volume %d is encrypted; file contents cannot be read", index);

	ret = apfs_omap_open(&vol->omap, nx, vol, LE64(vol->sb->apfs_omap_oid));
	if(ret < 0) {
		free(vol->raw);
		free(vol);
		return ret;
	}
	ret = vol_open_trees(vol, &vol->omap, LE64(vol->sb->apfs_root_tree_oid), LE64(vol->sb->apfs_extentref_tree_oid),
			LE64(vol->sb->apfs_snap_meta_tree_oid), 0);
	if(ret < 0) {
		vol_free(vol);
		return ret;
	}

	nx->vols[index] = vol;
	*out = vol;
	return APFS_OK;
}

int apfs_vol_open_snapshot(apfs_nx_t* nx, int index, const char* snapname, apfs_vol_t** out) {
	apfs_vol_t* live;
	apfs_vol_t* vol;
	apfs_snapshot_t* snaps;
	size_t count, i;
	uint32_t type;
	int ret;

	ret = apfs_vol_open(nx, index, &live);
	if(ret < 0)
		return ret;
	ret = apfs_snapshot_list(live, &snaps, &count);
	if(ret < 0)
		return ret;
	for(i = 0; i < count; i++)
		if(strcmp(snaps[i].name, snapname) == 0)
			break;
	if(i == count) {
		apfs_snapshots_free(snaps, count);
		return APFS_ENOENT;
	}

	vol = (apfs_vol_t*)apfs_calloc(1, sizeof(apfs_vol_t));
	vol->nx = nx;
	vol->index = index;
	vol->oid = live->oid;
	vol->readonly = 1;
	vol->snap_xid = snaps[i].xid;
	vol->snap_name = apfs_strdup(snapname);
	vol->raw = (uint8_t*)apfs_malloc(nx->bs);
	vol->sb = (apfs_superblock_t*)vol->raw;
	vol->paddr = snaps[i].sblock_oid;
	ret = apfs_read_obj(nx, vol->paddr, vol->raw, &type, NULL, NULL, &vol->xid);
	if(ret < 0 || (type & OBJECT_TYPE_MASK) != OBJECT_TYPE_FS) {
		apfs_snapshots_free(snaps, count);
		free(vol->raw);
		free(vol->snap_name);
		free(vol);
		return ret < 0 ? ret : APFS_ECORRUPT;
	}
	vol_set_flags(vol);
	/* snapshots share the live volume's object map */
	ret = vol_open_trees(vol, &live->omap, LE64(vol->sb->apfs_root_tree_oid), snaps[i].extentref_tree_oid, 0, snaps[i].xid);
	apfs_snapshots_free(snaps, count);
	if(ret < 0) {
		vol_free(vol);
		return ret;
	}
	*out = vol;
	return APFS_OK;
}

int apfs_vol_mark_dirty(apfs_vol_t* vol) {
	int ret;
	if(vol->readonly || vol->snap_xid)
		return APFS_EROFS;
	ret = apfs_nx_begin(vol->nx);
	if(ret < 0)
		return ret;
	vol->dirty = 1;
	return APFS_OK;
}

uint64_t apfs_vol_next_id(apfs_vol_t* vol) {
	uint64_t id = LE64(vol->sb->apfs_next_obj_id);
	vol->sb->apfs_next_obj_id = LE64(id + 1);
	vol->dirty = 1;
	return id;
}

static void update_modified_by(apfs_vol_t* vol) {
	apfs_superblock_t* vsb = vol->sb;
	apfs_modified_by_t* m = vsb->apfs_modified_by;
	uint64_t now = apfs_time_now();

	vsb->apfs_last_mod_time = LE64(now);
	/* a volume created by this transaction has no modification history */
	if(LE64(vsb->apfs_formatted_by.last_xid) == vol->nx->txn)
		return;
	if(strncmp((const char*)m[0].id, APFS_TOOL_ID, APFS_MODIFIED_NAMELEN) != 0) {
		memmove(&m[1], &m[0], sizeof(apfs_modified_by_t) * (APFS_MAX_HIST - 1));
		memset(&m[0], 0, sizeof(apfs_modified_by_t));
		strncpy((char*)m[0].id, APFS_TOOL_ID, APFS_MODIFIED_NAMELEN - 1);
	}
	m[0].timestamp = LE64(now);
	m[0].last_xid = LE64(vol->nx->txn);
}

int apfs_vol_flush(apfs_vol_t* vol) {
	apfs_nx_t* nx = vol->nx;
	apfs_superblock_t* vsb = vol->sb;
	int ret;

	if(!vol->dirty || vol->readonly)
		return APFS_OK;

	ret = apfs_bt_flush(&vol->fs);
	if(ret == APFS_OK && vol->extref.loaded)
		ret = apfs_bt_flush(&vol->extref);
	if(ret == APFS_OK && vol->snapmeta.loaded)
		ret = apfs_bt_flush(&vol->snapmeta);
	if(ret == APFS_OK && vol->fext.loaded)
		ret = apfs_bt_flush(&vol->fext);
	if(ret < 0)
		return ret;

	vsb->apfs_root_tree_oid = LE64(vol->fs.root);
	if(vol->extref.loaded)
		vsb->apfs_extentref_tree_oid = LE64(vol->extref.root);
	if(vol->snapmeta.loaded)
		vsb->apfs_snap_meta_tree_oid = LE64(vol->snapmeta.root);
	if(vol->fext.loaded)
		vsb->apfs_fext_tree_oid = LE64(vol->fext.root);

	ret = apfs_omap_flush(&vol->omap);
	if(ret < 0)
		return ret;
	vsb->apfs_omap_oid = LE64(vol->omap.paddr);

	update_modified_by(vol);

	if(vol->xid != nx->txn) {
		paddr_t newp;
		ret = apfs_omap_cow(&nx->omap, vol->oid, vol->xid, vol->paddr, &newp, 0);
		if(ret < 0)
			return ret;
		vol->paddr = newp;
		vol->xid = nx->txn;
	}
	vsb->apfs_o.o_oid = LE64(vol->oid);
	vsb->apfs_o.o_xid = LE64(nx->txn);
	vsb->apfs_o.o_type = LE32(OBJ_VIRTUAL | OBJECT_TYPE_FS);
	vsb->apfs_o.o_subtype = 0;
	ret = apfs_write_obj(nx, vol->paddr, vol->raw);
	if(ret < 0)
		return ret;
	vol->dirty = 0;
	return APFS_OK;
}

void apfs_vol_close(apfs_vol_t* vol) {
	if(!vol)
		return;
	if(vol->snap_xid) {
		vol_free(vol);
		return;
	}
	if(vol->nx->vols[vol->index] == vol)
		vol->nx->vols[vol->index] = NULL;
	vol_free(vol);
}

/* Accepts a volume index, or a volume name */
int apfs_vol_find(apfs_nx_t* nx, const char* name_or_index, int* index) {
	int i;
	char* end;
	long v;

	if(!name_or_index || !*name_or_index) {
		for(i = 0; i < NX_MAX_FILE_SYSTEMS; i++) {
			if(nx->sb->nx_fs_oid[i]) {
				*index = i;
				return APFS_OK;
			}
		}
		return APFS_ENOENT;
	}

	v = strtol(name_or_index, &end, 10);
	if(!*end && v >= 0 && v < NX_MAX_FILE_SYSTEMS) {
		if(!nx->sb->nx_fs_oid[v])
			return APFS_ENOENT;
		*index = (int)v;
		return APFS_OK;
	}

	for(i = 0; i < NX_MAX_FILE_SYSTEMS; i++) {
		apfs_vol_t* vol;
		if(!nx->sb->nx_fs_oid[i])
			continue;
		if(apfs_vol_open(nx, i, &vol) < 0)
			continue;
		if(strncmp((const char*)vol->sb->apfs_volname, name_or_index, APFS_VOLNAME_LEN) == 0) {
			*index = i;
			return APFS_OK;
		}
	}
	return APFS_ENOENT;
}

int apfs_vol_rename(apfs_vol_t* vol, const char* name) {
	int ret;
	if(strlen(name) >= APFS_VOLNAME_LEN || !*name)
		return APFS_ENAMETOOLONG;
	ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;
	memset(vol->sb->apfs_volname, 0, APFS_VOLNAME_LEN);
	memcpy(vol->sb->apfs_volname, name, strlen(name));
	return APFS_OK;
}

int apfs_vol_set_role(apfs_vol_t* vol, uint16_t role) {
	int ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;
	vol->sb->apfs_role = LE16(role);
	return APFS_OK;
}

static int insert_dir_inode(apfs_vol_t* vol, uint64_t ino, const char* name, uint16_t mode, uint32_t uid, uint32_t gid, uint64_t now) {
	apfs_inode_t inode;
	int ret;
	memset(&inode, 0, sizeof(inode));
	inode.ino = ino;
	inode.parent_id = ROOT_DIR_PARENT;
	inode.private_id = ino;
	inode.create_time = inode.mod_time = inode.change_time = inode.access_time = now;
	inode.internal_flags = INODE_NO_RSRC_FORK;
	inode.write_gen = 1;
	inode.uid = uid;
	inode.gid = gid;
	inode.mode = mode;
	apfs_inode_set_name(&inode, name);
	ret = apfs_inode_write(vol, &inode);
	apfs_inode_release(&inode);
	return ret;
}

static int insert_root_drec(apfs_vol_t* vol, const char* name, uint64_t ino, uint64_t now) {
	uint8_t key[12 + 64], val[18];
	int klen = apfs_drec_key(vol, ROOT_DIR_PARENT, name, key);
	if(klen < 0)
		return klen;
	apfs_put64(val, ino);
	apfs_put64(val + 8, now);
	apfs_put16(val + 16, APFS_DT_DIR);
	return apfs_bt_insert(&vol->fs, key, (uint16_t)klen, val, sizeof(val));
}

int apfs_vol_create(apfs_nx_t* nx, const apfs_vol_params_t* params, int* index_out) {
	apfs_vol_t* vol;
	apfs_superblock_t* s;
	uint32_t max = LE32(nx->sb->nx_max_file_systems);
	uint64_t now = apfs_time_now();
	const char* name = params->name ? params->name : "untitled";
	int index, ret;

	if(strlen(name) >= APFS_VOLNAME_LEN)
		return APFS_ENAMETOOLONG;
	if(max > NX_MAX_FILE_SYSTEMS)
		max = NX_MAX_FILE_SYSTEMS;
	for(index = 0; index < (int)max; index++)
		if(!nx->sb->nx_fs_oid[index])
			break;
	if(index == (int)max) {
		APFS_ERR("the container already holds its maximum of %u volumes", max);
		return APFS_ENOSPC;
	}
	ret = apfs_nx_begin(nx);
	if(ret < 0)
		return ret;

	vol = (apfs_vol_t*)apfs_calloc(1, sizeof(apfs_vol_t));
	vol->nx = nx;
	vol->index = index;
	vol->raw = (uint8_t*)apfs_calloc(1, nx->bs);
	vol->sb = s = (apfs_superblock_t*)vol->raw;
	vol->oid = apfs_nx_new_oid(nx);
	vol->xid = nx->txn;

	ret = apfs_sm_alloc_block(nx, NULL, &vol->paddr);
	if(ret == APFS_OK)
		ret = apfs_omap_set(&nx->omap, vol->oid, nx->txn, 0, vol->paddr);
	if(ret < 0) {
		free(vol->raw);
		free(vol);
		return ret;
	}

	s->apfs_magic = LE32(APFS_MAGIC);
	s->apfs_fs_index = LE32((uint32_t)index);
	s->apfs_features = LE64(APFS_FEATURE_HARDLINK_MAP_RECORDS);
	if(params->case_sensitive)
		s->apfs_incompatible_features = LE64(params->norm_insensitive ? APFS_INCOMPAT_NORMALIZATION_INSENSITIVE : 0);
	else
		s->apfs_incompatible_features = LE64(APFS_INCOMPAT_CASE_INSENSITIVE);
	s->apfs_fs_reserve_block_count = LE64(params->reserve_blocks);
	s->apfs_fs_quota_block_count = LE64(params->quota_blocks);
	s->apfs_meta_crypto.major_version = LE16(5);
	s->apfs_meta_crypto.minor_version = LE16(0);
	s->apfs_meta_crypto.persistent_class = LE32(6);
	s->apfs_meta_crypto.key_revision = LE16(1);
	s->apfs_root_tree_type = LE32(OBJ_VIRTUAL | OBJECT_TYPE_BTREE);
	s->apfs_extentref_tree_type = LE32(OBJ_PHYSICAL | OBJECT_TYPE_BTREE);
	s->apfs_snap_meta_tree_type = LE32(OBJ_PHYSICAL | OBJECT_TYPE_BTREE);
	s->apfs_fext_tree_type = LE32(OBJ_PHYSICAL | OBJECT_TYPE_BTREE);
	s->apfs_next_obj_id = LE64(MIN_USER_INO_NUM);
	s->apfs_next_doc_id = LE32(3);
	s->apfs_fs_flags = LE64(APFS_FS_UNENCRYPTED);
	s->apfs_role = LE16(params->role);
	s->apfs_last_mod_time = LE64(now);
	if(params->uuid)
		memcpy(s->apfs_vol_uuid, params->uuid, 16);
	else
		apfs_uuid_generate(s->apfs_vol_uuid);
	memcpy(s->apfs_volname, name, strlen(name));
	strncpy((char*)s->apfs_formatted_by.id, APFS_TOOL_ID, APFS_MODIFIED_NAMELEN - 1);
	s->apfs_formatted_by.timestamp = LE64(now);
	s->apfs_formatted_by.last_xid = LE64(nx->txn);
	vol_set_flags(vol);

	ret = apfs_omap_create(&vol->omap, nx, vol, 0);
	if(ret == APFS_OK) {
		apfs_bt_init(&vol->fs, nx, vol, &vol->omap, OBJ_VIRTUAL, OBJECT_TYPE_FSTREE, 0, apfs_cmp_fs);
		ret = apfs_bt_create(&vol->fs, BTREE_SEQUENTIAL_INSERT | BTREE_KV_NONALIGNED, 0, 0);
	}
	if(ret == APFS_OK) {
		apfs_bt_init(&vol->extref, nx, vol, NULL, OBJ_PHYSICAL, OBJECT_TYPE_BLOCKREFTREE, 0, apfs_cmp_fs);
		ret = apfs_bt_create(&vol->extref, BTREE_SEQUENTIAL_INSERT | BTREE_PHYSICAL | BTREE_KV_NONALIGNED, 0, 0);
	}
	if(ret == APFS_OK) {
		apfs_bt_init(&vol->snapmeta, nx, vol, NULL, OBJ_PHYSICAL, OBJECT_TYPE_SNAPMETATREE, 0, apfs_cmp_fs);
		ret = apfs_bt_create(&vol->snapmeta, BTREE_SEQUENTIAL_INSERT | BTREE_PHYSICAL | BTREE_KV_NONALIGNED, 0, 0);
	}
	if(ret < 0)
		return ret;

	s->apfs_omap_oid = LE64(vol->omap.paddr);
	s->apfs_root_tree_oid = LE64(vol->fs.root);
	s->apfs_extentref_tree_oid = LE64(vol->extref.root);
	s->apfs_snap_meta_tree_oid = LE64(vol->snapmeta.root);

	nx->vols[index] = vol;
	nx->sb->nx_fs_oid[index] = LE64(vol->oid);
	vol->dirty = 1;
	nx->dirty = 1;

	ret = insert_root_drec(vol, "private-dir", PRIV_DIR_INO_NUM, now);
	if(ret == APFS_OK)
		ret = insert_root_drec(vol, "root", ROOT_DIR_INO_NUM, now);
	if(ret == APFS_OK)
		ret = insert_dir_inode(vol, ROOT_DIR_INO_NUM, "root", (uint16_t)(APFS_S_IFDIR | (params->root_mode ? params->root_mode & 07777 : 0755)),
				params->root_uid, params->root_gid, now);
	if(ret == APFS_OK)
		ret = insert_dir_inode(vol, PRIV_DIR_INO_NUM, "private-dir", APFS_S_IFDIR | 0644, 0, 0, now);
	if(ret < 0)
		return ret;
	if(index_out)
		*index_out = index;
	return APFS_OK;
}

/* Volume deletion: every block the volume owns goes back to the container */

typedef struct {
	apfs_extent_t* list;
	size_t count;
	size_t cap;
} extlist_t;

static void ext_add(extlist_t* l, paddr_t start, uint64_t count) {
	if(!start || !count)
		return;
	if(l->count == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 256;
		l->list = (apfs_extent_t*)apfs_realloc(l->list, l->cap * sizeof(apfs_extent_t));
	}
	l->list[l->count].start = start;
	l->list[l->count].count = count;
	l->list[l->count].owner = NULL;
	l->count++;
}

static int ext_cmp(const void* a, const void* b) {
	const apfs_extent_t* x = (const apfs_extent_t*)a;
	const apfs_extent_t* y = (const apfs_extent_t*)b;
	return x->start < y->start ? -1 : (x->start > y->start ? 1 : 0);
}

static int node_cb(apfs_btree_t* bt, apfs_btnode_t* n, int depth, void* ctx) {
	(void)bt;
	(void)depth;
	ext_add((extlist_t*)ctx, n->paddr, 1);
	return APFS_OK;
}

/* Adds the nodes of a physical tree and, for extent reference trees, the
 * extents it owns. Extents of file data are listed by the tree that owns
 * them, the live one or a snapshot's. */
static int collect_tree(apfs_btree_t* bt, extlist_t* l, int extref) {
	apfs_btiter_t it;
	apfs_kv_t kv;
	int ret;

	ret = apfs_bt_walk(bt, node_cb, l);
	if(ret < 0 || !extref)
		return ret;
	ret = apfs_bt_iter_seek(&it, bt, NULL, 0);
	while(ret == APFS_OK && it.valid) {
		apfs_bt_iter_get(&it, &kv);
		if(kv.klen == 8 && kv.vlen >= 20 && apfs_key_type(kv.k) == APFS_TYPE_EXTENT &&
				(apfs_get64(kv.v) >> PEXT_KIND_SHIFT) == APFS_KIND_NEW)
			ext_add(l, apfs_key_id(kv.k), apfs_get64(kv.v) & PEXT_LEN_MASK);
		ret = apfs_bt_iter_next(&it);
	}
	return ret;
}

static int collect_phys_tree(apfs_vol_t* vol, oid_t root, uint32_t subtype, extlist_t* l, int extref) {
	apfs_btree_t bt;
	int ret;
	if(!root)
		return APFS_OK;
	apfs_bt_init(&bt, vol->nx, vol, NULL, OBJ_PHYSICAL, subtype, root, apfs_cmp_fs);
	ret = apfs_bt_load(&bt);
	if(ret == APFS_OK)
		ret = collect_tree(&bt, l, extref);
	apfs_bt_close(&bt);
	return ret;
}

/* File extents of the live volume, in case some lack extent references */
static int collect_file_extents(apfs_vol_t* vol, extlist_t* l) {
	apfs_btree_t* bt = vol->fext.loaded ? &vol->fext : &vol->fs;
	uint32_t bs = vol->nx->bs;
	apfs_btiter_t it;
	apfs_kv_t kv;
	int ret;

	ret = apfs_bt_iter_seek(&it, bt, NULL, 0);
	while(ret == APFS_OK && it.valid) {
		apfs_bt_iter_get(&it, &kv);
		if(bt == &vol->fext ? (kv.klen == 16 && kv.vlen == 16) :
				(kv.klen == 16 && kv.vlen >= 24 && apfs_key_type(kv.k) == APFS_TYPE_FILE_EXTENT)) {
			uint64_t len = apfs_get64(kv.v) & J_FILE_EXTENT_LEN_MASK;
			ext_add(l, apfs_get64(kv.v + 8), (len + bs - 1) / bs);
		}
		ret = apfs_bt_iter_next(&it);
	}
	return ret;
}

int apfs_vol_delete(apfs_nx_t* nx, int index) {
	apfs_vol_t* vol;
	apfs_omap_rec_t* om;
	apfs_snapshot_t* snaps = NULL;
	extlist_t l = { NULL, 0, 0 };
	size_t n, i, nsnaps = 0;
	apfs_superblock_t* vsb;
	int ret;

	ret = apfs_vol_open(nx, index, &vol);
	if(ret < 0)
		return ret;
	ret = apfs_nx_begin(nx);
	if(ret < 0)
		return ret;
	vsb = vol->sb;

	/* virtual objects: every mapping, including those kept for snapshots */
	ret = apfs_omap_list(&vol->omap, &om, &n);
	if(ret < 0)
		return ret;
	for(i = 0; i < n; i++)
		if(!(om[i].flags & OMAP_VAL_DELETED))
			ext_add(&l, om[i].paddr, om[i].size > nx->bs ? (om[i].size + nx->bs - 1) / nx->bs : 1);
	free(om);

	ret = collect_tree(&vol->omap.tree, &l, 0);
	ext_add(&l, vol->omap.paddr, 1);
	if(ret == APFS_OK)
		ret = collect_phys_tree(vol, LE64(APFS_OMAP_PHYS(&vol->omap)->om_snapshot_tree_oid), OBJECT_TYPE_OMAP_SNAPSHOT, &l, 0);
	if(ret == APFS_OK && vol->extref.loaded)
		ret = collect_tree(&vol->extref, &l, 1);
	if(ret == APFS_OK)
		ret = apfs_snapshot_list(vol, &snaps, &nsnaps);
	for(i = 0; ret == APFS_OK && i < nsnaps; i++) {
		ext_add(&l, snaps[i].sblock_oid, 1);
		ret = collect_phys_tree(vol, snaps[i].extentref_tree_oid, OBJECT_TYPE_BLOCKREFTREE, &l, 1);
	}
	apfs_snapshots_free(snaps, nsnaps);
	if(ret == APFS_OK && vol->snapmeta.loaded)
		ret = collect_tree(&vol->snapmeta, &l, 0);
	if(ret == APFS_OK && vol->fext.loaded)
		ret = collect_tree(&vol->fext, &l, 0);
	if(ret == APFS_OK && (LE32(vsb->apfs_doc_id_tree_type) & OBJ_STORAGETYPE_MASK) == OBJ_PHYSICAL)
		ret = collect_phys_tree(vol, LE64(vsb->apfs_doc_id_tree_oid), 0, &l, 0);
	if(ret == APFS_OK && (LE32(vsb->apfs_sec_root_tree_type) & OBJ_STORAGETYPE_MASK) == OBJ_PHYSICAL)
		ret = collect_phys_tree(vol, LE64(vsb->apfs_sec_root_tree_oid), 0, &l, 0);
	if(ret == APFS_OK)
		ret = collect_file_extents(vol, &l);
	if(ret < 0) {
		free(l.list);
		return ret;
	}

	/* free each block once: views of the same data overlap */
	if(l.count) {
		size_t k = 0;
		qsort(l.list, l.count, sizeof(apfs_extent_t), ext_cmp);
		for(i = 1; i < l.count; i++) {
			apfs_extent_t* cur = &l.list[k];
			if(l.list[i].start <= cur->start + cur->count) {
				paddr_t end = l.list[i].start + l.list[i].count;
				if(end > cur->start + cur->count)
					cur->count = end - cur->start;
			} else {
				l.list[++k] = l.list[i];
			}
		}
		l.count = k + 1;
	}
	for(i = 0; i < l.count && ret == APFS_OK; i++)
		ret = apfs_sm_free(nx, NULL, l.list[i].start, l.list[i].count);
	free(l.list);
	if(ret < 0)
		return ret;

	ret = apfs_omap_remove(&nx->omap, vol->oid, vol->xid);
	if(ret < 0 && ret != APFS_ENOENT)
		return ret;
	ret = apfs_sm_free(nx, NULL, vol->paddr, 1);
	if(ret < 0)
		return ret;
	if(!(LE64(vsb->apfs_fs_flags) & APFS_FS_UNENCRYPTED))
		APFS_WARN("the container keybag may still hold keys for the deleted volume");

	nx->sb->nx_fs_oid[index] = 0;
	vol->dirty = 0;
	apfs_vol_close(vol);
	nx->dirty = 1;
	return APFS_OK;
}
