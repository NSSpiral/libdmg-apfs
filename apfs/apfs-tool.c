/*
 * apfs-tool.c
 * apfs-tool
 *
 * Command-line tool for APFS images, modelled on libdmg-hfsplus' hfsplus
 * and hdutil tools: it accepts the same commands, plus APFS-specific ones.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "apfs/apfs.h"
#include "apfs/apfslib.h"
#include "apfs/image.h"
#include "apfs/debug.h"
#include "apfs/udif.h"
#include "apfs/platform.h"
#include "apfs/trustcache.h"
#include "apfs/mtree.h"
#include "lzfse.h"
#include "apfs/manifest.h"
#include "apfs/volume-export.h"

#include "apfs/fit.h"

#define APFS_TOOL_VERSION "1.0"

char endianness = IS_LITTLE_ENDIAN;

static void TestByteOrder(void) {
	short int word = 0x0001;
	char* byte = (char*)&word;
	endianness = byte[0] ? IS_LITTLE_ENDIAN : IS_BIG_ENDIAN;
}

typedef struct {
	apfs_image_t* img;
	apfs_vol_t* vol;
	apfslib_opts_t opts;
	const char* image_path;
} ctx_t;

static int parse_size(const char* s, uint64_t* out) {
	char* end;
	double v = strtod(s, &end);
	uint64_t mult = 1;
	if(end == s || v < 0)
		return APFS_EINVAL;
	switch(*end) {
		case 0: break;
		case 'b': case 'B': mult = 1; break;
		case 'k': case 'K': mult = 1024ULL; break;
		case 'm': case 'M': mult = 1024ULL * 1024; break;
		case 'g': case 'G': mult = 1024ULL * 1024 * 1024; break;
		case 't': case 'T': mult = 1024ULL * 1024 * 1024 * 1024; break;
		case 's': case 'S': mult = 512; break;
		default: return APFS_EINVAL;
	}
	if(*end) {
		const char* rest = end + 1;
		if(*rest == 'i')
			rest++;
		if(*rest == 'b' || *rest == 'B')
			rest++;
		if(*rest)
			return APFS_EINVAL;
	}
	*out = (uint64_t)(v * (double)mult);
	return APFS_OK;
}

static int parse_owner(const char* s, uint32_t* uid, uint32_t* gid) {
	char* end;
	*uid = (uint32_t)strtoul(s, &end, 10);
	if(end == s)
		return APFS_EINVAL;
	if(*end == ':') {
		const char* g = end + 1;
		*gid = (uint32_t)strtoul(g, &end, 10);
		if(end == g)
			return APFS_EINVAL;
	}
	return *end ? APFS_EINVAL : APFS_OK;
}

static int parse_mode(const char* s, uint16_t* mode) {
	char* end;
	unsigned long v = strtoul(s, &end, 8);
	if(end == s || *end || v > 07777)
		return APFS_EINVAL;
	*mode = (uint16_t)v;
	return APFS_OK;
}

static int need(int argc, int n, const char* usage) {
	if(argc < n) {
		fprintf(stderr, "usage: %s\n", usage);
		return 0;
	}
	return 1;
}

/* Common per-command options. Returns the index of the first argument. */
static int parse_opts(ctx_t* c, int argc, char** argv, const char* allowed, int* recursive) {
	int i = 1;
	if(recursive)
		*recursive = 0;
	while(i < argc && argv[i][0] == '-' && argv[i][1]) {
		const char* a = argv[i];
		if(!strcmp(a, "--")) {
			i++;
			break;
		}
		if(strchr(allowed, a[1]) == NULL || a[2]) {
			fprintf(stderr, "error: unknown option %s\n", a);
			return -1;
		}
		switch(a[1]) {
			case 'R':
			case 'r':
			case 'p':
				if(recursive)
					*recursive = 1;
				if(a[1] == 'p')
					c->opts.preserve_mode = 1;
				break;
			case 'P':
				c->opts.preserve_owner = 1;
				break;
			case 'X':
				c->opts.xattrs = 0;
				break;
			case 's':
				c->opts.sparse = 1;
				break;
			case 'c':
				if(i + 1 >= argc || apfs_compress_parse(argv[i + 1], &c->opts.compress) < 0) {
					fprintf(stderr, "error: -c needs zlib, lzvn, lzfse or none\n");
					return -1;
				}
				i++;
				break;
			case 'm':
				if(i + 1 >= argc || parse_mode(argv[i + 1], &c->opts.file_mode) < 0) {
					fprintf(stderr, "error: -m needs an octal mode\n");
					return -1;
				}
				c->opts.dir_mode = c->opts.file_mode;
				i++;
				break;
			case 'O':
				if(i + 1 >= argc || parse_owner(argv[i + 1], &c->opts.uid, &c->opts.gid) < 0) {
					fprintf(stderr, "error: -O needs uid:gid\n");
					return -1;
				}
				i++;
				break;
			case 'q':
				c->opts.quiet = 1;
				break;
		}
		i++;
	}
	return i;
}

/* hfsplus-compatible commands */

static int cmd_ls(ctx_t* c, int argc, char** argv) {
	int flags = 0, i = 1;
	while(i < argc && argv[i][0] == '-' && argv[i][1]) {
		const char* p = argv[i] + 1;
		for(; *p; p++) {
			if(*p == 'R') flags |= APFSLIB_LS_RECURSIVE;
			else if(*p == 'i') flags |= APFSLIB_LS_INODES;
			else if(*p == 'l' || *p == 'a') ;
			else {
				fprintf(stderr, "error: unknown option -%c\n", *p);
				return 1;
			}
		}
		i++;
	}
	return apfslib_ls(c->vol, i < argc ? argv[i] : "/", flags, stdout) < 0;
}

static int cmd_cat(ctx_t* c, int argc, char** argv) {
	if(!need(argc, 2, "cat <path>"))
		return 1;
	return apfslib_cat(c->vol, argv[1], stdout) < 0;
}

/* Refuses an output file that is the image itself, which writing would
 * destroy */
static int is_image(ctx_t* c, const char* path) {
	if(!plat_same_file(path, c->image_path))
		return 0;
	fprintf(stderr, "error: %s is the image itself\n", path);
	return 1;
}

static int cmd_extract(ctx_t* c, int argc, char** argv) {
	if(!need(argc, 3, "extract <path> <dest>") || is_image(c, argv[2]))
		return 1;
	return apfslib_extract(c->vol, argv[1], argv[2]) < 0;
}

static int cmd_extractall(ctx_t* c, int argc, char** argv) {
	if(argc == 4 && !strcmp(argv[2], "--manifest"))
		return apfs_manifest_export(c->vol, argv[1], argv[3]) < 0;
	int i = parse_opts(c, argc, argv, "Xq", NULL);
	if(i < 0)
		return 1;
	return apfslib_extractall(c->vol, i < argc ? argv[i] : "/", i + 1 < argc ? argv[i + 1] : ".", &c->opts) < 0;
}

