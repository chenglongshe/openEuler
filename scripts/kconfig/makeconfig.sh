#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

if [ ! -f .config ]; then
	echo ".config does not exist"
	exit 1
fi

mv .config .config.new
make openeuler_defconfig
diff -Nur .config .config.new > .config-diff
sed -e "s/\.config\.new/arch\/$1\/configs\/openeuler_defconfig/" -i .config-diff
sed -e "s/\.config/arch\/$1\/configs\/openeuler_defconfig/" -i .config-diff
patch -p0 < .config-diff
rm .config-diff
mv .config.new .config
make olddefconfig
