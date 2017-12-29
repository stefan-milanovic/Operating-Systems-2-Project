#include <chrono>
#include <cstdint>
#include <memory>
#include <iostream>
#include <thread>
#include "System.h"
#include "part.h"
#include "vm_declarations.h"
#include "ProcessTest.h"
#include "SystemTest.h"

/* 

// 10000
#define VM_SPACE_SIZE (10000)
#define PMT_SPACE_SIZE (3000)
#define N_PROCESS (20)
#define PERIODIC_JOB_COST (1)

PhysicalAddress alignPointer(PhysicalAddress address) {
	uint64_t addr = reinterpret_cast<uint64_t> (address);

	addr += PAGE_SIZE;
	addr = addr / PAGE_SIZE * PAGE_SIZE;

	return reinterpret_cast<PhysicalAddress> (addr);
}

int main() {
	Partition part("p1.ini");
	uint64_t size = (VM_SPACE_SIZE + 2) * PAGE_SIZE;
	PhysicalAddress vmSpace = (PhysicalAddress) new char[size];
	PhysicalAddress alignedVmSpace = alignPointer(vmSpace);

	size = (PMT_SPACE_SIZE + 2) * PAGE_SIZE;
	PhysicalAddress pmtSpace = (PhysicalAddress) new char[size];
	PhysicalAddress alignedPmtSpace = alignPointer(pmtSpace);

	System system(alignedVmSpace, VM_SPACE_SIZE, alignedPmtSpace, PMT_SPACE_SIZE, &part);
	SystemTest systemTest(system, alignedVmSpace, VM_SPACE_SIZE);
	ProcessTest* process[N_PROCESS];
	std::thread *threads[N_PROCESS];

	std::mutex globalMutex;

	for (int i = 0; i < N_PROCESS; i++) {
		process[i] = new ProcessTest(system, systemTest);
	}

	for (int i = 0; i < N_PROCESS; i++) {
		std::cout << "Create process " << i << std::endl;
		threads[i] = new std::thread(&ProcessTest::run, process[i]);
	}

	Time time;
	while ((time = system.periodicJob())) {
		std::this_thread::sleep_for(std::chrono::microseconds(time));

		std::lock_guard<std::mutex> guard(systemTest.getGlobalMutex());

		std::cout << "Doing periodic job\n";

		std::this_thread::sleep_for(std::chrono::microseconds(PERIODIC_JOB_COST));

		bool finished = true;
		for (int i = 0; i < N_PROCESS; i++) {
			finished = finished && process[i]->isFinished();
		}

		if (finished) {
			break;
		}
	}

	for (int i = 0; i < N_PROCESS; i++) {
		threads[i]->join();
		delete threads[i];
		delete process[i];
	}

	delete[] vmSpace;
	delete[] pmtSpace;

	std::cout << "Test finished\n";

}

*/ 

#define VM_SPACE_SIZE (10000)
#define PMT_SPACE_SIZE (3000)

PhysicalAddress alignPointer(PhysicalAddress address) {
	uint64_t addr = reinterpret_cast<uint64_t> (address);

	addr += PAGE_SIZE;
	addr = addr / PAGE_SIZE * PAGE_SIZE;

	return reinterpret_cast<PhysicalAddress> (addr);
}


int main()
{
	Partition part("p1.ini");
	uint64_t size = (VM_SPACE_SIZE + 2) * PAGE_SIZE;
	PhysicalAddress vmSpace = (PhysicalAddress) new char[size];
	PhysicalAddress alignedVmSpace = alignPointer(vmSpace);

	size = (PMT_SPACE_SIZE + 2) * PAGE_SIZE;
	PhysicalAddress pmtSpace = (PhysicalAddress) new char[size];
	PhysicalAddress alignedPmtSpace = alignPointer(pmtSpace);

	System system(alignedVmSpace, VM_SPACE_SIZE, alignedPmtSpace, PMT_SPACE_SIZE, &part);

	Process * p1 = system.createProcess();
	p1->createSharedSegment(1024, 100, "prvi", READ_WRITE);

	Status status = system.access(p1->getProcessId(), 1030, WRITE);
	if (status == PAGE_FAULT)
	{
		p1->pageFault(1030);
	}
	PhysicalAddress paddr = p1->getPhysicalAddress(1030);
	std::cout << "Adresa je " << paddr;
	*(char *)paddr = (char)-1;

	Process * p2 = system.createProcess();
	p2->createSharedSegment(1024, 50, "prvi", READ);
	status = system.access(p2->getProcessId(), 1030, READ);
	if (status == PAGE_FAULT)
	{
		p1->pageFault(1030);
	}

	Process * p3 = system.createProcess();
	p3->createSharedSegment(1024, 50, "prvi", READ);
	status = system.access(p3->getProcessId(), 1030, READ);
	if (status == PAGE_FAULT)
	{
		p1->pageFault(1030);
	}

	paddr = p2->getPhysicalAddress(1030);
	std::cout << *((char *)paddr);
	std::cout << "\nNova adresa je " << paddr;

	p3->deleteSharedSegment("prvi");

	paddr = p3->getPhysicalAddress(1030);
	std::cout << "\nNova adresa je " << paddr;

	delete p1;
	delete p2;
}