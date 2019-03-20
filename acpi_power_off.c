/*-
 * Copyright (c) 2019 Gheorghe Crihan. 
 * All rights reserved.
 * Credits to others indicated wherever applicable.
 *
 * ACPI power off module for legacy FreeBSD systems. Tested on 4.8 Release #0.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/share/examples/kld/syscall/module/syscall.c,v 1.2 1999/08/28 00:19:23 peter Exp $
 */

/*
 * here is the slighlty complicated ACPI poweroff code
 * Stolen from: https://stackoverflow.com/questions/3145569/how-to-power-down-the-computer-from-a-freestanding-environment
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/module.h>
#include <sys/sysent.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include <sys/eventhandler.h>
#include <stddef.h>
#include <inttypes.h>
typedef uint8_t byte;
typedef uint16_t word;
typedef uint32_t dword;
#include <machine/cpufunc.h> /* outw() et al. */
#include <time.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>

#include <machine/md_var.h>
#include <machine/pc/bios.h> /* BIOS_PADDRTOVADDR */

static int dbg = 1;
static eventhandler_tag eh;

dword *SMI_CMD;
byte ACPI_ENABLE;
byte ACPI_DISABLE;
dword *PM1a_CNT;
dword *PM1b_CNT;
word SLP_TYPa;
word SLP_TYPb;
word SLP_EN;
word SCI_EN;
byte PM1_CNT_LEN;



struct RSDPtr
{
   byte Signature[8];
   byte CheckSum;
   byte OemID[6];
   byte Revision;
   dword *RsdtAddress;
};



struct FACP
{
   byte Signature[4];
   dword Length;
   byte unneded1[40 - 8];
   dword *DSDT;
   byte unneded2[48 - 44];
   dword *SMI_CMD;
   byte ACPI_ENABLE;
   byte ACPI_DISABLE;
   byte unneded3[64 - 54];
   dword *PM1a_CNT_BLK;
   dword *PM1b_CNT_BLK;
   byte unneded4[89 - 72];
   byte PM1_CNT_LEN;
};

static void acpi_power_off_func(void);
static void apm_power_off(void);
static int initAcpi(void);
static int acpiEnable(void);
static unsigned int *acpiCheckRSDPtr(unsigned int *ptr);
static unsigned int *acpiGetRSDPtr(void);
static int acpiCheckHeader(unsigned int *ptr, char *sig);

/* check if the given address has a valid header */
static unsigned int *acpiCheckRSDPtr(unsigned int *ptr)
{
   char *sig = "RSD PTR ";
   struct RSDPtr *rsdp = (struct RSDPtr *) ptr;
   byte *bptr;
   byte check = 0;
   int i;

   if (memcmp(sig, rsdp, 8) == 0)
   {
      /* check checksum rsdpd */
      bptr = (byte *) ptr;
      for (i=0; i<sizeof(struct RSDPtr); i++)
      {
         check += *bptr;
         bptr++;
      }

      /* found valid rsdpd */
      if (check == 0) {
         if (dbg)
#if 0
            if (desc->Revision == 0)
               printf("acpi 1");
            else
               printf("acpi 2");
#endif
         return (unsigned int *) rsdp->RsdtAddress;
      }
   }

   return NULL;
}



/* finds the acpi header and returns the address of the rsdt */
static unsigned int *acpiGetRSDPtr(void)
{
   unsigned int *addr;
   unsigned int *rsdp;
   int ebda;

   /* search below the 1mb mark for RSDP signature */
   for (addr = (unsigned int *) BIOS_PADDRTOVADDR(0x000E0000); (int) addr<BIOS_PADDRTOVADDR(0x00100000); addr += 0x10/sizeof(addr))
   {
      rsdp = acpiCheckRSDPtr(addr);
      if (rsdp != NULL)
         return rsdp;
   }


   /* at address 0x40:0x0E is the RM segment of the ebda */
   ebda = *((short *) 0x40E);   /* get pointer */
   ebda = BIOS_PADDRTOVADDR(ebda*0x10 &0x000FFFFF);   /* transform segment into linear address */

   /* search Extended BIOS Data Area for the Root System Description Pointer signature */
   for (addr = (unsigned int *) ebda; (int) addr<ebda+1024; addr+= 0x10/sizeof(addr))
   {
      rsdp = acpiCheckRSDPtr(addr);
      if (rsdp != NULL)
         return rsdp;
   }

   return NULL;
}



