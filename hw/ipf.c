/*
 * Itanium Platforma Emulator derived from QEMU PC System Emulator
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * Copyright (c) 2007 Intel
 * Ported for IA64 Platform Zhang Xiantao <xiantao.zhang@intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "vl.h"
#include "firmware.h"
#include "ia64intrin.h"
#include <unistd.h>

#ifdef USE_KVM
#include "qemu-kvm.h"
extern int kvm_allowed;
#endif

/* output Bochs bios info messages */
//#define DEBUG_BIOS

#define FW_FILENAME "Flash.fd"

/* Leave a chunk of memory at the top of RAM for the BIOS ACPI tables.  */
#define ACPI_DATA_SIZE       0x10000

static fdctrl_t *floppy_controller;
static PCIDevice *i440fx_state;

static const int ide_iobase[2] = { 0x1f0, 0x170 };
static const int ide_iobase2[2] = { 0x3f6, 0x376 };
static const int ide_irq[2] = { 14, 15 };

#define NE2000_NB_MAX 6

static int ne2000_io[NE2000_NB_MAX] = { 0x300, 0x320, 0x340, 0x360, 0x280, 0x380 };
static int ne2000_irq[NE2000_NB_MAX] = { 9, 10, 11, 3, 4, 5 };

static int serial_io[MAX_SERIAL_PORTS] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8 };
static int serial_irq[MAX_SERIAL_PORTS] = { 4, 3, 4, 3 };

static int parallel_io[MAX_PARALLEL_PORTS] = { 0x378, 0x278, 0x3bc };
static int parallel_irq[MAX_PARALLEL_PORTS] = { 7, 7, 7 };

#ifdef HAS_AUDIO
static void audio_init (PCIBus *pci_bus, qemu_irq *pic)
{
	struct soundhw *c;
	int audio_enabled = 0;

	for (c = soundhw; !audio_enabled && c->name; ++c) {
		audio_enabled = c->enabled;
	}

	if (audio_enabled) {
		AudioState *s;

		s = AUD_init ();
		if (s) {
			for (c = soundhw; c->name; ++c) {
				if (c->enabled) {
					if (c->isa) {
						c->init.init_isa (s, pic);
					}
					else {
					if (pci_bus) {
						c->init.init_pci (pci_bus, s);
						}
					}
				}
			}
		}
	}
}
#endif

static void pc_init_ne2k_isa(NICInfo *nd, qemu_irq *pic)
{
	static int nb_ne2k = 0;

	if (nb_ne2k == NE2000_NB_MAX)
		return;
	isa_ne2000_init(ne2000_io[nb_ne2k], pic[ne2000_irq[nb_ne2k]], nd);
	nb_ne2k++;
}

#ifdef USE_KVM
extern kvm_context_t kvm_context;
extern int kvm_allowed;

void kvm_sync_icache(unsigned long address, int len)
{
	int l;

	for(l = 0; l < (len + 32); l += 32)
		__ia64_fc(address + l);

	ia64_sync_i();
	ia64_srlz_i();
}
#endif

static void main_cpu_reset(void *opaque)
{
	CPUState *env = opaque;
	cpu_reset(env);
}
static void pic_irq_request(void *opaque, int irq, int level)
{
	fprintf(stderr,"pic_irq_request called!\n");
}

