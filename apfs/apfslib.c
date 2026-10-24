/*
 * apfslib.c
 * apfs-tool
 *
 * Path-based operations: the APFS counterpart of libdmg-hfsplus' hfslib.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apfs/apfs.h"
#include "apfs/apfslib.h"
#include "apfs/platform.h"

#define IO_CHUNK (1024 * 1024)

void apfslib_default_opts(apfslib_opts_t* o) {
	memset(o, 0, sizeof(*o));
	o->uid = APFS_OWNER_INHERIT;
	o->gid = APFS_OWNER_INHERIT;
	o->file_mode = 0644;
	o->dir_mode = 0755;
	o->xattrs = 1;
	o->resource_fork_size = UINT64_MAX;
}

static void report(const char* what, const char* path, int err) {
	APFS_ERR("%s %s: %s", what, path, apfs_strerror(err));
}

/* Splits a path into its existing parent directory and the final name */
static int resolve_parent(apfs_vol_t* vol, const char* path, uint64_t* parent, char** name) {
	char* copy = apfs_strdup(path);
	char* slash;
	size_t len = strlen(copy);
	int ret;

	*name = NULL;
	while(len > 1 && copy[len - 1] == '/')
		copy[--len] = 0;
	slash = strrchr(copy, '/');
	if(!slash) {
		*parent = ROOT_DIR_INO_NUM;
		*name = copy;
		ret = APFS_OK;
	} else {
		char* base = apfs_strdup(slash + 1);
		*slash = 0;
		ret = apfs_lookup(vol, *copy ? copy : "/", 1, parent, NULL, NULL);
		free(copy);
		if(ret < 0) {
			free(base);
			return ret;
		}
		*name = base;
	}
	if(!**name || !strcmp(*name, ".") || !strcmp(*name, "..")) {
		free(*name);
		*name = NULL;
		return APFS_EINVAL;
	}
	return ret;
}

static void owner_for(apfs_vol_t* vol, uint64_t parent, const apfslib_opts_t* o, uint32_t* uid, uint32_t* gid) {
	apfs_inode_t p;
	*uid = 0;
	*gid = 0;
	if(apfs_inode_read(vol, parent, &p) == APFS_OK) {
		*uid = p.uid;
		*gid = p.gid;
		apfs_inode_release(&p);
	}
	if(o->uid != APFS_OWNER_INHERIT)
		*uid = o->uid;
	if(o->gid != APFS_OWNER_INHERIT)
		*gid = o->gid;
}

static const char* type_char(uint16_t mode) {
	switch(mode & APFS_S_IFMT) {
		case APFS_S_IFDIR: return "d";
		case APFS_S_IFLNK: return "l";
		case APFS_S_IFCHR: return "c";
		case APFS_S_IFBLK: return "b";
		case APFS_S_IFIFO: return "p";
		case APFS_S_IFSOCK: return "s";
		default: return "-";
	}
}

static void print_time(FILE* out, uint64_t ns) {
	time_t t = (time_t)(ns / 1000000000ULL);
	struct tm* tm = localtime(&t);
	if(tm)
		fprintf(out, "%02d/%02d/%04d %02d:%02d ", tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900, tm->tm_hour, tm->tm_min);
	else
		fprintf(out, "                 ");
}

static void ls_line(apfs_vol_t* vol, const apfs_inode_t* inode, const char* name, int flags, FILE* out) {
	uint64_t size = 0;
	char* target = NULL;
	if(flags & APFSLIB_LS_INODES)
		fprintf(out, "%8llu ", (unsigned long long)inode->ino);
	fprintf(out, "%06o %3u %3u ", inode->mode, inode->uid, inode->gid);
	if((inode->mode & APFS_S_IFMT) == APFS_S_IFDIR)
		size = inode->nlink;
	else if((inode->mode & APFS_S_IFMT) == APFS_S_IFLNK && apfs_readlink(vol, inode->ino, &target) == APFS_OK)
		size = strlen(target);   /* a link's size is the length of its target */
	else
		apfs_file_size(vol, inode, &size);
	fprintf(out, "%12llu ", (unsigned long long)size);
	print_time(out, inode->mod_time);
	fputs(name, out);
	if(target)
		fprintf(out, " -> %s", target);
	free(target);
	fputc('\n', out);
}

static int ls_dir(apfs_vol_t* vol, uint64_t dir, const char* path, int flags, FILE* out) {
	apfs_dirent_t* ents;
	size_t n, i;
	int ret = apfs_dir_list(vol, dir, &ents, &n);
	if(ret < 0)
		return ret;
	for(i = 0; i < n; i++) {
		apfs_inode_t inode;
		if(apfs_inode_read(vol, ents[i].ino, &inode) < 0) {
			fprintf(out, "?????? ??? ??? %12s %17s%s\n", "?", "", ents[i].name);
			continue;
		}
		ls_line(vol, &inode, ents[i].name, flags, out);
		apfs_inode_release(&inode);
	}
	if(flags & APFSLIB_LS_RECURSIVE) {
		for(i = 0; i < n; i++) {
			char* sub;
			if((ents[i].flags & DREC_TYPE_MASK) != APFS_DT_DIR)
				continue;
			sub = plat_join(path, ents[i].name);
			fprintf(out, "\n%s:\n", sub);
			ls_dir(vol, ents[i].ino, sub, flags, out);
			free(sub);
		}
	}
	apfs_dirents_free(ents, n);
	return APFS_OK;
}

int apfslib_ls(apfs_vol_t* vol, const char* path, int flags, FILE* out) {
	uint64_t ino;
	apfs_inode_t inode;
	uint64_t total, freeb;
	int ret;

	ret = apfs_lookup(vol, path, 1, &ino, NULL, NULL);
	if(ret < 0) {
		report("cannot access", path, ret);
		return ret;
	}
	ret = apfs_inode_read(vol, ino, &inode);
	if(ret < 0)
		return ret;
	fprintf(out, "%s: \n", path);
	if((inode.mode & APFS_S_IFMT) == APFS_S_IFDIR) {
		ret = ls_dir(vol, ino, strcmp(path, "/") == 0 ? "" : path, flags, out);
	} else {
		char** names;
		size_t n, i;
		const char* base = strrchr(path, '/');
		ls_line(vol, &inode, base ? base + 1 : path, flags, out);
		if(apfs_xattr_list(vol, ino, &names, &n) == APFS_OK) {
			if(n)
				fprintf(out, "Extended attributes\n");
			for(i = 0; i < n; i++)
				fprintf(out, "\t%s\n", names[i]);
			apfs_names_free(names, n);
		}
	}
	apfs_inode_release(&inode);
	total = LE64(vol->nx->sb->nx_block_count) * vol->nx->bs;
	freeb = apfs_sm_free_blocks(vol->nx) * vol->nx->bs;
	fprintf(out, "Total filesystem size: %llu, free: %llu\n", (unsigned long long)(total - freeb), (unsigned long long)freeb);
	return ret;
}

