#include <vector>
#include <iterator>
#include <algorithm>

#include "KernelSystem.h"
#include "KernelProcess.h"
#include "Process.h"
#include "vm_declarations.h"

KernelProcess::KernelProcess(ProcessId pid) {
	this->id = pid;																	// assign the id of the process
	// ...																			// other members are assigned in KernelSystem's createProcess()
}

KernelProcess::~KernelProcess() {
		
	while (segments.size() > 0) {													// remove any leftover segments from memory and/or disk
		optimisedDeleteSegment(&(segments.back()));
		segments.pop_back();
	}

	system->freePMTSlot(PMT1);														// declare the PMT1 as free

	system->activeProcesses.erase(id);												// remove the process from the system's active process hash map
}

Status KernelProcess::createSegment(VirtualAddress startAddress, PageNum segmentSize,
	AccessType flags) {

	if (inconsistencyCheck(startAddress, segmentSize)) return TRAP;					// check if squared into start of page or overlapping segment

	// no need to check here for disk space -- disk for a created segment is only reserved once a page with no disk cluster has to be swapped out

	KernelSystem::PMT2Descriptor* firstDescriptor = system->allocateDescriptors(this, startAddress, segmentSize, flags, false, nullptr);

	if (!firstDescriptor) return TRAP;

	SegmentInfo newSegmentInfo(startAddress, flags, segmentSize, firstDescriptor);	// create info about the segment for the process
	segments.insert(std::upper_bound(segments.begin(), segments.end(), newSegmentInfo, [newSegmentInfo](const SegmentInfo& info) {
		return newSegmentInfo.startAddress < info.startAddress;
	}), newSegmentInfo);															// insert into the segment list sorted by startAddress

	return OK;
}

Status KernelProcess::loadSegment(VirtualAddress startAddress, PageNum segmentSize,
	AccessType flags, void* content) {

	if (inconsistencyCheck(startAddress, segmentSize)) return TRAP;					// check if squared into start of page or overlapping segment

	if (!system->diskManager->hasEnoughSpace(segmentSize)) return TRAP;				// if the partition doesn't have enough space

	// possibility to optimise this -- check it later

	KernelSystem::PMT2Descriptor* firstDescriptor = system->allocateDescriptors(this, startAddress, segmentSize, flags, true, content);

	if (!firstDescriptor) return TRAP;												// error in descriptor allocation (eg. not enough room for all PMT2's)

	SegmentInfo newSegmentInfo(startAddress, flags, segmentSize, firstDescriptor);	// create info about the segment for the process
	segments.insert(std::upper_bound(segments.begin(), segments.end(), newSegmentInfo, [newSegmentInfo](const SegmentInfo& info) {
		return newSegmentInfo.startAddress < info.startAddress;
	}), newSegmentInfo);															// insert into the segment list sorted by startAddress

	return OK;
}

Status KernelProcess::deleteSegment(VirtualAddress startAddress) {
	if (inconsistentAddressCheck(startAddress)) return TRAP;						// check if squared into start of page

	bool segmentFound = false;
	int foundPosition = 0;
		
	SegmentInfo* segmentInfo;														// check if the start address is a start of a segment
	for (auto segment = segments.begin(); segment != segments.end(); segment++, foundPosition++) {	

		if (segment->startAddress > startAddress) return TRAP;						// address isn't at the start of any previous segment

		if (segment->startAddress == startAddress) {
			segmentFound = true;
			segmentInfo = &(*segment);
			break;
		}
	}

	if (!segmentFound) return TRAP;													// address offshoots all segment start addresses

	releaseMemoryAndDisk(segmentInfo);												// release memory and disk of the entire segment

	segments.erase(segments.begin() + foundPosition);								// remove the segment from the segment vector

	return OK;
}

Status KernelProcess::pageFault(VirtualAddress address) {
	
	// returns trap if blocks are full but disk is full as well and no space to save

	// POSSIBLE OPTIMISATION: no cluster reservation beforehand for createSegment

	KernelSystem::PMT2Descriptor* pageDescriptor = system->getPageDescriptor(this, address);
	if (!pageDescriptor) {															// if there is no pmt2 for this address (aka random address)
		return TRAP;
	}

	if (!pageDescriptor->getInUse()) {												// access of random descriptor, page is not part of any segment
		return TRAP;
	}

	if (pageDescriptor->getV()) return OK;											// page is already loaded in memory

	PhysicalAddress freeBlock = system->getFreeBlock();								// attempt to find a free block, function returns nullptr if none exist
	if (!freeBlock)
		freeBlock = system->getSwappedBlock();										// if a free block doesn't exist -- choose a block to swap out

	if (!freeBlock) return TRAP;													// in case of createSegment: if no space on disk do not allow swap

	if (pageDescriptor->getHasCluster()) {											// if the page has a cluster on disk, read the contents
		if (!system->diskManager->read(freeBlock, pageDescriptor->getDisk()))
			return TRAP;															// if the read was unsucessful return adequate status
	}


	pageDescriptor->setV();
	pageDescriptor->setBlock(freeBlock);											// set the given block in the descriptor

																					// set register's descriptor pointer to this descriptor
	system->referenceRegisters[(*((unsigned*)freeBlock) - *((unsigned*)system->processVMSpace)) / PAGE_SIZE].pageDescriptor = pageDescriptor;

	return OK;
}

