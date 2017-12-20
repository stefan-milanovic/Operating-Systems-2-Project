#include "DiskManager.h"

DiskManager::DiskManager(Partition* partition) {
	partition = partition;												// assign the partition pointer
																		// create cluster usage vector

	clusterUsageVector = new ClusterNo(this->clusterUsageVectorSize = partition->getNumOfClusters()); 
	clusterUsageVectorHead = 0;

	for (int i = 0; i < clusterUsageVectorSize - 1; i++) {				// initialise it
		clusterUsageVector[i] = i + 1;
	}
	clusterUsageVector[clusterUsageVectorSize - 1] = -1;

	numberOfFreeClusters = clusterUsageVectorSize;						// assign number of free clusters

}

DiskManager::~DiskManager() {
	delete[] clusterUsageVector;
}

ClusterNo DiskManager::write(void* content) {

	if (clusterUsageVector[clusterUsageVectorHead] == -1) return -1;	// exception -- no free clusters

	ClusterNo chosenCluster = clusterUsageVectorHead;					// choose a free cluster and move the free cluster head
	clusterUsageVectorHead = clusterUsageVector[clusterUsageVectorHead];

	partition->writeCluster(chosenCluster, (char*)content);				// Write the content onto the partition.

	numberOfFreeClusters--;												// decrease the free cluster counter

	return chosenCluster;
}