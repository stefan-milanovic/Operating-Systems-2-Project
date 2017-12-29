#include <algorithm>
#include <iterator>
#include <mutex>
#include <cmath>
#include <string>

#include "DiskManager.h"
#include "KernelSystem.h"
#include "System.h"
#include "KernelProcess.h"
#include "Process.h"
#include "part.h"
#include "vm_declarations.h"

KernelSystem::KernelSystem(PhysicalAddress processVMSpace_, PageNum processVMSpaceSize_, 
	PhysicalAddress pmtSpace_, PageNum pmtSpaceSize_, Partition* partition_) {

	processVMSpace = processVMSpace_;										// initialise info about physical blocks
	processVMSpaceSize = processVMSpaceSize_;
	
	pmtSpace = pmtSpace_;													// initialise info about PMT blocks 
	pmtSpaceSize = pmtSpaceSize_;

	freeBlocksHead = processVMSpace_;										// assign head pointers
	freePMTSlotHead = pmtSpace_;

	referenceRegisters = new ReferenceRegister[processVMSpaceSize];			// create reference registers

	diskManager = new DiskManager(partition_);								// create the manager for the partition

	this->numberOfFreePMTSlots = pmtSpaceSize;
																			// initialise lists
	unsigned* blocksTemp = (unsigned*)freeBlocksHead, *pmtTemp = (unsigned*)freePMTSlotHead;	
	for (PageNum i = 0; i < (processVMSpaceSize <= pmtSpaceSize ? pmtSpaceSize : processVMSpaceSize); i++) {
		if (i < processVMSpaceSize) {										// block list
			if (i == processVMSpaceSize - 1) {
				*blocksTemp = 0;
			}
			else {
				*blocksTemp = (unsigned)((char*)blocksTemp + 1024);
				blocksTemp = (unsigned*)((char*)blocksTemp + 1024);
			}
		}
		if (i < pmtSpaceSize) {
			if (i == pmtSpaceSize - 1) {									// PMT slot list
				*pmtTemp = 0;
			}
			else {
				*pmtTemp = (unsigned)((char*)pmtTemp + 1024);
				pmtTemp = (unsigned*)((char*)pmtTemp + 1024);
			}
		}
	}

}

KernelSystem::~KernelSystem() {

	delete[] referenceRegisters;
	delete diskManager;
}

Process* KernelSystem::createProcess() {

	mutex.lock();

	if (!numberOfFreePMTSlots) { mutex.unlock(); return nullptr; }					// no space for a new PMT1 at the moment

	Process* newProcess = new Process(processIDGenerator++);

	newProcess->pProcess->system = this;

	newProcess->pProcess->PMT1 = (PMT1*)getFreePMTSlot();					// grab a free PMT slot for the PMT1
	if (!newProcess->pProcess->PMT1) {
		delete newProcess;
		mutex.unlock();
		return nullptr;														// this exception should never occur
	}

	for (unsigned short i = 0; i < PMT1Size; i++) {							// initialise all of its pointers to nullptr
		(*(newProcess->pProcess->PMT1))[i] = nullptr;
	}
																			// add the new process to the hash map
	activeProcesses.insert(std::pair<ProcessId, Process*>(processIDGenerator - 1, newProcess));	

	// do other things if needed

	mutex.unlock();

	return newProcess;
}

Time KernelSystem::periodicJob() {											// shift reference bit into reference bits

	for (PageNum i = 0; i < processVMSpaceSize; i++) {						// shift each reference bit into that block's register
		if (referenceRegisters[i].pageDescriptor) {							// only if there is a page in that block slot
			referenceRegisters[i].value >>= 1;
			referenceRegisters[i].value |= (referenceRegisters[i].pageDescriptor->getReferenced() ? 1U : 0U) << (sizeof(unsigned) * 8 - 1);
			if (referenceRegisters[i].pageDescriptor->getReferenced())
				referenceRegisters[i].pageDescriptor->resetReferenced();
		}
	}

	return 10;																// 10ms period

}


