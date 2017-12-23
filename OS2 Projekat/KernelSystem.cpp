#include <algorithm>
#include <iterator>

#include "DiskManager.h"
#include "KernelSystem.h"
#include "System.h"
#include "KernelProcess.h"
#include "Process.h"
#include "part.h"
#include "vm_declarations.h"

KernelSystem::KernelSystem(PhysicalAddress processVMSpace, PageNum processVMSpaceSize, 
	PhysicalAddress pmtSpace, PageNum pmtSpaceSize, Partition* partition) {

	this->processVMSpace = processVMSpace;									// initialise memory containing physical blocks
	this->processVMSpaceSize = processVMSpaceSize;
	
	this->freeBlocksHead = processVMSpace;									// assign head pointers
	this->freePMTSlotHead = pmtSpace;

	referenceRegisters = new ReferenceRegister[processVMSpaceSize];			// create reference registers

	diskManager = new DiskManager(partition);								// create the manager for the partition

	this->numberOfFreePMTSlots = pmtSpaceSize;
																			// initialise lists
	unsigned* blocksTemp = (unsigned*)freeBlocksHead, *pmtTemp = (unsigned*)freePMTSlotHead;	
	for (int i = 0; i < (processVMSpaceSize <= pmtSpaceSize ? pmtSpaceSize : processVMSpaceSize); i++) {
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
			if (i == pmtSpaceSize) {										// pmt list
				*pmtTemp = 0;
			}
			else {
				*pmtTemp = (unsigned)((char*)pmtTemp + 1024);
				pmtTemp = (unsigned*)((char*)pmtTemp + 1024);
			}
		}
	}

	this->pmtSpace = pmtSpace;												// initialise memory for the page map tables
	this->pmtSpaceSize = pmtSpaceSize;
}

KernelSystem::~KernelSystem() {
	delete[] referenceRegisters;
	delete diskManager;
}

Process* KernelSystem::createProcess() {

	if (!numberOfFreePMTSlots) return nullptr;								// no space for a new PMT1 at the moment

	Process* newProcess = new Process(processIDGenerator++);

	newProcess->pProcess->system = this;

	newProcess->pProcess->PMT1 = (PMT1*)getFreePMTSlot();					// grab a free PMT slot for the PMT1
	if (!newProcess->pProcess->PMT1) {
		delete newProcess;
		return nullptr;														// this exception should never occur
	}

	for (unsigned short i = 0; i < PMT1Size; i++) {							// initialise all of its pointers to nullptr
		(*(newProcess->pProcess->PMT1))[i] = nullptr;
	}
																			// add the new process to the hash map
	activeProcesses.insert(std::pair<ProcessId, Process*>(processIDGenerator - 1, newProcess));	

	// do other things if needed

	return newProcess;
}

Time KernelSystem::periodicJob() {											// shift reference bit into reference bits

	for (PageNum i = 0; i < processVMSpaceSize; i++) {						// shift each reference bit into that block's register
		if (referenceRegisters[i].pageDescriptor) {							// only if there is a page in that block slot
			referenceRegisters[i].value >>= 1;
			referenceRegisters[i].value |= referenceRegisters[i].pageDescriptor->getReferenced() << sizeof(unsigned) * 8;
			if (referenceRegisters[i].pageDescriptor->getReferenced())
				referenceRegisters[i].pageDescriptor->resetReferenced();
		}
	}

	return 10;																// 10ms period

}


