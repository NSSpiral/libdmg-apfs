#ifndef APFS_MANIFEST_H
#define APFS_MANIFEST_H
#include "apfs/apfs.h"
#define APFS_NATIVE_SCHEMA "apple-firmware.apfs-native/1"
#define APFS_PORTABLE_SCHEMA "apple-firmware.portable-metadata/1"
int apfs_manifest_inventory_metadata(apfs_vol_t *vol, const char *manifest);
int apfs_manifest_inventory(apfs_vol_t *vol, const char *manifest);
int apfs_manifest_extractfiles(apfs_vol_t *vol, const char *tree, const char *request);
int apfs_manifest_export(apfs_vol_t* vol, const char* tree, const char* manifest);
int apfs_manifest_validate(const char* tree, const char* manifest);
int apfs_manifest_import(apfs_vol_t* vol, const char* tree, const char* manifest);
int apfs_metadata_import(apfs_vol_t* vol, const char* tree, const char* manifest);
#endif
