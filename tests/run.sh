#!/bin/sh
#
# End-to-end tests of the apfs-tool command line.
#
#   sh tests/run.sh <path to apfs-tool> <scratch directory>
#
# Every image is checked with "apfs-tool verify" after it changes. On macOS,
# images are also checked with Apple's fsck_apfs (through hdiutil, which
# needs no privileges); set APFS_TEST_FSCK=0 to skip that. Set
# APFS_TEST_NATIVE=0 to skip all tests that attach images with macOS tools.

TOOL=$1
WORK=$2
if [ -z "$TOOL" ] || [ -z "$WORK" ]; then
	echo "usage: $0 <apfs-tool> <scratch directory>" >&2
	exit 2
fi
case $TOOL in /*) ;; *) TOOL=$(pwd)/$TOOL ;; esac
DMGTOOL=$(dirname "$TOOL")/dmg
WORK=$WORK/e2e
rm -rf "$WORK"
mkdir -p "$WORK" || exit 2
cd "$WORK" || exit 2
WORK=$(pwd)

PASS=0
FAIL=0
FSCK=0
if [ "${APFS_TEST_FSCK:-1}" != 0 ] && command -v hdiutil >/dev/null 2>&1 && command -v fsck_apfs >/dev/null 2>&1; then
	FSCK=1
fi

ok() {
	PASS=$((PASS + 1))
	echo "ok - $1"
}

not_ok() {
	FAIL=$((FAIL + 1))
	echo "not ok - $1"
}

# t <description> <command...>: the command must succeed
t() {
	desc=$1
	shift
	if "$@" >cmd.out 2>&1; then
		ok "$desc"
	else
		not_ok "$desc"
		sed 's/^/    /' cmd.out | head -20
	fi
}

# f <description> <command...>: the command must fail
f() {
	desc=$1
	shift
	if "$@" >cmd.out 2>&1; then
		not_ok "$desc (succeeded)"
	else
		ok "$desc"
	fi
}

A() {
	"$TOOL" "$@"
}

# same <image> <path in image> <host file>
same() {
	"$TOOL" "$1" cat "$2" >cat.out 2>cmd.err && cmp -s cat.out "$3"
}

# has <text> <image> <command...>: the output contains the text
has() {
	text=$1
	shift
	"$TOOL" "$@" >has.out 2>&1 && grep -F -- "$text" has.out >/dev/null
}

verify() {
	"$TOOL" "$1" verify >verify.out 2>&1 || {
		cat verify.out >&2
		return 1
	}
	if [ $FSCK = 1 ]; then
		case $1 in
		*.raw) apple_fsck "$1" || return 1 ;;
		esac
	fi
	return 0
}

apple_fsck() {
	dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$1" 2>/dev/null | head -1 | awk '{print $1}')
	[ -n "$dev" ] || return 1
	fsck_apfs -n "$dev" >fsck.out 2>&1
	rc=$?
	hdiutil detach "$dev" >/dev/null 2>&1 || hdiutil detach -force "$dev" >/dev/null 2>&1
	[ $rc = 0 ] || cat fsck.out >&2
	return $rc
}

# Source files

mkdir -p src/sub/deeper src/empty
printf 'hello, world\n' > src/hello.txt
: > src/zero.bin
head -c 5000 /dev/urandom > src/sub/r5k.bin
head -c 300000 /dev/urandom > src/sub/r300k.bin
yes "compressible line of text, repeated to fill the file" | head -c 1200000 > src/sub/deeper/text.txt
head -c 1048576 /dev/zero > src/zeros.bin
printf 'caf\303\251\n' > "src/caf$(printf '\303\251').txt"
ln -s hello.txt src/link
ln src/hello.txt src/sub/hardlink 2>/dev/null

echo "# image creation"
t "create a 64 MiB image" A img.raw create 64m -n Test
t "the new image verifies" verify img.raw
t "info names the volume" has "\"Test\"" img.raw info
t "create a 16 MiB image" A small.raw create 16m
t "it takes under 1 MiB of host disk" test "$(du -k small.raw | cut -f1)" -lt 1024
rm -f small.raw
t "create a case-insensitive image" A ci.raw create 32m -i -n Mac
t "create a container with three volumes" A multi.raw create 2g -n System -r system --volume -n Data -r data --volume -n Preboot -r preboot
t "the multi-volume container verifies" verify multi.raw
t "volumes lists them" has Preboot multi.raw volumes
t "create a UDIF image" A new.dmg create 48m -n InDMG
t "the UDIF image verifies" verify new.dmg
t "create an IMG4 payload" A new.im4p create 16m -n InIM4P -f im4p
t "the IMG4 payload verifies" verify new.im4p
t "create a 100 GiB image" A big.raw create 100g -n Big
t "it takes under 4 MiB of host disk" test "$(du -k big.raw | cut -f1)" -lt 4096
t "the 100 GiB image verifies" verify big.raw
t "export it as a UDIF image" A big.raw dmg big.dmg
t "dmg extract of that image" "$DMGTOOL" extract big.dmg big.img
t "the extracted disk takes under 4 MiB of host disk" test "$(du -k big.img | cut -f1)" -lt 4096
t "dmg extract of its partition" "$DMGTOOL" extract big.dmg big.img Apple_APFS
t "the extracted partition takes under 4 MiB of host disk" test "$(du -k big.img | cut -f1)" -lt 4096
rm -f big.raw big.dmg big.img
# raw exports and -o copies read every byte, so they get a smaller image
t "create a 1 GiB image" A gig.raw create 1g -n Gig
t "export it raw" A gig.raw raw gig-copy.raw
t "the raw export takes under 1 MiB of host disk" test "$(du -k gig-copy.raw | cut -f1)" -lt 1024
t "and matches the image" cmp -s gig.raw gig-copy.raw
t "write a change to another file with -o" A gig.raw -o gig-new.raw mkdir /new
t "the new file takes under 1 MiB of host disk" test "$(du -k gig-new.raw | cut -f1)" -lt 1024
t "it has the change" A gig-new.raw stat /new
f "and the original does not" A gig.raw stat /new
t "the new file verifies" verify gig-new.raw
t "-o naming the image another way edits it in place" A gig.raw -o ./gig.raw mkdir /same
t "the image has the change" A gig.raw stat /same
t "-o naming the image by its full path edits it in place" A gig.raw -o "$WORK/gig.raw" mkdir /absolute
t "the image has that change too" A gig.raw stat /absolute
ln -s gig.raw gig-link.raw
t "so does -o naming a link to it" A gig.raw -o gig-link.raw mkdir /linked
t "the image has that change too" A gig.raw stat /linked
f "raw export onto the image is refused" A gig.raw raw ./gig.raw
f "so is a UDIF export through the link" A gig.raw dmg gig-link.raw
t "the image is intact" verify gig.raw
rm -f gig.raw gig-copy.raw gig-new.raw gig-link.raw
f "a size that is too small fails" A tiny.raw create 100k
f "and leaves no file behind" test -e tiny.raw

echo "# hfsplus commands"
t "mkdir" A img.raw mkdir /a
t "mkdir -p" A img.raw mkdir -p /a/b/c
t "add" A img.raw add src/hello.txt /a/hello.txt
t "cat returns the same data" same img.raw /a/hello.txt src/hello.txt
t "add a 300 KiB file" A img.raw add src/sub/r300k.bin /r300k.bin
t "its contents match" same img.raw /r300k.bin src/sub/r300k.bin
t "add with LZFSE compression" A img.raw add -c lzfse src/sub/deeper/text.txt /lzfse.txt
t "LZFSE contents match" same img.raw /lzfse.txt src/sub/deeper/text.txt
t "add with zlib compression" A img.raw add -c zlib src/sub/deeper/text.txt /zlib.txt
t "zlib contents match" same img.raw /zlib.txt src/sub/deeper/text.txt
t "add with LZVN compression" A img.raw add -c lzvn src/sub/deeper/text.txt /lzvn.txt
t "LZVN contents match" same img.raw /lzvn.txt src/sub/deeper/text.txt
t "add a sparse file" A img.raw add -s src/zeros.bin /zeros.bin
t "sparse contents match" same img.raw /zeros.bin src/zeros.bin
t "add an empty file" A img.raw add src/zero.bin /empty
t "empty contents match" same img.raw /empty src/zero.bin
t "add replaces an existing file" A img.raw add src/sub/r5k.bin /r300k.bin
t "the replacement is read back" same img.raw /r300k.bin src/sub/r5k.bin
t "ls lists the root" has lzfse.txt img.raw ls /
t "ls -R descends" has hello.txt img.raw ls -R /
t "the image verifies" verify img.raw
t "extract" A img.raw extract /lzvn.txt out.txt
t "extracted data matches" cmp -s out.txt src/sub/deeper/text.txt
t "mv into a directory" A img.raw mv /a/hello.txt /a/b/moved.txt
t "the moved file reads" same img.raw /a/b/moved.txt src/hello.txt
f "the old name is gone" A img.raw cat /a/hello.txt
t "mv renames directories" A img.raw mv /a/b /a/renamed
t "files follow their directory" same img.raw /a/renamed/moved.txt src/hello.txt
t "symlink" A img.raw symlink /a/link renamed/moved.txt
t "readlink returns the target" has renamed/moved.txt img.raw readlink /a/link
t "ls shows the link target" has "link -> renamed/moved.txt" img.raw ls /a
t "chmod" A img.raw chmod 600 /lzfse.txt
t "stat shows the new mode" has "Mode: 100600" img.raw stat /lzfse.txt
t "chmod -R" A img.raw chmod -R 700 /a
t "chmod -R reaches the files" has "Mode: 100700" img.raw stat /a/renamed/moved.txt
t "chown" A img.raw chown 501:20 /zlib.txt
t "stat shows the new owner" has "Uid: 501  Gid: 20" img.raw stat /zlib.txt
t "chown -R" A img.raw chown -R 99:99 /a
t "chown -R reaches the files" has "Uid: 99  Gid: 99" img.raw stat /a/renamed/moved.txt
t "setattr (for getattr)" A img.raw setattr /zlib.txt user.comment "a comment"
t "getattr prints the value" has "a comment" img.raw getattr /zlib.txt user.comment
t "the image verifies" verify img.raw
t "rm" A img.raw rm /empty
f "rm of a missing file fails" A img.raw rm /empty
f "rm of a non-empty directory fails" A img.raw rm /a
t "rm -r" A img.raw rm -r /a/renamed
t "rmall" A img.raw rmall /a
f "the directory is gone" A img.raw ls /a
t "mkdir for addall" A img.raw mkdir /tree
t "addall" A img.raw addall src /tree
t "addall copies contents" same img.raw /tree/sub/r300k.bin src/sub/r300k.bin
t "addall keeps unicode names" same img.raw "/tree/caf$(printf '\303\251').txt" "src/caf$(printf '\303\251').txt"
t "addall copies links" has hello.txt img.raw readlink /tree/link
t "the image verifies" verify img.raw
t "extractall" A img.raw extractall /tree outtree
t "extracted trees match" cmp -s outtree/sub/deeper/text.txt src/sub/deeper/text.txt
t "extracted links match" test "$(readlink outtree/link)" = hello.txt
if command -v tar >/dev/null 2>&1; then
	t "make a tar archive" tar -cf src.tar src
	t "untar" A img.raw untar src.tar /untarred
	t "untarred contents match" same img.raw /untarred/src/sub/r5k.bin src/sub/r5k.bin
	t "the image verifies" verify img.raw
fi
t "grow" A img.raw grow 160m
t "info shows the new size" has "167772160 bytes" img.raw info
t "data survives growing" same img.raw /tree/sub/deeper/text.txt src/sub/deeper/text.txt
t "the grown image verifies" verify img.raw
t "debug" A img.raw debug
t "debug verbose" A img.raw debug verbose

echo "# more commands"
t "stat" has "Size: 13" img.raw stat /tree/hello.txt
t "ln makes a hard link" A img.raw ln /tree/sub/r5k.bin /hard
t "the hard link reads" same img.raw /hard src/sub/r5k.bin
t "stat counts both links" has "Links: 2" img.raw stat /hard
t "ln -s" A img.raw ln -s tree/hello.txt /soft
t "the symbolic link resolves" has tree/hello.txt img.raw readlink /soft
t "mknod makes a FIFO" A img.raw mknod /fifo p
t "touch" A img.raw touch /touched
t "chflags" A img.raw chflags hidden /touched
t "stat shows flags" has hidden img.raw stat /touched
t "listattr" has user.comment img.raw listattr /zlib.txt
t "rmattr" A img.raw rmattr /zlib.txt user.comment
f "the attribute is gone" A img.raw getattr /zlib.txt user.comment
t "a large attribute is stored as a stream" A img.raw setattr /zlib.txt user.big -f src/sub/r5k.bin
t "getattr returns it" sh -c "'$TOOL' img.raw getattr /zlib.txt user.big > attr.out && cmp -s attr.out src/sub/r5k.bin"
t "compress -R" A img.raw compress -R /tree lzfse
t "compressed files read the same" same img.raw /tree/sub/deeper/text.txt src/sub/deeper/text.txt
t "stat reports compression" has lzfse img.raw stat /tree/sub/deeper/text.txt
t "decompress" A img.raw decompress /tree/sub/deeper/text.txt
t "decompressed file reads the same" same img.raw /tree/sub/deeper/text.txt src/sub/deeper/text.txt
t "hash lists checksums" has hello.txt img.raw hash /tree
t "the image verifies" verify img.raw

echo "# snapshots"
t "snapshot create" A img.raw snapshot create first
t "snapshots lists it" has first img.raw snapshots
t "change the volume" A img.raw add src/sub/r300k.bin /tree/hello.txt
t "remove a file" A img.raw rm /lzfse.txt
t "the image with a snapshot verifies" verify img.raw
t "the snapshot keeps the old contents" sh -c "'$TOOL' img.raw -S first cat /tree/hello.txt | cmp -s - src/hello.txt"
t "the snapshot keeps removed files" sh -c "'$TOOL' img.raw -S first cat /lzfse.txt | cmp -s - src/sub/deeper/text.txt"
f "snapshots are read-only" A img.raw -S first rm /tree/hello.txt
t "snapshot rename" A img.raw snapshot rename first renamed
t "the renamed snapshot is listed" has renamed img.raw snapshots
t "a second snapshot" A img.raw snapshot create second
t "change the volume again" A img.raw rm -r /tree
t "the image with two snapshots verifies" verify img.raw
t "snapshot delete (oldest)" A img.raw snapshot delete renamed
t "the remaining snapshot still reads" sh -c "'$TOOL' img.raw -S second cat /tree/sub/r300k.bin | cmp -s - src/sub/r300k.bin"
t "the image verifies" verify img.raw
t "snapshot delete (last)" A img.raw snapshot delete second
t "snapshots are gone" has "No snapshots" img.raw snapshots
t "the image verifies" verify img.raw

echo "# volumes"
t "volume add" A multi.raw volume add -n Extra -r data
t "the new volume is usable" A multi.raw -V Extra add src/sub/r300k.bin /x
t "it reads back" sh -c "'$TOOL' multi.raw -V Extra cat /x | cmp -s - src/sub/r300k.bin"
t "volume rename" A multi.raw -V Extra volume rename Renamed
t "volume role" A multi.raw -V Renamed volume role user
t "volumes shows the change" has Renamed multi.raw volumes
t "the container verifies" verify multi.raw
t "volume delete" A multi.raw volume delete Renamed
f "the volume is gone" A multi.raw -V Renamed ls /
t "the container verifies after deleting" verify multi.raw
t "case-insensitive lookups" sh -c "'$TOOL' ci.raw add src/hello.txt /MixedCase.txt && '$TOOL' ci.raw cat /mixedcase.TXT | cmp -s - src/hello.txt"
f "names that differ in case collide" A ci.raw mkdir /MIXEDCASE.txt
t "the case-insensitive image verifies" verify ci.raw

SEALER=/System/Library/Filesystems/apfs.fs/Contents/Resources/apfs_prepare_cryptex
if [ "${APFS_TEST_NATIVE:-1}" != 0 ] && [ -x $SEALER ] && command -v hdiutil >/dev/null 2>&1; then
	echo "# sealed volumes (sealed with apfs_prepare_cryptex)"
	seal() {
		dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$1" 2>/dev/null | head -1 | awk '{print $1}')
		[ -n "$dev" ] || return 1
		vol=$(diskutil list "$dev" 2>/dev/null | awk '/APFS Volume/ {print $NF; exit}')
		[ -n "$vol" ] || vol=$(diskutil list 2>/dev/null | awk -v n="$2" '$0 ~ "APFS Volume " n {print $NF; exit}')
		$SEALER -M sealed.metadata "/dev/$vol" >seal.out 2>&1
		rc=$?
		hdiutil detach "$dev" >/dev/null 2>&1 || hdiutil detach -force "$dev" >/dev/null 2>&1
		return $rc
	}
	t "make an image to seal" sh -c "'$TOOL' sealed.raw create 32m -n SealMe && '$TOOL' sealed.raw add -c lzfse src/sub/deeper/text.txt /text.txt && '$TOOL' sealed.raw add src/sub/r300k.bin /r300k.bin"
	t "seal it" seal sealed.raw SealMe
	t "info shows the root hash" has "root hash" sealed.raw info
	t "write its IPSW root-hash companion" A sealed.raw root-hash sealed.root_hash
	t "the root-hash metadata matches macOS" sh -c "test \$(wc -c < sealed.root_hash) -eq 229 && dd if=sealed.root_hash bs=1 skip=21 2>/dev/null | cmp -s - sealed.metadata"
	t "another root-hash Image4 type is accepted" A sealed.raw root-hash sealed-csys.root_hash -t csys
	t "the requested Image4 type was written" sh -c "test \"\$(dd if=sealed-csys.root_hash bs=1 skip=11 count=4 2>/dev/null)\" = csys"
	f "root-hash refuses to overwrite its image" A sealed.raw root-hash ./sealed.raw
	t "sealed files read" same sealed.raw /text.txt src/sub/deeper/text.txt
	t "the sealed image verifies" verify sealed.raw
	f "sealed volumes refuse changes" A sealed.raw rm /r300k.bin
	t "unseal" A sealed.raw unseal
	t "the unsealed image verifies" verify sealed.raw
	t "unsealed files read" same sealed.raw /r300k.bin src/sub/r300k.bin
	t "the unsealed volume takes changes" A sealed.raw add src/hello.txt /new.txt
	t "and still verifies" verify sealed.raw
fi

echo "# tar archives"
inode() {
	"$TOOL" tar.raw stat "$1" | awk '/Inode:/ {print $2}'
}
mkdir -p tarsrc/sub
printf 'linked\n' > tarsrc/a
ln tarsrc/a tarsrc/sub/b
t "make an image for archives" A tar.raw create 64m -i -n Tar
if command -v tar >/dev/null 2>&1; then
	t "archive a hard link" tar -cf links.tar -C tarsrc .
	t "untar it" A tar.raw untar links.tar /links
	t "hard links share an inode" test "$(inode /links/a)" = "$(inode /links/sub/b)"
	target=$(tar -tvf links.tar | sed -n 's/.* link to //p')
	if tar --version 2>/dev/null | grep -q bsdtar; then
		t "an archive without the link target" tar -cf dangling.tar --exclude "$target" @links.tar
	else
		cp links.tar dangling.tar
		t "an archive without the link target" tar --delete -f dangling.tar "$target"
	fi
	f "untar refuses the dangling hard link" A tar.raw untar dangling.tar /dangling
	cp cmd.out dangling.out
	t "and says why" sh -c 'grep -q "cannot hard link " dangling.out && grep -Fq -- "$1" dangling.out' sh "$target"
	f "and writes nothing" A tar.raw stat /dangling
fi
if [ "$(uname)" = Darwin ] && tar --version 2>/dev/null | grep -q bsdtar; then
	echo "# Apple metadata from macOS tar"
	mkdir -p macsrc
	printf 'meta\n' > macsrc/f
	xattr -w user.k v macsrc/f
	xattr -wx com.apple.FinderInfo 5445585474747874000000000000000000000000000000000000000000000000 macsrc/f
	chflags hidden macsrc/f
	chmod +a "everyone allow read" macsrc/f
	# kauth_filesec for "group:everyone allow read", as macOS stores it
	acl=012cc16d$(printf '0%.0s' $(seq 64))0000000100000000abcdefabcdefabcdefabcdef0000000c0000000100000002
	t "archive with AppleDouble entries" tar -cf mac.tar -C macsrc .
	t "archive with PAX records" tar --no-mac-metadata --xattrs --acls --fflags --format pax -cf macpax.tar -C macsrc .
	for a in mac macpax; do
		t "untar $a.tar" A tar.raw untar $a.tar /$a
		f "$a: no ._ files are left" A tar.raw stat /$a/._f
		t "$a: extended attributes" sh -c "'$TOOL' tar.raw getattr /$a/f user.k | grep -qx v"
		t "$a: Finder info" has com.apple.FinderInfo tar.raw listattr /$a/f
		t "$a: file flags" has hidden tar.raw stat /$a/f
		t "$a: the ACL" sh -c "'$TOOL' tar.raw getattr /$a/f com.apple.system.Security | od -An -tx1 | tr -d ' \n' | grep -qx $acl"
	done
fi
if command -v gtar >/dev/null 2>&1; then
	echo "# GNU tar sparse files"
	head -c 8192 /dev/urandom > tarsrc/sparse
	dd if=/dev/urandom of=tarsrc/sparse bs=4096 seek=200 count=1 conv=notrunc 2>/dev/null
	t "sparse file, old GNU format" gtar --format=gnu --sparse -cf sparse-gnu.tar -C tarsrc sparse
	for v in 0.0 0.1 1.0; do
		t "sparse file, PAX $v" gtar --format=posix --sparse --sparse-version=$v -cf sparse-$v.tar -C tarsrc sparse
	done
	for a in gnu 0.0 0.1 1.0; do
		t "untar sparse-$a.tar" A tar.raw untar sparse-$a.tar /sparse-$a
		t "sparse-$a: contents match" same tar.raw /sparse-$a/sparse tarsrc/sparse
	done
fi
t "the archive image verifies" verify tar.raw

echo "# disk images"
t "export a UDIF image" A img.raw dmg out.dmg
t "the UDIF image verifies" verify out.dmg
t "files read from the UDIF image" same out.dmg /zlib.txt src/sub/deeper/text.txt
t "modify the UDIF image in place" A out.dmg add src/hello.txt /added.txt
t "the modified UDIF image reads" same out.dmg /added.txt src/hello.txt
t "the modified UDIF image verifies" verify out.dmg
t "-o naming the UDIF another way edits it in place" A out.dmg -o ./out.dmg mkdir /same-dmg
ln -s out.dmg out-link.dmg
t "-o through a link edits the UDIF in place" A out.dmg -o out-link.dmg mkdir /linked-dmg
t "the UDIF has both in-place changes" sh -c "'$TOOL' out.dmg stat /same-dmg && '$TOOL' out.dmg stat /linked-dmg"
t "the output link was preserved" test -L out-link.dmg
t "export a zlib UDIF image without a partition map" A img.raw dmg bare.dmg -c zlib -b
t "it verifies" verify bare.dmg
t "export an IMG4 payload" A out.dmg im4p out.im4p -t rdsk -z
t "the payload verifies" verify out.im4p
t "files read from the payload" same out.im4p /added.txt src/hello.txt
t "modify the payload in place" A out.im4p mkdir /in-im4p
t "the modified payload verifies" verify out.im4p
t "export the raw container" A out.im4p raw back.raw -c
t "the raw export verifies" verify back.raw
t "grow a UDIF image" A out.dmg grow 256m
t "the grown UDIF image verifies" verify out.dmg
if [ "${APFS_TEST_NATIVE:-1}" != 0 ] && command -v hdiutil >/dev/null 2>&1; then
	t "hdiutil accepts the UDIF image" hdiutil verify out.dmg
fi

echo "# dmg tool"
t "dmg info lists the partitions" sh -c "'$DMGTOOL' info out.dmg | grep -q Apple_APFS"
t "dmg verify" "$DMGTOOL" verify out.dmg
t "dmg extract" "$DMGTOOL" extract out.dmg whole.img
t "the extracted disk verifies" verify whole.img
t "it matches the disk read from the UDIF image" sh -c "'$TOOL' out.dmg raw whole.raw && cmp -s whole.img whole.raw"
t "dmg extract of one partition" "$DMGTOOL" extract out.dmg part.img Apple_APFS
t "the extracted partition verifies" verify part.img
t "it matches the container read from the UDIF image" sh -c "'$TOOL' out.dmg raw part.raw -c && cmp -s part.img part.raw"
t "dmg build" "$DMGTOOL" build whole.img rebuilt.dmg -c lzfse
t "the rebuilt image verifies" verify rebuilt.dmg
t "files read from the rebuilt image" same rebuilt.dmg /added.txt src/hello.txt
f "dmg extract of a raw image fails" "$DMGTOOL" extract whole.img nothing.img
f "dmg extract onto its input is refused" "$DMGTOOL" extract out.dmg ./out.dmg
f "so is dmg build onto its input" "$DMGTOOL" build whole.img ./whole.img
t "both are intact" sh -c "'$DMGTOOL' verify out.dmg && cmp -s whole.img whole.raw"

if [ -x /usr/bin/aea ]; then
	echo "# Apple Encrypted Archives"
	AEAKEY=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
	t "wrap a UDIF image in AEA" /usr/bin/aea encrypt -i out.dmg -o out.dmg.aea -profile 1 -a lzfse -checksum sha256 -padding none -key-value "hex:$AEAKEY"
	t "apfs-tool reads the AEA-wrapped image" sh -c "'$TOOL' out.dmg.aea -k '$AEAKEY' cat /added.txt | cmp -s - src/hello.txt"
	t "dmg reads the AEA-wrapped image" "$DMGTOOL" info out.dmg.aea -k "$AEAKEY"
	t "dmg extracts the AEA-wrapped image" "$DMGTOOL" extract out.dmg.aea aea.img -k "$AEAKEY"
	t "the AEA extraction matches" cmp -s aea.img whole.img
	f "a wrong AEA key is refused" A out.dmg.aea -k 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e00 info
	f "an AEA archive is not modified in place" A out.dmg.aea -k "$AEAKEY" mkdir /bad
	t "AEA changes can be saved decrypted with -o" A out.dmg.aea -k "$AEAKEY" -o aea-new.dmg mkdir /from-aea
	t "the decrypted output has the change" A aea-new.dmg stat /from-aea
fi

if [ "${APFS_TEST_NATIVE:-1}" != 0 ] && command -v hdiutil >/dev/null 2>&1; then
	echo "# encrypted DMGs"
	t "create an encrypted DMG" sh -c "printf secret | hdiutil create -size 20m -fs APFS -volname Encrypted -encryption AES-128 -stdinpass enc.dmg"
	t "read the encrypted DMG with -k" sh -c "key=\$('$DMGTOOL' info enc.dmg -p secret | sed -n 's/^encrypted:.* key //p') && test -n \"\$key\" && '$TOOL' enc.dmg -k \"\$key\" info >/dev/null"
	t "modify and re-encrypt the DMG with -k" sh -c "key=\$('$DMGTOOL' info enc.dmg -p secret | sed -n 's/^encrypted:.* key //p') && '$TOOL' enc.dmg -k \"\$key\" mkdir /encrypted"
	t "the encrypted DMG keeps the change" sh -c "key=\$('$DMGTOOL' info enc.dmg -p secret | sed -n 's/^encrypted:.* key //p') && '$TOOL' enc.dmg -k \"\$key\" stat /encrypted"
	t "dmg extracts the encrypted image with -k" sh -c "key=\$('$DMGTOOL' info enc.dmg -p secret | sed -n 's/^encrypted:.* key //p') && '$DMGTOOL' extract enc.dmg enc.raw -k \"\$key\""
	t "the extracted encrypted image verifies" verify enc.raw
fi

echo "# trust caches"
t "a volume without code gives an empty cache" sh -c "'$TOOL' img.raw trustcache empty.trustcache && test \$(wc -c < empty.trustcache) -eq 24"
f "trustcache rejects unknown versions" A img.raw trustcache bad.trustcache -v 3
f "trustcache of a missing path fails" A img.raw trustcache bad.trustcache /missing
printf 'int main(void) { return 0; }\n' > tc.c
if command -v codesign >/dev/null 2>&1 && cc -arch arm64 -arch x86_64 -o tc-fat tc.c 2>/dev/null; then
	echo "# trust caches (checked against codesign)"
	cdhash() {
		codesign -dvvv ${2:+-a $2} "$1" 2>&1 | sed -n 's/^CDHash=//p'
	}
	t "sign test binaries" sh -c "cc -o tc-thin tc.c && codesign -s - -f tc-thin && codesign -s - -f tc-fat && cp tc-thin tc-unsigned && codesign --remove-signature tc-unsigned"
	t "add them to an image" sh -c "'$TOOL' tc.raw create 16m && '$TOOL' tc.raw mkdir -p /usr/bin && '$TOOL' tc.raw add tc-thin /usr/bin/thin && '$TOOL' tc.raw add tc-fat /usr/bin/fat && '$TOOL' tc.raw add tc-unsigned /usr/bin/unsigned && '$TOOL' tc.raw add tc.c /tc.c"
	t "make a trust cache" sh -c "'$TOOL' tc.raw trustcache tc.trustcache >tc.list 2>tc.err"
	t "the CDHash of a thin binary matches codesign" grep -q "^$(cdhash tc-thin)  /usr/bin/thin\$" tc.list
	t "so do both of a universal binary" sh -c "grep -q '^$(cdhash tc-fat arm64)  /usr/bin/fat\$' tc.list && grep -q '^$(cdhash tc-fat x86_64)  /usr/bin/fat\$' tc.list"
	t "unsigned binaries are skipped with a warning" grep -q "skipping /usr/bin/unsigned: not signed" tc.err
	t "the cache holds the three hashes (version 1)" sh -c "test \$(wc -c < tc.trustcache) -eq \$((24 + 3 * 22)) && od -An -tx1 -v tc.trustcache | tr -d ' \n' | grep -q $(cdhash tc-fat x86_64)"
	t "version 2 in an IMG4 payload" sh -c "'$TOOL' tc.raw trustcache tc.im4p /usr/bin -v 2 -t rtsc >/dev/null && head -c 16 tc.im4p | grep -q rtsc && test \$(wc -c < tc.im4p) -eq \$((2 + 15 + 2 + 24 + 3 * 24))"
	t "adding to an existing cache keeps it" sh -c "'$TOOL' tc.raw trustcache again.trustcache -m tc.trustcache >/dev/null && cmp tc.trustcache again.trustcache"
	t "existing IM4P caches are read" sh -c "'$TOOL' tc.raw trustcache from-im4p.trustcache /usr/bin/thin -m tc.im4p >/dev/null && test \$(wc -c < from-im4p.trustcache) -eq \$((24 + 3 * 24))"
fi

echo "# mtree"
t "mtree writes a specification" sh -c "'$TOOL' img.raw mtree spec.mtree -T 1600000000 && grep -q '^/set type=file xattrsdigest=none.0\$' spec.mtree && grep -q 'ptime=1600000000.000000000' spec.mtree"
t "mtree -t msys writes an IMG4 payload" sh -c "'$TOOL' img.raw mtree spec.im4p -t msys && head -c 32 spec.im4p | grep -q msys"
f "mtree rejects a bad time" A img.raw mtree bad.mtree -T soon
if [ "${APFS_TEST_NATIVE:-1}" != 0 ] && command -v hdiutil >/dev/null 2>&1 && command -v mtree >/dev/null 2>&1 && command -v aa >/dev/null 2>&1; then
	echo "# mtree (checked against macOS mtree and aa)"
	mt_image() {
		"$TOOL" mt.raw create 32m -n MtreeTest >/dev/null &&
		"$TOOL" mt.raw add src/zero.bin /.file &&
		for n in 'sp ace' 'hash#' 'star*' 'q?' 'br[a]' 'back\slash' "caf$(printf '\303\251')" "ta$(printf '\t')b" 'trail ' 'abcdefghijkl' 'a-long-name-that-wraps.plist'; do
			"$TOOL" mt.raw add src/hello.txt "/$n" || return 1
		done &&
		"$TOOL" mt.raw mkdir -p "/Library/Application Support" /usr/bin /dir16charsxxxxxx &&
		"$TOOL" mt.raw add src/hello.txt /usr/bin/tool &&
		"$TOOL" mt.raw ln /usr/bin/tool /usr/bin/tool-link &&
		"$TOOL" mt.raw symlink /etc private/etc &&
		"$TOOL" mt.raw mknod /usr/bin/fifo p &&
		"$TOOL" mt.raw add src/hello.txt /inline-xattr &&
		"$TOOL" mt.raw setattr /inline-xattr user.small value &&
		"$TOOL" mt.raw add src/hello.txt /stream-xattr &&
		"$TOOL" mt.raw setattr /stream-xattr user.big -f src/sub/r5k.bin &&
		"$TOOL" mt.raw add src/hello.txt /resource-fork &&
		"$TOOL" mt.raw setattr /resource-fork com.apple.ResourceFork -f src/sub/r5k.bin &&
		"$TOOL" mt.raw add -c lzfse src/hello.txt "/Library/Application Support/inline.plist" &&
		"$TOOL" mt.raw add -c lzfse src/sub/deeper/text.txt "/Library/Application Support/fork.plist" &&
		"$TOOL" mt.raw setattr "/Library/Application Support" user.dir value
	}
	# the specifications without the header comments and the entries of . and
	# /.file, which apfs-tool gives the time as Apple's files do
	mt_filter() {
		awk '/^#\t/ { next } /^\.  / || /^    \.file / { skip = 1 } { if(!skip) print; if(skip && !/\\$/) skip = 0 }' "$1"
	}
	mt_compare() {
		"$TOOL" mt.raw mtree mt.ours || return 1
		mkdir -p mnt
		dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -readonly -nobrowse -mountpoint "$WORK/mnt" mt.raw 2>/dev/null | head -1 | awk '{print $1}')
		[ -n "$dev" ] || return 1
		mtree -c -S -k type,inode,siblingid,nxattr,xattrsdigest -p "$WORK/mnt" >mt.macos
		rc=$?
		hdiutil detach "$dev" >/dev/null 2>&1 || hdiutil detach -force "$dev" >/dev/null 2>&1
		[ $rc = 0 ] && mt_filter mt.macos >mt.macos.f && mt_filter mt.ours >mt.ours.f && diff mt.macos.f mt.ours.f
	}
	# im4p_payload <in> <out>: the payload of an IM4P with a 4-character type and a 1-character description
	im4p_payload() {
		b=$(od -An -tu1 -j1 -N1 "$1" | tr -d ' ')
		p=$((2 + (b > 127 ? b - 128 : 0) + 15))
		b=$(od -An -tu1 -j$((p + 1)) -N1 "$1" | tr -d ' ')
		tail -c +$((p + 3 + (b > 127 ? b - 128 : 0))) "$1" >"$2"
	}
	mt_aa() {
		"$TOOL" mt.raw mtree mt.txt -T 1600000000 && "$TOOL" mt.raw mtree mt.im4p -T 1600000000 -t msys &&
		im4p_payload mt.im4p mt.payload && rm -rf mt.aa && mkdir mt.aa &&
		aa extract -i mt.payload -d mt.aa && cmp mt.aa/mtree.txt mt.txt
	}
	t "make an image with odd names, links and attributes" mt_image
	t "the specification matches macOS mtree on the mounted image" mt_compare
	t "aa unpacks the IPSW form to the same specification" mt_aa
fi

echo "# errors"
f "a missing image fails" A missing.raw ls /
f "an unknown command fails" A img.raw frobnicate
f "cat of a directory fails" A img.raw cat /
f "mkdir over a file fails" A img.raw mkdir /zlib.txt
f "extract onto the image is refused" A img.raw extract /lzvn.txt ./img.raw
f "so is a trust cache" A img.raw trustcache ./img.raw
f "and an mtree" A img.raw mtree ./img.raw
t "the image is still consistent" verify img.raw

echo
echo "$PASS passed, $FAIL failed"
if [ $FAIL = 0 ]; then
	cd / && rm -rf "$WORK"
	exit 0
fi
echo "the images are kept in $WORK"
exit 1
