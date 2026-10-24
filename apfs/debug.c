/*
 * debug.c
 * apfs-tool
 *
 * Human-readable dumps of container, volume and B-tree structures.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apfs/apfs.h"
#include "apfs/debug.h"

static const char* obj_type_name(uint32_t type) {
	switch(type & OBJECT_TYPE_MASK) {
		case OBJECT_TYPE_NX_SUPERBLOCK: return "NX_SUPERBLOCK";
		case OBJECT_TYPE_BTREE: return "BTREE";
		case OBJECT_TYPE_BTREE_NODE: return "BTREE_NODE";
		case OBJECT_TYPE_SPACEMAN: return "SPACEMAN";
		case OBJECT_TYPE_SPACEMAN_CAB: return "SPACEMAN_CAB";
		case OBJECT_TYPE_SPACEMAN_CIB: return "SPACEMAN_CIB";
		case OBJECT_TYPE_SPACEMAN_BITMAP: return "SPACEMAN_BITMAP";
		case OBJECT_TYPE_SPACEMAN_FREE_QUEUE: return "SPACEMAN_FREE_QUEUE";
		case OBJECT_TYPE_EXTENT_LIST_TREE: return "EXTENT_LIST_TREE";
		case OBJECT_TYPE_OMAP: return "OMAP";
		case OBJECT_TYPE_CHECKPOINT_MAP: return "CHECKPOINT_MAP";
		case OBJECT_TYPE_FS: return "FS";
		case OBJECT_TYPE_FSTREE: return "FSTREE";
		case OBJECT_TYPE_BLOCKREFTREE: return "BLOCKREFTREE";
		case OBJECT_TYPE_SNAPMETATREE: return "SNAPMETATREE";
		case OBJECT_TYPE_NX_REAPER: return "NX_REAPER";
		case OBJECT_TYPE_NX_REAP_LIST: return "NX_REAP_LIST";
		case OBJECT_TYPE_OMAP_SNAPSHOT: return "OMAP_SNAPSHOT";
		case OBJECT_TYPE_EFI_JUMPSTART: return "EFI_JUMPSTART";
		case OBJECT_TYPE_FUSION_MIDDLE_TREE: return "FUSION_MIDDLE_TREE";
		case OBJECT_TYPE_NX_FUSION_WBC: return "NX_FUSION_WBC";
		case OBJECT_TYPE_NX_FUSION_WBC_LIST: return "NX_FUSION_WBC_LIST";
		case OBJECT_TYPE_ER_STATE: return "ER_STATE";
		case OBJECT_TYPE_GBITMAP: return "GBITMAP";
		case OBJECT_TYPE_GBITMAP_TREE: return "GBITMAP_TREE";
		case OBJECT_TYPE_GBITMAP_BLOCK: return "GBITMAP_BLOCK";
		case OBJECT_TYPE_SNAP_META_EXT: return "SNAP_META_EXT";
		case OBJECT_TYPE_INTEGRITY_META: return "INTEGRITY_META";
		case OBJECT_TYPE_FEXT_TREE: return "FEXT_TREE";
		case 0: return "INVALID";
		default: return "?";
	}
}

static const char* storage_name(uint32_t type) {
	switch(type & OBJ_STORAGETYPE_MASK) {
		case OBJ_PHYSICAL: return "physical";
		case OBJ_EPHEMERAL: return "ephemeral";
		case OBJ_VIRTUAL: return "virtual";
		default: return "?";
	}
}

static const char* j_type_name(uint8_t t) {
	switch(t) {
		case APFS_TYPE_SNAP_METADATA: return "SNAP_METADATA";
		case APFS_TYPE_EXTENT: return "EXTENT";
		case APFS_TYPE_INODE: return "INODE";
		case APFS_TYPE_XATTR: return "XATTR";
		case APFS_TYPE_SIBLING_LINK: return "SIBLING_LINK";
		case APFS_TYPE_DSTREAM_ID: return "DSTREAM_ID";
		case APFS_TYPE_CRYPTO_STATE: return "CRYPTO_STATE";
		case APFS_TYPE_FILE_EXTENT: return "FILE_EXTENT";
		case APFS_TYPE_DIR_REC: return "DIR_REC";
		case APFS_TYPE_DIR_STATS: return "DIR_STATS";
		case APFS_TYPE_SNAP_NAME: return "SNAP_NAME";
		case APFS_TYPE_SIBLING_MAP: return "SIBLING_MAP";
		case APFS_TYPE_FILE_INFO: return "FILE_INFO";
		default: return "?";
	}
}

static void print_uuid(FILE* f, const uint8_t* u) {
	char s[37];
	apfs_uuid_format(u, s);
	fputs(s, f);
}

static void print_time(FILE* f, uint64_t ns) {
	fprintf(f, "%llu.%09llu", (unsigned long long)(ns / 1000000000ULL), (unsigned long long)(ns % 1000000000ULL));
}

static void hexdump(FILE* f, const uint8_t* p, size_t len, const char* indent) {
	size_t i;
	for(i = 0; i < len; i++) {
		if(i % 32 == 0)
			fprintf(f, "%s%04lx:", indent, (unsigned long)i);
		fprintf(f, " %02x", p[i]);
		if(i % 32 == 31 || i == len - 1)
			fputc('\n', f);
	}
}

void apfs_dump_container(apfs_nx_t* nx, FILE* f) {
	nx_superblock_t* sb = nx->sb;
	uint32_t i;
	int v;

	fprintf(f, "Container superblock (checkpoint xid %llu)\n", (unsigned long long)nx->xid);
	fprintf(f, "  block size:          %u\n", LE32(sb->nx_block_size));
	fprintf(f, "  block count:         %llu (%llu bytes)\n", (unsigned long long)LE64(sb->nx_block_count),
		(unsigned long long)LE64(sb->nx_block_count) * LE32(sb->nx_block_size));
	fprintf(f, "  features:            0x%llx ro_compat 0x%llx incompat 0x%llx\n", (unsigned long long)LE64(sb->nx_features),
		(unsigned long long)LE64(sb->nx_readonly_compatible_features), (unsigned long long)LE64(sb->nx_incompatible_features));
	fprintf(f, "  uuid:                ");
	print_uuid(f, sb->nx_uuid);
	fputc('\n', f);
	fprintf(f, "  next oid/xid:        0x%llx / %llu\n", (unsigned long long)LE64(sb->nx_next_oid), (unsigned long long)LE64(sb->nx_next_xid));
	fprintf(f, "  checkpoint desc:     base 0x%llx blocks %u next %u index %u len %u\n", (unsigned long long)LE64(sb->nx_xp_desc_base),
		LE32(sb->nx_xp_desc_blocks), LE32(sb->nx_xp_desc_next), LE32(sb->nx_xp_desc_index), LE32(sb->nx_xp_desc_len));
	fprintf(f, "  checkpoint data:     base 0x%llx blocks %u next %u index %u len %u\n", (unsigned long long)LE64(sb->nx_xp_data_base),
		LE32(sb->nx_xp_data_blocks), LE32(sb->nx_xp_data_next), LE32(sb->nx_xp_data_index), LE32(sb->nx_xp_data_len));
	fprintf(f, "  spaceman/omap/reaper: 0x%llx / 0x%llx / 0x%llx\n", (unsigned long long)LE64(sb->nx_spaceman_oid),
		(unsigned long long)LE64(sb->nx_omap_oid), (unsigned long long)LE64(sb->nx_reaper_oid));
	fprintf(f, "  max file systems:    %u\n", LE32(sb->nx_max_file_systems));
	fprintf(f, "  flags:               0x%llx\n", (unsigned long long)LE64(sb->nx_flags));
	fprintf(f, "  efi jumpstart:       0x%llx\n", (unsigned long long)LE64(sb->nx_efi_jumpstart));
	fprintf(f, "  keylocker:           0x%llx+%llu\n", (unsigned long long)LE64(sb->nx_keylocker.pr_start_paddr),
		(unsigned long long)LE64(sb->nx_keylocker.pr_block_count));
	fprintf(f, "  ephemeral info:      0x%llx 0x%llx 0x%llx 0x%llx\n", (unsigned long long)LE64(sb->nx_ephemeral_info[0]),
		(unsigned long long)LE64(sb->nx_ephemeral_info[1]), (unsigned long long)LE64(sb->nx_ephemeral_info[2]),
		(unsigned long long)LE64(sb->nx_ephemeral_info[3]));
	fprintf(f, "  newest mounted ver:  0x%llx\n", (unsigned long long)LE64(sb->nx_newest_mounted_version));
	fprintf(f, "  free blocks:         %llu\n", (unsigned long long)apfs_sm_free_blocks(nx));

	fprintf(f, "Ephemeral objects\n");
	for(i = 0; i < nx->neph; i++) {
		apfs_eph_t* e = &nx->eph[i];
		fprintf(f, "  oid 0x%llx %s/%s %s subtype %s size %u\n", (unsigned long long)e->oid, obj_type_name(e->type),
			storage_name(e->type), "", obj_type_name(e->subtype), e->size);
	}

	{
		apfs_eph_t* e = apfs_nx_eph(nx, LE64(sb->nx_spaceman_oid));
		if(e) {
			spaceman_phys_t* sm = (spaceman_phys_t*)e->data;
			int q;
			fprintf(f, "Space manager\n");
			fprintf(f, "  blocks/chunk %u chunks/cib %u cibs/cab %u\n", LE32(sm->sm_blocks_per_chunk), LE32(sm->sm_chunks_per_cib), LE32(sm->sm_cibs_per_cab));
			fprintf(f, "  main: blocks %llu chunks %llu cibs %u cabs %u free %llu addr_off 0x%x\n",
				(unsigned long long)LE64(sm->sm_dev[0].sm_block_count), (unsigned long long)LE64(sm->sm_dev[0].sm_chunk_count),
				LE32(sm->sm_dev[0].sm_cib_count), LE32(sm->sm_dev[0].sm_cab_count), (unsigned long long)LE64(sm->sm_dev[0].sm_free_count),
				LE32(sm->sm_dev[0].sm_addr_offset));
			fprintf(f, "  flags 0x%x ip_bm_tx_mult %u ip blocks %llu ip_bm_size %u ip_bm_block_count %u ip_bm_base 0x%llx ip_base 0x%llx\n",
				LE32(sm->sm_flags), LE32(sm->sm_ip_bm_tx_multiplier), (unsigned long long)LE64(sm->sm_ip_block_count),
				LE32(sm->sm_ip_bm_size_in_blocks), LE32(sm->sm_ip_bm_block_count), (unsigned long long)LE64(sm->sm_ip_bm_base),
				(unsigned long long)LE64(sm->sm_ip_base));
			fprintf(f, "  fs reserve %llu/%llu\n", (unsigned long long)LE64(sm->sm_fs_reserve_block_count), (unsigned long long)LE64(sm->sm_fs_reserve_alloc_count));
			for(q = 0; q < SFQ_COUNT; q++)
				fprintf(f, "  fq[%d]: count %llu tree 0x%llx oldest %llu node_limit %u\n", q, (unsigned long long)LE64(sm->sm_fq[q].sfq_count),
					(unsigned long long)LE64(sm->sm_fq[q].sfq_tree_oid), (unsigned long long)LE64(sm->sm_fq[q].sfq_oldest_xid),
					LE16(sm->sm_fq[q].sfq_tree_node_limit));
			fprintf(f, "  ip_bm_free head %u tail %u xid_off 0x%x bitmap_off 0x%x free_next_off 0x%x version %u struct_size 0x%x\n",
				LE16(sm->sm_ip_bm_free_head), LE16(sm->sm_ip_bm_free_tail), LE32(sm->sm_ip_bm_xid_offset), LE32(sm->sm_ip_bitmap_offset),
				LE32(sm->sm_ip_bm_free_next_offset), LE32(sm->sm_version), LE32(sm->sm_struct_size));
			if(apfs_verbosity >= 3)
				hexdump(f, e->data + 0x158, e->size - 0x158 > 512 ? 512 : e->size - 0x158, "    ");
		}
	}

	fprintf(f, "Volumes\n");
	for(v = 0; v < NX_MAX_FILE_SYSTEMS; v++) {
		if(sb->nx_fs_oid[v])
			fprintf(f, "  [%d] oid 0x%llx\n", v, (unsigned long long)LE64(sb->nx_fs_oid[v]));
	}
}

void apfs_dump_volume(apfs_vol_t* vol, FILE* f) {
	apfs_superblock_t* s = vol->sb;
	int i;

	fprintf(f, "Volume %d \"%s\"%s\n", vol->index, (const char*)s->apfs_volname, vol->snap_xid ? " (snapshot)" : "");
	fprintf(f, "  superblock oid 0x%llx at 0x%llx xid %llu\n", (unsigned long long)vol->oid, (unsigned long long)vol->paddr, (unsigned long long)vol->xid);
	fprintf(f, "  role:              %s (0x%x)\n", apfs_role_name(LE16(s->apfs_role)), LE16(s->apfs_role));
	fprintf(f, "  uuid:              ");
	print_uuid(f, s->apfs_vol_uuid);
	fputc('\n', f);
	fprintf(f, "  features:          0x%llx ro_compat 0x%llx incompat 0x%llx\n", (unsigned long long)LE64(s->apfs_features),
		(unsigned long long)LE64(s->apfs_readonly_compatible_features), (unsigned long long)LE64(s->apfs_incompatible_features));
	fprintf(f, "  fs flags:          0x%llx\n", (unsigned long long)LE64(s->apfs_fs_flags));
	fprintf(f, "  alloc count:       %llu (reserve %llu quota %llu)\n", (unsigned long long)LE64(s->apfs_fs_alloc_count),
		(unsigned long long)LE64(s->apfs_fs_reserve_block_count), (unsigned long long)LE64(s->apfs_fs_quota_block_count));
	fprintf(f, "  blocks alloced/freed: %llu / %llu\n", (unsigned long long)LE64(s->apfs_total_blocks_alloced), (unsigned long long)LE64(s->apfs_total_blocks_freed));
	fprintf(f, "  files %llu dirs %llu symlinks %llu other %llu snapshots %llu\n", (unsigned long long)LE64(s->apfs_num_files),
		(unsigned long long)LE64(s->apfs_num_directories), (unsigned long long)LE64(s->apfs_num_symlinks),
		(unsigned long long)LE64(s->apfs_num_other_fsobjects), (unsigned long long)LE64(s->apfs_num_snapshots));
	fprintf(f, "  next obj id:       %llu next doc id %u\n", (unsigned long long)LE64(s->apfs_next_obj_id), LE32(s->apfs_next_doc_id));
	fprintf(f, "  omap 0x%llx root tree 0x%llx (type 0x%x) extentref 0x%llx (0x%x) snapmeta 0x%llx (0x%x)\n",
		(unsigned long long)LE64(s->apfs_omap_oid), (unsigned long long)LE64(s->apfs_root_tree_oid), LE32(s->apfs_root_tree_type),
		(unsigned long long)LE64(s->apfs_extentref_tree_oid), LE32(s->apfs_extentref_tree_type),
		(unsigned long long)LE64(s->apfs_snap_meta_tree_oid), LE32(s->apfs_snap_meta_tree_type));
	fprintf(f, "  integrity meta 0x%llx fext tree 0x%llx (0x%x) er_state 0x%llx snap_meta_ext 0x%llx\n",
		(unsigned long long)LE64(s->apfs_integrity_meta_oid), (unsigned long long)LE64(s->apfs_fext_tree_oid),
		LE32(s->apfs_fext_tree_type), (unsigned long long)LE64(s->apfs_er_state_oid), (unsigned long long)LE64(s->apfs_snap_meta_ext_oid));
	if(vol->sealed) {
		uint8_t hash[APFS_HASH_MAX_SIZE];
		uint32_t type, size, i;
		xid_t broken;
		if(apfs_vol_seal_info(vol, &type, hash, &size, &broken) == APFS_OK) {
			fprintf(f, "  seal:              hash type %u, root hash ", type);
			for(i = 0; i < size; i++)
				fprintf(f, "%02x", hash[i]);
			if(broken)
				fprintf(f, " (broken by xid %llu)", (unsigned long long)broken);
			fprintf(f, "\n");
		}
	}
	fprintf(f, "  meta crypto:       v%u.%u flags 0x%x class %u os 0x%x rev %u\n", LE16(s->apfs_meta_crypto.major_version),
		LE16(s->apfs_meta_crypto.minor_version), LE32(s->apfs_meta_crypto.cpflags), LE32(s->apfs_meta_crypto.persistent_class),
		LE32(s->apfs_meta_crypto.key_os_version), LE16(s->apfs_meta_crypto.key_revision));
	fprintf(f, "  unmount time ");
	print_time(f, LE64(s->apfs_unmount_time));
	fprintf(f, " last mod ");
	print_time(f, LE64(s->apfs_last_mod_time));
	fputc('\n', f);
	fprintf(f, "  cloneinfo epoch %llu xid %llu doc_id_index xid %llu flags 0x%x\n", (unsigned long long)LE64(s->apfs_cloneinfo_id_epoch),
		(unsigned long long)LE64(s->apfs_cloneinfo_xid), (unsigned long long)LE64(s->apfs_doc_id_index_xid), LE32(s->apfs_doc_id_index_flags));
	fprintf(f, "  volume group:      ");
	print_uuid(f, s->apfs_volume_group_id);
	fputc('\n', f);
	fprintf(f, "  formatted by:      %.32s xid %llu\n", (const char*)s->apfs_formatted_by.id, (unsigned long long)LE64(s->apfs_formatted_by.last_xid));
	for(i = 0; i < APFS_MAX_HIST; i++) {
		if(s->apfs_modified_by[i].id[0])
			fprintf(f, "  modified by:       %.32s xid %llu\n", (const char*)s->apfs_modified_by[i].id, (unsigned long long)LE64(s->apfs_modified_by[i].last_xid));
	}
}

typedef struct {
	FILE* f;
	int records;
} dump_ctx_t;

static void dump_fs_record(apfs_btree_t* bt, const apfs_btrec_t* r, FILE* f) {
	uint8_t t;
	uint64_t id;
	if(r->klen < 8) {
		fprintf(f, "      (short key)\n");
		return;
	}
	t = apfs_key_type(r->k);
	id = apfs_key_id(r->k);
	fprintf(f, "      %s id 0x%llx", j_type_name(t), (unsigned long long)id);
	switch(t) {
		case APFS_TYPE_INODE:
			if(r->vlen >= 0x5c && r->vlen != 0xffff) {
				const uint8_t* v = r->v;
				fprintf(f, " parent %llu priv %llu iflags 0x%llx n %u prot %u gen %u bsd 0x%x uid %u gid %u mode 0%o",
					(unsigned long long)apfs_get64(v), (unsigned long long)apfs_get64(v + 8), (unsigned long long)apfs_get64(v + 0x30),
					apfs_get32(v + 0x38), apfs_get32(v + 0x3c), apfs_get32(v + 0x40), apfs_get32(v + 0x44), apfs_get32(v + 0x48),
					apfs_get32(v + 0x4c), apfs_get16(v + 0x50));
				if(r->vlen > 0x5c) {
					const uint8_t* blob = v + 0x5c;
					uint16_t n = apfs_get16(blob), i;
					uint32_t off = 4 + n * 4;
					fprintf(f, " xf[%u used %u]:", n, apfs_get16(blob + 2));
					for(i = 0; i < n; i++) {
						const uint8_t* h = blob + 4 + i * 4;
						uint16_t sz = apfs_get16(h + 2);
						fprintf(f, " (t%u f0x%x s%u", h[0], h[1], sz);
						if(h[0] == INO_EXT_TYPE_NAME)
							fprintf(f, " \"%.*s\"", sz, (const char*)blob + off);
						else if(h[0] == INO_EXT_TYPE_DSTREAM && sz >= 40)
							fprintf(f, " size %llu alloc %llu crypto %llu written %llu read %llu", (unsigned long long)apfs_get64(blob + off),
								(unsigned long long)apfs_get64(blob + off + 8), (unsigned long long)apfs_get64(blob + off + 16),
								(unsigned long long)apfs_get64(blob + off + 24), (unsigned long long)apfs_get64(blob + off + 32));
						else if(sz == 8 || sz == 4)
							fprintf(f, " val 0x%llx", (unsigned long long)(sz == 8 ? apfs_get64(blob + off) : apfs_get32(blob + off)));
						fputc(')', f);
						off += (sz + 7U) & ~7U;
					}
				}
			}
			break;
		case APFS_TYPE_DIR_REC:
			if(bt->vol && bt->vol->hashed_names && r->klen >= 12)
				fprintf(f, " hash 0x%06x len %u \"%.*s\"", apfs_get32(r->k + 8) >> 10, apfs_get32(r->k + 8) & 0x3ff, (int)(r->klen - 12), (const char*)r->k + 12);
			else if(r->klen >= 10)
				fprintf(f, " len %u \"%.*s\"", apfs_get16(r->k + 8), (int)(r->klen - 10), (const char*)r->k + 10);
			if(r->vlen >= 18 && r->vlen != 0xffff)
				fprintf(f, " -> %llu added %llu flags 0x%x%s", (unsigned long long)apfs_get64(r->v), (unsigned long long)apfs_get64(r->v + 8),
					apfs_get16(r->v + 16), r->vlen > 18 ? " +xfields" : "");
			break;
		case APFS_TYPE_XATTR:
			if(r->klen >= 10)
				fprintf(f, " \"%.*s\"", (int)(r->klen - 10), (const char*)r->k + 10);
			if(r->vlen >= 4 && r->vlen != 0xffff)
				fprintf(f, " flags 0x%x len %u", apfs_get16(r->v), apfs_get16(r->v + 2));
			break;
		case APFS_TYPE_FILE_EXTENT:
			if(r->klen >= 16 && r->vlen >= 24 && r->vlen != 0xffff)
				fprintf(f, " off 0x%llx len 0x%llx flags 0x%x phys 0x%llx crypto %llu", (unsigned long long)apfs_get64(r->k + 8),
					(unsigned long long)(apfs_get64(r->v) & J_FILE_EXTENT_LEN_MASK), (unsigned)(apfs_get64(r->v) >> 56),
					(unsigned long long)apfs_get64(r->v + 8), (unsigned long long)apfs_get64(r->v + 16));
			break;
		case APFS_TYPE_EXTENT:
			if(r->vlen >= 20 && r->vlen != 0xffff)
				fprintf(f, " len %llu kind %u owner 0x%llx refcnt %d", (unsigned long long)(apfs_get64(r->v) & PEXT_LEN_MASK),
					(unsigned)(apfs_get64(r->v) >> PEXT_KIND_SHIFT), (unsigned long long)apfs_get64(r->v + 8), (int32_t)apfs_get32(r->v + 16));
			break;
		case APFS_TYPE_DSTREAM_ID:
			if(r->vlen >= 4 && r->vlen != 0xffff)
				fprintf(f, " refcnt %u", apfs_get32(r->v));
			break;
		case APFS_TYPE_SIBLING_LINK:
			if(r->klen >= 16 && r->vlen >= 10 && r->vlen != 0xffff)
				fprintf(f, " sibling %llu parent %llu \"%.*s\"", (unsigned long long)apfs_get64(r->k + 8), (unsigned long long)apfs_get64(r->v),
					(int)apfs_get16(r->v + 8), (const char*)r->v + 10);
			break;
		case APFS_TYPE_SIBLING_MAP:
			if(r->vlen >= 8 && r->vlen != 0xffff)
				fprintf(f, " file %llu", (unsigned long long)apfs_get64(r->v));
			break;
		case APFS_TYPE_SNAP_METADATA:
			if(r->vlen >= 50 && r->vlen != 0xffff)
				fprintf(f, " extref 0x%llx sblock 0x%llx inum %llu flags 0x%x name \"%.*s\"", (unsigned long long)apfs_get64(r->v),
					(unsigned long long)apfs_get64(r->v + 8), (unsigned long long)apfs_get64(r->v + 32), apfs_get32(r->v + 44),
					(int)apfs_get16(r->v + 48), (const char*)r->v + 50);
			break;
		case APFS_TYPE_SNAP_NAME:
			if(r->klen >= 10)
				fprintf(f, " \"%.*s\"", (int)(r->klen - 10), (const char*)r->k + 10);
			if(r->vlen >= 8 && r->vlen != 0xffff)
				fprintf(f, " xid %llu", (unsigned long long)apfs_get64(r->v));
			break;
		case APFS_TYPE_DIR_STATS:
			if(r->vlen >= 32 && r->vlen != 0xffff)
				fprintf(f, " children %llu size %llu chained %llu gen %llu", (unsigned long long)apfs_get64(r->v),
					(unsigned long long)apfs_get64(r->v + 8), (unsigned long long)apfs_get64(r->v + 16), (unsigned long long)apfs_get64(r->v + 24));
			break;
		case APFS_TYPE_FILE_INFO:
			if(r->klen >= 16 && r->vlen >= 3 && r->vlen != 0xffff)
				fprintf(f, " lba 0x%llx type %u hashed_len %u hash_size %u", (unsigned long long)(apfs_get64(r->k + 8) & J_FILE_INFO_LBA_MASK),
					(unsigned)(apfs_get64(r->k + 8) >> 56), apfs_get16(r->v), r->v[2]);
			break;
		default:
			break;
	}
	fputc('\n', f);
}

static int dump_node_cb(apfs_btree_t* bt, apfs_btnode_t* n, int depth, void* ctx) {
	dump_ctx_t* d = (dump_ctx_t*)ctx;
	uint8_t* raw = (uint8_t*)apfs_malloc(bt->nx->bs);
	uint32_t i;
	int have_raw = 0;

	if(bt->storage != OBJ_EPHEMERAL && n->paddr)
		have_raw = apfs_read_blocks(bt->nx, n->paddr, 1, raw) == APFS_OK;
	fprintf(d->f, "%*snode oid 0x%llx paddr 0x%llx xid %llu level %u nrec %u flags 0x%x", depth * 2, "",
		(unsigned long long)n->oid, (unsigned long long)n->paddr, (unsigned long long)n->xid, n->level, n->nrec, n->flags);
	if(have_raw)
		fprintf(d->f, " toc %u+%u free %u+%u kfl %u+%u vfl %u+%u", apfs_get16(raw + 40), apfs_get16(raw + 42), apfs_get16(raw + 44),
			apfs_get16(raw + 46), apfs_get16(raw + 48), apfs_get16(raw + 50), apfs_get16(raw + 52), apfs_get16(raw + 54));
	fputc('\n', d->f);
	free(raw);

	if(!d->records)
		return APFS_OK;
	for(i = 0; i < n->nrec; i++) {
		apfs_btrec_t* r = &n->rec[i];
		if(n->level > 0) {
			oid_t c = 0;
			apfs_bt_child_oid(bt, r, &c);
			if(bt->subtype == OBJECT_TYPE_OMAP)
				fprintf(d->f, "%*s  [%u] oid 0x%llx xid %llu -> child 0x%llx\n", depth * 2, "", i, (unsigned long long)apfs_get64(r->k),
					(unsigned long long)apfs_get64(r->k + 8), (unsigned long long)c);
			else if(bt->subtype == OBJECT_TYPE_FSTREE || bt->subtype == OBJECT_TYPE_BLOCKREFTREE ||
					bt->subtype == OBJECT_TYPE_SNAPMETATREE) {
				fprintf(d->f, "%*s  [%u] -> child 0x%llx ", depth * 2, "", i, (unsigned long long)c);
				dump_fs_record(bt, r, d->f);
			}
			else
				fprintf(d->f, "%*s  [%u] klen %u -> child 0x%llx\n", depth * 2, "", i, r->klen, (unsigned long long)c);
			continue;
		}
		switch(bt->subtype) {
			case OBJECT_TYPE_OMAP:
				fprintf(d->f, "%*s  oid 0x%llx xid %llu -> flags 0x%x size %u paddr 0x%llx\n", depth * 2, "",
					(unsigned long long)apfs_get64(r->k), (unsigned long long)apfs_get64(r->k + 8), apfs_get32(r->v), apfs_get32(r->v + 4),
					(unsigned long long)apfs_get64(r->v + 8));
				break;
			case OBJECT_TYPE_FSTREE:
			case OBJECT_TYPE_BLOCKREFTREE:
			case OBJECT_TYPE_SNAPMETATREE:
				fprintf(d->f, "%*s", depth * 2, "");
				dump_fs_record(bt, r, d->f);
				break;
			case OBJECT_TYPE_FEXT_TREE:
				fprintf(d->f, "%*s  id 0x%llx off 0x%llx -> len 0x%llx phys 0x%llx\n", depth * 2, "", (unsigned long long)apfs_get64(r->k),
					(unsigned long long)apfs_get64(r->k + 8), (unsigned long long)(apfs_get64(r->v) & J_FILE_EXTENT_LEN_MASK),
					(unsigned long long)apfs_get64(r->v + 8));
				break;
			case OBJECT_TYPE_SPACEMAN_FREE_QUEUE:
				fprintf(d->f, "%*s  xid %llu paddr 0x%llx len %llu\n", depth * 2, "", (unsigned long long)apfs_get64(r->k),
					(unsigned long long)apfs_get64(r->k + 8), (unsigned long long)(r->vlen == 0xffff ? 1 : apfs_get64(r->v)));
				break;
			default:
				fprintf(d->f, "%*s  klen %u vlen %u\n", depth * 2, "", r->klen, r->vlen);
				hexdump(d->f, r->k, r->klen, "        ");
				break;
		}
	}
	return APFS_OK;
}

int apfs_dump_tree(apfs_btree_t* bt, const char* name, int records, FILE* f) {
	dump_ctx_t d;
	if(!bt->root) {
		fprintf(f, "%s: (none)\n", name);
		return APFS_OK;
	}
	fprintf(f, "%s: root 0x%llx flags 0x%x node_size %u key_size %u val_size %u longest_key %u longest_val %u keys %llu nodes %llu\n",
		name, (unsigned long long)bt->root, bt->info.bt_fixed.bt_flags, bt->info.bt_fixed.bt_node_size, bt->info.bt_fixed.bt_key_size,
		bt->info.bt_fixed.bt_val_size, bt->info.bt_longest_key, bt->info.bt_longest_val, (unsigned long long)bt->info.bt_key_count,
		(unsigned long long)bt->info.bt_node_count);
	d.f = f;
	d.records = records;
	return apfs_bt_walk(bt, dump_node_cb, &d);
}

int apfs_dump_free_queues(apfs_nx_t* nx, int records, FILE* f) {
	apfs_eph_t* e = apfs_nx_eph(nx, LE64(nx->sb->nx_spaceman_oid));
	spaceman_phys_t* sm;
	int q, ret = APFS_OK;
	if(!e)
		return APFS_ECORRUPT;
	sm = (spaceman_phys_t*)e->data;
	for(q = 0; q < SFQ_COUNT && ret == APFS_OK; q++) {
		apfs_btree_t bt;
		char name[32];
		oid_t root = LE64(sm->sm_fq[q].sfq_tree_oid);
		if(!root)
			continue;
		apfs_bt_init(&bt, nx, NULL, NULL, OBJ_EPHEMERAL, OBJECT_TYPE_SPACEMAN_FREE_QUEUE, root, apfs_cmp_u64pair);
		snprintf(name, sizeof(name), "free queue %d", q);
		ret = apfs_bt_load(&bt);
		if(ret == APFS_OK)
			ret = apfs_dump_tree(&bt, name, records, f);
		apfs_bt_close(&bt);
	}
	return ret;
}