static const struct {
	uint32_t flag;
	const char* name;
} flag_names[] = {
	{ APFS_UF_NODUMP, "nodump" }, { APFS_UF_IMMUTABLE, "uchg" }, { APFS_UF_APPEND, "uappnd" },
	{ APFS_UF_OPAQUE, "opaque" }, { APFS_UF_NOUNLINK, "uunlnk" }, { APFS_UF_COMPRESSED, "compressed" },
	{ APFS_UF_TRACKED, "tracked" }, { APFS_UF_DATAVAULT, "datavault" }, { APFS_UF_HIDDEN, "hidden" },
	{ APFS_SF_ARCHIVED, "arch" }, { APFS_SF_IMMUTABLE, "schg" }, { APFS_SF_APPEND, "sappnd" },
	{ APFS_SF_RESTRICTED, "restricted" }, { APFS_SF_NOUNLINK, "sunlnk" }, { APFS_SF_FIRMLINK, "firmlink" },
	{ APFS_SF_DATALESS, "dataless" }, { 0, NULL }
};

int apfslib_parse_flags(const char* s, uint32_t* set, uint32_t* clear) {
	char* copy = apfs_strdup(s);
	char* tok;
	char* save = NULL;
	*set = 0;
	*clear = 0;
	if(s[0] >= '0' && s[0] <= '9') {
		*set = (uint32_t)strtoul(s, NULL, 0);
		*clear = ~*set;
		free(copy);
		return APFS_OK;
	}
	for(tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		int neg = 0, i;
		if(!strncmp(tok, "no", 2) && strcmp(tok, "nodump") != 0) {
			neg = 1;
			tok += 2;
		}
		for(i = 0; flag_names[i].name; i++)
			if(!strcmp(tok, flag_names[i].name))
				break;
		if(!flag_names[i].name) {
			free(copy);
			return APFS_EINVAL;
		}
		if(neg)
			*clear |= flag_names[i].flag;
		else
			*set |= flag_names[i].flag;
	}
	free(copy);
	return APFS_OK;
}

static void print_flags(FILE* out, uint32_t flags) {
	int i, first = 1;
	for(i = 0; flag_names[i].name; i++) {
		if(flags & flag_names[i].flag) {
			fprintf(out, "%s%s", first ? "" : ",", flag_names[i].name);
			first = 0;
		}
	}
	if(first)
		fprintf(out, "-");
}

int apfslib_stat(apfs_vol_t* vol, const char* path, FILE* out) {
	uint64_t ino, size;
	apfs_inode_t inode;
	apfs_dstream_t ds;
	char** names;
	size_t n, i;
	int ret;

	ret = apfs_lookup(vol, path, 0, &ino, NULL, NULL);
	if(ret < 0) {
		report("cannot access", path, ret);
		return ret;
	}
	ret = apfs_inode_read(vol, ino, &inode);
	if(ret < 0)
		return ret;
	apfs_file_size(vol, &inode, &size);
	fprintf(out, "  File: %s\n", path);
	fprintf(out, " Inode: %llu  Parent: %llu  Private id: %llu\n", (unsigned long long)inode.ino,
		(unsigned long long)inode.parent_id, (unsigned long long)inode.private_id);
	fprintf(out, "  Type: %s  Mode: %06o  Uid: %u  Gid: %u\n", type_char(inode.mode), inode.mode, inode.uid, inode.gid);
	if((inode.mode & APFS_S_IFMT) == APFS_S_IFDIR)
		fprintf(out, "Children: %u\n", inode.nlink);
	else
		fprintf(out, " Links: %u  Size: %llu\n", inode.nlink, (unsigned long long)size);
	if(apfs_inode_get_dstream(&inode, &ds) == APFS_OK)
		fprintf(out, "Stream: size %llu allocated %llu\n", (unsigned long long)ds.size, (unsigned long long)ds.alloced_size);
	fprintf(out, " Flags: ");
	print_flags(out, inode.bsd_flags);
	fprintf(out, " (0x%x)  Internal flags: 0x%llx  Protection class: %u\n", inode.bsd_flags,
		(unsigned long long)inode.internal_flags, inode.prot_class);
	if(inode.bsd_flags & APFS_UF_COMPRESSED) {
		uint8_t* hdr;
		size_t len;
		if(apfs_xattr_get(vol, ino, DECMPFS_EA_NAME, &hdr, &len) == APFS_OK) {
			if(len >= 16)
				fprintf(out, "Compression: %s, %llu bytes uncompressed\n", apfs_decmpfs_type_name(apfs_get32(hdr + 4)),
					(unsigned long long)apfs_get64(hdr + 8));
			free(hdr);
		}
	}
	{
		uint16_t rs;
		const uint8_t* r = apfs_inode_xf(&inode, INO_EXT_TYPE_RDEV, &rs);
		if(r && rs >= 4)
			fprintf(out, "Device: %u, %u\n", (apfs_get32(r) >> 24) & 0xff, apfs_get32(r) & 0xffffff);
	}
	fprintf(out, "Modify: ");
	print_time(out, inode.mod_time);
	fprintf(out, "\nChange: ");
	print_time(out, inode.change_time);
	fprintf(out, "\nAccess: ");
	print_time(out, inode.access_time);
	fprintf(out, "\n Birth: ");
	print_time(out, inode.create_time);
	fputc('\n', out);
	if(apfs_xattr_list(vol, ino, &names, &n) == APFS_OK) {
		for(i = 0; i < n; i++) {
			uint8_t* d;
			size_t len;
			if(apfs_xattr_get(vol, ino, names[i], &d, &len) == APFS_OK) {
				fprintf(out, " Xattr: %s (%lu bytes)\n", names[i], (unsigned long)len);
				free(d);
			} else {
				fprintf(out, " Xattr: %s\n", names[i]);
			}
		}
		apfs_names_free(names, n);
	}
	apfs_inode_release(&inode);
	return APFS_OK;
}

static int write_contents(apfs_vol_t* vol, const apfs_inode_t* inode, FILE* f) {
	uint64_t size, off = 0;
	uint8_t* buf = (uint8_t*)apfs_malloc(IO_CHUNK);
	int ret = APFS_OK;

	apfs_file_size(vol, inode, &size);
	while(off < size) {
		size_t got;
		ret = apfs_file_read(vol, inode, off, buf, IO_CHUNK, &got);
		if(ret < 0)
			break;
		if(!got) {
			ret = APFS_ECORRUPT;
			break;
		}
		if(fwrite(buf, 1, got, f) != got) {
			ret = APFS_EIO;
			break;
		}
		off += got;
	}
	free(buf);
	return ret;
}

int apfslib_cat(apfs_vol_t* vol, const char* path, FILE* out) {
	uint64_t ino;
	apfs_inode_t inode;
	int ret;

	ret = apfs_lookup(vol, path, 1, &ino, NULL, NULL);
	if(ret < 0) {
		report("cannot access", path, ret);
		return ret;
	}
	ret = apfs_inode_read(vol, ino, &inode);
	if(ret < 0)
		return ret;
	if((inode.mode & APFS_S_IFMT) != APFS_S_IFREG) {
		apfs_inode_release(&inode);
		APFS_ERR("%s: not a file", path);
		return APFS_EISDIR;
	}
	ret = write_contents(vol, &inode, out);
	apfs_inode_release(&inode);
	if(ret < 0)
		report("cannot read", path, ret);
	return ret;
}