Status KernelSystem::access(ProcessId pid, VirtualAddress address, AccessType type) {
	mutex.lock();

	Process* wantedProcess = nullptr;
	try {
		wantedProcess = activeProcesses.at(pid);							// check for the key but don't insert if nonexistant 
	}																		// (that is what unordered_map::operator[] would do)
	catch (std::out_of_range noProcessWithPID) {
		mutex.unlock();
		return TRAP;
	}

	PMT2Descriptor* pageDescriptor = getPageDescriptor(wantedProcess->pProcess, address);
	if (!pageDescriptor) { mutex.unlock(); return PAGE_FAULT; }				// if PMT2 isn't created

	if (!pageDescriptor->getInUse()) { mutex.unlock(); return TRAP; }		// attempted access of address that doesn't belong to any segment

	if (pageDescriptor->getShared())										// if this page is of a shared segment, switch to the appropriate descriptor
		pageDescriptor = (PMT2Descriptor*)pageDescriptor->getBlock();

	if (!pageDescriptor->getV()) {											// the page isn't loaded in memory -- return page fault
		mutex.unlock();
		return PAGE_FAULT;
	}
	else {
		pageDescriptor->setReferenced();									// the page has been accessed in this period -- set the ref bit

		switch (type) {														// check access rights
		case READ:
			if (!pageDescriptor->getRd()) { mutex.unlock(); return TRAP; }
			break;
		case WRITE:
			if (!pageDescriptor->getWr()) { mutex.unlock(); return TRAP; }
			pageDescriptor->setD();											// indicate that the page is dirty
			break;
		case READ_WRITE:
			if (!pageDescriptor->getRd() || !pageDescriptor->getWr()) { mutex.unlock(); return TRAP; }
			break;
		case EXECUTE:
			if (!pageDescriptor->getEx()) { mutex.unlock(); return TRAP; }
			break;
		}
		mutex.unlock();
		return OK;															// page is in memory and the operation is allowed
	}

}

Process* KernelSystem::cloneProcess(ProcessId pid) {
	mutex.lock();

	Process* wantedProcess = nullptr;										// try and find target process for cloning
	try {
		wantedProcess = activeProcesses.at(pid);							// check for the key but don't insert if nonexistant 
	}																		// (that is what unordered_map::operator[] would do)
	catch (std::out_of_range noProcessWithPID) {
		mutex.unlock();
		return nullptr;
	}

	// TODO: check how much space wantedProcess has and see if it's replicable (both in memory and disk)

	PageNum wantedProcessCurrentSpace = 1;

	// If everything's ok, clone
	mutex.unlock();
	return wantedProcess->clone(processIDGenerator++);
}

// private methods



KernelSystem::PMT2Descriptor* KernelSystem::getPageDescriptor(const KernelProcess* process, VirtualAddress address) {
	unsigned page1Part = 0;													// extract parts of the virtual address	
	unsigned page2Part = 0;

	page1Part = KernelSystem::extractPage1Part(address);
	page2Part = KernelSystem::extractPage2Part(address);

	PMT1* pmt1 = process->PMT1;												// access the PMT1 of the process
	PMT2* pmt2 = (*pmt1)[page1Part];										// attempt access to a PMT2 pointer

	if (!pmt2) return nullptr;
	else return &(*pmt2)[page2Part];										// access the targetted descriptor

}

