// SPDX-License-Identifier: GPL-2.0
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/kvm.h>
#include <linux/kvm_host.h>
#include <linux/bitmap.h>
#include <linux/sched/mm.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/pagewalk.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>
#include <asm/page.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <linux/huge_mm.h>
#ifdef CONFIG_ARM64
#include <asm/pgtable-types.h>
#include <asm/memory.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_arm.h>
#include <asm/stage2_pgtable.h>
#endif
#include "etmem_scan.h"
#include <linux/hugetlb_inline.h>

#ifdef CONFIG_X86_64
/*
 * Fallback to false for kernel doens't support KVM_INVALID_SPTE
 * ept_idle can sitll work in this situation but the scan accuracy may drop,
 * depends on the access frequences of the workload.
 */
#ifdef KVM_INVALID_SPTE
#define KVM_CHECK_INVALID_SPTE(val) ((val) == KVM_INVALID_SPTE)
#else
#define KVM_CHECK_INVALID_SPTE(val) (0)
#endif

# define kvm_arch_mmu_pointer(vcpu) (vcpu->arch.mmu)
# define kvm_mmu_ad_disabled(mmu) (mmu->cpu_role.base.ad_disabled)
#endif /*CONFIG_X86_64*/

#ifdef CONFIG_ARM64
#define if_pmd_thp_or_huge(pmd) (if_pmd_huge(pmd) || pmd_trans_huge(pmd))
#endif /* CONFIG_ARM64  */

#ifdef DEBUG

#define debug_printk trace_printk

#define set_restart_gpa(val, note)	({			\
	unsigned long old_val = pic->restart_gpa;		\
	pic->restart_gpa = (val);				\
	trace_printk("restart_gpa=%lx %luK	%s	%s %d\n",	\
			 (val), (pic->restart_gpa - old_val) >> 10,	\
			 note, __func__, __LINE__);			\
})

#define set_next_hva(val, note)	({				\
	unsigned long old_val = pic->next_hva;			\
	pic->next_hva = (val);					\
	trace_printk("	 next_hva=%lx %luK	%s	%s %d\n",	\
			 (val), (pic->next_hva - old_val) >> 10,	\
			 note, __func__, __LINE__);			\
})

#else

#define debug_printk(...)

#define set_restart_gpa(val, note)	({			\
	pic->restart_gpa = (val);				\
})

#define set_next_hva(val, note)	({				\
	pic->next_hva = (val);					\
})

#endif

#define RET_RESCAN_FLAG 0x10000

/* error return IDLE_PAGE_TYPE_MAX or return valid page type */
enum ProcIdlePageType (*vm_handle_pte_hole)(unsigned long addr,
		unsigned long next, int depth, struct mm_walk *walk) = NULL;
EXPORT_SYMBOL_GPL(vm_handle_pte_hole);

static int set_walk_step(const char *val, const struct kernel_param *kp)
{
	int ret;
	unsigned int n;

	ret = kstrtouint(val, 0, &n);
	if (ret != 0 || n == 0)
		return -EINVAL;

	return param_set_uint(val, kp);
}

static struct kernel_param_ops walk_step_ops = {
	.set = set_walk_step,
	.get = param_get_uint,
};

static unsigned int __read_mostly walk_step = 512; // in PAGE_SIZE
module_param_cb(walk_step, &walk_step_ops, &walk_step, 0644);

static unsigned int resched_step = 10;
module_param(resched_step, uint, 0644);

static unsigned long pagetype_size[16] = {
	[PTE_ACCESSED]	= PAGE_SIZE,	/* 4k page */
	[PMD_ACCESSED]	= PMD_SIZE,	/* 2M page */
	[PUD_PRESENT]	= PUD_SIZE,	/* 1G page */

	[PTE_DIRTY_M]	= PAGE_SIZE,
	[PMD_DIRTY_M]	= PMD_SIZE,

	[PTE_IDLE]	= PAGE_SIZE,
	[PMD_IDLE]	= PMD_SIZE,
	[PMD_IDLE_PTES] = PMD_SIZE,

	[PTE_HOLE]	= PAGE_SIZE,
	[PMD_HOLE]	= PMD_SIZE,
};

static void u64_to_u8(uint64_t n, uint8_t *p)
{
	p += sizeof(uint64_t) - 1;

	*p-- = n; n >>= 8;
	*p-- = n; n >>= 8;
	*p-- = n; n >>= 8;
	*p-- = n; n >>= 8;

	*p-- = n; n >>= 8;
	*p-- = n; n >>= 8;
	*p-- = n; n >>= 8;
	*p	 = n;
}

static void dump_pic(struct page_idle_ctrl *pic)
{
	debug_printk("page_idle_ctrl: pie_read=%d pie_read_max=%d",
			 pic->pie_read,
			 pic->pie_read_max);
	debug_printk(" buf_size=%d bytes_copied=%d next_hva=%pK",
			pic->buf_size,
			pic->bytes_copied,
			pic->next_hva);
	debug_printk(" restart_gpa=%pK pa_to_hva=%pK\n",
			pic->restart_gpa,
			pic->gpa_to_hva);
}

#ifdef CONFIG_ARM64
static int if_pmd_huge(pmd_t pmd)
{
	return pmd_val(pmd) && !(pmd_val(pmd) & PMD_TABLE_BIT);
}

static int if_pud_huge(pud_t pud)
{
#ifndef __PAGETABLE_PMD_FOLDED
	return pud_val(pud) && !(pud_val(pud) & PUD_TABLE_BIT);
#else
	return 0;
#endif
}
#endif