int apfslib_extract(apfs_vol_t* vol, const char* path, const char* dest) {
	FILE* f;
	uint64_t ino;
	apfs_inode_t inode;
	int ret;

	ret = apfs_lookup(vol, path, 1, &ino, NULL, NULL);
	if(ret < 0) {
		report("cannot access", path, ret);
		return ret;
	}
	ret = apfs_inode_read(vol, ino, &inode);
	if(ret < 0)
		return ret;
	if((inode.mode & APFS_S_IFMT) != APFS_S_IFREG) {
		apfs_inode_release(&inode);
		APFS_ERR("%s: not a file", path);
		return APFS_EISDIR;
	}
	f = fopen(dest, "wb");
	if(!f) {
		apfs_inode_release(&inode);
		APFS_ERR("cannot create %s", dest);
		return APFS_EIO;
	}
	ret = write_contents(vol, &inode, f);
	if(fclose(f) != 0 && ret == APFS_OK)
		ret = APFS_EIO;
	apfs_inode_release(&inode);
	if(ret < 0)
		report("cannot extract", path, ret);
	return ret;
}

/* extractall */

typedef struct {
	uint64_t ino;
	char* path;
} link_map_t;

typedef struct {
	apfs_vol_t* vol;
	const apfslib_opts_t* o;
	link_map_t* links;
	size_t nlinks;
	int errors;
	int is_root;
} extract_ctx_t;

static void extract_meta(extract_ctx_t* c, const apfs_inode_t* inode, const char* dest) {
	uint16_t type = inode->mode & APFS_S_IFMT;
	if(c->o->xattrs) {
		char** names;
		size_t n, i;
		if(apfs_xattr_list(c->vol, inode->ino, &names, &n) == APFS_OK) {
			for(i = 0; i < n; i++) {
				uint8_t* d;
				size_t len;
				if(!strcmp(names[i], DECMPFS_EA_NAME) || !strcmp(names[i], SYMLINK_EA_NAME))
					continue;
				if((inode->bsd_flags & APFS_UF_COMPRESSED) && !strcmp(names[i], RESOURCEFORK_EA_NAME))
					continue;
				if(apfs_xattr_get(c->vol, inode->ino, names[i], &d, &len) == APFS_OK) {
					plat_xattr_set(dest, names[i], d, len);
					free(d);
				}
			}
			apfs_names_free(names, n);
		}
	}
	if(c->is_root)
		plat_chown(dest, inode->uid, inode->gid);
	if(type != APFS_S_IFLNK)
		plat_chmod(dest, inode->mode & 07777);
	plat_set_times(dest, inode->access_time, inode->mod_time);
}

static int extract_tree(extract_ctx_t* c, uint64_t dir, const char* destdir) {
	apfs_dirent_t* ents;
	size_t n, i;
	int ret = apfs_dir_list(c->vol, dir, &ents, &n);
	if(ret < 0)
		return ret;
	for(i = 0; i < n; i++) {
		apfs_inode_t inode;
		char* dest = plat_join(destdir, ents[i].name);
		uint16_t type;

		if(apfs_inode_read(c->vol, ents[i].ino, &inode) < 0) {
			APFS_ERR("cannot read inode of %s", dest);
			c->errors++;
			free(dest);
			continue;
		}
		type = inode.mode & APFS_S_IFMT;
		if(!c->o->quiet)
			printf("%s: %s\n", type == APFS_S_IFDIR ? "folder" : "file", dest);

		if(type == APFS_S_IFDIR) {
			if(plat_mkdir(dest, 0755) < 0) {
				APFS_ERR("cannot create directory %s", dest);
				c->errors++;
			} else {
				extract_tree(c, inode.ino, dest);
				extract_meta(c, &inode, dest);
			}
		} else if(type == APFS_S_IFLNK) {
			char* target;
			if(apfs_readlink(c->vol, inode.ino, &target) == APFS_OK) {
				plat_remove(dest);
				if(plat_symlink(target, dest) < 0) {
					APFS_WARN("cannot create symbolic link %s -> %s", dest, target);
				} else {
					extract_meta(c, &inode, dest);
				}
				free(target);
			}
		} else if(type == APFS_S_IFREG) {
			size_t k;
			int linked = 0;
			if(inode.nlink > 1) {
				for(k = 0; k < c->nlinks; k++) {
					if(c->links[k].ino == inode.ino) {
						plat_remove(dest);
						if(plat_link(c->links[k].path, dest) == APFS_OK)
							linked = 1;
						break;
					}
				}
			}
			if(!linked) {
				FILE* f = fopen(dest, "wb");
				if(!f) {
					APFS_ERR("cannot create %s", dest);
					c->errors++;
				} else {
					if(write_contents(c->vol, &inode, f) < 0) {
						APFS_ERR("cannot extract %s", dest);
						c->errors++;
					}
					fclose(f);
					extract_meta(c, &inode, dest);
					if(inode.nlink > 1) {
						c->links = (link_map_t*)apfs_realloc(c->links, (c->nlinks + 1) * sizeof(link_map_t));
						c->links[c->nlinks].ino = inode.ino;
						c->links[c->nlinks].path = apfs_strdup(dest);
						c->nlinks++;
					}
				}
			}
		} else {
			uint32_t rdev = 0;
			uint16_t rs;
			const uint8_t* r = apfs_inode_xf(&inode, INO_EXT_TYPE_RDEV, &rs);
			if(r && rs >= 4)
				rdev = apfs_get32(r);
			plat_remove(dest);
			if(plat_mknod(dest, inode.mode, rdev) < 0)
				APFS_WARN("cannot create special file %s", dest);
			else
				extract_meta(c, &inode, dest);
		}
		apfs_inode_release(&inode);
		free(dest);
	}
	apfs_dirents_free(ents, n);
	return APFS_OK;
}

int apfslib_extractall(apfs_vol_t* vol, const char* path, const char* destdir, const apfslib_opts_t* o) {
	extract_ctx_t c;
	uint64_t ino;
	apfs_inode_t inode;
	size_t i;
	int ret;

	ret = apfs_lookup(vol, path, 1, &ino, NULL, NULL);
	if(ret < 0) {
		report("cannot access", path, ret);
		return ret;
	}
	ret = apfs_inode_read(vol, ino, &inode);
	if(ret < 0)
		return ret;
	if((inode.mode & APFS_S_IFMT) != APFS_S_IFDIR) {
		apfs_inode_release(&inode);
		APFS_ERR("%s: not a folder", path);
		return APFS_ENOTDIR;
	}
	apfs_inode_release(&inode);
	if(plat_mkdir(destdir, 0755) < 0) {
		APFS_ERR("cannot create %s", destdir);
		return APFS_EIO;
	}
	memset(&c, 0, sizeof(c));
	c.vol = vol;
	c.o = o;
	c.is_root = plat_is_root();
	ret = extract_tree(&c, ino, destdir);
	for(i = 0; i < c.nlinks; i++)
		free(c.links[i].path);
	free(c.links);
	if(ret == APFS_OK && c.errors)
		ret = APFS_EIO;
	return ret;
}

