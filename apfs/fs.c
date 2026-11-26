/*
 * fs.c
 * apfs-tool
 *
 * File-system tree records: inodes, directory entries, extended attributes
 * and path resolution.
 */

#include <stdlib.h>
#include <string.h>

#include "apfs/apfs.h"
#include "compat_macos.h"

#define INODE_FIXED_SIZE 0x5c

/* Inodes */

int apfs_inode_parse(uint64_t ino, const uint8_t* v, uint16_t vlen, apfs_inode_t* out) {
	memset(out, 0, sizeof(*out));
	if(vlen < INODE_FIXED_SIZE)
		return APFS_ECORRUPT;
	out->ino = ino;
	out->parent_id = apfs_get64(v + 0x00);
	out->private_id = apfs_get64(v + 0x08);
	out->create_time = apfs_get64(v + 0x10);
	out->mod_time = apfs_get64(v + 0x18);
	out->change_time = apfs_get64(v + 0x20);
	out->access_time = apfs_get64(v + 0x28);
	out->internal_flags = apfs_get64(v + 0x30);
	out->nlink = apfs_get32(v + 0x38);
	out->prot_class = apfs_get32(v + 0x3c);
	out->write_gen = apfs_get32(v + 0x40);
	out->bsd_flags = apfs_get32(v + 0x44);
	out->uid = apfs_get32(v + 0x48);
	out->gid = apfs_get32(v + 0x4c);
	out->mode = apfs_get16(v + 0x50);
	out->pad1 = apfs_get16(v + 0x52);
	out->uncompressed_size = apfs_get64(v + 0x54);

	if(vlen > INODE_FIXED_SIZE) {
		const uint8_t* blob = v + INODE_FIXED_SIZE;
		uint32_t avail = vlen - INODE_FIXED_SIZE;
		uint16_t n, i;
		uint32_t data_off;

		if(avail < 4)
			return APFS_ECORRUPT;
		n = apfs_get16(blob);
		data_off = 4 + (uint32_t)n * 4;
		if(n > APFS_MAX_XF || data_off > avail)
			return APFS_ECORRUPT;
		for(i = 0; i < n; i++) {
			apfs_xf_t* xf = &out->xf[i];
			const uint8_t* h = blob + 4 + i * 4;
			xf->type = h[0];
			xf->flags = h[1];
			xf->size = apfs_get16(h + 2);
			if(data_off + xf->size > avail) {
				out->nxf = i;
				apfs_inode_release(out);
				return APFS_ECORRUPT;
			}
			xf->data = (uint8_t*)apfs_malloc(xf->size ? xf->size : 1);
			memcpy(xf->data, blob + data_off, xf->size);
			data_off += (xf->size + 7U) & ~7U;
			out->nxf = i + 1;
		}
	}
	return APFS_OK;
}

static uint32_t inode_val_size(const apfs_inode_t* inode) {
	uint32_t size = INODE_FIXED_SIZE;
	int i;
	if(inode->nxf) {
		size += 4 + inode->nxf * 4;
		for(i = 0; i < inode->nxf; i++)
			size += (inode->xf[i].size + 7U) & ~7U;
	}
	return size;
}

static void inode_serialize(const apfs_inode_t* inode, uint8_t* v) {
	uint32_t total = inode_val_size(inode);
	memset(v, 0, total);
	apfs_put64(v + 0x00, inode->parent_id);
	apfs_put64(v + 0x08, inode->private_id);
	apfs_put64(v + 0x10, inode->create_time);
	apfs_put64(v + 0x18, inode->mod_time);
	apfs_put64(v + 0x20, inode->change_time);
	apfs_put64(v + 0x28, inode->access_time);
	apfs_put64(v + 0x30, inode->internal_flags);
	apfs_put32(v + 0x38, inode->nlink);
	apfs_put32(v + 0x3c, inode->prot_class);
	apfs_put32(v + 0x40, inode->write_gen);
	apfs_put32(v + 0x44, inode->bsd_flags);
	apfs_put32(v + 0x48, inode->uid);
	apfs_put32(v + 0x4c, inode->gid);
	apfs_put16(v + 0x50, inode->mode);
	apfs_put16(v + 0x52, inode->pad1);
	apfs_put64(v + 0x54, inode->uncompressed_size);
	if(inode->nxf) {
		uint8_t* blob = v + INODE_FIXED_SIZE;
		uint32_t data_off = 4 + inode->nxf * 4, used = 0;
		int i;
		apfs_put16(blob, (uint16_t)inode->nxf);
		for(i = 0; i < inode->nxf; i++) {
			const apfs_xf_t* xf = &inode->xf[i];
			uint8_t* h = blob + 4 + i * 4;
			uint32_t padded = (xf->size + 7U) & ~7U;
			h[0] = xf->type;
			h[1] = xf->flags;
			apfs_put16(h + 2, xf->size);
			memcpy(blob + data_off, xf->data, xf->size);
			data_off += padded;
			used += padded;
		}
		apfs_put16(blob + 2, (uint16_t)used);
	}
}

static void inode_key(uint8_t* key, uint64_t ino) {
	apfs_key_hdr(key, ino, APFS_TYPE_INODE);
}

int apfs_inode_read(apfs_vol_t* vol, uint64_t ino, apfs_inode_t* out) {
	uint8_t key[8];
	apfs_kv_t kv;
	int ret;

	inode_key(key, ino);
	ret = apfs_bt_lookup(&vol->fs, key, sizeof(key), &kv);
	if(ret < 0)
		return ret;
	ret = apfs_inode_parse(ino, kv.v, kv.vlen, out);
	if(ret < 0)
		APFS_ERR("inode %llu is corrupted", (unsigned long long)ino);
	return ret;
}

int apfs_inode_write(apfs_vol_t* vol, apfs_inode_t* inode) {
	uint8_t key[8];
	uint8_t* val;
	uint32_t size = inode_val_size(inode);
	int ret;

	if(size > 3900)
		return APFS_ERANGE;
	ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;
	val = (uint8_t*)apfs_malloc(size);
	inode_serialize(inode, val);
	inode_key(key, inode->ino);
	ret = apfs_bt_insert(&vol->fs, key, sizeof(key), val, (uint16_t)size);
	free(val);
	return ret;
}

void apfs_inode_release(apfs_inode_t* inode) {
	int i;
	for(i = 0; i < inode->nxf; i++) {
		free(inode->xf[i].data);
		inode->xf[i].data = NULL;
	}
	inode->nxf = 0;
}

int apfs_inode_copy(apfs_inode_t* dst, const apfs_inode_t* src) {
	int i;
	*dst = *src;
	for(i = 0; i < src->nxf; i++) {
		dst->xf[i].data = (uint8_t*)apfs_malloc(src->xf[i].size ? src->xf[i].size : 1);
		memcpy(dst->xf[i].data, src->xf[i].data, src->xf[i].size);
	}
	return APFS_OK;
}

const uint8_t* apfs_inode_xf(const apfs_inode_t* inode, uint8_t type, uint16_t* size) {
	int i;
	for(i = 0; i < inode->nxf; i++) {
		if(inode->xf[i].type == type) {
			if(size)
				*size = inode->xf[i].size;
			return inode->xf[i].data;
		}
	}
	return NULL;
}

