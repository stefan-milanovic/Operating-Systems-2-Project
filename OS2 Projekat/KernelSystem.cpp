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

	this->processVMSpace = processVMSpace;								// initialise memory containing physical blocks
	this->processVMSpaceSize = processVMSpaceSize;

	this->clockHand = nullptr;											// assign head pointers
	this->freeBlocksHead = processVMSpace;
	this->freePMTSlotHead = pmtSpace;

	this->diskManager = new DiskManager(partition);						// create the manager for the partition

	this->numberOfFreePMTSlots = pmtSpaceSize;

	unsigned* blocksTemp = (unsigned*)freeBlocksHead, *pmtTemp = (unsigned*)freePMTSlotHead;	// initialise lists
	for (int i = 0; i < (processVMSpaceSize <= pmtSpaceSize ? pmtSpaceSize : processVMSpaceSize); i++) {
		if (i < processVMSpaceSize) {												// block list
			if (i == processVMSpaceSize - 1) {
				*blocksTemp = 0;
			}
			else {
				*blocksTemp = (unsigned)((char*)blocksTemp + 1024);
				blocksTemp = (unsigned*)((char*)blocksTemp + 1024);
			}
		}
		if (i < pmtSpaceSize) {
			if (i == pmtSpaceSize) {												// pmt list
				*pmtTemp = 0;
			}
			else {
				*pmtTemp = (unsigned)((char*)pmtTemp + 1024);
				pmtTemp = (unsigned*)((char*)pmtTemp + 1024);
			}
		}
	}

	this->pmtSpace = pmtSpace;											// initialise memory for the page map tables
	this->pmtSpaceSize = pmtSpaceSize;
}

KernelSystem::~KernelSystem() {
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

Time KernelSystem::periodicJob() {}


Status KernelSystem::access(ProcessId pid, VirtualAddress address, AccessType type) {

	// Page1 - 8 bits ; Page2 - 6 bits ; Word - 10 bits
	
	Process* wantedProcess = nullptr;
	try {
		wantedProcess = activeProcesses.at(pid);								// check for the key but don't insert if nonexistant 
	}																			// (that is what unordered_map::operator[] would do)
	catch (std::out_of_range noProcessWithPID) {
		return TRAP;
	}

	PMT2Descriptor* pageDescriptor = getPageDescriptor(wantedProcess->pProcess, address);
	if (!pageDescriptor) return PAGE_FAULT;										// if PMT2 isn't created

	if (!pageDescriptor->getV())												// the page isn't loaded in memory -- return page fault
		return PAGE_FAULT;
	else {
		switch (type) {															// check access rights
		case READ:
			if (!pageDescriptor->getRd()) return TRAP;
			break;
		case WRITE:
			if (!pageDescriptor->getWr()) return TRAP;
			pageDescriptor->setD();												// indicate that the page is dirty
			break;
		case READ_WRITE:
			if (!pageDescriptor->getRd() || !pageDescriptor->getWr()) return TRAP;
			break;
		case EXECUTE:
			if (!pageDescriptor->getEx()) return TRAP;
			break;
		}
		return OK;																// page is in memory and the operation is allowed
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

PhysicalAddress KernelSystem::getSwappedBlock() {							// this function always returns a block from the list

	const unsigned short numberOfPointers = 4;								
	PMT2Descriptor* candidates[numberOfPointers] = { nullptr };				// First descriptor candidate in each category

	PMT2Descriptor* startingPoint = clockHand;

	// check first descriptor
	for (clockHand = clockHand->next; clockHand != startingPoint; clockHand = clockHand->next) {
		// check all the rest

	}

	PMT2Descriptor* victim;
	for (int i = 0; i < numberOfPointers; i++) {
		if (candidates[i] != nullptr) {
			victim = candidates[i];
			break;
		}
	}

	if (victim->getD()) {													// write the block to the disk if it's dirty
		diskManager->writeToCluster(victim->getBlock(), victim->getDisk());
	}

	victim->resetV();														// the page is no longer in memory, set valid to zero
	return victim->getBlock();												// return the address of the block the victim had
}

void KernelSystem::addDescriptorToClockhandList(PMT2Descriptor* pageDescriptor) {

	if (!clockHand) {														// chain the descriptor in the clockhand list
		clockHand = pageDescriptor;
		pageDescriptor->next = pageDescriptor;
	}
	else {
		pageDescriptor->next = clockHand->next;
		clockHand->next = pageDescriptor;
		clockHand = clockHand->next;
	}
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