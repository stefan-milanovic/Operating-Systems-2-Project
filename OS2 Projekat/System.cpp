#include "System.h"
#include "KernelSystem.h"
#include "vm_declarations.h"

System::System(PhysicalAddress processVMSpace, PageNum processVMSpaceSize, PhysicalAddress pmtSpace, PageNum pmtSpaceSize, Partition* partition) {
	pSystem = new KernelSystem(processVMSpace, processVMSpaceSize, pmtSpace, pmtSpaceSize, partition);
}

System::~System() {
	delete pSystem;
}

Process* System::createProcess() {
	return pSystem->createProcess();
}

Time System::periodicJob() {
	// uncomment if necessary
	return pSystem->periodicJob();
}


Status System::access(ProcessId pid, VirtualAddress address, AccessType type) {
	return pSystem->access(pid, address, type);
}

Process* System::cloneProcess(ProcessId pid) {
	return pSystem->cloneProcess(pid);
}