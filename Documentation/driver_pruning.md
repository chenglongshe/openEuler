# Driver directory retention guidance for OLK-6.6

This note (bilingual: 中文/English) summarizes which top-level `drivers/` subdirectories are needed under two scenarios, plus a review-needed bucket for symbols not set in the defconfig:

1. **服务器场景 (Server scenario)** – 保留 `drivers/Makefile` 中无条件编译或在 `openeuler_defconfig` 里为 `y`/`m` 的目录 (keep driver directories whose controlling Kconfig options are `y`/`m` or unconditional).
2. **`arch/x86/configs/openeuler_defconfig` 中 `CONFIG=n` 场景 (CONFIG=n in openeuler_defconfig)** – 显式为 `n` 的目录可以去掉 (directories whose controlling options are explicitly `n` can be dropped).

The mapping between driver directories and their controlling Kconfig symbols comes from `drivers/Makefile` in the OLK-6.6 branch of this repository (commit `759078c5fdcfc339f47b2f2e6ca62830a5f46d7e`, generated on 2025-12-25 UTC).

## Server scenario – directories to keep

Directories with configs that are built in (`y`), modular (`m`), or unconditional in `drivers/Makefile`:

```
acpi
amba
android
ata
atm
auxdisplay
base
bcma
block
bluetooth
bus
cache
cdrom
char
char/ipmi
clk
clocksource
connector
cpufreq
cpuidle
crypto
cxl
dax
dca
dma
dma-buf
edac
firewire
firmware
gpio
gpu
hid
hooks
hwmon
hwspinlock
hwtracing/intel_th
i2c
i3c
idle
iio
infiniband
input
input/serio
iommu
irqchip
isdn
leds
macintosh
mailbox
md
media
memstick
message
mfd
misc
mmc
mtd
net
nfc
ntb
nvdimm
nvme
nvmem
parport
pci
pcmcia
perf
pinctrl
platform
pmdomain
pnp
power
powercap
pps
ptp
pwm
ras
reset
rtc
scsi
soc
spi
target
thermal
thunderbolt
tty
ufs
uio
usb
vdpa
vfio
vhost
video
virtio
watchdog
xen
```

> Note: Directories whose configs are not present in `openeuler_defconfig` (see the section titled “Review-needed directories (config not specified)” below) should be evaluated against actual server hardware needs (for example, cross-checking platform BOM, `lspci`/`lsusb` output, and required out-of-tree modules).

## `CONFIG=n` in `openeuler_defconfig` – directories that can be removed

Directories whose controlling Kconfig options are explicitly `n` in `arch/x86/configs/openeuler_defconfig`:

```
accel
accessibility
block/aoe
comedi
counter
cpuinspect
devfreq
eisa
extcon
fpga
gnss
greybus
hsi
hte
hwtracing/stm
input/gameport
interconnect
ipack
mcb
memory
most
of
peci
phy
rapidio
regulator
remoteproc
siox
slimbus
soundwire
spmi
ssb
staging
tee
virt
w1
```

## Review-needed directories (config not specified)

Configs not present in `openeuler_defconfig` default to their Kconfig defaults; keep or drop based on platform requirements:

```
cdx
coda
dio
fsi
hwtracing/coresight
hwtracing/ptt
mux
nubus
opp
parisc
ps3
roh
rpmsg
s390
sbus
sh
tc
video/fbdev/i810
video/fbdev/intelfb
vlynq
zorro
```
