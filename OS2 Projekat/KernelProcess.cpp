#include <vector>
#include <algorithm>

#include "KernelSystem.h"
#include "KernelProcess.h"
#include "Process.h"
#include "vm_declarations.h"

KernelProcess::KernelProcess(ProcessId pid) {
	this->id = pid;											// assign the id of the process
	// ...													// other parameters are assigned in the KernelSystem's createProcess()
}

KernelProcess::~KernelProcess() {
															// remove any leftover segments from memory and disk + remove from hash map of sys
}

Status KernelProcess::createSegment(VirtualAddress startAddress, PageNum segmentSize,
	AccessType flags) {
		// check inconsistencies
	if (inconsistencyCheck(startAddress, segmentSize)) return TRAP;					// check if squared into start of page or overlapping segment

}

Status KernelProcess::loadSegment(VirtualAddress startAddress, PageNum segmentSize,
	AccessType flags, void* content) {

	if (inconsistencyCheck(startAddress, segmentSize)) return TRAP;					// check if squared into start of page or overlapping segment

	if (!system->diskManager->hasEnoughSpace(segmentSize)) return TRAP;

	// possibility to optimise this -- check it later

	struct EntryCreationHelper {													// helper struct for transcation reasons
		unsigned short pmt1Entry;													// entry in pmt1
		unsigned short pmt2Entry;													// entry in pmt2
		KernelSystem::PMT2Descriptor descriptor;									// this will be used if there is enough memory to store all pmt2s
	};

	std::vector<unsigned short> missingPMT2s;										// remember indices of PMT2 tables that need to be allocated
	std::vector<EntryCreationHelper> entries;										// all pages that will be loaded

	for (PageNum i = 0; i < segmentSize; i++) {										// create PMT2 descriptors
		VirtualAddress blockVirtualAddress = startAddress + i * PAGE_SIZE;
		EntryCreationHelper entry;
		unsigned pmt1Entry = 0, pmt2Entry = 0;										// extract relevant parts of the address

		for (VirtualAddress mask = 1 << KernelSystem::wordPartBitLength, unsigned i = KernelSystem::wordPartBitLength;
			i < KernelSystem::usefulBitLength - KernelSystem::wordPartBitLength; i++) {

			if (i < KernelSystem::wordPartBitLength + KernelSystem::page2PartBitLength) {
				pmt2Entry |= blockVirtualAddress & mask >> KernelSystem::wordPartBitLength;
			}
			else {
				pmt1Entry |= blockVirtualAddress & mask >> (KernelSystem::wordPartBitLength + KernelSystem::page2PartBitLength);
			}
			mask <<= 1;
		}

		entry.pmt1Entry = pmt1Entry, entry.pmt2Entry = pmt2Entry;
		entries.push_back(entry);

		KernelSystem::PMT2* pmt2 = (*PMT1)[pmt1Entry];			     				// access the PMT2 pointer
		if (!pmt2 && !std::binary_search(missingPMT2s.begin(), missingPMT2s.end(), pmt1Entry)) {					
			missingPMT2s.push_back(pmt1Entry);										// if that pmt2 table doesn't exist yet, add it to the miss list
		}
	}

	if (missingPMT2s.size() > system->numberOfFreePMTSlots) return TRAP;			// insufficient slots in PMT memory

	PageNum pageOffsetCounter = 0;													// create descriptor for each page, allocate pmt2 if needed
	KernelSystem::PMT2Descriptor* firstDescriptor = nullptr;
	for (auto entry = entries.begin(); entry != entries.end(); entry++) {

		KernelSystem::PMT2* pmt2 = (*(this->PMT1))[entry->pmt1Entry];

		if (!pmt2) {																// if the pmt2 table doesn't exist, create it
			pmt2 = (KernelSystem::PMT2*)system->freePMTSlotHead;					// assign a free block to the required PMT2

			system->freePMTSlotHead = (PhysicalAddress)(*((unsigned*)system->freePMTSlotHead));		// move the pmt list head
		}

		KernelSystem::PMT2Descriptor* pageDescriptor = &(*pmt2)[entry->pmt2Entry];	// access the targetted descriptor
		if (!pageOffsetCounter) firstDescriptor = pageDescriptor;

		switch (flags) {															// set access rights
		case READ:
			pageDescriptor->setRd();
			break;
		case WRITE:
			pageDescriptor->setWr();
			break;
		case READ_WRITE:
			pageDescriptor->setRdWr();
			break;
		case EXECUTE:
			pageDescriptor->setEx();
			break;
		}

		void* pageContent = (void*)((char*)content + pageOffsetCounter++ * PAGE_SIZE);
		pageDescriptor->setDisk(system->diskManager->write(pageContent));			// the disk manager's write() returns the cluster number
		pageDescriptor->setClusterBit();											// the page's location on the partition is known

		if (!system->clockHand) {													// chain the descriptor in the clockhand list
			system->clockHand = pageDescriptor;
			pageDescriptor->next = pageDescriptor;
		}
		else {
			pageDescriptor->next = system->clockHand->next;
			system->clockHand->next = pageDescriptor;
			system->clockHand = system->clockHand->next;
		}
	}

	SegmentInfo newSegmentInfo(startAddress, flags, segmentSize, firstDescriptor);	// create info about the segment for the process

	segments.insert(std::upper_bound(segments.begin(), segments.end(), newSegmentInfo, [newSegmentInfo](const SegmentInfo& info) {
		return newSegmentInfo.startAddress < info.startAddress;
	}), newSegmentInfo);															// insert into the segment list sorted by startAddress

	return OK;
}

Status KernelProcess::deleteSegment(VirtualAddress startAddress) {
	 // check for inconsistencies, the start addr has to be the beginning of a block/segment
	
}

Status KernelProcess::pageFault(VirtualAddress address) {
	
}

PhysicalAddress KernelProcess::getPhysicalAddress(VirtualAddress address) {

	KernelSystem::PMT2Descriptor* pageDescriptor = KernelSystem::getPageDescriptor(this, address);

	if (!pageDescriptor) return 0;

	if (!pageDescriptor->v) return 0;

	PhysicalAddress pageBase = pageDescriptor->block;										// extract base of page;
	unsigned long word = 0;
																							// extract word from the virtual address
	for (VirtualAddress mask = 1, unsigned short i = 0; i < KernelSystem::wordPartBitLength; i++) {
		word |= address & mask;
		mask <<= 1;
	}

	return (PhysicalAddress)((unsigned long)(pageBase) + word);				// ?? check
}


// private methods

bool KernelProcess::inconsistencyCheck(VirtualAddress startAddress, PageNum segmentSize) {

	for (VirtualAddress mask = 1, int i = 0; i < 10; i++) {						// check if squared into start of page
		if (startAddress & mask)
			return true;														// at least 1 of the lowest 10 bits isn't zero
		else
			mask <<= 1;
	}

	// segments is sorted by startAddress
	// check if there is any overlapping segment

	VirtualAddress endAddress = startAddress + segmentSize * PAGE_SIZE;

	for (auto segment = segments.begin(); segment != segments.end(); segment++) {
		// no overlaps when ( start < seg->start && end <= seg->start ) or ( start >= seg->end)
		if (!((startAddress < segment->startAddress && endAddress <= segment->startAddress) ||
			(startAddress >= segment->startAddress + segment->length * PAGE_SIZE)))
			return true;														// there's an overlap with the currently observed segment
	}

	return false;
	// returns false if there is no inconsistency, true if the new segment would overlap with an existing one
}