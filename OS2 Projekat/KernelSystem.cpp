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

	this->clockHand = processVMSpace;									// assign head pointers
	this->freeBlocksHead = processVMSpace;
	this->freePMTSlotHead = pmtSpace;

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

	this->clusterUsageVector = new unsigned(this->clusterUsageVectorSize = partition->getNumOfClusters()); // create cluster usage vector
	this->clusterUsageVectorHead = 0;
	for (int i = 0; i < clusterUsageVectorSize - 1; i++) {				// initialise it
		clusterUsageVector[i] = i + 1;
	}
	clusterUsageVector[clusterUsageVectorSize - 1] = -1;


}

KernelSystem::~KernelSystem() {
	delete[] clusterUsageVector;
}

Process* KernelSystem::createProcess() {

	if (freePMTSlotHead) return nullptr;									// no space for a new PMT1 at the moment

	Process* newProcess = new Process(processIDGenerator++);

	newProcess->pProcess->PMT1 = (PMT1*)freePMTSlotHead;					// assign a free PMT page
	freePMTSlotHead = (PhysicalAddress)(*((unsigned*)freePMTSlotHead));		// move the pmt list head

	// do other things if needed

	return newProcess;
}

Time KernelSystem::periodicJob() {}


Status KernelSystem::access(ProcessId pid, VirtualAddress address, AccessType type) {

	// Page1 - 8 bits ; Page2 - 6 bits ; Word - 10 bits
		
	Process* wantedProcess = nullptr;
	/*
	std::for_each(activeProcesses.begin(), activeProcesses.end(), [pid, &wantedProcess](Process* process) {
		if (process->getProcessId() == pid) {
			wantedProcess = process;
		}
	});
	*/ 

	for (std::vector<Process*>::iterator process = activeProcesses.begin(); process != activeProcesses.end(); ++process) {
		if ((*process)->getProcessId() == pid) {
			wantedProcess = *process;
			break;
		}
	}

	/* std::_Vector_iterator<std::_Vector_val<std::_Simple_types<Process*>>> 
	for (auto process = activeProcesses.begin(); process != activeProcesses.end(); process++) {
		if (process.elem)
	} */ 

	if (!wantedProcess) return TRAP; // ? 

	bool segmentFound = false;													// check if the address belongs to an allocated segment
	for (std::vector<KernelProcess::SegmentInfo>::iterator segment = wantedProcess->pProcess->segments.begin();
		segment != wantedProcess->pProcess->segments.end(); ++segment) {
		VirtualAddress segStartAddress = segment->startAddress, segEndAddress = segment->startAddress + segment->length * PAGE_SIZE;

		if (segStartAddress <= address && address <= segEndAddress) {			// the address belongs to a segment
			segmentFound = true;
			if (type != segment->accessType)
				return TRAP;													// invalid type of operation
			else
				break;
		}
	}

	if (!segmentFound) return TRAP;												// the address doesn't belong to any segment currently

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

	PMT1* pmt1 = wantedProcess->pProcess->PMT1;
	PMT2* pmt2 = (*pmt1)[page1Part];
	PMT2Descriptor pageDescriptor = (*pmt2)[page2Part];

	if (pageDescriptor.v == 0)
		return PAGE_FAULT;
	else 
		return OK;

}