int apfs_inode_xf_set(apfs_inode_t* inode, uint8_t type, uint8_t flags, const void* data, uint16_t size) {
	int i, pos;
	for(i = 0; i < inode->nxf; i++) {
		if(inode->xf[i].type == type) {
			free(inode->xf[i].data);
			inode->xf[i].data = (uint8_t*)apfs_malloc(size ? size : 1);
			memcpy(inode->xf[i].data, data, size);
			inode->xf[i].size = size;
			inode->xf[i].flags = flags;
			return APFS_OK;
		}
	}
	if(inode->nxf >= APFS_MAX_XF)
		return APFS_ERANGE;
	/* keep the fields sorted by type */
	for(pos = 0; pos < inode->nxf && inode->xf[pos].type < type; pos++)
		;
	memmove(&inode->xf[pos + 1], &inode->xf[pos], (inode->nxf - pos) * sizeof(apfs_xf_t));
	inode->xf[pos].type = type;
	inode->xf[pos].flags = flags;
	inode->xf[pos].size = size;
	inode->xf[pos].data = (uint8_t*)apfs_malloc(size ? size : 1);
	memcpy(inode->xf[pos].data, data, size);
	inode->nxf++;
	return APFS_OK;
}

void apfs_inode_xf_del(apfs_inode_t* inode, uint8_t type) {
	int i;
	for(i = 0; i < inode->nxf; i++) {
		if(inode->xf[i].type == type) {
			free(inode->xf[i].data);
			memmove(&inode->xf[i], &inode->xf[i + 1], (inode->nxf - i - 1) * sizeof(apfs_xf_t));
			inode->nxf--;
			return;
		}
	}
}

int apfs_inode_get_dstream(const apfs_inode_t* inode, apfs_dstream_t* ds) {
	uint16_t size;
	const uint8_t* d = apfs_inode_xf(inode, INO_EXT_TYPE_DSTREAM, &size);
	memset(ds, 0, sizeof(*ds));
	if(!d || size < sizeof(j_dstream_t))
		return APFS_ENOENT;
	ds->size = apfs_get64(d);
	ds->alloced_size = apfs_get64(d + 8);
	ds->crypto_id = apfs_get64(d + 16);
	ds->bytes_written = apfs_get64(d + 24);
	ds->bytes_read = apfs_get64(d + 32);
	return APFS_OK;
}

void apfs_inode_set_dstream(apfs_inode_t* inode, const apfs_dstream_t* ds) {
	uint8_t d[sizeof(j_dstream_t)];
	apfs_put64(d, ds->size);
	apfs_put64(d + 8, ds->alloced_size);
	apfs_put64(d + 16, ds->crypto_id);
	apfs_put64(d + 24, ds->bytes_written);
	apfs_put64(d + 32, ds->bytes_read);
	apfs_inode_xf_set(inode, INO_EXT_TYPE_DSTREAM, XF_SYSTEM_FIELD, d, sizeof(d));
}

const char* apfs_inode_name(const apfs_inode_t* inode) {
	uint16_t size;
	const uint8_t* d = apfs_inode_xf(inode, INO_EXT_TYPE_NAME, &size);
	if(!d || !size || d[size - 1] != 0)
		return NULL;
	return (const char*)d;
}

int apfs_inode_set_name(apfs_inode_t* inode, const char* name) {
	return apfs_inode_xf_set(inode, INO_EXT_TYPE_NAME, XF_DO_NOT_COPY, name, (uint16_t)(strlen(name) + 1));
}

uint8_t apfs_mode_to_dtype(uint16_t mode) {
	switch(mode & APFS_S_IFMT) {
		case APFS_S_IFIFO: return APFS_DT_FIFO;
		case APFS_S_IFCHR: return APFS_DT_CHR;
		case APFS_S_IFDIR: return APFS_DT_DIR;
		case APFS_S_IFBLK: return APFS_DT_BLK;
		case APFS_S_IFREG: return APFS_DT_REG;
		case APFS_S_IFLNK: return APFS_DT_LNK;
		case APFS_S_IFSOCK: return APFS_DT_SOCK;
		case APFS_S_IFWHT: return APFS_DT_WHT;
		default: return APFS_DT_UNKNOWN;
	}
}

/* Directory records */

/* Builds a directory record key. Returns its length or a negative error. */
int apfs_drec_key(apfs_vol_t* vol, uint64_t dir, const char* name, uint8_t* key) {
	size_t len = strlen(name);
	if(len > 255)
		return APFS_ENAMETOOLONG;
	apfs_key_hdr(key, dir, APFS_TYPE_DIR_REC);
	if(vol->hashed_names) {
		uint32_t hash = apfs_name_hash(name, len, vol->case_fold);
		apfs_put32(key + 8, (hash & J_DREC_HASH_MASK) | (uint32_t)(len + 1));
		memcpy(key + 12, name, len + 1);
		return (int)(12 + len + 1);
	}
	apfs_put16(key + 8, (uint16_t)(len + 1));
	memcpy(key + 10, name, len + 1);
	return (int)(10 + len + 1);
}

static const char* drec_key_name(apfs_vol_t* vol, const uint8_t* k, uint16_t klen, size_t* len) {
	uint32_t off = vol->hashed_names ? 12 : 10;
	if(klen <= off) {
		*len = 0;
		return "";
	}
	*len = strnlen((const char*)k + off, klen - off);
	return (const char*)k + off;
}

static void drec_parse(apfs_vol_t* vol, const apfs_kv_t* kv, apfs_dirent_t* out) {
	size_t nlen;
	const char* name = drec_key_name(vol, kv->k, kv->klen, &nlen);
	memset(out, 0, sizeof(*out));
	out->name = (char*)apfs_malloc(nlen + 1);
	memcpy(out->name, name, nlen);
	out->name[nlen] = 0;
	if(kv->vlen >= 18) {
		out->ino = apfs_get64(kv->v);
		out->date_added = apfs_get64(kv->v + 8);
		out->flags = apfs_get16(kv->v + 16);
	}
	if(kv->vlen > 18 + 4) {
		const uint8_t* blob = kv->v + 18;
		uint16_t n = apfs_get16(blob), i;
		uint32_t off = 4 + (uint32_t)n * 4;
		for(i = 0; i < n && 18 + off <= kv->vlen; i++) {
			const uint8_t* h = blob + 4 + i * 4;
			uint16_t sz = apfs_get16(h + 2);
			if(18 + off + sz > kv->vlen)
				break;
			if(h[0] == DREC_EXT_TYPE_SIBLING_ID && sz >= 8)
				out->sibling_id = apfs_get64(blob + off);
			off += (sz + 7U) & ~7U;
		}
	}
}

void apfs_dirent_release(apfs_dirent_t* d) {
	free(d->name);
	d->name = NULL;
}

void apfs_dirents_free(apfs_dirent_t* d, size_t count) {
	size_t i;
	for(i = 0; i < count; i++)
		free(d[i].name);
	free(d);
}

