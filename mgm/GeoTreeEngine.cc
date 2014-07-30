// ----------------------------------------------------------------------
// File: ConfigEngine.cc
// Author: Geoffray Adde - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/*----------------------------------------------------------------------------*/
#include "mgm/GeoTreeEngine.hh"
#include "mgm/XrdMgmOfs.hh"
#include "common/FileSystem.hh"
/*----------------------------------------------------------------------------*/
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <tuple>
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/

using namespace std;
using namespace eos::common;
using namespace eos::mgm;

EOSMGMNAMESPACE_BEGIN

GeoTreeEngine gGeoTreeEngine;

const size_t GeoTreeEngine::gGeoBufferSize = 64 * 1024;
__thread void* GeoTreeEngine::tlGeoBuffer = NULL;
__thread const FsGroup* GeoTreeEngine::tlCurrentGroup = NULL;

const int
GeoTreeEngine::sfgId = 1,
GeoTreeEngine::sfgHost = 1 << 1,
GeoTreeEngine::sfgGeotag = 1 << 2,
GeoTreeEngine::sfgBoot = 1 << 3,
GeoTreeEngine::sfgActive = 1 << 4,
GeoTreeEngine::sfgConfigstatus = 1 << 5,
GeoTreeEngine::sfgDrain = 1 << 6,
GeoTreeEngine::sfgDrainer = 1 << 6,
GeoTreeEngine::sfgBlcingrun = 1 << 6,
GeoTreeEngine::sfgBlcerrun = 1 << 6,
GeoTreeEngine::sfgBalthres = 1 << 7,
GeoTreeEngine::sfgBlkavailb = 1 << 8,
GeoTreeEngine::sfgFsfilled = 1 << 9,
GeoTreeEngine::sfgNomfilled = 1 << 10,
GeoTreeEngine::sfgWriteratemb = 1 << 11,
GeoTreeEngine::sfgReadratemb = 1 << 12,
GeoTreeEngine::sfgDiskload = 1 << 13,
GeoTreeEngine::sfgEthmib = 1 << 14,
GeoTreeEngine::sfgInratemib = 1 << 15,
GeoTreeEngine::sfgOutratemib = 1 << 16,
GeoTreeEngine::sfgErrc = 1 << 17;

set<string> GeoTreeEngine::gWatchedKeys;

const map<string,int> GeoTreeEngine::gNotifKey2Enum =
{
	make_pair("id",sfgId),
	make_pair("host",sfgHost),
	make_pair("stat.geotag",sfgGeotag),
	make_pair("stat.boot",sfgBoot),
	make_pair("stat.active",sfgActive),
	make_pair("configstatus",sfgConfigstatus),
	make_pair("stat.drain",sfgDrain),
	make_pair("stat.drainer",sfgDrainer),
	make_pair("stat.balancing.running",sfgBlcingrun),
	make_pair("stat.balancer.running",sfgBlcerrun),
	make_pair("stat.balance.threshold",sfgBalthres),
	make_pair("stat.nominal.filled",sfgNomfilled),
	make_pair("stat.statfs.bavail",sfgBlkavailb),
	make_pair("stat.statfs.filled",sfgFsfilled),
	make_pair("stat.disk.writeratemb",sfgWriteratemb),
	make_pair("stat.disk.readratemb",sfgReadratemb),
	make_pair("stat.disk.load",sfgDiskload),
	make_pair("stat.net.ethmib",sfgEthmib),
	make_pair("stat.net.inratemib",sfgInratemib),
	make_pair("stat.net.outratemib",sfgOutratemib),
	make_pair("stat.errc",sfgErrc)
};

map<string,int> GeoTreeEngine::gNotificationsBuffer;
bool GeoTreeEngine::gUpdaterPaused;

bool GeoTreeEngine::TreeMapEntry::updateFastStructures()
{
	FastStructures *ft = backgroundFastStruct;

	if(!slowTree->buildFastStrctures(
					ft->placementTree , ft->rOAccessTree, ft->rWAccessTree,
					ft->blcPlacementTree , ft->blcAccessTree,
					ft->drnPlacementTree , ft->drnAccessTree,
					ft->treeInfo , ft->fs2TreeIdx, ft->tag2NodeIdx
			))
	{
		eos_static_crit("Error updating the fast structures");
		return false;
	}

	return true;
}

bool GeoTreeEngine::insertFsIntoGroup(FileSystem *fs ,
		FsGroup *group,
		bool updateFastStruct)
{
	// a race condition might happen if two different threads try to insert the same fs in the group at the same time
	// luckily this NOT supposed to happen in the way things are currently implemented
	FileSystem::fsid_t fsid = fs->GetId();
	TreeMapEntry *mapEntry;

	{
		pTreeMapMutex.LockWrite();
		// ==== check that fs is not already registered
		if(pFs2TreeMapEntry.count(fsid))
		{
			eos_err("error inserting fs %lu into group %s : fs is already part of a group",
					(unsigned long)fsid,
					group->mName.c_str()
			);
			pTreeMapMutex.UnLockWrite();
			return false;
		}

		// ==== get the entry
		if(pGroup2TreeMapEntry.count(group))
		mapEntry = pGroup2TreeMapEntry[group];
		else
		{
			mapEntry = new TreeMapEntry;
			mapEntry->slowTree = new SlowTree(group->mName.c_str());
		}
		mapEntry->slowTreeMutex.LockWrite();
		pTreeMapMutex.UnLockWrite();
	}

	// ==== fill the entry
	// create new TreeNodeInfo/TreeNodeState pair and update its data
	eos::common::FileSystem::fs_snapshot_t fsn;
	fs->SnapShotFileSystem(fsn, true);

	SchedTreeBase::TreeNodeInfo info;
	info.geotag = fsn.mGeoTag;
	if(info.geotag.empty())
	{
		char buffer[64];
		snprintf(buffer,64,"nogeotag");
		info.geotag = buffer;
	}
	info.host= fsn.mHost;
	if(info.host.empty())
	{
		uuid_t uuid;
		char buffer[64];
		snprintf(buffer,64,"nohost-");
		uuid_generate_time(uuid);
		uuid_unparse(uuid, buffer+7);
		info.geotag = buffer;
	}
	info.fsId = 0;
	info.fsId= fsn.mId;
	if(!info.fsId)
	{
		mapEntry->slowTreeMutex.UnLockWrite();

		eos_err("error inserting fs %lu into group %s : FsId is not set!",
				(unsigned long)fsid,
				group->mName.c_str()
		);

		return false;
	}

	SchedTreeBase::TreeNodeStateFloat state;

	// try to insert the new node in the Slowtree
	SlowTreeNode *node = mapEntry->slowTree->insert(&info,&state);
	if(node==NULL)
	{
		mapEntry->slowTreeMutex.UnLockWrite();

		eos_err("error inserting fs %lu into group %s : slow tree node insertion failed",
				(unsigned long)fsid,
				group->mName.c_str()
		);

		return false;
	}

	// update all the information about this new node
	if(!updateTreeInfo(mapEntry,&fsn,~sfgGeotag & ~sfgId & ~sfgHost ,0,node))
	{
		mapEntry->slowTreeMutex.UnLockWrite();
		pTreeMapMutex.LockRead();
		eos_err("error inserting fs %lu into group %s : slow tree node update failed",
				(unsigned long)fsid,
				group->mName.c_str()
		);
		pTreeMapMutex.UnLockRead();
		return false;
	}

	mapEntry->fs2SlowTreeNode[fsid] = node;
	mapEntry->slowTreeModified = true;

	// update the fast structures now if requested
	if(updateFastStruct)
	{
		if(!updateFastStructures(mapEntry))
		{
			mapEntry->slowTreeMutex.UnLockWrite();
			pTreeMapMutex.LockRead();
			eos_err("error inserting fs %lu into group %s : fast structures update failed",
					fsid,
					group->mName.c_str(),
					pFs2TreeMapEntry[fsid]->group->mName.c_str()
			);
			pTreeMapMutex.UnLockRead();
			return false;
		}
		else
		{
			mapEntry->slowTreeModified = false;
		}
	}

	// ==== update the entry in the map
	{
		pTreeMapMutex.LockWrite();
		mapEntry->group = group;
		pGroup2TreeMapEntry[group] = mapEntry;
		pFs2TreeMapEntry[fsid] = mapEntry;
		pTreeMapMutex.UnLockWrite();
		mapEntry->slowTreeMutex.UnLockWrite();
	}

	// ==== update the shared object notifications
	{
		if(gWatchedKeys.empty())
		{
			for(auto it = gNotifKey2Enum.begin(); it != gNotifKey2Enum.end(); it++ )
			{
				gWatchedKeys.insert(it->first);
			}
		}
		if(!gOFS->ObjectNotifier.SubscribesToSubjectAndKey("geotreeengine",fs->GetQueuePath(),gWatchedKeys,XrdMqSharedObjectChangeNotifier::kMqSubjectStrictModification))
		{
			eos_crit("error inserting fs %lu into group %s : error subscribing to shared object notifications",
					(unsigned long)fsid,
					group->mName.c_str()
			);
			return false;
		}
	}

	if(eos::common::Logging::gLogMask & LOG_INFO)
	{
		stringstream ss;
		ss << (*mapEntry->slowTree);

		eos_info("inserted fs %lu into group %s geotag is %s and fullgeotag is %s\n%s",
				(unsigned long)fsid,
				group->mName.c_str(),
				node->pNodeInfo.geotag.c_str(),
				node->pNodeInfo.fullGeotag.c_str(),
				ss.str().c_str()
		);
	}

	return true;
}