static void pic_report_addr(struct page_idle_ctrl *pic, unsigned long addr)
{
	unsigned long hva;

	pic->kpie[pic->pie_read++] = PIP_CMD_SET_HVA;
	hva = addr;
	u64_to_u8(hva, &pic->kpie[pic->pie_read]);
	pic->pie_read += sizeof(uint64_t);
	dump_pic(pic);
}

static int pic_add_page(struct page_idle_ctrl *pic,
			unsigned long addr,
			unsigned long next,
			enum ProcIdlePageType page_type)
{
	unsigned long page_size = pagetype_size[page_type];

	dump_pic(pic);

	/* align kernel/user vision of cursor position */
	next = round_up(next, page_size);

	if (!pic->pie_read ||
		addr + pic->gpa_to_hva != pic->next_hva) {
		/* merge hole */
		if (page_type == PTE_HOLE ||
			page_type == PMD_HOLE) {
			set_restart_gpa(next, "PTE_HOLE|PMD_HOLE");
			return 0;
		}

		if (addr + pic->gpa_to_hva < pic->next_hva) {
			debug_printk("page_idle: addr moves backwards\n");
			WARN_ONCE(1, "page_idle: addr moves backwards");
		}

		if (pic->pie_read + sizeof(uint64_t) + 2 >= pic->pie_read_max) {
			set_restart_gpa(addr, "PAGE_IDLE_KBUF_FULL");
			return PAGE_IDLE_KBUF_FULL;
		}

		pic_report_addr(pic, round_down(addr, page_size) +
							pic->gpa_to_hva);
	} else {
		if (PIP_TYPE(pic->kpie[pic->pie_read - 1]) == page_type &&
			PIP_SIZE(pic->kpie[pic->pie_read - 1]) < 0xF) {
			set_next_hva(next + pic->gpa_to_hva, "IN-PLACE INC");
			set_restart_gpa(next, "IN-PLACE INC");
			pic->kpie[pic->pie_read - 1]++;
			WARN_ONCE(page_size < next-addr, "next-addr too large");
			return 0;
		}
		if (pic->pie_read >= pic->pie_read_max) {
			set_restart_gpa(addr, "PAGE_IDLE_KBUF_FULL");
			return PAGE_IDLE_KBUF_FULL;
		}
	}

	set_next_hva(next + pic->gpa_to_hva, "NEW-ITEM");
	set_restart_gpa(next, "NEW-ITEM");
	pic->kpie[pic->pie_read] = PIP_COMPOSE(page_type, 1);
	pic->pie_read++;

	return 0;
}

static int init_page_idle_ctrl_buffer(struct page_idle_ctrl *pic)
{
	pic->pie_read = 0;
	pic->pie_read_max = min(PAGE_IDLE_KBUF_SIZE,
				pic->buf_size - pic->bytes_copied);
	/* reserve space for PIP_CMD_SET_HVA in the end */
	pic->pie_read_max -= sizeof(uint64_t) + 1;

	/*
	 * Align with PAGE_IDLE_KBUF_FULL
	 * logic in pic_add_page(), to avoid pic->pie_read = 0 when
	 * PAGE_IDLE_KBUF_FULL happened.
	 */
	if (pic->pie_read_max <= sizeof(uint64_t) + 2)
		return PAGE_IDLE_KBUF_FULL;

	memset(pic->kpie, 0, sizeof(pic->kpie));
	return 0;
}

static void setup_page_idle_ctrl(struct page_idle_ctrl *pic, void *buf,
				int buf_size, unsigned int flags)
{
	pic->buf = buf;
	pic->buf_size = buf_size;
	pic->bytes_copied = 0;
	pic->next_hva = 0;
	pic->gpa_to_hva = 0;
	pic->restart_gpa = 0;
	pic->last_va = 0;
	pic->flags = flags;
}

static int page_idle_copy_user(struct page_idle_ctrl *pic,
				unsigned long start, unsigned long end)
{
	int bytes_read;
	int ret;

	dump_pic(pic);

	bytes_read = pic->pie_read;
	if (!bytes_read)
		return 0;

	ret = copy_to_user(pic->buf, pic->kpie, bytes_read);
	if (ret)
		return -EFAULT;

	pic->buf += bytes_read;
	pic->bytes_copied += bytes_read;
	if (pic->bytes_copied >= pic->buf_size)
		return PAGE_IDLE_BUF_FULL;

	ret = init_page_idle_ctrl_buffer(pic);
	if (ret)
		return ret;

	cond_resched();
	return 0;
}

#ifdef CONFIG_X86_64
static int vm_walk_host_range(unsigned long long start,
		unsigned long end,
		struct mm_walk *walk)
{
	int ret;
	struct page_idle_ctrl *pic = walk->private;
	unsigned long tmp_gpa_to_hva = pic->gpa_to_hva;

	pic->gpa_to_hva = 0;
	read_unlock(&pic->kvm->mmu_lock);
	mmap_read_lock(walk->mm);
	local_irq_disable();
	ret = walk_page_range(walk->mm, start + tmp_gpa_to_hva, end + tmp_gpa_to_hva,
			walk->ops, walk->private);
	local_irq_enable();
	mmap_read_unlock(walk->mm);
	pic->gpa_to_hva = tmp_gpa_to_hva;
	if (pic->flags & VM_SCAN_HOST) {
		pic->restart_gpa -= tmp_gpa_to_hva;
		pic->flags &= ~VM_SCAN_HOST;
	}
	if (ret != PAGE_IDLE_KBUF_FULL && end > pic->restart_gpa)
		pic->restart_gpa = end;

