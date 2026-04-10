Filesystems kept for OLK-6.6
================================

基于 ``fs/Makefile`` 与 ``arch/x86/configs/openeuler_defconfig``，下述规则给出了在两个场景下需要保留的 ``fs/`` 子目录（仅列出保留目录，不列出可裁剪目录）：

- ``obj-y`` 目录无条件保留；
- ``obj-$(CONFIG_FOO)`` 仅在 ``CONFIG_FOO`` 为 ``y/m`` 时保留；
- 其余未启用（``n``）的目录可以去掉。

.. _fs_keep_server:

场景1：服务器场景
-----------------

openEuler 的 ``openeuler_defconfig`` 已按服务器场景启用相应文件系统，需保留的目录如下（其中 ``mfs/`` 对应 ``CONFIG_MFS_FS``，为具备可编程缓存能力的可堆叠文件系统，详见 ``fs/mfs/Kconfig``，按 Makefile 保留）：

::

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

.. _fs_keep_defconfig_n:

场景2：``arch/x86/configs/openeuler_defconfig`` 中 ``CONFIG_FOO = n`` / ``# CONFIG_FOO is not set`` 的文件系统被剔除后
----------------------------------------------------------------------------------------------------------------

在该场景下，仅保留 defconfig 中启用为 ``y/m`` 的文件系统目录，所有 ``CONFIG_FOO = n``（或 ``# CONFIG_FOO is not set``）对应的目录均移除。计算结果与 :ref:`fs_keep_server` 的保留集保持一致（基于当前 defconfig 计算）。
