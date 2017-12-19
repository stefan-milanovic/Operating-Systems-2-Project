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
	
}

Status KernelProcess::createSegment(VirtualAddress startAddress, PageNum segmentSize,
	AccessType flags) {
		// check inconsistencies
	if (inconsistencyCheck(startAddress, segmentSize)) return TRAP;					// check if squared into start of page or overlapping segment

}

Status KernelProcess::loadSegment(VirtualAddress startAddress, PageNum segmentSize,
	AccessType flags, void* content) {
	if (inconsistencyCheck(startAddress, segmentSize)) return TRAP;					// check if squared into start of page or overlapping segment


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