	/* ept page table may change after spin_unlock, rescan vm from root ept */
	ret |= RET_RESCAN_FLAG;

	return ret;
}

static int ept_pte_range(struct page_idle_ctrl *pic,
			 pmd_t *pmd, unsigned long addr, unsigned long end,
			 struct mm_walk *walk)
{
	pte_t *pte;
	enum ProcIdlePageType page_type;
	int err = 0;

	pte = pte_offset_kernel(pmd, addr);
	do {
		if (KVM_CHECK_INVALID_SPTE(pte->pte)) {
			page_type = PTE_IDLE;
		} else if (!ept_pte_present(*pte)) {
			err = vm_walk_host_range(addr, end, walk);
			goto next;
		} else if (!test_and_clear_bit(_PAGE_BIT_EPT_ACCESSED,
						 (unsigned long *) &pte->pte))
			page_type = PTE_IDLE;
		else {
			page_type = PTE_ACCESSED;
			if (pic->flags & SCAN_DIRTY_PAGE) {
				if (test_and_clear_bit(_PAGE_BIT_EPT_DIRTY,
						(unsigned long *) &pte->pte))
					page_type = PTE_DIRTY_M;
			}
		}

		err = pic_add_page(pic, addr, addr + PAGE_SIZE, page_type);
next:
		if (err)
			break;
	} while (pte++, addr += PAGE_SIZE, addr != end);

	return err;
}

static enum ProcIdlePageType ept_huge_accessed(pmd_t *pmd, unsigned long addr,
		unsigned long end)
{
	int accessed = PMD_IDLE;
	pte_t *pte;

	pte = pte_offset_kernel(pmd, addr);
	do {
		if (!KVM_CHECK_INVALID_SPTE(pte->pte))
			continue;
		if (!ept_pte_present(*pte))
			continue;
		if (!test_and_clear_bit(_PAGE_BIT_EPT_ACCESSED,
					(unsigned long *)&pte->pte))
			continue;
		accessed = PMD_ACCESSED;
	} while (pte++, addr += PAGE_SIZE, addr != end);

	return accessed;
}

static int ept_pmd_range(struct page_idle_ctrl *pic,
			 pud_t *pud, unsigned long addr, unsigned long end,
			 struct mm_walk *walk)
{
	pmd_t *pmd;
	unsigned long next;
	enum ProcIdlePageType page_type;
	enum ProcIdlePageType pte_page_type;
	int err = 0;

	if (pic->flags & SCAN_HUGE_PAGE)
		pte_page_type = PMD_IDLE_PTES;
	else
		pte_page_type = IDLE_PAGE_TYPE_MAX;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (KVM_CHECK_INVALID_SPTE(pmd->pmd))
			page_type = PMD_IDLE;
		else if (!ept_pmd_present(*pmd)) {
			err = vm_walk_host_range(addr, next, walk);
			goto next;
		} else if (!pmd_large(*pmd)) {
			if (pic->flags & SCAN_AS_HUGE)
				page_type = ept_huge_accessed(pmd, addr, next);
			else
				page_type = pte_page_type;
		} else if (!test_and_clear_bit(_PAGE_BIT_EPT_ACCESSED,
						(unsigned long *)pmd))
			page_type = PMD_IDLE;
		else {
			page_type = PMD_ACCESSED;
			if ((pic->flags & SCAN_DIRTY_PAGE) &&
				test_and_clear_bit(_PAGE_BIT_EPT_DIRTY,
					(unsigned long *) pmd))
				page_type = PMD_DIRTY_M;
		}

		if (page_type != IDLE_PAGE_TYPE_MAX)
			err = pic_add_page(pic, addr, next, page_type);
		else
			err = ept_pte_range(pic, pmd, addr, next, walk);

next:
		if (err)
			break;
	} while (pmd++, addr = next, addr != end);

	return err;
}


static int ept_pud_range(struct page_idle_ctrl *pic,
			 p4d_t *p4d, unsigned long addr, unsigned long end,
			 struct mm_walk *walk)
{
	pud_t *pud;
	unsigned long next;
	int err = 0;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);

		if (!ept_pud_present(*pud)) {
			err = vm_walk_host_range(addr, next, walk);
			goto next;
		}

		if (pud_large(*pud))
			err = pic_add_page(pic, addr, next, PUD_PRESENT);
		else
			err = ept_pmd_range(pic, pud, addr, next, walk);

next:
		if (err)
			break;
	} while (pud++, addr = next, addr != end);

	return err;
}

static int ept_p4d_range(struct page_idle_ctrl *pic,
			 p4d_t *p4d, unsigned long addr, unsigned long end,
			 struct mm_walk *walk)
{
	unsigned long next;
	int err = 0;

	p4d += p4d_index(addr);
	do {
		next = p4d_addr_end(addr, end);
		if (!ept_p4d_present(*p4d)) {
			set_restart_gpa(next, "P4D_HOLE");
			continue;
		}

		err = ept_pud_range(pic, p4d, addr, next, walk);
		if (err)
			break;
	} while (p4d++, addr = next, addr != end);

	return err;
}

static int ept_pgd_range(struct page_idle_ctrl *pic,
		pgd_t *pgd,
		unsigned long addr,
		unsigned long end,
		struct mm_walk *walk)
{
	p4d_t *p4d;
	unsigned long next;
	int err = 0;

