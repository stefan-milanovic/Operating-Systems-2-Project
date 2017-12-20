#ifndef _kernelsystem_h_

#define _kernelsystem_h_

#include <vector>
#include <iostream>
#include <unordered_map>

#include "vm_declarations.h"
#include "part.h"
#include "DiskManager.h"

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


private:																		// private attributes

	PhysicalAddress processVMSpace;
	PageNum processVMSpaceSize;

	PhysicalAddress pmtSpace;
	PageNum pmtSpaceSize;

	Partition* partition;

	ProcessId processIDGenerator = 0;											// generates the ID for each new process
	std::unordered_map<ProcessId, Process*> activeProcesses;					// active process hash map

	struct PMT2Descriptor;
	PMT2Descriptor* clockHand;													// clockhand for the second chance swapping algorithm

	PhysicalAddress freePMTSlotHead;											// head for the PMT1 blocks
	PhysicalAddress freeBlocksHead;												// head for the free physical blocks in memory

	PageNum numberOfFreePMTSlots;												// counts the number of free PMT slots 

	DiskManager* diskManager;													// encapsulates all of the operations with the partition

	// ima listu svakog aktivnog procesa -> svaki aktivni proces ima svoj PMT1 i listu svojih segmenata,
	// pri cemu se za svaki segment pamti svasta

																				// CONSTANTS
	static const unsigned short usefulBitLength    = 24;
	static const unsigned short page1PartBitLength =  8;						// lengths of parts of the virtual address (in bits)
	static const unsigned short page2PartBitLength =  6;
	static const unsigned short wordPartBitLength  = 10;

	static const unsigned short PMT1Size = 256;									// pmt1 and pmt2 sizes
	static const unsigned short PMT2Size =  64;

	struct PMT2Descriptor {
		bool v = 0;																// valid and dirty bits
		bool d = 0;

		bool rd;																// read/write/execute bits
		bool wr;
		bool ex;

		bool refClockhand = 0;													// reference bits
		bool refThrashing = 0;
			
		bool hasCluster = 0;													// indicates whether a cluster has been reserved for this page

		PhysicalAddress block = nullptr;										// remember pointer to a block of physical memory
		PMT2Descriptor* next =  nullptr;										// next in segment and next in the global politics swapping technique
		ClusterNo disk;															// which cluster holds this exact page

		PMT2Descriptor() {}

		void setV() { v = 1; } void resetV() { v = 0; }
		void setD() { d = 1; } void resetD() { d = 0; }
		void setRd() { rd = 1; } void setWr() { wr = 1; } 
		void setRdWr() { rd = wr = 1; }
		void setEx() { ex = 1; }
		
		void setRefClockhand() { refClockhand = 1; }
		void resetRefClockhand() { refClockhand = 0; }

		void setRefThrashing() { refThrashing = 1; }
		void resetRefThrashing() { refThrashing = 0; }

		void setClusterBit() { hasCluster = 1; }

		void setDisk(ClusterNo clusterNo) { disk = clusterNo; }
	};

	typedef PMT2Descriptor PMT2[PMT2Size];
	typedef PMT2* PMT1[PMT1Size];

	friend class Process;
	friend class KernelProcess;

private:

	static PMT2Descriptor* getPageDescriptor(const KernelProcess* process, VirtualAddress address);

};


#endif