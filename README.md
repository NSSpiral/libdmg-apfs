# apfs-tool

Create, inspect and modify APFS images from the command line, on macOS, Linux
and Windows. apfs-tool is the APFS counterpart of the `hfsplus` tool from
[libdmg-hfsplus](https://github.com/justtestingafewthings/libdmg-hfsplus): it
accepts the same commands, works directly on raw images, UDIF disk images
(`.dmg`) and IMG4 payloads (`.im4p`), and does what is needed to build or
change the file system images of iOS and macOS restore IPSWs.

Everything is implemented from the on-disk format: no kernel driver, FUSE or
macOS frameworks are involved. The dependencies are zlib and json-c for the
POSIX native-manifest commands.

## Building

You need GNU make, a C99 compiler, zlib and json-c development headers/libraries.
`pkg-config` supplies json-c flags when available. Windows builds retain the
other commands without json-c; native-manifest commands currently require POSIX.

```sh
make            # build/apfs-tool, build/dmg and build/libapfs.a
make test       # unit and end-to-end tests
make install    # into /usr/local/bin (PREFIX=... to change)
```

For Windows, build with MinGW-w64, natively (MSYS2) or as a cross compiler:

```sh
make CC=x86_64-w64-mingw32-gcc
```

LZFSE and LZVN come from Apple's reference implementation, included in
`third_party/lzfse` (BSD license).

## Usage

```
apfs-tool <image> [-V volume] [-S snapshot] [-o output] [-t tmpdir] [-k key|-p passphrase] [-q] [-v] <command> [arguments]
apfs-tool <image> create <size> [options]
```

`<image>` can be a raw disk image (a bare container, or a GPT disk with an
APFS partition), a UDIF disk image, an IMG4 payload, an `encrcdsa` encrypted
disk image, or an Apple Encrypted Archive (`.aea`). Changes to a `.dmg` or
`.im4p` are made on a temporary raw copy (in `-t tmpdir`, or next to the
image) and written back in the original format, with the same compression,
when the command finishes; `-o` writes the result to another file instead.
Encrypted disk images take `-k` or `-p` and stay encrypted when rewritten.
AEA archives take a 32-byte `-k` key, are read-only, and require `-o` for a
modified, decrypted output image.

`-V` selects a volume by index or name (the first volume by default), `-S`
opens a snapshot of it read-only.

### hfsplus commands

| command | |
|---|---|
| `ls [-R] [-i] [path]` | list a directory (`-i` adds inode numbers) |
| `cat <path>` | write a file to standard output |
| `extract <path> <dest>` | copy a file out of the image |
| `extractall [-X] [path] [dest]` | copy a tree out (`-X` skips extended attributes) |
| `mv <src> <dst>` | move or rename |
| `symlink <path> <target>` | create a symbolic link |
| `mkdir [-p] [-m mode] <path>` | create a directory |
| `add [-c algo] [-s] [-p] [-m mode] <src> <dst>` | copy a file in (`-s` sparse, `-p` keep mode and times) |
| `rm [-r] <path>` | remove a file, link or (empty, or with `-r` any) directory |
| `chmod [-R] <mode> <path>` | change permissions |
| `chown [-R] <uid>:<gid> <path>` | change owner |
| `rmall [path]` | remove a directory tree |
| `addall [-c algo] [-s] [-P] [-O uid:gid] [-X] <dir> [dst]` | copy a host directory into an existing directory |
| `grow <size>` | grow the container (and its partition and image) |
| `untar [-c algo] [-s] <tar> [dst]` | extract a tar archive into the image ([details](#tar-archives)) |
| `getattr <path> <name>` | write an extended attribute to standard output |
| `debug [verbose]` | dump the on-disk structures |

`-c` compresses new files with `zlib`, `lzvn` or `lzfse` (stored the way
macOS stores compressed files, readable by the kernel); `addall -P` keeps the
owners of the source files, `-O` sets them.

Native manifest interchange preserves Apple metadata without applying it to
the host staging tree:

```sh
apfs-tool source.dmg extractall tree --manifest manifest.json
apfs-tool - validateall tree --manifest manifest.json
apfs-tool fresh.raw create 3g -n Root -e
apfs-tool fresh.raw addall tree / --manifest manifest.json
apfs-tool fresh.raw verify
```

Keep `manifest.json.files/` beside the JSON file. Export paths must be new.
Import requires a fresh empty volume with the same block size and name
comparison modes. `manifest-version` prints the schema,
`apple-firmware.apfs-native/1`. The manifest retains inode IDs, timestamps,
ownership, modes, flags, FinderInfo, hardlinks, symlinks, xattrs and original
compressed storage. Allocation, checkpoint history and clone sharing are rebuilt.
Special nodes remain in the manifest and need no host device privileges.

Validation checks catalog relationships, host links, stream layouts and content
hashes before import. It independently decodes decmpfs types 1/3/4/7/8/9/10/11/12.
Native interchange accepts one unencrypted, unsealed volume without snapshots
or auxiliary index trees. Unsupported records and formats fail explicitly.

For a host tree without a native manifest, use `addall tree / --metadata
metadata.json`. The `apple-firmware.portable-metadata/1` document supplies
ownership, modes, timestamps, BSD flags and base64 xattrs. Entries with
`UF_COMPRESSED` use its `compression` algorithm. Contents and links come from
the host tree. Manifest commands require POSIX and run without mounting images.

#### Tar archives

`untar` reads ustar, GNU and PAX archives: long names and link targets, PAX
extended and global headers, base-256 numbers, hard links (an entry that
repeats the data is fine), symbolic links, device nodes, FIFOs and GNU
sparse files (the old format and PAX 0.0, 0.1 and 1.0; holes stay holes).
Owners, modes and modification times come from the archive, with
nanoseconds when PAX records have them, as do access and creation
(`LIBARCHIVE.creationtime`) times; without a creation time, it moves back
to the modification time, as on macOS. Directory times are set once their
contents are in place.

Apple metadata is restored from both places tar tools keep it:

- PAX records: extended attributes (`SCHILY.xattr.*`, and `LIBARCHIVE.xattr.*`
  as macOS and libarchive write them), file flags (`SCHILY.fflags`, e.g.
  `uchg,hidden`) and NFSv4 ACLs (`SCHILY.acl.ace`). ACL users and groups get
  the GUIDs macOS derives from their IDs; POSIX.1e ACLs from Linux have no
  APFS equivalent and are skipped with a warning.
- The AppleDouble `._name` entries that macOS `tar` writes before a file:
  Finder info, resource fork, extended attributes and the ACL (with its
  original GUIDs). They are merged into the file instead of being extracted;
  a `._` entry that is not followed by its file, or is not AppleDouble, is
  extracted as it is.

ACLs end up in `com.apple.system.Security`, as macOS stores them. The whole
archive is checked before anything is written, including that every hard
link points to an earlier entry or a file already in the image; absolute
paths are made relative and `..` components are refused.

### Additional commands

| command | |
|---|---|
| `info`, `volumes` | describe the container and its volumes |
| `stat <path>`, `readlink <path>` | file details, link targets |
| `hash [path]` | SHA-256 of every file under a path |
| `verify [verbose]` | check the consistency of the whole container |
| `ln <existing> <path>`, `ln -s <target> <path>` | hard and symbolic links |
| `mknod <path> <c\|b\|p> [major minor]` | device nodes and FIFOs |
| `touch <path>` | create a file or update its times |
| `chflags [-R] <flags> <path>` | BSD flags, e.g. `schg,hidden` or `noschg` |
| `setattr <path> <name> <value>\|-f <file>`, `rmattr`, `listattr` | extended attributes |
| `compress [-R] <path> [algo]`, `decompress [-R] <path>` | compress files in place |
| `snapshots`, `snapshot create\|delete\|rename <name> [new]` | snapshots |
| `volume add [options]`, `volume delete <volume>` | add or remove volumes |
| `volume rename <name>`, `volume role <role>` | change the selected volume |
| `volume export <output> [--format dmg\|raw]` | copy the selected volume and snapshots into a single-volume image |
| `unseal` | turn a sealed volume into a plain, writable one |
| `dmg <out.dmg> [-c lzfse\|zlib\|raw] [-b] [-n]` | export a UDIF image laid out like an IPSW's |
| `raw <out> [-c]` | export the raw disk (`-c`: the container alone) |
| `im4p <out> [-t type] [-d desc] [-z]` | export an IMG4 payload (`-z`: LZFSE) |
| `trustcache <out> [path...] [-v 1\|2] [-t type] [-m cache]` | write a trust cache of the signed code ([below](#trust-caches)) |
| `mtree <out> [-T seconds] [-t type]` | write the canonical metadata of a system volume ([below](#system-volume-metadata)) |
| `root-hash <out> [-t type]` | write a sealed volume's IPSW `.root_hash` payload ([below](#root-hashes)) |

### Creating images

```sh
apfs-tool root.dmg create 2g -n Root -r system
apfs-tool disk.raw create 10g -i -n "Macintosh HD" -r system --volume -n Data -r data
apfs-tool ramdisk.im4p create 120m -n Ramdisk -f im4p
```

The format follows the file extension, or `-f raw|dmg|im4p`. Options:
`-b` block size, `-u` container UUID, and per volume `-n` name, `-r` role
(`system`, `data`, `preboot`, `recovery`, `update`, `vm`, `xart`, `hardware`,
`none`), `-i` case-insensitive (the macOS default), `-e` case- and
normalization-sensitive, `-U` volume UUID, `-O uid:gid` and `-m mode` for the
root directory, `-q`/`-R` quota and reserve. `--volume` starts another
volume's options. Without `-i`/`-e`, volumes are case-sensitive and
normalization-insensitive, like on iOS.

Raw images are sparse files where the host file system supports them, also
when exported with `raw`, copied with `-o` or written by `dmg extract`: a new
100 GB image takes about 150 KB of disk.
`create` refuses existing output paths. Fresh formatting needs no donor image.

### The dmg tool

`dmg` converts between UDIF and raw images, like libdmg-hfsplus' tool of the
same name, for any file system:

```sh
dmg extract in.dmg out.img [partition]   # whole disk, or one partition (number or name)
dmg build in.img out.dmg [-c lzfse|zlib|raw]
dmg info in.dmg
dmg verify in.dmg                         # check the data checksums
```

`dmg iso` and `dmg dmg` work as aliases of `extract` and `build`, as in
libdmg-hfsplus. `extract`, `info` and `verify` accept `encrcdsa` images with
`-p <passphrase>` or `-k <key>` (the AES key followed by the HMAC-SHA1 key,
in hex), and AEA archives with a 32-byte hex or base64 `-k <key>`.

## Restore images

Root file systems and ramdisks in IPSWs are UDIF images (`.dmg`) holding a GPT
disk with one APFS partition, IMG4 payloads (`.im4p`, type `rdsk`), or AEA
archives (`.dmg.aea`) wrapping either form. apfs-tool opens them directly:

```sh
apfs-tool 096-12345-678.dmg ls /usr/bin
apfs-tool 096-12345-678.dmg add -c lzfse mytool /usr/local/bin/mytool
apfs-tool 096-12345-678.dmg grow 1g
```

Each command on a `.dmg` or `.im4p` unpacks and repacks the whole image.
For several changes, work on a raw copy and export once at the end:

```sh
apfs-tool rootfs.dmg raw rootfs.raw
apfs-tool rootfs.raw addall -c lzfse overlay /
apfs-tool rootfs.raw chown -R 0:0 /usr/local
apfs-tool rootfs.raw verify
apfs-tool rootfs.raw dmg rootfs-new.dmg -c lzfse
```

Exported UDIF images match Apple's: a GPT disk, LZFSE (or zlib) chunks, free
APFS blocks stored as ignored runs, CRC32 checksums, and the `cSum`, `nsiz`
and `plst` resources that `asr imagescan` adds, so `asr` can restore them
without scanning first. IMG4 payloads are rewritten with their type and
description, optionally LZFSE-compressed; signatures and manifests are not
handled, so a modified payload has to be signed again for a device that
checks it.

### Trust caches

A trust cache lists the CDHashes (code directory hashes) of the code that
iOS and macOS run as platform code. IPSWs carry one for each image in
`Firmware/`: `<ramdisk>.dmg.trustcache` (IMG4 type `rtsc`, the
RestoreTrustCache) and `<system>.dmg.trustcache` (`trst`, the
StaticTrustCache). Binaries added to these images run as platform code only
once their CDHashes are in the image's trust cache, so make it again after
changing the image:

```sh
apfs-tool 096-12345-678.dmg trustcache new.trustcache -t rtsc -m Firmware/096-12345-678.dmg.trustcache
```

`trustcache` hashes the signed files under the given paths (the whole volume
by default) and lists each CDHash with its path. A Mach-O file counts with
the code directory the kernel uses (the strongest hash, so SHA-256 before
SHA-1), once per architecture; dyld shared caches count the same way, and
standalone signatures such as `launchd.plist.sig` with every code
directory, as in Apple's caches. Unsigned binaries are skipped with a
warning, as are the rare SHA-384 signatures.

- `-v 1|2`: the format version. iOS 16 and later use version 2, which adds
  launch constraint categories; the default is 1, or the version of the
  `-m` cache.
- `-t type`: write an IMG4 payload of that type instead of a raw cache. Like
  other payloads, it has to be signed for a device that checks it.
- `-m cache`: start from an existing cache (raw or IMG4) and keep its
  entries as they are, adding only the hashes it lacks. New entries get no
  flags and no launch constraints; Apple's flags (such as the one that marks
  `amfid`) and constraint categories cannot be derived from the binaries,
  so keep Apple's cache with `-m` rather than building one from scratch.

The UUID of the cache is derived from its contents.

### System volume metadata

IPSWs carry the canonical metadata of each system volume as
`Firmware/<system>.dmg.mtree` (IMG4 type `msys`, the
Ap,SystemVolumeCanonicalMetadata): a BSD mtree specification of the volume.
`mtree` writes it for the selected volume:

```sh
apfs-tool 096-12345-678.dmg mtree new.mtree -t msys -T 1679128083
```

The specification is what macOS `mtree -c -S -k
type,inode,siblingid,nxattr,xattrsdigest` prints for the mounted volume:
each item's type, inode number, hard link sibling id and number of extended
attributes, and for items with attributes the id of the data stream holding
the resource fork (`authapfs.<id>`, 0 without one). Apple's files also give
the root directory and `/.file` one time, that of the build; `-T` sets it in
seconds (by default, the root directory's modification time).

Without `-t` the specification is written as text; `-t msys` packs it as
IPSWs do, in an Apple Archive compressed with LZFSE (what `aa archive -a
lzfse` writes) inside an IMG4 payload. For the tvOS 16.4 recovery image, the
text matches Apple's file except for the comments at the top (user,
machine, tree and date).

### Root hashes

IPSWs accompany a sealed system volume with `Firmware/<system>.dmg.root_hash`.
`root-hash` writes that 229-byte IM4P directly from the selected volume:

```sh
apfs-tool sealed.dmg root-hash sealed.dmg.root_hash
```

The default Image4 type is `isys` (System Volume Root Hash); `-t csys` or
`-t xsys` selects the base-system or x86 form. The payload contains SHA-256
seals for the catalog laid out with 4 KiB, 8 KiB and 16 KiB APFS blocks, as
Apple's restore files do. The volume must already be sealed and unbroken.

### Sealed volumes

Sealed volumes (macOS system volumes, cryptexes) can be read, and `info`
prints their root hash. To change one, `unseal` first: it rebuilds the volume
as a plain one, without the hashes. On macOS the result can be sealed again
with `/System/Library/Filesystems/apfs.fs/Contents/Resources/apfs_prepare_cryptex`.

## Checking images

`apfs-tool <image> verify` checks a whole container: every block reachable
from the checkpoint is used once and agrees with the allocation bitmap,
B-trees are ordered and their counts right, every object map entry is used,
each volume owns the blocks it claims, and its file-system records agree
with each other and with its counters. It is meant for platforms without
`fsck_apfs`; on macOS, Apple's checker remains the reference:

```sh
dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount image.raw | head -1 | awk '{print $1}')
fsck_apfs -n $dev; hdiutil detach $dev
```

`make test` runs a randomized workload against an in-memory model, with
`verify` after every transaction, and then the command-line tests; on macOS
these also run `fsck_apfs` on the images (`APFS_TEST_FSCK=0` turns that off).

## Limitations

- Encrypted volumes: the structure can be inspected, but not file contents.
- Fusion (two-device) containers and encryption rolling are not supported.
- Snapshot deletion releases the blocks only that snapshot used; extended
  snapshot metadata (`snap_meta_ext`) is left in place.

## Library

`build/libapfs.a` and the headers in `includes/apfs` expose everything the
tool uses: containers and transactions (`apfs.h`), file operations at the
level of the commands (`apfslib.h`), image formats (`image.h`, `udif.h`)
and the IPSW companions of system images (`trustcache.h`, `mtree.h`).

## License

GPL-3.0, as libdmg-hfsplus, from which `common/abstractfile.c` comes. The
LZFSE code is under its own BSD license (`third_party/lzfse/LICENSE`). The
format follows Apple's *Apple File System Reference*; the Unicode tables used
for file name hashing are generated by `tools/gen_unicode.py`.