	pgd = pgd_offset_pgd(pgd, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (!ept_pgd_present(*pgd)) {
			set_restart_gpa(next, "PGD_HOLE");
			continue;
		}

		p4d = (p4d_t *)pgd_page_vaddr(*pgd);
		err = ept_p4d_range(pic, p4d, addr, next, walk);
		if (err)
			break;
	} while (pgd++, addr = next, addr != end);

	return err;
}

static int ept_page_range(struct page_idle_ctrl *pic,
			  unsigned long addr,
			  unsigned long end,
			  struct mm_walk *walk)
{
	struct kvm_vcpu *vcpu;
	struct kvm_mmu *mmu;
	uint64_t *ept_root;
	int err = 0;

	WARN_ON(addr >= end);

	read_lock(&pic->kvm->mmu_lock);

	vcpu = kvm_get_vcpu(pic->kvm, 0);
	if (!vcpu) {
		pic->gpa_to_hva = 0;
		set_restart_gpa(TASK_SIZE, "NO-VCPU");
		read_unlock(&pic->kvm->mmu_lock);
		return -EINVAL;
	}

	mmu = kvm_arch_mmu_pointer(vcpu);
	if (!VALID_PAGE(mmu->root.hpa)) {
		pic->gpa_to_hva = 0;
		set_restart_gpa(TASK_SIZE, "NO-HPA");
		read_unlock(&pic->kvm->mmu_lock);
		return -EINVAL;
	}

	ept_root = __va(mmu->root.hpa);

	/* Walk start at p4d when vm has 4 level table pages */
	if (mmu->root_role.level != 4)
		err = ept_pgd_range(pic, (pgd_t *)ept_root, addr, end, walk);
	else
		err = ept_p4d_range(pic, (p4d_t *)ept_root, addr, end, walk);

	/* mmu_lock is unlock in vm_walk_host_range which will unlock mmu_lock
	 * and RET_RESCAN_FLAG will be set in ret value
	 */
	if (!(err & RET_RESCAN_FLAG))
		read_unlock(&pic->kvm->mmu_lock);
	else
		err &= ~RET_RESCAN_FLAG;

	return err;
}

static int ept_idle_supports_cpu(struct kvm *kvm)
{
		struct kvm_vcpu *vcpu;
		struct kvm_mmu *mmu;
		int ret;

		vcpu = kvm_get_vcpu(kvm, 0);
		if (!vcpu)
			return -EINVAL;

		read_lock(&kvm->mmu_lock);
		mmu = kvm_arch_mmu_pointer(vcpu);
		if (kvm_mmu_ad_disabled(mmu)) {
			pr_notice("CPU does not support EPT A/D bits tracking\n");
			ret = -EINVAL;
		} else if (mmu->root_role.level < 4 ||
				(mmu->root_role.level == 5 && !pgtable_l5_enabled())) {
			pr_notice("Unsupported EPT level %d\n", mmu->root_role.level);
			ret = -EINVAL;
		} else
			ret = 0;
		read_unlock(&kvm->mmu_lock);

		return ret;
}

#else
static inline phys_addr_t stage2_range_addr_end(phys_addr_t addr, phys_addr_t end)
{
	phys_addr_t size = kvm_granule_size(KVM_PGTABLE_MIN_BLOCK_LEVEL);
	phys_addr_t boundary = ALIGN_DOWN(addr + size, size);

	return (boundary - 1 < end - 1) ? boundary : end;
}

static int arm_pte_range(struct page_idle_ctrl *pic,
			pmd_t *pmd, unsigned long addr, unsigned long end)
{
	pte_t *pte;
	enum ProcIdlePageType page_type;
	int err = 0;

	pte = pte_offset_kernel(pmd, addr);
	do {
		if (!pte_present(*pte))
			page_type = PTE_HOLE;
		else if (!test_and_clear_bit(_PAGE_MM_BIT_ACCESSED,
					(unsigned long *) &pte->pte))
			page_type = PTE_IDLE;
		else
			page_type = PTE_ACCESSED;

		err = pic_add_page(pic, addr, addr + PAGE_SIZE, page_type);
		if (err)
			break;
	} while (pte++, addr += PAGE_SIZE, addr != end);

	return err;
}

static int arm_pmd_range(struct page_idle_ctrl *pic,
			pud_t *pud, unsigned long addr, unsigned long end)
{
	pmd_t *pmd;
	unsigned long next;
	enum ProcIdlePageType page_type;
	enum ProcIdlePageType pte_page_type;
	int err = 0;

	if (pic->flags & SCAN_HUGE_PAGE)
		pte_page_type = PMD_IDLE_PTES;
	else
		pte_page_type = IDLE_PAGE_TYPE_MAX;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (!pmd_present(*pmd))
			page_type = PMD_HOLE;
		else if (!if_pmd_thp_or_huge(*pmd))
			page_type = pte_page_type;
		else if (!test_and_clear_bit(_PAGE_MM_BIT_ACCESSED,
					(unsigned long *)pmd))
			page_type = PMD_IDLE;
		else
			page_type = PMD_ACCESSED;

		if (page_type != IDLE_PAGE_TYPE_MAX)
			err = pic_add_page(pic, addr, next, page_type);
		else
			err = arm_pte_range(pic, pmd, addr, next);
		if (err)
			break;
	} while (pmd++, addr = next, addr != end);

	return err;
}

static int arm_pud_range(struct page_idle_ctrl *pic,
			p4d_t *p4d, unsigned long addr, unsigned long end)
{
	pud_t *pud;
	unsigned long next;
	int err = 0;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (!pud_present(*pud)) {
			set_restart_gpa(next, "PUD_HOLE");
			continue;
		}

		if (if_pud_huge(*pud))
			err = pic_add_page(pic, addr, next, PUD_PRESENT);
		else
			err = arm_pmd_range(pic, pud, addr, next);
		if (err)
			break;
	} while (pud++, addr = next, addr != end);

	return err;
}