bool GeoTreeEngine::removeFsFromGroup(FileSystem *fs ,
		FsGroup *group,
		bool updateFastStruct)
{
	TreeMapEntry *mapEntry;
	FileSystem::fsid_t fsid = fs->GetId();

	{
		pTreeMapMutex.LockWrite();
		// ==== check that fs is registered
		if(!pFs2TreeMapEntry.count(fsid))
		{
			eos_err("error removing fs %lu from group %s : fs is not registered",
					(unsigned long)fsid,
					group->mName.c_str()
			);
			pTreeMapMutex.UnLockWrite();
			return false;
		}
		mapEntry = pFs2TreeMapEntry[fsid];

		// ==== get the entry
		if(!pGroup2TreeMapEntry.count(group))
		{
			eos_err("error removing fs %lu from group %s : fs is not registered ",
					(unsigned long)fsid,
					group->mName.c_str()
			);
			pTreeMapMutex.UnLockWrite();
			return false;
		}
		pTreeMapMutex.UnLockWrite();
		mapEntry = pGroup2TreeMapEntry[group];
		mapEntry->slowTreeMutex.LockWrite();
	}

	// ==== update the entry
	SchedTreeBase::TreeNodeInfo info;
	const SlowTreeNode *intree = mapEntry->fs2SlowTreeNode[fsid];
	info = intree->pNodeInfo;
	info.geotag = intree->pNodeInfo.fullGeotag;
	eos_debug("SlowNodeTree to be removed is %lu   %s   %s   %s",
			(unsigned long)intree->pNodeInfo.fsId,
			intree->pNodeInfo.host.c_str(),
			intree->pNodeInfo.geotag.c_str(),
			intree->pNodeInfo.fullGeotag.c_str());
	// try to update the SlowTree
	if(!mapEntry->slowTree->remove(&info))
	{
		mapEntry->slowTreeMutex.UnLockWrite();
		eos_err("error removing fs %lu from group %s : removing the slow tree node failed. geotag is %s and geotag in tree is %s and %s",
				(unsigned long)fsid,
				group->mName.c_str(),
				info.geotag.c_str(),
				intree->pNodeInfo.fullGeotag.c_str(),
				intree->pNodeInfo.geotag.c_str()
		);
		return false;
	}
	mapEntry->fs2SlowTreeNode.erase(fsid);
	// if the tree is empty, remove the entry from the map
	if(!mapEntry->fs2SlowTreeNode.empty())// if the tree is getting empty, no need to update it
	mapEntry->slowTreeModified = true;

	if(updateFastStruct && mapEntry->slowTreeModified)
	if(!updateFastStructures(mapEntry))
	{
		mapEntry->slowTreeMutex.UnLockWrite();
		pTreeMapMutex.LockRead();
		eos_err("error removing fs %lu from group %s : fast structures update failed",
				fsid,
				group->mName.c_str(),
				pFs2TreeMapEntry[fsid]->group->mName.c_str()
		);
		pTreeMapMutex.UnLockRead();
		return false;
	}

	// ==== update the entry in the map if needed
	{
		pTreeMapMutex.LockWrite();
		pFs2TreeMapEntry.erase(fsid);
		if(mapEntry->fs2SlowTreeNode.empty())
		{
			pGroup2TreeMapEntry.erase(group); // prevent from access by other threads
			pPendingDeletions.push_back(mapEntry);
		}
		mapEntry->slowTreeMutex.UnLockWrite();
		pTreeMapMutex.UnLockWrite();
	}

	// ==== update the shared object notifications
	{
		if(!gOFS->ObjectNotifier.UnsubscribesToSubjectAndKey("geotreeengine",fs->GetQueuePath(),gWatchedKeys,XrdMqSharedObjectChangeNotifier::kMqSubjectStrictModification))
		{
			eos_crit("error removing fs %lu into group %s : error unsubscribing to shared object notifications",
					(unsigned long)fsid,
					group->mName.c_str()
			);
			return false;
		}
	}

	return true;
}