/* add */

static size_t file_source(void* ctx, void* buf, size_t len) {
	return fread(buf, 1, len, (FILE*)ctx);
}

static int read_all(FILE* f, uint64_t size, uint8_t** out) {
	uint8_t* buf;
	if(size > ((uint64_t)1 << 31))
		return APFS_ERANGE;
	buf = (uint8_t*)apfs_malloc((size_t)size + 1);
	if(fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		return APFS_EIO;
	}
	*out = buf;
	return APFS_OK;
}

/* Finds or creates the regular file at `dst` (inside a directory if `dst`
 * names one) and returns its inode number. */
static int prepare_file(apfs_vol_t* vol, const char* dst, const char* srcname, const apfslib_opts_t* o, uint16_t mode, uint64_t* ino_out) {
	uint64_t ino, parent;
	char* name = NULL;
	int ret;

	ret = apfs_lookup(vol, dst, 1, &ino, &parent, &name);
	if(ret == APFS_OK) {
		apfs_inode_t inode;
		ret = apfs_inode_read(vol, ino, &inode);
		if(ret < 0) {
			free(name);
			return ret;
		}
		if((inode.mode & APFS_S_IFMT) == APFS_S_IFDIR && srcname) {
			/* copy into the directory */
			apfs_inode_release(&inode);
			free(name);
			parent = ino;
			name = apfs_strdup(srcname);
			if(apfs_dir_lookup(vol, parent, name, NULL) == APFS_OK) {
				char* full = plat_join(dst, name);
				free(name);
				ret = prepare_file(vol, full, NULL, o, mode, ino_out);
				free(full);
				return ret;
			}
		} else {
			uint16_t type = inode.mode & APFS_S_IFMT;
			apfs_inode_release(&inode);
			free(name);
			if(type != APFS_S_IFREG)
				return type == APFS_S_IFDIR ? APFS_EISDIR : APFS_EEXIST;
			*ino_out = ino;
			return APFS_OK;
		}
	} else if(ret != APFS_ENOENT || !name) {
		free(name);
		return ret;
	}

	{
		apfs_create_t a;
		memset(&a, 0, sizeof(a));
		owner_for(vol, parent, o, &a.uid, &a.gid);
		a.mode = (uint16_t)(APFS_S_IFREG | (mode & 07777));
		ret = apfs_create(vol, parent, name, &a, ino_out);
	}
	free(name);
	return ret;
}

static int write_from_file(apfs_vol_t* vol, uint64_t ino, FILE* f, uint64_t size, const apfslib_opts_t* o) {
	if(o->compress != APFS_COMPRESS_NONE || o->compression_type) {
		uint8_t* data;
		int ret = read_all(f, size, &data);
		if(ret < 0)
			return ret;
		ret = o->compression_type ? apfs_file_write_compressed_type(vol, ino, data,
			(size_t)size, o->compression_type, o->resource_fork_size) :
			apfs_file_write_compressed(vol, ino, data, (size_t)size, o->compress);
		free(data);
		return ret;
	}
	return apfs_file_write_ex(vol, ino, file_source, f, size, o->sparse ? APFS_WRITE_SPARSE : 0);
}

int apfslib_add(apfs_vol_t* vol, const char* src, const char* dst, const apfslib_opts_t* o) {
	plat_stat_t st;
	FILE* f;
	uint64_t ino;
	const char* base;
	int ret;

	if(plat_lstat(src, &st) < 0 || (st.mode & APFS_S_IFMT) == APFS_S_IFDIR) {
		APFS_ERR("cannot read %s", src);
		return APFS_ENOENT;
	}
	f = fopen(src, "rb");
	if(!f) {
		APFS_ERR("cannot open %s", src);
		return APFS_EIO;
	}
	base = strrchr(src, '/');
	base = base ? base + 1 : src;
	ret = prepare_file(vol, dst, base, o, o->preserve_mode ? st.mode : o->file_mode, &ino);
	if(ret == APFS_OK)
		ret = write_from_file(vol, ino, f, st.size, o);
	fclose(f);
	if(ret == APFS_OK && o->preserve_mode) {
		apfs_setattr_t a;
		memset(&a, 0, sizeof(a));
		a.mode = st.mode & 07777;
		a.mtime = st.mtime;
		a.atime = st.atime;
		a.btime = st.btime;
		ret = apfs_setattr(vol, ino, APFS_SETATTR_MODE | APFS_SETATTR_MTIME | APFS_SETATTR_ATIME | APFS_SETATTR_BTIME, &a);
	}
	if(ret < 0)
		report("cannot add", dst, ret);
	return ret;
}

typedef struct {
	const uint8_t* p;
	size_t left;
} mem_src_t;

static size_t mem_source(void* ctx, void* buf, size_t len) {
	mem_src_t* m = (mem_src_t*)ctx;
	if(len > m->left)
		len = m->left;
	memcpy(buf, m->p, len);
	m->p += len;
	m->left -= len;
	return len;
}

int apfslib_add_data(apfs_vol_t* vol, const uint8_t* data, size_t size, const char* dst, const apfslib_opts_t* o) {
	uint64_t ino;
	int ret = prepare_file(vol, dst, NULL, o, o->file_mode, &ino);
	if(ret == APFS_OK) {
		if(o->compress != APFS_COMPRESS_NONE || o->compression_type) {
			ret = o->compression_type ? apfs_file_write_compressed_type(vol, ino, data,
				size, o->compression_type, o->resource_fork_size) :
				apfs_file_write_compressed(vol, ino, data, size, o->compress);
		} else {
			mem_src_t m;
			m.p = data;
			m.left = size;
			ret = apfs_file_write_ex(vol, ino, mem_source, &m, size, o->sparse ? APFS_WRITE_SPARSE : 0);
		}
	}
	if(ret < 0)
		report("cannot add", dst, ret);
	return ret;
}

/* addall */

typedef struct {
	uint64_t dev;
	uint64_t ino;
	uint64_t apfs_ino;
} hl_map_t;

typedef struct {
	apfs_vol_t* vol;
	const apfslib_opts_t* o;
	hl_map_t* links;
	size_t nlinks;
	int errors;
} add_ctx_t;