/* IPF hardware initialisation */
static void ipf_init1(ram_addr_t ram_size, int vga_ram_size, int boot_device,
		DisplayState *ds, const char **fd_filename, int snapshot,
		const char *kernel_filename, const char *kernel_cmdline,
		const char *initrd_filename,
		int pci_enabled)
{
	char buf[1024];
	int i;
	ram_addr_t ram_addr, vga_ram_addr;
	ram_addr_t above_4g_mem_size = 0;
	PCIBus *pci_bus;
	int piix3_devfn = -1;
	CPUState *env;
	NICInfo *nd;
	qemu_irq *cpu_irq;
	qemu_irq *i8259;
	int page_size;

	page_size = getpagesize();
	if (page_size != TARGET_PAGE_SIZE) {
		fprintf(stderr,"Error! Host page size != qemu target page size,"
			" you may need to change TARGET_PAGE_BITS in qemu!\n");
		exit(-1);
	}

	if (ram_size >= 0xc0000000 ) {
		above_4g_mem_size = ram_size - 0xc0000000;
		ram_size = 0xc0000000;
	}

	/* init CPUs */
	for(i = 0; i < smp_cpus; i++) {
		env = cpu_init();
		if (i != 0)
			env->hflags |= HF_HALTED_MASK;
		register_savevm("cpu", i, 4, cpu_save, cpu_load, env);
		qemu_register_reset(main_cpu_reset, env);
	}

	/* allocate RAM */
#ifdef USE_KVM
#ifdef KVM_CAP_USER_MEMORY
	if (kvm_allowed && kvm_qemu_check_extension(KVM_CAP_USER_MEMORY)) {
		ram_addr = qemu_ram_alloc(0xa0000);
		cpu_register_physical_memory(0, 0xa0000, ram_addr);
		kvm_cpu_register_physical_memory(0, 0xa0000, ram_addr);

		ram_addr = qemu_ram_alloc(0x20000); // Workaround 0xa0000-0xc0000

		ram_addr = qemu_ram_alloc(0x40000);
		cpu_register_physical_memory(0xc0000, 0x40000, ram_addr);
		kvm_cpu_register_physical_memory(0xc0000, 0x40000, ram_addr);

		ram_addr = qemu_ram_alloc(ram_size - 0x100000);
		cpu_register_physical_memory(0x100000, ram_size - 0x100000, ram_addr);
		kvm_cpu_register_physical_memory(0x100000, ram_size - 0x100000,
				ram_addr);
	} else
#endif
#endif
	{
		ram_addr = qemu_ram_alloc(ram_size);
		cpu_register_physical_memory(0, ram_size, ram_addr);
	}
	/* allocate VGA RAM */
	vga_ram_addr = qemu_ram_alloc(vga_ram_size);

	/* above 4giga memory allocation */
	if (above_4g_mem_size > 0) {
		ram_addr = qemu_ram_alloc(above_4g_mem_size);
		cpu_register_physical_memory(0x100000000, above_4g_mem_size, ram_addr);
#ifdef USE_KVM
		if (kvm_allowed)
			kvm_cpu_register_physical_memory(0x100000000, above_4g_mem_size,
					ram_addr);
#endif
	}

	/*Load firware to its proper position.*/
#ifdef USE_KVM
	if (kvm_allowed) {
		int r;
		unsigned long  image_size;
		char *image = NULL;
		char *fw_image_start;
		ram_addr_t fw_offset = qemu_ram_alloc(GFW_SIZE);
		char *fw_start = phys_ram_base + fw_offset;

		snprintf(buf, sizeof(buf), "%s/%s", bios_dir, FW_FILENAME);
		image = read_image(buf, &image_size );
		if (NULL == image || !image_size) {
			fprintf(stderr, "Error when reading Guest Firmware!\n");
			return ;
		}
		fw_image_start = fw_start + GFW_SIZE - image_size;

#ifdef KVM_CAP_USER_MEMORY
		r = kvm_qemu_check_extension(KVM_CAP_USER_MEMORY);
		if (r) {
			cpu_register_physical_memory(GFW_START, GFW_SIZE, fw_offset);
			kvm_cpu_register_physical_memory(GFW_START,GFW_SIZE, fw_offset);
			memcpy(fw_image_start, image, image_size);
		}
		else
#endif
		{
			fw_start = kvm_create_phys_mem(kvm_context, (uint32_t)(-image_size),
					image_size, 0, 1);
			if (!fw_start)
				exit(1);
			fw_image_start = fw_start + GFW_SIZE - image_size;
			memcpy(fw_image_start, image, image_size);
		}
		free(image);
		kvm_sync_icache((unsigned long)fw_image_start, image_size);
		kvm_ia64_build_hob(ram_size, smp_cpus, fw_start);
	}
#endif

	cpu_irq = qemu_allocate_irqs(pic_irq_request, first_cpu, 1);
	i8259 = i8259_init(cpu_irq[0]);

	if (pci_enabled) {
		pci_bus = i440fx_init(&i440fx_state, i8259);
		piix3_devfn = piix3_init(pci_bus, -1);
	} else {
		pci_bus = NULL;
	}

	if (cirrus_vga_enabled) {
		if (pci_enabled) {
			pci_cirrus_vga_init(pci_bus,
					ds, phys_ram_base + vga_ram_addr,
					vga_ram_addr, vga_ram_size);
		} else {
			isa_cirrus_vga_init(ds, phys_ram_base + vga_ram_addr,
					vga_ram_addr, vga_ram_size);
		}
	} else {
		if (pci_enabled) {
			pci_vga_init(pci_bus, ds, phys_ram_base + vga_ram_addr,
					vga_ram_addr, vga_ram_size, 0, 0);
		} else {
			isa_vga_init(ds, phys_ram_base + vga_ram_addr,
					vga_ram_addr, vga_ram_size);
		}
	}

	if (pci_enabled) {
		pic_set_alt_irq_func(isa_pic, NULL, NULL);
	}

	for(i = 0; i < MAX_SERIAL_PORTS; i++) {
		if (serial_hds[i]) {
			serial_init(serial_io[i], i8259[serial_irq[i]], serial_hds[i]);
		}
	}

	for(i = 0; i < MAX_PARALLEL_PORTS; i++) {
		if (parallel_hds[i]) {
			parallel_init(parallel_io[i], i8259[parallel_irq[i]],
					parallel_hds[i]);
		}
	}

	for(i = 0; i < nb_nics; i++) {
		nd = &nd_table[i];
		if (!nd->model) {
			if (pci_enabled) {
				nd->model = "ne2k_pci";
			} else {
				nd->model = "ne2k_isa";
			}
		}
		if (strcmp(nd->model, "ne2k_isa") == 0) {
			pc_init_ne2k_isa(nd, i8259);
		} else if (pci_enabled) {
			if (strcmp(nd->model, "?") == 0)
				fprintf(stderr, "qemu: Supported ISA NICs: ne2k_isa\n");
			pci_nic_init(pci_bus, nd, -1);
		} else if (strcmp(nd->model, "?") == 0) {
			fprintf(stderr, "qemu: Supported ISA NICs: ne2k_isa\n");
			exit(1);
		} else {
			fprintf(stderr, "qemu: Unsupported NIC: %s\n", nd->model);
			exit(1);
		}
	}

#undef USE_HYPERCALL  // Disable hypercall now, due to shor of support for VT-i.
#ifdef USE_HYPERCALL
	pci_hypercall_init(pci_bus);
#endif
	if (pci_enabled) {
		pci_piix3_ide_init(pci_bus, bs_table, piix3_devfn + 1, i8259);
	} else {
		for(i = 0; i < 2; i++) {
			isa_ide_init(ide_iobase[i], ide_iobase2[i], i8259[ide_irq[i]],
					bs_table[2 * i], bs_table[2 * i + 1]);
		}
	}

	i8042_init(i8259[1], i8259[12], 0x60);
	DMA_init(0);
#ifdef HAS_AUDIO
	audio_init(pci_enabled ? pci_bus : NULL, i8259);
#endif

	floppy_controller = fdctrl_init(i8259[6], 2, 0, 0x3f0, fd_table);

	/*Disable cmos support for ia64.*/
	//cmos_init(ram_size, above_4g_mem_size, boot_device, bs_table, smp_cpus);

	if (pci_enabled && usb_enabled) {
		usb_uhci_piix3_init(pci_bus, piix3_devfn + 2);
	}

	if (pci_enabled && acpi_enabled) {
		uint8_t *eeprom_buf = qemu_mallocz(8 * 256); /* XXX: make this persistent */
		i2c_bus *smbus;

		/* TODO: Populate SPD eeprom data.  */
		smbus = piix4_pm_init(pci_bus, piix3_devfn + 3, 0xb100);
		for (i = 0; i < 8; i++) {
			smbus_eeprom_device_init(smbus, 0x50 + i, eeprom_buf + (i * 256));
		}
	}

	if (i440fx_state) {
		i440fx_init_memory_mappings(i440fx_state);
	}
#if 0
	/* ??? Need to figure out some way for the user to
	   specify SCSI devices.  */
	if (pci_enabled) {
		void *scsi;
		BlockDriverState *bdrv;

		scsi = lsi_scsi_init(pci_bus, -1);
		bdrv = bdrv_new("scsidisk");
		bdrv_open(bdrv, "scsi_disk.img", 0);
		lsi_scsi_attach(scsi, bdrv, -1);
		bdrv = bdrv_new("scsicd");
		bdrv_open(bdrv, "scsi_cd.iso", 0);
		bdrv_set_type_hint(bdrv, BDRV_TYPE_CDROM);
		lsi_scsi_attach(scsi, bdrv, -1);
	}
#endif
}

static void ipf_init_pci(ram_addr_t ram_size, int vga_ram_size, int boot_device,
		DisplayState *ds, const char **fd_filename,
		int snapshot,
		const char *kernel_filename,
		const char *kernel_cmdline,
		const char *initrd_filename,
		const char *cpu_model)
{
	ipf_init1(ram_size, vga_ram_size, boot_device,
			ds, fd_filename, snapshot,
			kernel_filename, kernel_cmdline,
			initrd_filename, 1);
}

QEMUMachine ipf_machine = {
	"itanium",
	"Itanium Platform",
	ipf_init_pci,
};