static int cmd_mv(ctx_t* c, int argc, char** argv) {
	if(!need(argc, 3, "mv <src> <dst>"))
		return 1;
	return apfslib_mv(c->vol, argv[1], argv[2]) < 0;
}

static int cmd_symlink(ctx_t* c, int argc, char** argv) {
	int i = parse_opts(c, argc, argv, "O", NULL);
	if(i < 0 || !need(argc - i + 1, 3, "symlink [-O uid:gid] <path> <target>"))
		return 1;
	return apfslib_symlink(c->vol, argv[i], argv[i + 1], &c->opts) < 0;
}

static int cmd_mkdir(ctx_t* c, int argc, char** argv) {
	int parents;
	int i = parse_opts(c, argc, argv, "pmO", &parents);
	if(i < 0 || !need(argc - i + 1, 2, "mkdir [-p] [-m mode] [-O uid:gid] <path>"))
		return 1;
	c->opts.preserve_mode = 0;
	for(; i < argc; i++)
		if(apfslib_mkdir(c->vol, argv[i], parents, &c->opts) < 0)
			return 1;
	return 0;
}

static int cmd_add(ctx_t* c, int argc, char** argv) {
	int i = parse_opts(c, argc, argv, "cspmO", NULL);
	if(i < 0 || !need(argc - i + 1, 3, "add [-c algo] [-s] [-p] [-m mode] [-O uid:gid] <src> <dst>"))
		return 1;
	return apfslib_add(c->vol, argv[i], argv[i + 1], &c->opts) < 0;
}

static int cmd_rm(ctx_t* c, int argc, char** argv) {
	int recursive;
	int i = parse_opts(c, argc, argv, "rRq", &recursive);
	if(i < 0 || !need(argc - i + 1, 2, "rm [-r] <path>"))
		return 1;
	for(; i < argc; i++)
		if(apfslib_rm(c->vol, argv[i], recursive, 1) < 0)
			return 1;
	return 0;
}

static int cmd_chmod(ctx_t* c, int argc, char** argv) {
	int recursive;
	uint16_t mode;
	int i = parse_opts(c, argc, argv, "R", &recursive);
	if(i < 0 || !need(argc - i + 1, 3, "chmod [-R] <mode> <path>"))
		return 1;
	if(parse_mode(argv[i], &mode) < 0) {
		fprintf(stderr, "error: invalid mode %s\n", argv[i]);
		return 1;
	}
	for(i++; i < argc; i++)
		if(apfslib_chmod(c->vol, argv[i], mode, recursive) < 0)
			return 1;
	return 0;
}

static int cmd_chown(ctx_t* c, int argc, char** argv) {
	int recursive;
	uint32_t uid = 0, gid = 0;
	int i = parse_opts(c, argc, argv, "R", &recursive);
	if(i < 0 || !need(argc - i + 1, 3, "chown [-R] <uid>:<gid> <path>"))
		return 1;
	if(parse_owner(argv[i], &uid, &gid) < 0) {
		fprintf(stderr, "error: invalid owner %s\n", argv[i]);
		return 1;
	}
	for(i++; i < argc; i++)
		if(apfslib_chown(c->vol, argv[i], uid, gid, recursive) < 0)
			return 1;
	return 0;
}

static int cmd_rmall(ctx_t* c, int argc, char** argv) {
	return apfslib_rmall(c->vol, argc > 1 ? argv[1] : "/", c->opts.quiet) < 0;
}

static int cmd_addall(ctx_t* c, int argc, char** argv) {
	int legacy_hardlinks = argc > 1 && !strcmp(argv[1], "--legacy-hardlinks");
	if(legacy_hardlinks) {
		c->vol->sibling_maps = 0;
		argc--;
		argv++;
	}
	if(argc == 5 && !strcmp(argv[2], "/") && !strcmp(argv[3], "--manifest"))
		return apfs_manifest_import(c->vol, argv[1], argv[4]) < 0;
	if(argc == 5 && !strcmp(argv[2], "/") && !strcmp(argv[3], "--metadata"))
		return apfs_metadata_import(c->vol, argv[1], argv[4]) < 0;
	int i = parse_opts(c, argc, argv, "csPOXq", NULL);
	if(i < 0 || !need(argc - i + 1, 2, "addall [--legacy-hardlinks] [-c algo] [-s] [-P] [-O uid:gid] [-X] <dir> [dst]"))
		return 1;
	return apfslib_addall(c->vol, argv[i], i + 1 < argc ? argv[i + 1] : "/", &c->opts) < 0;
}

static int cmd_grow(ctx_t* c, int argc, char** argv) {
	uint64_t size;
	int ret;
	if(!need(argc, 2, "grow <size>"))
		return 1;
	if(parse_size(argv[1], &size) < 0) {
		fprintf(stderr, "error: invalid size %s\n", argv[1]);
		return 1;
	}
	ret = apfs_image_grow(c->img, size);
	if(ret < 0) {
		fprintf(stderr, "error: cannot grow the container: %s\n", apfs_strerror(ret));
		return 1;
	}
	printf("grew volume: %llu\n", (unsigned long long)(LE64(c->img->nx->sb->nx_block_count) * c->img->nx->bs));
	return 0;
}

static int cmd_untar(ctx_t* c, int argc, char** argv) {
	AbstractFile* tar;
	int ret, i = parse_opts(c, argc, argv, "csq", NULL);
	if(i < 0 || !need(argc - i + 1, 2, "untar [-c algo] [-s] <tar> [dst]"))
		return 1;
	tar = createAbstractFileFromFile(fopen(argv[i], "rb"));
	if(!tar) {
		fprintf(stderr, "untar: cannot open archive\n");
		return 1;
	}
	ret = apfslib_untar(c->vol, tar, i + 1 < argc ? argv[i + 1] : "/", &c->opts);
	tar->close(tar);
	return ret < 0;
}

static int cmd_getattr(ctx_t* c, int argc, char** argv) {
	if(!need(argc, 3, "getattr <path> <name>"))
		return 1;
	return apfslib_getattr(c->vol, argv[1], argv[2], stdout) < 0;
}

static int cmd_debug(ctx_t* c, int argc, char** argv) {
	int records = argc > 1 && !strcmp(argv[1], "verbose");
	apfs_vol_t* v = c->vol;
	apfs_dump_container(c->img->nx, stdout);
	apfs_dump_tree(&c->img->nx->omap.tree, "container omap", records, stdout);
	apfs_dump_free_queues(c->img->nx, records, stdout);
	apfs_dump_volume(v, stdout);
	if(!v->snap_xid)
		apfs_dump_tree(&v->omap.tree, "volume omap", records, stdout);
	apfs_dump_tree(&v->fs, "fs tree", records, stdout);
	if(v->extref.loaded)
		apfs_dump_tree(&v->extref, "extentref tree", records, stdout);
	if(v->snapmeta.loaded)
		apfs_dump_tree(&v->snapmeta, "snapmeta tree", records, stdout);
	if(v->fext.loaded)
		apfs_dump_tree(&v->fext, "fext tree", records, stdout);
	return 0;
}

