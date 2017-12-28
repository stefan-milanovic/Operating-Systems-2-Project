#ifndef _kernelprocess_h_

#define _kernelprocess_h_

#include <vector>
#include "KernelSystem.h"
#include "vm_declarations.h"

class KernelProcess {

public:

	KernelProcess(ProcessId pid);

	~KernelProcess();

	ProcessId getProcessId() const { return id; }

	Status createSegment(VirtualAddress startAddress, PageNum segmentSize,
		AccessType flags);
	Status loadSegment(VirtualAddress startAddress, PageNum segmentSize,
		AccessType flags, void* content);
	Status deleteSegment(VirtualAddress startAddress);

	Status pageFault(VirtualAddress address);
	PhysicalAddress getPhysicalAddress(VirtualAddress address);

	void blockIfThrashing();

	Process* clone(ProcessId pid);
 	Status createSharedSegment(VirtualAddress startAddress,
 	PageNum segmentSize, const char* name, AccessType flags);
 	Status disconnectSharedSegment(const char* name);
 	Status deleteSharedSegment(const char* name);

private:
	struct SegmentInfo;

	bool inconsistencyCheck(VirtualAddress startAddress, PageNum segmentSize);
	bool inconsistentAddressCheck(VirtualAddress startAddress);
																			// Deletes a segment and skips several checks present in the user deleteSegment() method.
	Status optimisedDeleteSegment(SegmentInfo* segment, bool checkIndex, unsigned index );

	void releaseMemoryAndDisk(SegmentInfo* segment);						// Releases everything reserved by the given segment. Used in the delete methods.


	unsigned concatenatePageParts(unsigned short page1, unsigned short page2);

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

	std::vector<SegmentInfo> segments;					// current segment list
	ProcessId id;										// process id
	KernelSystem* system;								// the system this process is being run on, set in system's createProcess()
	KernelSystem::PMT1* PMT1;							// page map table pointer of the first level, set in system's createProcess()

	// unsigned short allocatedPMT2Counter;				// counts the number of allocated PMT2 tables (useful for cloning)

	friend class System;
	friend class KernelSystem;


};


#endif