bool GeoTreeEngine::placeNewReplicasOneGroup( FsGroup* group, const size_t &nNewReplicas,
		vector<FileSystem::fsid_t> *newReplicas,
		SchedType type,
		vector<FileSystem::fsid_t> *existingReplicas,
		vector<FileSystem::fsid_t> *excludeFs,
		vector<string> *excludeGeoTags,
		vector<string> *forceGeoTags)
{
	assert(nNewReplicas);
	assert(newReplicas);

	// find the entry in the map
	tlCurrentGroup = group;
	TreeMapEntry *entry;
	{
		RWMutexReadLock lock(this->pTreeMapMutex);
		if(!pGroup2TreeMapEntry.count(group))
		{
			eos_err("could not find the requested placement group in the map");
			return false;
		}
		entry = pGroup2TreeMapEntry[group];
		AtomicInc(entry->fastStructLockWaitersCount);
	}

	// readlock the original fast structure
	entry->doubleBufferMutex.LockRead();

	// locate the existing replicas and the excluded fs in the tree
	vector<SchedTreeBase::tFastTreeIdx> newReplicasIdx(nNewReplicas),*existingReplicasIdx=NULL,*excludeFsIdx=NULL,*forceBrIdx=NULL;
	newReplicasIdx.resize(0);
	if(existingReplicas)
	{
		existingReplicasIdx = new vector<SchedTreeBase::tFastTreeIdx>(existingReplicas->size());
		existingReplicasIdx->resize(0);
		for(auto it = existingReplicas->begin(); it != existingReplicas->end(); ++it)
		{
			const SchedTreeBase::tFastTreeIdx *idx;
			if(!entry->foregroundFastStruct->fs2TreeIdx->get(*it,idx))
			{
				eos_warning("could not place preexisting replica on the fast tree");
				continue;
			}
			existingReplicasIdx->push_back(*idx);
		}
	}
	if(excludeFs)
	{
		excludeFsIdx = new vector<SchedTreeBase::tFastTreeIdx>(excludeFs->size());
		excludeFsIdx->resize(0);
		for(auto it = excludeFs->begin(); it != excludeFs->end(); ++it)
		{
			const SchedTreeBase::tFastTreeIdx *idx;
			if(!entry->foregroundFastStruct->fs2TreeIdx->get(*it,idx))
			{
				eos_warning("could not place excluded fs on the fast tree");
				continue;
			}
			excludeFsIdx->push_back(*idx);
		}
	}
	if(excludeGeoTags)
	{
		if(!excludeFsIdx)
		{
			excludeFsIdx = new vector<SchedTreeBase::tFastTreeIdx>(excludeGeoTags->size());
			excludeFsIdx->resize(0);
		}
		for(auto it = excludeGeoTags->begin(); it != excludeGeoTags->end(); ++it)
		{
			SchedTreeBase::tFastTreeIdx idx;
			idx=entry->foregroundFastStruct->tag2NodeIdx->getClosestFastTreeNode(it->c_str());
			excludeFsIdx->push_back(idx);
		}
	}
	if(forceGeoTags)
	{
		forceBrIdx = new vector<SchedTreeBase::tFastTreeIdx>(forceGeoTags->size());
		excludeFsIdx->resize(0);
		for(auto it = forceGeoTags->begin(); it != forceGeoTags->end(); ++it)
		{
			SchedTreeBase::tFastTreeIdx idx;
			idx=entry->foregroundFastStruct->tag2NodeIdx->getClosestFastTreeNode(it->c_str());
			forceBrIdx->push_back(idx);
		}
	}

	// actually do the job
	bool success = false;
	switch(type)
	{
		case regularRO:
		case regularRW:
		success = placeNewReplicas(entry,nNewReplicas,&newReplicasIdx,entry->foregroundFastStruct->placementTree,
				existingReplicasIdx,excludeFsIdx,forceBrIdx,skipSaturatedPlct);
		break;
		case draining:
		success = placeNewReplicas(entry,nNewReplicas,&newReplicasIdx,entry->foregroundFastStruct->drnPlacementTree,
				existingReplicasIdx,excludeFsIdx,forceBrIdx,skipSaturatedDrnPlct);
		break;
		case balancing:
		success = placeNewReplicas(entry,nNewReplicas,&newReplicasIdx,entry->foregroundFastStruct->blcPlacementTree,
				existingReplicasIdx,excludeFsIdx,forceBrIdx,skipSaturatedBlcPlct);
		break;
		default:
		break;
	}
	if(!success) goto cleanup;

	// fill the resulting vector and
	// update the fastTree UlScore and DlScore by applying the penalties
	newReplicas->resize(0);
	for(auto it = newReplicasIdx.begin(); it != newReplicasIdx.end(); ++it)
	{
		const SchedTreeBase::tFastTreeIdx *idx=NULL;
		const unsigned int fsid = (*entry->foregroundFastStruct->treeInfo)[*it].fsId;
		newReplicas->push_back(fsid);
		// apply the penalties
		entry->foregroundFastStruct->fs2TreeIdx->get(fsid,idx);
		if(entry->foregroundFastStruct->placementTree->pNodes[*idx].fsData.dlScore>=0)
		applyDlScorePenalty(entry,*idx,plctDlScorePenalty);
		if(entry->foregroundFastStruct->placementTree->pNodes[*idx].fsData.ulScore>=0)
		applyUlScorePenalty(entry,*idx,plctUlScorePenalty);
	}

	// unlock, cleanup
	cleanup:
	entry->doubleBufferMutex.UnLockRead();
	AtomicDec(entry->fastStructLockWaitersCount);
	if(existingReplicasIdx) delete existingReplicasIdx;
	if(excludeFsIdx) delete excludeFsIdx;
	if(forceBrIdx) delete forceBrIdx;

	return success;
}

