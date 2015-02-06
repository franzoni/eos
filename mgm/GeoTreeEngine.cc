// ----------------------------------------------------------------------
// File: GeoTreeEngine.cc
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
GeoTreeEngine::sfgErrc = 1 << 17,
GeoTreeEngine::sfgPubTmStmp = 1 << 18;

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
  make_pair("stat.net.ethratemib",sfgEthmib),
  make_pair("stat.net.inratemib",sfgInratemib),
  make_pair("stat.net.outratemib",sfgOutratemib),
  make_pair("stat.errc",sfgErrc),
  make_pair("stat.publishtimestamp",sfgPubTmStmp)
};

map<string,int> GeoTreeEngine::gNotificationsBuffer;
bool GeoTreeEngine::gUpdaterPaused = false;

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
  ft->penalties->resize(slowTree->getNodeCount());

  return true;
}

bool GeoTreeEngine::forceRefresh()
{
  // signal a pause to the background updating
  PauseUpdater();

  // prevent any other use of the fast structures
  pAddRmFsMutex.LockWrite();
  pTreeMapMutex.LockWrite();

  // mark all fs needing a refresh for all the watched attributes
  for(auto it=pFsId2FsPtr.begin(); it!=pFsId2FsPtr.end(); it++)
    gNotificationsBuffer[it->second->GetQueuePath()]=(~0);

  for(auto it = pGroup2TreeMapEntry.begin(); it != pGroup2TreeMapEntry.end(); it++)
  {
  it->second->fastStructModified = true;
  it->second->slowTreeModified = true;
  }

  // do the update
  pTreeMapMutex.UnLockWrite();
  updateTreeInfo(gNotificationsBuffer);
  pAddRmFsMutex.UnLockWrite();
  // signal a resume to the background updating
  ResumeUpdater();

  return true;
}

