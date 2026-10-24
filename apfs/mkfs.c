/*
 * mkfs.c
 * apfs-tool
 *
 * Creation of new containers. The container skeleton (checkpoint areas,
 * space manager, reaper, free queues and container object map) is written
 * directly as transaction 1; volumes are then added through the regular
 * transaction machinery.
 */

#include <stdlib.h>
#include <string.h>

#include "apfs/apfs.h"

#define SM_STRUCT_SIZE 0x9d8
#define MKFS_XID 1

#define SPACEMAN_OID OID_RESERVED_COUNT
#define REAPER_OID (SPACEMAN_OID + 1)
#define IP_FQ_OID (SPACEMAN_OID + 2)
#define MAIN_FQ_OID (SPACEMAN_OID + 3)
#define FIRST_FREE_OID (SPACEMAN_OID + 4)

static uint32_t desc_blocks_for(uint64_t n) {
	if(n < 131072)
		return 8;
	if(n < 262144)
		return 12;
	if(n < 13107200)
		return 20 + (uint32_t)(60 * ((n - 262144) / 131072) / 23);
	return 280;
}

static uint32_t data_blocks_for(uint64_t n) {
	uint64_t v;
	if(n < 4545)
		return 52;
	if(n < 13633)
		return 124;
	if(n < 36353)
		return 160 + 36 * (uint32_t)((n - 13633) / 4544);
	if(n < 131777)
		return 308 + 4 * (uint32_t)((n - 36353) / 4544);
	if(n < 262144)
		return 648 + 4 * (uint32_t)((n - 131777) / 4544);
	v = 992 + (n - 262144) / 256;
	return v > 32768 ? 32768 : (uint32_t)v;
}

/* Node limit of the internal pool free queue */
uint16_t apfs_ip_fq_node_limit(uint64_t chunks) {
	uint16_t ret = (uint16_t)(3 * (chunks + 751) / 1127 - 1);
	if(ret == 2)
		ret = 3;
	return ret;
}

/* Node limit of the main free queue */
uint16_t apfs_main_fq_node_limit(uint64_t blocks) {
	uint16_t ret;
	if(blocks < 0x40000)
		ret = (uint16_t)(1 + (blocks - 1) / 4544);
	else if(blocks < 0x100000)
		ret = (uint16_t)(116 + (blocks - 261281) / 2272);
	else
		ret = 512;
	if(ret == 2)
		ret = 3;
	return ret;
}

static uint32_t max_volumes_for(uint64_t bytes) {
	uint64_t v = (bytes + 512ULL * 1024 * 1024 - 1) / (512ULL * 1024 * 1024);
	if(v > NX_MAX_FILE_SYSTEMS)
		v = NX_MAX_FILE_SYSTEMS;
	if(v == 0)
		v = 1;
	return (uint32_t)v;
}

static void bm_set(uint8_t* bm, uint64_t start, uint64_t count) {
	uint64_t i;
	for(i = start; i < start + count; i++)
		bm[i >> 3] |= (uint8_t)(1 << (i & 7));
}

static void set_obj(void* buf, oid_t oid, xid_t xid, uint32_t type, uint32_t subtype) {
	obj_phys_t* o = (obj_phys_t*)buf;
	o->o_oid = LE64(oid);
	o->o_xid = LE64(xid);
	o->o_type = LE32(type);
	o->o_subtype = LE32(subtype);
}

static int write_blocks(io_func* io, uint64_t base, uint32_t bs, uint64_t bno, uint64_t count, const void* buf) {
	if(!io->write(io, (off_t)(base + bno * bs), (size_t)(count * bs), (void*)buf))
		return APFS_EIO;
	return APFS_OK;
}

static int write_object(io_func* io, uint64_t base, uint32_t bs, uint64_t bno, void* buf, uint32_t size) {
	apfs_obj_set_cksum(buf, size);
	return write_blocks(io, base, bs, bno, size / bs, buf);
}

/* Zeroes the blocks that do not read as zero already, so that a new image
 * file stays sparse */
