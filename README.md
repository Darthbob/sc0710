# Fork
This fork aims to add support for linux kernel version >= 6.5.0
# sc0710
Linux driver for the Elgato 4k60 Pro Mk.2

This is a reverse engineering project. The goal is to bring support for the
Elgato 4k60 card to the linux platform.

The primary development platform for the project is Centos 7.5.1804 (Core), although
the driver is expected to work on multiple distributions.

# License
Driver for the Elgato 4k60 Pro mk.2 HDMI capture card.

Copyright (c) 2021 Steven Toth <stoth@kernellabs.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the

GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

# Background
Most of the investigation work is being done on Windows 10.
I'm instrumenting the hardware with debug wiring, identifying common
buses, sketching out a basic hardware digram, understanding the
individual components, monitoring hardware behavior and outlining
a plan for the linux implementation.

The project started Early Jan 2021. One month in, early feb, I understand enough of the
basic design, hardware layout, board debug points to start making an early
linux driver - enough to perform signal detection of the HDMI port and do some
basic hardware servicing.

All of my working notes, analyzer traces, daily journal notes will be
stored in this repository - as a single source for any interested viewers.

I'm maintaining a basic 'developer journal' so interested readers can follow along.
It's not my intension to do a "how to reverse engineer step-by-step" intro guide,
it's really to describe the process, show some of the tooling, highlight things that
worked and things that didn't work. I'm not writing an essay, its random utterances
that may help another developer on a similar project.

At this stage, everything is contained in master. We don't have any branches. As the
project progresses and the driver becomes usable, almost certainly, a new 'cleaner' repo
will emerge and users will not be expected to download this entire repo, with huge images,
analyzer traces, random notes - just to use the driver.

# Status
* Jun 26 2022 - On Ubuntu, /usr/bin/pulseaudio keeps the driver open and prevents make unload during development.
* Jun 26 2022 - Forward port driver, fix broken APIs for use on Ubuntu 22.04. Basic video works on Ubuntu now.
* Jun 26 2022 - Use tag e2908371f4c2b28ea613622815dcf2b4739d3bb7 for Centos 3.10 kernels. After this we're moving to Ubuntu 5.x kernels.
* Feb 15 2021 - Detected colorimetry and colospace HDMI support.
* Feb 15 2021 - Added basic DV Timing support to expose resolution / rate material through the v4l api.
* Feb 14 2021 - Added audio support, PCM 16bit 48KHz.
* Feb 14 2021 - Driver is usable for certain resolutions for video and audio capture via ffmpeg.
* Feb 13 2021 - Overhauled the scatter gather subsystem to support 4k video.
* Feb 11 2021 - First every colorbar still iage captured via the driver.
* Aug  1 2021 - Driver adjusts to auto detect 1280x720p vs 1920x1080p and work accordingly.
* Aug  1 2021 - 4k is untested with the latest changes, but should be full supported.

# TODO
* Test/Support HDR 10bit.
* Intermittent issue during capture, possible short video frame, casuses ffmpeg to error and stall.

# Comments / Support

Email: stoth@kernellabs.com

# Content
* Project root - Driver source code.
* Docs - Daily journal, random notes.
* Traces - Various dump files taken from analyzers.
* Pics - Interesting or curious pictures I've taken during the process.