/* checks for a given header and validates checksum */
static int acpiCheckHeader(unsigned int *ptr, char *sig)
{
   if (memcmp(ptr, sig, 4) == 0)
   {
      char *checkPtr = (char *) ptr;
      int len = *(ptr + 1);
      char check = 0;
      while (0<len--)
      {
         check += *checkPtr;
         checkPtr++;
      }
      if (check == 0)
         return 0;
   }
   return -1;
}



static int acpiEnable(void)
{
   int i;

   /* check if acpi is enabled */
   if ( (inw((unsigned int) PM1a_CNT) &SCI_EN) == 0 )
   {
      /* check if acpi can be enabled */
      if (SMI_CMD != 0 && ACPI_ENABLE != 0)
      {
         outb((unsigned int) SMI_CMD, ACPI_ENABLE); /* send acpi enable command */
         /* give 3 seconds time to enable acpi */
         for (i=0; i<300; i++ )
         {
            if ( (inw((unsigned int) PM1a_CNT) &SCI_EN) == 1 )
               break;
            if (dbg)
               printf("Supposed to be sleeping...");
            /* sleep(10); */
         }
         if (PM1b_CNT != 0)
            for (; i<300; i++ )
            {
               if ( (inw((unsigned int) PM1b_CNT) &SCI_EN) == 1 )
                  break;
               if (dbg)
                  printf("supposed to be sleeping...");
               /* sleep(10); */
            }
         if (i<300) {
            printf("enabled acpi.\n");
            return 0;
         } else {
            printf("couldn't enable acpi.\n");
            return -1;
         }
      } else {
         printf("no known way to enable acpi.\n");
         return -1;
      }
   } else {
      if (dbg)
         printf("acpi was already enabled.\n");
      return 0;
   }
}

/*
 * bytecode of the \_S5 object
 * -----------------------------------------
 *        | (optional) |    |    |    |   
 * NameOP | \          | _  | S  | 5  | _
 * 08     | 5A         | 5F | 53 | 35 | 5F
 *
 * -----------------------------------------------------------------------------------------------------------
 *           |           |              | ( SLP_TYPa   ) | ( SLP_TYPb   ) | ( Reserved   ) | (Reserved    )
 * PackageOP | PkgLength | NumElements  | byteprefix Num | byteprefix Num | byteprefix Num | byteprefix Num
 * 12        | 0A        | 04           | 0A         05  | 0A          05 | 0A         05  | 0A         05
 *
 *----this-structure-was-also-seen----------------------
 * PackageOP | PkgLength | NumElements |
 * 12        | 06        | 04          | 00 00 00 00
 *
 * (Pkglength bit 6-7 encode additional PkgLength bytes [shouldn't be the case here])
 */

static int initAcpi(void)
{
   unsigned int *ptr = acpiGetRSDPtr();
   struct FACP *facp;

   /* check if address is correct  ( if acpi is available on this pc ) */
   if (ptr != NULL && acpiCheckHeader(ptr, "RSDT") == 0)
   {
      /* the RSDT contains an unknown number of pointers to acpi tables */
      int entrys = *(ptr + 1);
      entrys = (entrys-36) /4;
      ptr += 36/4;   /* skip header information */

      while (0<entrys--)
      {
         /* check if the desired table is reached */
         if (acpiCheckHeader((unsigned int *) *ptr, "FACP") == 0)
         {
            entrys = -2;
            facp = (struct FACP *) *ptr;
            if (acpiCheckHeader((unsigned int *) facp->DSDT, "DSDT") == 0)
            {
               /* search the \_S5 package in the DSDT */
               char *S5Addr = (char *) facp->DSDT +36; /* skip header */
               int dsdtLength = *(facp->DSDT+1) -36;
               while (0 < dsdtLength--)
               {
                  if ( memcmp(S5Addr, "_S5_", 4) == 0)
                     break;
                  S5Addr++;
               }
               /* check if \_S5 was found */
               if (dsdtLength > 0)
               {
                  /* check for valid AML structure */
                  if ( ( *(S5Addr-1) == 0x08 || ( *(S5Addr-2) == 0x08 && *(S5Addr-1) == '\\') ) && *(S5Addr+4) == 0x12 )
                  {
                     S5Addr += 5;
                     S5Addr += ((*S5Addr &0xC0)>>6) +2;   /* calculate PkgLength size */

                     if (*S5Addr == 0x0A)
                        S5Addr++;   /* skip byteprefix */
                     SLP_TYPa = *(S5Addr)<<10;
                     S5Addr++;

                     if (*S5Addr == 0x0A)
                        S5Addr++;   /* skip byteprefix */
                     SLP_TYPb = *(S5Addr)<<10;

                     SMI_CMD = facp->SMI_CMD;

                     ACPI_ENABLE = facp->ACPI_ENABLE;
                     ACPI_DISABLE = facp->ACPI_DISABLE;

                     PM1a_CNT = facp->PM1a_CNT_BLK;
                     PM1b_CNT = facp->PM1b_CNT_BLK;

                     PM1_CNT_LEN = facp->PM1_CNT_LEN;

                     SLP_EN = 1<<13;
                     SCI_EN = 1;

                     return 0;
                  } else {
                     printf("\\_S5 parse error.\n");
                  }
               } else {
                  printf("\\_S5 not present.\n");
               }
            } else {
               printf("DSDT invalid.\n");
            }
         }
         ptr++;
      }
      printf("no valid FACP present.\n");
   } else {
      printf("no acpi.\n");
   }

   return -1;
}

