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
DCP, display-subsystem, MIPI, or AGX node; it only exposes the
firmware-provided framebuffer that simplefb consumes.

Required next data
==================

Real DCP/KMS and AGX enablement must be based on T8122 data, not copied
from T8112 or T602x nodes. The next safe development step is to capture or
derive the missing T8122 hardware description, most likely with m1n1
proxy/hypervisor tooling before Linux takes over.

At minimum, DCP/KMS needs the T8122 DCP coprocessor resources, mailbox,
DART stream IDs, display pipe/MIPI resources, power domains, panel wiring,
firmware compatibility, and bandwidth/scratch resources expected by the
driver.

AGX needs the T8122 GPU node, ASC/firmware resources, DART and stream IDs,
power domains, clocks, firmware init data expectations, per-SoC hardware
configuration, tunables, p-states, and I/O mappings.

Until those facts are available, this branch should keep the simplefb path
as the boot display path and avoid enabling DRM modules at boot.
