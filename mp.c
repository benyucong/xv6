#include "types.h"
#include "mp.h"
#include "defs.h"
#include "param.h"
#include "x86.h"
#include "traps.h"
#include "mmu.h"
#include "proc.h"

static char* buses[] = {
	"CBUSI ",
	"CBUSII",
	"EISA  ",
	"FUTURE",
	"INTERN",
	"ISA   ",
	"MBI   ",
	"MBII  ",
	"MCA   ",
	"MPI   ",
	"MPSA  ",
	"NUBUS ",
	"PCI   ",
	"PCMCIA",
	"TC    ",
	"VL    ",
	"VME   ",
	"XPRESS",
	0,
};

struct cpu cpus[NCPU];
int ncpu;
uchar ioapic_id;

static struct cpu *bcpu;
static struct mp* mp;  // The MP floating point structure

static struct mp*
mp_scan(uchar *addr, int len)
{
  uchar *e, *p, sum;
  int i;

  cprintf("scanning: 0x%x\n", (uint)addr);
  e = addr+len;
  for(p = addr; p < e; p += sizeof(struct mp)){
    if(memcmp(p, "_MP_", 4))
      continue;
    sum = 0;
    for(i = 0; i < sizeof(struct mp); i++)
      sum += p[i];
    if(sum == 0)
      return (struct mp *)p;
  }
  return 0;
}

static struct mp*
mp_search(void)
{
  uchar *bda;
  uint p;
  struct mp *mp;

  /*
   * Search for the MP Floating Pointer Structure, which according to the
   * spec is in one of the following three locations:
   * 1) in the first KB of the EBDA;
   * 2) in the last KB of system base memory;
   * 3) in the BIOS ROM between 0xE0000 and 0xFFFFF.
   */
  bda = (uchar*) 0x400;
  if((p = (bda[0x0F]<<8)|bda[0x0E])){
    if((mp = mp_scan((uchar*) p, 1024)))
      return mp;
  }
  else{
    p = ((bda[0x14]<<8)|bda[0x13])*1024;
    if((mp = mp_scan((uchar*)p-1024, 1024)))
      return mp;
  }
  return mp_scan((uchar*)0xF0000, 0x10000);
}

static int 
mp_detect(void)
{
  struct mpctb *pcmp;
  uchar *p, sum;
  uint length;

  /*
   * Search for an MP configuration table. For now,
   * don't accept the default configurations (physaddr == 0).
   * Check for correct signature, calculate the checksum and,
   * if correct, check the version.
   * To do: check extended table checksum.
   */
  if((mp = mp_search()) == 0 || mp->physaddr == 0)
    return 1;

  pcmp = (struct mpctb *) mp->physaddr;
  if(memcmp(pcmp, "PCMP", 4))
    return 2;

  length = pcmp->length;
  sum = 0;
  for(p = (uchar*)pcmp; length; length--)
    sum += *p++;

  if(sum || (pcmp->version != 1 && pcmp->version != 4))
    return 3;

  return 0;
}

void
mp_init(void)
{ 
  int r;
  uchar *p, *e;
  struct mpctb *mpctb;
  struct mppe *proc;
  struct mpbe *bus;
  struct mpioapic *ioapic;
  struct mpie *intr;
  int i;
  uchar byte;

  ncpu = 0;
  if ((r = mp_detect()) != 0) return;

  cprintf("Mp spec rev #: %x imcrp 0x%x\n", mp->specrev, mp->imcrp);

  /*
   * Run through the table saving information needed for starting
   * application processors and initialising any I/O APICs. The table
   * is guaranteed to be in order such that only one pass is necessary.
   */
  mpctb = (struct mpctb *) mp->physaddr;
  lapicaddr = (uint *) mpctb->lapicaddr;
  cprintf("apicaddr: %x\n", lapicaddr);
  p = ((uchar*)mpctb)+sizeof(struct mpctb);
  e = ((uchar*)mpctb)+mpctb->length;

  while(p < e) {
    switch(*p){
    case MPPROCESSOR:
      proc = (struct mppe *) p;
      cpus[ncpu].apicid = proc->apicid;
      cprintf("a processor %x\n", cpus[ncpu].apicid);
      if (proc->flags & MPBP) {
	bcpu = &cpus[ncpu];
      }
      ncpu++;
      p += sizeof(struct mppe);
      continue;
    case MPBUS:
      bus = (struct mpbe *) p;
      for(i = 0; buses[i]; i++){
	if(strncmp(buses[i], bus->string, sizeof(bus->string)) == 0)
	  break;
      }
      cprintf("a bus %d\n", i);
      p += sizeof(struct mpbe);
      continue;
    case MPIOAPIC:
      ioapic = (struct mpioapic *) p;
      cprintf("an I/O APIC: id %d %x\n", ioapic->apicno, ioapic->flags);
      ioapic_id = ioapic->apicno;
      p += sizeof(struct mpioapic);
      continue;
    case MPIOINTR:
      intr = (struct mpie *) p;
      // cprintf("an I/O intr: type %d flags 0x%x bus %d souce bus irq %d dest ioapic id %d dest ioapic intin %d\n", intr->intr, intr->flags, intr->busno, intr->irq, intr->apicno, intr->intin);
      p += sizeof(struct mpie);
      continue;
    default:
      cprintf("mpinit: unknown PCMP type 0x%x (e-p 0x%x)\n", *p, e-p);
      while(p < e){
	cprintf("%uX ", *p);
	p++;
      }
      break;
    }
  }

  if (mp->imcrp) {  // it appears that bochs doesn't support IMCR, and code won't run
    outb(0x22, 0x70);	/* select IMCR */
    byte = inb(0x23);	/* current contents */
    byte |= 0x01;	/* mask external INTR */
    outb(0x23, byte);	/* disconnect 8259s/NMI */
  }

  cprintf("ncpu: %d boot %d\n", ncpu, bcpu-cpus);
}


int
mp_bcpu(void)
{
  return bcpu-cpus;
}

extern void mpmain(void);

#define APBOOTCODE 0x7000 // XXX hack

void
mp_startthem(void)
{
  extern uchar _binary_bootother_start[], _binary_bootother_size[];
  extern int main();
  int c;

  memmove((void *) APBOOTCODE,_binary_bootother_start, 
	  (uint) _binary_bootother_size);

  for(c = 0; c < ncpu; c++){
    if (c == cpu()) continue;
    cprintf ("cpu%d: starting processor %d\n", cpu(), c);
    *(uint *)(APBOOTCODE-4) = (uint) (cpus[c].mpstack) + MPSTACK; // tell it what to use for %esp
    *(uint *)(APBOOTCODE-8) = (uint)mpmain; // tell it where to jump to
    lapic_startap(cpus[c].apicid, (uint) APBOOTCODE);
    while(cpus[c].booted == 0)
      ;
  }
}