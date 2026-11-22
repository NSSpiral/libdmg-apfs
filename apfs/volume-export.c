/* Preserve physical addresses so snapshots and sealed trees remain valid. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apfs/apfs.h"
#include "apfs/image.h"
#include "apfs/platform.h"
#include "apfs/volume-export.h"

static int bit_get(const uint8_t* bm, uint64_t i) { return (bm[i >> 3] >> (i & 7)) & 1; }

static char* workspace_path(void) {
	char* path = plat_temp_path("volume-export");
	if(apfs_image_tmpdir) {
		const char* name = strrchr(path, '/');
#ifdef _WIN32
		const char* backslash = strrchr(path, '\\');
		if(backslash && (!name || backslash > name)) name = backslash;
#endif
		char* chosen = plat_join(apfs_image_tmpdir, name ? name + 1 : path);
		free(path);
		path = chosen;
	}
	return path;
}

static int clone_allocated(apfs_nx_t* nx, const char* path) {
	uint64_t blocks = LE64(nx->sb->nx_block_count), first;
	uint32_t bs = nx->bs, chunk = (1U << 20) / bs;
	uint8_t* buf;
	io_func* out;
	int ret = apfs_sm_load(nx);
	if(ret < 0) return ret;
	out = apfs_rawfile_create(path, blocks * bs);
	if(!out) return APFS_EIO;
	buf = (uint8_t*)apfs_malloc(1 << 20);
	for(first = 0; first < blocks; first += chunk) {
		uint32_t n = blocks - first < chunk ? (uint32_t)(blocks - first) : chunk, i;
		for(i = 0; i < n && !bit_get(nx->sm.bitmap, first + i); i++);
		if(i == n) continue;
		if(!nx->io->read(nx->io, (off_t)(nx->base + first * bs), (size_t)n * bs, buf)) {
			ret = APFS_EIO; break;
		}
		for(i = 0; i < n; i++)
			if(!bit_get(nx->sm.bitmap, first + i)) memset(buf + (size_t)i * bs, 0, bs);
		if(!out->write(out, (off_t)(first * bs), (size_t)n * bs, buf)) {
			ret = APFS_EIO; break;
		}
	}
	free(buf);
	CLOSE(out);
	return ret;
}

static int detach_siblings(apfs_nx_t* nx, int selected) {
	int i, ret = apfs_nx_begin(nx);
	if(ret < 0) return ret;
	for(i = 0; i < NX_MAX_FILE_SYSTEMS; i++) {
		apfs_omap_rec_t* records = NULL;
		size_t count = 0, j;
		oid_t oid = LE64(nx->sb->nx_fs_oid[i]);
		if(i == selected || !oid) continue;
		ret = apfs_omap_list(&nx->omap, &records, &count);
		if(ret < 0) return ret;
		for(j = 0; j < count; j++) {
			if(records[j].oid != oid) continue;
			ret = apfs_omap_remove(&nx->omap, oid, records[j].xid);
			if(ret < 0 && ret != APFS_ENOENT) break;
		}
		free(records);
		if(ret < 0 && ret != APFS_ENOENT) return ret;
		nx->sb->nx_fs_oid[i] = 0;
	}
	memset(&nx->sb->nx_keylocker, 0, sizeof(nx->sb->nx_keylocker));
	memset(&nx->sb->nx_mkb_locker, 0, sizeof(nx->sb->nx_mkb_locker));
	nx->dirty = 1;
	return APFS_OK;
}

static int release_orphans(apfs_nx_t* nx) {
	uint8_t *used = NULL, *pending = NULL;
	uint64_t i, blocks = LE64(nx->sb->nx_block_count);
	int ret = apfs_reachable_blocks(nx, &used, &pending);
	if(ret < 0) goto done;
	for(i = 0; i < blocks;) {
		uint64_t start;
		if(!bit_get(nx->sm.bitmap, i) || bit_get(used, i) || bit_get(pending, i)) {
			i++; continue;
		}
		start = i++;
		while(i < blocks && bit_get(nx->sm.bitmap, i) && !bit_get(used, i) && !bit_get(pending, i)) i++;
		ret = apfs_sm_free(nx, NULL, start, i - start);
		if(ret < 0) break;
	}
done:
	free(used);
	free(pending);
	return ret;
}

static int scrub_checkpoints(apfs_nx_t* nx) {
	nx_superblock_t* sb = nx->sb;
	uint32_t dc = LE32(sb->nx_xp_desc_blocks), ac = LE32(sb->nx_xp_data_blocks);
	uint32_t di = LE32(sb->nx_xp_desc_index), ai = LE32(sb->nx_xp_data_index);
	uint32_t dl = LE32(sb->nx_xp_desc_len), al = LE32(sb->nx_xp_data_len), i;
	uint8_t* zero = (uint8_t*)apfs_calloc(1, nx->bs);
	int ret = APFS_OK;
	for(i = 0; i < dc && ret == APFS_OK; i++)
		if((i + dc - di) % dc >= dl)
			ret = apfs_write_blocks(nx, LE64(sb->nx_xp_desc_base) + i, 1, zero);
	for(i = 0; i < ac && ret == APFS_OK; i++)
		if((i + ac - ai) % ac >= al)
			ret = apfs_write_blocks(nx, LE64(sb->nx_xp_data_base) + i, 1, zero);
	free(zero);
	return ret;
}

/* Pending frees can still contain data from removed volumes. */
static int scrub_pending(apfs_nx_t* nx) {
	uint8_t *used = NULL, *pending = NULL;
	uint8_t* zero;
	uint64_t i, blocks = LE64(nx->sb->nx_block_count);
	int ret = apfs_reachable_blocks(nx, &used, &pending);
	if(ret < 0) goto done;
	zero = (uint8_t*)apfs_calloc(1, nx->bs);
	for(i = 0; i < blocks && ret == APFS_OK; i++)
		if(bit_get(pending, i) && !bit_get(used, i))
			ret = apfs_write_blocks(nx, i, 1, zero);
	free(zero);
done:
	free(used);
	free(pending);
	return ret;
}

