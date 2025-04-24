#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

if [ ! -f .config ] || [ ! -f .config.old ]; then
	echo ".config or .config.old does not exist"
	exit 1
fi

diff -Nur .config.old .config > .config-diff
sed -e "s/\.config\.old/arch\/$1\/configs\/openeuler_defconfig/" -i .config-diff
sed -e "s/\.config/arch\/$1\/configs\/openeuler_defconfig/" -i .config-diff
patch -p0 < .config-diff
rm .config-diff
