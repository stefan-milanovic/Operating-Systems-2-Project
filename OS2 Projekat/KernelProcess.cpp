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
}

Status KernelProcess::loadSegment(VirtualAddress startAddress, PageNum segmentSize,
	AccessType flags, void* content) {
	
}

Status KernelProcess::deleteSegment(VirtualAddress startAddress) {
	 // check for inconsistencies, the start addr has to be the beginning of a block/segment

}

Status KernelProcess::pageFault(VirtualAddress address) {
	
}

PhysicalAddress KernelProcess::getPhysicalAddress(VirtualAddress address) {

}