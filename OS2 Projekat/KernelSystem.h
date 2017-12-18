#ifndef _kernelsystem_h_

#include <vector>
#include <iostream>
#include "vm_declarations.h"
#include "part.h"

#define _kernelsystem_h_

class Partition;
class Process;
class KernelProcess;
class KernelSystem;

class KernelSystem {

public:

	KernelSystem(PhysicalAddress processVMSpace, PageNum processVMSpaceSize,
		PhysicalAddress pmtSpace, PageNum pmtSpaceSize,
		Partition* partition);

	~KernelSystem();

	Process* createProcess();
	Time periodicJob();

	// Hardware job
	Status access(ProcessId pid, VirtualAddress address, AccessType type);

private:									// private attributes

	PhysicalAddress processVMSpace;
	PageNum processVMSpaceSize;

	PhysicalAddress pmtSpace;
	PageNum pmtSpaceSize;

	Partition* partition;

	ProcessId processIDGenerator = 0;					// generates the ID for each new process
	std::vector<Process*> activeProcesses;				// active process list

	PhysicalAddress clockHand;							// place initial value to VMSpace, indicating block 0 is the first free block
	PhysicalAddress freePMTSlotHead;					// head for the PMT1 blocks
	PhysicalAddress freeBlocksHead;						// head for the free physical blocks in memory

	unsigned clusterUsageVectorHead = 0;				// Indicates the first next free cluster.
	unsigned clusterUsageVectorSize;
	unsigned* clusterUsageVector;						// vector of free clusters. Index inside the vector points to the next free cluster number.

	// ima listu svakog aktivnog procesa -> svaki aktivni proces ima svoj PMT1 i listu svojih segmenata,
	// pri cemu se za svaki segment pamti svasta

															// CONSTANTS
	static const unsigned short usefulBitLength = 24;
	static const unsigned short page1PartBitLength = 8;		// lengths of parts of the virtual address (in bits)
	static const unsigned short page2PartBitLength = 6;
	static const unsigned short wordPartBitLength = 10;

	struct PMT2Descriptor {
		bool v;
		bool d;
		bool refClockhand;
		bool refThrashing;
		bool copyOnWrite;
		// bool firstAccess; // za createSegment?
		unsigned block;				// remember only the first 22 bits because each block has a size of 1KB
		PhysicalAddress next;		// next in segment (if taken) or next in the global politics swapping technique
		ClusterNo disk;				// which cluster holds this exact page
	};

	typedef PMT2Descriptor PMT2[64];
	typedef PMT2* PMT1[256];

	friend class Process;
	friend class KernelProcess;

};


#endif