int apfs_dir_lookup(apfs_vol_t* vol, uint64_t dir, const char* name, apfs_dirent_t* out) {
	uint8_t key[12 + 256];
	int klen, ret;
	apfs_kv_t kv;

	klen = apfs_drec_key(vol, dir, name, key);
	if(klen < 0)
		return klen;

	ret = apfs_bt_lookup(&vol->fs, key, (uint16_t)klen, &kv);
	if(ret == APFS_OK) {
		if(out)
			drec_parse(vol, &kv, out);
		return APFS_OK;
	}
	if(ret != APFS_ENOENT || !vol->hashed_names)
		return ret;

	/* a canonically equivalent spelling has the same hash but different bytes */
	{
		apfs_btiter_t it;
		uint8_t start[13];
		uint32_t hash = apfs_get32(key + 8) & J_DREC_HASH_MASK;
		memcpy(start, key, 12);
		apfs_put32(start + 8, hash);
		start[12] = 0;
		ret = apfs_bt_iter_seek(&it, &vol->fs, start, sizeof(start));
		while(ret == APFS_OK && it.valid) {
			size_t nlen;
			const char* n;
			apfs_bt_iter_get(&it, &kv);
			if(kv.klen < 12 || apfs_key_id(kv.k) != dir || apfs_key_type(kv.k) != APFS_TYPE_DIR_REC ||
					(apfs_get32(kv.k + 8) & J_DREC_HASH_MASK) != hash)
				break;
			n = drec_key_name(vol, kv.k, kv.klen, &nlen);
			if(apfs_name_equal(n, nlen, name, strlen(name), 1, vol->case_fold) == 0) {
				if(out)
					drec_parse(vol, &kv, out);
				return APFS_OK;
			}
			ret = apfs_bt_iter_next(&it);
		}
		return ret < 0 ? ret : APFS_ENOENT;
	}
}

int apfs_dir_list(apfs_vol_t* vol, uint64_t dir, apfs_dirent_t** out, size_t* count) {
	apfs_btiter_t it;
	apfs_kv_t kv;
	uint8_t start[8];
	apfs_dirent_t* list = NULL;
	size_t n = 0, cap = 0;
	int ret;

	apfs_key_hdr(start, dir, APFS_TYPE_DIR_REC);
	ret = apfs_bt_iter_seek(&it, &vol->fs, start, sizeof(start));
	while(ret == APFS_OK && it.valid) {
		apfs_bt_iter_get(&it, &kv);
		if(apfs_key_id(kv.k) != dir || apfs_key_type(kv.k) != APFS_TYPE_DIR_REC)
			break;
		if(n == cap) {
			cap = cap ? cap * 2 : 32;
			list = (apfs_dirent_t*)apfs_realloc(list, cap * sizeof(apfs_dirent_t));
		}
		drec_parse(vol, &kv, &list[n++]);
		ret = apfs_bt_iter_next(&it);
	}
	if(ret < 0) {
		apfs_dirents_free(list, n);
		return ret;
	}
	*out = list;
	*count = n;
	return APFS_OK;
}

/* Extended attributes */

static int xattr_key(uint8_t* key, uint64_t ino, const char* name) {
	size_t len = strlen(name);
	if(len == 0 || len > 255)
		return APFS_EINVAL;
	apfs_key_hdr(key, ino, APFS_TYPE_XATTR);
	apfs_put16(key + 8, (uint16_t)(len + 1));
	memcpy(key + 10, name, len + 1);
	return (int)(10 + len + 1);
}

void apfs_names_free(char** names, size_t count) {
	size_t i;
	for(i = 0; i < count; i++)
		free(names[i]);
	free(names);
}

int apfs_xattr_list(apfs_vol_t* vol, uint64_t ino, char*** names, size_t* count) {
	apfs_btiter_t it;
	apfs_kv_t kv;
	uint8_t start[8];
	char** list = NULL;
	size_t n = 0;
	int ret;

	apfs_key_hdr(start, ino, APFS_TYPE_XATTR);
	ret = apfs_bt_iter_seek(&it, &vol->fs, start, sizeof(start));
	while(ret == APFS_OK && it.valid) {
		size_t len;
		apfs_bt_iter_get(&it, &kv);
		if(apfs_key_id(kv.k) != ino || apfs_key_type(kv.k) != APFS_TYPE_XATTR)
			break;
		if(kv.klen > 10) {
			len = strnlen((const char*)kv.k + 10, kv.klen - 10);
			list = (char**)apfs_realloc(list, (n + 1) * sizeof(char*));
			list[n] = (char*)apfs_malloc(len + 1);
			memcpy(list[n], kv.k + 10, len);
			list[n][len] = 0;
			n++;
		}
		ret = apfs_bt_iter_next(&it);
	}
	if(ret < 0) {
		apfs_names_free(list, n);
		return ret;
	}
	*names = list;
	*count = n;
	return APFS_OK;
}

/* Reads the raw value of an xattr record */
int apfs_xattr_raw(apfs_vol_t* vol, uint64_t ino, const char* name, uint16_t* flags, uint8_t** xdata, uint16_t* xlen) {
	uint8_t key[10 + 256];
	apfs_kv_t kv;
	int klen, ret;

	klen = xattr_key(key, ino, name);
	if(klen < 0)
		return klen;
	ret = apfs_bt_lookup(&vol->fs, key, (uint16_t)klen, &kv);
	if(ret < 0)
		return ret;
	if(kv.vlen < 4 || apfs_get16(kv.v + 2) > kv.vlen - 4)
		return APFS_ECORRUPT;
	*flags = apfs_get16(kv.v);
	*xlen = apfs_get16(kv.v + 2);
	*xdata = (uint8_t*)apfs_malloc(*xlen ? *xlen : 1);
	memcpy(*xdata, kv.v + 4, *xlen);
	return APFS_OK;
}

int apfs_xattr_get(apfs_vol_t* vol, uint64_t ino, const char* name, uint8_t** data, size_t* size) {
	uint16_t flags, xlen;
	uint8_t* x;
	int ret;

	ret = apfs_xattr_raw(vol, ino, name, &flags, &x, &xlen);
	if(ret < 0)
		return ret;
	if(flags & XATTR_DATA_EMBEDDED) {
		*data = x;
		*size = xlen;
		return APFS_OK;
	}
	if(flags & XATTR_DATA_STREAM) {
		uint64_t id, len;
		uint8_t* buf;
		if(xlen < sizeof(j_xattr_dstream_t)) {
			free(x);
			return APFS_ECORRUPT;
		}
		id = apfs_get64(x);
		len = apfs_get64(x + 8);
		free(x);
		if(len > ((uint64_t)1 << 32))
			return APFS_ERANGE;
		buf = (uint8_t*)apfs_malloc(len ? (size_t)len : 1);
		ret = apfs_dstream_read(vol, id, len, 0, buf, (size_t)len);
		if(ret < 0) {
			free(buf);
			return ret;
		}
		*data = buf;
		*size = (size_t)len;
		return APFS_OK;
	}
	free(x);
	return APFS_ECORRUPT;
}