bool GeoTreeEngine::accessReplicasOneGroup(FsGroup* group, const size_t &nAccessReplicas,
		vector<FileSystem::fsid_t> *accessedReplicas,
		vector<FileSystem::fsid_t> *existingReplicas,
		SchedType type,
		const string &accesserGeotag,
		vector<FileSystem::fsid_t> *excludeFs,
		vector<string> *excludeGeoTags,
		vector<string> *forceGeoTags)
{

	// some basic checks
	assert(nAccessReplicas);
	assert(accessedReplicas);
	assert(existingReplicas);
	// check that enough replicas exist already
	if(nAccessReplicas > existingReplicas->size())
	return false;
	// if there is no choice, return all replicas
	if(nAccessReplicas == existingReplicas->size())
	{
		accessedReplicas->resize(0);
		accessedReplicas->insert(accessedReplicas->begin(),existingReplicas->begin(),existingReplicas->end());
		return true;
	}

	// find the entry in the map
	tlCurrentGroup = group;
	TreeMapEntry *entry;
	{
		RWMutexReadLock lock(this->pTreeMapMutex);
		if(!pGroup2TreeMapEntry.count(group))
		{
			eos_err("could not find the requested placement group in the map");
			return false;
		}
		entry = pGroup2TreeMapEntry[group];
		AtomicInc(entry->fastStructLockWaitersCount);
	}

	// readlock the original fast structure
	entry->doubleBufferMutex.LockRead();

	// locate the existing replicas and the excluded fs in the tree
	vector<SchedTreeBase::tFastTreeIdx> accessedReplicasIdx(nAccessReplicas),*existingReplicasIdx=NULL,*excludeFsIdx=NULL,*forceBrIdx=NULL;
	accessedReplicasIdx.resize(0);
	if(existingReplicas)
	{
		existingReplicasIdx = new vector<SchedTreeBase::tFastTreeIdx>(existingReplicas->size());
		existingReplicasIdx->resize(0);
		for(auto it = existingReplicas->begin(); it != existingReplicas->end(); ++it)
		{
			const SchedTreeBase::tFastTreeIdx *idx;
			if(!entry->foregroundFastStruct->fs2TreeIdx->get(*it,idx))
			{
				eos_warning("could not place preexisting replica on the fast tree");
				continue;
			}
			existingReplicasIdx->push_back(*idx);
		}
	}
	if(excludeFs)
	{
		excludeFsIdx = new vector<SchedTreeBase::tFastTreeIdx>(excludeFs->size());
		excludeFsIdx->resize(0);
		for(auto it = excludeFs->begin(); it != excludeFs->end(); ++it)
		{
			const SchedTreeBase::tFastTreeIdx *idx;
			if(!entry->foregroundFastStruct->fs2TreeIdx->get(*it,idx))
			{
				eos_warning("could not place excluded fs on the fast tree");
				continue;
			}
			excludeFsIdx->push_back(*idx);
		}
	}
	if(excludeGeoTags)
	{
		if(!excludeFsIdx)
		{
			excludeFsIdx = new vector<SchedTreeBase::tFastTreeIdx>(excludeGeoTags->size());
			excludeFsIdx->resize(0);
		}
		for(auto it = excludeGeoTags->begin(); it != excludeGeoTags->end(); ++it)
		{
			SchedTreeBase::tFastTreeIdx idx;
			idx=entry->foregroundFastStruct->tag2NodeIdx->getClosestFastTreeNode(it->c_str());
			excludeFsIdx->push_back(idx);
		}
	}
	if(forceGeoTags)
	{
		forceBrIdx = new vector<SchedTreeBase::tFastTreeIdx>(forceGeoTags->size());
		excludeFsIdx->resize(0);
		for(auto it = forceGeoTags->begin(); it != forceGeoTags->end(); ++it)
		{
			SchedTreeBase::tFastTreeIdx idx;
			idx=entry->foregroundFastStruct->tag2NodeIdx->getClosestFastTreeNode(it->c_str());
			forceBrIdx->push_back(idx);
		}
	}

	// find the closest tree node to the accesser
	SchedTreeBase::tFastTreeIdx accesserNode = entry->foregroundFastStruct->tag2NodeIdx->getClosestFastTreeNode(accesserGeotag.c_str());;

	// actually do the job
	bool success = false;
	switch(type)
	{
		case regularRO:
		success = accessReplicas(entry,nAccessReplicas,&accessedReplicasIdx,accesserNode,existingReplicasIdx,
				entry->foregroundFastStruct->rOAccessTree,excludeFsIdx,forceBrIdx,skipSaturatedAccess);
		break;
		case regularRW:
		success = accessReplicas(entry,nAccessReplicas,&accessedReplicasIdx,accesserNode,existingReplicasIdx,
				entry->foregroundFastStruct->rWAccessTree,excludeFsIdx,forceBrIdx,skipSaturatedAccess);
		break;
		case draining:
		success = accessReplicas(entry,nAccessReplicas,&accessedReplicasIdx,accesserNode,existingReplicasIdx,
				entry->foregroundFastStruct->drnAccessTree,excludeFsIdx,forceBrIdx,skipSaturatedDrnAccess);
		break;
		case balancing:
		success = accessReplicas(entry,nAccessReplicas,&accessedReplicasIdx,accesserNode,existingReplicasIdx,
				entry->foregroundFastStruct->blcAccessTree,excludeFsIdx,forceBrIdx,skipSaturatedBlcAccess);
		break;
		default:
		break;
	}
	if(!success) goto cleanup;

	// fill the resulting vector
	// update the fastTree UlScore and DlScore by applying the penalties
	accessedReplicas->resize(0);
	for(auto it = accessedReplicasIdx.begin(); it != accessedReplicasIdx.end(); ++it)
	{
		const SchedTreeBase::tFastTreeIdx *idx;
		const unsigned int fsid = (*entry->foregroundFastStruct->treeInfo)[*it].fsId;
		accessedReplicas->push_back(fsid);
		// apply the penalties
		entry->foregroundFastStruct->fs2TreeIdx->get(fsid,idx);
		if(entry->foregroundFastStruct->placementTree->pNodes[*idx].fsData.dlScore<100)
		applyDlScorePenalty(entry,*idx,accessDlScorePenalty);
		if(entry->foregroundFastStruct->placementTree->pNodes[*idx].fsData.ulScore<100)
		applyUlScorePenalty(entry,*idx,accessUlScorePenalty);
	}

	// unlock, cleanup
	cleanup:
	entry->doubleBufferMutex.UnLockRead();
	AtomicDec(entry->fastStructLockWaitersCount);
	if(existingReplicasIdx) delete existingReplicasIdx;
	if(excludeFsIdx) delete excludeFsIdx;
	if(forceBrIdx) delete forceBrIdx;

	return success;
}

/// helper class to order filesystems for a given geoscore
struct FsComparator
{
	eos::mgm::SchedTreeBase::TreeNodeSlots freeSlot;
	GeoTreeEngine::SchedType type;
	FsComparator(GeoTreeEngine::SchedType _type) : type(_type)
	{	freeSlot.freeSlotsCount=1;};
	int operator() (const tuple< FileSystem::fsid_t , GeoTreeEngine::TreeMapEntry* , SchedTreeBase::tFastTreeIdx > & left, const tuple< FileSystem::fsid_t , GeoTreeEngine::TreeMapEntry* , SchedTreeBase::tFastTreeIdx > & right) const
	{
		switch(type)
		{
			case GeoTreeEngine::regularRO:
			return eos::mgm::FastROAccessTree::compareAccess(
					& get<1>(left)->foregroundFastStruct->rOAccessTree->pNodes[std::get<2>(left)].fsData,
					&freeSlot,
					& get<1>(right)->foregroundFastStruct->rOAccessTree->pNodes[std::get<2>(right)].fsData,
					&freeSlot
			);
			break;
			case GeoTreeEngine::regularRW:
			return eos::mgm::FastROAccessTree::compareAccess(
					& get<1>(left)->foregroundFastStruct->rWAccessTree->pNodes[std::get<2>(left)].fsData,
					&freeSlot,
					& get<1>(right)->foregroundFastStruct->rWAccessTree->pNodes[std::get<2>(right)].fsData,
					&freeSlot
			);
			break;
			case GeoTreeEngine::draining:
			return eos::mgm::FastROAccessTree::compareAccess(
					& get<1>(left)->foregroundFastStruct->drnAccessTree->pNodes[std::get<2>(left)].fsData,
					&freeSlot,
					& get<1>(right)->foregroundFastStruct->drnAccessTree->pNodes[std::get<2>(right)].fsData,
					&freeSlot
			);
			break;
			case GeoTreeEngine::balancing:
			return eos::mgm::FastROAccessTree::compareAccess(
					& get<1>(left)->foregroundFastStruct->blcAccessTree->pNodes[std::get<2>(left)].fsData,
					&freeSlot,
					& get<1>(right)->foregroundFastStruct->blcAccessTree->pNodes[std::get<2>(right)].fsData,
					&freeSlot
			);
			break;
			default:
			break;
		}
		return 0;
	}
};