static int arm_p4d_range(struct page_idle_ctrl *pic,
			pgd_t *pgd, unsigned long addr, unsigned long end)
{
	p4d_t *p4d;
	unsigned long next;
	int err = 0;

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (!p4d_present(*p4d)) {
			set_restart_gpa(next, "P4D_HOLE");
			continue;
		}

		err = arm_pud_range(pic, p4d, addr, next);
		if (err)
			break;
	} while (p4d++, addr = next, addr != end);

	return err;
}

static int arm_page_range(struct page_idle_ctrl *pic,
						   unsigned long addr,
						   unsigned long end)
{
	pgd_t *pgd;
	unsigned long next;
	struct kvm *kvm = pic->kvm;
	int err = 0;

	WARN_ON(addr >= end);

	read_lock(&pic->kvm->mmu_lock);
	pgd = (pgd_t *)kvm->arch.mmu.pgt->pgd + pgd_index(addr);
	read_unlock(&pic->kvm->mmu_lock);

	local_irq_disable();
	do {
		next = stage2_range_addr_end(addr, end);
		if (!pgd_present(*pgd)) {
			set_restart_gpa(next, "PGD_HOLE");
			continue;
		}

		err = arm_p4d_range(pic, pgd, addr, next);
		if (err)
			break;
	} while (pgd++, addr = next, addr != end);

	local_irq_enable();
	return err;
}
#endif

/*
 * Depending on whether hva falls in a memslot:
 *
 * 1) found => return gpa and remaining memslot size in *addr_range
 *
 *				   |<----- addr_range --------->|
 *				   [   mem slot	              ]
 *				   ^hva
 *
 * 2) not found => return hole size in *addr_range
 *
 *				   |<----- addr_range --------->|
 *				   [first mem slot above hva  ]
 *				   ^hva
 *
 * If hva is above all mem slots, *addr_range will be ~0UL.
 * We can finish read(2).
 */
static unsigned long vm_idle_find_gpa(struct page_idle_ctrl *pic,
					   unsigned long hva,
					   unsigned long *addr_range)
{
	struct kvm *kvm = pic->kvm;
	struct kvm_memslots *slots;
	struct kvm_memory_slot *memslot;
	unsigned long hva_end;
	gfn_t gfn;
	int bkt;

	*addr_range = ~0UL;
	mutex_lock(&kvm->slots_lock);
	slots = kvm_memslots(pic->kvm);
	kvm_for_each_memslot(memslot, bkt, slots) {
		hva_end = memslot->userspace_addr +
			(memslot->npages << PAGE_SHIFT);

		if (hva >= memslot->userspace_addr && hva < hva_end) {
			gpa_t gpa;

			gfn = hva_to_gfn_memslot(hva, memslot);
			*addr_range = hva_end - hva;
			gpa = gfn_to_gpa(gfn);
			mutex_unlock(&kvm->slots_lock);
			return gpa;
		}

		if (memslot->userspace_addr > hva)
			*addr_range = min(*addr_range,
					  memslot->userspace_addr - hva);
	}
	mutex_unlock(&kvm->slots_lock);
	return INVALID_PAGE;
}

static inline unsigned long mask_to_size(unsigned long mask)
{
	return ~mask + 1;
}

static int mm_idle_hugetlb_entry(pte_t *pte, unsigned long hmask,
		unsigned long addr, unsigned long next,
		struct mm_walk *walk);
static int vm_idle_hugetlb_entry(pte_t *pte, unsigned long hmask,
		unsigned long addr, unsigned long next,
		struct mm_walk *walk)
{
	struct page_idle_ctrl *pic = walk->private;
	enum ProcIdlePageType page_type;

	pic->flags |= VM_SCAN_HOST;

	/* hugetlb page table entry of vm maybe not present while page is resident
	 * in address_space
	 */
	if (mask_to_size(hmask) != PUD_SIZE && !pte_present(*pte) &&
			vm_handle_pte_hole != NULL) {
		page_type = vm_handle_pte_hole(addr, next, -1, walk);
		if (page_type < IDLE_PAGE_TYPE_MAX)
			return pic_add_page(pic, addr, next, page_type);
	}

	return mm_idle_hugetlb_entry(pte, hmask, addr, next, walk);
}

static int vm_idle_pte_hole(unsigned long addr, unsigned long next, int depth, struct mm_walk *walk)
{
	struct page_idle_ctrl *pic = walk->private;
	enum ProcIdlePageType pagetype;

	if (vm_handle_pte_hole == NULL)
		return 0;

	pagetype = vm_handle_pte_hole(addr, next, depth, walk);
	if (pagetype >= IDLE_PAGE_TYPE_MAX)
		return 0;

	debug_printk("scan pte hole addr %pK type %d\n", addr, pagetype);
	pic->flags |= VM_SCAN_HOST;
	return pic_add_page(pic, addr, next, pagetype);
}

static int mm_idle_pmd_entry(pmd_t *pmd, unsigned long addr,
		unsigned long next, struct mm_walk *walk);
static int vm_idle_pmd_entry(pmd_t *pmd, unsigned long addr,
		unsigned long next, struct mm_walk *walk)
{
	struct page_idle_ctrl *pic = walk->private;

	pic->flags |= VM_SCAN_HOST;
	return mm_idle_pmd_entry(pmd, addr, next, walk);
}