/* Stolen from: https://stackoverflow.com/questions/678458/shutdown-the-computer-using-assembly
 */
/**
 * apm_power_off - ask the BIOS to power off
 *
 * Handle the power off sequence. This is the one piece of code we
 * will execute even on SMP machines. In order to deal with BIOS
 * bugs we support real mode APM BIOS power off calls. We also make
 * the SMP call on CPU0 as some systems will only honour this call
 * on their first cpu.
 */

static void apm_power_off(void)
{
#if 0
 unsigned char po_bios_call[] = {
  0xb8, 0x00, 0x10, /* movw  $0x1000,ax  */
  0x8e, 0xd0,  /* movw  ax,ss       */
  0xbc, 0x00, 0xf0, /* movw  $0xf000,sp  */
  0xb8, 0x07, 0x53, /* movw  $0x5307,ax  */
  0xbb, 0x01, 0x00, /* movw  $0x0001,bx  */
  0xb9, 0x03, 0x00, /* movw  $0x0003,cx  */
  0xcd, 0x15  /* int   $0x15       */
 };

 /* Some bioses don't like being called from CPU != 0 */
 if (apm_info.realmode_power_off) {
  set_cpus_allowed_ptr(current, cpumask_of(0));
  machine_real_restart(po_bios_call, sizeof(po_bios_call));
 } else {
  (void)set_system_power_state(APM_STATE_OFF);
 }
#else
    printf("Nothing.\n");
#endif
}

static void acpi_power_off_func(void)
{
   printf("Trying to shutdown via ACPI... %s\n", (SCI_EN == 0) ? "false":"true");
   /**/ initAcpi(); /**/
   /**/ acpiEnable();
   /* SCI_EN is set to 1 if acpi shutdown is possible */
   if (SCI_EN == 0)
      return;
   else {
      printf("Attempting APM power off...\n");
      apm_power_off();
   }

   acpiEnable();

   /* send the shutdown command */
   outw((unsigned int) PM1a_CNT, SLP_TYPa | SLP_EN );
   if ( PM1b_CNT != 0 )
      outw((unsigned int) PM1b_CNT, SLP_TYPb | SLP_EN );

   printf("acpi poweroff failed.\n");
}

/*
 * The function called at load/unload.
 */

static int
load (struct module *module, int cmd, void *arg)
{
	int error = 0;

	switch (cmd) {
	case MOD_LOAD :
                eh = EVENTHANDLER_REGISTER(shutdown_final, acpi_power_off_func, NULL, SHUTDOWN_PRI_LAST + 99); /* Prior to shutdown_halt() in kern/kern_shutdown.c */
		printf ("ACPI power off loaded at %X\n", (int)eh);
		break;
	case MOD_UNLOAD :
                EVENTHANDLER_DEREGISTER(shutdown_final, eh);
		printf ("ACPI power off unloaded from %X\n", (int)eh);
		break;
	default :
		error = EINVAL;
		break;
	}
	return error;
}

static moduledata_t acpi_power_off_mod = {
        "acpi_power_off_mod",
        load,
        NULL
        };

DECLARE_MODULE(acpi_power_off, acpi_power_off_mod, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_VERSION(acpi_power_off, 1);