/* Path resolution */

int apfs_readlink(apfs_vol_t* vol, uint64_t ino, char** target) {
	uint8_t* data;
	size_t size;
	int ret = apfs_xattr_get(vol, ino, SYMLINK_EA_NAME, &data, &size);
	if(ret < 0)
		return ret;
	*target = (char*)apfs_realloc(data, size + 1);
	(*target)[size] = 0;
	return APFS_OK;
}

static int lookup_internal(apfs_vol_t* vol, const char* path, int follow, uint64_t start_dir, int depth,
		uint64_t* ino_out, uint64_t* parent_out, char** base_out) {
	uint64_t cur = start_dir, parent = start_dir;
	char* copy;
	char* p;
	char* last = NULL;
	int ret = APFS_OK;

	if(depth > 32)
		return APFS_ELOOP;
	if(path[0] == '/')
		cur = parent = ROOT_DIR_INO_NUM;

	copy = apfs_strdup(path);
	p = copy;
	while(*p) {
		char* comp;
		char* end;
		apfs_dirent_t de;
		int is_last;

		while(*p == '/')
			p++;
		if(!*p)
			break;
		comp = p;
		end = strchr(p, '/');
		if(end) {
			*end = 0;
			p = end + 1;
		} else {
			p += strlen(p);
		}
		{
			const char* q = p;
			while(*q == '/')
				q++;
			is_last = (*q == 0);
		}

		if(strcmp(comp, ".") == 0) {
			if(is_last)
				last = NULL;
			continue;
		}
		if(strcmp(comp, "..") == 0) {
			apfs_inode_t di;
			if(cur != ROOT_DIR_INO_NUM) {
				ret = apfs_inode_read(vol, cur, &di);
				if(ret < 0)
					break;
				cur = di.parent_id;
				apfs_inode_release(&di);
			}
			parent = cur;
			last = NULL;
			continue;
		}

		ret = apfs_dir_lookup(vol, cur, comp, &de);
		if(ret < 0) {
			if(ret == APFS_ENOENT && is_last) {
				/* report the parent and the missing name */
				parent = cur;
				last = comp;
			}
			break;
		}
		parent = cur;
		last = comp;

		if((de.flags & DREC_TYPE_MASK) == APFS_DT_LNK && (follow || !is_last)) {
			char* target;
			uint64_t resolved;
			ret = apfs_readlink(vol, de.ino, &target);
			apfs_dirent_release(&de);
			if(ret < 0)
				break;
			ret = lookup_internal(vol, target, 1, cur, depth + 1, &resolved, NULL, NULL);
			free(target);
			if(ret < 0)
				break;
			cur = resolved;
			continue;
		}
		if(!is_last && (de.flags & DREC_TYPE_MASK) != APFS_DT_DIR) {
			apfs_dirent_release(&de);
			ret = APFS_ENOTDIR;
			break;
		}
		cur = de.ino;
		apfs_dirent_release(&de);
	}

	if(parent_out)
		*parent_out = parent;
	if(base_out)
		*base_out = last ? apfs_strdup(last) : NULL;
	if(ret == APFS_OK && ino_out)
		*ino_out = cur;
	free(copy);
	return ret;
}

/* Resolves a path. On APFS_ENOENT for the last component, *parent and
 * *basename still describe where the entry would be created. */
int apfs_lookup(apfs_vol_t* vol, const char* path, int follow, uint64_t* ino, uint64_t* parent, char** basename) {
	return lookup_internal(vol, path, follow, ROOT_DIR_INO_NUM, 0, ino, parent, basename);
}

/* Modifications */

static int check_name(const char* name) {
	size_t len = strlen(name);
	if(len == 0 || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, ".."))
		return APFS_EINVAL;
	if(len > 255)
		return APFS_ENAMETOOLONG;
	if(!apfs_utf8_valid(name, len))
		return APFS_EINVAL;
	return APFS_OK;
}

static void count_object(apfs_vol_t* vol, uint16_t mode, int delta) {
	apfs_superblock_t* s = vol->sb;
	uint64_t* field;
	switch(mode & APFS_S_IFMT) {
		case APFS_S_IFREG: field = &s->apfs_num_files; break;
		case APFS_S_IFDIR: field = &s->apfs_num_directories; break;
		case APFS_S_IFLNK: field = &s->apfs_num_symlinks; break;
		default: field = &s->apfs_num_other_fsobjects; break;
	}
	*field = LE64(LE64(*field) + (uint64_t)(int64_t)delta);
	vol->dirty = 1;
}

static int drec_insert(apfs_vol_t* vol, uint64_t parent, const char* name, uint64_t ino, uint16_t dtype,
		uint64_t date_added, uint64_t sibling_id) {
	uint8_t key[12 + 256];
	uint8_t val[18 + 4 + 4 + 8];
	uint16_t vlen = 18;
	int klen = apfs_drec_key(vol, parent, name, key);
	if(klen < 0)
		return klen;
	memset(val, 0, sizeof(val));
	apfs_put64(val, ino);
	apfs_put64(val + 8, date_added);
	apfs_put16(val + 16, dtype);
	if(sibling_id) {
		apfs_put16(val + 18, 1);
		apfs_put16(val + 20, 8);
		val[22] = DREC_EXT_TYPE_SIBLING_ID;
		val[23] = 0;
		apfs_put16(val + 24, 8);
		apfs_put64(val + 26, sibling_id);
		vlen = sizeof(val);
	}
	return apfs_bt_insert(&vol->fs, key, (uint16_t)klen, val, vlen);
}

/* Deletes the record of an existing entry, spelled as it is stored */
static int drec_delete(apfs_vol_t* vol, uint64_t parent, const char* stored_name) {
	uint8_t key[12 + 256];
	int klen = apfs_drec_key(vol, parent, stored_name, key);
	if(klen < 0)
		return klen;
	return apfs_bt_delete(&vol->fs, key, (uint16_t)klen);
}

static int parent_update(apfs_vol_t* vol, uint64_t parent, int delta, uint64_t now) {
	apfs_inode_t p;
	int ret = apfs_inode_read(vol, parent, &p);
	if(ret < 0)
		return ret;
	if((p.mode & APFS_S_IFMT) != APFS_S_IFDIR) {
		apfs_inode_release(&p);
		return APFS_ENOTDIR;
	}
	if(delta < 0 && p.nlink == 0)
		APFS_WARN("directory %llu child count underflow", (unsigned long long)parent);
	else
		p.nlink = (uint32_t)((int64_t)p.nlink + delta);
	p.mod_time = now;
	p.change_time = now;
	p.write_gen++;
	ret = apfs_inode_write(vol, &p);
	apfs_inode_release(&p);
	return ret;
}

static int dstream_id_set(apfs_vol_t* vol, uint64_t id, uint32_t refcnt) {
	uint8_t key[8], val[4];
	apfs_key_hdr(key, id, APFS_TYPE_DSTREAM_ID);
	apfs_put32(val, refcnt);
	return apfs_bt_insert(&vol->fs, key, sizeof(key), val, sizeof(val));
}