bool GeoTreeEngine::accessReplicasMultipleGroup(const size_t &nAccessReplicas,
		vector<FileSystem::fsid_t> *accessedReplicas,
		vector<FileSystem::fsid_t> *existingReplicas,
		SchedType type,
		const string &accesserGeotag,
		vector<FileSystem::fsid_t> *excludeFs,
		vector<string> *excludeGeoTags,
		vector<string> *forceGeoTags)
{

	// some basic checks
	assert(nAccessReplicas);
	assert(accessedReplicas);
	assert(existingReplicas);
	// check that enough replicas exist already
	if(nAccessReplicas > existingReplicas->size())
	{
		eos_static_debug("not enough replica");
		return false;
	}

	// if there is no choice, return all replicas
	if(nAccessReplicas == existingReplicas->size())
	{
		accessedReplicas->resize(0);
		accessedReplicas->insert(accessedReplicas->begin(),existingReplicas->begin(),existingReplicas->end());
		return true;
	}

	// find the group holdings the fs of the existing replicas
	map<TreeMapEntry*,vector<FileSystem::fsid_t>> entry2FsId;
	TreeMapEntry *entry=NULL;
	{
		RWMutexReadLock lock(this->pTreeMapMutex);
		for(auto exrepIt = existingReplicas->begin(); exrepIt != existingReplicas->end(); exrepIt++)
		{
			auto mentry = pFs2TreeMapEntry.find(*exrepIt);
			// if we cannot find the fs in any group, there is an inconsistency somewhere
			if(mentry == pFs2TreeMapEntry.end())
			{
				eos_static_err("cannot find the existing replica in any scheduling group");
				return false;
			}
			entry = mentry->second;
			entry2FsId[entry].push_back(*exrepIt);
		}
		// to prevent any change of the trees
		entry->doubleBufferMutex.LockRead();
		// to prevent the destruction of the entry
		AtomicInc(entry->fastStructLockWaitersCount);
	}

	// if we have only one group , we use the one group procedure
	if(entry2FsId.size()==1)
	{
		entry = entry2FsId.begin()->first;
		eos_static_debug("existing replicas are only in one group, using accessReplicasOneGroup");
		// to prevent the destruction of the entry
		entry->doubleBufferMutex.UnLockRead();
		AtomicDec(entry->fastStructLockWaitersCount);
		return accessReplicasOneGroup( entry->group, nAccessReplicas,
				accessedReplicas,existingReplicas,
				type,accesserGeotag,excludeFs,
				excludeGeoTags,forceGeoTags);
	}

	// we have multiple groups
	eos::mgm::ROAccessPriorityComparator comp;
	eos::mgm::SchedTreeBase::TreeNodeSlots freeSlot;
	freeSlot.freeSlotsCount=1;

	// compute their geolocation score
	size_t availFsCount = 0;
	map< unsigned , std::vector< tuple< FileSystem::fsid_t , TreeMapEntry* , SchedTreeBase::tFastTreeIdx > > > geoScore2Fs;
	for(auto entryIt = entry2FsId.begin(); entryIt != entry2FsId.end(); entryIt ++)
	for(auto fsIt = entryIt->second.begin(); fsIt != entryIt->second.end(); fsIt++)
	{
		const SchedTreeBase::tFastTreeIdx *idx;
		if(!entryIt->first->foregroundFastStruct->fs2TreeIdx->get(*fsIt,idx) )
		{
			eos_static_warning("cannot find fs in the group in the 2nd pass");
			continue;
		}
		// check if the fs is available
		bool isValid = false;
		switch(type)
		{
			case regularRO:
			comp.isValidSlot(&entryIt->first->foregroundFastStruct->rOAccessTree->pNodes[*idx].fsData,&freeSlot);
			break;
			case regularRW:
			comp.isValidSlot(&entryIt->first->foregroundFastStruct->rWAccessTree->pNodes[*idx].fsData,&freeSlot);
			break;
			case draining:
			comp.isValidSlot(&entryIt->first->foregroundFastStruct->drnAccessTree->pNodes[*idx].fsData,&freeSlot);
			break;
			case balancing:
			comp.isValidSlot(&entryIt->first->foregroundFastStruct->blcAccessTree->pNodes[*idx].fsData,&freeSlot);
			break;
			default:
			break;
		}
		if(!isValid)
		{
			eos_static_debug("fs skipped because unavailable");
			continue;
		}

		const string &fsGeotag = (*entryIt->first->foregroundFastStruct->treeInfo)[*idx].fullGeotag;
		unsigned geoScore = 0;
		size_t kmax = min(accesserGeotag.length(),fsGeotag.length());
		for(size_t k=0; k<kmax; k++)
		{
			if(accesserGeotag[k]!=fsGeotag[k])
			break;
			if(accesserGeotag[k]==':' && k+1 < kmax && accesserGeotag[k+1]==':')
			geoScore++;
		}
		geoScore2Fs[geoScore].push_back(make_tuple(*fsIt,entryIt->first,*idx));
		availFsCount++;
	}

	// check we have enough available fs
	if(availFsCount<nAccessReplicas)
	{
		for(auto it = entry2FsId.begin(); it != entry2FsId.end(); it++ )
		{
			it->first->doubleBufferMutex.UnLockRead();
			AtomicDec(it->first->fastStructLockWaitersCount);
		}
		eos_static_debug("not enough replica available");
		return false;
	}

	FsComparator fscomp(type);
	size_t fsToGet = nAccessReplicas;
	for(auto geoscoreIt = geoScore2Fs.begin(); geoscoreIt != geoScore2Fs.end(); geoscoreIt++)
	{
		// sort in descending order.
		std::sort(geoscoreIt->second.begin() , geoscoreIt->second.end() , fscomp);
		size_t n = min(fsToGet,geoscoreIt->second.size());
		for(auto it=geoscoreIt->second.begin(); it!=geoscoreIt->second.begin()+n; it++)
		accessedReplicas->push_back(get<0>(*it));
		fsToGet -= n;
		if(fsToGet==0) break;
	}
	if(fsToGet)
	{
		eos_err("inconsistency : could not retrieve enough fs");
		for(auto it = entry2FsId.begin(); it != entry2FsId.end(); it++ )
		{
			it->first->doubleBufferMutex.UnLockRead();
			AtomicDec(it->first->fastStructLockWaitersCount);
		}
		accessedReplicas->clear();
		return false;
	}

	// cleanup and exit
	for(auto it = entry2FsId.begin(); it != entry2FsId.end(); it++ )
	{
		it->first->doubleBufferMutex.UnLockRead();
		AtomicDec(it->first->fastStructLockWaitersCount);
	}
	return true;
}

