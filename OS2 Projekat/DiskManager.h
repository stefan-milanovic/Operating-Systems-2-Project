#ifndef _diskmanager_h_


#include "part.h"

#define _diskmanager_h_

class DiskManager {

public:

	DiskManager(Partition*);
	~DiskManager();

private:

	Partition* partition;

	ClusterNo clusterUsageVectorHead = 0;		// Indicates the first next free cluster.
	ClusterNo clusterUsageVectorSize;
	ClusterNo numberOfFreeClusters;
	ClusterNo* clusterUsageVector;				// vector of free clusters. Index inside the vector points to the next free cluster number.

};


#endif