static int dstream_id_get(apfs_vol_t* vol, uint64_t id, uint32_t* refcnt) {
	uint8_t key[8];
	apfs_kv_t kv;
	int ret;
	apfs_key_hdr(key, id, APFS_TYPE_DSTREAM_ID);
	ret = apfs_bt_lookup(&vol->fs, key, sizeof(key), &kv);
	if(ret < 0)
		return ret;
	*refcnt = kv.vlen >= 4 ? apfs_get32(kv.v) : 0;
	return APFS_OK;
}

static int dstream_id_delete(apfs_vol_t* vol, uint64_t id) {
	uint8_t key[8];
	apfs_key_hdr(key, id, APFS_TYPE_DSTREAM_ID);
	return apfs_bt_delete(&vol->fs, key, sizeof(key));
}

int apfs_dstream_id_ensure(apfs_vol_t* vol, uint64_t id) {
	uint32_t rc;
	if(dstream_id_get(vol, id, &rc) == APFS_OK)
		return APFS_OK;
	return dstream_id_set(vol, id, 1);
}

/* Drops the inode's reference to its data stream */
int apfs_inode_drop_dstream(apfs_vol_t* vol, apfs_inode_t* inode) {
	uint32_t rc = 0;
	int ret;

	if(!apfs_inode_xf(inode, INO_EXT_TYPE_DSTREAM, NULL))
		return APFS_OK;
	ret = dstream_id_get(vol, inode->private_id, &rc);
	if(ret < 0 && ret != APFS_ENOENT)
		return ret;
	if(ret == APFS_OK && rc > 1) {
		ret = dstream_id_set(vol, inode->private_id, rc - 1);
	} else {
		ret = apfs_dstream_release(vol, inode->private_id);
		if(ret == APFS_OK && rc)
			ret = dstream_id_delete(vol, inode->private_id);
	}
	if(ret < 0)
		return ret;
	apfs_inode_xf_del(inode, INO_EXT_TYPE_DSTREAM);
	apfs_inode_xf_del(inode, INO_EXT_TYPE_SPARSE_BYTES);
	inode->internal_flags &= ~(uint64_t)INODE_IS_SPARSE;
	return APFS_OK;
}

static int xattr_val_release(apfs_vol_t* vol, const uint8_t* v, uint16_t vlen) {
	uint16_t flags;
	if(vlen < 4)
		return APFS_OK;
	flags = apfs_get16(v);
	if((flags & XATTR_DATA_STREAM) && vlen >= 4 + sizeof(j_xattr_dstream_t))
		return apfs_dstream_release(vol, apfs_get64(v + 4));
	return APFS_OK;
}

/* Removes every record that belongs to an inode and its data, except the
 * inode record itself */
static int inode_destroy(apfs_vol_t* vol, apfs_inode_t* inode) {
	apfs_btiter_t it;
	apfs_kv_t kv;
	uint8_t start[8];
	uint8_t** keys = NULL;
	uint16_t* klens = NULL;
	size_t n = 0, i;
	int ret;

	ret = apfs_inode_drop_dstream(vol, inode);
	if(ret < 0)
		return ret;

	/* collect the remaining records of this object (xattrs, siblings...) */
	apfs_key_hdr(start, inode->ino, APFS_TYPE_ANY);
	ret = apfs_bt_iter_seek(&it, &vol->fs, start, sizeof(start));
	while(ret == APFS_OK && it.valid) {
		uint8_t t;
		apfs_bt_iter_get(&it, &kv);
		if(apfs_key_id(kv.k) != inode->ino)
			break;
		t = apfs_key_type(kv.k);
		if(t != APFS_TYPE_DIR_REC && t != APFS_TYPE_INODE) {
			keys = (uint8_t**)apfs_realloc(keys, (n + 1) * sizeof(uint8_t*));
			klens = (uint16_t*)apfs_realloc(klens, (n + 1) * sizeof(uint16_t));
			keys[n] = (uint8_t*)apfs_malloc(kv.klen);
			memcpy(keys[n], kv.k, kv.klen);
			klens[n] = kv.klen;
			n++;
		}
		ret = apfs_bt_iter_next(&it);
	}

	for(i = 0; i < n && ret == APFS_OK; i++) {
		uint8_t t = apfs_key_type(keys[i]);
		if(t == APFS_TYPE_XATTR && apfs_bt_lookup(&vol->fs, keys[i], klens[i], &kv) == APFS_OK) {
			uint8_t* copy = (uint8_t*)apfs_malloc(kv.vlen + 1);
			uint16_t len = kv.vlen;
			memcpy(copy, kv.v, len);
			ret = xattr_val_release(vol, copy, len);
			free(copy);
		} else if(t == APFS_TYPE_SIBLING_LINK && klens[i] >= 16) {
			uint8_t mkey[8];
			apfs_key_hdr(mkey, apfs_get64(keys[i] + 8), APFS_TYPE_SIBLING_MAP);
			apfs_bt_delete(&vol->fs, mkey, sizeof(mkey));
		}
		if(ret == APFS_OK) {
			ret = apfs_bt_delete(&vol->fs, keys[i], klens[i]);
			if(ret == APFS_ENOENT)
				ret = APFS_OK;
		}
	}
	for(i = 0; i < n; i++)
		free(keys[i]);
	free(keys);
	free(klens);
	if(ret < 0)
		return ret;

	count_object(vol, inode->mode, -1);
	return APFS_OK;
}

int apfs_create(apfs_vol_t* vol, uint64_t parent, const char* name, const apfs_create_t* attr, uint64_t* ino_out) {
	apfs_inode_t inode, dir;
	uint64_t ino, now;
	uint16_t type = attr->mode & APFS_S_IFMT;
	int ret;

	ret = check_name(name);
	if(ret < 0)
		return ret;
	if(type == 0 || type == APFS_S_IFWHT)
		return APFS_EINVAL;
	ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;

	ret = apfs_inode_read(vol, parent, &dir);
	if(ret < 0)
		return ret;
	if((dir.mode & APFS_S_IFMT) != APFS_S_IFDIR) {
		apfs_inode_release(&dir);
		return APFS_ENOTDIR;
	}
	apfs_inode_release(&dir);
	if(apfs_dir_lookup(vol, parent, name, NULL) == APFS_OK)
		return APFS_EEXIST;

	now = attr->time ? attr->time : apfs_time_now();
	ino = apfs_vol_next_id(vol);
	memset(&inode, 0, sizeof(inode));
	inode.ino = ino;
	inode.parent_id = parent;
	inode.private_id = ino;
	inode.create_time = inode.mod_time = inode.change_time = inode.access_time = now;
	inode.internal_flags = INODE_NO_RSRC_FORK;
	inode.nlink = (type == APFS_S_IFDIR) ? 0 : 1;
	inode.write_gen = 1;
	inode.bsd_flags = attr->bsd_flags;
	inode.uid = attr->uid;
	inode.gid = attr->gid;
	inode.mode = attr->mode;
	apfs_inode_set_name(&inode, name);
	if(type == APFS_S_IFCHR || type == APFS_S_IFBLK) {
		uint8_t rdev[4];
		apfs_put32(rdev, attr->rdev);
		apfs_inode_xf_set(&inode, INO_EXT_TYPE_RDEV, 0, rdev, sizeof(rdev));
	}

	ret = apfs_inode_write(vol, &inode);
	apfs_inode_release(&inode);
	if(ret == APFS_OK)
		ret = drec_insert(vol, parent, name, ino, apfs_mode_to_dtype(attr->mode), now, 0);
	if(ret == APFS_OK)
		ret = parent_update(vol, parent, 1, now);
	if(ret < 0)
		return ret;
	count_object(vol, attr->mode, 1);
	if(ino_out)
		*ino_out = ino;
	return APFS_OK;
}