bool GeoTreeEngine::StartUpdater()
{
	if (XrdSysThread::Run(&pUpdaterTid, GeoTreeEngine::startFsChangeListener, static_cast<void *>(this),
					XRDSYSTHREAD_HOLD, "GeoTreeEngine Updater"))
	{
		return false;
	}
	return true;
}

bool GeoTreeEngine::StopUpdater()
{
	XrdSysThread::Cancel(pUpdaterTid);
	XrdSysThread::Join(pUpdaterTid, 0);
	return true;
}

void* GeoTreeEngine::startFsChangeListener(void *pp)
{
	((GeoTreeEngine*)pp)->listenFsChange();
	return 0;
}

void GeoTreeEngine::listenFsChange()
{
	gOFS->ObjectNotifier.BindCurrentThread("geotreeengine");

	if(!gOFS->ObjectNotifier.StartNotifyCurrentThread())
	eos_crit("error starting shared objects change notifications");
	else
	eos_info("GeoTreeEngine updater is starting...");

	struct timespec curtime,prevtime;
	clock_gettime(CLOCK_MONOTONIC_COARSE,&prevtime);
	curtime = prevtime;

	do
	{
		gOFS->ObjectNotifier.tlSubscriber->SubjectsSem.Wait();

		XrdSysThread::SetCancelOff();

		// we always take a lock to take something from the queue and then release it
		gOFS->ObjectNotifier.tlSubscriber->SubjectsMutex.Lock();

		// listens on modifications on filesystem objects
		while (gOFS->ObjectNotifier.tlSubscriber->NotificationSubjects.size())
		{
			XrdMqSharedObjectManager::Notification event;
			event = gOFS->ObjectNotifier.tlSubscriber->NotificationSubjects.front();
			gOFS->ObjectNotifier.tlSubscriber->NotificationSubjects.pop_front();
			gOFS->ObjectNotifier.tlSubscriber->SubjectsMutex.UnLock();

			string newsubject = event.mSubject.c_str();

			if (event.mType == XrdMqSharedObjectManager::kMqSubjectCreation)
			{
				// ---------------------------------------------------------------------
				// handle subject creation
				// ---------------------------------------------------------------------
				eos_warning("received creation on subject %s : don't know what to do with this!", newsubject.c_str());

				gOFS->ObjectNotifier.tlSubscriber->SubjectsMutex.Lock();
				continue;
			}

			if (event.mType == XrdMqSharedObjectManager::kMqSubjectDeletion)
			{
				// ---------------------------------------------------------------------
				// handle subject deletion
				// ---------------------------------------------------------------------
				eos_warning("received deletion on subject %s : don't know what to do with this!", newsubject.c_str());

				gOFS->ObjectNotifier.tlSubscriber->SubjectsMutex.Lock();
				continue;
			}

			if (event.mType == XrdMqSharedObjectManager::kMqSubjectModification)
			{
				// ---------------------------------------------------------------------
				// handle subject modification
				// ---------------------------------------------------------------------

				eos_debug("received modification on subject %s", newsubject.c_str());

				string key = newsubject;
				string queue = newsubject;
				size_t dpos = 0;
				if ((dpos = queue.find(";")) != string::npos)
				{
					key.erase(0, dpos + 1);
					queue.erase(dpos);
				}

				// store the notification for the next update
				if(gNotificationsBuffer.count(queue))
				{
					gNotificationsBuffer[queue] |= gNotifKey2Enum.at(key);
				}
				else
				{
					gNotificationsBuffer[queue] = gNotifKey2Enum.at(key);
				}

				gOFS->ObjectNotifier.tlSubscriber->SubjectsMutex.Lock();
				continue;
			}

			if (event.mType == XrdMqSharedObjectManager::kMqSubjectKeyDeletion)
			{
				// ---------------------------------------------------------------------
				// handle subject key deletion
				// ---------------------------------------------------------------------
				eos_warning("received subject deletion on subject %s : don't know what to do with this!", newsubject.c_str());

				gOFS->ObjectNotifier.tlSubscriber->SubjectsMutex.Lock();
				continue;
			}
			eos_warning("msg=\"don't know what to do with subject\" subject=%s", newsubject.c_str());
			gOFS->ObjectNotifier.tlSubscriber->SubjectsMutex.Lock();
			continue;
		}
		gOFS->ObjectNotifier.tlSubscriber->SubjectsMutex.UnLock();
		// do the processing
		clock_gettime(CLOCK_MONOTONIC_COARSE,&curtime);
		if(!gUpdaterPaused && (curtime.tv_sec*1000+curtime.tv_nsec/1000-prevtime.tv_sec*1000-prevtime.tv_nsec/1000 > 1000))
		{
			checkPendingDeletions(); // do it before tree info to leave some time to the other threads
			updateTreeInfo(gNotificationsBuffer);
			prevtime = curtime;
			gNotificationsBuffer.clear();
		}
		XrdSysThread::SetCancelOff();
	}
	while (1);
}