static int clear_blocks(io_func* io, uint64_t base, uint32_t bs, uint64_t bno, uint64_t count, const uint8_t* zero) {
	uint32_t per = (1U << 20) / bs, n, i;
	uint8_t* buf = (uint8_t*)apfs_malloc((size_t)per * bs);
	int ret = APFS_OK;
	for(; count && ret == APFS_OK; bno += n, count -= n) {
		n = count < per ? (uint32_t)count : per;
		if(!io->read(io, (off_t)(base + bno * bs), (size_t)n * bs, buf)) {
			ret = APFS_EIO;
			break;
		}
		for(i = 0; i < n && ret == APFS_OK; i++)
			if(memcmp(buf + (size_t)i * bs, zero, bs))
				ret = write_blocks(io, base, bs, bno + i, 1, zero);
	}
	free(buf);
	return ret;
}

/* An empty B-tree root node with fixed-size records */
static void make_empty_fixed_root(uint8_t* buf, uint32_t bs, oid_t oid, uint32_t storage, uint32_t subtype,
		uint32_t bt_flags, uint32_t ksize, uint32_t vsize) {
	uint32_t toc = ((bs - 56) / (ksize + vsize + 4)) * 4;
	uint8_t* info = buf + bs - 40;
	memset(buf, 0, bs);
	set_obj(buf, oid, MKFS_XID, storage | OBJECT_TYPE_BTREE, subtype);
	apfs_put16(buf + 32, BTNODE_ROOT | BTNODE_LEAF | BTNODE_FIXED_KV_SIZE);
	apfs_put16(buf + 34, 0);
	apfs_put32(buf + 36, 0);
	apfs_put16(buf + 40, 0);
	apfs_put16(buf + 42, (uint16_t)toc);
	apfs_put16(buf + 44, 0);
	apfs_put16(buf + 46, (uint16_t)(bs - 56 - toc - 40));
	apfs_put16(buf + 48, BTOFF_INVALID);
	apfs_put16(buf + 50, 0);
	apfs_put16(buf + 52, BTOFF_INVALID);
	apfs_put16(buf + 54, 0);
	apfs_put32(info, bt_flags);
	apfs_put32(info + 4, bs);
	apfs_put32(info + 8, ksize);
	apfs_put32(info + 12, vsize);
	/* Fixed-width trees advertise their widths even before the first key.
	 * In particular, an empty free queue still has 16-byte keys. */
	apfs_put32(info + 16, ksize);
	apfs_put32(info + 20, vsize);
	apfs_put64(info + 24, 0);
	apfs_put64(info + 32, 1);
}

