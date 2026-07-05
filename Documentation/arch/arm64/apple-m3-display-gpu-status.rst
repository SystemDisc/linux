.. SPDX-License-Identifier: GPL-2.0

======================================
Apple M3 Display/GPU Bring-Up Status
======================================

This is a branch-local status note for the Apple MacBook Air 15-inch M3
(``apple,j615`` / ``apple,t8122``) bring-up work. It records why this
branch keeps the machine on the firmware-provided simple framebuffer
instead of enabling the DCP/KMS and AGX DRM drivers.

Current boot state
==================

The test system boots kernel ``7.1.2-codex-simplefb+`` through a direct
m1n1 Linux payload. The display is currently provided by simplefb:

* ``/sys/class/graphics/fb0/name``: ``simple``
* mode: ``2880x1800``
* bits per pixel: ``32``
* kernel log format: ``x2r10g10b10``

There are no DRM devices under ``/sys/class/drm`` on this boot. The boot
arguments intentionally blacklist ``appledrm``, ``adpdrm``,
``adpdrm_mipi``, and ``asahi`` so that an incomplete DRM probe cannot
take away the known-good framebuffer TTY.

An explicit runtime ``modprobe`` of those four modules succeeds, but it
creates no ``/sys/class/drm/card*`` device and leaves ``fb0`` on simplefb.
The modules therefore appear loadable but unusable without matching T8122
device-tree nodes.

m1n1 exposes its raw Apple Device Tree and stage-2 log through
``reserved-memory`` nodes with ``compatible = "phram"`` and labels ``adt``
and ``m1n1_stage2.log``. This branch enables ``CONFIG_MTD_PHRAM=m`` so
those nodes can be exposed as MTD devices for bring-up captures without
using ``/dev/mem``.

Public refs checked
===================

The following Asahi Linux refs were checked on 2026-07-05:

* ``origin/asahi-wip``: ``64554fe8d4b0``
* ``origin/bits/001-devicetree-m3``: ``7770a0375781``
* ``origin/apple-soc/dt-7.2``: ``c65ab4905e58``
* ``origin/bits/200-dcp``: ``54e3c0d3b3f7``
* ``origin/bits/210-gpu``: ``31af4114de4a``
* ``origin/gpu-next``: ``84850f2ce3aa``

The targeted branch checks and an all-origin grep found no M3 compatible
strings or nodes for:

* ``apple,agx-t8122``
* ``apple,agx-t603*``
* ``apple,t8122-dcp``
* ``apple,t603*-dcp``
* T8122/T603 ``display-subsystem``, DCP mailbox/DART, MIPI, or AGX GPU
  nodes

The AGX driver compatible table in ``origin/bits/210-gpu`` stops at
``apple,agx-t8103``, ``apple,agx-t8112``, ``apple,agx-t600[0-2]``, and
``apple,agx-t602[0-2]``. The DCP driver compatible table in
``origin/bits/200-dcp`` has SoC-specific data for ``apple,t6020-dcp`` and
``apple,t8112-dcp`` plus generic ``apple,dcp``/``apple,dcpext`` entries,
but no T8122/T603 entry.

The checked T8122 device trees do not describe the hardware needed to bind
those drivers. The live FDT exported by the booted system also contains no
DCP, display-subsystem, MIPI, or AGX node for Linux to bind; it only
exposes the firmware-provided framebuffer that simplefb consumes.

Captured T8122 ADT data
=======================

Loading the ``phram`` module on the test system exposes:

* ``/dev/mtd/by-name/adt``
* ``/dev/mtd/by-name/m1n1_stage2.log``

The captured J615/T8122 ADT contains real display and GPU nodes:

* ``/arm-io/disp0``: ``disp0,t8122``
* ``/arm-io/dcp``: ``iop,ascwrap-v6``
* ``/arm-io/dart-dcp``: ``dart,t8110``, stream ``5`` for ``mapper-dcp``
* ``/arm-io/dart-disp0``: ``dart,t8110``, streams ``0`` and ``4`` for
  display and piodma
* ``/arm-io/dispext0`` and ``/arm-io/dcpext`` for external display
* ``/arm-io/sgx``: ``gpu,t8122``
* ``/arm-io/gfx-asc``: ``iop,ascwrap-v6``

The internal display carveouts use the same region IDs that m1n1 already
maps for T8112: ``region-id-49``, ``region-id-50``, ``region-id-57``,
``region-id-94``, and ``region-id-95``. The T8122 DCP/disp resources must
still be added to the Linux device tree before m1n1 can attach those
reserved-memory mappings to Linux device nodes.

Required next data
==================

Real DCP/KMS and AGX enablement must continue to be based on T8122 data,
not copied from T8112 or T602x nodes. The ADT capture provides the first
set of required addresses, IRQs, stream IDs, and carveout IDs, but it does
not by itself make the existing Linux drivers support M3.

At minimum, DCP/KMS needs the T8122 DCP coprocessor resources, mailbox,
DART stream IDs, display pipe/MIPI resources, power domains, panel wiring,
firmware compatibility, and bandwidth/scratch resources expected by the
driver.

AGX needs the T8122 GPU node, ASC/firmware resources, DART and stream IDs,
power domains, clocks, firmware init data expectations, per-SoC hardware
configuration, tunables, p-states, and I/O mappings.

Until those facts are available, this branch should keep the simplefb path
as the boot display path and avoid enabling DRM modules at boot.

The current DCP driver only accepts firmware compatibility ``12.3.0`` or
``13.5.0``. This J615 boot reports OS firmware ``14.7``, so native DCP/KMS
also needs firmware-interface work before it can be expected to produce a
DRM display.