int apfs_symlink(apfs_vol_t* vol, uint64_t parent, const char* name, const char* target, const apfs_create_t* attr, uint64_t* ino_out) {
	apfs_create_t a = *attr;
	uint64_t ino;
	size_t tlen = strlen(target);
	int ret;

	if(tlen == 0 || tlen >= 1024)
		return APFS_EINVAL;
	a.mode = APFS_S_IFLNK | (attr->mode & 07777 ? attr->mode & 07777 : 0755);
	ret = apfs_create(vol, parent, name, &a, &ino);
	if(ret < 0)
		return ret;
	ret = apfs_xattr_set(vol, ino, SYMLINK_EA_NAME, target, tlen + 1, 1);
	if(ret == APFS_OK && ino_out)
		*ino_out = ino;
	return ret;
}

static void sibling_key(uint8_t* key, uint64_t ino, uint64_t sibling) {
	apfs_key_hdr(key, ino, APFS_TYPE_SIBLING_LINK);
	apfs_put64(key + 8, sibling);
}

static int sibling_insert(apfs_vol_t* vol, uint64_t ino, uint64_t sibling, uint64_t parent, const char* name) {
	uint8_t key[16], mkey[8], mval[8];
	size_t nlen = strlen(name) + 1;
	uint8_t* val = (uint8_t*)apfs_malloc(10 + nlen);
	int ret;
	sibling_key(key, ino, sibling);
	apfs_put64(val, parent);
	apfs_put16(val + 8, (uint16_t)nlen);
	memcpy(val + 10, name, nlen);
	ret = apfs_bt_insert(&vol->fs, key, sizeof(key), val, (uint16_t)(10 + nlen));
	free(val);
	if(ret < 0 || !vol->sibling_maps)
		return ret;
	apfs_key_hdr(mkey, sibling, APFS_TYPE_SIBLING_MAP);
	apfs_put64(mval, ino);
	return apfs_bt_insert(&vol->fs, mkey, sizeof(mkey), mval, sizeof(mval));
}

static int sibling_delete(apfs_vol_t* vol, uint64_t ino, uint64_t sibling) {
	uint8_t key[16], mkey[8];
	int ret;
	sibling_key(key, ino, sibling);
	ret = apfs_bt_delete(&vol->fs, key, sizeof(key));
	if(ret < 0 && ret != APFS_ENOENT)
		return ret;
	apfs_key_hdr(mkey, sibling, APFS_TYPE_SIBLING_MAP);
	ret = apfs_bt_delete(&vol->fs, mkey, sizeof(mkey));
	return ret == APFS_ENOENT ? APFS_OK : ret;
}

typedef struct {
	uint64_t id;
	uint64_t parent;
	char* name;
} sibling_t;

static int sibling_list(apfs_vol_t* vol, uint64_t ino, sibling_t** out, size_t* count) {
	apfs_btiter_t it;
	apfs_kv_t kv;
	uint8_t start[16];
	sibling_t* list = NULL;
	size_t n = 0;
	int ret;

	sibling_key(start, ino, 0);
	ret = apfs_bt_iter_seek(&it, &vol->fs, start, sizeof(start));
	while(ret == APFS_OK && it.valid) {
		apfs_bt_iter_get(&it, &kv);
		if(apfs_key_id(kv.k) != ino || apfs_key_type(kv.k) != APFS_TYPE_SIBLING_LINK)
			break;
		if(kv.klen >= 16 && kv.vlen >= 10) {
			uint16_t nlen = apfs_get16(kv.v + 8);
			if(10 + (uint32_t)nlen > kv.vlen)
				nlen = (uint16_t)(kv.vlen - 10);
			list = (sibling_t*)apfs_realloc(list, (n + 1) * sizeof(sibling_t));
			list[n].id = apfs_get64(kv.k + 8);
			list[n].parent = apfs_get64(kv.v);
			list[n].name = (char*)apfs_malloc((size_t)nlen + 1);
			memcpy(list[n].name, kv.v + 10, nlen);
			list[n].name[nlen] = 0;
			n++;
		}
		ret = apfs_bt_iter_next(&it);
	}
	*out = list;
	*count = n;
	return ret < 0 ? ret : APFS_OK;
}

static void siblings_free(sibling_t* s, size_t n) {
	size_t i;
	for(i = 0; i < n; i++)
		free(s[i].name);
	free(s);
}

int apfs_link(apfs_vol_t* vol, uint64_t ino, uint64_t newparent, const char* newname) {
	apfs_inode_t inode;
	sibling_t* sibs;
	size_t nsibs;
	uint64_t now = apfs_time_now(), sib;
	int ret;

	ret = check_name(newname);
	if(ret < 0)
		return ret;
	ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;
	if(apfs_dir_lookup(vol, newparent, newname, NULL) == APFS_OK)
		return APFS_EEXIST;
	ret = apfs_inode_read(vol, ino, &inode);
	if(ret < 0)
		return ret;
	if((inode.mode & APFS_S_IFMT) == APFS_S_IFDIR) {
		apfs_inode_release(&inode);
		return APFS_EISDIR;
	}

	ret = sibling_list(vol, ino, &sibs, &nsibs);
	if(ret == APFS_OK && nsibs == 0) {
		/* the existing link becomes a sibling too */
		apfs_dirent_t de;
		const char* pname = apfs_inode_name(&inode);
		if(!pname) {
			ret = APFS_ECORRUPT;
		} else {
			ret = apfs_dir_lookup(vol, inode.parent_id, pname, &de);
			if(ret == APFS_OK) {
				sib = apfs_vol_next_id(vol);
				ret = sibling_insert(vol, ino, sib, inode.parent_id, de.name);
				if(ret == APFS_OK)
					ret = drec_insert(vol, inode.parent_id, de.name, ino, de.flags & DREC_TYPE_MASK, de.date_added, sib);
				apfs_dirent_release(&de);
			}
		}
	}
	siblings_free(sibs, nsibs);
	if(ret == APFS_OK) {
		sib = apfs_vol_next_id(vol);
		ret = sibling_insert(vol, ino, sib, newparent, newname);
		if(ret == APFS_OK)
			ret = drec_insert(vol, newparent, newname, ino, apfs_mode_to_dtype(inode.mode), now, sib);
	}
	if(ret == APFS_OK) {
		inode.nlink++;
		inode.change_time = now;
		ret = apfs_inode_write(vol, &inode);
	}
	apfs_inode_release(&inode);
	if(ret == APFS_OK)
		ret = parent_update(vol, newparent, 1, now);
	return ret;
}

