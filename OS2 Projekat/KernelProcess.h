#ifndef _kernelprocess_h_

#define _kernelprocess_h_

#include <vector>
#include "vm_declarations.h"

class KernelProcess {

public:

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
	struct SegmentInfo;

	bool inconsistencyCheck(VirtualAddress startAddress, PageNum segmentSize);
	bool inconsistentAddressCheck(VirtualAddress startAddress);

	Status optimisedDeleteSegment(SegmentInfo* segment);// Deletes a certain segment, skips several checks for the user deleteSegment method.

	void releaseMemoryAndDisk(SegmentInfo* segment);	// Releases everything reserved by the given segment. Used in the delete methods.

private:
		
	struct SegmentInfo {								// info about each segment the process has allocated
		
		VirtualAddress startAddress;					// start address in virtual space
		AccessType accessType;							// the access type for the segment that the process declared would use
		PageNum length = 0;								// each segment's length (in blocks required)
		KernelSystem::PMT2Descriptor* firstDescAddress;	// address of the first descriptor (from this point onwards for _length_ descriptors)

		SegmentInfo(VirtualAddress startAddr, AccessType access, PageNum newLength, KernelSystem::PMT2Descriptor* descriptorAddress) :
			startAddress(startAddr), accessType(access), length(newLength), firstDescAddress(descriptorAddress) {}

		~SegmentInfo() {}
	};

	std::vector<SegmentInfo> segments;				// current segment list
	ProcessId id;									// process id
	KernelSystem* system;							// the system this process is being run on, set in system's createProcess()
	KernelSystem::PMT1* PMT1;						// page map table pointer of the first level, set in system's createProcess()

	friend class System;
	friend class KernelSystem;


};


#endif