static void import_meta(add_ctx_t* c, const char* src, const plat_stat_t* st, uint64_t ino, int is_link) {
	apfs_setattr_t a;
	int mask = APFS_SETATTR_MTIME | APFS_SETATTR_ATIME | APFS_SETATTR_BTIME;

	memset(&a, 0, sizeof(a));
	a.mtime = st->mtime;
	a.atime = st->atime;
	a.btime = st->btime;
	if(!is_link) {
		a.mode = st->mode & 07777;
		mask |= APFS_SETATTR_MODE;
	}
	if(c->o->preserve_owner) {
		a.uid = st->uid;
		a.gid = st->gid;
		mask |= APFS_SETATTR_UID | APFS_SETATTR_GID;
	}
	if(st->flags & ~(uint32_t)APFS_UF_COMPRESSED) {
		a.bsd_flags = st->flags & ~(uint32_t)APFS_UF_COMPRESSED;
		mask |= APFS_SETATTR_FLAGS;
	}
	apfs_setattr(c->vol, ino, mask, &a);

	if(c->o->xattrs) {
		char** names;
		size_t n, i;
		if(plat_xattr_list(src, &names, &n) == APFS_OK) {
			for(i = 0; i < n; i++) {
				uint8_t* d;
				size_t len;
				if(!strcmp(names[i], DECMPFS_EA_NAME) || !strcmp(names[i], SYMLINK_EA_NAME))
					continue;
				if(plat_xattr_get(src, names[i], &d, &len) == APFS_OK) {
					apfs_xattr_set(c->vol, ino, names[i], d, len, 0);
					free(d);
				}
			}
			plat_free_list(names, n);
		}
	}
}

static int add_tree(add_ctx_t* c, const char* srcdir, uint64_t dir, const char* dstpath) {
	char** names;
	size_t n, i;
	int ret = plat_list_dir(srcdir, &names, &n);
	if(ret < 0) {
		APFS_ERR("cannot read directory %s", srcdir);
		c->errors++;
		return ret;
	}
	for(i = 0; i < n; i++) {
		char* src = plat_join(srcdir, names[i]);
		char* dst = plat_join(dstpath, names[i]);
		plat_stat_t st;
		apfs_dirent_t de;
		int exists;
		uint16_t type;
		uint64_t ino = 0;

		if(plat_lstat(src, &st) < 0) {
			APFS_ERR("cannot stat %s", src);
			c->errors++;
			goto next;
		}
		type = st.mode & APFS_S_IFMT;
		exists = apfs_dir_lookup(c->vol, dir, names[i], &de) == APFS_OK;

		if(type == APFS_S_IFDIR) {
			if(!c->o->quiet)
				printf("folder: %s\n", dst);
			if(exists && (de.flags & DREC_TYPE_MASK) != APFS_DT_DIR) {
				apfs_unlink(c->vol, dir, names[i]);
				exists = 0;
			}
			if(exists) {
				ino = de.ino;
			} else {
				apfs_create_t a;
				memset(&a, 0, sizeof(a));
				owner_for(c->vol, dir, c->o, &a.uid, &a.gid);
				a.mode = (uint16_t)(APFS_S_IFDIR | (st.mode & 07777));
				ret = apfs_create(c->vol, dir, names[i], &a, &ino);
				if(ret < 0) {
					report("cannot create", dst, ret);
					c->errors++;
					goto next;
				}
			}
			add_tree(c, src, ino, dst);
			import_meta(c, src, &st, ino, 0);
		} else {
			size_t k;
			int linked = 0;

			if(exists) {
				int replace = (type != APFS_S_IFREG) || ((de.flags & DREC_TYPE_MASK) != APFS_DT_REG);
				if((de.flags & DREC_TYPE_MASK) == APFS_DT_DIR) {
					APFS_ERR("cannot replace directory %s with a file", dst);
					c->errors++;
					apfs_dirent_release(&de);
					goto next;
				}
				if(replace) {
					apfs_unlink(c->vol, dir, names[i]);
					exists = 0;
				} else {
					ino = de.ino;
				}
			}

			/* hard links within the imported tree */
			if(st.nlink > 1 && type != APFS_S_IFDIR) {
				for(k = 0; k < c->nlinks; k++) {
					if(c->links[k].dev == st.dev && c->links[k].ino == st.ino) {
						if(exists) {
							apfs_unlink(c->vol, dir, names[i]);
							exists = 0;
						}
						ret = apfs_link(c->vol, c->links[k].apfs_ino, dir, names[i]);
						if(ret < 0) {
							report("cannot link", dst, ret);
							c->errors++;
						} else if(!c->o->quiet) {
							printf("link: %s\n", dst);
						}
						linked = 1;
						break;
					}
				}
			}
			if(linked)
				goto next_meta;

			if(type == APFS_S_IFLNK) {
				char* target;
				apfs_create_t a;
				if(!c->o->quiet)
					printf("link: %s\n", dst);
				if(plat_readlink(src, &target) < 0) {
					APFS_ERR("cannot read link %s", src);
					c->errors++;
					goto next;
				}
				memset(&a, 0, sizeof(a));
				owner_for(c->vol, dir, c->o, &a.uid, &a.gid);
				a.mode = (uint16_t)(APFS_S_IFLNK | (st.mode & 07777));
				ret = apfs_symlink(c->vol, dir, names[i], target, &a, &ino);
				free(target);
				if(ret < 0) {
					report("cannot create link", dst, ret);
					c->errors++;
					goto next;
				}
				import_meta(c, src, &st, ino, 1);
			} else if(type == APFS_S_IFREG) {
				FILE* f;
				if(!c->o->quiet)
					printf("file: %s\n", dst);
				if(!exists) {
					apfs_create_t a;
					memset(&a, 0, sizeof(a));
					owner_for(c->vol, dir, c->o, &a.uid, &a.gid);
					a.mode = (uint16_t)(APFS_S_IFREG | (st.mode & 07777));
					ret = apfs_create(c->vol, dir, names[i], &a, &ino);
					if(ret < 0) {
						report("cannot create", dst, ret);
						c->errors++;
						goto next;
					}
				}
				f = fopen(src, "rb");
				if(!f) {
					APFS_ERR("cannot open %s", src);
					c->errors++;
					goto next;
				}
				apfslib_opts_t file_options = *c->o;
				if (c->o->compression_for_path)
					file_options.compress = c->o->compression_for_path(dst, c->o->compression_context);
				ret = c->o->compression_layout_for_path ?
					c->o->compression_layout_for_path(dst, c->o->compression_context,
						&file_options.compression_type, &file_options.resource_fork_size) : APFS_OK;
				if(ret >= 0) ret = write_from_file(c->vol, ino, f, st.size, &file_options);
				fclose(f);
				if(ret < 0) {
					report("cannot write", dst, ret);
					c->errors++;
					if(ret == APFS_ENOSPC) {
						free(src);
						free(dst);
						plat_free_list(names, n);
						return ret;
					}
					goto next;
				}
				import_meta(c, src, &st, ino, 0);
			} else {
				apfs_create_t a;
				if(!c->o->quiet)
					printf("special: %s\n", dst);
				memset(&a, 0, sizeof(a));
				owner_for(c->vol, dir, c->o, &a.uid, &a.gid);
				a.mode = st.mode;
				a.rdev = st.rdev;
				ret = apfs_create(c->vol, dir, names[i], &a, &ino);
				if(ret < 0) {
					report("cannot create", dst, ret);
					c->errors++;
					goto next;
				}
				import_meta(c, src, &st, ino, 0);
			}

			if(st.nlink > 1) {
				c->links = (hl_map_t*)apfs_realloc(c->links, (c->nlinks + 1) * sizeof(hl_map_t));
				c->links[c->nlinks].dev = st.dev;
				c->links[c->nlinks].ino = st.ino;
				c->links[c->nlinks].apfs_ino = ino;
				c->nlinks++;
			}
		}
next_meta:
		if(exists)
			apfs_dirent_release(&de);
next:
		free(src);
		free(dst);
	}
	plat_free_list(names, n);
	return APFS_OK;
}

