#include <algorithm>
#include <iterator>

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

	this->numberOfFreeBlocks = processVMSpaceSize;

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

	this->partition = partition;										// assign the partition pointer

	this->clusterUsageVector = new ClusterNo(this->clusterUsageVectorSize = partition->getNumOfClusters()); // create cluster usage vector
	this->clusterUsageVectorHead = 0;
	for (int i = 0; i < clusterUsageVectorSize - 1; i++) {				// initialise it
		clusterUsageVector[i] = i + 1;
	}
	clusterUsageVector[clusterUsageVectorSize - 1] = -1;
	this->numberOfFreeClusters = this->clusterUsageVectorSize;			// assign number of free clusters

}

KernelSystem::~KernelSystem() {
	delete[] clusterUsageVector;
}

Process* KernelSystem::createProcess() {

	if (freePMTSlotHead) return nullptr;									// no space for a new PMT1 at the moment

	Process* newProcess = new Process(processIDGenerator++);

	newProcess->pProcess->PMT1 = (PMT1*)freePMTSlotHead;					// assign a free PMT page
	for (unsigned short i = 0; i < PMT1Size; i++) {							// initialise all of its pointers to nullptr
		(*(newProcess->pProcess->PMT1))[i] = nullptr;
	}

	freePMTSlotHead = (PhysicalAddress)(*((unsigned*)freePMTSlotHead));		// move the pmt list head

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

	if (pageDescriptor->v == 0)													// the page isn't loaded in memory -- return page fault
		return PAGE_FAULT;
	else {
		switch (type) {															// check access flags
		case READ:
			if (!pageDescriptor->rd) return TRAP;
			break;
		case WRITE:
			if (!pageDescriptor->wr) return TRAP;
			break;
		case READ_WRITE:
			if (!pageDescriptor->rd || !pageDescriptor->wr) return TRAP;
			break;
		case EXECUTE:
			if (!pageDescriptor->ex) return TRAP;
			break;
		}
		return OK;
	}

}

KernelSystem::PMT2Descriptor* KernelSystem::getPageDescriptor(const KernelProcess* process, VirtualAddress address) {
	unsigned page1Part = 0;														// extract parts of the virtual address	
	unsigned page2Part = 0;
	unsigned wordPart = 0;

	for (VirtualAddress mask = 1, unsigned i = 0; i < usefulBitLength; i++) {
		if (i < wordPartBitLength) {
			wordPart |= address & mask;
		}
		else if (i < wordPartBitLength + page2PartBitLength) {
			page2Part |= address & mask;
		}
		else {
			page1Part |= address & mask;
		}
		mask <<= 1;
	}

	PMT1* pmt1 = process->PMT1;												// access the PMT1 of the process
	PMT2* pmt2 = (*pmt1)[page1Part];										// attempt access to a PMT2 pointer

	if (!pmt2) return nullptr;
	else return &(*pmt2)[page2Part];										// access the targetted descriptor

}