KernelSystem::PMT2Descriptor* KernelSystem::allocateDescriptors(KernelProcess* process, VirtualAddress startAddress,
										PageNum segmentSize, AccessType flags, bool load, void* content) {

	mutex.lock();

	struct EntryCreationHelper {													// helper struct for transcation reasons
		unsigned short pmt1Entry;													// entry in pmt1
		unsigned short pmt2Entry;													// entry in pmt2
	};

	std::vector<unsigned short> missingPMT2s;										// remember indices of PMT2 tables that need to be allocated
	std::vector<EntryCreationHelper> entries;										// contains info of all pages that will be loaded

	for (PageNum i = 0; i < segmentSize; i++) {										// document PMT2 descriptors
		VirtualAddress blockVirtualAddress = startAddress + i * PAGE_SIZE;
		EntryCreationHelper entry;													// extract relevant parts of the address

		entry.pmt1Entry = KernelSystem::extractPage1Part(blockVirtualAddress);
		entry.pmt2Entry = KernelSystem::extractPage2Part(blockVirtualAddress);

		entries.push_back(entry);

		PMT2* pmt2 = (*(process->PMT1))[entry.pmt1Entry];			     			// access the PMT2 pointer
		if (!pmt2 && !std::binary_search(missingPMT2s.begin(), missingPMT2s.end(), entry.pmt1Entry)) {
			missingPMT2s.push_back(entry.pmt1Entry);								// if that pmt2 table doesn't exist yet, add it to the miss list
			if (missingPMT2s.size() > numberOfFreePMTSlots) {
				mutex.unlock();
				return nullptr;														// surely insufficient number of slots in PMT memory
			}
		}
	}

	PageNum pageOffsetCounter = 0;													// create descriptor for each page, allocate pmt2 if needed
	PMT2Descriptor* firstDescriptor = nullptr, *temp = nullptr;

	for (auto entry = entries.begin(); entry != entries.end(); entry++) {			// create all documented descriptors

		PMT2* pmt2 = (*(process->PMT1))[entry->pmt1Entry];

		unsigned pageKey = simpleHash(process->id, entry->pmt1Entry);				// key used to access the PMT2 descriptor counter hash table

		if (!pmt2) {																// if the PMT2 table doesn't exist, create it
			pmt2 = (*(process->PMT1))[entry->pmt1Entry] = (PMT2*)getFreePMTSlot();
			initialisePMT2(pmt2);
			if (!pmt2) { mutex.unlock(); return nullptr; }							// this exception should never happen (number of free PMT slots was checked in previous loop)

			PMT2DescriptorCounter newPMT2Counter(pmt2);								// add new PMT2 to the system's PMT2 descriptor counter
			activePMT2Counter.insert(std::pair<unsigned, PMT2DescriptorCounter>(pageKey, newPMT2Counter));

		}

		activePMT2Counter[pageKey].counter++;										// a new descriptor is being added to this PMT2 -- increase the counter

		PMT2Descriptor* pageDescriptor = &(*pmt2)[entry->pmt2Entry];				// access the targetted descriptor
		if (!firstDescriptor) {														// chain it
			firstDescriptor = pageDescriptor;
			temp = firstDescriptor;
		}
		else {
			temp->next = pageDescriptor;
			temp = temp->next;
		}

		pageDescriptor->setInUse();													// set that the descriptor is now in use
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

		if (load) {																	// if loadSegment() is being called, load content
			void* pageContent = (void*)((char*)content + pageOffsetCounter++ * PAGE_SIZE);
			pageDescriptor->setDisk(diskManager->write(pageContent));				// the disk manager's write() returns the cluster number
			pageDescriptor->setHasCluster();										// the page's location on the partition is known
		}
		else {
			pageDescriptor->resetHasCluster();										// the page does not have a reserved cluster on the disk yet
		}
	}

	mutex.unlock();
	return firstDescriptor;															// operation was successful -- return address of the first descriptor
}

