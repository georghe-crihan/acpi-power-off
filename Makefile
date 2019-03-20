# Makefile for building the ACPI power off module
# $FreeBSD: src/share/examples/kld/syscall/module/Makefile,v 1.1.6.1 2002/07/17 14:13:53 ru Exp $

KMOD=	acpi_power_off
SRCS=	acpi_power_off.c

.include <bsd.kmod.mk>