int apfslib_addall(apfs_vol_t* vol, const char* srcdir, const char* dst, const apfslib_opts_t* o) {
	add_ctx_t c;
	uint64_t ino;
	apfs_inode_t inode;
	plat_stat_t st;
	int ret;

	if(plat_lstat(srcdir, &st) < 0 || (st.mode & APFS_S_IFMT) != APFS_S_IFDIR) {
		APFS_ERR("cannot open that directory: %s", srcdir);
		return APFS_ENOENT;
	}
	ret = apfs_lookup(vol, dst, 1, &ino, NULL, NULL);
	if(ret < 0) {
		report("cannot access", dst, ret);
		return ret;
	}
	ret = apfs_inode_read(vol, ino, &inode);
	if(ret < 0)
		return ret;
	if((inode.mode & APFS_S_IFMT) != APFS_S_IFDIR) {
		apfs_inode_release(&inode);
		APFS_ERR("%s: not a folder", dst);
		return APFS_ENOTDIR;
	}
	apfs_inode_release(&inode);

	memset(&c, 0, sizeof(c));
	c.vol = vol;
	c.o = o;
	ret = add_tree(&c, srcdir, ino, strcmp(dst, "/") == 0 ? "" : dst);
	free(c.links);
	if(ret == APFS_OK && c.errors)
		ret = APFS_EIO;
	return ret;
}

/* Simple modifications */

int apfslib_mkdir(apfs_vol_t* vol, const char* path, int parents, const apfslib_opts_t* o) {
	uint64_t parent, ino;
	char* name;
	apfs_create_t a;
	int ret;

	if(parents) {
		char* copy = apfs_strdup(path);
		char* p;
		for(p = copy + 1; *p; p++) {
			if(*p != '/')
				continue;
			*p = 0;
			if(apfs_lookup(vol, copy, 1, &ino, NULL, NULL) == APFS_ENOENT) {
				ret = apfslib_mkdir(vol, copy, 0, o);
				if(ret < 0) {
					free(copy);
					return ret;
				}
			}
			*p = '/';
		}
		free(copy);
		if(apfs_lookup(vol, path, 1, &ino, NULL, NULL) == APFS_OK)
			return APFS_OK;
	}

	ret = resolve_parent(vol, path, &parent, &name);
	if(ret < 0) {
		report("cannot create directory", path, ret);
		return ret;
	}
	memset(&a, 0, sizeof(a));
	owner_for(vol, parent, o, &a.uid, &a.gid);
	a.mode = (uint16_t)(APFS_S_IFDIR | (o->dir_mode & 07777));
	ret = apfs_create(vol, parent, name, &a, &ino);
	free(name);
	if(ret < 0)
		report("cannot create directory", path, ret);
	return ret;
}

static int rm_tree(apfs_vol_t* vol, uint64_t dir, const char* path, int quiet) {
	apfs_dirent_t* ents;
	size_t n, i;
	int ret = apfs_dir_list(vol, dir, &ents, &n);
	if(ret < 0)
		return ret;
	for(i = 0; i < n && ret == APFS_OK; i++) {
		char* sub = plat_join(path, ents[i].name);
		if((ents[i].flags & DREC_TYPE_MASK) == APFS_DT_DIR)
			ret = rm_tree(vol, ents[i].ino, sub, quiet);
		if(ret == APFS_OK) {
			if(!quiet)
				printf("%s\n", sub);
			ret = apfs_unlink(vol, dir, ents[i].name);
			if(ret < 0)
				report("cannot remove", sub, ret);
		}
		free(sub);
	}
	apfs_dirents_free(ents, n);
	return ret;
}

int apfslib_rm(apfs_vol_t* vol, const char* path, int recursive, int quiet) {
	uint64_t parent, ino;
	char* name;
	apfs_dirent_t de;
	int ret;

	ret = resolve_parent(vol, path, &parent, &name);
	if(ret < 0) {
		report("cannot remove", path, ret);
		return ret;
	}
	ret = apfs_dir_lookup(vol, parent, name, &de);
	if(ret < 0) {
		report("cannot remove", path, ret);
		free(name);
		return ret;
	}
	ino = de.ino;
	if(recursive && (de.flags & DREC_TYPE_MASK) == APFS_DT_DIR)
		ret = rm_tree(vol, ino, path, quiet);
	apfs_dirent_release(&de);
	if(ret == APFS_OK)
		ret = apfs_unlink(vol, parent, name);
	if(ret < 0)
		report("cannot remove", path, ret);
	free(name);
	return ret;
}

int apfslib_rmall(apfs_vol_t* vol, const char* path, int quiet) {
	uint64_t ino;
	int ret = apfs_lookup(vol, path, 1, &ino, NULL, NULL);
	if(ret < 0) {
		report("cannot access", path, ret);
		return ret;
	}
	if(ino == ROOT_DIR_INO_NUM)
		return rm_tree(vol, ino, "", quiet);
	return apfslib_rm(vol, path, 1, quiet);
}

int apfslib_mv(apfs_vol_t* vol, const char* src, const char* dst) {
	uint64_t sparent, dparent, ino;
	char* sname = NULL;
	char* dname = NULL;
	int ret;

	ret = resolve_parent(vol, src, &sparent, &sname);
	if(ret < 0) {
		report("cannot move", src, ret);
		return ret;
	}
	/* moving onto an existing directory moves into it */
	if(apfs_lookup(vol, dst, 1, &ino, NULL, NULL) == APFS_OK) {
		apfs_inode_t inode;
		if(apfs_inode_read(vol, ino, &inode) == APFS_OK) {
			int isdir = (inode.mode & APFS_S_IFMT) == APFS_S_IFDIR;
			apfs_inode_release(&inode);
			if(isdir) {
				apfs_dirent_t de;
				if(apfs_dir_lookup(vol, sparent, sname, &de) == APFS_OK) {
					int same = de.ino == ino;
					apfs_dirent_release(&de);
					if(!same) {
						ret = apfs_rename(vol, sparent, sname, ino, sname);
						free(sname);
						if(ret < 0)
							report("cannot move", src, ret);
						return ret;
					}
				}
			}
		}
	}
	ret = resolve_parent(vol, dst, &dparent, &dname);
	if(ret == APFS_OK)
		ret = apfs_rename(vol, sparent, sname, dparent, dname);
	if(ret < 0)
		report("cannot move", src, ret);
	free(sname);
	free(dname);
	return ret;
}