/* Removes one link of an inode; the inode goes away with its last link */
static int unlink_internal(apfs_vol_t* vol, uint64_t parent, const apfs_dirent_t* de, uint64_t now) {
	apfs_inode_t inode;
	int ret;

	ret = apfs_inode_read(vol, de->ino, &inode);
	if(ret < 0)
		return ret;
	if((inode.mode & APFS_S_IFMT) == APFS_S_IFDIR && inode.nlink != 0) {
		apfs_inode_release(&inode);
		return APFS_ENOTEMPTY;
	}

	ret = drec_delete(vol, parent, de->name);
	if(ret == APFS_OK)
		ret = parent_update(vol, parent, -1, now);
	if(ret < 0) {
		apfs_inode_release(&inode);
		return ret;
	}

	if((inode.mode & APFS_S_IFMT) != APFS_S_IFDIR && inode.nlink > 1) {
		sibling_t* sibs;
		size_t nsibs, i;
		if(de->sibling_id)
			ret = sibling_delete(vol, inode.ino, de->sibling_id);
		inode.nlink--;
		inode.change_time = now;
		if(ret == APFS_OK)
			ret = sibling_list(vol, inode.ino, &sibs, &nsibs);
		if(ret == APFS_OK) {
			/* the primary link is the remaining sibling with the lowest id */
			if(nsibs > 0) {
				size_t best = 0;
				for(i = 1; i < nsibs; i++)
					if(sibs[i].id < sibs[best].id)
						best = i;
				inode.parent_id = sibs[best].parent;
				apfs_inode_set_name(&inode, sibs[best].name);
			}
			siblings_free(sibs, nsibs);
			ret = apfs_inode_write(vol, &inode);
		}
		apfs_inode_release(&inode);
		return ret;
	}

	ret = inode_destroy(vol, &inode);
	if(ret == APFS_OK) {
		uint8_t key[8];
		apfs_key_hdr(key, inode.ino, APFS_TYPE_INODE);
		ret = apfs_bt_delete(&vol->fs, key, sizeof(key));
	}
	apfs_inode_release(&inode);
	return ret;
}

int apfs_unlink(apfs_vol_t* vol, uint64_t parent, const char* name) {
	apfs_dirent_t de;
	int ret;

	ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;
	ret = apfs_dir_lookup(vol, parent, name, &de);
	if(ret < 0)
		return ret;
	if(de.ino < MIN_USER_INO_NUM) {
		apfs_dirent_release(&de);
		return APFS_EINVAL;
	}
	ret = unlink_internal(vol, parent, &de, apfs_time_now());
	apfs_dirent_release(&de);
	return ret;
}

/* Is `dir` the same as or below `ancestor`? */
static int is_descendant(apfs_vol_t* vol, uint64_t dir, uint64_t ancestor) {
	int depth = 0;
	while(dir != ROOT_DIR_INO_NUM && depth++ < 4096) {
		apfs_inode_t i;
		if(dir == ancestor)
			return 1;
		if(apfs_inode_read(vol, dir, &i) < 0)
			return 0;
		dir = i.parent_id;
		apfs_inode_release(&i);
	}
	return dir == ancestor;
}

int apfs_rename(apfs_vol_t* vol, uint64_t oldparent, const char* oldname, uint64_t newparent, const char* newname) {
	apfs_dirent_t src, dst;
	apfs_inode_t inode;
	uint64_t now = apfs_time_now();
	int ret, have_dst;

	ret = check_name(newname);
	if(ret < 0)
		return ret;
	ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;
	ret = apfs_dir_lookup(vol, oldparent, oldname, &src);
	if(ret < 0)
		return ret;
	if(src.ino < MIN_USER_INO_NUM) {
		apfs_dirent_release(&src);
		return APFS_EINVAL;
	}
	if((src.flags & DREC_TYPE_MASK) == APFS_DT_DIR && is_descendant(vol, newparent, src.ino)) {
		apfs_dirent_release(&src);
		return APFS_EINVAL;
	}

	have_dst = apfs_dir_lookup(vol, newparent, newname, &dst) == APFS_OK;
	if(have_dst) {
		if(dst.ino == src.ino) {
			int same_entry = (oldparent == newparent && strcmp(src.name, dst.name) == 0);
			apfs_dirent_release(&dst);
			if(same_entry && strcmp(oldname, newname) == 0) {
				apfs_dirent_release(&src);
				return APFS_OK;
			}
			if(!same_entry) {
				/* another link to the same file: just drop the source */
				ret = unlink_internal(vol, oldparent, &src, now);
				apfs_dirent_release(&src);
				return ret;
			}
			/* only the spelling changes */
		} else {
			int sdir = (src.flags & DREC_TYPE_MASK) == APFS_DT_DIR;
			int ddir = (dst.flags & DREC_TYPE_MASK) == APFS_DT_DIR;
			if(sdir != ddir) {
				apfs_dirent_release(&src);
				apfs_dirent_release(&dst);
				return sdir ? APFS_ENOTDIR : APFS_EISDIR;
			}
			ret = unlink_internal(vol, newparent, &dst, now);
			apfs_dirent_release(&dst);
			if(ret < 0) {
				apfs_dirent_release(&src);
				return ret;
			}
		}
	}

	ret = drec_delete(vol, oldparent, src.name);
	if(ret == APFS_OK)
		ret = drec_insert(vol, newparent, newname, src.ino, src.flags & DREC_TYPE_MASK, src.date_added, src.sibling_id);
	if(ret == APFS_OK && oldparent != newparent) {
		ret = parent_update(vol, oldparent, -1, now);
		if(ret == APFS_OK)
			ret = parent_update(vol, newparent, 1, now);
	} else if(ret == APFS_OK) {
		ret = parent_update(vol, newparent, 0, now);
	}
	if(ret == APFS_OK)
		ret = apfs_inode_read(vol, src.ino, &inode);
	if(ret == APFS_OK) {
		if(src.sibling_id) {
			sibling_t* sibs;
			size_t nsibs, i;
			ret = sibling_delete(vol, src.ino, src.sibling_id);
			if(ret == APFS_OK)
				ret = sibling_insert(vol, src.ino, src.sibling_id, newparent, newname);
			if(ret == APFS_OK)
				ret = sibling_list(vol, src.ino, &sibs, &nsibs);
			if(ret == APFS_OK) {
				size_t best = 0;
				for(i = 1; i < nsibs; i++)
					if(sibs[i].id < sibs[best].id)
						best = i;
				if(nsibs) {
					inode.parent_id = sibs[best].parent;
					apfs_inode_set_name(&inode, sibs[best].name);
				}
				siblings_free(sibs, nsibs);
			}
		} else {
			inode.parent_id = newparent;
			apfs_inode_set_name(&inode, newname);
		}
		inode.change_time = now;
		if(ret == APFS_OK)
			ret = apfs_inode_write(vol, &inode);
		apfs_inode_release(&inode);
	}
	apfs_dirent_release(&src);
	return ret;
}

