.. SPDX-License-Identifier: GPL-2.0

=================
HoloLens 1 retail
=================

The ``hololens1_retail_defconfig`` target builds a 32-bit x86 kernel for
first-generation retail HoloLens bring-up. It includes an IA32 EFI stub
and can boot a RAM root from an initramfs with a 32-bit ``/init``.

Build the kernel and modules from the top of the source tree::

    out=/absolute/path/to/hololens1-build
    make ARCH=x86 O="$out" hololens1_retail_defconfig
    make ARCH=x86 O="$out" -j"$(nproc)" bzImage modules

The EFI-capable image is ``arch/x86/boot/bzImage`` in the output directory.
For information on loading an initramfs, see
:doc:`../../../admin-guide/efi-stub`.

The kernel includes ACPI, Cherryview GPIO and DesignWare platform I2C
for the status LED controller. It includes both ACPI and PCI SDHCI
drivers to discover eMMC after boot. SimpleDRM can use a framebuffer
supplied by the firmware. The DesignWare USB PCI driver and the configfs
CDC ACM gadget components are built as modules for USB diagnostics.

The FAN5702 and LED timer trigger build as modules. Include
``drivers/leds/leds-fan5702.ko`` in the initramfs or install it alongside
the retail kernel. The module matches ACPI ID ``MSFT0A0B`` and provides
``hololens::status-1`` through ``hololens::status-5`` under
``/sys/class/leds``. Each exposes brightness levels 0 through 40.
Record the physical segment controlled by each channel on the headset.

A 32-bit UEFI virtual machine can exercise the EFI and initramfs path.
Headset bring-up requires a firmware-authorized launch path. After boot,
record the live ACPI and PCI inventory and check which eMMC interface is
present. Output through the firmware framebuffer and the USB device mode
both require verification on the headset.