KernelSystem::PMT2Descriptor* KernelSystem::connectToSharedSegment(KernelProcess* process, VirtualAddress startAddress,
	PageNum segmentSize, const char* name, AccessType flags) {

	mutex.lock();

	SharedSegment* sharedSegment;														// check if a shared segment with that name already exists

	struct EntryCreationHelper {														// helper struct for transcation reasons
		unsigned short pmt1Entry;														// entry in pmt1
		unsigned short pmt2Entry;														// entry in pmt2
	};

	std::vector<unsigned short> missingPMT2s;											// remember indices of PMT2 tables that need to be allocated
	std::vector<EntryCreationHelper> entries;											// contains info of all pages that will be loaded

	try {																				
		sharedSegment = &(sharedSegments.at(std::string(name)));						// check for the key but don't insert if nonexistant 
	}																					
	catch (std::out_of_range sharedSegmentDoesntExist) {
																						// shared segment doesn't exist -- create it, then connect
																						// (if there's space for all the needed page tables)

																						// for the shared segment: 1xPMT1 + needed, fixed amount of PMT2s
		unsigned short sharedSegmentRequiredPMTs = 1 + (unsigned short)ceil((double)segmentSize / PMT2Size);


		for (PageNum i = 0; i < segmentSize; i++) {										// document PMT2 descriptors
			VirtualAddress blockVirtualAddress = startAddress + i * PAGE_SIZE;
			EntryCreationHelper entry;													// extract relevant parts of the address

			entry.pmt1Entry = KernelSystem::extractPage1Part(blockVirtualAddress);
			entry.pmt2Entry = KernelSystem::extractPage2Part(blockVirtualAddress);

			entries.push_back(entry);

			PMT2* pmt2 = (*(process->PMT1))[entry.pmt1Entry];			     			// access the PMT2 pointer
			if (!pmt2 && !std::binary_search(missingPMT2s.begin(), missingPMT2s.end(), entry.pmt1Entry)) {
				missingPMT2s.push_back(entry.pmt1Entry);								// if that pmt2 table doesn't exist yet, add it to the miss list
				if (missingPMT2s.size() + sharedSegmentRequiredPMTs > numberOfFreePMTSlots) {		// also count the required PMT for the shared segment		
					mutex.unlock();
					return nullptr;														// surely insufficient number of slots in PMT memory
				}
			}
		}

		SharedSegment newSharedSegment;
		newSharedSegment.length = segmentSize;											// initialise the new shared segment
		newSharedSegment.pmt2Number = (unsigned short)ceil((double)segmentSize / PMT2Size);
		newSharedSegment.accessType = flags;
		newSharedSegment.numberOfProcessesSharing = 0;
		newSharedSegment.name = std::string(name);
		newSharedSegment.pmt1 = (PMT1*)getFreePMTSlot();

		for (unsigned short i = 0; i < PMT1Size; i++) {									// initialise all of its pointers to nullptr
			(*(newSharedSegment.pmt1))[i] = nullptr;
		}

																						// add the shared segment to the system's shared segment map
		sharedSegments.insert(std::pair<std::string, SharedSegment>(newSharedSegment.name, newSharedSegment));

		sharedSegment = &(sharedSegments.at(std::string(name)));						// check for the key but don't insert if nonexistant 

		PMT2Descriptor* sharedFirstDescriptor = nullptr, *sharedTemp = nullptr;
		for (unsigned short i = 0; i < sharedSegment->length; i++) {						// allocate PMT2s for shared segment and initialise descriptors
			unsigned short sharedPMT1Entry = i / PMT2Size;
			unsigned short sharedPMT2Entry = i % PMT2Size;

			PMT2* pmt2 = (*(sharedSegment->pmt1))[sharedPMT1Entry];

			if (!pmt2) {
				pmt2 = (*(sharedSegment->pmt1))[sharedPMT1Entry] = (PMT2*)getFreePMTSlot();
				initialisePMT2(pmt2);
			}
			PMT2Descriptor* pageDescriptor = &(*pmt2)[sharedPMT2Entry];					// access the targetted descriptor
			if (!sharedFirstDescriptor) {												// chain it
				sharedFirstDescriptor = pageDescriptor;
				sharedTemp = sharedFirstDescriptor;
			}
			else {
				sharedTemp->next = pageDescriptor;
				sharedTemp = sharedTemp->next;
			}

			pageDescriptor->setInUse();
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
			pageDescriptor->resetHasCluster();											// the page does not have a reserved cluster on the disk yet
		}

		PageNum pageOffsetCounter = 0;													// create descriptor for each page, allocate pmt2 if needed
		PMT2Descriptor* firstDescriptor = nullptr, *temp = nullptr;

		for (auto entry = entries.begin(); entry != entries.end(); entry++) {			// create all documented descriptors

			PMT2* pmt2 = (*(process->PMT1))[entry->pmt1Entry];

			unsigned pageKey = simpleHash(process->id, entry->pmt1Entry);				// key used to access the PMT2 descriptor counter hash table

			if (!pmt2) {																// if the PMT2 table doesn't exist, create it
				pmt2 = (*(process->PMT1))[entry->pmt1Entry] = (PMT2*)getFreePMTSlot();
				initialisePMT2(pmt2);
				if (!pmt2) { mutex.unlock(); return nullptr; }							// this exception should never happen (number of free PMT slots was checked in previous loop)

				PMT2DescriptorCounter newPMT2Counter(pmt2);								// add new PMT2 to the system's PMT2 descriptor counter
				activePMT2Counter.insert(std::pair<unsigned, KernelSystem::PMT2DescriptorCounter>(pageKey, newPMT2Counter));

			}

			activePMT2Counter[pageKey].counter++;										// a new descriptor is being added to this PMT2 -- increase the counter

			PMT2Descriptor* pageDescriptor = &(*pmt2)[entry->pmt2Entry];				// access the targetted descriptor
			if (!firstDescriptor) {														// chain it
				firstDescriptor = pageDescriptor;
				temp = firstDescriptor;
			}
			else {
				temp->next = pageDescriptor;
				temp = temp->next;
			}

			pageDescriptor->setShared();												// this descriptor represents a shared page
			PhysicalAddress sharedPageDescriptorAddress;								// find the address for the adequate sharedsegment descriptor

			unsigned short sharedPMT1Entry = (unsigned short)pageOffsetCounter / PMT2Size;
			unsigned short sharedPMT2Entry = (unsigned short)pageOffsetCounter % PMT2Size;
			
			PMT2* sharedPMT2 = (*(sharedSegment->pmt1))[sharedPMT1Entry];
			sharedPageDescriptorAddress = (PhysicalAddress)(&((*sharedPMT2)[sharedPMT2Entry]));

			pageDescriptor->setBlock(sharedPageDescriptorAddress);						// set the _block_ pointer to it

			pageDescriptor->setInUse();													// set that the descriptor is now in use
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
			
			pageOffsetCounter++;
			pageDescriptor->resetHasCluster();											// the page does not have a reserved cluster on the disk yet

		}

		ReverseSegmentInfo revSegInfo;													// remember the process that has started sharing
		revSegInfo.firstDescriptor = firstDescriptor;
		revSegInfo.process = process;
		sharedSegment->numberOfProcessesSharing++;
		sharedSegment->processesSharing.push_back(revSegInfo);

		mutex.unlock();
		return firstDescriptor;
	}

																						// shared segment already exists -- only connect (if there's space)
	if (segmentSize > sharedSegment->length) {
		mutex.unlock();																	// a process may connect to a segment with an equal or lower segSize
		return nullptr;
	}

	switch (sharedSegment->accessType) {													// access rights to the shared segment have to match for all processes
	case READ:	
		if (!(flags == READ || flags == READ_WRITE)) {	mutex.unlock();	return nullptr; }
		break;
	case WRITE:
		if (!(flags == WRITE || flags == READ_WRITE)) { mutex.unlock(); return nullptr;	}
		break;
	case READ_WRITE:
		if (flags == EXECUTE) { mutex.unlock(); return nullptr; }
		break;
	case EXECUTE:
		if (flags != EXECUTE) { mutex.unlock(); return nullptr; }
	}

	for (PageNum i = 0; i < segmentSize; i++) {											// document PMT2 descriptors
		VirtualAddress blockVirtualAddress = startAddress + i * PAGE_SIZE;
		EntryCreationHelper entry;														// extract relevant parts of the address

		entry.pmt1Entry = KernelSystem::extractPage1Part(blockVirtualAddress);
		entry.pmt2Entry = KernelSystem::extractPage2Part(blockVirtualAddress);

		entries.push_back(entry);

		PMT2* pmt2 = (*(process->PMT1))[entry.pmt1Entry];			     				// access the PMT2 pointer
		if (!pmt2 && !std::binary_search(missingPMT2s.begin(), missingPMT2s.end(), entry.pmt1Entry)) {
			missingPMT2s.push_back(entry.pmt1Entry);									// if that pmt2 table doesn't exist yet, add it to the miss list
			if (missingPMT2s.size() > numberOfFreePMTSlots) {
				mutex.unlock();
				return nullptr;															// surely insufficient number of slots in PMT memory
			}
		}
	}

	PageNum pageOffsetCounter = 0;													// create descriptor for each page, allocate pmt2 if needed
	PMT2Descriptor* firstDescriptor = nullptr, *temp = nullptr;

	for (auto entry = entries.begin(); entry != entries.end(); entry++) {			// create all documented descriptors

		PMT2* pmt2 = (*(process->PMT1))[entry->pmt1Entry];

		unsigned pageKey = simpleHash(process->id, entry->pmt1Entry);				// key used to access the PMT2 descriptor counter hash table

		if (!pmt2) {																// if the PMT2 table doesn't exist, create it
			pmt2 = (*(process->PMT1))[entry->pmt1Entry] = (PMT2*)getFreePMTSlot();
			initialisePMT2(pmt2);
			if (!pmt2) { mutex.unlock(); return nullptr; }							// this exception should never happen (number of free PMT slots was checked in previous loop)

			PMT2DescriptorCounter newPMT2Counter(pmt2);								// add new PMT2 to the system's PMT2 descriptor counter
			activePMT2Counter.insert(std::pair<unsigned, KernelSystem::PMT2DescriptorCounter>(pageKey, newPMT2Counter));

		}

		activePMT2Counter[pageKey].counter++;										// a new descriptor is being added to this PMT2 -- increase the counter

		PMT2Descriptor* pageDescriptor = &(*pmt2)[entry->pmt2Entry];				// access the targetted descriptor
		if (!firstDescriptor) {														// chain it
			firstDescriptor = pageDescriptor;
			temp = firstDescriptor;
		}
		else {
			temp->next = pageDescriptor;
			temp = temp->next;
		}

		pageDescriptor->setShared();												// this descriptor represents a shared page
		PhysicalAddress sharedPageDescriptorAddress;								// find the address for the adequate sharedsegment descriptor

		unsigned short sharedPMT1Entry = (unsigned short)pageOffsetCounter / PMT2Size;
		unsigned short sharedPMT2Entry = (unsigned short)pageOffsetCounter % PMT2Size;

		PMT2* sharedPMT2 = (*(sharedSegment->pmt1))[sharedPMT1Entry];
		sharedPageDescriptorAddress = (PhysicalAddress)(&((*sharedPMT2)[sharedPMT2Entry]));

		pageDescriptor->setBlock(sharedPageDescriptorAddress);						// set the _block_ pointer to it

		pageDescriptor->setInUse();													// set that the descriptor is now in use
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
		pageOffsetCounter++;
		pageDescriptor->resetHasCluster();											// the page does not have a reserved cluster on the disk yet

	}

	ReverseSegmentInfo revSegInfo;													// remember the process that has begun sharing
	revSegInfo.firstDescriptor = firstDescriptor;
	revSegInfo.process = process;
	sharedSegment->numberOfProcessesSharing++;
	sharedSegment->processesSharing.push_back(revSegInfo);

	mutex.unlock();
	return firstDescriptor;
}

