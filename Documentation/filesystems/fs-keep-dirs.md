# Filesystems kept for OLK-6.6

基于 `fs/Makefile` 中的目录与 `arch/x86/configs/openeuler_defconfig` 的配置，列出在两个场景下需要保留的 `fs/` 子目录（只列出保留目录，不列出可去掉的目录）。

- 判定规则：`obj-y` 无条件保留；`obj-$(CONFIG_FOO)` 仅在 `CONFIG_FOO` 为 `y/m` 时保留；其余未启用（`n`）的目录可裁剪。

## 场景1：服务器场景
openEuler 的 `openeuler_defconfig` 已按服务器场景启用相应文件系统，需保留的目录如下：

```
autofs/
btrfs/
cachefiles/
ceph/
configfs/
cramfs/
debugfs/
devpts/
dlm/
efivarfs/
erofs/
exfat/
exportfs/
ext4/
fat/
fscache/
fuse/
gfs2/
hostfs/
hugetlbfs/
iomap/
isofs/
jbd2/
kernfs/
lockd/
mfs/
netfs/
nfs/
nfs_common/
nfsd/
nls/
notify/
ntfs/
ntfs3/
overlayfs/
proc/
pstore/
quota/
ramfs/
resctrl/
smb/
squashfs/
sysfs/
tracefs/
udf/
unicode/
xfs/
```

## 场景2：`arch/x86/configs/openeuler_defconfig` 中 `config = n`
在该 defconfig 中未启用的文件系统对应目录均可去掉，保留目录与上表一致（因 defconfig 已体现服务器默认启用集）。为便于对比，保留目录再次列出：

```
autofs/
btrfs/
cachefiles/
ceph/
configfs/
cramfs/
debugfs/
devpts/
dlm/
efivarfs/
erofs/
exfat/
exportfs/
ext4/
fat/
fscache/
fuse/
gfs2/
hostfs/
hugetlbfs/
iomap/
isofs/
jbd2/
kernfs/
lockd/
mfs/
netfs/
nfs/
nfs_common/
nfsd/
nls/
notify/
ntfs/
ntfs3/
overlayfs/
proc/
pstore/
quota/
ramfs/
resctrl/
smb/
squashfs/
sysfs/
tracefs/
udf/
unicode/
xfs/
```
