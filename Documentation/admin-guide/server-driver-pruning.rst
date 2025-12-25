======================================
Server driver pruning for OLK-6.6
======================================

This note lists driver subsystems under ``drivers/`` that are typically
*not* required on rack/server deployments (for example x86 servers running
openEuler without local displays or mobile/embedded peripherals). Disable
them in ``make menuconfig`` only after confirming the hardware inventory.

Keep enabled for servers
------------------------

- Storage and networking: ``block/``, ``scsi/``, ``nvme/``, ``md/``, ``net/``,
  ``usb/``, ``pci/``.
- Reliability and power: ``cpufreq/``, ``cpuidle/``, ``edac/``, ``ras/``,
  ``thermal/``, ``powercap/``.
- Platform glue commonly used on servers: ``firmware/``, ``i2c/``, ``hwmon/``,
  ``rtc/``, ``iommu/``, ``vfio/``, ``virtio/``, ``vhost/``, ``xen/`` (as
  needed for virtualization).

Commonly disabled on headless servers
-------------------------------------

- ``android/``: binder/ashmem mobile stack.
- ``atm/``: legacy ATM networking hardware.
- ``auxdisplay/``: character LCD/LED front panels.
- ``bcma/``: Broadcom AMBA bus for embedded boards.
- ``firewire/``: IEEE1394 storage/audio/video devices.
- ``gpu/`` and ``video/``: display pipelines when no local GPU output is
  required.
- ``media/``: TV/radio/camera capture devices.
- ``macintosh/``: Apple-specific glue drivers.
- ``memstick/``: MemoryStick storage.
- ``pcmcia/`` and ``parport/``: PCMCIA/CardBus and parallel ports.
- ``thunderbolt/``: only needed when the chassis exposes Thunderbolt ports.
- ``nfc/`` and ``bluetooth/``: short-range wireless peripherals.
- ``input/``, ``input/serio/`` and most ``hid/``: keyboards/mice/touchpads
  can be disabled on fully headless systems; keep minimal USB HID if remote
  KVM emulates it.
- ``leds/``: indicator LEDs, unless the platform uses them for status.
- SoC and embedded fabric drivers (typically not needed on x86 servers):
  ``amba/``, ``mailbox/``, ``hwspinlock/``, ``soc/``.
