#ifndef _kernelsystem_h_

#define _kernelsystem_h_

#include <vector>
#include <iostream>
#include <mutex>
#include <unordered_map>

#include "vm_declarations.h"
#include "Semaphore.h"
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

	Process* cloneProcess(ProcessId pid);

private:																		// private attributes

	PhysicalAddress processVMSpace;												// physical block memory
	PageNum processVMSpaceSize;

	PhysicalAddress pmtSpace;													// page map tables memory
	PageNum pmtSpaceSize;

	ProcessId processIDGenerator = 0;											// generates the ID for each new process
	std::unordered_map<ProcessId, Process*> activeProcesses;					// active process hash map

	struct PMT2Descriptor;
	struct ReferenceRegister {
		unsigned value = 0;														// value of the reference register (32-bit history)
		PMT2Descriptor* pageDescriptor = nullptr;								// descriptor for the page that currently holds this register's block
	};
	ReferenceRegister* referenceRegisters;										// dynamic array of reference registers 

	struct PMT2DescriptorCounter {
		PhysicalAddress pmt2StartAddress;										// start address of the PMT2
		unsigned short counter = 0;												// number of descriptors in the PMT2 with the inUse bit equal to 1

																				// for each descriptor in this PMT2
		std::vector<unsigned> sourceDescriptorCounters;							// used for shared cloning PMT2s to count number of references to each descriptor

		PMT2DescriptorCounter() {}
		PMT2DescriptorCounter(PhysicalAddress startAddress) : pmt2StartAddress(startAddress) {}
	};

	std::unordered_map<unsigned, PMT2DescriptorCounter> activePMT2Counter;		// keeps track of the number of descriptors in the allocated PMT2 tables (non-shared)

	PhysicalAddress freePMTSlotHead;											// head for the PMT1 blocks
	PhysicalAddress freeBlocksHead;												// head for the free physical blocks in memory

	PageNum numberOfFreePMTSlots;												// counts the number of free PMT slots 

	DiskManager* diskManager;													// encapsulates all of the operations with the partition

	std::recursive_mutex mutex;													// a mutex for synchronisation
	Semaphore thrashingSemaphore;												// semaphore that blocks processes that initiated system thrashing
	unsigned short consecutivePageFaultsCounter = 0;							// counts consecutive page faults and compares this value to _pageFaultLimitNumber_

	struct SharedSegment;
	std::unordered_map<std::string, SharedSegment> sharedSegments;				// keeps track of all the shared segments

																				// CONSTANTS


	static const unsigned short usefulBitLength = 24;
	static const unsigned short page1PartBitLength = 8;							// lengths of parts of the virtual address (in bits)
	static const unsigned short page2PartBitLength = 6;
	static const unsigned short wordPartBitLength = 10;

	static const unsigned short PMT1Size = 256;									// pmt1 and pmt2 sizes
	static const unsigned short PMT2Size = 64;

	static const unsigned short pageFaultLimitNumber = 50;						// after _pageFaultLmitNumber_ consecutive page faults thrashing is detected

	// MEMORY ORGANISATION

	struct PMT2Descriptor {
		char basicBits = 0;														// _/_/_/execute/write/read/dirty/valid bits
		char advancedBits = 0;													// _/_/copyOnWrite/isShared/referenced/cloned/hasCluster/inUse bits

		// bool hasCluster = 0;													// indicates whether a cluster has been reserved for this page
		// bool inUse = 0;														// indicates whether the descriptor is in use yet or not
		// bool isShared = 0;													// if the page is shared, the _block_ field points to the mutual descriptor

		// if isShared == 1														=> only bits ex/wr/rd + inUse are looked at (in the original descriptors)

		PhysicalAddress block = nullptr;										// remember pointer to a block of physical memory
		PMT2Descriptor* next = nullptr;										// next in segment and next in the global politics swapping technique
		ClusterNo disk;															// which cluster holds this exact page

		PMT2Descriptor() {}
		// basic bit operations
		void setV() { basicBits |= 0x01; } void resetV() { basicBits &= 0xFE; } bool getV() { return (basicBits & 0x01) ? true : false; }
		void setD() { basicBits |= 0x02; } void resetD() { basicBits &= 0xFD; } bool getD() { return (basicBits & 0x02) ? true : false; }
		void setRd() { basicBits |= 0x04; } bool getRd() { return (basicBits & 0x04) ? true : false; }
		void setWr() { basicBits |= 0x08; } bool getWr() { return (basicBits & 0x08) ? true : false; }
		void setRdWr() { basicBits |= 0x0C; }
		void setEx() { basicBits |= 0x10; } bool getEx() { return (basicBits & 0x10) ? true : false; }

		// advanced bit operations

		void setCopyOnWrite() { advancedBits |= 0x20; } void resetCopyOnWrite() { advancedBits &= 0xDF; }
		bool getCopyOnWrite() { return (advancedBits & 0x20) ? true : false; }

		void setShared() { advancedBits |= 0x10; } void resetShared() { advancedBits &= 0xEF; }
		bool getShared() { return (advancedBits & 0x10) ? true : false; }

		void setReferenced() { advancedBits |= 0x08; } void resetReferenced() { advancedBits &= 0xF7; }
		bool getReferenced() { return (advancedBits & 0x08) ? true : false; }

		void setCloned() { advancedBits |= 0x04; } void resetCloned() { advancedBits &= 0xFB; }
		bool getCloned() { return (advancedBits & 0x04) ? true : false; }

		void setHasCluster() { advancedBits |= 0x02; } void resetHasCluster() { advancedBits &= 0xFD; }
		bool getHasCluster() { return (advancedBits & 0x02) ? true : false; }

		void setInUse() { advancedBits |= 0x01; } void resetInUse() { advancedBits &= 0xFE; }
		bool getInUse() { return (advancedBits & 0x01) ? true : false; }

		void setBlock(PhysicalAddress newBlock) { block = newBlock; }
		PhysicalAddress getBlock() { return block; }

		void setDisk(ClusterNo clusterNo) { disk = clusterNo; }
		ClusterNo getDisk() { return disk; }

	};

	typedef PMT2Descriptor PMT2[PMT2Size];
	typedef PMT2* PMT1[PMT1Size];

	// SHARED SEGMENT ORGANISATION

	struct ReverseSegmentInfo {
		KernelProcess* process;													// process pointer to a process that is currently sharing a segment
		PMT2Descriptor* firstDescriptor;										// fast access to the first descriptor for deletion
	};

	struct SharedSegment {
		std::string name;
		AccessType accessType;													// the access type for the segment that the process declared would use

		PageNum length = 0;														// length of the shared segment
		unsigned short pmt2Number;												// number of allocated PMT2s for this shared segment

		PMT1* pmt1;																// pointer to this shared segment's PMT1 table

		ProcessId numberOfProcessesSharing;										// a counter for all the processes that are sharing this segment
		std::vector<ReverseSegmentInfo> processesSharing;						// remembers relevant pointers to all the processes currently sharing this segment

	};

	friend class Process;
	friend class KernelProcess;

