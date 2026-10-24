/*
 * apfslib.h
 * apfs-tool
 *
 * Path-based operations used by the command-line tool.
 */

#ifndef APFSLIB_H
#define APFSLIB_H

#include <stdio.h>
#include "apfs/apfs.h"

#define APFS_OWNER_INHERIT 0xffffffffU

typedef struct {
	uint32_t uid;                 /* owner of new objects, APFS_OWNER_INHERIT to copy the parent's */
	uint32_t gid;
	int preserve_owner;           /* addall/untar: keep the owners of the source */
	int preserve_mode;            /* add: copy permissions and times from the source */
	int compress;                 /* APFS_COMPRESS_* for new file contents */
	int (*compression_for_path)(const char *path, void *context);
	int (*compression_layout_for_path)(const char *path, void *context, uint32_t *type, uint64_t *resource_size);
	void *compression_context;
	uint32_t compression_type;
	uint64_t resource_fork_size;
	int sparse;                   /* turn zero blocks into holes */
	int xattrs;                   /* copy extended attributes */
	int quiet;
	uint16_t file_mode;           /* permissions for new files (add) */
	uint16_t dir_mode;            /* permissions for new directories */
} apfslib_opts_t;

void apfslib_default_opts(apfslib_opts_t* o);

#define APFSLIB_LS_RECURSIVE 0x1
#define APFSLIB_LS_INODES 0x2
#define APFSLIB_LS_HIDDEN 0x4

int apfslib_ls(apfs_vol_t* vol, const char* path, int flags, FILE* out);
int apfslib_stat(apfs_vol_t* vol, const char* path, FILE* out);
int apfslib_cat(apfs_vol_t* vol, const char* path, FILE* out);
int apfslib_extract(apfs_vol_t* vol, const char* path, const char* dest);
int apfslib_extractall(apfs_vol_t* vol, const char* path, const char* destdir, const apfslib_opts_t* o);
int apfslib_add(apfs_vol_t* vol, const char* src, const char* dst, const apfslib_opts_t* o);
int apfslib_add_data(apfs_vol_t* vol, const uint8_t* data, size_t size, const char* dst, const apfslib_opts_t* o);
int apfslib_addall(apfs_vol_t* vol, const char* srcdir, const char* dst, const apfslib_opts_t* o);
int apfslib_mkdir(apfs_vol_t* vol, const char* path, int parents, const apfslib_opts_t* o);
int apfslib_rm(apfs_vol_t* vol, const char* path, int recursive, int quiet);
int apfslib_rmall(apfs_vol_t* vol, const char* path, int quiet);
int apfslib_mv(apfs_vol_t* vol, const char* src, const char* dst);
int apfslib_symlink(apfs_vol_t* vol, const char* path, const char* target, const apfslib_opts_t* o);
int apfslib_link(apfs_vol_t* vol, const char* existing, const char* path);
int apfslib_mknod(apfs_vol_t* vol, const char* path, uint16_t mode, uint32_t rdev, const apfslib_opts_t* o);
int apfslib_chmod(apfs_vol_t* vol, const char* path, uint16_t mode, int recursive);
int apfslib_chown(apfs_vol_t* vol, const char* path, uint32_t uid, uint32_t gid, int recursive);
int apfslib_chflags(apfs_vol_t* vol, const char* path, uint32_t set, uint32_t clear, int recursive);
int apfslib_touch(apfs_vol_t* vol, const char* path, uint64_t time, const apfslib_opts_t* o);
int apfslib_getattr(apfs_vol_t* vol, const char* path, const char* name, FILE* out);
int apfslib_setattr(apfs_vol_t* vol, const char* path, const char* name, const uint8_t* data, size_t size);
int apfslib_rmattr(apfs_vol_t* vol, const char* path, const char* name);
int apfslib_listattr(apfs_vol_t* vol, const char* path, FILE* out);
int apfslib_compress(apfs_vol_t* vol, const char* path, int algo, int recursive, int quiet);
int apfslib_readlink(apfs_vol_t* vol, const char* path, FILE* out);
int apfslib_hash(apfs_vol_t* vol, const char* path, FILE* out);
int apfslib_untar(apfs_vol_t* vol, AbstractFile* tar, const char* dest, const apfslib_opts_t* o);
int apfslib_parse_flags(const char* s, uint32_t* set, uint32_t* clear);

#endif