static int export_filtered_raw(apfs_nx_t* nx, const char* path) {
	uint64_t blocks = LE64(nx->sb->nx_block_count), first;
	uint32_t bs = nx->bs, chunk = (1U << 20) / bs;
	uint8_t* buf = (uint8_t*)apfs_malloc(1 << 20);
	FILE* out = fopen(path, "w+b");
	int ret = APFS_OK;
	if(!out) { free(buf); return APFS_EIO; }
	if(plat_sparse_file(out, blocks * bs) < 0) { fclose(out); free(buf); return APFS_EIO; }
	for(first = 0; first < blocks && ret == APFS_OK; first += chunk) {
		uint32_t n = blocks - first < chunk ? (uint32_t)(blocks - first) : chunk, i;
		for(i = 0; i < n && !bit_get(nx->sm.bitmap, first + i); i++);
		if(i == n) continue;
		if(apfs_read_blocks(nx, first, n, buf) < 0) { ret = APFS_EIO; break; }
		for(i = 0; i < n; i++)
			if(!bit_get(nx->sm.bitmap, first + i)) memset(buf + (size_t)i * bs, 0, bs);
		ret = plat_write_sparse(out, first * bs, buf, (size_t)n * bs);
	}
	if(ret == APFS_OK) ret = plat_sparse_finish(out);
	if(fclose(out) != 0 && ret == APFS_OK) ret = APFS_EIO;
	free(buf);
	return ret;
}

int apfs_export_volume(apfs_vol_t* source, const char* output, const char* format) {
	char *work = NULL, *raw = NULL, *outtmp = NULL;
	apfs_image_t* image = NULL;
	plat_stat_t st;
	uint8_t random[16];
	int ret = APFS_EINVAL, made_work = 0;
	if(!source || !output || !format || (strcmp(format, "dmg") && strcmp(format, "raw"))) return APFS_EINVAL;
	if(plat_lstat(output, &st) == APFS_OK) {
		fprintf(stderr, "error: volume export output already exists\n"); return APFS_EINVAL;
	}
	if(!(LE64(source->sb->apfs_fs_flags) & APFS_FS_UNENCRYPTED) || source->snap_xid) {
		fprintf(stderr, "error: select an unencrypted live volume\n"); return APFS_EUNSUPP;
	}
	work = workspace_path();
	if(plat_mkdir(work, 0700) < 0) { ret = APFS_EIO; goto done; }
	made_work = 1;
	raw = plat_join(work, "container.raw");
	ret = clone_allocated(source->nx, raw);
	if(ret < 0) goto done;
	ret = apfs_image_open(raw, 1, &image);
	if(ret < 0) goto done;
	ret = detach_siblings(image->nx, source->index);
	if(apfs_image_close(image, ret == APFS_OK) < 0) ret = APFS_EIO;
	image = NULL;
	if(ret < 0) goto done;
	ret = apfs_image_open(raw, 1, &image);
	if(ret < 0) goto done;
	ret = release_orphans(image->nx);
	if(apfs_image_close(image, ret == APFS_OK) < 0) ret = APFS_EIO;
	image = NULL;
	if(ret < 0) goto done;
	ret = apfs_image_open(raw, 1, &image);
	if(ret < 0) goto done;
	ret = scrub_checkpoints(image->nx);
	if(ret == APFS_OK) ret = scrub_pending(image->nx);
	if(ret == APFS_OK) ret = apfs_verify(image->nx, 0);
	if(ret < 0) goto done;
	apfs_uuid_generate(random);
	outtmp = (char*)malloc(strlen(output) + 40);
	if(!outtmp) { ret = APFS_ENOMEM; goto done; }
	sprintf(outtmp, "%s.tmp-%02x%02x%02x%02x%02x%02x%02x%02x", output,
		random[0], random[1], random[2], random[3], random[4], random[5], random[6], random[7]);
	if(!strcmp(format, "raw")) {
		ret = apfs_sm_load(image->nx);
		if(ret == APFS_OK) ret = export_filtered_raw(image->nx, outtmp);
	} else {
		ret = apfs_image_export(image, "dmg", outtmp, 0, NULL);
	}
	if(ret == APFS_OK) ret = plat_link(outtmp, output);
done:
	if(image) apfs_image_close(image, 0);
	if(outtmp) plat_remove(outtmp);
	if(made_work) {
		if(raw) plat_remove(raw);
		plat_remove(work);
	}
	free(work); free(raw); free(outtmp);
	if(ret < 0) fprintf(stderr, "error: volume export failed: %s\n", apfs_strerror(ret));
	return ret;
}