PhysicalAddress KernelProcess::getPhysicalAddress(VirtualAddress address) {

	KernelSystem::PMT2Descriptor* pageDescriptor = system->getPageDescriptor(this, address);

	if (!pageDescriptor) return 0;															// pmt2 not allocated

	if (!pageDescriptor->getV()) return 0;													// page isn't loaded in memory

	PhysicalAddress pageBase = pageDescriptor->block;										// extract base of page;
	unsigned long word = 0;
																							// extract word from the virtual address
	for (VirtualAddress mask = 1, unsigned short i = 0; i < KernelSystem::wordPartBitLength; i++) {
		word |= address & mask;
		mask <<= 1;
	}

	return (PhysicalAddress)((unsigned long)(pageBase) + word);				
}


void KernelProcess::blockIfThrashing() {
	

}

// private methods

bool KernelProcess::inconsistencyCheck(VirtualAddress startAddress, PageNum segmentSize) {

	if (inconsistentAddressCheck(startAddress)) return true;					// check if squared into start of page

	// segments field is sorted by startAddress
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

bool KernelProcess::inconsistentAddressCheck(VirtualAddress startAddress) {
	for (VirtualAddress mask = 1, int i = 0; i < 10; i++) {						// check if squared into start of page
		if (startAddress & mask)
			return true;														// at least 1 of the lowest 10 bits isn't zero
		else
			mask <<= 1;
	}
	return false;
}

Status KernelProcess::optimisedDeleteSegment(SegmentInfo* segment) {

	releaseMemoryAndDisk(segment);													// release the memory and the disk of the entire segment
	segments.pop_back();															// remove the last segment from the segment vector

	return OK;
}

void KernelProcess::releaseMemoryAndDisk(SegmentInfo* segment) {

	system->mutex.lock();

	KernelSystem::PMT2Descriptor* temp = segment->firstDescAddress;
	VirtualAddress tempAddress = segment->startAddress;

																						// for each page of the segment do
	for (PageNum i = 0; i < segment->length; i++, temp = temp->next, tempAddress += PAGE_SIZE) {		

		if (temp->getV()) {																// if the page is in memory, declare the block as free
			system->setFreeBlock(temp->block);
		}

		if (temp->getHasCluster()) {													// if the page is saved on disk, declare the cluster as free
			system->diskManager->freeCluster(temp->disk);
		}

		temp->resetInUse();																// the page is not used anymore

		unsigned short pmt1Entry = 0, pmt2Entry = 0;									// extract relevant parts of the address

		for (VirtualAddress mask = 1 << KernelSystem::wordPartBitLength, unsigned i = KernelSystem::wordPartBitLength;
			i < KernelSystem::usefulBitLength - KernelSystem::wordPartBitLength; i++) {

			if (i < KernelSystem::wordPartBitLength + KernelSystem::page2PartBitLength) {
				pmt2Entry |= tempAddress & mask >> KernelSystem::wordPartBitLength;
			}
			else {
				pmt1Entry |= tempAddress & mask >> (KernelSystem::wordPartBitLength + KernelSystem::page2PartBitLength);
			}
			mask <<= 1;
		}

		unsigned pageKey = system->simpleHash(id, pmt1Entry);							// find key

		system->activePMT2Counter[pageKey].counter--;									// access the counter for the specific pmt2
		if (system->activePMT2Counter[pageKey].counter == 0) {							// if the counter has reached 0, deallocate the pmt2
			system->freePMTSlot(system->activePMT2Counter[pageKey].pmt2StartAddress);
			system->activePMT2Counter.erase(pageKey);									// erase the pmt2 from the counter hash table
		}
	}

	system->mutex.unlock();
}

unsigned KernelProcess::concatenatePageParts(unsigned short page1, unsigned short page2) {
	return (page1 << KernelSystem::page1PartBitLength) | page2;
}