int apfs_mkfs(io_func* io, uint64_t base, const apfs_mkfs_params_t* params) {
	uint32_t bs = params->block_size ? params->block_size : NX_DEFAULT_BLOCK_SIZE;
	uint64_t nblocks = params->size / bs;
	uint32_t desc_blocks, data_blocks, bpc, cpc, cpcab, ipbm, ring, i;
	uint64_t nchunks, ncib, ncab, ip_blocks, used = 0;
	paddr_t desc_base, data_base, ring_base, ip_base, omap_bno, omap_root_bno, end;
	paddr_t cab_first, cib_first, bmap_first;
	uint64_t nbmaps, xp_data_used;
	uint32_t sm_size, sm_blocks, xid_off, bmoff_off, free_next_off, addr_off;
	uint8_t* bitmap;
	uint8_t* buf;
	uint8_t* sm_buf;
	uint8_t uuid[16];
	uint64_t now = apfs_time_now();
	int ret = APFS_OK, v;

	if(bs < NX_MINIMUM_BLOCK_SIZE || bs > NX_MAXIMUM_BLOCK_SIZE || (bs & (bs - 1)))
		return APFS_EINVAL;
	if(params->size < NX_MINIMUM_CONTAINER_SIZE) {
		APFS_ERR("containers must be at least %u bytes", NX_MINIMUM_CONTAINER_SIZE);
		return APFS_EINVAL;
	}

	desc_blocks = desc_blocks_for(nblocks);
	data_blocks = data_blocks_for(nblocks);
	bpc = bs * 8;
	cpc = (bs - (uint32_t)sizeof(chunk_info_block_t)) / (uint32_t)sizeof(chunk_info_t);
	cpcab = (bs - (uint32_t)sizeof(cib_addr_block_t)) / (uint32_t)sizeof(paddr_t);
	nchunks = (nblocks + bpc - 1) / bpc;
	ncib = (nchunks + cpc - 1) / cpc;
	ncab = (ncib + cpcab - 1) / cpcab;
	if(ncab == 1)
		ncab = 0;
	ip_blocks = (nchunks + ncib + ncab) * 3;
	ipbm = (uint32_t)((ip_blocks + bpc - 1) / bpc);
	ring = 16 * ipbm;
	if(ring > SPACEMAN_IP_BM_BLOCK_COUNT_MAX)
		return APFS_ERANGE;

	desc_base = 1;
	data_base = desc_base + desc_blocks;
	ring_base = data_base + data_blocks;
	ip_base = ring_base + ring;
	omap_bno = ip_base + ip_blocks;
	omap_root_bno = omap_bno + 1;
	end = omap_root_bno + 1;
	if(end + 16 > nblocks) {
		APFS_ERR("container is too small");
		return APFS_EINVAL;
	}

	/* internal pool contents: CABs, CIBs, then the bitmaps of used chunks */
	cab_first = ip_base;
	cib_first = cab_first + ncab;
	bmap_first = cib_first + ncib;
	nbmaps = (end - 1) / bpc + 1;
	if(ncab + ncib + nbmaps > ip_blocks)
		return APFS_ERANGE;

	/* space manager object layout */
	xid_off = SM_STRUCT_SIZE;
	bmoff_off = xid_off + 8 * ipbm;
	free_next_off = bmoff_off + ((2 * ipbm + 7) & ~7U);
	addr_off = free_next_off + ((2 * ring + 7) & ~7U);
	sm_size = addr_off + 8 * (uint32_t)(ncab ? ncab : ncib) + 8;
	sm_blocks = (sm_size + bs - 1) / bs;
	sm_size = sm_blocks * bs;
	xp_data_used = sm_blocks + 3; /* spaceman, reaper, two free queues */
	if(xp_data_used * 2 > data_blocks)
		return APFS_ERANGE;

	bitmap = (uint8_t*)apfs_calloc((size_t)nchunks, bs);
	bm_set(bitmap, 0, end);
	used = end;

	buf = (uint8_t*)apfs_calloc(1, bs);

	/* clear the metadata areas */
	ret = clear_blocks(io, base, bs, 0, end, buf);
	/* make sure the image covers the whole container */
	if(ret == APFS_OK)
		ret = write_blocks(io, base, bs, nblocks - 1, 1, buf);

	/* chunk bitmaps */
	for(i = 0; i < nbmaps && ret == APFS_OK; i++)
		ret = write_blocks(io, base, bs, bmap_first + i, 1, bitmap + (size_t)i * bs);

	/* chunk-info blocks */
	for(i = 0; i < ncib && ret == APFS_OK; i++) {
		chunk_info_block_t* cib = (chunk_info_block_t*)buf;
		uint64_t first = (uint64_t)i * cpc, c;
		uint32_t count = (uint32_t)((nchunks - first) > cpc ? cpc : (nchunks - first));
		memset(buf, 0, bs);
		set_obj(buf, cib_first + i, MKFS_XID, OBJ_PHYSICAL | OBJECT_TYPE_SPACEMAN_CIB, 0);
		cib->cib_index = LE32(i);
		cib->cib_chunk_info_count = LE32(count);
		for(c = 0; c < count; c++) {
			uint64_t chunk = first + c;
			uint64_t cstart = chunk * bpc;
			uint32_t blocks = (uint32_t)((nblocks - cstart) > bpc ? bpc : (nblocks - cstart));
			uint64_t usedc = 0, b;
			chunk_info_t* ci = &cib->cib_chunk_info[c];
			for(b = cstart; b < cstart + blocks && b < end; b++)
				usedc++;
			ci->ci_xid = LE64(MKFS_XID);
			ci->ci_addr = LE64(cstart);
			ci->ci_block_count = LE32(blocks);
			ci->ci_free_count = LE32((uint32_t)(blocks - usedc));
			ci->ci_bitmap_addr = LE64(chunk < nbmaps ? bmap_first + chunk : 0);
		}
		ret = write_object(io, base, bs, cib_first + i, buf, bs);
	}

	/* chunk-info address blocks */
	for(i = 0; i < ncab && ret == APFS_OK; i++) {
		cib_addr_block_t* cab = (cib_addr_block_t*)buf;
		uint64_t first = (uint64_t)i * cpcab, c;
		uint32_t count = (uint32_t)((ncib - first) > cpcab ? cpcab : (ncib - first));
		memset(buf, 0, bs);
		set_obj(buf, cab_first + i, MKFS_XID, OBJ_PHYSICAL | OBJECT_TYPE_SPACEMAN_CAB, 0);
		cab->cab_index = LE32(i);
		cab->cab_cib_count = LE32(count);
		for(c = 0; c < count; c++)
			cab->cab_cib_addr[c] = LE64(cib_first + first + c);
		ret = write_object(io, base, bs, cab_first + i, buf, bs);
	}

	/* internal pool bitmap, stored in ring slots 0..ipbm-1 */
	if(ret == APFS_OK) {
		uint8_t* ipbitmap = (uint8_t*)apfs_calloc(ipbm, bs);
		bm_set(ipbitmap, 0, ncab + ncib + nbmaps);
		for(i = 0; i < ipbm && ret == APFS_OK; i++)
			ret = write_blocks(io, base, bs, ring_base + i, 1, ipbitmap + (size_t)i * bs);
		free(ipbitmap);
	}

	/* space manager */
	sm_buf = (uint8_t*)apfs_calloc(1, sm_size);
	{
		spaceman_phys_t* sm = (spaceman_phys_t*)sm_buf;
		uint8_t* p = sm_buf;
		set_obj(sm_buf, SPACEMAN_OID, MKFS_XID, OBJ_EPHEMERAL | OBJECT_TYPE_SPACEMAN, 0);
		sm->sm_block_size = LE32(bs);
		sm->sm_blocks_per_chunk = LE32(bpc);
		sm->sm_chunks_per_cib = LE32(cpc);
		sm->sm_cibs_per_cab = LE32(cpcab);
		sm->sm_dev[SD_MAIN].sm_block_count = LE64(nblocks);
		sm->sm_dev[SD_MAIN].sm_chunk_count = LE64(nchunks);
		sm->sm_dev[SD_MAIN].sm_cib_count = LE32((uint32_t)ncib);
		sm->sm_dev[SD_MAIN].sm_cab_count = LE32((uint32_t)ncab);
		sm->sm_dev[SD_MAIN].sm_free_count = LE64(nblocks - used);
		sm->sm_dev[SD_MAIN].sm_addr_offset = LE32(addr_off);
		/* the (empty) tier 2 device's address array follows the main one */
		sm->sm_dev[SD_TIER2].sm_addr_offset = LE32(addr_off + 8 * (uint32_t)(ncab ? ncab : ncib));
		sm->sm_flags = LE32(SM_FLAG_VERSIONED);
		sm->sm_ip_bm_tx_multiplier = LE32(SPACEMAN_IP_BM_TX_MULTIPLIER);
		sm->sm_ip_block_count = LE64(ip_blocks);
		sm->sm_ip_bm_size_in_blocks = LE32(ipbm);
		sm->sm_ip_bm_block_count = LE32(ring);
		sm->sm_ip_bm_base = LE64(ring_base);
		sm->sm_ip_base = LE64(ip_base);
		sm->sm_fq[SFQ_IP].sfq_tree_oid = LE64(IP_FQ_OID);
		sm->sm_fq[SFQ_IP].sfq_tree_node_limit = LE16(apfs_ip_fq_node_limit(nchunks));
		sm->sm_fq[SFQ_MAIN].sfq_tree_oid = LE64(MAIN_FQ_OID);
		sm->sm_fq[SFQ_MAIN].sfq_tree_node_limit = LE16(apfs_main_fq_node_limit(nblocks));
		sm->sm_ip_bm_free_head = LE16((uint16_t)ipbm);
		sm->sm_ip_bm_free_tail = LE16((uint16_t)(ring - 1));
		sm->sm_ip_bm_xid_offset = LE32(xid_off);
		sm->sm_ip_bitmap_offset = LE32(bmoff_off);
		sm->sm_ip_bm_free_next_offset = LE32(free_next_off);
		sm->sm_version = LE32(1);
		sm->sm_struct_size = LE32(SM_STRUCT_SIZE);
		for(i = 0; i < ipbm; i++) {
			apfs_put64(p + xid_off + i * 8, MKFS_XID);
			apfs_put16(p + bmoff_off + i * 2, (uint16_t)i);
		}
		for(i = 0; i < ring; i++) {
			uint16_t next = SPACEMAN_IP_BM_INDEX_INVALID;
			if(i >= ipbm && i < ring - 1)
				next = (uint16_t)(i + 1);
			apfs_put16(p + free_next_off + i * 2, next);
		}
		for(i = 0; i < (ncab ? ncab : ncib); i++)
			apfs_put64(p + addr_off + i * 8, ncab ? cab_first + i : cib_first + i);
	}

	/* checkpoint data: spaceman, reaper, free queues */
	if(ret == APFS_OK)
		ret = write_object(io, base, bs, data_base, sm_buf, sm_size);
	if(ret == APFS_OK) {
		nx_reaper_phys_t* r = (nx_reaper_phys_t*)buf;
		memset(buf, 0, bs);
		set_obj(buf, REAPER_OID, MKFS_XID, OBJ_EPHEMERAL | OBJECT_TYPE_NX_REAPER, 0);
		r->nr_next_reap_id = LE64(1);
		r->nr_flags = LE32(NR_BHM_FLAG);
		r->nr_state_buffer_size = LE32(bs - (uint32_t)sizeof(nx_reaper_phys_t));
		ret = write_object(io, base, bs, data_base + sm_blocks, buf, bs);
	}
	if(ret == APFS_OK) {
		make_empty_fixed_root(buf, bs, IP_FQ_OID, OBJ_EPHEMERAL, OBJECT_TYPE_SPACEMAN_FREE_QUEUE,
				BTREE_SEQUENTIAL_INSERT | BTREE_ALLOW_GHOSTS | BTREE_EPHEMERAL, 16, 8);
		ret = write_object(io, base, bs, data_base + sm_blocks + 1, buf, bs);
	}
	if(ret == APFS_OK) {
		make_empty_fixed_root(buf, bs, MAIN_FQ_OID, OBJ_EPHEMERAL, OBJECT_TYPE_SPACEMAN_FREE_QUEUE,
				BTREE_SEQUENTIAL_INSERT | BTREE_ALLOW_GHOSTS | BTREE_EPHEMERAL, 16, 8);
		ret = write_object(io, base, bs, data_base + sm_blocks + 2, buf, bs);
	}

	/* container object map */
	if(ret == APFS_OK) {
		omap_phys_t* om = (omap_phys_t*)buf;
		memset(buf, 0, bs);
		set_obj(buf, omap_bno, MKFS_XID, OBJ_PHYSICAL | OBJECT_TYPE_OMAP, 0);
		om->om_flags = LE32(OMAP_MANUALLY_MANAGED);
		om->om_tree_type = LE32(OBJ_PHYSICAL | OBJECT_TYPE_BTREE);
		om->om_snapshot_tree_type = LE32(OBJ_PHYSICAL | OBJECT_TYPE_BTREE);
		om->om_tree_oid = LE64(omap_root_bno);
		ret = write_object(io, base, bs, omap_bno, buf, bs);
	}
	if(ret == APFS_OK) {
		make_empty_fixed_root(buf, bs, omap_root_bno, OBJ_PHYSICAL, OBJECT_TYPE_OMAP,
				BTREE_SEQUENTIAL_INSERT | BTREE_PHYSICAL, 16, 16);
		ret = write_object(io, base, bs, omap_root_bno, buf, bs);
	}

	/* checkpoint map and superblock */
	if(ret == APFS_OK) {
		checkpoint_map_phys_t* cpm = (checkpoint_map_phys_t*)buf;
		struct {
			uint32_t type;
			uint32_t subtype;
			uint32_t size;
			oid_t oid;
			paddr_t paddr;
		} maps[4];
		maps[0].type = OBJ_EPHEMERAL | OBJECT_TYPE_SPACEMAN; maps[0].subtype = 0; maps[0].size = sm_size;
		maps[0].oid = SPACEMAN_OID; maps[0].paddr = data_base;
		maps[1].type = OBJ_EPHEMERAL | OBJECT_TYPE_NX_REAPER; maps[1].subtype = 0; maps[1].size = bs;
		maps[1].oid = REAPER_OID; maps[1].paddr = data_base + sm_blocks;
		maps[2].type = OBJ_EPHEMERAL | OBJECT_TYPE_BTREE; maps[2].subtype = OBJECT_TYPE_SPACEMAN_FREE_QUEUE; maps[2].size = bs;
		maps[2].oid = IP_FQ_OID; maps[2].paddr = data_base + sm_blocks + 1;
		maps[3].type = OBJ_EPHEMERAL | OBJECT_TYPE_BTREE; maps[3].subtype = OBJECT_TYPE_SPACEMAN_FREE_QUEUE; maps[3].size = bs;
		maps[3].oid = MAIN_FQ_OID; maps[3].paddr = data_base + sm_blocks + 2;
		memset(buf, 0, bs);
		set_obj(buf, desc_base, MKFS_XID, OBJ_PHYSICAL | OBJECT_TYPE_CHECKPOINT_MAP, 0);
		cpm->cpm_flags = LE32(CHECKPOINT_MAP_LAST);
		cpm->cpm_count = LE32(4);
		for(i = 0; i < 4; i++) {
			cpm->cpm_map[i].cpm_type = LE32(maps[i].type);
			cpm->cpm_map[i].cpm_subtype = LE32(maps[i].subtype);
			cpm->cpm_map[i].cpm_size = LE32(maps[i].size);
			cpm->cpm_map[i].cpm_oid = LE64(maps[i].oid);
			cpm->cpm_map[i].cpm_paddr = LE64(maps[i].paddr);
		}
		ret = write_object(io, base, bs, desc_base, buf, bs);
	}
	if(ret == APFS_OK) {
		nx_superblock_t* sb = (nx_superblock_t*)buf;
		memset(buf, 0, bs);
		set_obj(buf, OID_NX_SUPERBLOCK, MKFS_XID, OBJ_EPHEMERAL | OBJECT_TYPE_NX_SUPERBLOCK, 0);
		sb->nx_magic = LE32(NX_MAGIC);
		sb->nx_block_size = LE32(bs);
		sb->nx_block_count = LE64(nblocks);
		sb->nx_incompatible_features = LE64(NX_INCOMPAT_VERSION2);
		if(params->uuid)
			memcpy(uuid, params->uuid, 16);
		else
			apfs_uuid_generate(uuid);
		memcpy(sb->nx_uuid, uuid, 16);
		sb->nx_next_oid = LE64(FIRST_FREE_OID);
		sb->nx_next_xid = LE64(MKFS_XID + 1);
		sb->nx_xp_desc_blocks = LE32(desc_blocks);
		sb->nx_xp_data_blocks = LE32(data_blocks);
		sb->nx_xp_desc_base = LE64(desc_base);
		sb->nx_xp_data_base = LE64(data_base);
		sb->nx_xp_desc_next = LE32(2);
		sb->nx_xp_data_next = LE32((uint32_t)xp_data_used);
		sb->nx_xp_desc_index = LE32(0);
		sb->nx_xp_desc_len = LE32(2);
		sb->nx_xp_data_index = LE32(0);
		sb->nx_xp_data_len = LE32((uint32_t)xp_data_used);
		sb->nx_spaceman_oid = LE64(SPACEMAN_OID);
		sb->nx_omap_oid = LE64(omap_bno);
		sb->nx_reaper_oid = LE64(REAPER_OID);
		sb->nx_max_file_systems = LE32(max_volumes_for(nblocks * bs));
		{
			uint64_t minblk = (nblocks * bs < 128ULL * 1024 * 1024) ? apfs_main_fq_node_limit(nblocks) : NX_EPH_MIN_BLOCK_COUNT;
			sb->nx_ephemeral_info[0] = LE64((minblk << 32) | ((uint64_t)NX_MAX_FILE_SYSTEM_EPH_STRUCTS << 16) | NX_EPH_INFO_VERSION_1);
		}
		ret = write_object(io, base, bs, desc_base + 1, buf, bs);
		if(ret == APFS_OK)
			ret = write_object(io, base, bs, 0, buf, bs);
	}

	free(sm_buf);
	free(buf);
	free(bitmap);
	if(ret < 0)
		return ret;

	/* volumes */
	if(params->nvols > 0) {
		apfs_nx_t* nx;
		ret = apfs_nx_open(&nx, io, base, 1);
		if(ret < 0)
			return ret;
		for(v = 0; v < params->nvols && ret == APFS_OK; v++) {
			int idx;
			ret = apfs_vol_create(nx, &params->vols[v], &idx);
		}
		if(ret == APFS_OK)
			ret = apfs_nx_commit(nx);
		else
			nx->txn = 0;
		apfs_nx_close(nx);
	}
	(void)now;
	return ret;
}