Status KernelSystem::access(ProcessId pid, VirtualAddress address, AccessType type) {

	// Page1 - 8 bits ; Page2 - 6 bits ; Word - 10 bits
	
	Process* wantedProcess = nullptr;
	try {
		wantedProcess = activeProcesses.at(pid);							// check for the key but don't insert if nonexistant 
	}																		// (that is what unordered_map::operator[] would do)
	catch (std::out_of_range noProcessWithPID) {
		return TRAP;
	}

	PMT2Descriptor* pageDescriptor = getPageDescriptor(wantedProcess->pProcess, address);
	if (!pageDescriptor) return PAGE_FAULT;									// if PMT2 isn't created

	if (!pageDescriptor->getInUse()) return TRAP;							// attempted access of address that doesn't belong to any segment

	if (!pageDescriptor->getV())											// the page isn't loaded in memory -- return page fault
		return PAGE_FAULT;
	else {
		pageDescriptor->setReferenced();									// the page has been accessed in this period -- set the ref bit

		switch (type) {														// check access rights
		case READ:
			if (!pageDescriptor->getRd()) return TRAP;
			break;
		case WRITE:
			if (!pageDescriptor->getWr()) return TRAP;
			pageDescriptor->setD();											// indicate that the page is dirty
			break;
		case READ_WRITE:
			if (!pageDescriptor->getRd() || !pageDescriptor->getWr()) return TRAP;
			break;
		case EXECUTE:
			if (!pageDescriptor->getEx()) return TRAP;
			break;
		}
		return OK;															// page is in memory and the operation is allowed
	}

}



// private methods



KernelSystem::PMT2Descriptor* KernelSystem::getPageDescriptor(const KernelProcess* process, VirtualAddress address) {
	unsigned page1Part = 0;													// extract parts of the virtual address	
	unsigned page2Part = 0;
	unsigned wordPart = 0;

	for (VirtualAddress mask = 1, unsigned i = 0; i < usefulBitLength; i++) {
		if (i < wordPartBitLength) {
			wordPart |= address & mask;
		}
		else if (i < wordPartBitLength + page2PartBitLength) {
			page2Part |= address & mask >> wordPartBitLength;
		}
		else {
			page1Part |= address & mask >> (wordPartBitLength + page2PartBitLength);
		}
		mask <<= 1;
	}

	PMT1* pmt1 = process->PMT1;												// access the PMT1 of the process
	PMT2* pmt2 = (*pmt1)[page1Part];										// attempt access to a PMT2 pointer

	if (!pmt2) return nullptr;
	else return &(*pmt2)[page2Part];										// access the targetted descriptor

}

