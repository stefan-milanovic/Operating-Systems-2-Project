#ifndef _kernelprocess_h_

#define _kernelprocess_h_

#include <vector>
#include "vm_declarations.h"

class KernelProcess {

public:

	// KOD CR/LOAD SEG - proveri da li je poravnat na virtuelni blok (nizih 10b su 0) i da li se ne preklapa sa drugim segmentom

	KernelProcess(ProcessId pid);

	~KernelProcess();	// check if needed

	ProcessId getProcessId() const { return id; }

	Status createSegment(VirtualAddress startAddress, PageNum segmentSize,
		AccessType flags);
	Status loadSegment(VirtualAddress startAddress, PageNum segmentSize,
		AccessType flags, void* content);
	Status deleteSegment(VirtualAddress startAddress);

	Status pageFault(VirtualAddress address);
	PhysicalAddress getPhysicalAddress(VirtualAddress address);

private:
		
	struct SegmentInfo {								// info about each segment the process has allocated
		
		VirtualAddress startAddress;					// start address in virtual space
		AccessType accessType;							// the access type for the segment that the process declared would use
		unsigned length = 0;							// each segment's length (in blocks required)
															
														// PHYSICAL MEMORY INFO
		bool continuous = true;							// continuous until a page of the segment has been swapped out or not enough room instantly
		unsigned* pageLocations;						// used only if the segment isn't continuous anymore

		SegmentInfo(VirtualAddress startAddr, AccessType access, unsigned newLength) :
			startAddress(startAddr), accessType(access), length(newLength), pageLocations(nullptr) {}

		~SegmentInfo() { if (pageLocations) delete[] pageLocations; }
	};

	std::vector<SegmentInfo> segments;				// current segment list
	ProcessId id;									// process id
	KernelSystem::PMT1* PMT1;						// page map table pointer of the first level, set in system's createProcess()

	friend class System;
	friend class KernelSystem;


};


#endif