bool GeoTreeEngine::insertFsIntoGroup(FileSystem *fs ,
    FsGroup *group,
    bool updateFastStruct)
{
  eos::common::RWMutexWriteLock lock(pAddRmFsMutex);

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
      mapEntry = new TreeMapEntry(group->mName.c_str());
      updateFastStruct = true; // force update to be sure that the fast structures are properly created
#ifdef EOS_GEOTREEENGINE_USE_INSTRUMENTED_MUTEX
#ifdef EOS_INSTRUMENTED_RWMUTEX
      char buffer[64],buffer2[64];
      sprintf(buffer,"GTE %s doublebuffer",group->mName.c_str());
      sprintf(buffer2,"%s doublebuffer",group->mName.c_str());
      mapEntry->doubleBufferMutex.SetDebugName(buffer2);
      int retcode = eos::common::RWMutex::AddOrderRule(buffer,std::vector<eos::common::RWMutex*>(
	      { &pAddRmFsMutex,&pTreeMapMutex,&mapEntry->doubleBufferMutex}));
      eos_static_info("creating RWMutex rule order %p, retcode is %d",&mapEntry->doubleBufferMutex, retcode);

      sprintf(buffer,"GTE %s slowtree",group->mName.c_str());
      sprintf(buffer2,"%s slowtree",group->mName.c_str());
      mapEntry->slowTreeMutex.SetDebugName(buffer2);
      retcode = eos::common::RWMutex::AddOrderRule(buffer,std::vector<eos::common::RWMutex*>(
	      { &pAddRmFsMutex,&pTreeMapMutex,&mapEntry->slowTreeMutex}));
      eos_static_info("creating RWMutex rule order %p, retcode is %d",&mapEntry->slowTreeMutex, retcode);
#endif
#endif
    }
    mapEntry->slowTreeMutex.LockWrite();
    pTreeMapMutex.UnLockWrite();
  }

  // ==== fill the entry
  // create new TreeNodeInfo/TreeNodeState pair and update its data
  eos::common::FileSystem::fs_snapshot_t fsn;
  fs->SnapShotFileSystem(fsn, true);

  // check if there is still some space for a new fs
  {
    size_t depth=1;
    std::string sub("::");
    for (size_t offset = fsn.mGeoTag.find(sub); offset != std::string::npos;
             offset = fsn.mGeoTag.find(sub, offset + sub.length())) depth++;
    if(depth + mapEntry->slowTree->getNodeCount() > SchedTreeBase::sGetMaxNodeCount()-2 )
    {
      mapEntry->slowTreeMutex.UnLockWrite();

      eos_err("error inserting fs %lu into group %s : the group-tree is full",
          (unsigned long)fsid,
          group->mName.c_str()
      );

      return false;
    }
  }

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
  info.netSpeedClass = (unsigned char)round(log10(fsn.mNetEthRateMiB*8 * 1024 * 1024 + 1));
  info.netSpeedClass = info.netSpeedClass>8 ? info.netSpeedClass-8 : (unsigned char)0; // netSpeedClass 1 means 1Gbps
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

  // ==== update the penalties vectors if necessary
  if(!pFsId2FsPtr.empty() && fsn.mId>(--pFsId2FsPtr.end())->first)
  {
    for(auto it=pCircFrCnt2FsPenalties.begin(); it!=pCircFrCnt2FsPenalties.end(); it++)
    {
      it->resize(fsn.mId+1);
    }
    pFsId2LatencyStats.resize(fsn.mId+1);
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
    pFsId2FsPtr[fsid] = fs;
    pTreeMapMutex.UnLockWrite();
    mapEntry->slowTreeMutex.UnLockWrite();
  }

  if(eos::common::Logging::gLogMask & LOG_INFO)
  {
    stringstream ss;
    ss << (*mapEntry->slowTree);

    eos_debug("inserted fs %lu into group %s geotag is %s and fullgeotag is %s\n%s",
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
  eos::common::RWMutexWriteLock lock(pAddRmFsMutex);

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

  // ==== update the shared object notifications
  {
    if(!gOFS->ObjectNotifier.UnsubscribesToSubjectAndKey("geotreeengine",fs->GetQueuePath(),gWatchedKeys,XrdMqSharedObjectChangeNotifier::kMqSubjectStrictModification))
    {
      pTreeMapMutex.UnLockWrite();
      eos_crit("error removing fs %lu into group %s : error unsubscribing to shared object notifications",
	  (unsigned long)fsid,
	  group->mName.c_str()
      );
      return false;
    }
  }

  // ==== discard updates about this fs
  // ==== clean the notifications buffer
  gNotificationsBuffer.erase(fs->GetQueuePath());
  // ==== clean the thread-local notification queue
  {
    XrdMqSharedObjectChangeNotifier::Subscriber *subscriber = gOFS->ObjectNotifier.GetSubscriberFromCatalog("geotreeengine",false);
    subscriber->SubjectsMutex.Lock();
    for ( auto it = subscriber->NotificationSubjects.begin();
	it != subscriber->NotificationSubjects.end(); it++ )
    {
      // to mark the filesystem as removed, we change the notification type flag
      if(it->mSubject.compare(0,fs->GetQueuePath().length(),fs->GetQueuePath())==0)
      {
	eos_static_warning("found a notification to remove %s ",it->mSubject.c_str());
	it->mType = XrdMqSharedObjectManager::kMqSubjectDeletion;
      }
    }
    subscriber->SubjectsMutex.UnLock();
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
  info.fsId = 0;
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
    pFsId2FsPtr.erase(fsid);
    if(mapEntry->fs2SlowTreeNode.empty())
    {
      pGroup2TreeMapEntry.erase(group); // prevent from access by other threads
      pPendingDeletions.push_back(mapEntry);
    }
    mapEntry->slowTreeMutex.UnLockWrite();
    pTreeMapMutex.UnLockWrite();
  }

  return true;
}

void GeoTreeEngine::printInfo(std::string &info,
    bool dispTree, bool dispSnaps, bool dispParam, bool dispState,
    const std::string &schedgroup, const std::string &optype, bool useColors)
{
  RWMutexReadLock lock(pTreeMapMutex);

  stringstream ostr;

  map<string,string> orderByGroupName;

  if(dispParam)
  {
    ostr << "*** GeoTreeEngine parameters :" << std::endl;
    ostr << "skipSaturatedPlct = " << pSkipSaturatedPlct << std::endl;
    ostr << "skipSaturatedAccess = "<< pSkipSaturatedAccess << std::endl;
    ostr << "skipSaturatedDrnAccess = "<< pSkipSaturatedDrnAccess << std::endl;
    ostr << "skipSaturatedBlcAccess = "<< pSkipSaturatedBlcAccess << std::endl;
    ostr << "skipSaturatedDrnPlct = "<< pSkipSaturatedDrnPlct << std::endl;
    ostr << "skipSaturatedBlcPlct = "<< pSkipSaturatedBlcPlct << std::endl;
    ostr << "penaltyUpdateRate = "<< pPenaltyUpdateRate << std::endl;
    ostr << "plctDlScorePenalty = "<< pPlctDlScorePenaltyF[0] << "(default)" << " | "
        << pPlctDlScorePenaltyF[1] << "(1Gbps)" << " | "
        << pPlctDlScorePenaltyF[2] << "(10Gbps)" << " | "
        << pPlctDlScorePenaltyF[3] << "(100Gbps)" << " | "
        << pPlctDlScorePenaltyF[4] << "(1000Gbps)" << std::endl;
    ostr << "plctUlScorePenalty = "<< pPlctUlScorePenaltyF[0] << "(defaUlt)" << " | "
        << pPlctUlScorePenaltyF[1] << "(1Gbps)" << " | "
        << pPlctUlScorePenaltyF[2] << "(10Gbps)" << " | "
        << pPlctUlScorePenaltyF[3] << "(100Gbps)" << " | "
        << pPlctUlScorePenaltyF[4] << "(1000Gbps)" << std::endl;
    ostr << "accessDlScorePenalty = "<< pAccessDlScorePenaltyF[0] << "(default)" << " | "
        << pAccessDlScorePenaltyF[1] << "(1Gbps)" << " | "
        << pAccessDlScorePenaltyF[2] << "(10Gbps)" << " | "
        << pAccessDlScorePenaltyF[3] << "(100Gbps)" << " | "
        << pAccessDlScorePenaltyF[4] << "(1000Gbps)" << std::endl;
    ostr << "accessUlScorePenalty = "<< pAccessUlScorePenaltyF[0] << "(defaUlt)" << " | "
        << pAccessUlScorePenaltyF[1] << "(1Gbps)" << " | "
        << pAccessUlScorePenaltyF[2] << "(10Gbps)" << " | "
        << pAccessUlScorePenaltyF[3] << "(100Gbps)" << " | "
        << pAccessUlScorePenaltyF[4] << "(1000Gbps)" << std::endl;
    ostr << "fillRatioLimit = "<< (int)pFillRatioLimit << std::endl;
    ostr << "fillRatioCompTol = "<< (int)pFillRatioCompTol << std::endl;
    ostr << "saturationThres = "<< (int)pSaturationThres << std::endl;
    ostr << "timeFrameDurationMs = "<< (int)pTimeFrameDurationMs << std::endl;
  }
  if(dispState)
  {
    ostr << "frameCount = " << pFrameCount << std::endl;
    ostr << "==== added penalties for each fs over successive frames  ===="<<std::endl;
    {
      // to be sure that no fs in inserted removed in the meantime
      eos::common::RWMutexWriteLock lock(pAddRmFsMutex);
      struct timeval curtime;
      gettimeofday(&curtime, 0);
      size_t ts = curtime.tv_sec*1000+curtime.tv_usec/1000;

      ostr << std::setw(6)<<"fsid"<< std::setw(6)<<"drct ";
      for(size_t itcol=0; itcol<pCircSize;itcol++)
      ostr << std::setw(6)<< std::fixed << std::setprecision(1)<<(pCircFrCnt2Timestamp[(pFrameCount+pCircSize-1-itcol)%pCircSize]?(ts-pCircFrCnt2Timestamp[(pFrameCount+pCircSize-1-itcol)%pCircSize])*0.001:0);
      ostr << std::endl;
      for(size_t itline=1; itline<pCircFrCnt2FsPenalties.begin()->size();itline++)
      {
        ostr << std::setw(6)<< itline << std::setw(6)<<"UL";
        for(size_t itcol=0; itcol<pCircSize;itcol++)
        ostr << std::setw(6)<< (int)(pCircFrCnt2FsPenalties[(pFrameCount+pCircSize-1-itcol)%pCircSize][itline].ulScorePenalty);
        ostr <<std::endl;
        ostr << std::setw(6)<< "" <<  std::setw(6)<<"DL";
        for(size_t itcol=0; itcol<pCircSize;itcol++)
        ostr << std::setw(6)<< (int)(pCircFrCnt2FsPenalties[(pFrameCount+pCircSize-1-itcol)%pCircSize][itline].dlScorePenalty);
        ostr <<std::endl;
      }
    }
    ostr << "============================================================="<<std::endl<<std::endl;
    ostr << "================ fst2GeotreeEngine latency  ================="<<std::endl;
    struct timeval nowtv;
    gettimeofday(&nowtv,NULL);
    size_t nowms = nowtv.tv_sec*1000 + nowtv.tv_usec/1000;
    double avAge = 0.0;
    size_t count = 0;
    for(size_t n=1; n<pFsId2LatencyStats.size(); n++)
    {
      if(pFsId2LatencyStats[n].getage(nowms)<600000) // consider only if less than a minute
      {
      avAge += pFsId2LatencyStats[n].getage(nowms);
      count++;
      }
    }
    avAge /= count;
    ostr << "globalLatency  = "<< setw(5)<<(int)pGlobalLatencyStats.minlatency <<"ms.(min)"<< " | "
        << setw(5)<<(int)pGlobalLatencyStats.averagelatency <<"ms.(avg)"<< " | "
        << setw(5)<<(int)pGlobalLatencyStats.maxlatency <<"ms.(max)"<<"  |  age="<< setw(6)<<(int)avAge<<"ms.(avg)"<<std::endl;
    for(size_t n=1; n<pFsId2LatencyStats.size(); n++)
    {
    ostr << "fsLatency (fsid="<<std::setw(6)<<n<<")  = ";
    if(pFsId2LatencyStats[n].getage(nowms)>600000) // more than 1 minute, something is wrong
      ostr<< setw(5)<<"NA" <<"ms.(min)"<< " | "
            << setw(5)<<"NA" <<"ms.(avg)"<< " | "
            << setw(5)<<"NA" <<"ms.(max)"<<"  |  age="<< setw(6)<<"NA"<<"ms.(last)" <<std::endl;
    else
      ostr<< setw(5)<<(int)pFsId2LatencyStats[n].minlatency <<"ms.(min)"<< " | "
            << setw(5)<<(int)pFsId2LatencyStats[n].averagelatency <<"ms.(avg)"<< " | "
            << setw(5)<<(int)pFsId2LatencyStats[n].maxlatency <<"ms.(max)"<<"  |  age="<< setw(6)<<(int)pFsId2LatencyStats[n].getage(nowms)<<"ms.(last)" <<std::endl;
    }
    ostr << "============================================================="<<std::endl;
  }
  // ==== run through the map of file systems
  for(auto it = pGroup2TreeMapEntry.begin(); it != pGroup2TreeMapEntry.end(); it++)
  {
    stringstream ostr;

    if(dispTree && (schedgroup.empty() || schedgroup=="*" || (schedgroup == it->second->group->mName) ) )
    {
      ostr << "*** scheduling tree for scheduling group "<< it->second->group->mName <<" :" << std::endl;
      it->second->slowTree->display(ostr,useColors);
      ostr <<std::endl;
    }

    if(dispSnaps && (schedgroup.empty() || schedgroup=="*" || (schedgroup == it->second->group->mName) ) )
    {
      if(optype.empty() || (optype == "plct") )
      {
	ostr << "*** scheduling snapshot for scheduling group "<< it->second->group->mName <<" and operation \'Placement\' :" << std::endl;
	it->second->foregroundFastStruct->placementTree->recursiveDisplay(ostr,useColors)<<endl;
      }
      if(optype.empty() || (optype == "accsro") )
      {
	ostr << "*** scheduling snapshot for scheduling group "<< it->second->group->mName <<" and operation \'Access RO\' :" << std::endl;
	it->second->foregroundFastStruct->rOAccessTree->recursiveDisplay(ostr,useColors)<<endl;
      }
      if(optype.empty() || (optype == "accsrw") )
      {
	ostr << "*** scheduling snapshot for scheduling group "<< it->second->group->mName <<" and operation \'Access RW\' :" << std::endl;
	it->second->foregroundFastStruct->rWAccessTree->recursiveDisplay(ostr,useColors)<<endl;
      }
      if(optype.empty() || (optype == "accsdrain") )
      {
	ostr << "*** scheduling snapshot for scheduling group "<< it->second->group->mName <<" and operation \'Draining Access\' :" << std::endl;
	it->second->foregroundFastStruct->drnAccessTree->recursiveDisplay(ostr,useColors)<<endl;
      }
      if(optype.empty() || (optype == "plctdrain") )
      {
	ostr << "*** scheduling snapshot for scheduling group "<< it->second->group->mName <<" and operation \'Draining Placement\' :" << std::endl;
	it->second->foregroundFastStruct->drnPlacementTree->recursiveDisplay(ostr,useColors)<<endl;
      }
      if(optype.empty() || (optype == "accsblc") )
      {
	ostr << "*** scheduling snapshot for scheduling group "<< it->second->group->mName <<" and operation \'Balancing Access\' :" << std::endl;
	it->second->foregroundFastStruct->blcAccessTree->recursiveDisplay(ostr,useColors)<<endl;
      }
      if(optype.empty() || (optype == "plctblc") )
      {
	ostr << "*** scheduling snapshot for scheduling group "<< it->second->group->mName <<" and operation \'Draining Placement\' :" << std::endl;
	it->second->foregroundFastStruct->blcPlacementTree->recursiveDisplay(ostr,useColors)<<endl;
      }
    }

    orderByGroupName[it->second->group->mName] = ostr.str();
  }

  if(dispParam)
  {
    ostr << "*** GeoTreeEngine list of groups :" << std::endl;
    if(orderByGroupName.size())
    {
      const int lineWidth = 80;
      const int countNamePerLine = lineWidth / (orderByGroupName.begin()->first.size()+3);
      int count = 1;
      for(auto it = orderByGroupName.begin(); it != orderByGroupName.end(); it++, count++)
      {
	ostr << it->first;
	if(count%countNamePerLine)
	ostr << " , ";
	else
	ostr << "\n";
      }
    }
  }

  for(auto it = orderByGroupName.begin(); it != orderByGroupName.end(); it++)
  ostr << it->second;

  info = ostr.str();
}

bool GeoTreeEngine::placeNewReplicasOneGroup( FsGroup* group, const size_t &nNewReplicas,
    vector<FileSystem::fsid_t> *newReplicas,
    SchedType type,
    vector<FileSystem::fsid_t> *existingReplicas,
    std::vector<std::string> *fsidsgeotags,
    unsigned long long bookingSize,
    const std::string &startFromGeoTag,
    const size_t &nCollocatedReplicas,
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
    int count = 0;
    for(auto it = existingReplicas->begin(); it != existingReplicas->end(); ++it , ++count)
    {
      const SchedTreeBase::tFastTreeIdx *idx = static_cast<const SchedTreeBase::tFastTreeIdx*>(0);
      if(!entry->foregroundFastStruct->fs2TreeIdx->get(*it,idx) && !(*fsidsgeotags)[count].empty())
      {
	// the fs is not in that group.
	// this could happen because the former file scheduler
	// could place replicas across multiple groups
	// with the new geoscheduler, it should not happen

	// in that case, we try to match a filesystem having the same geotag
	SchedTreeBase::tFastTreeIdx idx = entry->foregroundFastStruct->tag2NodeIdx->getClosestFastTreeNode((*fsidsgeotags)[count].c_str());
	if(idx && (*entry->foregroundFastStruct->treeInfo)[idx].nodeType == SchedTreeBase::TreeNodeInfo::fs)
	{
	  if((std::find(existingReplicasIdx->begin(),existingReplicasIdx->end(),idx) == existingReplicasIdx->end()))
	  existingReplicasIdx->push_back(idx);
	}
	// if we can't find any such filesystem, the information is not taken into account
	// (and then can lead to unoptimal placement
	else
	{
	  eos_debug("could not place preexisting replica on the fast tree");
	}
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
	// the excluded fs might belong to another group
	// so it's not an error condition
	// eos_warning("could not place excluded fs on the fast tree");
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

  SchedTreeBase::tFastTreeIdx startFromNode=0;
  if(!startFromGeoTag.empty())
  {
    startFromNode=entry->foregroundFastStruct->tag2NodeIdx->getClosestFastTreeNode(startFromGeoTag.c_str());
  }

  // actually do the job
  bool success = false;
  switch(type)
  {
    case regularRO:
    case regularRW:
    success = placeNewReplicas(entry,nNewReplicas,&newReplicasIdx,entry->foregroundFastStruct->placementTree,
	existingReplicasIdx,bookingSize,startFromNode,nCollocatedReplicas,excludeFsIdx,forceBrIdx,pSkipSaturatedPlct);
    break;
    case draining:
    success = placeNewReplicas(entry,nNewReplicas,&newReplicasIdx,entry->foregroundFastStruct->drnPlacementTree,
	existingReplicasIdx,bookingSize,startFromNode,nCollocatedReplicas,excludeFsIdx,forceBrIdx,pSkipSaturatedDrnPlct);
    break;
    case balancing:
    success = placeNewReplicas(entry,nNewReplicas,&newReplicasIdx,entry->foregroundFastStruct->blcPlacementTree,
	existingReplicasIdx,bookingSize,startFromNode,nCollocatedReplicas,excludeFsIdx,forceBrIdx,pSkipSaturatedBlcPlct);
    break;
    default:
    ;
  }
  if(!success) goto cleanup;

  // fill the resulting vector and
  // update the fastTree UlScore and DlScore by applying the penalties
  newReplicas->resize(0);
  for(auto it = newReplicasIdx.begin(); it != newReplicasIdx.end(); ++it)
  {
    const SchedTreeBase::tFastTreeIdx *idx=NULL;
    const unsigned int fsid = (*entry->foregroundFastStruct->treeInfo)[*it].fsId;
    entry->foregroundFastStruct->fs2TreeIdx->get(fsid,idx);
    const char netSpeedClass = (*entry->foregroundFastStruct->treeInfo)[*idx].netSpeedClass;
    newReplicas->push_back(fsid);
    // apply the penalties
    if(entry->foregroundFastStruct->placementTree->pNodes[*idx].fsData.dlScore>0)
    applyDlScorePenalty(entry,*idx,pPlctDlScorePenalty[netSpeedClass]);
    if(entry->foregroundFastStruct->placementTree->pNodes[*idx].fsData.ulScore>0)
    applyUlScorePenalty(entry,*idx,pPlctUlScorePenalty[netSpeedClass]);
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
  unsigned char success = 0;
  switch(type)
  {
    case regularRO:
    success = accessReplicas(entry,nAccessReplicas,&accessedReplicasIdx,accesserNode,existingReplicasIdx,
	entry->foregroundFastStruct->rOAccessTree,excludeFsIdx,forceBrIdx,pSkipSaturatedAccess);
    break;
    case regularRW:
    success = accessReplicas(entry,nAccessReplicas,&accessedReplicasIdx,accesserNode,existingReplicasIdx,
	entry->foregroundFastStruct->rWAccessTree,excludeFsIdx,forceBrIdx,pSkipSaturatedAccess);
    break;
    case draining:
    success = accessReplicas(entry,nAccessReplicas,&accessedReplicasIdx,accesserNode,existingReplicasIdx,
	entry->foregroundFastStruct->drnAccessTree,excludeFsIdx,forceBrIdx,pSkipSaturatedDrnAccess);
    break;
    case balancing:
    success = accessReplicas(entry,nAccessReplicas,&accessedReplicasIdx,accesserNode,existingReplicasIdx,
	entry->foregroundFastStruct->blcAccessTree,excludeFsIdx,forceBrIdx,pSkipSaturatedBlcAccess);
    break;
    default:
    ;
  }
  if(!success) goto cleanup;

  // fill the resulting vector
  // update the fastTree UlScore and DlScore by applying the penalties
  accessedReplicas->resize(0);
  for(auto it = accessedReplicasIdx.begin(); it != accessedReplicasIdx.end(); ++it)
  {
    const SchedTreeBase::tFastTreeIdx *idx=NULL;
    const unsigned int fsid = (*entry->foregroundFastStruct->treeInfo)[*it].fsId;
    if(!entry->foregroundFastStruct->fs2TreeIdx->get(fsid,idx))
    {
      eos_static_crit("inconsistency : cannot retrieve index of selected fs though it should be in the tree");
      success = false;
      goto cleanup;
    }
    const char netSpeedClass = (*entry->foregroundFastStruct->treeInfo)[*idx].netSpeedClass;
    accessedReplicas->push_back(fsid);
    // apply the penalties
    if(entry->foregroundFastStruct->placementTree->pNodes[*idx].fsData.dlScore>=pAccessDlScorePenalty[netSpeedClass])
    applyDlScorePenalty(entry,*idx,pAccessDlScorePenalty[netSpeedClass]);
    if(entry->foregroundFastStruct->placementTree->pNodes[*idx].fsData.ulScore>=pAccessUlScorePenalty[netSpeedClass])
    applyUlScorePenalty(entry,*idx,pAccessUlScorePenalty[netSpeedClass]);
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

int GeoTreeEngine::accessHeadReplicaMultipleGroup(const size_t &nAccessReplicas,
    unsigned long &fsIndex,
    std::vector<eos::common::FileSystem::fsid_t> *existingReplicas,
    SchedType type,
    const std::string &accesserGeotag,
    const eos::common::FileSystem::fsid_t &forcedFsId,
    std::vector<eos::common::FileSystem::fsid_t> *unavailableFs,
    bool noIO
)
{
  int returnCode = ENODATA;

  // some basic checks
  assert(nAccessReplicas);
  assert(existingReplicas);

  // check that enough replicas exist already
  if(nAccessReplicas > existingReplicas->size())
  {
    eos_static_debug("not enough replica : has %d and requires %d :",(int)existingReplicas->size(),(int)nAccessReplicas);
    return EROFS;
  }

  // check if the forced replicas (if any) is among the existing replicas
  if(forcedFsId>0 && (std::find(existingReplicas->begin(), existingReplicas->end(), forcedFsId) == existingReplicas->end()) )
  {
    return ENODATA;
  }

  // find the group holdings the fs of the existing replicas
  // check that the replicas are available
  size_t availFsCount = 0;
  eos::mgm::ROAccessPriorityComparator comp;
  eos::mgm::SchedTreeBase::TreeNodeSlots freeSlot;
  freeSlot.freeSlotsCount=1;
  std::vector<eos::common::FileSystem::fsid_t>::iterator it;

  // maps tree maps entries (i.e. scheduling groups) to fsids containing a replica being available and the corresponding fastTreeIndex
  map<TreeMapEntry*,vector< pair<FileSystem::fsid_t,SchedTreeBase::tFastTreeIdx> > > entry2FsId;
  TreeMapEntry *entry=NULL;
  {
    // lock the scheduling group -> trees map so that the a map entry cannot be delete while processing it
    RWMutexReadLock lock(this->pTreeMapMutex);
    for(auto exrepIt = existingReplicas->begin(); exrepIt != existingReplicas->end(); exrepIt++)
    {
      auto mentry = pFs2TreeMapEntry.find(*exrepIt);
      // if we cannot find the fs in any group, there is an inconsistency somewhere
      if(mentry == pFs2TreeMapEntry.end())
      {
	eos_static_warning("cannot find the existing replica in any scheduling group");
	continue;
      }
      entry = mentry->second;

      // lock the double buffering to make sure all the fast trees are not modified
      if(!entry2FsId.count(entry))
      {
	// if the entry is already there, it was locked already
	entry->doubleBufferMutex.LockRead();
	// to prevent the destruction of the entry
	AtomicInc(entry->fastStructLockWaitersCount);
      }

      const SchedTreeBase::tFastTreeIdx *idx;
      if(!entry->foregroundFastStruct->fs2TreeIdx->get(*exrepIt,idx) )
      {
	eos_static_warning("cannot find fs in the scheduling group in the 2nd pass");
	if(!entry2FsId.count(entry))
	{
	  entry->doubleBufferMutex.UnLockRead();
	  AtomicDec(entry->fastStructLockWaitersCount);
	}
	continue;
      }
      // check if the fs is available
      bool isValid = false;
      switch(type)
      {
	case regularRO:
	isValid = entry->foregroundFastStruct->rOAccessTree->pBranchComp.isValidSlot(&entry->foregroundFastStruct->rOAccessTree->pNodes[*idx].fsData,&freeSlot);
	break;
	case regularRW:
	isValid = entry->foregroundFastStruct->rWAccessTree->pBranchComp.isValidSlot(&entry->foregroundFastStruct->rWAccessTree->pNodes[*idx].fsData,&freeSlot);
	break;
	case draining:
	isValid = entry->foregroundFastStruct->drnAccessTree->pBranchComp.isValidSlot(&entry->foregroundFastStruct->drnAccessTree->pNodes[*idx].fsData,&freeSlot);
	break;
	case balancing:
	isValid = entry->foregroundFastStruct->blcAccessTree->pBranchComp.isValidSlot(&entry->foregroundFastStruct->blcAccessTree->pNodes[*idx].fsData,&freeSlot);
	break;
	default:
	break;
      }
      if(isValid)
      {
	entry2FsId[entry].push_back(make_pair(*exrepIt,*idx));
	availFsCount++;
      }
      else
      {
	// create an empty entry in the map if needed
	if(!entry2FsId.count(entry))
	entry2FsId[entry]=vector< pair<FileSystem::fsid_t,SchedTreeBase::tFastTreeIdx> >();
	// update the unavailable fs
	unavailableFs->push_back(*exrepIt);
      }
    }
  }

  // check there is enough available replicas
  if(availFsCount<nAccessReplicas)
  {
    returnCode = ENONET;
    goto cleanup;
  }

  // check if the forced replicas (if any) is available
  if(forcedFsId>0 && (std::find(unavailableFs->begin(), unavailableFs->end(), forcedFsId) != unavailableFs->end()) )
  {
    returnCode = ENONET;
    goto cleanup;
  }

  // we have multiple groups
  // compute their geolocation scores to the the available fsids (+things) having a replica
  {
    SchedTreeBase::tFastTreeIdx accesserNode = 0;
    FileSystem::fsid_t selectedFsId = 0;
    {
      // maps a geolocation scores (int) to all the file system having this geolocation scores
      map< unsigned , std::vector< FileSystem::fsid_t > > geoScore2Fs;
      vector<SchedTreeBase::tFastTreeIdx> accessedReplicasIdx(1);
      // find the closest tree node to the accesser
      accesserNode = entry->foregroundFastStruct->tag2NodeIdx->getClosestFastTreeNode(accesserGeotag.c_str());;
      for(auto entryIt = entry2FsId.begin(); entryIt != entry2FsId.end(); entryIt ++)
      {
	if(eos::common::Logging::gLogMask & LOG_DEBUG)
	{
	  char buffer[1024];
	  buffer[0]=0;
	  char *buf = buffer;
	  for(auto it = entryIt->second.begin(); it!= entryIt->second.end(); ++it)
	  buf += sprintf(buf,"%lu  ",(unsigned long)(it->second));
	  eos_static_debug("existing replicas indices in geotree -> %s", buffer);

	  buffer[0]=0;
	  buf = buffer;
	  for(auto it = entryIt->second.begin(); it!= entryIt->second.end(); ++it)
	  buf += sprintf(buf,"%s  ",(*entryIt->first->foregroundFastStruct->treeInfo)[it->second].fullGeotag.c_str());
	  eos_static_debug("existing replicas geotags in geotree -> %s", buffer);
	}

	// if there is no replica here (might happen if it's spotted as unavailable after the first pass)
	if(entryIt->second.empty())
	continue;

	// fill a vector with the indices of the replicas
	vector<SchedTreeBase::tFastTreeIdx> existingReplicasIdx(entryIt->second.size());
	for(size_t i = 0; i < entryIt->second.size(); i++)
	existingReplicasIdx[i] = entryIt->second[i].second;

	// pickup an access slot is this scheduling group
	accessedReplicasIdx.clear();
	unsigned char retCode = 0;
	switch(type)
	{
	  case regularRO:
	  retCode = accessReplicas(entryIt->first,1,&accessedReplicasIdx,accesserNode,&existingReplicasIdx,
	      entry->foregroundFastStruct->rOAccessTree,NULL,NULL,pSkipSaturatedAccess);
	  break;
	  case regularRW:
	  retCode = accessReplicas(entryIt->first,1,&accessedReplicasIdx,accesserNode,&existingReplicasIdx,
	      entry->foregroundFastStruct->rWAccessTree,NULL,NULL,pSkipSaturatedAccess);
	  break;
	  case draining:
	  retCode = accessReplicas(entryIt->first,1,&accessedReplicasIdx,accesserNode,&existingReplicasIdx,
	      entry->foregroundFastStruct->drnAccessTree,NULL,NULL,pSkipSaturatedDrnAccess);
	  break;
	  case balancing:
	  retCode = accessReplicas(entryIt->first,1,&accessedReplicasIdx,accesserNode,&existingReplicasIdx,
	      entry->foregroundFastStruct->blcAccessTree,NULL,NULL,pSkipSaturatedBlcAccess);
	  break;
	  default:
	  break;
	}
	if(!retCode) goto cleanup;

	const string &fsGeotag = (*entryIt->first->foregroundFastStruct->treeInfo)[*accessedReplicasIdx.begin()].fullGeotag;
	unsigned geoScore = 0;
	size_t kmax = min(accesserGeotag.length(),fsGeotag.length());
	for(size_t k=0; k<kmax; k++)
	{
	  if(accesserGeotag[k]!=fsGeotag[k])
	  break;
	  if(accesserGeotag[k]==':' && k+1 < kmax && accesserGeotag[k+1]==':')
	  geoScore++;
	}
	// if the box is unsaturated, give an advantage to this FS
	if(retCode == 2)
	{
	  geoScore+=100;
	  eos_static_debug("found unsaturated fs");
	}

	geoScore2Fs[geoScore].push_back(
	    (*entryIt->first->foregroundFastStruct->treeInfo)[*accessedReplicasIdx.begin()].fsId);
      }

      // randomly choose a fs among the highest scored ones
      selectedFsId = geoScore2Fs.rbegin()->second[rand()%geoScore2Fs.rbegin()->second.size()];

      // return the corresponding index
      for (it = existingReplicas->begin(); it != existingReplicas->end(); it++)
      {
	if(*it == selectedFsId)
	{
	  fsIndex = (eos::common::FileSystem::fsid_t) (it-existingReplicas->begin());
	  break;
	}
      }
    }

    if(eos::common::Logging::gLogMask & LOG_DEBUG)
    {
      char buffer[1024];
      buffer[0]=0;
      char *buf = buffer;
      for(auto it = existingReplicas->begin(); it!= existingReplicas->end(); ++it)
      buf += sprintf(buf,"%lu  ",(unsigned long)(*it));

      eos_static_debug("existing replicas fs id's -> %s", buffer);
      eos_static_debug("accesser closest node to %s index -> %d  /  %s",accesserGeotag.c_str(), (int)accesserNode,(*entry->foregroundFastStruct->treeInfo)[accesserNode].fullGeotag.c_str());
      eos_static_debug("selected FsId -> %d / idx %d", (int)selectedFsId,(int)fsIndex);
    }
  }

  // check we found it
  if(it == existingReplicas->end())
  {
    eos_err("inconsistency : unable to find the selected fs but it should be there");
    returnCode = EIO;
    goto cleanup;
  }

  if(!noIO)
  {
    std::set<eos::common::FileSystem::fsid_t> setunav(unavailableFs->begin(),unavailableFs->end());
    //for(auto erit=existingReplicas->begin(); erit!=existingReplicas->end(); erit++)
    for(size_t i=0; i<existingReplicas->size(); i++)
    {
      size_t j = (fsIndex+i)%existingReplicas->size();

      auto &fs = (*existingReplicas)[j];
      // if this one is unavailable, skip it
      if(setunav.count(fs))
      continue;

      //////////
      // apply the penalties
      //////////
      const SchedTreeBase::tFastTreeIdx *idx;
      if(entry->foregroundFastStruct->fs2TreeIdx->get(fs,idx))
      {
        const char netSpeedClass = (*entry->foregroundFastStruct->treeInfo)[*idx].netSpeedClass;
        // every available box will push data
        if(entry->foregroundFastStruct->placementTree->pNodes[*idx].fsData.ulScore>=pAccessUlScorePenalty[netSpeedClass])
        applyUlScorePenalty(entry,*idx,pAccessUlScorePenalty[netSpeedClass]);
        // every available box will have to pull data if it's a RW access (or if it's a gateway)
        if( (type==regularRW) || (j==fsIndex && nAccessReplicas>1) )
        {
          if(entry->foregroundFastStruct->placementTree->pNodes[*idx].fsData.dlScore>=pAccessDlScorePenalty[netSpeedClass])
          applyDlScorePenalty(entry,*idx,pAccessDlScorePenalty[netSpeedClass]);
        }
      }
      else
      eos_err("could not find fs on the fast tree to apply penalties");
      // the gateway will also have to pull data from the
      if(j==fsIndex && nAccessReplicas==1)// mainly replica layout RO case
      break;
    }
  }

  // if we arrive here, it all ran fine
  returnCode = 0;

  // cleanup and exit
  cleanup:
  for(auto cit = entry2FsId.begin(); cit != entry2FsId.end(); cit++ )
  {
    cit->first->doubleBufferMutex.UnLockRead();
    AtomicDec(cit->first->fastStructLockWaitersCount);
  }
  return returnCode;
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

  struct timeval curtime,prevtime;
  gettimeofday(&prevtime,NULL);
  curtime = prevtime;

  do
  {
    while(gUpdaterPaused) sleep(1);
    gOFS->ObjectNotifier.tlSubscriber->SubjectsSem.Wait(1);
    //gOFS->ObjectNotifier.tlSubscriber->SubjectsSem.Wait();

    XrdSysThread::SetCancelOff();

    // to be sure that we won't try to access a removed fs
    pAddRmFsMutex.LockWrite();

    // we always take a lock to take something from the queue and then release it
    gOFS->ObjectNotifier.tlSubscriber->SubjectsMutex.Lock();

    // listens on modifications on filesystem objects
    while (gOFS->ObjectNotifier.tlSubscriber->NotificationSubjects.size())
    {
      XrdMqSharedObjectManager::Notification event;
      event = gOFS->ObjectNotifier.tlSubscriber->NotificationSubjects.front();
      gOFS->ObjectNotifier.tlSubscriber->NotificationSubjects.pop_front();

      string newsubject = event.mSubject.c_str();

      if (event.mType == XrdMqSharedObjectManager::kMqSubjectCreation)
      {
	// ---------------------------------------------------------------------
	// handle subject creation
	// ---------------------------------------------------------------------
	eos_warning("received creation on subject %s : don't know what to do with this!", newsubject.c_str());

	continue;
      }

      if (event.mType == XrdMqSharedObjectManager::kMqSubjectDeletion)
      {
	// ---------------------------------------------------------------------
	// handle subject deletion
	// ---------------------------------------------------------------------
	eos_debug("received deletion on subject %s : the fs was removed from the GeoTreeEngine, skipping this update", newsubject.c_str());

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

	continue;
      }

      if (event.mType == XrdMqSharedObjectManager::kMqSubjectKeyDeletion)
      {
	// ---------------------------------------------------------------------
	// handle subject key deletion
	// ---------------------------------------------------------------------
	eos_warning("received subject deletion on subject %s : don't know what to do with this!", newsubject.c_str());

	continue;
      }
      eos_warning("msg=\"don't know what to do with subject\" subject=%s", newsubject.c_str());
      continue;
    }
    gOFS->ObjectNotifier.tlSubscriber->SubjectsMutex.UnLock();
    pAddRmFsMutex.UnLockWrite();
    // do the processing
    prevtime = curtime;
    gettimeofday(&curtime,NULL);

    eos_static_debug("Updating Fast Structures at %ds. %dns. Previous update was at prev: %ds. %dns. Time elapsed since the last update is: %dms.",(int)curtime.tv_sec,(int)curtime.tv_usec,(int)prevtime.tv_sec,(int)prevtime.tv_usec,(int)curtime.tv_sec*1000+((int)curtime.tv_usec)/1000-(int)prevtime.tv_sec*1000-((int)prevtime.tv_usec)/1000);
    {
      checkPendingDeletions(); // do it before tree info to leave some time to the other threads
      {
	eos::common::RWMutexWriteLock lock(pAddRmFsMutex);
	updateTreeInfo(gNotificationsBuffer);
      }
      gNotificationsBuffer.clear();
    }
    XrdSysThread::SetCancelOff();
    size_t elapsedMs = (curtime.tv_sec-prevtime.tv_sec)*1000 +(curtime.tv_usec-prevtime.tv_usec)/1000;
    if((int)elapsedMs<pTimeFrameDurationMs)
      XrdSysTimer::Wait(pTimeFrameDurationMs-(int)elapsedMs);
    pFrameCount++;
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

    //const string &oldGeoTag = oldNode->pNodeInfo.fullGeotag;
    string oldGeoTag = oldNode->pNodeInfo.fullGeotag;
    oldGeoTag = (oldGeoTag.rfind("::")!=std::string::npos)?oldGeoTag.substr(0,oldGeoTag.rfind("::")):std::string("");

    eos_debug("geotag change detected : old geotag is %s   new geotag is %s",oldGeoTag.c_str(),newGeoTag.c_str());
    //CHECK IF CHANGE ACTUALLY HAPPENED BEFORE ACTUALLY CHANGING SOMETHING
    if( oldGeoTag !=newGeoTag)
    { // do the change only if there is one
      SlowTreeNode *newNode = NULL;
      newNode = entry->slowTree->moveToNewGeoTag(oldNode,newGeoTag);
      if(!newNode)
      {
        stringstream ss;
        ss << (*entry->slowTree);
	    eos_err("error changing geotag in slowtree : move is %s => %s and slowtree is \n%s\n",
	        oldGeoTag.c_str(),
	        newGeoTag.c_str(),
	        ss.str().c_str()
	    );

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
    // should not happen
    //eos_crit("the FsId should not change once it's created:  new value is %lu",(unsigned long)fs->mId);
    // .... unless it is the first change to give to the id it's initial value. It happens after it's been created so it's seen as a change.
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
      eos_debug("fs %lu is getting unavailable ftidx=%d  stn=%p",(unsigned long) fs->mId,(int)ftIdx,stn);
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
    else
    {
      if(ftIdx)
      {
	unsetOneStateVarStatusInAllFastTrees(SchedTreeBase::Readable);
	unsetOneStateVarStatusInAllFastTrees(SchedTreeBase::Writable);
      }
      if(stn)
      {
	stn->pNodeState.mStatus &= ~SchedTreeBase::Readable;
	stn->pNodeState.mStatus &= ~SchedTreeBase::Writable;
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
  //	if(keys&sfgFsfilled)
  //	{
  //		//half fr = half(fs->mNominalFilled);
  //		float fr = float(fs->mNominalFilled);
  //		if(ftIdx) setOneStateVarInAllFastTrees(fillRatio,fr);
  //		if(stn) stn->pNodeState.fillRatio = fr;
  //	}

  if(keys&sfgBlkavailb)
  {
    float ts = float(fs->mDiskBfree * (double)fs->mDiskBsize );

    if(ftIdx) setOneStateVarInAllFastTrees(totalSpace,ts);
    if(stn) stn->pNodeState.totalSpace = ts;
  }
  size_t netSpeedClass = 0; // <1Gb/s -> 0 ; 1Gb/s -> 1; 10Gb/s->2 ; 100Gb/s->...etc

  if( (keys&sfgPubTmStmp) && fs->mPublishTimestamp)
  {
    pGlobalLatencyStats.lastupdate = fs->mPublishTimestamp;
    pGlobalLatencyStats.update();
    // update the latency of this fs
    tLatencyStats *lstat = NULL;
    if(ftIdx)
    {
      if( ( (int)((*entry->backgroundFastStruct->treeInfo)[ftIdx].fsId) ) < ((int)pFsId2LatencyStats.size()) )
      {
        lstat = &pFsId2LatencyStats[(*entry->backgroundFastStruct->treeInfo)[ftIdx].fsId];
      }
      else
      eos_crit("trying to update latency for fs %d but latency stats vector size is %d : something is wrong",(int)(*entry->backgroundFastStruct->treeInfo)[ftIdx].fsId,(int)pFsId2LatencyStats.size());
    }
    else if(stn)
    {
      if( (int)( stn->pNodeInfo.fsId ) < ((int)pFsId2LatencyStats.size()) )
      {
        lstat = &pFsId2LatencyStats[stn->pNodeInfo.fsId];
      }
      else
      eos_err("trying to update latency for fs %d but latency stats vector size is %d : something is wrong",(int)( stn->pNodeInfo.fsId ),(int)pFsId2LatencyStats.size());
    }
    if(lstat)
    {
      lstat->lastupdate = fs->mPublishTimestamp;
      lstat->update();
    }
  }

  if(keys&(sfgDiskload|sfgInratemib))
  {
    // update the upload score
    double ulScore = (1-fs->mDiskUtilization);
    double netoutweight = (1.0 - ((fs->mNetEthRateMiB) ? (fs->mNetOutRateMiB / fs->mNetEthRateMiB) : 0.0));
    ulScore *= ((netoutweight > 0) ? sqrt(netoutweight) : 0);

    if(ftIdx) setOneStateVarInAllFastTrees(ulScore,(char)(ulScore*100));
    if(stn) stn->pNodeState.ulScore = ulScore*100;
  }
  if(keys&(sfgOutratemib|sfgDiskload|sfgReadratemb))
  {
    double dlScore = (1-fs->mDiskUtilization);
    double netinweight = (1.0 - ((fs->mNetEthRateMiB) ? (fs->mNetInRateMiB / fs->mNetEthRateMiB) : 0.0));
    dlScore *= ((netinweight > 0) ? sqrt(netinweight) : 0);

    if(ftIdx) setOneStateVarInAllFastTrees(dlScore,(char)(dlScore*100));
    if(stn) stn->pNodeState.dlScore = dlScore*100;
  }
  if(keys&(sfgDiskload|sfgInratemib|sfgOutratemib|sfgEthmib))
  {
    netSpeedClass = round(log10(fs->mNetEthRateMiB*8 * 1024 * 1024 + 1));
    netSpeedClass = netSpeedClass>8 ? netSpeedClass-8 : 0; // netSpeedClass 1 means 1Gbps
    // check if netspeed calss need an update
    if(entry->backgroundFastStruct->treeInfo->size()>=netSpeedClass+1 &&
        (*entry->backgroundFastStruct->treeInfo)[ftIdx].netSpeedClass!=(unsigned char)netSpeedClass)
    {
      if(ftIdx) (*entry->backgroundFastStruct->treeInfo)[ftIdx].netSpeedClass = netSpeedClass;
      if(stn) stn->pNodeInfo.netSpeedClass = netSpeedClass;
    }

    nodeAgreg& na = pUpdatingNodes[fs->mQueue];// this one will create the entry if it doesnt exists already
    na.fsCount++;
    if(!na.saturated)
    {
      if(na.fsCount ==1 )
      {
        na.netSpeedClass = netSpeedClass;
        pMaxNetSpeedClass = std::max( pMaxNetSpeedClass , netSpeedClass);
        na.netOutWeight += (1.0 - ((fs->mNetEthRateMiB) ? (fs->mNetOutRateMiB / fs->mNetEthRateMiB) : 0.0));
        na.netInWeight += (1.0 - ((fs->mNetEthRateMiB) ? (fs->mNetInRateMiB / fs->mNetEthRateMiB) : 0.0));
        if(na.netOutWeight<0.1 || na.netInWeight<0.1)
        na.saturated = true; // network of the box is saturated
      }
      na.rOpen += fs->mDiskRopen;
      na.wOpen += fs->mDiskWopen;
      na.diskUtilSum += fs->mDiskUtilization;
      if(fs->mDiskUtilization > 0.9 )
      na.saturated = true; // one of the disks of the box is saturated
    }

    // apply penalties that are still valid on fast trees
    if(ftIdx) recallScorePenalty(entry, ftIdx);
    // in case the fs in not in the fast trees , it has not been
    // used recently to schedule , so there is no penalty to recall!
    // so there is nothing like if(stn) recallScorePenalty(entry, stn);
  }
  if(keys&sfgFsfilled)
  {
    if(ftIdx) setOneStateVarInAllFastTrees(fillRatio,(char)fs->mDiskFilled);
    if(stn) stn->pNodeState.fillRatio = (char)fs->mDiskFilled;
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
      pTreeMapMutex.UnLockRead();
      return false;
    }

    // copy the penalties of the last frame from each group and reset the penalties counter in the fast trees
    auto& pVec=pCircFrCnt2FsPenalties[pFrameCount%pCircSize];
    for(auto it2=entry->foregroundFastStruct->fs2TreeIdx->begin();
        it2!=entry->foregroundFastStruct->fs2TreeIdx->end();
        it2++)
    {
      auto cur=*it2;
      pVec[cur.first] = (*entry->foregroundFastStruct->penalties)[cur.second];
      AtomicCAS((*entry->foregroundFastStruct->penalties)[cur.second].dlScorePenalty,(*entry->foregroundFastStruct->penalties)[cur.second].dlScorePenalty,(char)0);
      AtomicCAS((*entry->foregroundFastStruct->penalties)[cur.second].ulScorePenalty,(*entry->foregroundFastStruct->penalties)[cur.second].ulScorePenalty,(char)0);
    }
  }
  pTreeMapMutex.UnLockRead();
  // timestamp the current frame
  {
    struct timeval curtime;
    gettimeofday(&curtime, 0);
    pCircFrCnt2Timestamp[pFrameCount%pCircSize] = ((size_t)curtime.tv_sec)*1000+((size_t)curtime.tv_usec)/1000;
  }
  pUpdatingNodes.clear();
  pMaxNetSpeedClass = 0;
  for(auto it = updates.begin(); it != updates.end(); ++it)
  {

    gOFS->ObjectManager.HashMutex.LockRead();
    XrdMqSharedHash* hash = gOFS->ObjectManager.GetObject(it->first.c_str(), "hash");
    if(!hash)
    {
      eos_static_warning("Inconsistency : Trying to access a deleted fs. Should not happen because any reference to a fs is cleaned from the updates buffer ehen the fs is being removed.");
      gOFS->ObjectManager.HashMutex.UnLockRead();
      continue;
    }
    FileSystem::fsid_t fsid = (FileSystem::fsid_t) hash->GetLongLong("id");
    if(!fsid)
    {
      eos_static_warning("Inconsistency : Trying to update an unregistered fs. Should not happen.");
      gOFS->ObjectManager.HashMutex.UnLockRead();
      continue;
    }
    gOFS->ObjectManager.HashMutex.UnLockRead();

    if(!pFsId2FsPtr.count(fsid))
    {
      eos_static_warning("Inconsistency: Trying to access an existing fs which is not referenced in the GeoTreeEngine anymore");
      continue;
    }
    eos::common::FileSystem *filesystem = pFsId2FsPtr[fsid];

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
	eos_crit("Inconsistency : cannot locate an fs %lu supposed to be in the fast structures",(unsigned long)fsid);
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
  updateAtomicPenalties();

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

bool GeoTreeEngine::getGroupsFromFsIds(const std::vector<FileSystem::fsid_t> fsids, std::vector<std::string> *fsgeotags, std::vector<FsGroup*> *sortedgroups)
{
  bool result = true;
  if(fsgeotags) fsgeotags->reserve(fsids.size());
  if(sortedgroups) sortedgroups->reserve(fsids.size());
  std::map<FsGroup*,size_t> group2idx;
  std::vector<std::pair<size_t,size_t> > groupcount;
  groupcount.reserve(fsids.size());
  {
    RWMutexReadLock lock(this->pTreeMapMutex);
    for(auto it = fsids.begin(); it != fsids.end(); ++ it)
    {
      if(pFs2TreeMapEntry.count(*it))
      {
	FsGroup *group = pFs2TreeMapEntry[*it]->group;
	if(fsgeotags)
	{
	  const SchedTreeBase::tFastTreeIdx *idx=NULL;
	  if(pFs2TreeMapEntry[*it]->foregroundFastStruct->fs2TreeIdx->get(*it,idx))
	  fsgeotags->push_back(
	      (*pFs2TreeMapEntry[*it]->foregroundFastStruct->treeInfo)[*idx].fullGeotag
	  );
	  else
	  fsgeotags->push_back("");
	}
	if(sortedgroups)
	{
	  if(!group2idx.count(group))
	  {
	    group2idx[group] = group2idx.size();
	    sortedgroups->push_back(group);
	    groupcount.push_back(make_pair(1,groupcount.size()));
	  }
	  else
	  {
	    size_t idx = group2idx[group];
	    groupcount[idx].first++;
	  }
	}
      }
      else
      {
	// put an empty entry in the result vector to preserve the indexing
	fsgeotags->push_back("");
	// to signal that one of the fsids was not mapped to a group
	result = false;
      }
    }
  }

  if(sortedgroups)
  {
    // sort the count vector in ascending order to get the permutation
    std::sort(groupcount.begin(),groupcount.end(),std::greater<std::pair<size_t,size_t>>());
    // apply the permutation
    std::vector<FsGroup*> final(groupcount.size());
    size_t count = 0;
    for(auto it = groupcount.begin(); it != groupcount.end(); it++)
    final[count++] = (*sortedgroups)[it->second];

    *sortedgroups = final;
  }
  return result;
}

void GeoTreeEngine::updateAtomicPenalties()
{
  // In this function, we compute a rought a simplified version
  // of the penalties applied to selected fs for placement and access.
  // there is only one penalty and it's copied to ulplct, dlplct, ulaccess and dlaccess
  // variants.

  // if the update is enabled
  if(pPenaltyUpdateRate)
  {
    if(pUpdatingNodes.empty())
    {
      //eos_static_debug("updatingNodes is empty!");
    }
    else
    {
      // each networking speed class has its own variables
      std::vector<double>
      ropen(pMaxNetSpeedClass+1,0.0),
      wopen(pMaxNetSpeedClass+1,0.0),
      ulload(pMaxNetSpeedClass+1,0.0),
      dlload(pMaxNetSpeedClass+1,0.0),
      fscount(pMaxNetSpeedClass+1,0.0),
      hostcount(pMaxNetSpeedClass+1,0.0),
      diskutil(pMaxNetSpeedClass+1,0.0);

      // we use the view to check that we have all the fs in a node
      // could be removed if we were sure to run a single on fst daemon / box
      FsView::gFsView.ViewMutex.LockRead();
      for( auto it = pUpdatingNodes.begin(); it!= pUpdatingNodes.end(); it++)
      {
        const std::string &nodestr = it->first;
        FsNode *node = NULL;
        if(FsView::gFsView.mNodeView.count(nodestr))
        node = FsView::gFsView.mNodeView[nodestr];
        else
        {
          std::stringstream ss;
          ss.str("");
          for (auto it2 = FsView::gFsView.mNodeView.begin(); it2 != FsView::gFsView.mNodeView.end(); it2++)
          ss << it2->first << "  ";
          eos_static_err("Inconsistency : cannot find updating node %s in %s",nodestr.c_str(),ss.str().c_str());
          continue;
        }
        if((!it->second.saturated) && it->second.fsCount == node->size())
        {
//          eos_static_debug("aggregated opened files for %s : wopen %d   ropen %d   outweight %lf   inweight %lf",
//              it->first.c_str(),it->second.wOpen,it->second.rOpen,it->second.netOutWeight,it->second.netInWeight);
        }
        else
        {
          // the fs/host is saturated, we don't use the whole host in the estimate
          if(it->second.saturated)
          eos_static_debug("fs update in node %s : box is saturated");
          // there is a mismatch between
          // this should not happen if only one single fst daemon is running on each fst node
          if(it->second.fsCount != node->size())
          eos_static_notice("fs update in node %s : %d fs in FsView vs %d fs in update. This probably means that several fst daemons are running on the same host",it->first.c_str(),(int)node->size(),(int)it->second.fsCount);
// could force to get everything
//          long long wopen = node->SumLongLong("stat.wopen",false);
//          long long ropen = node->SumLongLong("stat.ropen",false);
        }
        // update aggregated informations for the right networking class
        ropen[it->second.netSpeedClass]+=(it->second.rOpen);
        wopen[it->second.netSpeedClass]+=(it->second.wOpen);
        ulload[it->second.netSpeedClass]+=(1.0-it->second.netOutWeight);
        dlload[it->second.netSpeedClass]+=(1.0-it->second.netInWeight);
        diskutil[it->second.netSpeedClass]+=it->second.diskUtilSum;
        fscount[it->second.netSpeedClass]+=it->second.fsCount;
        hostcount[it->second.netSpeedClass]++;
      }
      FsView::gFsView.ViewMutex.UnLockRead();

      for(size_t netSpeedClass=0; netSpeedClass<=pMaxNetSpeedClass; netSpeedClass++)
      {
        if(ropen[netSpeedClass]+ropen[netSpeedClass]>4)
        {
          eos_static_debug("UPDATE netSpeedClass=%d  ulload=%lf  dlload=%lf  diskutil=%lf  ropen=%lf  wopen=%lf  fscount=%lf  hostcount=%lf",
              (int)netSpeedClass, ulload[netSpeedClass], dlload[netSpeedClass],diskutil[netSpeedClass], ropen[netSpeedClass],
              wopen[netSpeedClass], fscount[netSpeedClass], hostcount[netSpeedClass]);

          // the penalty aims at knowing roughly how many file concurrent file operations
          // can be led on a single fs before sturating a ressource (disk or network)

          // network penalty per file
          // the multiplication by the number of fs is to take into account
          // that the bw is shared between multiple fs
          double networkpen =
          0.5*(ulload[netSpeedClass]+dlload[netSpeedClass])/(ropen[netSpeedClass]+wopen[netSpeedClass])
          *(fscount[netSpeedClass]/hostcount[netSpeedClass]);

          // there is factor to take into account the read cache
          // TODO use a realistic value for this factor
          double diskpen =
          diskutil[netSpeedClass]/(0.4*ropen[netSpeedClass]+wopen[netSpeedClass]);

          eos_static_debug("penalties updates are network %lf   disk %lf",networkpen,diskpen);

          double update = 100*std::max(diskpen,networkpen);

          if(update<1 || update>99)// could be more restrictive
          {
            eos_static_debug("weird value for accessDlScorePenalty update : %lf. Not using this one.",update);
          }
          else
          {
            eos_static_debug("netSpeedClass %d : using update values %lf for penalties with weight %f%%",
                netSpeedClass, pPenaltyUpdateRate);
            eos_static_debug("netSpeedClass %d : values before update are accessDlScorePenalty=%f  plctDlScorePenalty=%f  accessUlScorePenalty=%f  plctUlScorePenalty=%f",
                netSpeedClass, pAccessDlScorePenaltyF[netSpeedClass],pPlctDlScorePenaltyF[netSpeedClass],pAccessUlScorePenaltyF[netSpeedClass],pPlctUlScorePenaltyF[netSpeedClass]);
            union
            {
              float f;
              uint32_t u;
            }uf;

            // atomic change, no need to lock anything
            uf.f = 0.01*( ( 100 - pPenaltyUpdateRate)*pAccessDlScorePenaltyF[netSpeedClass] + pPenaltyUpdateRate*update);
            AtomicCAS( reinterpret_cast<uint32_t&>(pAccessDlScorePenaltyF[netSpeedClass]) , reinterpret_cast<uint32_t&>(pAccessDlScorePenaltyF[netSpeedClass]) , uf.u );
            uf.f = 0.01*( ( 100 - pPenaltyUpdateRate)*pPlctDlScorePenaltyF[netSpeedClass] + pPenaltyUpdateRate*update);
            AtomicCAS( reinterpret_cast<uint32_t&>(pPlctDlScorePenaltyF[netSpeedClass]) , reinterpret_cast<uint32_t&>(pPlctDlScorePenaltyF[netSpeedClass]) , uf.u);
            uf.f = 0.01*( ( 100 - pPenaltyUpdateRate)*pAccessUlScorePenaltyF[netSpeedClass] + pPenaltyUpdateRate*update);
            AtomicCAS( reinterpret_cast<uint32_t&>(pAccessUlScorePenaltyF[netSpeedClass]) , reinterpret_cast<uint32_t&>(pAccessUlScorePenaltyF[netSpeedClass]) , uf.u);
            uf.f = 0.01*( ( 100 - pPenaltyUpdateRate)*pPlctUlScorePenaltyF[netSpeedClass] + pPenaltyUpdateRate*update);
            AtomicCAS( reinterpret_cast<uint32_t&>(pPlctUlScorePenaltyF[netSpeedClass]) , reinterpret_cast<uint32_t&>(pPlctUlScorePenaltyF[netSpeedClass]) , uf.u);
            eos_static_debug("netSpeedClass %d : values after update are accessDlScorePenalty=%f  plctDlScorePenalty=%f  accessUlScorePenalty=%f  plctUlScorePenalty=%f",
                netSpeedClass, pAccessDlScorePenaltyF[netSpeedClass],pPlctDlScorePenaltyF[netSpeedClass],pAccessUlScorePenaltyF[netSpeedClass],pPlctUlScorePenaltyF[netSpeedClass]);
            // update the casted versions too
            AtomicCAS( pPlctUlScorePenalty[netSpeedClass], pPlctUlScorePenalty[netSpeedClass], (SchedTreeBase::tFastTreeIdx) pPlctUlScorePenaltyF[netSpeedClass]);
            AtomicCAS( pPlctDlScorePenalty[netSpeedClass], pPlctDlScorePenalty[netSpeedClass], (SchedTreeBase::tFastTreeIdx) pPlctDlScorePenaltyF[netSpeedClass]);
            AtomicCAS( pAccessDlScorePenalty[netSpeedClass], pAccessDlScorePenalty[netSpeedClass], (SchedTreeBase::tFastTreeIdx) pAccessDlScorePenaltyF[netSpeedClass]);
            AtomicCAS( pAccessUlScorePenalty[netSpeedClass], pAccessUlScorePenalty[netSpeedClass], (SchedTreeBase::tFastTreeIdx) pAccessUlScorePenaltyF[netSpeedClass]);
          }
        }
        else
        {
          eos_static_debug("not enough file opened to get reliable statistics %d",(int)(ropen[netSpeedClass]+ropen[netSpeedClass]));
        }
      }
    }

  }
}

bool GeoTreeEngine::setSkipSaturatedPlct(bool value)
{
  return setInternalParam(pSkipSaturatedPlct,(int)value,false,"skipsaturatedplct");
}
bool GeoTreeEngine::setSkipSaturatedAccess(bool value)
{
  return setInternalParam(pSkipSaturatedAccess,(int)value,false,"skipsaturatedaccess");
}
bool GeoTreeEngine::setSkipSaturatedDrnAccess(bool value)
{
  return setInternalParam(pSkipSaturatedDrnAccess,(int)value,false,"skipsaturateddrnaccess");
}
bool GeoTreeEngine::setSkipSaturatedBlcAccess(bool value)
{
  return setInternalParam(pSkipSaturatedBlcAccess,(int)value,false,"skipsaturatedblcaccess");
}
bool GeoTreeEngine::setSkipSaturatedDrnPlct(bool value)
{
  return setInternalParam(pSkipSaturatedDrnPlct,(int)value,false,"skipsaturateddrnplct");
}
bool GeoTreeEngine::setSkipSaturatedBlcPlct(bool value)
{
  return setInternalParam(pSkipSaturatedBlcPlct,(int)value,false,"skipsaturatedblcplct");
}

bool GeoTreeEngine::setScorePenalty(std::vector<float> &fvector, std::vector<char> &cvector, const std::vector<char> &vvalue, const std::string &configentry)
{
  if(vvalue.size()!=8)
    return false;
  std::vector<float> valuef(8);
  for(int i=0;i<8;i++) valuef[i]=vvalue[i];
  return setInternalParam(fvector,valuef,false,"")
  && setInternalParam(cvector,vvalue,false,configentry);
}

bool GeoTreeEngine::setScorePenalty(std::vector<float> &fvector, std::vector<char> &cvector, const char* svalue, const std::string &configentry)
{
  std::vector<double> dvvalue(8);
  std::vector<char> vvalue(8);
 if(sscanf(svalue,"[%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf]",&dvvalue[0],&dvvalue[1],&dvvalue[2],&dvvalue[3],&dvvalue[4],&dvvalue[5],&dvvalue[6],&dvvalue[7])!=8)
   return false;
 for(int i=0;i<8;i++) vvalue[i]=(char)dvvalue[i];
 return setScorePenalty(fvector,cvector,vvalue,configentry);
}

bool GeoTreeEngine::setScorePenalty(std::vector<float> &fvector, std::vector<char> &cvector, char value, int netSpeedClass, const std::string &configentry)
{
  if(netSpeedClass>=0)
  {
    if(netSpeedClass>=(int)fvector.size())
      return false;
//    return setInternalParam(fvector[netSpeedClass],(float)value,false,"")
//    && setInternalParam(cvector[netSpeedClass],value,false,configentry);
    std::vector<char> vvalue(cvector);
    vvalue[netSpeedClass] = value;
    return setScorePenalty(fvector,cvector,vvalue,configentry);
  }
  else if(netSpeedClass==-1)
  {
    std::vector<char> vvalue(8,value);
    return setScorePenalty(fvector,cvector,vvalue,configentry);
  }
  return false;
}

bool GeoTreeEngine::setPlctDlScorePenalty(char value, int netSpeedClass)
{
  return setScorePenalty(pPlctDlScorePenaltyF,pPlctDlScorePenalty,value,netSpeedClass,"plctdlscorepenalty");
}
bool GeoTreeEngine::setPlctUlScorePenalty(char value, int netSpeedClass)
{
  return setScorePenalty(pPlctUlScorePenaltyF,pPlctUlScorePenalty,value,netSpeedClass,"plctulscorepenalty");
}
bool GeoTreeEngine::setAccessDlScorePenalty(char value, int netSpeedClass)
{
  return setScorePenalty(pAccessDlScorePenaltyF,pAccessDlScorePenalty,value,netSpeedClass,"accessdlscorepenalty");
}
bool GeoTreeEngine::setAccessUlScorePenalty(char value, int netSpeedClass)
{
   return setScorePenalty(pAccessUlScorePenaltyF,pAccessUlScorePenalty,value,netSpeedClass,"accessulscorepenalty");
}

bool GeoTreeEngine::setPlctDlScorePenalty(const char *value)
{
  return setScorePenalty(pPlctDlScorePenaltyF,pPlctDlScorePenalty,value,"plctdlscorepenalty");
}
bool GeoTreeEngine::setPlctUlScorePenalty(const char *value)
{
  return setScorePenalty(pPlctUlScorePenaltyF,pPlctUlScorePenalty,value,"plctulscorepenalty");
}
bool GeoTreeEngine::setAccessDlScorePenalty(const char *value)
{
  return setScorePenalty(pAccessDlScorePenaltyF,pAccessDlScorePenalty,value,"accessdlscorepenalty");
}
bool GeoTreeEngine::setAccessUlScorePenalty(const char *value)
{
  return setScorePenalty(pAccessUlScorePenaltyF,pAccessUlScorePenalty,value,"accessulscorepenalty");
}

bool GeoTreeEngine::setFillRatioLimit(char value)
{
  return setInternalParam(pFillRatioLimit,value,true,"fillratiolimit");
}
bool GeoTreeEngine::setFillRatioCompTol(char value)
{
  return setInternalParam(pFillRatioCompTol,value,true,"fillratiocomptol");
}
bool GeoTreeEngine::setSaturationThres(char value)
{
  return setInternalParam(pSaturationThres,value,true,"saturationthres");
}
bool GeoTreeEngine::setTimeFrameDurationMs(int value)
{
  return setInternalParam(pTimeFrameDurationMs,value,false,"timeframedurationms");
}
bool GeoTreeEngine::setPenaltyUpdateRate(float value)
{
  return setInternalParam(pPenaltyUpdateRate,value,false,"penaltyupdaterate");
}

bool GeoTreeEngine::setParameter( std::string param, const std::string &value,int iparamidx)
{
  std::transform(param.begin(), param.end(),param.begin(), ::tolower);
  double dval = 0.0;
  sscanf(value.c_str(),"%lf",&dval);
  int ival = (int)dval;
  bool ok = false;
#define readParamVFromString(PARAM,VALUE) { std::string q; if(sscanf(VALUE.c_str(),"[%f,%f,%f,%f,%f,%f,%f,%f]",&PARAM##F[0],&PARAM##F[1],&PARAM##F[2],&PARAM##F[3],&PARAM##F[4],&PARAM##F[5],&PARAM##F[6],&PARAM##F[7])!=8) return false; for(int i=0;i<8;i++) PARAM[i]=(char)PARAM##F[i]; ok = true;}
  if(param == "timeframedurationms")
  {
    ok = gGeoTreeEngine.setTimeFrameDurationMs(ival);
  }
  else if(param == "saturationthres")
  {
    ok = gGeoTreeEngine.setSaturationThres((char)ival);
  }
  else if(param == "fillratiocomptol")
  {
    ok = gGeoTreeEngine.setFillRatioCompTol((char)ival);
  }
  else if(param == "fillratiolimit")
  {
    ok = gGeoTreeEngine.setFillRatioLimit((char)ival);
  }
  else if(param == "accessulscorepenalty")
  {
    if(iparamidx>-2)
      ok = gGeoTreeEngine.setAccessUlScorePenalty((char)ival,iparamidx);
    else
      readParamVFromString(pAccessUlScorePenalty,value);
  }
  else if(param == "accessdlscorepenalty")
  {
    if(iparamidx>-2)
      ok = gGeoTreeEngine.setAccessDlScorePenalty((char)ival,iparamidx);
    else
      readParamVFromString(pAccessDlScorePenalty,value);
  }
  else if(param == "plctulscorepenalty")
  {
    if(iparamidx>-2)
      ok = gGeoTreeEngine.setPlctUlScorePenalty((char)ival,iparamidx);
    else
      readParamVFromString(pPlctUlScorePenalty,value);
  }
  else if(param == "plctdlscorepenalty")
  {
    if(iparamidx>-2)
      ok = gGeoTreeEngine.setPlctDlScorePenalty((char)ival,iparamidx);
    else
      readParamVFromString(pPlctDlScorePenalty,value);
  }
  else if(param == "skipsaturatedblcplct")
  {
    ok = gGeoTreeEngine.setSkipSaturatedBlcPlct((bool)ival);
  }
  else if(param == "skipsaturateddrnplct")
  {
    ok = gGeoTreeEngine.setSkipSaturatedDrnPlct((bool)ival);
  }
  else if(param == "skipsaturatedblcaccess")
  {
    ok = gGeoTreeEngine.setSkipSaturatedBlcAccess((bool)ival);
  }
  else if(param == "skipsaturateddrnaccess")
  {
    ok = gGeoTreeEngine.setSkipSaturatedDrnAccess((bool)ival);
  }
  else if(param == "skipsaturatedaccess")
  {
    ok = gGeoTreeEngine.setSkipSaturatedAccess((bool)ival);
  }
  else if(param == "skipsaturatedplct")
  {
    ok = gGeoTreeEngine.setSkipSaturatedPlct((bool)ival);
  }
  else if(param == "penaltyupdaterate")
  {
    ok = gGeoTreeEngine.setPenaltyUpdateRate((float)dval);
  }
  else if(param == "disabledbranches")
  {
    ok = true;
    if(value.size()>4)
    {
      // first, clear the list of disabled branches
      gGeoTreeEngine.rmDisabledBranch("*","*","*",NULL);
      eos_warning("disablebranches full line %s",value.c_str());
      // remove leading and trailing square brackets
      string list(value.substr(2,value.size()-4));
      // from the end to avoid reallocation of the string
      size_t idxl,idxr;
      while((idxr=list.rfind(')'))!=std::string::npos && ok)
      {
        idxl=list.rfind('(');
        eos_warning("disablebranches full token %s",value.c_str()+idxl);
        auto comidx = list.find(',',idxl);
        string geotag(list.substr(idxl+1,comidx-idxl-1));
        eos_warning("geotag token %s",geotag.c_str());
        auto comidx2 = list.find(',',comidx+1);
        string optype(list.substr(comidx+1,comidx2-comidx-1));
        eos_warning("optype token %s",optype.c_str());
        string group(list.substr(comidx2+1,idxr-comidx2-1));
        eos_warning("group token %s",group.c_str());
        ok = ok && gGeoTreeEngine.addDisabledBranch(group,optype,geotag,NULL);
        list.erase(idxl,std::string::npos);
      }
    }
  }
  return ok;
}

void GeoTreeEngine::setConfigValue (const char* prefix,
                                          const char* key,
                                          const char* val,
                                          bool tochangelog)
{
  gOFS->ConfEngine->SetConfigValue(prefix,key,val,tochangelog);
}

bool GeoTreeEngine::markPendingBranchDisablings(const std::string &group, const std::string&optype, const std::string&geotag)
{
  for(auto git = pGroup2TreeMapEntry.begin(); git != pGroup2TreeMapEntry.end(); git++)
  {
    RWMutexReadLock lock(git->second->doubleBufferMutex);
    if(group=="*" || git->first->mName==group)
    {
      git->second->slowTreeModified = true;
    }
  }
  return true;
}

bool GeoTreeEngine::applyBranchDisablings(const TreeMapEntry& entry)
{
  for(auto mit = pDisabledBranches.begin(); mit != pDisabledBranches.end(); mit++)
  {
    // should I lock configMutex or is it already locked?
    const std::string &group(mit->first);
    if(group!="*" && entry.group->mName!=group)
    continue;

    for(auto oit = mit->second.begin(); oit != mit->second.end(); oit++)
    {
      const std::string &optype(oit->first);
      for(auto geoit = oit->second.begin(); geoit != oit->second.end(); geoit++)
      {
        const std::string &geotag(*geoit);
        auto idx = entry.backgroundFastStruct->tag2NodeIdx->getClosestFastTreeNode(geotag.c_str());

        // check there is an exact geotag match
        if((*entry.backgroundFastStruct->treeInfo)[idx].fullGeotag!=geotag)
          continue;

        if(optype=="*" || optype=="plct")
          entry.backgroundFastStruct->placementTree->disableSubTree(idx);
        if(optype=="*" || optype=="accsro")
          entry.backgroundFastStruct->rOAccessTree->disableSubTree(idx);
        if(optype=="*" || optype=="accsrw")
          entry.backgroundFastStruct->rWAccessTree->disableSubTree(idx);
        if(optype=="*" || optype=="plctdrain")
          entry.backgroundFastStruct->drnPlacementTree->disableSubTree(idx);
        if(optype=="*" || optype=="accsdrain")
          entry.backgroundFastStruct->drnAccessTree->disableSubTree(idx);
        if(optype=="*" || optype=="plctblc")
          entry.backgroundFastStruct->blcPlacementTree->disableSubTree(idx);
        if(optype=="*" || optype=="accsblc")
          entry.backgroundFastStruct->blcAccessTree->disableSubTree(idx);
      }
    }
  }
  return true;
}

bool GeoTreeEngine::addDisabledBranch (const std::string& group, const std::string &optype, const std::string&geotag, XrdOucString *output, bool toConfig)
{
  eos::common::RWMutexWriteLock lock(pAddRmFsMutex);
  eos::common::RWMutexWriteLock lock2(pTreeMapMutex);
  eos::common::RWMutexWriteLock lock3(configMutex);

  std::vector<std::string> intersection;
  // do checks
  // go through the potentially intersecting groups
  auto git_begin = group=="*"?pDisabledBranches.begin():pDisabledBranches.find(group);
  auto git_end = group=="*"?pDisabledBranches.end():pDisabledBranches.find(group);
  if(git_end!=pDisabledBranches.end()) git_end++;
  for(auto git=git_begin;git!=git_end;git++)
  {
    // go through the potentially intersecting optypes
    auto oit_begin = optype=="*"?git->second.begin():git->second.find(group);
    auto oit_end = optype=="*"?git->second.end():git->second.find(group);
    if(oit_end!=git->second.end()) oit_end++;
    for(auto oit=oit_begin;oit!=oit_end;oit++)
    {
      XrdOucString toinsert(geotag.c_str());
      // check that none of the disabled geotag is a prefix of the current one and the other way around
      for(auto geoit=oit->second.begin();geoit!=oit->second.end();geoit++)
      {
        XrdOucString alreadyThere(geoit->c_str());
        if(alreadyThere.beginswith(toinsert) || toinsert.beginswith(alreadyThere))
        {
          intersection.push_back( std::string("(") + geotag.c_str() + std::string(",") + oit->first + std::string(",") + git->first + std::string(")") + std::string(alreadyThere.c_str()) );
        }
      }
    }
  }

  if(intersection.size())
  {
    if(output)
    {
    output->append( (std::string("unable to add disabled branch : ")
        + std::string("(") + geotag + std::string(",") + optype + std::string(",") + geotag +
        std::string(") clashes with : ")).c_str() );
    for(auto iit = intersection.begin();iit!=intersection.end();iit++)
      output->append((*iit+" , ").c_str());
    }
    return false;
  }

  // update the internal value
  pDisabledBranches[group][optype].insert(geotag);

  // to apply the new set of rules, mark the involved slow trees as modified to force a refresh
  markPendingBranchDisablings(group,optype,geotag);

  // update the config
  if(toConfig)
  {
    XrdOucString outStr("[ ");
    showDisabledBranches("*","*","*",&outStr,false);
    outStr.replace(")\n(",") , ("); outStr.replace(")\n",")");
    outStr += " ]";
    setConfigValue("geosched","disabledbranches" , outStr.c_str());
  }
  return true;
}

bool GeoTreeEngine::rmDisabledBranch (const std::string& group, const std::string &optype, const std::string&geotag, XrdOucString *output)
{
  eos::common::RWMutexWriteLock lock(pAddRmFsMutex);
  eos::common::RWMutexWriteLock lock2(pTreeMapMutex);
  eos::common::RWMutexWriteLock lock3(configMutex);

  bool found = false;
  if(group=="*" && optype=="*" && geotag=="*")
  {
    found = true;
    eos_notice("clearing disabled branch list in GeoTreeEngine");
    pDisabledBranches.clear();
  }
  else if(pDisabledBranches.count(group))
  {
    if(pDisabledBranches[group].count(optype))
    {
      found = (bool)pDisabledBranches[group][optype].erase(geotag);
    }
  }

  if(!found)
  {
    if(output) output->append( (std::string("could not find disabled branch : ")
                  + std::string("(") + group + std::string(" , ") + optype + std::string(") -> ") + geotag).c_str()
                  );
  }
  else
  {
    // to apply the new set of rules, mark the involved slow trees as modified to force a refresh
    markPendingBranchDisablings(group,optype,geotag);

    // update the config
    XrdOucString outStr("[ ");
    showDisabledBranches("*","*","*",&outStr,false);
    outStr.replace(")\n(",") , ("); outStr.replace(")\n",")");
    outStr += " ]";
    setConfigValue("geosched","disabledbranches" , outStr.c_str());
  }

  return found;
}

bool GeoTreeEngine::showDisabledBranches (const std::string& group, const std::string &optype, const std::string&geotag, XrdOucString *output, bool lock)
{
  if(lock) configMutex.LockRead();

  for(auto git = pDisabledBranches.begin(); git != pDisabledBranches.end(); git++)
  {
    if(group=="*" || group==git->first)
    for(auto oit = git->second.begin(); oit!=git->second.end(); oit++)
    {
      if(optype=="*" || optype==oit->first)
      for(auto geoit = oit->second.begin(); geoit !=oit->second.end(); geoit++)
      {
        if(geotag=="*" || geotag==*geoit)
        if(output) output->append( (std::string("(") + *geoit + std::string(",") + oit->first + std::string(",") + git->first + std::string(")\n")).c_str() );
      }
    }
  }

  if(lock) configMutex.UnLockRead();
  return true;
}

EOSMGMNAMESPACE_END
