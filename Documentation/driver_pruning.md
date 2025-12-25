# Driver directory retention guidance (OLK-6.6)

This note (bilingual: 中文/English) summarizes which top-level `drivers/` subdirectories are needed under two scenarios:

1. **服务器场景（Server scenario）** – keep the drivers whose controlling Kconfig options are built as `y`/`m` or unconditional in `drivers/Makefile`.
2. **`arch/x86/configs/openeuler_defconfig` 中 `CONFIG=n` 场景** – driver directories whose controlling options are explicitly set to `n` can be dropped.

The mapping between driver directories and their controlling Kconfig symbols comes from `drivers/Makefile`.

## Server scenario – directories to keep

Directories with configs that are built in (`y`), modular (`m`), or unconditional in `drivers/Makefile`:

```
acpi, amba, android, ata, atm, auxdisplay, base, bcma, block, bluetooth, bus,
cache, cdrom, char, char/ipmi, clk, clocksource, connector, cpufreq, cpuidle,
crypto, cxl, dax, dca, dma, dma-buf, edac, firewire, firmware, gpio, gpu, hid,
hooks, hwmon, hwspinlock, hwtracing/intel_th, i2c, i3c, idle, iio, infiniband,
input, input/serio, iommu, irqchip, isdn, leds, macintosh, mailbox, md, media,
memstick, message, mfd, misc, mmc, mtd, net, nfc, ntb, nvdimm, nvme, nvmem,
parport, pci, pcmcia, perf, pinctrl, platform, pmdomain, pnp, power, powercap,
pps, ptp, pwm, ras, reset, rtc, scsi, soc, spi, target, thermal, thunderbolt,
tty, ufs, uio, usb, vdpa, vfio, vhost, video, virtio, watchdog, xen
```

> Note: Directories whose configs are not present in `openeuler_defconfig` (listed below as “review”) should be evaluated against actual server hardware needs.

## `CONFIG=n` in `openeuler_defconfig` – directories that can be removed

Directories whose controlling Kconfig options are explicitly `n` in `arch/x86/configs/openeuler_defconfig`:

```
accel, accessibility, block/aoe, comedi, counter, cpuinspect, devfreq, eisa,
extcon, fpga, gnss, greybus, hsi, hte, hwtracing/stm, input/gameport,
interconnect, ipack, mcb, memory, most, of, peci, phy, rapidio, regulator,
remoteproc, siox, slimbus, soundwire, spmi, ssb, staging, tee, virt, w1
```

## Review-needed directories (config not specified)

Configs not present in `openeuler_defconfig` default to their Kconfig defaults; keep or drop based on platform requirements:

```
cdx, coda, dio, fsi, hwtracing/coresight, hwtracing/ptt, mux, nubus, opp,
parisc, ps3, roh, rpmsg, s390, sbus, sh, tc, video/fbdev/i810,
video/fbdev/intelfb, vlynq, zorro
```