static int mm_idle_pud_entry(pud_t *pud, unsigned long addr,
		unsigned long next, struct mm_walk *walk);
static int vm_idle_pud_entry(pud_t *pud, unsigned long addr,
		unsigned long next, struct mm_walk *walk)
{
	struct page_idle_ctrl *pic = walk->private;

	pic->flags |= VM_SCAN_HOST;
	return mm_idle_pud_entry(pud, addr, next, walk);
}

static int vm_idle_walk_hva_range(struct page_idle_ctrl *pic,
				   unsigned long start, unsigned long end,
				   struct mm_walk *walk)
{
	unsigned long gpa_addr;
	unsigned long gpa_next;
	unsigned long gpa_end;
	unsigned long addr_range;
	unsigned long va_end;
	int ret;
	int steps;

#ifdef CONFIG_X86_64
	ret = ept_idle_supports_cpu(pic->kvm);
	if (ret)
		return ret;
#endif

	ret = init_page_idle_ctrl_buffer(pic);
	if (ret)
		return ret;

	for (; start < end;) {
		gpa_addr = vm_idle_find_gpa(pic, start, &addr_range);

		if (gpa_addr == INVALID_PAGE) {
			pic->gpa_to_hva = 0;
			if (addr_range == ~0UL) {
				set_restart_gpa(TASK_SIZE, "EOF");
				va_end = end;
			} else {
				start += addr_range;
				set_restart_gpa(start, "OUT-OF-SLOT");
				va_end = start;
			}
		} else {
			pic->gpa_to_hva = start - gpa_addr;
			gpa_end = gpa_addr + addr_range;
			steps = 0;
			for (; gpa_addr < gpa_end;) {
				gpa_next = min(gpa_end, gpa_addr + walk_step * PAGE_SIZE);
#ifdef CONFIG_ARM64
				ret = arm_page_range(pic, gpa_addr, gpa_next);
#else
				ret = ept_page_range(pic, gpa_addr, gpa_next, walk);
#endif
				gpa_addr = pic->restart_gpa;

				if (ret)
					break;

				if (++steps >= resched_step) {
					cond_resched();
					steps = 0;
				}
			}
			va_end = pic->gpa_to_hva + gpa_end;
		}

		start = pic->restart_gpa + pic->gpa_to_hva;
		ret = page_idle_copy_user(pic, start, va_end);
		if (ret)
			break;
	}

	if (start > pic->next_hva)
		set_next_hva(start, "NEXT-START");

	if (pic->bytes_copied)
		ret = 0;
	return ret;
}

static int mm_idle_test_walk(unsigned long start, unsigned long end,
				 struct mm_walk *walk);
static ssize_t vm_idle_read(struct file *file, char *buf,
				 size_t count, loff_t *ppos)
{
	struct mm_struct *mm = file->private_data;
	struct mm_walk mm_walk = {};
	struct mm_walk_ops mm_walk_ops = {};
	struct page_idle_ctrl *pic;
	unsigned long hva_start = *ppos;
	unsigned long hva_end = hva_start + (count << (3 + PAGE_SHIFT));
	int ret;

	pic = kzalloc(sizeof(*pic), GFP_KERNEL);
	if (!pic)
		return -ENOMEM;

	setup_page_idle_ctrl(pic, buf, count, file->f_flags);
	pic->kvm = mm_kvm(mm);

	mm_walk_ops.pmd_entry = vm_idle_pmd_entry;
	mm_walk_ops.pud_entry = vm_idle_pud_entry;
	mm_walk_ops.hugetlb_entry = vm_idle_hugetlb_entry;
	mm_walk_ops.pte_hole = vm_idle_pte_hole;
	mm_walk_ops.test_walk = mm_idle_test_walk;

	mm_walk.mm = mm;
	mm_walk.ops = &mm_walk_ops;
	mm_walk.private = pic;

	ret = vm_idle_walk_hva_range(pic, hva_start, hva_end, &mm_walk);
	if (ret)
		goto out_kvm;

	ret = pic->bytes_copied;
	*ppos = pic->next_hva;
out_kvm:
	kfree(pic);
	return ret;

}

static ssize_t mm_idle_read(struct file *file, char *buf,
				size_t count, loff_t *ppos);

static ssize_t page_scan_read(struct file *file, char *buf,
				 size_t count, loff_t *ppos)
{
	struct mm_struct *mm = file->private_data;
	unsigned long hva_start = *ppos;
	unsigned long hva_end = hva_start + (count << (3 + PAGE_SHIFT));

	if ((hva_start >= TASK_SIZE) || (hva_end >= TASK_SIZE)) {
		debug_printk("page_idle_read past TASK_SIZE: %pK %pK %lx\n",
			hva_start, hva_end, TASK_SIZE);
		return 0;
	}
	if (hva_end <= hva_start) {
		debug_printk("page_idle_read past EOF: %pK %pK\n",
					hva_start, hva_end);
		return 0;
	}
	if (*ppos & (PAGE_SIZE - 1)) {
		debug_printk("page_idle_read unaligned ppos: %pK\n",
					hva_start);
		return -EINVAL;
	}
	if (count < PAGE_IDLE_BUF_MIN) {
		debug_printk("page_idle_read small count: %lx\n",
					(unsigned long)count);
		return -EINVAL;
	}

	if (!mm_kvm(mm))
		return mm_idle_read(file, buf, count, ppos);

	return vm_idle_read(file, buf, count, ppos);
}

static int page_scan_open(struct inode *inode, struct file *file)
{
	if (!try_module_get(THIS_MODULE))
		return -EBUSY;

	return 0;
}

