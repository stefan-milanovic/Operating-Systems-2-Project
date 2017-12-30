#include <iostream>
#include <vector>
#include <iterator>
#include <algorithm>
#include <random>

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

		auto segmentInfo = segments.back();
																					// remove this process from the adequate shared segment
		if (segmentInfo.sharedSegmentName != "") {									// (only if the user hasn't called disconnectShared() or deleteShared())

			KernelSystem::SharedSegment& sharedSegment = system->sharedSegments.at(segmentInfo.sharedSegmentName);
			auto reverseInfoToDelete = std::find_if(sharedSegment.processesSharing.begin(), sharedSegment.processesSharing.end(),
				[this](const KernelSystem::ReverseSegmentInfo& info) {
				return info.process == this;
			});
			sharedSegment.numberOfProcessesSharing--;								// decrease counter of processes sharing
			sharedSegment.processesSharing.erase(reverseInfoToDelete);				// remove this process from the list of processes sharing the segment

		}
		optimisedDeleteSegment(&(segments.back()), false, -1);						// calls segments.pop_back() 
	}

	system->freePMTSlot(PMT1);														// declare the PMT1 as free


	if (system->thrashingSemaphore.get_count() < 0)									// there's at least one process that was blocked because of thrashing
		system->thrashingSemaphore.notify();

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

	system->mutex.lock();
	if (!system->diskManager->hasEnoughSpace(segmentSize)) {
		system->mutex.unlock();
		return TRAP;																// if the partition doesn't have enough space
	}
	system->mutex.unlock();
	KernelSystem::PMT2Descriptor* firstDescriptor = system->allocateDescriptors(this, startAddress, segmentSize, flags, true, content);

	if (!firstDescriptor) return TRAP;												// error in descriptor allocation (eg. not enough room for all PMT2's)

	SegmentInfo newSegmentInfo(startAddress, flags, segmentSize, firstDescriptor);	// create info about the segment for the process
	segments.insert(std::upper_bound(segments.begin(), segments.end(), newSegmentInfo, [](const SegmentInfo& segment1, const SegmentInfo& segment2) {
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
	system->mutex.lock();

	KernelSystem::PMT2Descriptor* pageDescriptor = system->getPageDescriptor(this, address);
	if (!pageDescriptor) {															// if there is no pmt2 for this address (aka random address)
		system->mutex.unlock();
		return TRAP;
	}

	if (!pageDescriptor->getInUse()) {												// access of random descriptor, page is not part of any segment
		system->mutex.unlock();
		return TRAP;
	}

	if (pageDescriptor->getCloned()) {												// if there was a page-fault for a cloned descriptor there's a chance it's a write attempt
																					// if this process attempted to write it will be in the copyOnWrite buffer
		auto iterator = std::find(system->processesAttemptingCopyOnWrite.begin(), system->processesAttemptingCopyOnWrite.end(), this->id);

		if (iterator != system->processesAttemptingCopyOnWrite.end()) {				// this process attempted to write in a cloned page
			system->processesAttemptingCopyOnWrite.erase(iterator);

																					// access cloning descriptor and reserve a slot on the disk
			KernelSystem::PMT2Descriptor* cloningDescriptor = (KernelSystem::PMT2Descriptor*) pageDescriptor->getBlock();

			if (!system->diskManager->hasEnoughSpace(1)) {
				system->mutex.unlock();
				return TRAP;														// no more space on disk
			}
			unsigned cloningKey = pageDescriptor->getDisk();

			if (cloningDescriptor->getV()) {										// allocate space on disk for this page
				pageDescriptor->setDisk(system->diskManager->write(cloningDescriptor->getBlock()));
			}
			else {
				pageDescriptor->setDisk(system->diskManager->writeFromCluster(cloningDescriptor->getDisk()));
			}

			pageDescriptor->resetV();
			pageDescriptor->resetCloned();											// this page no longer points to a cloning PMT2
			//pageDescriptor->resetCopyOnWrite();
			pageDescriptor->setHasCluster();
																					// find the cloning PMT2 and decrease counters
			KernelSystem::PMT2DescriptorCounter* cloningPMT2Counter = &(system->activePMT2Counter.at(cloningKey));

																					// find counter to decrease
			unsigned pmt2entry = KernelSystem::extractPage2Part(address);
			auto counterToDecrease = std::find_if(cloningPMT2Counter->sourceDescriptorCounters.begin(),
				cloningPMT2Counter->sourceDescriptorCounters.end(),
				[pmt2entry](std::pair<unsigned, unsigned>& pair) { return pair.first == pmt2entry; });

			counterToDecrease->second--;											// decrease the counter
			if (counterToDecrease->second == 0) {									// if it reached zero, remove the descriptor counter and adjust PMT2 counter
				cloningPMT2Counter->sourceDescriptorCounters.erase(counterToDecrease);
				cloningPMT2Counter->counter--;
				if (cloningPMT2Counter->counter == 0) {								// if the cloning PMT2 is not pointed to at all anymore, deallocate it
					system->freePMTSlot((PhysicalAddress)cloningPMT2Counter->pmt2StartAddress);
					system->activePMT2Counter.erase(cloningKey);
				}
			}


		}
		else {
			pageDescriptor = (KernelSystem::PMT2Descriptor*)pageDescriptor->getBlock();
		}
	}

	if (pageDescriptor->getShared())												// if this page is of a shared segment, switch to the appropriate descriptor
		pageDescriptor = (KernelSystem::PMT2Descriptor*)pageDescriptor->getBlock();

	if (pageDescriptor->getV()) { system->mutex.unlock(); return OK; }				// page is already loaded in memory

	PhysicalAddress freeBlock = system->getFreeBlock();								// attempt to find a free block, function returns nullptr if none exist
	if (!freeBlock) {
		freeBlock = system->getSwappedBlock();										// if a free block doesn't exist -- choose a block to swap out
																					// std::cout << "Proces " << id << "got a swapped block." << std::endl;
	}
	if (!freeBlock) { system->mutex.unlock(); return TRAP; }						// in case of createSegment: if no space on disk do not allow swap

	if (pageDescriptor->getHasCluster()) {											// if the page has a cluster on disk, read the contents
		if (!system->diskManager->read(freeBlock, pageDescriptor->getDisk())) {
			system->mutex.unlock();
			return TRAP;															// if the read was unsucessful return adequate status
		}
	}


	pageDescriptor->setV();
	pageDescriptor->setBlock(freeBlock);											// set the given block in the descriptor

																					// set register's descriptor pointer to this descriptor
	system->referenceRegisters[((unsigned)(freeBlock)-(unsigned)(system->processVMSpace)) / PAGE_SIZE].pageDescriptor = pageDescriptor;

	system->mutex.unlock();
	return OK;
}

PhysicalAddress KernelProcess::getPhysicalAddress(VirtualAddress address) {

	KernelSystem::PMT2Descriptor* pageDescriptor = system->getPageDescriptor(this, address);

	if (!pageDescriptor) return 0;															// pmt2 not allocated

	if (pageDescriptor->getShared() || pageDescriptor->getCloned())							// if this page is of a shared segment, switch to the appropriate descriptor
		pageDescriptor = (KernelSystem::PMT2Descriptor*)pageDescriptor->getBlock();

	if (!pageDescriptor->getV()) return 0;													// page isn't loaded in memory

	PhysicalAddress pageBase = pageDescriptor->block;										// extract base of page;
	unsigned long word = 0;

	word = KernelSystem::extractWordPart(address);

	// std::cout << "VA: " << address << " => PA: " << (unsigned long)pageBase + word << std::endl;

	return (PhysicalAddress)((unsigned long)(pageBase)+word);
}


void KernelProcess::blockIfThrashing() {

	if (shouldBlockFlag) {

		system->mutex.lock();
																							// for each segment do
		for (auto segment = segments.begin(); segment != segments.end(); segment++) {

			KernelSystem::PMT2Descriptor* temp = segment->firstDescAddress;
			for (PageNum i = 0; i < segment->length; i++, temp = temp->next) {

				if (temp->getShared() || temp->getCloned())
					temp = (KernelSystem::PMT2Descriptor*)temp->getBlock();

				if (temp->getV()) {																// if this descriptor has a page in memory 

					if (temp->getD()) {															// write the block to the disk if it's dirty (always true for never-before-written-to-disk createSegment() pages)
						if (temp->getHasCluster())												// if the page already has a reserved cluster on the disk, write contents there
							system->diskManager->writeToCluster(temp->getBlock(), temp->getDisk());
						else {																	// if not, attempt to find an empty slot
							temp->setDisk(system->diskManager->write(temp->getBlock()));
							if (temp->getDisk() == -1) {
								system->mutex.unlock();
								return;															// no room on the disk or error while writing
							}
							temp->setHasCluster();												// the victim now has a cluster on the disk
						}
						temp->resetD();
					}

					temp->resetV();																// this page is no longer in memory
					system->setFreeBlock(temp->getBlock());										// chain the block in the free block list
				}
				temp->resetReferenced();
			}

		}
		
		shouldBlockFlag = false;
		system->mutex.unlock();
		system->thrashingSemaphore.wait();
	}

}

Process* KernelProcess::clone(ProcessId pid) {

	// this method doesn't need to check for space, it just performs cloning (KernelSystem has checked this already)

	Process* clonedProcess = new Process(pid);
	clonedProcess->pProcess->system = system;								// initialise system pointer and get a PMT1 slot
	clonedProcess->pProcess->PMT1 = (KernelSystem::PMT1*)system->getFreePMTSlot();

	for (unsigned short i = 0; i < KernelSystem::PMT1Size; i++) {			// initialise all of its pointers to nullptr
		(*(clonedProcess->pProcess->PMT1))[i] = nullptr;
	}

	KernelSystem::PMT1* originalPMT1 = this->PMT1;							// go through all of the descriptors of the original and initialise appropriately

	for (unsigned short i = 0; i < KernelSystem::PMT1Size; i++) {			// copy all tables, if a cloning PMT2 table is being made link both original and new one to it
		KernelSystem::PMT2* originalPMT2 = (*originalPMT1)[i];
		if (originalPMT2 != nullptr) {															// if a pmt2 exists perform cloning

																								// create a PMT2 for the cloned process and initialise it
			(*(clonedProcess->pProcess->PMT1))[i] = (KernelSystem::PMT2*)system->getFreePMTSlot();
			system->initialisePMT2((*(clonedProcess->pProcess->PMT1))[i]);
			KernelSystem::PMT2* clonedPMT2 = (*(clonedProcess->pProcess->PMT1))[i];

			unsigned pageKey = system->simpleHash(clonedProcess->pProcess->id, i);				// key used to access the PMT2 descriptor counter hash table
			KernelSystem::PMT2DescriptorCounter newPMT2Counter(clonedPMT2);						// add new PMT2 to the system's PMT2 descriptor counter
			system->activePMT2Counter.insert(std::pair<unsigned, KernelSystem::PMT2DescriptorCounter>(pageKey, newPMT2Counter));

																								// clone info
			CloningPMTRequest request = *(find_if(cloningPMTRequests.begin(),
				cloningPMTRequests.end(), [i](CloningPMTRequest& request) {
				return request.originalPMT1Entry == i;
			}));							
			KernelSystem::PMT2* cloningPMT2 = nullptr;
			KernelSystem::PMT2DescriptorCounter cloningPMT2Counter;								// remember counters for the cloning PMT2 as well
			unsigned cloningKey;

			if (request.shouldMakeCloningPMT2) {												// if a cloning PMT2 has to be made, descriptors must be redirected
				cloningPMT2 = (KernelSystem::PMT2*)system->getFreePMTSlot();
				system->initialisePMT2(cloningPMT2);

				std::default_random_engine generator(time(0));
				std::uniform_int_distribution<unsigned> randomKeyGenerator;
				cloningKey = randomKeyGenerator(generator);										// key used to access the PMT2 descriptor counter hash table
				cloningPMT2Counter.counter = 0;
				cloningPMT2Counter.pmt2StartAddress = cloningPMT2;
				system->activePMT2Counter.insert(std::pair<unsigned, KernelSystem::PMT2DescriptorCounter>(cloningKey, cloningPMT2Counter));

			}

			for (unsigned short j = 0; j < KernelSystem::PMT2Size; j++) {
				KernelSystem::PMT2Descriptor* descriptor = &((*originalPMT2)[j]);
				KernelSystem::PMT2Descriptor* clonedDescriptor = &((*clonedPMT2)[j]);

				if (descriptor->getInUse()) {													// only observe the page descriptor if it is in use
				
					system->activePMT2Counter[pageKey].counter++;								// a new descriptor is being added to this cloned PMT2 -- increase the counter
					clonedDescriptor->basicBits = descriptor->basicBits;						// the bits stay the same
					clonedDescriptor->advancedBits = descriptor->advancedBits;
					// disk should never be set in here, it's always in either shared segment PMT2 or a cloning PMT2 (or hold a hash table key)
					// block is set depending on the bits in the original
					// next is set while creating the segments for the process

					if (descriptor->getShared()) {												// if the descriptor is pointing to a shared PMT2, link
						clonedDescriptor->block = descriptor->block;
					}
					else {																		// the descriptor could have either cloned = 0 or cloned = 1
						if (descriptor->getCloned()) {											// if cloned = 1 just link new clone to existing cloning PMT2

							clonedDescriptor->block = descriptor->block;						// set the cloned block to point

							cloningKey = descriptor->getDisk();									// key used to access the PMT2 descriptor counter hash table
							clonedDescriptor->setDisk(cloningKey);								// set the hash key in the new descriptor

							KernelSystem::PMT2DescriptorCounter* cloningPMT2Counter = &(system->activePMT2Counter.at(cloningKey));

																								// find counter to increase
							auto counterToIncrease = std::find_if(cloningPMT2Counter->sourceDescriptorCounters.begin(), 
								cloningPMT2Counter->sourceDescriptorCounters.end(),
								[j](std::pair<unsigned, unsigned>& pair) { return pair.first == j; });

							counterToIncrease->second++;										// increase the counter
						}
						else {																	// cloned = 0 => begin cloning
							std::pair<unsigned, unsigned> newDescCounter = { j, 2 };			// remember descriptor _j_ who will have 2 pointers to it initially

							KernelSystem::PMT2Descriptor* cloningDescriptor = &((*cloningPMT2)[j]);

							cloningDescriptor->basicBits = descriptor->basicBits;
							cloningDescriptor->advancedBits = descriptor->advancedBits;
							cloningDescriptor->block = descriptor->block;
							cloningDescriptor->disk = descriptor->disk;

							//if (descriptor->getV()) {											// if there is a page in memory, switch the reference register pointer
							//}

							descriptor->disk = clonedDescriptor->disk = cloningKey;				// remember the key in both 

							descriptor->setCloned();											// set that the two original descriptors are now cloned
							// descriptor->setCopyOnWrite();
							descriptor->block = cloningDescriptor;								// they share the same entry in the cloning PMT2

							clonedDescriptor->setCloned();
							// clonedDescriptor->setCopyOnWrite();
							clonedDescriptor->block = cloningDescriptor;

							system->activePMT2Counter[cloningKey].counter++;					// a descriptor in the cloning PMT2 is being used	
							system->activePMT2Counter[cloningKey].sourceDescriptorCounters.push_back(newDescCounter);	// count the pointers for this descriptor
						}
					}
				}

			}

		}
	}

	this->cloningPMTRequests = std::vector<CloningPMTRequest>();			// empty the request vector

																			// copy all the segments, add the clone to a shared segment if the original is connected
																			// also chain cloned descriptors by segment
	for (auto originalSegment = segments.begin(); originalSegment != segments.end(); originalSegment++) {

																			// first chain the cloned descriptors
		unsigned short startPMT1Entry = KernelSystem::extractPage1Part(originalSegment->startAddress);
		unsigned short startPMT2Entry = KernelSystem::extractPage2Part(originalSegment->startAddress);

		KernelSystem::PMT2Descriptor* clonedFirstDescAddress = &((*((*clonedProcess->pProcess->PMT1)[startPMT1Entry]))[startPMT2Entry]);
		KernelSystem::PMT2Descriptor* currentDesc = clonedFirstDescAddress;
		VirtualAddress blockVirtualAddress = originalSegment->startAddress + PAGE_SIZE;		// start from the second page

		for (PageNum i = 1; i < originalSegment->length; i++, blockVirtualAddress += PAGE_SIZE) {
			unsigned short pmt1Entry = KernelSystem::extractPage1Part(blockVirtualAddress);
			unsigned short pmt2Entry = KernelSystem::extractPage2Part(blockVirtualAddress);

			KernelSystem::PMT2Descriptor* next = &((*((*clonedProcess->pProcess->PMT1)[pmt1Entry]))[pmt2Entry]);
			currentDesc->next = next;										// perform chaining
			currentDesc = next;

		}
																			// then create new segment info for the cloned process
		SegmentInfo clonedSegmentInfo(originalSegment->startAddress, originalSegment->accessType, originalSegment->length, clonedFirstDescAddress);

		if (originalSegment->sharedSegmentName != "") {						// if the original segment is shared, this one is shared as well
			clonedSegmentInfo.sharedSegmentName = originalSegment->sharedSegmentName;
			KernelSystem::ReverseSegmentInfo revClonedSegInfo;
			revClonedSegInfo.process = clonedProcess->pProcess;
			revClonedSegInfo.firstDescriptor = clonedSegmentInfo.firstDescAddress;

			KernelSystem::SharedSegment* sharedSegment = &(system->sharedSegments.at(originalSegment->sharedSegmentName));

			sharedSegment->numberOfProcessesSharing++;
			sharedSegment->processesSharing.push_back(revClonedSegInfo);
		}

		clonedProcess->pProcess->segments.push_back(clonedSegmentInfo);		// no need to insert sorted because the original segments are sorted
	}

	system->activeProcesses.insert(std::pair<ProcessId, Process*>(system->processIDGenerator - 1, clonedProcess));

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
	newSegmentInfo.sharedSegmentName = name;

	segments.insert(std::upper_bound(segments.begin(), segments.end(), newSegmentInfo, []
	(const SegmentInfo& seg1, SegmentInfo& seg2) {
		return seg1.startAddress < seg2.startAddress;
	}), newSegmentInfo);															// insert into the segment list sorted by startAddress

	return OK;
}

Status KernelProcess::disconnectSharedSegment(const char* name) {					// works like deleteSegment() but doesn't affect the shared segment, memory or disk

	KernelSystem::SharedSegment* sharedSegment;
	try {
		sharedSegment = &(system->sharedSegments.at(std::string(name)));			// check for the key but don't insert if nonexistant 
	}																				// (that is what unordered_map::operator[] would do)
	catch (std::out_of_range noProcessWithPID) {
		return TRAP;																// cannot disconnect from a shared segment that doesn't exist
	}

	// check if a segment is connected to the shared segment

	for (auto processInfo = sharedSegment->processesSharing.begin(); processInfo != sharedSegment->processesSharing.end(); processInfo++) {
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

			sharedSegment->numberOfProcessesSharing--;								// decrease counter of processes sharing
			sharedSegment->processesSharing.erase(processInfo);						// remove this process from the list of processes sharing the segment

			return OK;
		}
	}

	return TRAP;																	// shared segment with this name exists but this process isn't connected to it
}

Status KernelProcess::deleteSharedSegment(const char* name) {						// any process can request a shared segment deletion

	system->mutex.lock();

	KernelSystem::SharedSegment* sharedSegment;
	try {
		sharedSegment = &(system->sharedSegments.at(std::string(name)));			// check for the key but don't insert if nonexistant 
	}
	catch (std::out_of_range noProcessWithPID) {
		system->mutex.unlock();
		return TRAP;																// cannot delete a shared segment that doesn't exist
	}

	// shared segment found -- delete PMT2s for all segments, all segment infos from respective processes, then delete PMT1+PMT2s+memory+disk for the shared segment

	for (auto processInfo = sharedSegment->processesSharing.begin(); processInfo != sharedSegment->processesSharing.end(); processInfo++) {
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
	}

	while (sharedSegment->processesSharing.size() > 0) {							// empty all processes from shared segment
		sharedSegment->numberOfProcessesSharing--;									// decrease counter of processes sharing
		sharedSegment->processesSharing.pop_back();									// remove this process from the list of processes sharing the segment
	}

	// delete PMT1+PMT2s+memory+disk for the shared segment

	for (unsigned short i = 0; i < sharedSegment->length; i++) {					// go through all PMT2s and deallocate descriptors, memory and disk
		unsigned short sharedPMT1Entry = i / KernelSystem::PMT2Size;
		unsigned short sharedPMT2Entry = i % KernelSystem::PMT2Size;

		KernelSystem::PMT2* pmt2 = (*(sharedSegment->pmt1))[sharedPMT1Entry];

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

	for (unsigned short i = 0; i < sharedSegment->pmt2Number; i++) {				// free PMT2 tables for the shared segment
		system->freePMTSlot((PhysicalAddress)(*(sharedSegment->pmt1))[i]);
	}
	system->freePMTSlot((PhysicalAddress)sharedSegment->pmt1);						// free PMT1 table for the shared segment

	system->sharedSegments.erase(std::string(name));								// erase the shared segment from the system's map

	system->mutex.unlock();
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

		// if it's a cloned page the cloning PMT2s, memory and disk can be declared as free if this is the last process pointing to it

		if (temp->getCloned()) {
			KernelSystem::PMT2Descriptor* cloningDesc = (KernelSystem::PMT2Descriptor*)temp->getBlock();

																					// find the cloning PMT2 and decrease counters
			unsigned cloningKey = temp->getDisk();
			KernelSystem::PMT2DescriptorCounter* cloningPMT2Counter = &(system->activePMT2Counter.at(cloningKey));

																					// find counter to decrease
			unsigned pmt2entry = KernelSystem::extractPage2Part(tempAddress);
			auto counterToDecrease = std::find_if(cloningPMT2Counter->sourceDescriptorCounters.begin(),
				cloningPMT2Counter->sourceDescriptorCounters.end(),
				[pmt2entry](std::pair<unsigned, unsigned>& pair) { return pair.first == pmt2entry; });

			counterToDecrease->second--;											// decrease the counter
			if (counterToDecrease->second == 0) {									// if it reached zero, remove the descriptor counter and adjust PMT2 counter

																					// it reaching zero means that for this descriptor it's possible to free memory and disk
				if (cloningDesc->getV()) {											// if the page is in memory, declare the block as free
					system->setFreeBlock(cloningDesc->block);
				}

				if (cloningDesc->getHasCluster()) {									// if the page is saved on disk, declare the cluster as free
					system->diskManager->freeCluster(cloningDesc->disk);
				}

				cloningPMT2Counter->sourceDescriptorCounters.erase(counterToDecrease);
				cloningPMT2Counter->counter--;
				if (cloningPMT2Counter->counter == 0) {								// if the cloning PMT2 is not pointed to at all anymore, deallocate it
					system->freePMTSlot((PhysicalAddress)cloningPMT2Counter->pmt2StartAddress);
					system->activePMT2Counter.erase(cloningKey);
				}
			}
		}

		if (!temp->getShared() && !temp->getCloned()) {									// only free memory and disk if it's not a shared page
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