PhysicalAddress KernelSystem::getSwappedBlock() {									// this function always returns a block from the list, nullptr if no space on disk

	mutex.lock();

	PMT2Descriptor* victimHasCluster, *victimHasNoCluster;
	PageNum victimHasClusterIndex = -1, victimHasNoClusterIndex = -1;				// only compared if there is no room for a new write to the disk
		
	PMT2Descriptor* victim;
	PageNum victimIndex;

	for (PageNum i = 0; i < processVMSpaceSize; i++) {								// find victim
		if (referenceRegisters[i].pageDescriptor->getHasCluster()) {
			if (victimHasClusterIndex == -1) {
				victimHasCluster = referenceRegisters[i].pageDescriptor;
				victimHasClusterIndex = i;
			}
			else {
				if (referenceRegisters[i].value < referenceRegisters[victimHasClusterIndex].value) {
					victimHasCluster = referenceRegisters[i].pageDescriptor;
					victimHasClusterIndex = i;
				}
			}
		}
		else {
			if (victimHasNoClusterIndex == -1) {
				victimHasNoCluster = referenceRegisters[i].pageDescriptor;
				victimHasNoClusterIndex = i;
			}
			else {
				if (referenceRegisters[i].value < referenceRegisters[victimHasNoClusterIndex].value) {
					victimHasNoCluster = referenceRegisters[i].pageDescriptor;
					victimHasNoClusterIndex = i;
				}
			}
		}
	}

	if (victimHasClusterIndex == -1 && victimHasNoClusterIndex == -1) {
		mutex.unlock();																// this should never be entered
		return nullptr;
	}
	else {
		if (!(victimHasClusterIndex != -1 && victimHasNoClusterIndex != -1)) {		// only pages of one type or the other are in memory
			if (victimHasClusterIndex != -1) {
				victim = victimHasCluster;
				victimIndex = victimHasClusterIndex;
			}
			else {
				victim = victimHasNoCluster;
				victimIndex = victimHasNoClusterIndex;
			}
		}
		else {																		// find minimal, reverse only if there is no room on disk at the moment
			if (referenceRegisters[victimHasClusterIndex].value <= referenceRegisters[victimHasNoClusterIndex].value) {
				victim = victimHasCluster;
				victimIndex = victimHasClusterIndex;
			}
			else {
				if (diskManager->hasEnoughSpace(1)) {								// if there's room on the disk, allow the minimal to reserve it
					victim = victimHasNoCluster;
					victimIndex = victimHasNoClusterIndex;
				}
				else {																// otherwise swap minimal with no disk for first minimal with disk
					victim = victimHasCluster;
					victimIndex = victimHasClusterIndex;
				}

			}
		}
	}

	referenceRegisters[victimIndex].value = 0;										// reset history bits of block to zero
																					// the pointer field is set in pageFault() after this function returns a block address

	if (victim->getD()) {															// write the block to the disk if it's dirty (always true for never-before-written-to-disk createSegment() pages)
		if (victim->getHasCluster())												// if the page already has a reserved cluster on the disk, write contents there
			diskManager->writeToCluster(victim->getBlock(), victim->getDisk());
		else {																		// if not, attempt to find an empty slot
			victim->setDisk(diskManager->write(victim->getBlock()));
			if (victim->getDisk() == -1) {
				mutex.unlock();
				return nullptr;														// no room on the disk or error while writing
			}
			victim->setHasCluster();												// the victim now has a cluster on the disk
		}
		victim->resetD();
	}

	victim->resetReferenced();														// if it was referenced, it might not immediately be on the next load
	victim->resetV();																// the page is no longer in memory, set valid to zero

	mutex.unlock();
	return victim->getBlock();														// return the address of the block the victim had
}