int apfs_setattr(apfs_vol_t* vol, uint64_t ino, int mask, const apfs_setattr_t* attr) {
	apfs_inode_t inode;
	int ret;

	ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;
	ret = apfs_inode_read(vol, ino, &inode);
	if(ret < 0)
		return ret;
	if(mask & APFS_SETATTR_MODE)
		inode.mode = (uint16_t)((inode.mode & APFS_S_IFMT) | (attr->mode & 07777));
	if(mask & APFS_SETATTR_UID)
		inode.uid = attr->uid;
	if(mask & APFS_SETATTR_GID)
		inode.gid = attr->gid;
	if(mask & APFS_SETATTR_FLAGS) {
		/* the compressed flag is owned by the file system */
		inode.bsd_flags = (attr->bsd_flags & ~(uint32_t)APFS_UF_COMPRESSED) | (inode.bsd_flags & APFS_UF_COMPRESSED);
	}
	if(mask & APFS_SETATTR_MTIME)
		inode.mod_time = attr->mtime;
	if(mask & APFS_SETATTR_ATIME)
		inode.access_time = attr->atime;
	if(mask & APFS_SETATTR_BTIME)
		inode.create_time = attr->btime;
	inode.change_time = (mask & APFS_SETATTR_CTIME) ? attr->ctime : apfs_time_now();
	ret = apfs_inode_write(vol, &inode);
	apfs_inode_release(&inode);
	return ret;
}

/* Extended attributes */

static size_t mem_source(void* ctx, void* buf, size_t len) {
	struct {
		const uint8_t* p;
		size_t left;
	}* m = ctx;
	if(len > m->left)
		len = m->left;
	memcpy(buf, m->p, len);
	m->p += len;
	m->left -= len;
	return len;
}

static int xattr_inode_flags(apfs_vol_t* vol, uint64_t ino, const char* name, int present) {
	apfs_inode_t inode;
	uint64_t before;
	int ret;

	if(strcmp(name, RESOURCEFORK_EA_NAME) && strcmp(name, SECURITY_EA_NAME) && strcmp(name, FINDERINFO_EA_NAME))
		return APFS_OK;
	ret = apfs_inode_read(vol, ino, &inode);
	if(ret < 0)
		return ret;
	before = inode.internal_flags;
	if(!strcmp(name, RESOURCEFORK_EA_NAME)) {
		inode.internal_flags &= ~(uint64_t)(INODE_HAS_RSRC_FORK | INODE_NO_RSRC_FORK);
		inode.internal_flags |= present ? INODE_HAS_RSRC_FORK : INODE_NO_RSRC_FORK;
	} else if(!strcmp(name, SECURITY_EA_NAME)) {
		if(present)
			inode.internal_flags |= INODE_HAS_SECURITY_EA;
		else
			inode.internal_flags &= ~(uint64_t)INODE_HAS_SECURITY_EA;
	} else if(!apfs_inode_xf(&inode, INO_EXT_TYPE_FINDER_INFO, NULL)) {
		if(present)
			inode.internal_flags |= INODE_HAS_FINDER_INFO;
		else
			inode.internal_flags &= ~(uint64_t)INODE_HAS_FINDER_INFO;
	}
	ret = APFS_OK;
	if(before != inode.internal_flags)
		ret = apfs_inode_write(vol, &inode);
	apfs_inode_release(&inode);
	return ret;
}

int apfs_xattr_set(apfs_vol_t* vol, uint64_t ino, const char* name, const void* data, size_t size, int fs_owned) {
	uint8_t key[10 + 256];
	uint8_t* val;
	uint16_t vlen;
	apfs_kv_t kv;
	int klen, ret;

	klen = xattr_key(key, ino, name);
	if(klen < 0)
		return klen;
	if(size > 0xFFFFFFFFULL)
		return APFS_ERANGE;
	ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;

	/* release the old value first */
	if(apfs_bt_lookup(&vol->fs, key, (uint16_t)klen, &kv) == APFS_OK) {
		uint8_t* copy = (uint8_t*)apfs_malloc(kv.vlen + 1);
		uint16_t len = kv.vlen;
		memcpy(copy, kv.v, len);
		ret = xattr_val_release(vol, copy, len);
		free(copy);
		if(ret < 0)
			return ret;
	}

	/* resource forks are always stored as data streams */
	if(size <= XATTR_MAX_EMBEDDED_SIZE && strcmp(name, RESOURCEFORK_EA_NAME) != 0) {
		vlen = (uint16_t)(4 + size);
		val = (uint8_t*)apfs_malloc(vlen);
		apfs_put16(val, (uint16_t)(XATTR_DATA_EMBEDDED | (fs_owned ? XATTR_FILE_SYSTEM_OWNED : 0)));
		apfs_put16(val + 2, (uint16_t)size);
		memcpy(val + 4, data, size);
	} else {
		apfs_dstream_t ds;
		uint64_t id = apfs_vol_next_id(vol);
		struct {
			const uint8_t* p;
			size_t left;
		} m;
		m.p = (const uint8_t*)data;
		m.left = size;
		ret = apfs_dstream_write(vol, id, size, mem_source, &m, &ds);
		if(ret < 0)
			return ret;
		vlen = 4 + sizeof(j_xattr_dstream_t);
		val = (uint8_t*)apfs_calloc(1, vlen);
		apfs_put16(val, (uint16_t)(XATTR_DATA_STREAM | (fs_owned ? XATTR_FILE_SYSTEM_OWNED : 0)));
		apfs_put16(val + 2, sizeof(j_xattr_dstream_t));
		apfs_put64(val + 4, id);
		apfs_put64(val + 12, ds.size);
		apfs_put64(val + 20, ds.alloced_size);
		apfs_put64(val + 28, 0);
		apfs_put64(val + 36, ds.bytes_written);
		apfs_put64(val + 44, 0);
	}
	ret = apfs_bt_insert(&vol->fs, key, (uint16_t)klen, val, vlen);
	free(val);
	if(ret == APFS_OK)
		ret = xattr_inode_flags(vol, ino, name, 1);
	return ret;
}

int apfs_xattr_remove(apfs_vol_t* vol, uint64_t ino, const char* name) {
	uint8_t key[10 + 256];
	apfs_kv_t kv;
	int klen, ret;

	klen = xattr_key(key, ino, name);
	if(klen < 0)
		return klen;
	ret = apfs_vol_mark_dirty(vol);
	if(ret < 0)
		return ret;
	ret = apfs_bt_lookup(&vol->fs, key, (uint16_t)klen, &kv);
	if(ret < 0)
		return ret;
	{
		uint8_t* copy = (uint8_t*)apfs_malloc(kv.vlen + 1);
		uint16_t len = kv.vlen;
		memcpy(copy, kv.v, len);
		ret = xattr_val_release(vol, copy, len);
		free(copy);
	}
	if(ret == APFS_OK)
		ret = apfs_bt_delete(&vol->fs, key, (uint16_t)klen);
	if(ret == APFS_OK)
		ret = xattr_inode_flags(vol, ino, name, 0);
	return ret;
}