int apfslib_symlink(apfs_vol_t* vol, const char* path, const char* target, const apfslib_opts_t* o) {
	uint64_t parent, ino;
	char* name;
	apfs_create_t a;
	int ret;

	ret = resolve_parent(vol, path, &parent, &name);
	if(ret < 0) {
		report("cannot create link", path, ret);
		return ret;
	}
	if(apfs_dir_lookup(vol, parent, name, NULL) == APFS_OK) {
		/* replace an existing link, like hfsplus */
		apfs_dirent_t de;
		apfs_dir_lookup(vol, parent, name, &de);
		if((de.flags & DREC_TYPE_MASK) == APFS_DT_LNK)
			apfs_unlink(vol, parent, name);
		apfs_dirent_release(&de);
	}
	memset(&a, 0, sizeof(a));
	owner_for(vol, parent, o, &a.uid, &a.gid);
	a.mode = APFS_S_IFLNK | 0755;
	ret = apfs_symlink(vol, parent, name, target, &a, &ino);
	free(name);
	if(ret < 0)
		report("cannot create link", path, ret);
	return ret;
}

int apfslib_link(apfs_vol_t* vol, const char* existing, const char* path) {
	uint64_t ino, parent;
	char* name;
	int ret;

	ret = apfs_lookup(vol, existing, 0, &ino, NULL, NULL);
	if(ret < 0) {
		report("cannot access", existing, ret);
		return ret;
	}
	ret = resolve_parent(vol, path, &parent, &name);
	if(ret == APFS_OK) {
		ret = apfs_link(vol, ino, parent, name);
		free(name);
	}
	if(ret < 0)
		report("cannot link", path, ret);
	return ret;
}

int apfslib_mknod(apfs_vol_t* vol, const char* path, uint16_t mode, uint32_t rdev, const apfslib_opts_t* o) {
	uint64_t parent, ino;
	char* name;
	apfs_create_t a;
	int ret;

	ret = resolve_parent(vol, path, &parent, &name);
	if(ret < 0) {
		report("cannot create", path, ret);
		return ret;
	}
	memset(&a, 0, sizeof(a));
	owner_for(vol, parent, o, &a.uid, &a.gid);
	a.mode = mode;
	a.rdev = rdev;
	ret = apfs_create(vol, parent, name, &a, &ino);
	free(name);
	if(ret < 0)
		report("cannot create", path, ret);
	return ret;
}

typedef int (*inode_fn)(apfs_vol_t* vol, uint64_t ino, void* ctx);

static int apply_recursive(apfs_vol_t* vol, uint64_t ino, int recursive, inode_fn fn, void* ctx) {
	int ret = fn(vol, ino, ctx);
	if(ret < 0 || !recursive)
		return ret;
	{
		apfs_dirent_t* ents;
		size_t n, i;
		if(apfs_dir_list(vol, ino, &ents, &n) < 0)
			return APFS_OK; /* not a directory */
		for(i = 0; i < n && ret == APFS_OK; i++) {
			if((ents[i].flags & DREC_TYPE_MASK) == APFS_DT_DIR)
				ret = apply_recursive(vol, ents[i].ino, 1, fn, ctx);
			else
				ret = fn(vol, ents[i].ino, ctx);
		}
		apfs_dirents_free(ents, n);
	}
	return ret;
}

static int chmod_fn(apfs_vol_t* vol, uint64_t ino, void* ctx) {
	apfs_setattr_t a;
	memset(&a, 0, sizeof(a));
	a.mode = *(uint16_t*)ctx;
	return apfs_setattr(vol, ino, APFS_SETATTR_MODE, &a);
}

int apfslib_chmod(apfs_vol_t* vol, const char* path, uint16_t mode, int recursive) {
	uint64_t ino;
	int ret = apfs_lookup(vol, path, 0, &ino, NULL, NULL);
	if(ret == APFS_OK)
		ret = apply_recursive(vol, ino, recursive, chmod_fn, &mode);
	if(ret < 0)
		report("cannot chmod", path, ret);
	return ret;
}

static int chown_fn(apfs_vol_t* vol, uint64_t ino, void* ctx) {
	apfs_setattr_t a;
	memset(&a, 0, sizeof(a));
	a.uid = ((uint32_t*)ctx)[0];
	a.gid = ((uint32_t*)ctx)[1];
	return apfs_setattr(vol, ino, APFS_SETATTR_UID | APFS_SETATTR_GID, &a);
}

int apfslib_chown(apfs_vol_t* vol, const char* path, uint32_t uid, uint32_t gid, int recursive) {
	uint64_t ino;
	uint32_t ids[2];
	int ret = apfs_lookup(vol, path, 0, &ino, NULL, NULL);
	ids[0] = uid;
	ids[1] = gid;
	if(ret == APFS_OK)
		ret = apply_recursive(vol, ino, recursive, chown_fn, ids);
	if(ret < 0)
		report("cannot chown", path, ret);
	return ret;
}

static int chflags_fn(apfs_vol_t* vol, uint64_t ino, void* ctx) {
	uint32_t* sc = (uint32_t*)ctx;
	apfs_inode_t inode;
	apfs_setattr_t a;
	int ret = apfs_inode_read(vol, ino, &inode);
	if(ret < 0)
		return ret;
	memset(&a, 0, sizeof(a));
	a.bsd_flags = (inode.bsd_flags | sc[0]) & ~sc[1];
	apfs_inode_release(&inode);
	return apfs_setattr(vol, ino, APFS_SETATTR_FLAGS, &a);
}

int apfslib_chflags(apfs_vol_t* vol, const char* path, uint32_t set, uint32_t clear, int recursive) {
	uint64_t ino;
	uint32_t sc[2];
	int ret = apfs_lookup(vol, path, 0, &ino, NULL, NULL);
	sc[0] = set;
	sc[1] = clear;
	if(ret == APFS_OK)
		ret = apply_recursive(vol, ino, recursive, chflags_fn, sc);
	if(ret < 0)
		report("cannot chflags", path, ret);
	return ret;
}

int apfslib_touch(apfs_vol_t* vol, const char* path, uint64_t t, const apfslib_opts_t* o) {
	uint64_t ino, parent;
	char* name = NULL;
	apfs_setattr_t a;
	int ret = apfs_lookup(vol, path, 1, &ino, &parent, &name);
	if(ret == APFS_ENOENT && name) {
		apfs_create_t c;
		memset(&c, 0, sizeof(c));
		owner_for(vol, parent, o, &c.uid, &c.gid);
		c.mode = (uint16_t)(APFS_S_IFREG | (o->file_mode & 07777));
		c.time = t;
		ret = apfs_create(vol, parent, name, &c, &ino);
	}
	free(name);
	if(ret == APFS_OK) {
		memset(&a, 0, sizeof(a));
		a.mtime = a.atime = t ? t : apfs_time_now();
		ret = apfs_setattr(vol, ino, APFS_SETATTR_MTIME | APFS_SETATTR_ATIME, &a);
	}
	if(ret < 0)
		report("cannot touch", path, ret);
	return ret;
}

/* Extended attributes */