/* Additional commands */

static int cmd_info(ctx_t* c, int argc, char** argv) {
	int i;
	(void)argc;
	(void)argv;
	apfs_dump_container(c->img->nx, stdout);
	for(i = 0; i < NX_MAX_FILE_SYSTEMS; i++) {
		apfs_vol_t* v;
		if(c->img->nx->sb->nx_fs_oid[i] && apfs_vol_open(c->img->nx, i, &v) == APFS_OK)
			apfs_dump_volume(v, stdout);
	}
	return 0;
}

static int cmd_volumes(ctx_t* c, int argc, char** argv) {
	int i;
	(void)argc;
	(void)argv;
	printf("%-3s %-36s %-10s %-27s %s\n", "#", "UUID", "Role", "Names", "Name");
	for(i = 0; i < NX_MAX_FILE_SYSTEMS; i++) {
		apfs_vol_t* v;
		char u[37];
		uint64_t inc;
		if(!c->img->nx->sb->nx_fs_oid[i] || apfs_vol_open(c->img->nx, i, &v) < 0)
			continue;
		apfs_uuid_format(v->sb->apfs_vol_uuid, u);
		inc = LE64(v->sb->apfs_incompatible_features);
		printf("%-3d %-36s %-10s %-27s %s%s\n", i, u, apfs_role_name(LE16(v->sb->apfs_role)),
			(inc & APFS_INCOMPAT_CASE_INSENSITIVE) ? "case-insensitive" :
			((inc & APFS_INCOMPAT_NORMALIZATION_INSENSITIVE) ? "case-sensitive" : "case/normalization-sensitive"),
			(const char*)v->sb->apfs_volname, (inc & APFS_INCOMPAT_SEALED_VOLUME) ? " [sealed]" : "");
	}
	return 0;
}

static int cmd_stat(ctx_t* c, int argc, char** argv) {
	if(!need(argc, 2, "stat <path>"))
		return 1;
	return apfslib_stat(c->vol, argv[1], stdout) < 0;
}

static int cmd_readlink(ctx_t* c, int argc, char** argv) {
	if(!need(argc, 2, "readlink <path>"))
		return 1;
	return apfslib_readlink(c->vol, argv[1], stdout) < 0;
}

static int cmd_ln(ctx_t* c, int argc, char** argv) {
	if(argc > 1 && !strcmp(argv[1], "-s")) {
		if(!need(argc, 4, "ln -s <target> <path>"))
			return 1;
		return apfslib_symlink(c->vol, argv[3], argv[2], &c->opts) < 0;
	}
	if(!need(argc, 3, "ln <existing> <path>"))
		return 1;
	return apfslib_link(c->vol, argv[1], argv[2]) < 0;
}

static int cmd_mknod(ctx_t* c, int argc, char** argv) {
	uint16_t mode;
	uint32_t rdev = 0;
	int i;
	c->opts.file_mode = 0;
	i = parse_opts(c, argc, argv, "mO", NULL);
	if(i < 0 || !need(argc - i + 1, 3, "mknod [-m mode] [-O uid:gid] <path> <c|b|p|s> [major minor]"))
		return 1;
	switch(argv[i + 1][0]) {
		case 'c': mode = APFS_S_IFCHR; break;
		case 'b': mode = APFS_S_IFBLK; break;
		case 'p': mode = APFS_S_IFIFO; break;
		case 's': mode = APFS_S_IFSOCK; break;
		default:
			fprintf(stderr, "error: type must be c, b, p or s\n");
			return 1;
	}
	if(mode == APFS_S_IFCHR || mode == APFS_S_IFBLK) {
		if(!need(argc - i + 1, 5, "mknod <path> <c|b> <major> <minor>"))
			return 1;
		rdev = ((uint32_t)strtoul(argv[i + 2], NULL, 0) << 24) | ((uint32_t)strtoul(argv[i + 3], NULL, 0) & 0xffffff);
	}
	mode |= c->opts.file_mode ? (c->opts.file_mode & 07777) : 0644;
	return apfslib_mknod(c->vol, argv[i], mode, rdev, &c->opts) < 0;
}

static int cmd_touch(ctx_t* c, int argc, char** argv) {
	int i = parse_opts(c, argc, argv, "mO", NULL);
	if(i < 0 || !need(argc - i + 1, 2, "touch [-m mode] [-O uid:gid] <path>"))
		return 1;
	for(; i < argc; i++)
		if(apfslib_touch(c->vol, argv[i], 0, &c->opts) < 0)
			return 1;
	return 0;
}

static int cmd_chflags(ctx_t* c, int argc, char** argv) {
	int recursive;
	uint32_t set, clear;
	int i = parse_opts(c, argc, argv, "R", &recursive);
	if(i < 0 || !need(argc - i + 1, 3, "chflags [-R] <flags> <path>"))
		return 1;
	if(apfslib_parse_flags(argv[i], &set, &clear) < 0) {
		fprintf(stderr, "error: invalid flags %s\n", argv[i]);
		return 1;
	}
	for(i++; i < argc; i++)
		if(apfslib_chflags(c->vol, argv[i], set, clear, recursive) < 0)
			return 1;
	return 0;
}

static int read_file(const char* path, uint8_t** data, size_t* size) {
	FILE* f = fopen(path, "rb");
	long len;
	if(!f)
		return APFS_EIO;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	*data = (uint8_t*)apfs_malloc((size_t)len + 1);
	*size = fread(*data, 1, (size_t)len, f);
	fclose(f);
	return *size == (size_t)len ? APFS_OK : APFS_EIO;
}

static int cmd_setattr(ctx_t* c, int argc, char** argv) {
	uint8_t* data;
	size_t size;
	int ret;
	if(!need(argc, 4, "setattr <path> <name> <value> | setattr <path> <name> -f <file>"))
		return 1;
	if(!strcmp(argv[3], "-f")) {
		if(!need(argc, 5, "setattr <path> <name> -f <file>"))
			return 1;
		if(read_file(argv[4], &data, &size) < 0) {
			fprintf(stderr, "error: cannot read %s\n", argv[4]);
			return 1;
		}
	} else {
		size = strlen(argv[3]);
		data = (uint8_t*)apfs_strdup(argv[3]);
	}
	ret = apfslib_setattr(c->vol, argv[1], argv[2], data, size);
	free(data);
	return ret < 0;
}