PhysicalAddress KernelSystem::getFreeBlock() {

	mutex.lock();

	if (!freeBlocksHead) { mutex.unlock(); return nullptr; }

	PhysicalAddress block = freeBlocksHead;											// retrieve the free block
	freeBlocksHead = (PhysicalAddress)(*(unsigned*)(block));						// move the free blocks head onto the next free block in the list
	
	mutex.unlock();
	return block;
}

void KernelSystem::setFreeBlock(PhysicalAddress newFreeBlock) {

	mutex.lock();
	unsigned* block = (unsigned*)newFreeBlock;

	*block = (unsigned)((char*)freeBlocksHead);										// chain the new block as the new first element of the list
	freeBlocksHead = block;
	mutex.unlock();
}

PhysicalAddress KernelSystem::getFreePMTSlot() {

	mutex.lock();

	if (!numberOfFreePMTSlots) { mutex.unlock(); return nullptr; }

	PhysicalAddress freeSlot = freePMTSlotHead;										// assign a free block to the required PMT1/PMT2
	freePMTSlotHead = (PhysicalAddress)(*((unsigned*)freePMTSlotHead));				// move the pmt list head

	numberOfFreePMTSlots--;															// decrease the number of free slots

	mutex.unlock();

	return freeSlot;
}

void KernelSystem::freePMTSlot(PhysicalAddress slotAddress) {

	mutex.lock();
	unsigned* slot = (unsigned*)slotAddress;

	*slot = (unsigned)((char*)freePMTSlotHead);										// chain the new slot as the new first element of the list
	freePMTSlotHead = slot;

	numberOfFreePMTSlots++;															// increase number of free slots
	mutex.unlock();
}

void KernelSystem::initialisePMT2(PMT2* pmt2) {
	for (unsigned short i = 0; i < PMT2Size; i++) {
		(*pmt2)[i].basicBits = (*pmt2)[i].advancedBits = 0;
		(*pmt2)[i].block = (*pmt2)[i].next = nullptr;
	}
}

// 64bit 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000
// 24bit va												   xxxx xxxx xxxx xxxx xxxx xxxx
// 8bit page 1 part										   pppp pppp
// 6bit page 2 part													 pppp pp
// 10bit word part                                                          pp pppp pppp

unsigned short KernelSystem::extractPage1Part(VirtualAddress address) {
	return (unsigned short)((address & 0x0000000000FF0000) >> 16);
}
unsigned short KernelSystem::extractPage2Part(VirtualAddress address) {
	return (unsigned short)((address & 0x000000000000FC00) >> 10);
}
unsigned short KernelSystem::extractWordPart(VirtualAddress address) {
	return (unsigned short)(address & 0x00000000000003FF);
}