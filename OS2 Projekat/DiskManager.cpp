#include "DiskManager.h"

DiskManager::DiskManager(Partition* partition) {
	this->partition = partition;										// assign the partition pointer

	this->clusterUsageVector = new ClusterNo(this->clusterUsageVectorSize = partition->getNumOfClusters()); // create cluster usage vector
	this->clusterUsageVectorHead = 0;
	for (int i = 0; i < clusterUsageVectorSize - 1; i++) {				// initialise it
		clusterUsageVector[i] = i + 1;
	}
	clusterUsageVector[clusterUsageVectorSize - 1] = -1;
	this->numberOfFreeClusters = this->clusterUsageVectorSize;			// assign number of free clusters

}

DiskManager::~DiskManager() {
	delete[] clusterUsageVector;
}