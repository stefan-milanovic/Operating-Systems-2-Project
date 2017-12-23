#ifndef VM_PROCESSTEST_H
#define VM_PROCESSTEST_H

#include <thread>
#include <vector>
#include "vm_declarations.h"
#include "Process.h"
#include "SystemTest.h"

class ProcessTest {
public:
	explicit ProcessTest(System& system, SystemTest& systemTest_);
	Status addCodeSegment(VirtualAddress address, PageNum size);
	Status addDataSegment(VirtualAddress address, PageNum size);
	void writeToAddress(VirtualAddress address, char value);
	char readFromAddress(VirtualAddress address);
	bool isFinished() const;
	void run();
	~ProcessTest();
private:
	VirtualAddress alignToPage(VirtualAddress address);
	VirtualAddress getOffset(VirtualAddress address);
	PhysicalAddress getPhyAddress(VirtualAddress address);

	std::vector<std::tuple<PhysicalAddress, VirtualAddress, PageNum>> checkMemory;
	Process *process;
	SystemTest &systemTest;
	bool finished;
};


#endif //VM_PROCESSTEST_H
