#ifndef _kernelsystem_h_

#include <vector>
#include <iostream>
#include <unordered_map>

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

	ProcessId processIDGenerator = 0;							// generates the ID for each new process
	std::unordered_map<ProcessId, Process*> activeProcesses;	// active process hash map

	PhysicalAddress clockHand;							// place initial value to VMSpace, indicating block 0 is the first free block
	PhysicalAddress freePMTSlotHead;					// head for the PMT1 blocks
	PhysicalAddress freeBlocksHead;						// head for the free physical blocks in memory

	unsigned clusterUsageVectorHead = 0;				// Indicates the first next free cluster.
	unsigned clusterUsageVectorSize;
	unsigned* clusterUsageVector;						// vector of free clusters. Index inside the vector points to the next free cluster number.

	// ima listu svakog aktivnog procesa -> svaki aktivni proces ima svoj PMT1 i listu svojih segmenata,
	// pri cemu se za svaki segment pamti svasta

															// CONSTANTS
	static const unsigned short usefulBitLength    = 24;
	static const unsigned short page1PartBitLength =  8;	// lengths of parts of the virtual address (in bits)
	static const unsigned short page2PartBitLength =  6;
	static const unsigned short wordPartBitLength  = 10;

	static const unsigned short PMT1Size = 256;				// pmt1 and pmt2 sizes
	static const unsigned short PMT2Size =  64;

	struct PMT2Descriptor {
		bool v;												// valid and dirty bits
		bool d;

		bool rd;											// read/write/execute bits
		bool wr;
		bool ex;

		bool refClockhand;
		bool refThrashing;
		bool copyOnWrite;

		// bool firstAccess; // za createSegment?
		unsigned block;				// remember only the first 22 bits because each block has a size of 1KB
		PhysicalAddress next;		// next in segment (if taken) or next in the global politics swapping technique
		ClusterNo disk;				// which cluster holds this exact page
	};

	typedef PMT2Descriptor PMT2[PMT2Size];
	typedef PMT2* PMT1[PMT1Size];

	friend class Process;
	friend class KernelProcess;

};


#endif