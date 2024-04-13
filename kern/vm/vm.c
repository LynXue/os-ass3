#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <current.h>

// ADDED():
#include <elf.h>

/* Place your page table functions here */


void vm_bootstrap(void)
{
    /* Initialise any global components of your VM sub-system here.  
     *  
     * You may or may not need to add anything here depending what's
     * provided or required by the assignment spec.
     */
}


// called on a page fault, return 0 load TLB successfully
int
vm_fault(int faulttype, vaddr_t faultaddress)
{
    /**  VM Fault Flow
     * If faulttype is VM_FAULT_READONLY, returns EFAULT.
     * Attempts to find a page table entry (PTE).
     * If PTE is present(valid translation), loads it into the TLB.
     * If PTE is absent, checks if the fault address is in a valid region:
     * - If region valid, allocates a frame, zeroes it, inserts PTE, and loads TLB entry.
     * - If not valid, returns EFAULT. 
    */  

    if (curproc == NULL) { // no process
        return EFAULT; 
    }

    switch (faulttype)
    {
        case VM_FAULT_READONLY:
            return EFAULT;
        case VM_FAULT_WRITE:
        case VM_FAULT_READ:
            break;
        default:
            return EINVAL; 
    }

    // get address space of current process
    struct addrspace *as = proc_getas(); 
    if (as == NULL)  return EFAULT;
    if (as->pagetable == NULL)  return EFAULT;  // no pagetable
    if (as->first == NULL)  return EFAULT;  // no region

    uint32_t pd_index = get_pd_bits(faultaddress);
    uint32_t pt_index = get_pt_bits(faultaddress);

    // look up page table => translation valid or invalid
    if (lookup_pt(as, faultaddress) != PTE_UNALLOCATED) { // valid ranslation => load TLB entry
        load_tlb_entry(faultaddress & PAGE_FRAME, as->pagetable[pd_index][pt_index]); 
        return 0; // return successful
    } else { // invalid translation => check whether in valid region
        if (lookup_region(as, faultaddress, faulttype) == EFAULT) { // not in valid region
            return EFAULT; 
        } else {  // in valid region => allocate frame, zero-fill, insert PTE
            vaddr_t new_vaddr = alloc_kpages(1);
            if (new_vaddr == 0) return ENOMEM;
            bzero((void *)new_vaddr, PAGE_SIZE);
            paddr_t paddr = KVADDR_TO_PADDR(new_vaddr) & PAGE_FRAME;
            
            // insert PTE
            insert_pte(as, faultaddress, paddr);
            // load TLB entry
            load_tlb_entry(faultaddress & PAGE_FRAME, as->pagetable[pd_index][pt_index]);
            return 0; // return sucessful
        }
    }


}

/*
 * SMP-specific functions.  Unused in our UNSW configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}


// ADDED(): Helper function 
int insert_pte(struct addrspace *as, vaddr_t vaddr, paddr_t paddr) {
    uint32_t pd_index = get_pd_bits(vaddr); // get page directory index (Level 1)
    uint32_t pt_index = get_pt_bits(vaddr); // get page table index (Level 2)

    // check the range of index
    if (pd_index >= NUM_PD_ENTRY || pt_index >= NUM_PT_ENTRY) {
        return EFAULT; 
    }

    // check whether the page directory is initialized
    if (as->pagetable[pd_index] == NULL) { // not initialized, need to allocate memory for L2
        as->pagetable[pd_index] = (paddr_t *)kmalloc(sizeof(paddr_t) * NUM_PT_ENTRY);
        if (as->pagetable[pd_index] == NULL) {
            return ENOMEM; // out of memory
        }
        // initiliaze all the entry in L2 with -1, represent unallocated entry
        for (int i = 0; i < NUM_PT_ENTRY; i++) {
            as->pagetable[pd_index][i] = PTE_UNALLOCATED;
        }
    }

    // check if the PTE is already allocated
    if (as->pagetable[pd_index][pt_index] != PTE_UNALLOCATED) {
        return EFAULT; 
    }

    // check the write permission of the region of the vaddr
    struct region *curr = as->first;
    while (curr != NULL) { // find the matching region of the vaddr
        if (vaddr >= curr->base_addr && (vaddr < (curr->base_addr + curr->memsize))) {
            break;
        }
        curr = curr->next;
    }
    if (curr->permissions & PF_W) {  // if has writable permission
        paddr = paddr | TLBLO_DIRTY;   // set dirty bit of the PTE
    }
    // inset PTE
    as->pagetable[pd_index][pt_index] = paddr | TLBLO_VALID; 

    return 0; // insert successfully
}

int lookup_region(struct addrspace *as, vaddr_t vaddr, int faulttype) {
    struct region *curr = as->first; // Start from the head of the list

    while (curr != NULL) { // Traverse the linked list
        // Check if the virtual address is within the current region's range
        if (vaddr >= curr->base_addr && vaddr < (curr->base_addr + curr->memsize)) {
            // If inside the range, check permissions
            int allowed = 0; // Initially not allowed

            // Check permissions based on fault type and region permissions
            switch (faulttype) {
                case VM_FAULT_READ:
                    allowed = curr->permissions & PF_R; // Check if readable
                    break;
                case VM_FAULT_WRITE:
                    allowed = curr->permissions & PF_W; // Check if writable
                    break;
                default:
                    return EINVAL; 
            }

            if (!allowed) {
                return EPERM; // Return an error if not allowed
            }

            return 0; // Return success if allowed
        }

        curr = curr->next; // Move to the next region in the list
    }

    // Return an error if the address is not within any region
    return EFAULT;
}


paddr_t lookup_pt(struct addrspace *as, vaddr_t vaddr) {
    uint32_t pd_index = get_first_10_bits(vaddr); // Get the page directory index
    uint32_t pt_index = get_middle_10_bits(vaddr); // Get the page table index

    if (pd_index >= NUM_PD_ENTRY || pt_index >= NUM_PT_ENTRY) {
        return -1; // Index exceeds page table size
    } 

    // Return PTE_UNALLOCATED if the page directory entry or page table entry is empty
    if (as->pagetable[pd_index] == NULL || as->pagetable[pd_index][pt_index] == PTE_UNALLOCATED) {
        return PTE_UNALLOCATED;
    }

    paddr_t paddr = as->pagetable[pd_index][pt_index] & PAGE_FRAME; // Read the physical address

    return paddr; // Return the found physical address
}



// get the page directory bits, top 11 bits
vaddr_t get_pd_bits(vaddr_t vaddr) {
    return vaddr >> 21;
}

// get the page table bits, 9 bits
vaddr_t get_pt_bits(vaddr_t vaddr) {
    return (vaddr << 11) >> 23;
}

// load an entry to TLB
void load_tlb_entry(uint32_t entryhi, uint32_t entrylo)
{
    // Raise interrupt level to prevent any further interrupts
    int spl = splhigh();
    tlb_random(entryhi, entrylo);
    // Restore previous interrupt level
    splx(spl);
}

