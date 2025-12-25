# OLK-6.6 `fs` 目录裁剪分析

基于 **arch/x86/configs/openeuler_defconfig** 中的配置，对 `fs/` 目录在两种场景下进行梳理：

- 场景 1（服务器场景）：面向常规服务器/虚拟化节点的主流需求，列出应保留与可裁剪的目录。
- 场景 2（`openeuler_defconfig` 中配置为 `n` 的项）：直接依据 `# CONFIG_xxx_FS is not set` 统计可移除目录，并列出已启用的目录。

## 场景 1：服务器场景

**建议保留（常见或必要）：**
- 伪文件系统与基础：`proc/`、`sysfs/`、`kernfs/`、`configfs/`、`tmpfs/`、`hugetlbfs/`、`devpts/`、`pstore/`、`resctrl/`、`debugfs/`。
- 主流本地文件系统：`ext4/`（含 `jbd2/`、`mbcache/`，覆盖 ext3）、`xfs/`、`btrfs/`、`erofs/`、`squashfs/`、`cramfs/`。
- 镜像/可移除介质：`isofs/`、`udf/`、`fat/`、`exfat/`、`ntfs/`、`ntfs3/`。
- 自动/叠加：`autofs/`、`overlayfs/`、`fuse/`（含 virtio-fs）。
- 网络与分布式：`nfs/`、`nfsd/`、`lockd/`、`sunrpc/`、`cifs/`、`smb/`、`ceph/`、`netfs/`、`fscache/`、`cachefiles/`，按需保留 `gfs2/`（需要集群共享块时）。

**可裁剪（服务器常见场景下通常不需要）：**
- 旧/小众或嵌入式：`adfs/`、`affs/`、`befs/`、`bfs/`、`efs/`、`hfs/`、`hfsplus/`、`hpfs/`、`minix/`、`omfs/`、`orangefs/`、`qnx4/`、`qnx6/`、`reiserfs/`、`romfs/`、`sysv/`、`ufs/`、`vxfs/`。
- 闪存/嵌入式专用：`jffs2/`、`ubifs/`、`f2fs/`、`nilfs2/`、`zonefs/`。
- 集群/特定场景，可按需裁剪：`ocfs2/`、`9p/`、`coda/`、`afs/`。

## 场景 2：`openeuler_defconfig` 中配置为 `n` 的项

依据 `openeuler_defconfig`，以下 `fs/` 目录对应的功能被关闭（`CONFIG_xxx_FS` 为 `n`），可在裁剪时移除：
- `adfs/`、`affs/`、`befs/`、`bfs/`、`efs/`、`ecryptfs/`、`ext2/`、`f2fs/`、`hfs/`、`hfsplus/`、`hpfs/`、`jffs2/`、`jfs/`、`minix/`、`nilfs2/`、`ocfs2/`、`omfs/`、`orangefs/`、`qnx4/`、`qnx6/`、`reiserfs/`、`romfs/`、`sysv/`、`ufs/`、`ubifs/`、`vxfs/`、`zonefs/`、`coda/`、`afs/`。

`openeuler_defconfig` 中已启用（`y`/`m`）且需保留的主要目录：
- 基础/伪文件系统：`proc/`、`sysfs/`、`kernfs/`、`configfs/`、`tmpfs/`、`hugetlbfs/`、`pstore/`、`resctrl/`、`debugfs/`。
- 本地文件系统：`ext4/`（含 `jbd2/`、`mbcache/`，兼容 ext3）、`xfs/`、`btrfs/`、`erofs/`、`squashfs/`、`cramfs/`、`mfs/`。
- 镜像/可移除介质：`isofs/`、`udf/`、`fat/`、`exfat/`、`ntfs/`、`ntfs3/`。
- 自动/叠加：`autofs/`、`overlayfs/`、`fuse/`（含 virtio-fs）。
- 网络与分布式：`nfs/`、`nfsd/`、`lockd/`、`sunrpc/`、`cifs/`、`smb/`、`ceph/`、`gfs2/`、`netfs/`、`fscache/`、`cachefiles/`。

上述列表直接对应 `fs/` 目录，可用于精简构建或移除无关文件系统代码。