static int cmd_rmattr(ctx_t* c, int argc, char** argv) {
	if(!need(argc, 3, "rmattr <path> <name>"))
		return 1;
	return apfslib_rmattr(c->vol, argv[1], argv[2]) < 0;
}

static int cmd_listattr(ctx_t* c, int argc, char** argv) {
	if(!need(argc, 2, "listattr <path>"))
		return 1;
	return apfslib_listattr(c->vol, argv[1], stdout) < 0;
}

static int cmd_compress(ctx_t* c, int argc, char** argv) {
	int recursive, algo = APFS_COMPRESS_LZFSE;
	int i = parse_opts(c, argc, argv, "R", &recursive);
	if(i < 0 || !need(argc - i + 1, 2, "compress [-R] <path> [zlib|lzvn|lzfse]"))
		return 1;
	if(i + 1 < argc && apfs_compress_parse(argv[i + 1], &algo) < 0) {
		fprintf(stderr, "error: unknown algorithm %s\n", argv[i + 1]);
		return 1;
	}
	return apfslib_compress(c->vol, argv[i], algo, recursive, c->opts.quiet) < 0;
}

static int cmd_decompress(ctx_t* c, int argc, char** argv) {
	int recursive;
	int i = parse_opts(c, argc, argv, "R", &recursive);
	if(i < 0 || !need(argc - i + 1, 2, "decompress [-R] <path>"))
		return 1;
	return apfslib_compress(c->vol, argv[i], APFS_COMPRESS_NONE, recursive, c->opts.quiet) < 0;
}

static int cmd_hash(ctx_t* c, int argc, char** argv) {
	return apfslib_hash(c->vol, argc > 1 ? argv[1] : "/", stdout) < 0;
}

static int cmd_verify(ctx_t* c, int argc, char** argv) {
	int ret = apfs_verify(c->img->nx, argc > 1 && !strcmp(argv[1], "verbose"));
	if(ret == APFS_OK)
		printf("The container appears to be OK.\n");
	else
		printf("The container has errors.\n");
	return ret < 0;
}

static int cmd_snapshots(ctx_t* c, int argc, char** argv) {
	apfs_snapshot_t* s;
	size_t n, i;
	(void)argc;
	(void)argv;
	if(apfs_snapshot_list(c->vol, &s, &n) < 0)
		return 1;
	for(i = 0; i < n; i++)
		printf("%-8llu %s\n", (unsigned long long)s[i].xid, s[i].name);
	if(!n)
		printf("No snapshots.\n");
	apfs_snapshots_free(s, n);
	return 0;
}

static int cmd_snapshot(ctx_t* c, int argc, char** argv) {
	int ret;
	if(!need(argc, 2, "snapshot <list|create|delete|rename> [name] [newname]"))
		return 1;
	if(!strcmp(argv[1], "list"))
		return cmd_snapshots(c, argc - 1, argv + 1);
	if(!strcmp(argv[1], "create")) {
		if(!need(argc, 3, "snapshot create <name>"))
			return 1;
		ret = apfs_snapshot_create(c->vol, argv[2]);
	} else if(!strcmp(argv[1], "delete")) {
		if(!need(argc, 3, "snapshot delete <name>"))
			return 1;
		ret = apfs_snapshot_delete(c->vol, argv[2]);
	} else if(!strcmp(argv[1], "rename")) {
		if(!need(argc, 4, "snapshot rename <name> <newname>"))
			return 1;
		ret = apfs_snapshot_rename(c->vol, argv[2], argv[3]);
	} else {
		fprintf(stderr, "usage: snapshot <list|create|delete|rename> [name] [newname]\n");
		return 1;
	}
	if(ret < 0)
		fprintf(stderr, "error: %s\n", apfs_strerror(ret));
	return ret < 0;
}

static int parse_vol_params(int argc, char** argv, int i, apfs_vol_params_t* vp, uint8_t* uuid) {
	for(; i < argc; i++) {
		if(!strcmp(argv[i], "-n") && i + 1 < argc) {
			vp->name = argv[++i];
		} else if(!strcmp(argv[i], "-r") && i + 1 < argc) {
			if(apfs_role_parse(argv[++i], &vp->role) < 0) {
				fprintf(stderr, "error: unknown role %s\n", argv[i]);
				return -1;
			}
		} else if(!strcmp(argv[i], "-i")) {
			vp->case_sensitive = 0;
		} else if(!strcmp(argv[i], "-e")) {
			vp->case_sensitive = 1;
			vp->norm_insensitive = 0;
		} else if(!strcmp(argv[i], "-U") && i + 1 < argc) {
			if(apfs_uuid_parse(argv[++i], uuid) < 0) {
				fprintf(stderr, "error: invalid UUID %s\n", argv[i]);
				return -1;
			}
			vp->uuid = uuid;
		} else if(!strcmp(argv[i], "-O") && i + 1 < argc) {
			if(parse_owner(argv[++i], &vp->root_uid, &vp->root_gid) < 0)
				return -1;
		} else if(!strcmp(argv[i], "-m") && i + 1 < argc) {
			uint16_t m;
			if(parse_mode(argv[++i], &m) < 0)
				return -1;
			vp->root_mode = m;
		} else if(!strcmp(argv[i], "-q") && i + 1 < argc) {
			uint64_t q;
			if(parse_size(argv[++i], &q) < 0)
				return -1;
			vp->quota_blocks = q / 4096;
		} else if(!strcmp(argv[i], "-R") && i + 1 < argc) {
			uint64_t q;
			if(parse_size(argv[++i], &q) < 0)
				return -1;
			vp->reserve_blocks = q / 4096;
		} else {
			return i;
		}
	}
	return i;
}

static void default_vol_params(apfs_vol_params_t* vp) {
	memset(vp, 0, sizeof(*vp));
	vp->name = "untitled";
	vp->case_sensitive = 1;
	vp->norm_insensitive = 1;
	vp->root_mode = 0755;
}