static int page_scan_release(struct inode *inode, struct file *file)
{
	struct mm_struct *mm = file->private_data;
	struct kvm *kvm;
	int ret = 0;

	if (!mm) {
		ret = -EBADF;
		goto out;
	}

	kvm = mm_kvm(mm);
	if (!kvm) {
		ret = -EINVAL;
		goto out;
	}
#ifdef CONFIG_X86_64
	write_lock(&kvm->mmu_lock);
	kvm_flush_remote_tlbs(kvm);
	write_unlock(&kvm->mmu_lock);
#endif

out:
	module_put(THIS_MODULE);
	return ret;
}

static int mm_idle_pmd_large(pmd_t pmd)
{
#ifdef CONFIG_ARM64
	return if_pmd_thp_or_huge(pmd);
#else
	return pmd_large(pmd);
#endif
}

static int mm_idle_pte_range(struct page_idle_ctrl *pic, pmd_t *pmd,
				 unsigned long addr, unsigned long next)
{
	enum ProcIdlePageType page_type;
	pte_t *pte;
	int err = 0;

	pte = pte_offset_kernel(pmd, addr);
	do {
		if (!pte_present(*pte))
			page_type = PTE_HOLE;
		else if (pic->flags & SCAN_IGN_HOST)
			page_type = PTE_IDLE;
		else if (!test_and_clear_bit(_PAGE_MM_BIT_ACCESSED,
						 (unsigned long *) &pte->pte))
			page_type = PTE_IDLE;
		else {
			page_type = PTE_ACCESSED;
		}

		err = pic_add_page(pic, addr, addr + PAGE_SIZE, page_type);
		if (err)
			break;
	} while (pte++, addr += PAGE_SIZE, addr != next);

	return err;
}

static int mm_idle_hugetlb_entry(pte_t *pte, unsigned long hmask,
				unsigned long addr, unsigned long next,
				struct mm_walk *walk)
{
	struct page_idle_ctrl *pic = walk->private;
	enum ProcIdlePageType page_type;
	unsigned long start = addr & hmask; /* hugepage may be splited in vm */
	int ret;

	if (mask_to_size(hmask) == PUD_SIZE) {
		page_type = PUD_PRESENT;
		goto add_page;
	}

	if (!pte_present(*pte))
		page_type = PMD_HOLE;
	else if (pic->flags & SCAN_IGN_HOST)
		page_type = PMD_IDLE;
	else if (!test_and_clear_bit(_PAGE_MM_BIT_ACCESSED, (unsigned long *)pte))
		page_type = PMD_IDLE;
	else
		page_type = PMD_ACCESSED;

add_page:
	ret = pic_add_page(pic, start, start + pagetype_size[page_type], page_type);
	return ret;
}

static int mm_idle_pmd_entry(pmd_t *pmd, unsigned long addr,
				 unsigned long next, struct mm_walk *walk)
{
	struct page_idle_ctrl *pic = walk->private;
	enum ProcIdlePageType page_type;
	enum ProcIdlePageType pte_page_type;
	int err;

	/*
	 * Skip duplicate PMD_IDLE_PTES: when the PMD crosses VMA boundary,
	 * walk_page_range() can call on the same PMD twice.
	 */
	if ((addr & PMD_MASK) == (pic->last_va & PMD_MASK) && (pic->flags & SCAN_HUGE_PAGE)) {
		debug_printk("ignore duplicate addr %pK %pK\n",
				 addr, pic->last_va);
		set_restart_gpa(round_up(next, PMD_SIZE), "DUP_ADDR");
		return 0;
	}
	pic->last_va = addr;

	if (pic->flags & SCAN_HUGE_PAGE)
		pte_page_type = PMD_IDLE_PTES;
	else
		pte_page_type = IDLE_PAGE_TYPE_MAX;

	if (!pmd_present(*pmd))
		page_type = PMD_HOLE;
	else if (!mm_idle_pmd_large(*pmd))
		page_type = pte_page_type;
	else if (!test_and_clear_bit(_PAGE_MM_BIT_ACCESSED,
				(unsigned long *)pmd) ||
			pic->flags & SCAN_IGN_HOST)
		page_type = PMD_IDLE;
	else
		page_type = PMD_ACCESSED;

	if (page_type != IDLE_PAGE_TYPE_MAX)
		err = pic_add_page(pic, addr, next, page_type);
	else
		err = mm_idle_pte_range(pic, pmd, addr, next);

	return err;
}

static int mm_idle_pud_entry(pud_t *pud, unsigned long addr,
				 unsigned long next, struct mm_walk *walk)
{
	struct page_idle_ctrl *pic = walk->private;

	spinlock_t *ptl = pud_trans_huge_lock(pud, walk->vma);

	if (ptl) {
		if ((addr & PUD_MASK) != (pic->last_va & PUD_MASK)) {
			pic_add_page(pic, addr, next, PUD_PRESENT);
			pic->last_va = addr;
		}
		spin_unlock(ptl);
		return 1;
	}

	return 0;
}

static int mm_idle_test_walk(unsigned long start, unsigned long end,
				 struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;
	struct page_idle_ctrl *pic = walk->private;

	/* If the specified page swapout is set, the untagged vma is skipped. */
	if ((pic->flags & VMA_SCAN_FLAG) && !(vma->vm_flags & VM_SWAPFLAG))
		return 1;

	if (vma->vm_file) {
		if (is_vm_hugetlb_page(vma))
			return 0;
		if ((vma->vm_flags & (VM_WRITE|VM_MAYSHARE)) == VM_WRITE)
			return 0;
		return 1;
	}

	return 0;
}