KernelSystem::PMT2Descriptor* KernelSystem::allocateDescriptors(KernelProcess* process, VirtualAddress startAddress,
										PageNum segmentSize, AccessType flags, bool load, void* content) {

	struct EntryCreationHelper {													// helper struct for transcation reasons
		unsigned short pmt1Entry;													// entry in pmt1
		unsigned short pmt2Entry;													// entry in pmt2
	};

	std::vector<unsigned short> missingPMT2s;										// remember indices of PMT2 tables that need to be allocated
	std::vector<EntryCreationHelper> entries;										// contains info of all pages that will be loaded

	for (PageNum i = 0; i < segmentSize; i++) {										// document PMT2 descriptors
		VirtualAddress blockVirtualAddress = startAddress + i * PAGE_SIZE;
		EntryCreationHelper entry;
		unsigned short pmt1Entry = 0, pmt2Entry = 0;								// extract relevant parts of the address

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

		PMT2* pmt2 = (*(process->PMT1))[pmt1Entry];			     					// access the PMT2 pointer
		if (!pmt2 && !std::binary_search(missingPMT2s.begin(), missingPMT2s.end(), pmt1Entry)) {
			missingPMT2s.push_back(pmt1Entry);										// if that pmt2 table doesn't exist yet, add it to the miss list
			if (missingPMT2s.size() > numberOfFreePMTSlots) return nullptr;			// surely insufficient number of slots in PMT memory
		}
	}

	PageNum pageOffsetCounter = 0;													// create descriptor for each page, allocate pmt2 if needed
	KernelSystem::PMT2Descriptor* firstDescriptor = nullptr, *temp = nullptr;

	for (auto entry = entries.begin(); entry != entries.end(); entry++) {			// create all documented descriptors

		KernelSystem::PMT2* pmt2 = (*(process->PMT1))[entry->pmt1Entry];

		unsigned pageKey = simpleHash(process->id, entry->pmt1Entry);				// key used to access the PMT2 descriptor counter hash table

		if (!pmt2) {																// if the PMT2 table doesn't exist, create it
			pmt2 = (KernelSystem::PMT2*)getFreePMTSlot();
			if (!pmt2) return nullptr;												// this exception should never happen (number of free PMT slots was checked in previous loop)

			PMT2DescriptorCounter newPMT2Counter(pmt2);								// add new PMT2 to the system's PMT2 descriptor counter
			activePMT2Counter.insert(std::pair<unsigned, KernelSystem::PMT2DescriptorCounter>(pageKey, newPMT2Counter));

		}

		activePMT2Counter[pageKey].counter++;										// a new descriptor is being added to this PMT2 -- increase the counter

		KernelSystem::PMT2Descriptor* pageDescriptor = &(*pmt2)[entry->pmt2Entry];	// access the targetted descriptor
		if (!pageOffsetCounter) {													// chain it
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

	return firstDescriptor;															// operation was successful -- return address of the first descriptor
}

PhysicalAddress KernelSystem::getSwappedBlock() {							// this function always returns a block from the list, nullptr if no space on disk

	PMT2Descriptor* victimThatHasCluster, *victimHasNoCluster;
	PageNum victimHasIndex = -1, victimHasNoIndex = -1;						// only compared if there is no room for a new write to the disk

	PMT2Descriptor* victim = referenceRegisters[0].pageDescriptor;
	PageNum victimIndex = 0;
	for (PageNum i = 1; i < processVMSpaceSize; i++) {						// find victim
		if (referenceRegisters[i].value < referenceRegisters[victimIndex].value) {
			victimIndex = i;
			victim = referenceRegisters[i].pageDescriptor;
		}
	}

	// return nullptr if no space for write on disk (in case of CreateSegment)

	referenceRegisters[victimIndex].value = 0;								// reset history bits to zero

	if (victim->getD()) {													// write the block to the disk if it's dirty
		if (victim->getHasCluster())										// if the page already has a reserved cluster on the disk, write contents there
			diskManager->writeToCluster(victim->getBlock(), victim->getDisk());
		else {																// if not, attempt to find an empty slot
			victim->setDisk(diskManager->write(victim->block));				
			if (victim->getDisk() == -1)
				return nullptr;												// no room on the disk or error while writing
		}
		victim->resetD();
	}

	victim->resetV();														// the page is no longer in memory, set valid to zero
	return victim->getBlock();												// return the address of the block the victim had
}

PhysicalAddress KernelSystem::getFreeBlock() {
	if (!freeBlocksHead) return nullptr;

	PhysicalAddress block = freeBlocksHead;									// retrieve the free block
	freeBlocksHead = (PhysicalAddress)(*(unsigned*)(block));				// move the free blocks head onto the next free block in the list
	
	return block;
}

void KernelSystem::setFreeBlock(PhysicalAddress newFreeBlock) {
	unsigned* block = (unsigned*)newFreeBlock;

	*block = (unsigned)((char*)freeBlocksHead);								// chain the new block as the new first element of the list
	freeBlocksHead = block;
}

PhysicalAddress KernelSystem::getFreePMTSlot() {
	if (!numberOfFreePMTSlots) return nullptr;

	PhysicalAddress freeSlot = freePMTSlotHead;								// assign a free block to the required PMT1/PMT2
	freePMTSlotHead = (PhysicalAddress)(*((unsigned*)freePMTSlotHead));		// move the pmt list head

	numberOfFreePMTSlots--;													// decrease the number of free slots

	return freeSlot;
}

void KernelSystem::freePMTSlot(PhysicalAddress slotAddress) {
	unsigned* slot = (unsigned*)slotAddress;

	*slot = (unsigned)((char*)freePMTSlotHead);								// chain the new slot as the new first element of the list
	freePMTSlotHead = slot;

	numberOfFreePMTSlots++;													// increase number of free slots
}