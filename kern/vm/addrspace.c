/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
/* ADDED():*/
#include <elf.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	/*
	 * Initialize as needed.
	 */

	// initialize the region
	as->first = NULL; 
	
	// initialize the page directory
	paddr_t **page_dir = kmalloc(NUM_PD_ENTRY * sizeof(paddr_t *)); 
	if(page_dir == NULL) {
		kfree(as);
		return NULL;
	}
	as->pagetable = page_dir;
	for (int i = 0; i < NUM_PT_ENTRY; i++) {
		page_dir[i] = NULL;
	}
	
	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

	/*
	 * Write this.
	 */

	(void)old;

	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	/*
	 * Clean up as needed.
	 */

	// free PTE
	for (int i = 0; i < NUM_PD_ENTRY; i++) {
		if (as->pagetable[i] != NULL) {
			for(int j = 0; j < NUM_PT_ENTRY; j++) {
				if(as->pagetable[i][j] != PTE_UNALLOCATED) {
					free_kpages(PADDR_TO_KVADDR(as->pagetable[i][j]) & PAGE_FRAME);
				}
			}
			kfree(as->pagetable[i]);
		}
	}
	kfree(as->pagetable);
	//free linked list
	free_regions(as);

	kfree(as);
}

// clean up the linked list
void free_regions(struct addrspace *as) {
	struct region *temp;
	struct region *curr = as->first;
	while(curr != NULL) {
		temp = curr;
		curr = curr->next;
		kfree(temp);
	}
}

void
as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/*
	 * Write this.
	 */
}

void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{

	if (as == NULL) {
        return EINVAL; // Invalid argument
    }

	// page alignment in dumbvm.c
	/* Align the region. First, the base... */
	memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	memsize = (memsize + PAGE_SIZE - 1) & PAGE_FRAME;

	int result = check_region(as, vaddr, memsize);
	if (result) return result; // region invalid

	struct region *new_region = kmalloc(sizeof(struct region));
	if (new_region == NULL) {
		return ENOMEM;
	}

	int permissions = 0;
    if (readable) permissions |= PF_R;
    if (writeable) permissions |= PF_W;
    if (executable) permissions |= PF_X;

	new_region->base_addr = vaddr;
	new_region->memsize = memsize;
	new_region->permissions = permissions;
	new_region->next = NULL;

	// insert new region to head of the linked list
	new_region->next = as->first;
	as->first = new_region;

	return 0;
}

int check_region(struct addrspace *as, vaddr_t vaddr, size_t memsize) {
    // Check if the ending address would wrap around.
    if (vaddr + memsize < vaddr) {
        return EINVAL;  // Address overflow, invalid region
    }

	vaddr_t new_end = vaddr + memsize;
    // region overlap with KSEG0
    if (new_end > MIPS_KSEG0) {
        return EFAULT;  // Address out of allowed range
    }

    struct region *curr = as->first;
    while (curr != NULL) {
        vaddr_t curr_end = curr->base_addr + curr->memsize;
        
        // Check for any overlapping condition
        if (vaddr < curr_end && new_end > curr->base_addr) { // New region overlaps with current
            
            return EINVAL;  // Overlapping regions, invalid
        }
		curr = curr->next;
    }

    return 0;  // Valid region, no overlaps
}

int
as_prepare_load(struct addrspace *as)
{
	if (as == NULL) {
		return EFAULT;
	}

	struct region *curr = as->first;
	while (curr != NULL) {
		curr->old_permissions = curr->permissions; // save current permission 
		curr->permissions |= PF_W; // upadate permission to writable
		curr = curr->next; 
	}

	return 0; // successful
}

int
as_complete_load(struct addrspace *as)
{
	if (as == NULL) {
		return EFAULT;
	}

    as_activate(); // Flush TLB 

    paddr_t **pt = as->pagetable;
    for(int i = 0; i < NUM_PD_ENTRY; i++) {
        if (pt[i] != NULL) {
            for(int j = 0; j < NUM_PT_ENTRY; j++) {
                if (pt[i][j] != PTE_UNALLOCATED) {
                    // Compose the virtual address from page directory and page table indices
                    vaddr_t pd_index = i << 21;
                    vaddr_t pt_index = j << 12;
                    vaddr_t vaddr = pd_index | pt_index;

                    // find matching region of the virtual address
                    struct region *curr = as->first;
					while (curr != NULL) {
						if ((vaddr >= curr->base_addr) && (vaddr < (curr->base_addr + curr->memsize))) {
							break;
						}
						curr = curr->next;
					}
                    
					// check old permission, if doesn't have write permmison, remove write permission
					if (!(curr->old_permissions & PF_W)) {
						// remove write permmision
						pt[i][j] = (pt[i][j] & PAGE_FRAME) & ~PF_W; 
						// ensure PTE valid
						pt[i][j] |= TLBLO_VALID;
					}
                }
            }
        }
	}

	struct region *curr = as->first; 
    while (curr != NULL) {
        // Restore the write permission state from old_permissions
        if (curr->old_permissions & PF_W) {
            curr->permissions |= PF_W;  // Set write permission if it was previously set
        } else {
            curr->permissions &= ~PF_W; // Clear write permission if it was not set
        }
        curr = curr->next; 
    }

	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	/*
	 * Write this.
	 */

	(void)as;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return as_define_region(as, *stackptr - USTACK_SIZE, USTACK_SIZE, 1, 1, 0);

	return 0;
}