bool GeoTreeEngine::updateTreeInfo(TreeMapEntry* entry, eos::common::FileSystem::fs_snapshot_t *fs, int keys, SchedTreeBase::tFastTreeIdx ftIdx , SlowTreeNode *stn)
{
	eos::common::RWMutexReadLock lock(configMutex); // we git a consistent set of configuration parameters per refresh of the state
	// nothing to update
	if((!ftIdx && !stn) || !keys)
	return true;

#define setOneStateVarInAllFastTrees(variable,value) \
    { \
  entry->backgroundFastStruct->rOAccessTree->pNodes[ftIdx].fsData.variable = value; \
  entry->backgroundFastStruct->rWAccessTree->pNodes[ftIdx].fsData.variable = value; \
  entry->backgroundFastStruct->placementTree->pNodes[ftIdx].fsData.variable = value; \
  entry->backgroundFastStruct->drnAccessTree->pNodes[ftIdx].fsData.variable = value; \
  entry->backgroundFastStruct->drnPlacementTree->pNodes[ftIdx].fsData.variable = value; \
  entry->backgroundFastStruct->blcAccessTree->pNodes[ftIdx].fsData.variable = value; \
  entry->backgroundFastStruct->blcPlacementTree->pNodes[ftIdx].fsData.variable = value; \
    }

#define setOneStateVarStatusInAllFastTrees(flag) \
    { \
  entry->backgroundFastStruct->rOAccessTree->pNodes[ftIdx].fsData.mStatus |= flag; \
  entry->backgroundFastStruct->rWAccessTree->pNodes[ftIdx].fsData.mStatus |= flag; \
  entry->backgroundFastStruct->placementTree->pNodes[ftIdx].fsData.mStatus |= flag; \
  entry->backgroundFastStruct->drnAccessTree->pNodes[ftIdx].fsData.mStatus |= flag; \
  entry->backgroundFastStruct->drnPlacementTree->pNodes[ftIdx].fsData.mStatus |= flag; \
  entry->backgroundFastStruct->blcAccessTree->pNodes[ftIdx].fsData.mStatus |= flag; \
  entry->backgroundFastStruct->blcPlacementTree->pNodes[ftIdx].fsData.mStatus |= flag; \
    }

#define unsetOneStateVarStatusInAllFastTrees(flag) \
    { \
  entry->backgroundFastStruct->rOAccessTree->pNodes[ftIdx].fsData.mStatus &= ~flag; \
  entry->backgroundFastStruct->rWAccessTree->pNodes[ftIdx].fsData.mStatus &= ~flag; \
  entry->backgroundFastStruct->placementTree->pNodes[ftIdx].fsData.mStatus &= ~flag; \
  entry->backgroundFastStruct->drnAccessTree->pNodes[ftIdx].fsData.mStatus &= ~flag; \
  entry->backgroundFastStruct->drnPlacementTree->pNodes[ftIdx].fsData.mStatus &= ~flag; \
  entry->backgroundFastStruct->blcAccessTree->pNodes[ftIdx].fsData.mStatus &= ~flag; \
  entry->backgroundFastStruct->blcPlacementTree->pNodes[ftIdx].fsData.mStatus &= ~flag; \
    }

	if(keys&sfgGeotag)
	{
		// update the treenodeinfo
		string newGeoTag = fs->mGeoTag;
		FileSystem::fsid_t fsid = fs->mId;
		if(!fsid)
		{
			eos_err("could not get the FsId");
			return false;
		}
		entry->slowTreeMutex.LockWrite();
		if(!entry->fs2SlowTreeNode.count(fsid))
		{
			eos_err("could not get the slowtree node");
			entry->slowTreeMutex.UnLockWrite();
			return false;
		}
		SlowTreeNode *oldNode = entry->fs2SlowTreeNode[fsid];

		const string &oldGeoTag = oldNode->pNodeInfo.fullGeotag;
		eos_debug("geotag change detected : old geotag is %s   new geotag is %s",oldGeoTag.substr(0,oldGeoTag.rfind("::")).c_str(),newGeoTag.c_str());
		if(oldGeoTag.substr(0,oldGeoTag.rfind("::"))!=newGeoTag)
		{ // do the change only if there is one
			SlowTreeNode *newNode = NULL;
			newNode = entry->slowTree->moveToNewGeoTag(oldNode,newGeoTag);
			if(!newNode)
			{
				eos_err("error changing geotag in slowtree");
				entry->slowTreeMutex.UnLockWrite();
				return false;
			}
			entry->slowTreeModified = true;
			entry->fs2SlowTreeNode[fsid] = newNode;
			// !!! change the argument too
			stn = newNode;
		}
		entry->slowTreeMutex.UnLockWrite();
	}
	if(keys&sfgId)
	{
		//CHECK IF CHANGE ACTUALLY HAPPENED BEFORE ACTUALLY CHANGING SOMETHING
		// should not happen
		eos_crit("the FsId should not change once it's created:  new value is %lu",(unsigned long)fs->mId);
	}
	if(keys&(sfgBoot|sfgActive|sfgErrc))
	{
		FileSystem::fsstatus_t statboot = fs->mStatus;
		unsigned int errc = fs->mErrCode;

		FileSystem::fsactive_t statactive = fs->mActiveStatus;

		if( (statboot==FileSystem::kBooted) &&
				(errc == 0) &&		// this we probably don't need
				(statactive==FileSystem::kOnline)// this checks the heartbeat and the group & node are enabled
		)
		{ // the fs is available
			eos_debug("fs %lu is getting available  ftidx=%d  stn=%p",(unsigned long) fs->mId,(int)ftIdx,stn);
			if(ftIdx) setOneStateVarStatusInAllFastTrees(SchedTreeBase::Available);
			if(stn) stn->pNodeState.mStatus |= SchedTreeBase::Available;
		}
		else
		{ // the fs is unavailable
			eos_info("fs %lu is getting unavailable ftidx=%d  stn=%p",(unsigned long) fs->mId,(int)ftIdx,stn);
			if(ftIdx) unsetOneStateVarStatusInAllFastTrees(SchedTreeBase::Available);
			if(stn) stn->pNodeState.mStatus &= ~SchedTreeBase::Available;
		}
	}
	if(keys&sfgConfigstatus)
	{
		FileSystem::fsstatus_t status = fs->mConfigStatus;
		if(status==FileSystem::kRW)
		{
			if(ftIdx) setOneStateVarStatusInAllFastTrees(SchedTreeBase::Readable|SchedTreeBase::Writable);
			if(stn) stn->pNodeState.mStatus |= (SchedTreeBase::Readable|SchedTreeBase::Writable);
		}
		else if(status==FileSystem::kRO)
		{
			if(ftIdx)
			{
				setOneStateVarStatusInAllFastTrees(SchedTreeBase::Readable);
				unsetOneStateVarStatusInAllFastTrees(SchedTreeBase::Writable);
			}
			if(stn)
			{
				stn->pNodeState.mStatus |= SchedTreeBase::Readable;
				stn->pNodeState.mStatus &= ~SchedTreeBase::Writable;
			}
		}
		else if(status==FileSystem::kWO)
		{
			if(ftIdx)
			{
				unsetOneStateVarStatusInAllFastTrees(SchedTreeBase::Readable);
				setOneStateVarStatusInAllFastTrees(SchedTreeBase::Writable);
			}
			if(stn)
			{
				stn->pNodeState.mStatus &= ~SchedTreeBase::Readable;
				stn->pNodeState.mStatus |= SchedTreeBase::Writable;
			}
		}
	}
	if(keys&sfgDrain)
	{
		FileSystem::fsstatus_t drainStatus = fs->mDrainStatus;
		switch(drainStatus)
		{
			case FileSystem::kNoDrain:
			case FileSystem::kDrainPrepare:
			case FileSystem::kDrainWait:
			case FileSystem::kDrainStalling:
			case FileSystem::kDrained:
			case FileSystem::kDrainExpired:
			case FileSystem::kDrainLostFiles:
			if(ftIdx) unsetOneStateVarStatusInAllFastTrees(SchedTreeBase::Draining);
			if(stn) stn->pNodeState.mStatus &= ~SchedTreeBase::Draining;
			// mark as unavailable for read/write
			break;
			case FileSystem::kDraining:
			// mark as draining
			if(ftIdx) setOneStateVarStatusInAllFastTrees(SchedTreeBase::Draining);
			if(stn) stn->pNodeState.mStatus |= SchedTreeBase::Draining;
			break;
		}
	}
	if(keys&sfgBalthres)
	{
		if(fs->mBalRunning)
		{
			if(ftIdx) setOneStateVarStatusInAllFastTrees(SchedTreeBase::Balancing);
			if(stn) stn->pNodeState.mStatus |= SchedTreeBase::Balancing;
		}
		else
		{
			if(ftIdx) unsetOneStateVarStatusInAllFastTrees(SchedTreeBase::Balancing);
			if(stn) stn->pNodeState.mStatus &= ~SchedTreeBase::Balancing;
		}
	}
	if(keys&sfgFsfilled)
	{
		//half fr = half(fs->mNominalFilled);
		float fr = float(fs->mNominalFilled);
		if(ftIdx) setOneStateVarInAllFastTrees(fillRatio,fr);
		if(stn) stn->pNodeState.fillRatio = fr;
	}

	if(keys&sfgBlkavailb)
	{
		float ts = float(fs->mDiskBfree * (double)fs->mDiskBsize );

		if(ftIdx) setOneStateVarInAllFastTrees(totalSpace,ts);
		if(stn) stn->pNodeState.totalSpace = ts;
	}
	if(keys&(sfgDiskload|sfgInratemib|sfgWriteratemb))
	{
		// update the upload score
		double ulScore = (1-fs->mDiskUtilization);
		double netoutweight = (1.0 - ((fs->mNetEthRateMiB) ? (fs->mNetOutRateMiB / fs->mNetEthRateMiB) : 0.0));
		ulScore *= ((netoutweight > 0) ? sqrt(netoutweight) : 0);

		if(ftIdx) setOneStateVarInAllFastTrees(ulScore,(SchedTreeBase::tFastTreeIdx)(ulScore*100));
		if(stn) stn->pNodeState.ulScore = ulScore*100;
	}
	if(keys&(sfgOutratemib|sfgDiskload|sfgReadratemb))
	{
		double dlScore = (1-fs->mDiskUtilization);
		double netinweight = (1.0 - ((fs->mNetEthRateMiB) ? (fs->mNetInRateMiB / fs->mNetEthRateMiB) : 0.0));
		dlScore *= ((netinweight > 0) ? sqrt(netinweight) : 0);

		if(ftIdx) setOneStateVarInAllFastTrees(dlScore,(SchedTreeBase::tFastTreeIdx)(dlScore*100));
		if(stn) stn->pNodeState.dlScore = dlScore*100;

	}
	if(keys&sfgFsfilled)
	{
		if(ftIdx) setOneStateVarInAllFastTrees(fillRatio,(SchedTreeBase::tFastTreeIdx)(fs->mDiskFilled*100));
		if(stn) stn->pNodeState.fillRatio = fs->mDiskFilled*100;
	}

	// SHOULD WE TAKE THE NOMINAL FILLING AS SET BY THE BALANCING?
//	if(keys&(sfgNomfilled))
//	{
//		fs->
//	}

	return true;
}