static int mm_idle_walk_range(struct page_idle_ctrl *pic,
				  unsigned long start,
				  unsigned long end,
				  struct mm_walk *walk)
{
	struct vm_area_struct *vma;
	int ret = 0;

	ret = init_page_idle_ctrl_buffer(pic);
	if (ret)
		return ret;

	for (; start < end;) {
		mmap_read_lock(walk->mm);
		vma = find_vma(walk->mm, start);
		if (vma) {
			if (end > vma->vm_start) {
				local_irq_disable();
				ret = walk_page_range(walk->mm, start, end,
						walk->ops, walk->private);
				local_irq_enable();
			} else
				set_restart_gpa(vma->vm_start, "VMA-HOLE");
		} else
			set_restart_gpa(TASK_SIZE, "EOF");
		mmap_read_unlock(walk->mm);
		WARN_ONCE(pic->gpa_to_hva, "non-zero gpa_to_hva");
		if (ret != PAGE_IDLE_KBUF_FULL && end > pic->restart_gpa)
			pic->restart_gpa = end;
		start = pic->restart_gpa;
		ret = page_idle_copy_user(pic, start, end);
		if (ret)
			break;
	}

	if (start > pic->next_hva)
		set_next_hva(start, "NEXT-START");

	if (pic->bytes_copied) {
		if (ret != PAGE_IDLE_BUF_FULL && pic->next_hva < end)
			debug_printk("partial scan: next_hva=%pK end=%pK\n",
					 pic->next_hva, end);
		ret = 0;
	} else
		debug_printk("nothing read");
	return ret;
}

static ssize_t mm_idle_read(struct file *file, char *buf,
				size_t count, loff_t *ppos)
{
	struct mm_struct *mm = file->private_data;
	struct mm_walk_ops *mm_walk_ops = NULL;
	struct mm_walk mm_walk = {};
	struct page_idle_ctrl *pic;
	unsigned long va_start = *ppos;
	unsigned long va_end = va_start + (count << (3 + PAGE_SHIFT));
	int ret;

	if (va_end <= va_start) {
		debug_printk("%s past EOF: %pK %pK\n",
				__func__, va_start, va_end);
		return 0;
	}
	if (*ppos & (PAGE_SIZE - 1)) {
		debug_printk("%s unaligned ppos: %pK\n",
				__func__, va_start);
		return -EINVAL;
	}
	if (count < PAGE_IDLE_BUF_MIN) {
		debug_printk("%s small count: %lx\n",
				__func__, (unsigned long)count);
		return -EINVAL;
	}

	pic = kzalloc(sizeof(*pic), GFP_KERNEL);
	if (!pic)
		return -ENOMEM;

	mm_walk_ops = kzalloc(sizeof(struct mm_walk_ops), GFP_KERNEL);
	if (!mm_walk_ops) {
		kfree(pic);
		return -ENOMEM;
	}

	setup_page_idle_ctrl(pic, buf, count, file->f_flags);

	mm_walk_ops->pmd_entry = mm_idle_pmd_entry;
	mm_walk_ops->pud_entry = mm_idle_pud_entry;
	mm_walk_ops->hugetlb_entry = mm_idle_hugetlb_entry;
	mm_walk_ops->test_walk = mm_idle_test_walk;

	mm_walk.mm = mm;
	mm_walk.ops = mm_walk_ops;
	mm_walk.private = pic;
	mm_walk.pgd = NULL;
	mm_walk.no_vma = false;
	ret = mm_idle_walk_range(pic, va_start, va_end, &mm_walk);
	if (ret)
		goto out_free;

	ret = pic->bytes_copied;
	*ppos = pic->next_hva;
out_free:
	kfree(pic);
	kfree(mm_walk_ops);
	return ret;
}

static long page_scan_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	unsigned int flags;

	if (get_user(flags, (unsigned int __user *)argp))
		return -EFAULT;
	flags &= ALL_SCAN_FLAGS;

	switch (cmd) {
	case IDLE_SCAN_ADD_FLAGS:
		filp->f_flags |= flags;
		break;
	case IDLE_SCAN_REMOVE_FLAGS:
		filp->f_flags &= ~flags;
		break;
	case VMA_SCAN_ADD_FLAGS:
		filp->f_flags |= flags;
		break;
	case VMA_SCAN_REMOVE_FLAGS:
		filp->f_flags &= ~flags;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

extern struct file_operations proc_page_scan_operations;

static int page_scan_entry(void)
{
	proc_page_scan_operations.flock(NULL, 1, NULL);
	proc_page_scan_operations.owner = THIS_MODULE;
	proc_page_scan_operations.read = page_scan_read;
	proc_page_scan_operations.open = page_scan_open;
	proc_page_scan_operations.release = page_scan_release;
	proc_page_scan_operations.unlocked_ioctl = page_scan_ioctl;
	proc_page_scan_operations.flock(NULL, 0, NULL);

	return 0;
}

static void page_scan_exit(void)
{
	proc_page_scan_operations.flock(NULL, 1, NULL);
	proc_page_scan_operations.owner = NULL;
	proc_page_scan_operations.read = NULL;
	proc_page_scan_operations.open = NULL;
	proc_page_scan_operations.release = NULL;
	proc_page_scan_operations.unlocked_ioctl = NULL;
	proc_page_scan_operations.flock(NULL, 0, NULL);
}

MODULE_LICENSE("GPL");
module_init(page_scan_entry);
module_exit(page_scan_exit);
