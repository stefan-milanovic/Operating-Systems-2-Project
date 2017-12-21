#include "DiskManager.h"
#include "vm_declarations.h"
#include <cstring>

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

	if (!partition->writeCluster(chosenCluster, (char*)content))		// Write the content onto the partition.
		return -1;														// return -1 in case of error

	numberOfFreeClusters--;												// decrease the free cluster counter

	return chosenCluster;
}

bool DiskManager::writeToCluster(void* content, ClusterNo cluster) {
	if (cluster < 0 || cluster >= clusterUsageVectorSize) return false;

	if (!partition->writeCluster(cluster, (char*)content))				// Write the content onto the partition.
		return false;													// return false in case of error

	return true;
}

bool DiskManager::read(PhysicalAddress block, ClusterNo cluster) {

	if (cluster < 0 || cluster >= clusterUsageVectorSize) return false;

	char* buffer = new char[ClusterSize];

	if (!partition->readCluster(cluster, buffer))
		return false;													// read from partition was unsuccessful

	memcpy(block, buffer, ClusterSize);									// copy contents from buffer into physical block memory

	delete[] buffer;

	return true;
}

void DiskManager::freeCluster(ClusterNo clusterNumber) {

	clusterUsageVector[clusterNumber] = clusterUsageVectorHead;
	clusterUsageVectorHead = clusterNumber;								// optimised for a physical hard disk because of the head positioning

	numberOfFreeClusters++;
}