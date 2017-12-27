#include <iostream>
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
		optimisedDeleteSegment(&(segments.back()), false, -1);
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

	
	segments.insert(std::upper_bound(segments.begin(), segments.end(), newSegmentInfo, []
	(const SegmentInfo& seg1, SegmentInfo& seg2) {
		return seg1.startAddress < seg2.startAddress;
	}), newSegmentInfo);															// insert into the segment list sorted by startAddress
	
	return OK;
}

Status KernelProcess::loadSegment(VirtualAddress startAddress, PageNum segmentSize,
	AccessType flags, void* content) {

	if (inconsistencyCheck(startAddress, segmentSize)) return TRAP;					// check if squared into start of page or overlapping segment

	if (!system->diskManager->hasEnoughSpace(segmentSize)) 
		return TRAP;				// if the partition doesn't have enough space

	// possibility to optimise this -- check it later

	KernelSystem::PMT2Descriptor* firstDescriptor = system->allocateDescriptors(this, startAddress, segmentSize, flags, true, content);

	if (!firstDescriptor) return TRAP;												// error in descriptor allocation (eg. not enough room for all PMT2's)

	SegmentInfo newSegmentInfo(startAddress, flags, segmentSize, firstDescriptor);	// create info about the segment for the process
	segments.insert(std::upper_bound(segments.begin(), segments.end(), newSegmentInfo, [](const SegmentInfo& segment1,  const SegmentInfo& segment2) {
		return segment1.startAddress < segment2.startAddress;
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
		return TRAP;																// if the read was unsucessful return adequate status
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

	word = KernelSystem::extractWordPart(address);

	// std::cout << "VA: " << address << " => PA: " << (unsigned long)pageBase + word << std::endl;
	
	return (PhysicalAddress)((unsigned long)(pageBase) + word);				
}


void KernelProcess::blockIfThrashing() {
	

}

Process* KernelProcess::clone(ProcessId pid) {

	// this method doesn't need to check for space, it just performs copying (KernelSystem has checked this already)
	Process* clonedProcess;

	return clonedProcess;
}

Status KernelProcess::createSharedSegment(VirtualAddress startAddress, PageNum segmentSize, const char* name, AccessType flags) {
	if (inconsistencyCheck(startAddress, segmentSize)) return TRAP;					// check if squared into start of page or overlapping segment

																					// no need to check here for disk space -- disk for a created segment is
																				    // only reserved once a page with no disk cluster has to be swapped out

																					// creates one as well if it didn't exist
	KernelSystem::PMT2Descriptor* firstDescriptor = system->connectToSharedSegment(this, startAddress, segmentSize, name, flags);

	if (!firstDescriptor) return TRAP;

	SegmentInfo newSegmentInfo(startAddress, flags, segmentSize, firstDescriptor);	// create info about the segment for the process


	segments.insert(std::upper_bound(segments.begin(), segments.end(), newSegmentInfo, []
	(const SegmentInfo& seg1, SegmentInfo& seg2) {
		return seg1.startAddress < seg2.startAddress;
	}), newSegmentInfo);															// insert into the segment list sorted by startAddress

	return OK;
}

Status KernelProcess::disconnectSharedSegment(const char* name) {					// works like deleteSegment() but doesn't affect the shared segment, memory or disk

	KernelSystem::SharedSegment sharedSegment;
	try {
		sharedSegment = system->sharedSegments.at(std::string(name));				// check for the key but don't insert if nonexistant 
	}																				// (that is what unordered_map::operator[] would do)
	catch (std::out_of_range noProcessWithPID) {
		return TRAP;																// cannot disconnect from a shared segment that doesn't exist
	}

																					// check if a segment is connected to the shared segment

	for (auto processInfo = sharedSegment.processesSharing.begin(); processInfo != sharedSegment.processesSharing.end(); processInfo++) {
		if (processInfo->process == this) {											// segment in virtual address space found

			SegmentInfo* segmentInVirtualSpaceInfo;									// this pointer will surely be found
			unsigned indexOfSegment = 0;											// index of segment in the list

			for (auto segmentInfo = this->segments.begin(); segmentInfo != this->segments.end(); segmentInfo++, indexOfSegment++) {
				if (segmentInfo->firstDescAddress == processInfo->firstDescriptor) {
					segmentInVirtualSpaceInfo = &(*segmentInfo);
					break;
				}
			}

			optimisedDeleteSegment(segmentInVirtualSpaceInfo, true, indexOfSegment);// delete segment from process virtual address space

			sharedSegment.numberOfProcessesSharing--;								// decrease counter of processes sharing
			sharedSegment.processesSharing.erase(processInfo);						// remove this process from the list of processes sharing the segment

			return OK;
		}
	}

	return TRAP;																	// shared segment with this name exists but this process isn't connected to it
}

Status KernelProcess::deleteSharedSegment(const char* name) {						// any process can request a shared segment deletion

	KernelSystem::SharedSegment sharedSegment;
	try {
		sharedSegment = system->sharedSegments.at(std::string(name));				// check for the key but don't insert if nonexistant 
	}																				
	catch (std::out_of_range noProcessWithPID) {
		return TRAP;																// cannot delete a shared segment that doesn't exist
	}

	// shared segment found -- delete PMT2s for all segments, all segment infos from respective processes, then delete PMT1+PMT2s+memory+disk for the shared segment

	for (auto processInfo = sharedSegment.processesSharing.begin(); processInfo != sharedSegment.processesSharing.end(); processInfo++) {
		KernelProcess* processSharingSegment = processInfo->process;

		SegmentInfo* segmentProcessIsSharing = nullptr;								// this will surely be found
		unsigned indexOfSegment = 0;

																					// find the adequate segment in the current process
		for (auto segmentInfo = processSharingSegment->segments.begin(); segmentInfo != processSharingSegment->segments.end(); segmentInfo++, indexOfSegment++) {
			if (segmentInfo->firstDescAddress == processInfo->firstDescriptor) {
				segmentProcessIsSharing = &(*segmentInfo);
				break;
			}
		}
																					// delete segment from process virtual address space
		processSharingSegment->optimisedDeleteSegment(segmentProcessIsSharing, true, indexOfSegment);	

		sharedSegment.numberOfProcessesSharing--;									// decrease counter of processes sharing
		sharedSegment.processesSharing.erase(processInfo);							// remove this process from the list of processes sharing the segment

	}

	// delete PMT1+PMT2s+memory+disk for the shared segment

	for (unsigned short i = 0; i < sharedSegment.length; i++) {						// go through all PMT2s and deallocate descriptors, memory and disk
		unsigned short sharedPMT1Entry = i / sharedSegment.pmt2Number;
		unsigned short sharedPMT2Entry = i % sharedSegment.pmt2Number;

		KernelSystem::PMT2* pmt2 = (*(sharedSegment.pmt1))[sharedPMT1Entry];

		KernelSystem::PMT2Descriptor* pageDescriptor = &(*pmt2)[sharedPMT2Entry];	// access the targetted descriptor
		
																					// descriptors in these PMT2s surely have isShared = false
		if (pageDescriptor->getV()) {												// if the page is in memory, declare the block as free
			system->setFreeBlock(pageDescriptor->block);
		}

		if (pageDescriptor->getHasCluster()) {										// if the page is saved on disk, declare the cluster as free
			system->diskManager->freeCluster(pageDescriptor->disk);
		}

		pageDescriptor->resetInUse();												// the page is not used anymore

	}

	for (unsigned short i = 0; i < sharedSegment.pmt2Number; i++) {					// free PMT2 tables for the shared segment
		system->freePMTSlot((PhysicalAddress)(*(sharedSegment.pmt1))[i]);
	}
	system->freePMTSlot((PhysicalAddress)sharedSegment.pmt1);						// free PMT1 table for the shared segment

	system->sharedSegments.erase(std::string(name));								// erase the shared segment from the system's map

	return OK;
}

// private methods

bool KernelProcess::inconsistencyCheck(VirtualAddress startAddress, PageNum segmentSize) {

	if (inconsistentAddressCheck(startAddress)) return true;					// check if squared into start of page

																				// segments vector is sorted by startAddress
	VirtualAddress endAddress = startAddress + segmentSize * PAGE_SIZE;

	for (auto segment = segments.begin(); segment != segments.end(); segment++) {
		// no overlaps when ( start < seg->start && end <= seg->start ) or ( start >= seg->end)
		if (!((startAddress < segment->startAddress && endAddress <= segment->startAddress) ||
			(startAddress >= segment->startAddress + segment->length * PAGE_SIZE)))
			return true;														// there's an overlap with the currently observed segment
	}

	return false;																// returns false if there is no inconsistency, true if the new segment would overlap with an existing one
}

bool KernelProcess::inconsistentAddressCheck(VirtualAddress startAddress) {
	VirtualAddress mask = 1;
	for (int i = 0; i < 10; i++) {												// check if squared into start of page
		if (startAddress & mask)
			return true;														// at least 1 of the lowest 10 bits isn't zero
		else
			mask <<= 1;
	}
	return false;
}

Status KernelProcess::optimisedDeleteSegment(SegmentInfo* segment, bool checkIndex, unsigned index) {

	releaseMemoryAndDisk(segment);													// release the memory and the disk of the entire segment

	if (!checkIndex)
		segments.pop_back();														// remove the last segment from the segment vector
	else
		segments.erase(segments.begin() + index);									// remove target segment info at a specifix index

	return OK;
}

void KernelProcess::releaseMemoryAndDisk(SegmentInfo* segment) {

	system->mutex.lock();

	KernelSystem::PMT2Descriptor* temp = segment->firstDescAddress;
	VirtualAddress tempAddress = segment->startAddress;

																						// for each page of the segment do
	for (PageNum i = 0; i < segment->length; i++, temp = temp->next, tempAddress += PAGE_SIZE) {		

		if (!temp->getShared()) {														// only free memory and disk if it's not a shared page
			if (temp->getV()) {															// if the page is in memory, declare the block as free
				system->setFreeBlock(temp->block);
			}

			if (temp->getHasCluster()) {												// if the page is saved on disk, declare the cluster as free
				system->diskManager->freeCluster(temp->disk);
			}
		}

		temp->resetInUse();																// the page is not used anymore

		unsigned short pmt1Entry = 0, pmt2Entry = 0;									// extract relevant parts of the address

		pmt1Entry = KernelSystem::extractPage1Part(tempAddress);
		pmt2Entry = KernelSystem::extractPage2Part(tempAddress);

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