static int cmd_volume(ctx_t* c, int argc, char** argv) {
	int ret = APFS_EINVAL;
	if(!need(argc, 2, "volume <add|delete|rename|role|export> ..."))
		return 1;
	if(!strcmp(argv[1], "add")) {
		apfs_vol_params_t vp;
		uint8_t uuid[16];
		int idx, end;
		default_vol_params(&vp);
		end = parse_vol_params(argc, argv, 2, &vp, uuid);
		if(end < 0 || end < argc) {
			fprintf(stderr, "usage: volume add [-n name] [-r role] [-i|-e] [-U uuid] [-O uid:gid]\n");
			return 1;
		}
		ret = apfs_vol_create(c->img->nx, &vp, &idx);
		if(ret == APFS_OK)
			printf("created volume %d\n", idx);
	} else if(!strcmp(argv[1], "delete")) {
		int idx;
		if(!need(argc, 3, "volume delete <index|name>"))
			return 1;
		ret = apfs_vol_find(c->img->nx, argv[2], &idx);
		if(ret == APFS_OK) {
			if(c->vol && c->vol->index == idx)
				c->vol = NULL;
			ret = apfs_vol_delete(c->img->nx, idx);
		}
	} else if(!strcmp(argv[1], "rename")) {
		if(!need(argc, 3, "volume rename <name>"))
			return 1;
		ret = apfs_vol_rename(c->vol, argv[2]);
	} else if(!strcmp(argv[1], "role")) {
		uint16_t role;
		if(!need(argc, 3, "volume role <role>"))
			return 1;
		if(apfs_role_parse(argv[2], &role) < 0) {
			fprintf(stderr, "error: unknown role %s\n", argv[2]);
			return 1;
		}
		ret = apfs_vol_set_role(c->vol, role);
	} else if(!strcmp(argv[1], "export")) {
		const char* format = "dmg";
		if(!need(argc, 3, "volume export <output> [--format dmg|raw]"))
			return 1;
		if(argc == 5 && !strcmp(argv[3], "--format"))
			format = argv[4];
		else if(argc != 3) {
			fprintf(stderr, "usage: volume export <output> [--format dmg|raw]\n");
			return 1;
		}
		if(is_image(c, argv[2]))
			return 1;
		ret = apfs_export_volume(c->vol, argv[2], format);
	} else {
		fprintf(stderr, "usage: volume <add|delete|rename|role|export> ...\n");
		return 1;
	}
	if(ret < 0)
		fprintf(stderr, "error: %s\n", apfs_strerror(ret));
	return ret < 0;
}

static int cmd_unseal(ctx_t* c, int argc, char** argv) {
	int ret;
	(void)argc;
	(void)argv;
	ret = apfs_vol_unseal(c->vol);
	if(ret < 0)
		fprintf(stderr, "error: cannot unseal: %s\n", apfs_strerror(ret));
	return ret < 0;
}

static int cmd_export(ctx_t* c, int argc, char** argv) {
	int ret;
	if(!need(argc, 2, "dmg <out.dmg> [-c lzfse|zlib|raw] [-b] [-n] | raw <out> [-c] | im4p <out> [-t type] [-d desc] [-z]") || is_image(c, argv[1]))
		return 1;
	ret = apfs_image_export(c->img, argv[0], argv[1], argc - 2, argv + 2);
	if(ret < 0)
		fprintf(stderr, "error: cannot write %s: %s\n", argv[1], apfs_strerror(ret));
	return ret < 0;
}

static int cmd_trustcache(ctx_t* c, int argc, char** argv) {
	const char* out = NULL;
	const char* type = NULL;
	const char* merge = NULL;
	const char** paths = (const char**)apfs_malloc(argc * sizeof(char*));
	apfs_tc_t tc;
	int i, np = 0, version = 0, merged_version = 1, ret = APFS_OK;

	for(i = 1; i < argc; i++) {
		if(!strcmp(argv[i], "-v") && i + 1 < argc)
			version = atoi(argv[++i]);
		else if(!strcmp(argv[i], "-t") && i + 1 < argc)
			type = argv[++i];
		else if(!strcmp(argv[i], "-m") && i + 1 < argc)
			merge = argv[++i];
		else if(argv[i][0] == '-')
			ret = APFS_EINVAL;
		else if(!out)
			out = argv[i];
		else
			paths[np++] = argv[i];
	}
	if(ret < 0 || !out || (version && version != 1 && version != 2) || (type && strlen(type) != 4)) {
		fprintf(stderr, "usage: trustcache <out> [path...] [-v 1|2] [-t type] [-m trustcache]\n");
		free(paths);
		return 1;
	}
	if(is_image(c, out)) {
		free(paths);
		return 1;
	}
	memset(&tc, 0, sizeof(tc));
	if(merge && (ret = apfs_tc_load(merge, &tc, &merged_version)) < 0)
		fprintf(stderr, "error: cannot read %s: %s\n", merge, apfs_strerror(ret));
	for(i = 0; ret == APFS_OK && i < (np ? np : 1); i++)
		ret = apfs_tc_scan(c->vol, np ? paths[i] : "/", &tc, c->opts.quiet ? NULL : stdout);
	if(ret == APFS_OK && (ret = apfs_tc_save(&tc, version ? version : merged_version, type, out)) < 0)
		fprintf(stderr, "error: cannot write %s: %s\n", out, apfs_strerror(ret));
	apfs_tc_free(&tc);
	free(paths);
	return ret < 0;
}

static int cmd_mtree(ctx_t* c, int argc, char** argv) {
	const char* out = NULL;
	const char* type = NULL;
	uint64_t time = 0;
	int i, have_time = 0, ret = APFS_OK;
	char* spec;
	size_t len;

	for(i = 1; i < argc; i++) {
		if(!strcmp(argv[i], "-T") && i + 1 < argc) {
			char* end;
			time = strtoull(argv[++i], &end, 10);
			have_time = 1;
			if(end == argv[i] || *end)
				ret = APFS_EINVAL;
		} else if(!strcmp(argv[i], "-t") && i + 1 < argc) {
			type = argv[++i];
		} else if(argv[i][0] == '-' || out) {
			ret = APFS_EINVAL;
		} else {
			out = argv[i];
		}
	}
	if(ret < 0 || !out || (type && strlen(type) != 4)) {
		fprintf(stderr, "usage: mtree <out> [-T seconds] [-t type]\n");
		return 1;
	}
	if(is_image(c, out))
		return 1;
	if(!have_time) {
		/* Apple's files carry the time of the build; default to the root directory's */
		apfs_inode_t root;
		if(apfs_inode_read(c->vol, ROOT_DIR_INO_NUM, &root) == APFS_OK) {
			time = root.mod_time / 1000000000ULL;
			apfs_inode_release(&root);
		}
	}
	ret = apfs_mtree_spec(c->vol, time, &spec, &len);
	if(ret < 0) {
		fprintf(stderr, "error: cannot read the volume: %s\n", apfs_strerror(ret));
		return 1;
	}
	ret = apfs_mtree_save(spec, len, time, type, out);
	if(ret < 0)
		fprintf(stderr, "error: cannot write %s: %s\n", out, apfs_strerror(ret));
	free(spec);
	return ret < 0;
}

