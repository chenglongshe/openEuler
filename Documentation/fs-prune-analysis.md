# fs 目录裁剪分析（基于 OLK-6.6）

问题：梳理 `fs/` 目录在两个场景下需要保留或可以去掉的子目录。

## 场景 1：服务器场景（常规服务器/虚拟化/容器）

### 推荐保留
- 伪文件系统与基础支撑：`proc`、`sysfs`、`tmpfs`、`hugetlbfs`、`configfs`、`efivarfs`、`resctrl`、`pstore`、`tracefs`、`debugfs`。
- 主流本地文件系统：`ext4`（包含 `ext3` 兼容）、`xfs`、`btrfs`、`erofs`，常用镜像/只读介质：`cramfs`、`squashfs`、`isofs`、`udf`。
- 通用可读写格式：`fat`/`vfat`/`exfat`、`ntfs`/`ntfs3`。
- 网络/分布式与容器相关：`nfs`/`nfsd`（含 `lockd`、`sunrpc`）、`ceph`、`cifs`/`smbfs`、`gfs2`（含 `dlm`）、`fuse`/`virtio_fs`、`overlayfs`、`autofs`。
- 缓存与配套：`netfs`、`fscache`、`cachefiles`、`exportfs`、`quota`、`nls`、`unicode`。

### 可去掉（服务器场景极少使用，若确有需求再保留）
- 传统/小众或嵌入式文件系统：`ext2`、`reiserfs`、`jfs`、`ocfs2`（若无集群需求可裁剪）、`nilfs2`、`f2fs`、`zonefs`、`orangefs`、`ecryptfs`、`hfs`、`hfsplus`、`befs`、`bfs`/`freevxfs`、`efs`、`jffs2`、`ubifs`、`minix`、`omfs`、`hpfs`、`qnx4`、`qnx6`、`romfs`、`sysv`、`ufs`。
- 其他很少在服务器使用的实现：`9p`、`afs`、`coda`、`vboxsf`、`openpromfs`、`adfs`、`affs`、`smb`（ksmbd 服务器端，如无需求可裁剪）。

## 场景 2：`arch/x86/configs/openeuler_defconfig` 中 `CONFIG=n` 对应可裁剪的目录
直接在 defconfig 中被设置为未启用（`CONFIG_xxx is not set`），对应代码可在裁剪时删除：
- 本地/集群文件系统：`ext2`、`reiserfs`、`jfs`、`ocfs2`、`nilfs2`、`f2fs`、`zonefs`。
- 小众/嵌入式文件系统：`adfs`、`affs`、`befs`、`bfs`/`freevxfs`、`efs`、`orangefs`、`ecryptfs`、`hfs`、`hfsplus`、`jffs2`、`ubifs`、`minix`、`omfs`、`hpfs`、`qnx4`、`qnx6`、`romfs`、`sysv`、`ufs`。
- 网络及其他：`9p`、`afs`、`coda`、`smb`（`CONFIG_SMB_SERVER` 未启用）。

对应 defconfig 已经启用（`=y`/`=m`）的目录（如 `ext3/ext4`、`xfs`、`btrfs`、`gfs2`、`erofs`、`cramfs`、`squashfs`、`isofs`、`udf`、`fat`/`exfat`、`ntfs`/`ntfs3`、`fuse`/`virtio_fs`、`overlayfs`、`nfs`/`nfsd`、`ceph`、`cifs`/`smbfs`、`autofs`、`proc`、`sysfs`、`configfs`、`resctrl`、`pstore`、`fscache` 等）应保留。
