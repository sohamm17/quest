/* -*- Mode: C -*- */

/* Based on:
 * http://www.uruk.org/mps/, http://www.osdev.org/, http://www.osdever.net/ 
 * and the Intel Multiprocessing Specification v1.4.
 */
#include "arch/i386.h"
#include "kernel.h"
#include "mem/mem.h"
#include "smp/smp.h"
#include "smp/intel_mps.h"
#include "smp/apic.h"
#include "smp/spinlock.h"
#include "util/printf.h"

static int process_mp_fp (struct mp_fp *);
static int process_mp_config (struct mp_config *);
static int add_processor (struct mp_config_processor_entry *);
static struct mp_fp *probe_mp_fp (uint32, uint32);

/*******************************************************
 * Support for the Intel Multiprocessing Specification *
 *******************************************************/

/* The Intel MPS is a legacy standard which specifies a set of tables
 * populated by the BIOS that describe the system well enough to find
 * and start the Application Processors and IO-APICs. */

/* It is officially superseded by ACPI, but every system still
 * supports it.  It has the benefit of being simpler.  However, it is
 * not as informative, particularly when it comes to distinguishing
 * non-uniform architectures and also hyperthreading vs multicore
 * systems. */

/* Quest will attempt to make use of ACPI first, then fall-back to the
 * Intel MPS.  I do not plan on supporting systems pre-MPS, which
 * means Quest is effectively limited to booting on Pentium-Pro+ most
 * likely. */

/* intel_mps_init() -- Returns the number of processors found if
 * successfully initialized, or else 0. */
uint32
intel_mps_init(void)
{
  struct mp_fp *ptr;
  
  if ((ptr = probe_mp_fp (0x9F800, 0xA0000)));
  else if ((ptr = probe_mp_fp (0x0040E, 0x0140E)));
  else if ((ptr = probe_mp_fp (0xE0000, 0xFFFFF)));
  else return 0;

  if(ptr)
    return process_mp_fp(ptr);
  else
    return 0;
}

/* The Intel MPS tables are allowed to be placed in a variety of
 * memory regions available to the BIOS.  Most commonly found in the
 * ShadowROM area.  This function will search for the 4-byte signature
 * in a given memory region and return a pointer to the so-called
 * "Floating Pointer" table. */
static struct mp_fp *
probe_mp_fp (uint32 start, uint32 end)
{
  uint32 i;
  start &= ~0xF;                /* 16-byte aligned */
  for (i = start; i < end; i += 0x10) {
    if (*((volatile uint32 *) i) == MP_FP_SIGNATURE) {
      /* found it */
      return (struct mp_fp *) i;
    }
  }
  return NULL;
}

/* Once found, the pointer to the table is examined for sanity by this
 * function.  It makes further calls to interpret the information
 * found in the tables and begin SMP initialization. */
int
process_mp_fp (struct mp_fp *ptr)
{
  struct mp_config *cfg;

  /* Sanity checks */
  if (ptr == NULL) {
    print ("No SMP support detected.\n");
    return 1;
  }

  if (ptr->signature != MP_FP_SIGNATURE) {
    print ("MP floating pointer structure signature invalid.\n");
    return 1;
  }

  if (ptr->length != 1) {
    print ("MP floating pointer structure reports length != 16 bytes.\n");
    return 1;
  }

  switch (ptr->version) {
  case 1:
    print ("Intel MP specification v1.1\n");
    break;
  case 4:
    print ("Intel MP specification v1.4\n");
    break;
  default:
    print ("Unknown MP specification reported.\n");
    return 1;
  }

  if (checksum ((uint8 *) ptr, sizeof (struct mp_fp)) != 0) {
    print ("MP floating pointer structure failed checksum.\n");
    return 1;
  }

  /* Check MP config table given by floating pointer struct */
  cfg = (struct mp_config *) ptr->mpconfig_ptr;

  process_mp_config (cfg);

  return mp_num_cpus;
}

/* The MP Configuration table is pointed to by the MP Floating Pointer
 * table and contains the information in question that tells us how
 * many processors, IO-APICs, and various bits of useful details about
 * them. */