static int cmd_root_hash(ctx_t* c, int argc, char** argv) {
	const char* out = NULL;
	const char* type = "isys";
	uint8_t metadata[APFS_ROOT_HASH_METADATA_SIZE];
	int i, ret = APFS_OK;

	for(i = 1; i < argc; i++) {
		if(!strcmp(argv[i], "-t") && i + 1 < argc) {
			type = argv[++i];
		} else if(argv[i][0] == '-' || out) {
			ret = APFS_EINVAL;
		} else {
			out = argv[i];
		}
	}
	if(ret < 0 || !out || strlen(type) != 4) {
		fprintf(stderr, "usage: root-hash <out> [-t type]\n");
		return 1;
	}
	if(is_image(c, out))
		return 1;
	ret = apfs_vol_root_hash_metadata(c->vol, metadata);
	if(ret < 0) {
		fprintf(stderr, "error: cannot build the root-hash metadata: %s\n", apfs_strerror(ret));
		return 1;
	}
	ret = apfs_im4p_save(type, "0", metadata, sizeof(metadata), out);
	if(ret < 0)
		fprintf(stderr, "error: cannot write %s: %s\n", out, apfs_strerror(ret));
	return ret < 0;
}

/* Image creation */

static int do_create(const char* path, int argc, char** argv) {
	apfs_mkfs_params_t mp;
	static apfs_vol_params_t vols[NX_MAX_FILE_SYSTEMS];
	static uint8_t uuids[NX_MAX_FILE_SYSTEMS][16];
	uint8_t nxuuid[16];
	uint64_t size;
	io_func* io;
	int i, nv = 0, ret;
	const char* format = NULL;
	const char* ext;
	char* raw_path;
	plat_stat_t existing;
	if(plat_lstat(path, &existing) == APFS_OK) {
		fprintf(stderr, "error: create output already exists\n");
		return 1;
	}

	if(argc < 2 || parse_size(argv[1], &size) < 0) {
		fprintf(stderr, "usage: apfs-tool <image> create <size> [-b blocksize] [-u uuid] [-f raw|dmg|im4p]\n"
			"         [-n name] [-r role] [-i|-e] [-U uuid] [-O uid:gid] [-m mode] [-q quota] [-R reserve]\n"
			"         [--volume [-n name] [-r role] ...]...\n");
		return 1;
	}
	memset(&mp, 0, sizeof(mp));
	mp.size = size;
	default_vol_params(&vols[nv++]);
	i = 2;
	while(i < argc) {
		if(!strcmp(argv[i], "-b") && i + 1 < argc) {
			uint64_t bs;
			if(parse_size(argv[++i], &bs) < 0)
				return 1;
			mp.block_size = (uint32_t)bs;
			i++;
		} else if(!strcmp(argv[i], "-u") && i + 1 < argc) {
			if(apfs_uuid_parse(argv[++i], nxuuid) < 0)
				return 1;
			mp.uuid = nxuuid;
			i++;
		} else if(!strcmp(argv[i], "-f") && i + 1 < argc) {
			format = argv[++i];
			i++;
		} else if(!strcmp(argv[i], "--volume")) {
			if(nv >= NX_MAX_FILE_SYSTEMS)
				return 1;
			default_vol_params(&vols[nv++]);
			i++;
		} else if(!strcmp(argv[i], "--no-volume")) {
			nv = 0;
			i++;
		} else {
			int next;
			if(nv == 0)
				default_vol_params(&vols[nv++]);
			next = parse_vol_params(argc, argv, i, &vols[nv - 1], uuids[nv - 1]);
			if(next < 0)
				return 1;
			if(next == i) {
				fprintf(stderr, "error: unknown option %s\n", argv[i]);
				return 1;
			}
			i = next;
		}
	}
	mp.nvols = nv;
	mp.vols = vols;

	ext = strrchr(path, '.');
	if(!format)
		format = (ext && !strcmp(ext, ".dmg")) ? "dmg" : ((ext && !strcmp(ext, ".im4p")) ? "im4p" : "raw");
	if(strcmp(format, "raw") && strcmp(format, "dmg") && strcmp(format, "im4p")) {
		fprintf(stderr, "error: unknown format %s\n", format);
		return 1;
	}

	raw_path = strcmp(format, "raw") ? plat_temp_path("create.raw") : apfs_strdup(path);
	io = apfs_rawfile_create(raw_path, size);
	if(!io) {
		fprintf(stderr, "error: cannot create %s\n", raw_path);
		free(raw_path);
		return 1;
	}
	ret = apfs_mkfs(io, 0, &mp);
	CLOSE(io);
	if(ret < 0)
		plat_remove(raw_path);
	if(ret == APFS_OK && strcmp(format, "raw")) {
		apfs_image_t* img;
		ret = apfs_image_open(raw_path, 0, &img);
		if(ret == APFS_OK) {
			ret = apfs_image_export(img, format, path, 0, NULL);
			apfs_image_close(img, 0);
		}
		plat_remove(raw_path);
	}
	free(raw_path);
	if(ret < 0) {
		fprintf(stderr, "error: cannot create the container: %s\n", apfs_strerror(ret));
		return 1;
	}
	if(apfs_verbosity > 0)
		printf("created %s: %llu bytes, %d volume%s\n", path, (unsigned long long)size, nv, nv == 1 ? "" : "s");
	return 0;
}

typedef struct {
	const char* name;
	int (*fn)(ctx_t*, int, char**);
	int writes;                   /* 2: depends on the subcommand */
	int needs_volume;
} command_t;

/* Bounded host-side block decoding for OTA PBZE metadata. No image is opened. */
static int decode_lzfse_block(const char *source, const char *destination, const char *size_string) {
	char *end = NULL;
	unsigned long long requested = strtoull(size_string, &end, 10);
	FILE *in = NULL, *out = NULL;
	uint8_t *encoded = NULL, *decoded = NULL;
	long length;
	int result = 1;
	if (!*size_string || !end || *end || !requested || requested > 256ULL*1024*1024) {
		fprintf(stderr, "error: invalid LZFSE decoded block size\n");
		return 1;
	}
	in = fopen(source, "rb");
	if (!in || fseek(in, 0, SEEK_END) || (length = ftell(in)) < 4 || length > 256L*1024*1024 || fseek(in, 0, SEEK_SET)) goto done;
	encoded = malloc((size_t)length); decoded = malloc((size_t)requested + 1);
	if (!encoded || !decoded || fread(encoded, 1, (size_t)length, in) != (size_t)length ||
		memcmp(encoded + length - 4, "bvx$", 4) ||
		lzfse_decode_buffer(decoded, (size_t)requested + 1, encoded, (size_t)length, NULL) != requested) goto done;
	out = fopen(destination, "wbx");
	if (!out) goto done;
	if (fwrite(decoded, 1, (size_t)requested, out) != requested) { fclose(out); out = NULL; remove(destination); goto done; }
	result = fclose(out) != 0; out = NULL;
	if (result) remove(destination);
done:
	if (in) fclose(in);
	if (out) fclose(out);
	free(encoded); free(decoded);
	if (result) fprintf(stderr, "error: invalid LZFSE block or output failure\n");
	return result;
}

