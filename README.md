GNOME MultiWriter
=================

GNOME MultiWriter can be used to write an ISO file to multiple USB devices at
once. Supported drive sizes are between 1GB and 32GB.

This application may be useful for QA testing, to create a GNOME Live image for
a code sprint or to create hundreds of LiveUSB drives for a trade show.

![](https://git.gnome.org/browse/gnome-multi-writer/plain/data/appdata/gmw-startup.png)

Writing a more than 10 devices simultaneously can easy saturate the USB bus for
most storage devices. There are two ways to write more devices in parallel:

 * Use USB 3.0 hubs, even if the storage devices are USB 2.0
 * Install another USB 2.0 PCIe root hub

MultiWriter was originally written as part of the ColorHug project but was
split off as an independent application in 2015.

Bugs
----

Issues (and pull requests) accepted on GitHub; if there's sufficient interest
I'll move the project to git.gnome.org after a few releases.