bool GeoTreeEngine::updateTreeInfo(const map<string,int> &updates)
{
	// copy the foreground FastStructures to the BackGround FastStructures
	// so that the penalties applied after the placement/access are kept by defaut
	// (and overwritten if a new state is received from the fs)
	pTreeMapMutex.LockRead();
	for(auto it = pGroup2TreeMapEntry.begin(); it != pGroup2TreeMapEntry.end(); it++ )
	{
		TreeMapEntry *entry = it->second;
		RWMutexReadLock lock(entry->slowTreeMutex);
		if(!entry->foregroundFastStruct->DeepCopyTo(entry->backgroundFastStruct))
		{
			eos_crit("error deep copying in double buffering");
			return false;
		}
	}
	pTreeMapMutex.UnLockRead();

	for(auto it = updates.begin(); it != updates.end(); ++it)
	{

		gOFS->ObjectManager.HashMutex.LockRead();
		XrdMqSharedHash* hash = gOFS->ObjectManager.GetObject(it->first.c_str(), "hash");
		FileSystem::fsid_t fsid = (FileSystem::fsid_t) hash->GetLongLong("id");
		gOFS->ObjectManager.HashMutex.UnLockRead();

		FsView::gFsView.ViewMutex.LockRead();
		if(!FsView::gFsView.mIdView.count(fsid))
		return false;
		eos::common::FileSystem *filesystem = FsView::gFsView.mIdView[fsid];
		FsView::gFsView.ViewMutex.UnLockRead();
		eos::common::FileSystem::fs_snapshot_t fs;
		filesystem->SnapShotFileSystem(fs, true);

		pTreeMapMutex.LockRead();
		if(!pFs2TreeMapEntry.count(fsid))
		{
			eos_err("update : TreeEntryMap has been removed, skipping this update");
			pTreeMapMutex.UnLockRead();
			continue;
		}
		TreeMapEntry *entry = pFs2TreeMapEntry[fsid];
		AtomicInc(entry->fastStructLockWaitersCount);
		pTreeMapMutex.UnLockRead();

		eos_debug("CHANGE BITFIELD %x",it->second);

		// update only the fast structures because even if a fast structure rebuild is needed from the slow tree
		// its information and state is updated from the fast structures
		entry->doubleBufferMutex.LockRead();
		const SchedTreeBase::tFastTreeIdx *idx=NULL;
		SlowTreeNode *node=NULL;
		if( !entry->backgroundFastStruct->fs2TreeIdx->get(fsid,idx) )
		{
			auto nodeit = entry->fs2SlowTreeNode.find(fsid);
			if(nodeit == entry->fs2SlowTreeNode.end())
			{
				eos_crit("inconsistency : cannot locate an fs %lu supposed to be in the fast structures",(unsigned long)fsid);
				entry->doubleBufferMutex.UnLockRead();
				AtomicDec(entry->fastStructLockWaitersCount);
				return false;
			}
			node = nodeit->second;
			eos_debug("no fast tree for fs %lu : updating slowtree",(unsigned long)fsid);
		}
		else
		{
			eos_debug("fast tree available for fs %lu : not updating slowtree",(unsigned long)fsid);
		}
		updateTreeInfo(entry, &fs, it->second, idx?*idx:0 , node);
		if(idx) entry->fastStructModified = true;
		if(node) entry->slowTreeModified = true;
		// if we update the slowtree, then a fast tree generation is already pending
		entry->doubleBufferMutex.UnLockRead();
		AtomicDec(entry->fastStructLockWaitersCount);
	}

	// update the trees that need to be updated ( could maybe optimized by updating only the branch needing, might be worth it if only 1 or 2 branches are updated )
	// self update for the fast structure if update from slow tree is not needed
	// if convert from slowtree is needed, update the slowtree from the fast for the info and for the state
	pTreeMapMutex.LockRead();
	for(auto it = pGroup2TreeMapEntry.begin(); it != pGroup2TreeMapEntry.end(); it++ )
	{
		TreeMapEntry *entry = it->second;
		RWMutexReadLock lock(entry->slowTreeMutex);
		if(!updateFastStructures(entry))
		{
			pTreeMapMutex.UnLockRead();
			eos_err("updating the tree");
			return false;
		}
	}
	pTreeMapMutex.UnLockRead();

	return true;
}

EOSMGMNAMESPACE_END