static const command_t commands[] = {
	{ "ls", cmd_ls, 0, 1 },
	{ "cat", cmd_cat, 0, 1 },
	{ "extract", cmd_extract, 0, 1 },
	{ "extractall", cmd_extractall, 0, 1 },
	{ "mv", cmd_mv, 1, 1 },
	{ "symlink", cmd_symlink, 1, 1 },
	{ "mkdir", cmd_mkdir, 1, 1 },
	{ "add", cmd_add, 1, 1 },
	{ "rm", cmd_rm, 1, 1 },
	{ "chmod", cmd_chmod, 1, 1 },
	{ "chown", cmd_chown, 1, 1 },
	{ "rmall", cmd_rmall, 1, 1 },
	{ "addall", cmd_addall, 1, 1 },
	{ "grow", cmd_grow, 1, 0 },
	{ "untar", cmd_untar, 1, 1 },
	{ "getattr", cmd_getattr, 0, 1 },
	{ "debug", cmd_debug, 0, 1 },
	{ "info", cmd_info, 0, 0 },
	{ "volumes", cmd_volumes, 0, 0 },
	{ "stat", cmd_stat, 0, 1 },
	{ "readlink", cmd_readlink, 0, 1 },
	{ "ln", cmd_ln, 1, 1 },
	{ "mknod", cmd_mknod, 1, 1 },
	{ "touch", cmd_touch, 1, 1 },
	{ "chflags", cmd_chflags, 1, 1 },
	{ "setattr", cmd_setattr, 1, 1 },
	{ "rmattr", cmd_rmattr, 1, 1 },
	{ "listattr", cmd_listattr, 0, 1 },
	{ "compress", cmd_compress, 1, 1 },
	{ "decompress", cmd_decompress, 1, 1 },
	{ "hash", cmd_hash, 0, 1 },
	{ "verify", cmd_verify, 0, 0 },
	{ "snapshots", cmd_snapshots, 0, 1 },
	{ "snapshot", cmd_snapshot, 2, 1 },
	{ "volume", cmd_volume, 2, 0 },
	{ "unseal", cmd_unseal, 1, 1 },
	{ "dmg", cmd_export, 0, 0 },
	{ "raw", cmd_export, 0, 0 },
	{ "im4p", cmd_export, 0, 0 },
	{ "trustcache", cmd_trustcache, 0, 1 },
	{ "mtree", cmd_mtree, 0, 1 },
	{ "root-hash", cmd_root_hash, 0, 1 },
	{ "root_hash", cmd_root_hash, 0, 1 },
	{ NULL, NULL, 0, 0 }
};

static void usage(const char* prog) {
	printf("apfs-tool %s - create and modify APFS images\n\n", APFS_TOOL_VERSION);
	printf("usage: %s <image> [-V volume] [-S snapshot] [-o output] [-t tmpdir] [-k key | -p passphrase] [-q] [-v]\n", prog);
	printf("                  <command> [arguments]\n");
	printf("       %s <image> create <size> [options]\n\n", prog);
	printf("Native metadata interchange (POSIX, one unencrypted/unsealed volume):\n");
	printf("  apfs-tool - manifest-version\n");
	printf("  apfs-tool - decode-lzfse INPUT OUTPUT DECODED_BYTES\n");
	printf("  apfs-tool SOURCE extractall TREE --manifest MANIFEST.json\n");
	printf("  apfs-tool - validateall TREE --manifest MANIFEST.json\n");
	printf("  apfs-tool FRESH addall TREE / --manifest MANIFEST.json\n\n");
	printf("  apfs-tool FRESH addall TREE / --metadata METADATA.json\n\n");
	printf("<image> may be a raw image (bare or GPT), a UDIF disk image (.dmg), an IMG4 payload (.im4p),\n");
	printf("an encrypted disk image (hdiutil -encryption) or an Apple Encrypted Archive (.aea).\n");
	printf("Modified disk images and payloads are written back in their original format (or to -o).\n");
	printf("Encrypted disk images take -p or -k (the AES key, then the HMAC key, in hex); AEA archives take\n");
	printf("-k (hex or base64) and are not written: save modified ones elsewhere with -o.\n\n");
	printf("hfsplus-compatible commands:\n");
	printf("  ls [-R] [-i] [path]                      list a directory, or describe a file\n");
	printf("  cat <path>                               write a file to standard output\n");
	printf("  extract <path> <dest>                    copy a file out of the image\n");
	printf("  extractall [-X] [path] [dest]            copy a directory tree out of the image\n");
	printf("  mv <src> <dst>                           move or rename\n");
	printf("  symlink <path> <target>                  create a symbolic link at <path>\n");
	printf("  mkdir [-p] [-m mode] <path>              create a directory\n");
	printf("  add [-c algo] [-s] [-p] [-m mode] <src> <dst>   copy a file into the image\n");
	printf("  rm [-r] <path>                           remove a file, link or directory\n");
	printf("  chmod [-R] <mode> <path>                 change permissions (octal)\n");
	printf("  chown [-R] <uid>:<gid> <path>            change owner\n");
	printf("  rmall [path]                             remove a directory tree\n");
	printf("  shrink <size>                         rebuild a smaller bare single-volume image, 64 MiB..2 GiB\n");
	printf("  fit TREE --manifest JSON [--size N] [--reserve N] [--maximum N]\n");
	printf("  fit TREE --metadata JSON --like NATIVE [--size N] [--reserve N] [--maximum N]\n");
	printf("  addall [--legacy-hardlinks] [-c algo] [-s] [-P] [-O uid:gid] [-X] <dir> [dst]\n");
	printf("                                           copy a directory tree in\n");
	printf("  grow <size>                              grow the container\n");
	printf("  untar [-c algo] [-s] <tar> [dst]         extract a tar archive into the image\n");
	printf("  getattr <path> <name>                    write an extended attribute to stdout\n");
	printf("  debug [verbose]                          dump the on-disk structures\n");
	printf("\nadditional commands:\n");
	printf("  info | volumes | stat <path> | readlink <path> | hash [path] | verify [verbose]\n");
	printf("  ln <existing> <path> | ln -s <target> <path> | mknod <path> <c|b|p> [major minor]\n");
	printf("  touch <path> | chflags [-R] <flags> <path> (e.g. schg,hidden or noschg)\n");
	printf("  setattr <path> <name> <value>|-f <file> | rmattr <path> <name> | listattr <path>\n");
	printf("  compress [-R] <path> [zlib|lzvn|lzfse] | decompress [-R] <path>\n");
	printf("  snapshots | snapshot create|delete|rename <name> [newname]\n");
	printf("  volume add [-n name] [-r role] [-i|-e] | volume delete <volume> | volume rename <name>\n");
	printf("  volume export <output> [--format dmg|raw]  export the selected volume and snapshots\n");
	printf("  volume role <role> | unseal\n");
	printf("  dmg <out.dmg> [-c lzfse|zlib|raw] [-b] [-n]  export an IPSW-style UDIF image\n");
	printf("                                           (-b: no partition map, -n: skip imagescan data)\n");
	printf("  raw <out> [-c]                           export the raw disk (-c: the container only)\n");
	printf("  im4p <out> [-t type] [-d desc] [-z]      export an IMG4 payload, e.g. -t rdsk (-z: LZFSE)\n");
	printf("  trustcache <out> [path...] [-v 1|2] [-t type] [-m trustcache]\n");
	printf("                                           write a trust cache of the signed code (default\n");
	printf("                                           version 1; -t rtsc or trst: as an IMG4 payload;\n");
	printf("                                           -m: add to an existing cache, keeping its entries)\n");
	printf("  mtree <out> [-T seconds] [-t type]       write the volume's canonical metadata as an mtree\n");
	printf("                                           spec (-T: Apple's build time, default the root\n");
	printf("                                           directory's; -t msys: as an IPSW .mtree payload)\n");
	printf("  root-hash <out> [-t type]                write the sealed-volume .root_hash IM4P\n");
	printf("                                           (default type isys)\n");
	printf("\ncreate options:\n");
	printf("  -b <bytes> block size   -u <uuid> container UUID   -f raw|dmg|im4p output format\n");
	printf("  -n <name> volume name   -r <role> system, data, preboot, recovery, update, vm, xart, hardware, none\n");
	printf("  -i case-insensitive (macOS default)   -e case- and normalization-sensitive\n");
	printf("     (the default is case-sensitive and normalization-insensitive, like iOS)\n");
	printf("  -U <uuid> volume UUID   -O <uid>:<gid> root owner   -m <mode> root mode\n");
	printf("  --volume                starts the options of an additional volume\n");
	printf("\nsizes accept k, m, g and t suffixes; compression algorithms are zlib, lzvn and lzfse.\n");
}