#define printf com1_printf
static int
process_mp_config (struct mp_config *cfg)
{
  int i;
  uint8 *ptr;
  extern uint32 mp_LAPIC_addr, mp_IOAPIC_addr;
  extern uint32 mp_num_IOAPICs;
  extern mp_IOAPIC_info mp_IOAPICs[];
  extern mp_int_override mp_overrides[];
  extern uint32 mp_num_overrides;


  /* Sanity checks */
  if (cfg == NULL) {
    printf ("MP config pointer is NULL.\n");
    return 1;
  }

  if (cfg->signature != MP_CFG_SIGNATURE) {
    printf ("MP config signature invalid.\n");
    return 1;
  }

  if (cfg->specification_revision != 1 && cfg->specification_revision != 4) {
    printf ("Unknown MP specification reported by MP config table.\n");
    return 1;
  }

  if (checksum ((uint8 *) cfg, cfg->base_table_length) != 0) {
    printf ("MP config table failed checksum.\n");
    return 1;
  }

  printf ("Manufacturer: %.8s Product: %.12s Local APIC: %.8X\n",
          cfg->OEM_id, cfg->product_id, cfg->local_APIC);
  if (cfg->local_APIC)
    mp_LAPIC_addr = cfg->local_APIC;

  /* Check entries */
  ptr = (uint8 *) cfg->entries;
  for (i = 0; i < cfg->entry_count; i++) {
    struct mp_config_entry *entry = (struct mp_config_entry *) ptr;
    switch (*ptr) {
    case MP_CFG_TYPE_PROCESSOR:        /* Processor entry */
      printf ("Processor APIC-id: %X version: %X %s%s",
              entry->processor.APIC_id,
              entry->processor.APIC_version,
              (entry->processor.flags & 1) ? "(enabled)" : "(disabled)",
              (entry->processor.flags & 2) ? " (bootstrap)" : "");
      ptr += sizeof (struct mp_config_processor_entry);

      if (add_processor (&entry->processor))    /* Try to boot it if necessary */
        printf (" (booted)");
      printf ("\n");

      break;

    case MP_CFG_TYPE_BUS:      /* Bus entry, find out which one is ISA */
      printf ("Bus entry-id: %X type: %.6s\n",
              entry->bus.id, entry->bus.bus_type);
      if (entry->bus.bus_type[0] == 'I' &&
          entry->bus.bus_type[1] == 'S' && entry->bus.bus_type[2] == 'A') {
        mp_ISA_bus_id = entry->bus.id;
      }
      ptr += sizeof (struct mp_config_bus_entry);
      break;

    case MP_CFG_TYPE_IO_APIC:  /* IO-APIC entry */
      printf ("IO APIC-id: %X version: %X address: %.8X",
              entry->IO_APIC.id,
              entry->IO_APIC.version, entry->IO_APIC.address);
      if (entry->IO_APIC.flags & 1)
        mp_IOAPIC_addr = entry->IO_APIC.address;
      else
        printf (" (disabled)");
      printf ("\n");

      if (mp_num_IOAPICs == MAX_IOAPICS)
        panic ("Too many IO-APICs.");
      mp_IOAPICs[mp_num_IOAPICs].id = entry->IO_APIC.id;
      mp_IOAPICs[mp_num_IOAPICs].address = entry->IO_APIC.address;
      /* going to assume IO-APICs are listed in order */
      if (mp_num_IOAPICs == 0)
        mp_IOAPICs[mp_num_IOAPICs].startGSI = 0;
      else
        mp_IOAPICs[mp_num_IOAPICs].startGSI =
          mp_IOAPICs[mp_num_IOAPICs - 1].startGSI +
          mp_IOAPICs[mp_num_IOAPICs - 1].numGSIs;

      mp_IOAPICs[mp_num_IOAPICs].numGSIs =
        IOAPIC_num_entries();
      mp_num_IOAPICs++;

      ptr += sizeof (struct mp_config_IO_APIC_entry);
      break;

    case MP_CFG_TYPE_IO_INT:   /* IO-Interrupt entry */
      printf
        ("IO interrupt type: %X flags: %X source: (bus: %X irq: %X) dest: (APIC: %X int: %X)\n",
         entry->IO_int.int_type, entry->IO_int.flags,
         entry->IO_int.source_bus_id, entry->IO_int.source_bus_irq,
         entry->IO_int.dest_APIC_id, entry->IO_int.dest_APIC_intin);

      if (entry->IO_int.source_bus_irq != entry->IO_int.dest_APIC_intin
          && entry->IO_int.int_type == 0) {
        /* not sure if this is the right condition */
        if (mp_num_overrides == MAX_INT_OVERRIDES)
          panic ("Too many interrupt overrides.");
        mp_overrides[mp_num_overrides].src_bus = entry->IO_int.source_bus_id;
        mp_overrides[mp_num_overrides].src_IRQ = entry->IO_int.source_bus_irq;
        mp_overrides[mp_num_overrides].dest_GSI =
          IOAPIC_lookup (entry->IO_int.dest_APIC_id)->startGSI +
          entry->IO_int.dest_APIC_intin;
        mp_num_overrides++;
      }
      ptr += sizeof (struct mp_config_interrupt_entry);
      break;

    case MP_CFG_TYPE_LOCAL_INT:        /* Local-interrupt entry */
      printf
        ("Local interrupt type: %X flags: %X source: (bus: %X irq: %X) dest: (APIC: %X int: %X)\n",
         entry->local_int.int_type, entry->local_int.flags,
         entry->local_int.source_bus_id, entry->local_int.source_bus_irq,
         entry->local_int.dest_APIC_id, entry->local_int.dest_APIC_intin);
      /* It's conceivable that local interrupts could be overriden
       * like IO interrupts, but I have no good examples of it so I
       * will have to defer doing anything about it. */
      ptr += sizeof (struct mp_config_interrupt_entry);
      break;

    default:
      printf ("Unknown entry type: %X at address: %p\n", *ptr, ptr);
      return 1;
    }
  }

  return mp_num_cpus;
}

#undef printf

/* A small wrapper around smp_boot_cpu() which does some checks and
 * maintains two small tables. */
static int
add_processor (struct mp_config_processor_entry *proc)
{
  uint8 apic_id = proc->APIC_id;

  if (!(proc->flags & 1))
    return 0;                   /* disabled processor */
  if (proc->flags & 2)
    return 0;                   /* bootstrap processor */

  if (smp_boot_cpu (apic_id, proc->APIC_version)) {
    CPU_to_APIC[mp_num_cpus] = apic_id;
    APIC_to_CPU[apic_id] = mp_num_cpus;
    mp_num_cpus++;
    return 1;
  } else
    return 0;
}

/* End Intel Multiprocessing Specification implementation */
