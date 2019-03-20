# acpi-power-off
ACPI power off kernel module (KLD) for legacy FreeBSD systems (4.8+).

It demonstrates a few useful tecniques, in particular:
* Hooking the power off vector
* Scanning (searching for strings) EBDA (Extended BIOS Data Area).
* Scanning (searching for strings) in memory area below 1Mb.

To compile just type:
```
$ make
```