int main(int argc, char* argv[]) {
	ctx_t c;
	const char* image;
	const char* volname = NULL;
	const char* snapname = NULL;
	const char* out_path = NULL;
	const command_t* cmd;
	int i, ret, writable, vi;

	TestByteOrder();
	if(argc >= 3 && (!strcmp(argv[2], "fit") || !strcmp(argv[2], "shrink")))
		return apfs_fit_command(argc,argv);
	if(argc == 6 && !strcmp(argv[1], "-") && !strcmp(argv[2], "decode-lzfse"))
		return decode_lzfse_block(argv[3], argv[4], argv[5]);
	if(argc == 3 && !strcmp(argv[2], "manifest-version")) {
		puts(APFS_NATIVE_SCHEMA);
		return 0;
	}
	if(argc == 6 && !strcmp(argv[1], "-") && !strcmp(argv[2], "validateall") && !strcmp(argv[4], "--manifest"))
		return apfs_manifest_validate(argv[3], argv[5]) < 0;
#ifdef _WIN32
	/* cat and getattr write file contents to standard output */
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	if(argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(argv[0]);
		return 0;
	}
	if(!strcmp(argv[1], "--version")) {
		printf("apfs-tool %s\n", APFS_TOOL_VERSION);
		return 0;
	}
	image = argv[1];
	i = 2;
	while(i < argc && argv[i][0] == '-') {
		if(!strcmp(argv[i], "-V") && i + 1 < argc) {
			volname = argv[++i];
		} else if(!strcmp(argv[i], "-S") && i + 1 < argc) {
			snapname = argv[++i];
		} else if(!strcmp(argv[i], "-o") && i + 1 < argc) {
			out_path = argv[++i];
		} else if(!strcmp(argv[i], "-t") && i + 1 < argc) {
			apfs_image_tmpdir = argv[++i];
		} else if(!strcmp(argv[i], "-k") && i + 1 < argc) {
			apfs_image_key = argv[++i];
		} else if(!strcmp(argv[i], "-p") && i + 1 < argc) {
			apfs_image_passphrase = argv[++i];
		} else if(!strcmp(argv[i], "-q")) {
			apfs_verbosity = 0;
		} else if(!strcmp(argv[i], "-v")) {
			apfs_verbosity++;
		} else {
			fprintf(stderr, "error: unknown option %s\n", argv[i]);
			return 1;
		}
		i++;
	}
	if(i >= argc) {
		usage(argv[0]);
		return 1;
	}
	if(getenv("APFS_DEBUG"))
		apfs_verbosity = 3;

	if(!strcmp(argv[i], "create"))
		return do_create(image, argc - i, argv + i);

	for(cmd = commands; cmd->name; cmd++)
		if(!strcmp(cmd->name, argv[i]))
			break;
	if(!cmd->name) {
		fprintf(stderr, "error: unknown command %s\n", argv[i]);
		return 1;
	}
	writable = cmd->writes == 1 || (cmd->writes == 2 && i + 1 < argc && strcmp(argv[i + 1], "list") != 0 &&
		!(cmd->fn == cmd_volume && strcmp(argv[i + 1], "export") == 0));
	if(writable && snapname) {
		fprintf(stderr, "error: snapshots are read-only\n");
		return 1;
	}

	memset(&c, 0, sizeof(c));
	apfslib_default_opts(&c.opts);
	c.opts.quiet = apfs_verbosity == 0;
	c.image_path = image;
	ret = apfs_image_open_ex(image, writable, out_path, &c.img);
	if(ret < 0)
		return 1;
	ret = apfs_vol_find(c.img->nx, volname, &vi);
	if(ret == APFS_OK)
		ret = snapname ? apfs_vol_open_snapshot(c.img->nx, vi, snapname, &c.vol) : apfs_vol_open(c.img->nx, vi, &c.vol);
	if(ret < 0 && (cmd->needs_volume || volname || snapname)) {
		fprintf(stderr, "error: cannot open volume %s%s%s: %s\n", volname ? volname : "0", snapname ? " snapshot " : "",
			snapname ? snapname : "", apfs_strerror(ret));
		apfs_image_close(c.img, 0);
		return 1;
	}
	if(c.vol && c.vol->sealed && writable && cmd->needs_volume && strcmp(cmd->name, "unseal") != 0) {
		fprintf(stderr, "error: the volume is sealed; run 'unseal' first to make it writable\n");
		apfs_image_close(c.img, 0);
		return 1;
	}

	ret = cmd->fn(&c, argc - i, argv + i);
	if(snapname && c.vol)
		apfs_vol_close(c.vol);
	if(apfs_image_close(c.img, ret == 0 && writable) < 0) {
		fprintf(stderr, "error: cannot save the image\n");
		return 1;
	}
	return ret;
}
