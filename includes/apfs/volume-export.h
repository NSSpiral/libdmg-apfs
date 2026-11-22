#ifndef APFS_VOLUME_EXPORT_H
#define APFS_VOLUME_EXPORT_H

#include "apfs/apfs.h"

int apfs_export_volume(apfs_vol_t* source, const char* output, const char* format);

#endif