int apfslib_getattr(apfs_vol_t* vol, const char* path, const char* name, FILE* out) {
	uint64_t ino;
	uint8_t* data;
	size_t size;
	int ret = apfs_lookup(vol, path, 0, &ino, NULL, NULL);
	if(ret < 0) {
		report("cannot access", path, ret);
		return ret;
	}
	ret = apfs_xattr_get(vol, ino, name, &data, &size);
	if(ret < 0) {
		APFS_ERR("%s: no such attribute %s", path, name);
		return ret;
	}
	fwrite(data, 1, size, out);
	free(data);
	return APFS_OK;
}

int apfslib_setattr(apfs_vol_t* vol, const char* path, const char* name, const uint8_t* data, size_t size) {
	uint64_t ino;
	int ret = apfs_lookup(vol, path, 0, &ino, NULL, NULL);
	if(ret == APFS_OK) {
		if(!strcmp(name, DECMPFS_EA_NAME) || !strcmp(name, SYMLINK_EA_NAME))
			APFS_WARN("setting the file-system owned attribute %s by hand", name);
		ret = apfs_xattr_set(vol, ino, name, data, size, !strcmp(name, SYMLINK_EA_NAME));
	}
	if(ret < 0)
		report("cannot set attribute on", path, ret);
	return ret;
}

int apfslib_rmattr(apfs_vol_t* vol, const char* path, const char* name) {
	uint64_t ino;
	int ret = apfs_lookup(vol, path, 0, &ino, NULL, NULL);
	if(ret == APFS_OK)
		ret = apfs_xattr_remove(vol, ino, name);
	if(ret < 0)
		report("cannot remove attribute from", path, ret);
	return ret;
}

int apfslib_listattr(apfs_vol_t* vol, const char* path, FILE* out) {
	uint64_t ino;
	char** names;
	size_t n, i;
	int ret = apfs_lookup(vol, path, 0, &ino, NULL, NULL);
	if(ret == APFS_OK)
		ret = apfs_xattr_list(vol, ino, &names, &n);
	if(ret < 0) {
		report("cannot list attributes of", path, ret);
		return ret;
	}
	for(i = 0; i < n; i++) {
		uint8_t* d;
		size_t len;
		if(apfs_xattr_get(vol, ino, names[i], &d, &len) == APFS_OK) {
			fprintf(out, "%s\t%lu\n", names[i], (unsigned long)len);
			free(d);
		} else {
			fprintf(out, "%s\t?\n", names[i]);
		}
	}
	apfs_names_free(names, n);
	return APFS_OK;
}

int apfslib_readlink(apfs_vol_t* vol, const char* path, FILE* out) {
	uint64_t ino;
	char* target;
	int ret = apfs_lookup(vol, path, 0, &ino, NULL, NULL);
	if(ret == APFS_OK)
		ret = apfs_readlink(vol, ino, &target);
	if(ret < 0) {
		report("cannot read link", path, ret);
		return ret;
	}
	fprintf(out, "%s\n", target);
	free(target);
	return APFS_OK;
}

/* Compression of existing files */

typedef struct {
	int algo;
	int quiet;
} compress_ctx_t;

static int compress_fn(apfs_vol_t* vol, uint64_t ino, void* ctx) {
	compress_ctx_t* c = (compress_ctx_t*)ctx;
	apfs_inode_t inode;
	uint64_t size;
	uint8_t* data;
	size_t got;
	int ret = apfs_inode_read(vol, ino, &inode);
	if(ret < 0)
		return ret;
	if((inode.mode & APFS_S_IFMT) != APFS_S_IFREG || inode.nlink > 1) {
		apfs_inode_release(&inode);
		return APFS_OK;
	}
	apfs_file_size(vol, &inode, &size);
	if(size > ((uint64_t)1 << 31)) {
		apfs_inode_release(&inode);
		return APFS_OK;
	}
	data = (uint8_t*)apfs_malloc((size_t)size + 1);
	ret = apfs_file_read(vol, &inode, 0, data, (size_t)size, &got);
	apfs_inode_release(&inode);
	if(ret == APFS_OK && got == size) {
		if(c->algo == APFS_COMPRESS_NONE) {
			mem_src_t m;
			m.p = data;
			m.left = (size_t)size;
			ret = apfs_file_write(vol, ino, mem_source, &m, size);
		} else {
			ret = apfs_file_write_compressed(vol, ino, data, (size_t)size, c->algo);
		}
	} else if(ret == APFS_OK) {
		ret = APFS_ECORRUPT;
	}
	free(data);
	return ret;
}

int apfslib_compress(apfs_vol_t* vol, const char* path, int algo, int recursive, int quiet) {
	uint64_t ino;
	compress_ctx_t c;
	int ret = apfs_lookup(vol, path, 1, &ino, NULL, NULL);
	c.algo = algo;
	c.quiet = quiet;
	if(ret == APFS_OK)
		ret = apply_recursive(vol, ino, recursive, compress_fn, &c);
	if(ret < 0)
		report("cannot compress", path, ret);
	return ret;
}

/* SHA-256 of every file below a path, for verification */

static int hash_tree(apfs_vol_t* vol, uint64_t dir, const char* path, FILE* out) {
	apfs_dirent_t* ents;
	size_t n, i;
	int ret = apfs_dir_list(vol, dir, &ents, &n);
	if(ret < 0)
		return ret;
	for(i = 0; i < n; i++) {
		char* sub = plat_join(path, ents[i].name);
		uint16_t t = ents[i].flags & DREC_TYPE_MASK;
		if(t == APFS_DT_DIR) {
			hash_tree(vol, ents[i].ino, sub, out);
		} else if(t == APFS_DT_REG) {
			apfs_inode_t inode;
			if(apfs_inode_read(vol, ents[i].ino, &inode) == APFS_OK) {
				apfs_sha256_ctx ctx;
				uint8_t dg[32];
				uint64_t size, off = 0;
				uint8_t* buf = (uint8_t*)apfs_malloc(IO_CHUNK);
				int k, err = 0;
				apfs_file_size(vol, &inode, &size);
				apfs_sha256_init(&ctx);
				while(off < size) {
					size_t got;
					if(apfs_file_read(vol, &inode, off, buf, IO_CHUNK, &got) < 0 || !got) {
						err = 1;
						break;
					}
					apfs_sha256_update(&ctx, buf, got);
					off += got;
				}
				apfs_sha256_final(&ctx, dg);
				free(buf);
				for(k = 0; k < 32; k++)
					fprintf(out, "%02x", dg[k]);
				fprintf(out, "  %s%s\n", sub, err ? " (READ ERROR)" : "");
				apfs_inode_release(&inode);
			}
		}
		free(sub);
	}
	apfs_dirents_free(ents, n);
	return APFS_OK;
}

int apfslib_hash(apfs_vol_t* vol, const char* path, FILE* out) {
	uint64_t ino;
	int ret = apfs_lookup(vol, path, 1, &ino, NULL, NULL);
	if(ret < 0) {
		report("cannot access", path, ret);
		return ret;
	}
	return hash_tree(vol, ino, strcmp(path, "/") == 0 ? "." : path, out);
}