private:

	PMT2Descriptor* getPageDescriptor(const KernelProcess* process, VirtualAddress address);

	// returns address to first descriptor, nullptr if any errors occur
	PMT2Descriptor* allocateDescriptors(KernelProcess* process, VirtualAddress startAddress,
		PageNum segmentSize, AccessType flags, bool load, void* content);

	// returns address to first descriptor, allocates a new shared segment descriptor table if need be or places pointers to an existing one
	PMT2Descriptor* connectToSharedSegment(KernelProcess* process, VirtualAddress startAddress,
		PageNum segmentSize, const char* name, AccessType flags);

	PhysicalAddress getSwappedBlock();											// performs the swapping algorithm and returns a block

	PhysicalAddress getFreeBlock();												// retrieves a block from the free block list	
	void setFreeBlock(PhysicalAddress block);									// places a now free block to the free block list

	PhysicalAddress getFreePMTSlot();											// retrieves a free PMT1/PMT2 slot (or nullptr if none exist)
	void freePMTSlot(PhysicalAddress slotAddress);								// places a now free PMT1/PMT2 slot to the free slot list

	void initialisePMT2(PMT2* pmt2);											// called when a new PMT2 is created

	static unsigned short extractPage1Part(VirtualAddress address);				// extraction methods for the virtual address parts
	static unsigned short extractPage2Part(VirtualAddress address);
	static unsigned short extractWordPart(VirtualAddress address);

	// unsigned simpleHash(unsigned a, unsigned b) { return ((a + 1) * b + 3) % activeProcesses.max_size(); }
	unsigned simpleHash(unsigned a, unsigned b) { return (((a + b) * (a + b + 1)) / 2 + b) % activeProcesses.max_size(); }

};


#endif