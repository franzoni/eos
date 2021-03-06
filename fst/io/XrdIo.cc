//------------------------------------------------------------------------------
// File: XrdIo.cc
// Author: Elvin-Alin Sindrilaru <esindril@cern.ch>
//------------------------------------------------------------------------------

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
#include <stdint.h>
#include <cstdlib>
/*----------------------------------------------------------------------------*/
#include "fst/io/XrdIo.hh"
#include "fst/io/ChunkHandler.hh"
#include "fst/io/VectChunkHandler.hh"
/*----------------------------------------------------------------------------*/
#include "XrdCl/XrdClDefaultEnv.hh"
/*----------------------------------------------------------------------------*/

EOSFSTNAMESPACE_BEGIN

const uint64_t ReadaheadBlock::sDefaultBlocksize = 1 *1024 * 1024; ///< 1MB default
const uint32_t XrdIo::sNumRdAheadBlocks = 2;

//------------------------------------------------------------------------------
// Handle asynchronous open responses
//------------------------------------------------------------------------------
void AsyncIoOpenHandler::HandleResponseWithHosts(XrdCl::XRootDStatus* status,
                                                 XrdCl::AnyObject* response,
                                                 XrdCl::HostList* hostList)
{
  eos_info("handling response in AsyncIoOpenHandler");
  // response is nullptr
  delete hostList;

  if (status->IsOK())
  {
    // Store the last URL we are connected after open
    mFileIo->mXrdFile->GetProperty("LastURL", mFileIo->mLastUrl);
  }

  mLayoutOpenHandler->HandleResponseWithHosts(status, 0, 0);
  delete this;
}

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
XrdIo::XrdIo () :
FileIo(),
mDoReadahead (false),
mBlocksize (ReadaheadBlock::sDefaultBlocksize),
mXrdFile (NULL),
mMetaHandler(new AsyncMetaHandler())
{
  // Set the TimeoutResolution to 1 
  XrdCl::Env* env = XrdCl::DefaultEnv::GetEnv();
  env->PutInt( "TimeoutResolution", 1 );
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
XrdIo::~XrdIo ()
{
  if (mDoReadahead)
  {
    while (!mQueueBlocks.empty())
    {
      ReadaheadBlock* ptr_readblock = mQueueBlocks.front();
      mQueueBlocks.pop();
      delete ptr_readblock;
    }

    while (!mMapBlocks.empty())
    {
      delete mMapBlocks.begin()->second;
      mMapBlocks.erase(mMapBlocks.begin());
    }
  }

  delete mMetaHandler;  
  
  if (mXrdFile)
    delete mXrdFile;
}

//------------------------------------------------------------------------------
// Open file synchronously
//------------------------------------------------------------------------------
int
XrdIo::Open (const std::string& path, XrdSfsFileOpenMode flags, mode_t mode,
             const std::string& opaque, uint16_t timeout)
{
  const char* val = 0;
  std::string request;
  std::string lOpaque;
  size_t qpos = 0;
  mFilePath = path;

  // Opaque info can be part of the 'path' 
  if ( ( (qpos = path.find("?")) != std::string::npos) ) {
    lOpaque = path.substr(qpos+1);
    mFilePath.erase(qpos);
  } 
  else
  {
    lOpaque = opaque;
  }

  XrdOucEnv open_opaque(lOpaque.c_str());

  // Decide if readahead is used and the block size
  if ((val = open_opaque.Get("fst.readahead")) &&
      (strncmp(val, "true", 4) == 0))
  {
    eos_debug("Enabling the readahead.");
    mDoReadahead = true;
    val = 0;

    if ((val = open_opaque.Get("fst.blocksize")))
    {
      mBlocksize = static_cast<uint64_t> (atoll(val));
    }

    if (mQueueBlocks.empty())
    {
      for (unsigned int i = 0; i < sNumRdAheadBlocks; i++)
      {
        mQueueBlocks.push(new ReadaheadBlock(mBlocksize));
      }
    }
  }

  request = path;
  request += "?";
  request += lOpaque;
  if(mXrdFile)
  {    
    delete mXrdFile;
    mXrdFile = NULL;
  }
  mXrdFile = new XrdCl::File();

  // Disable recovery on read and write
  if (!mXrdFile->SetProperty("ReadRecovery", "false") ||
      !mXrdFile->SetProperty("WriteRecovery", "false"))
  {
    eos_warning("failed to set XrdCl::File properties read recovery and write "
                "recovery to false");
  }
  
  XrdCl::OpenFlags::Flags flags_xrdcl = eos::common::LayoutId::MapFlagsSfs2XrdCl(flags);
  XrdCl::Access::Mode mode_xrdcl = eos::common::LayoutId::MapModeSfs2XrdCl(mode);
  XrdCl::XRootDStatus status = mXrdFile->Open(request, flags_xrdcl, mode_xrdcl, timeout);
  mXrdFile->GetProperty("LastURL", mLastUrl);

  if (!status.IsOK())
  {
    eos_err("error=opening remote XrdClFile errno=%d errcode=%d msg=%s",
            (int)status.errNo,(int)status.code,status.ToString().c_str());
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    errno = status.errNo;
    return SFS_ERROR;
  }
  else
  {
    errno = 0;
  }

  return SFS_OK;
}

//------------------------------------------------------------------------------
// Open file asynchronously
//------------------------------------------------------------------------------
int
XrdIo::OpenAsync (const std::string& path, XrdCl::ResponseHandler* io_handler,
                  XrdSfsFileOpenMode flags, mode_t mode,
                  const std::string& opaque, uint16_t timeout)
{
  const char* val = 0;
  std::string request;
  std::string lOpaque;
  size_t qpos = 0;
  mFilePath = path;

  // Opaque info can be part of the 'path'
  if ( ( (qpos = path.find("?")) != std::string::npos) ) {
    lOpaque = path.substr(qpos+1);
    mFilePath.erase(qpos);
  }
  else
  {
    lOpaque = opaque;
  }

  XrdOucEnv open_opaque(lOpaque.c_str());

  // Decide if readahead is used and the block size
  if ((val = open_opaque.Get("fst.readahead")) &&
      (strncmp(val, "true", 4) == 0))
  {
    eos_debug("Enabling the readahead.");
    mDoReadahead = true;
    val = 0;

    if ((val = open_opaque.Get("fst.blocksize")))
    {
      mBlocksize = static_cast<uint64_t> (atoll(val));
    }

    // Allocate only if not already done - this can happen if open is called
    // multiple times on the same XrdIo object
    if (mQueueBlocks.empty())
    {
      for (unsigned int i = 0; i < sNumRdAheadBlocks; i++)
      {
        mQueueBlocks.push(new ReadaheadBlock(mBlocksize));
      }
    }
  }

  request = path;
  request += "?";
  request += lOpaque;
  if(mXrdFile)
  {    
    delete mXrdFile;
    mXrdFile = NULL;
  }
  mXrdFile = new XrdCl::File();

  // Disable recovery on read and write
  if (!mXrdFile->SetProperty("ReadRecovery", "false") ||
      !mXrdFile->SetProperty("WriteRecovery", "false"))
  {
    eos_warning("failed to set XrdCl::File properties read recovery and write "
                "recovery to false");
  }

  XrdCl::OpenFlags::Flags flags_xrdcl = eos::common::LayoutId::MapFlagsSfs2XrdCl(flags);
  XrdCl::Access::Mode mode_xrdcl = eos::common::LayoutId::MapModeSfs2XrdCl(mode);
  XrdCl::XRootDStatus status = mXrdFile->Open(request, flags_xrdcl, mode_xrdcl,
                                              io_handler, timeout);

  if (!status.IsOK())
  {
    delete io_handler;
    eos_err("error=opening remote XrdClFile");
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  return SFS_OK;
}

//------------------------------------------------------------------------------
// Read from file - sync
//------------------------------------------------------------------------------
int64_t
XrdIo::Read (XrdSfsFileOffset offset,
             char* buffer,
             XrdSfsXferSize length,
             uint16_t timeout)
{
  eos_debug("offset=%llu length=%llu",
            static_cast<uint64_t> (offset),
            static_cast<uint64_t> (length));

  uint32_t nread = 0;

  if(!mXrdFile)
  {
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::XRootDStatus status = mXrdFile->Read(static_cast<uint64_t> (offset),
                                              static_cast<uint32_t> (length),
                                              buffer,
                                              nread,
                                              timeout);

  if (!status.IsOK())
  {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  return static_cast<int64_t>(nread);
}


//------------------------------------------------------------------------------
// Vector read - sync
//------------------------------------------------------------------------------
 int64_t
 XrdIo::ReadV (XrdCl::ChunkList& chunkList,
               uint16_t timeout)
 {
   eos_debug("read count=%i", chunkList.size());
   int64_t nread = 0;

  if(!mXrdFile)
  {
    errno = EIO;
    return SFS_ERROR;
  }

   XrdCl::VectorReadInfo* vReadInfo = 0;
   XrdCl::XRootDStatus status = mXrdFile->VectorRead(chunkList, 0,
                                                     vReadInfo, timeout);
   
   if (!status.IsOK())
   {
     errno = status.errNo;
     mLastErrMsg = status.ToString().c_str();
     mLastErrCode  = status.code;
     mLastErrNo  = status.errNo;
     return SFS_ERROR;
   }

   nread = vReadInfo->GetSize();
   delete vReadInfo;
   return nread;       
 }


//------------------------------------------------------------------------------
//! Vector read - async
//------------------------------------------------------------------------------
int64_t
XrdIo::ReadVAsync (XrdCl::ChunkList& chunkList,
                   uint16_t timeout)
{
  if(!mXrdFile)
  {
    errno = EIO;
    return SFS_ERROR;
  }

  VectChunkHandler* vhandler = 0;
  XrdCl::XRootDStatus status;
 
  // Get vector handler and send async request
  eos_debug("read count=%i", chunkList.size());
  vhandler = mMetaHandler->Register(chunkList, NULL, false);
  int64_t nread = vhandler->GetLength();

  if (!vhandler)
  {
    eos_err("unable to get vector handler");
    return SFS_ERROR;
  }
  
  status = mXrdFile->VectorRead(chunkList, 
                                static_cast<void *> (0),
                                static_cast<XrdCl::ResponseHandler*> (vhandler),
                                timeout);
  
  if (!status.IsOK())
  {
    // TODO: for the time being we call this ourselves but this should be
    // dropped once XrdCl will call the handler for a request as it knows it
    // has already failed 
    mMetaHandler->HandleResponse(&status, vhandler);
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }
  
  return nread;
}


//------------------------------------------------------------------------------
// Read from file - async
//------------------------------------------------------------------------------
int64_t
XrdIo::ReadAsync (XrdSfsFileOffset offset,
                  char* buffer,
                  XrdSfsXferSize length,
                  bool readahead,
                  uint16_t timeout)
{
  eos_debug("offset=%llu length=%llu",
            static_cast<uint64_t> (offset),
            static_cast<uint64_t> (length));

  if(!mXrdFile)
  {
    errno = EIO;
    return SFS_ERROR;
  }

  bool done_read = false;
  int64_t nread = 0;
  char* pBuff = buffer;
  XrdCl::XRootDStatus status;
  ChunkHandler* handler = NULL;

  if (!mDoReadahead)
  {
    readahead = false;
    eos_debug("Readahead is disabled");
  }

  if (!readahead)
  {
    handler = mMetaHandler->Register(offset, length, buffer, false);

    // If previous read requests failed with a timeout error then we won't
    // get a new handler and we return directly an error
    if (!handler)
      return SFS_ERROR;    
    
    status = mXrdFile->Read(static_cast<uint64_t> (offset),
                            static_cast<uint32_t> (length),
                            buffer,
                            static_cast<XrdCl::ResponseHandler*> (handler),
                            timeout);

    if (!status.IsOK())
    {
      // TODO: for the time being we call this ourselves but this should be
      // dropped once XrdCl will call the handler for a request as it knows it
      // has already failed 
      mMetaHandler->HandleResponse(&status, handler);
    }
    
    nread = length;
  }
  else
  {
    eos_debug("readahead enabled, request offset=%lli, length=%i", offset, length);
    uint64_t read_length = 0;
    uint32_t aligned_length;
    uint32_t shift;
    std::map<uint64_t, ReadaheadBlock*>::iterator iter;

    mPrefetchMutex.Lock(); // -->

    while (length)
    {
      iter = FindBlock(offset);
      
      if (iter != mMapBlocks.end())
      {
        // Block found in prefetched blocks
        SimpleHandler* sh = iter->second->handler;
        shift = offset - iter->first;

        // We can prefetch another block if we still have available blocks in
        // the queue or if first read was from second prefetched block
        if (!mQueueBlocks.empty() || (iter != mMapBlocks.begin()))
        {
          if (iter != mMapBlocks.begin())
          {
            eos_debug("recycle the oldest block");
            mQueueBlocks.push(mMapBlocks.begin()->second);
            mMapBlocks.erase(mMapBlocks.begin());
          }

          eos_debug("prefetch new block(2)");
          if (!PrefetchBlock(offset + mBlocksize, false, timeout))
          {
            eos_warning("failed to send prefetch request(2)");
            break;
          }
        }
        
        if (sh->WaitOK())
        {
          eos_debug("block in cache, blk_off=%lld, req_off= %lld", iter->first, offset);
          
          if (sh->GetRespLength() == 0)
          {
            // The request got a response but it read 0 bytes
            eos_warning("response contains 0 bytes");
            break;           
          }

          aligned_length = sh->GetRespLength() - shift;
          read_length = ((uint32_t)length < aligned_length) ? length : aligned_length;

          // If prefetch block smaller than mBlocksize and current offset at end
          // of the prefetch block then we reached the end of file
          if ((sh->GetRespLength() != mBlocksize) &&
              ((uint64_t)offset >= iter->first + sh->GetRespLength()))
          {
            done_read = true;
            break;
          }

          pBuff = static_cast<char*> (memcpy(pBuff, iter->second->buffer + shift,
                                             read_length));

          pBuff += read_length;
          offset += read_length;
          length -= read_length;
          nread += read_length;
        }
        else
        {
          // Error while prefetching, remove block from map
          mQueueBlocks.push(iter->second);
          mMapBlocks.erase(iter);
          eos_err("error=prefetching failed, disable it and remove block from map");
          mDoReadahead = false;
          break;
        }
      }
      else
      {
        // Remove all elements from map so that we can align with the new 
        // requests and prefetch a new block. But first we need to collect any
        // responses which are in-flight as otherwise these response might
        // arrive later on, when we are expecting replies for other blocks since
        // we are recycling the SimpleHandler objects.
        while (!mMapBlocks.empty())
        {
          SimpleHandler* sh = mMapBlocks.begin()->second->handler;
          
          if (sh->HasRequest())
          {
            // Not interested in the result - discard it 
            sh->WaitOK();
          }
          
          mQueueBlocks.push(mMapBlocks.begin()->second);
          mMapBlocks.erase(mMapBlocks.begin());
        }

        if (!mQueueBlocks.empty())
        {
          eos_debug("prefetch new block(1)");
          
          if (!PrefetchBlock(offset, false, timeout))
          {
            eos_err("error=failed to send prefetch request(1)");
            mDoReadahead = false;
            break;
          }
        }
      }
    }

    mPrefetchMutex.UnLock(); // <--

    // If readahead not useful, use the classic way to read
    if (length && !done_read)
    {
      eos_debug("readahead useless, use the classic way for reading");
      handler = mMetaHandler->Register(offset, length, NULL, false);

      // If previous read requests failed then we won't get a new handler
      // and we return directly an error
      if (!handler)
        return SFS_ERROR;    
      
      status = mXrdFile->Read(static_cast<uint64_t> (offset),
                              static_cast<uint32_t> (length),
                              pBuff,
                              handler,
                              timeout);
      if (!status.IsOK())
      {
        // TODO: for the time being we call this ourselves but this should be
        // dropped once XrdCl will call the handler for a request as it knows it
        // has already failed 
        mMetaHandler->HandleResponse(&status, handler);
      }

      nread = length;
    }
  }

  return nread;
}


//------------------------------------------------------------------------------
// Try to find a block in cache which contains the required offset
//------------------------------------------------------------------------------
PrefetchMap::iterator
XrdIo::FindBlock(uint64_t offset)
{
  if (mMapBlocks.empty())
  {
    return mMapBlocks.end();
  }
  
  PrefetchMap::iterator iter = mMapBlocks.lower_bound(offset);
  if ((iter != mMapBlocks.end()) && (iter->first == offset))
  {
    // Found exactly the block needed
    return iter;
  }
  else
  {
    if (iter == mMapBlocks.begin())
    {
      // Only blocks with bigger offsets, return pointer to end of the map
      return mMapBlocks.end();      
    }
    else 
    {
      // Check if the previous block, we know the map is not empty
      iter--;
      
      if ((iter->first <= offset) && ( offset < (iter->first + mBlocksize)))
        return iter;
      else
        return mMapBlocks.end();
    }
  }  
}


//------------------------------------------------------------------------------
// Write to file - sync
//------------------------------------------------------------------------------
int64_t
XrdIo::Write (XrdSfsFileOffset offset,
              const char* buffer,
              XrdSfsXferSize length,
              uint16_t timeout)
{
  eos_debug("offset=%llu length=%llu",
            static_cast<uint64_t> (offset),
            static_cast<uint64_t> (length));

  if(!mXrdFile)
  {
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::XRootDStatus status = mXrdFile->Write(static_cast<uint64_t> (offset),
                                               static_cast<uint32_t> (length),
                                               buffer,
                                               timeout);

  if (!status.IsOK())
  {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  return length;
}


//------------------------------------------------------------------------------
// Write to file - async
//------------------------------------------------------------------------------
int64_t
XrdIo::WriteAsync (XrdSfsFileOffset offset,
                   const char* buffer,
                   XrdSfsXferSize length,
                   uint16_t timeout)
{
  eos_debug("offset=%llu length=%i", static_cast<uint64_t>(offset), length);

  if(!mXrdFile)
  {
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::XRootDStatus status;
  ChunkHandler* handler = mMetaHandler->Register(offset, length, (char*)buffer, true);

  // If previous write requests failed then we won't get a new handler
  // and we return directly an error
  if (!handler)
    return SFS_ERROR;    
  
  // Obs: Use the handler buffer for write requests
  status = mXrdFile->Write(static_cast<uint64_t> (offset),
                           static_cast<uint32_t> (length),
                           handler->GetBuffer(),
                           handler,
                           timeout);

  if (!status.IsOK())
  {
    mMetaHandler->HandleResponse(&status, handler);
    return SFS_ERROR;
  }

  return length;
}


//------------------------------------------------------------------------------
// Truncate file
//------------------------------------------------------------------------------
int
XrdIo::Truncate (XrdSfsFileOffset offset, uint16_t timeout)
{
  if(!mXrdFile)
  {
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::XRootDStatus status = mXrdFile->Truncate(static_cast<uint64_t> (offset),
                                                  timeout);
  
  if (!status.IsOK())
  {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  return SFS_OK;
}


//------------------------------------------------------------------------------
// Sync file to disk
//------------------------------------------------------------------------------
int
XrdIo::Sync (uint16_t timeout)
{
  if(!mXrdFile)
  {
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::XRootDStatus status = mXrdFile->Sync(timeout);

  if (!status.IsOK())
  {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  return SFS_OK;
}


//------------------------------------------------------------------------------
// Get stats about the file
//------------------------------------------------------------------------------
int
XrdIo::Stat (struct stat* buf, uint16_t timeout)
{
  if(!mXrdFile)
  {
    errno = EIO;
    return SFS_ERROR;
  }

  int rc = SFS_ERROR;
  XrdCl::StatInfo* stat = 0;
  XrdCl::XRootDStatus status = mXrdFile->Stat(true, stat, timeout);

  if (status.IsOK())
  {
    buf->st_dev = static_cast<dev_t> (atoi(stat->GetId().c_str()));
    buf->st_mode = static_cast<mode_t> (stat->GetFlags());
    buf->st_size = static_cast<off_t> (stat->GetSize());
    buf->st_mtime = static_cast<time_t> (stat->GetModTime());
    rc = SFS_OK;
  }
  else
  {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
  }
  
  if (stat)
    delete stat;

  return rc;
}


//------------------------------------------------------------------------------
// Close file
//------------------------------------------------------------------------------
int
XrdIo::Close (uint16_t timeout)
{
  if(!mXrdFile)
  {
    errno = EIO;
    return SFS_ERROR;
  }

  bool async_ok = true;

  if (mDoReadahead)
  {
    // Wait for any requests on the fly and then close
    while (!mMapBlocks.empty())
    {
      SimpleHandler* shandler = mMapBlocks.begin()->second->handler;
      if (shandler->HasRequest())
      {
        async_ok = shandler->WaitOK();
      }
      delete mMapBlocks.begin()->second;
      mMapBlocks.erase(mMapBlocks.begin());
    }
  }
  
  // Wait for any async requests before closing
  if (mMetaHandler)
  {
    if (mMetaHandler->WaitOK() != XrdCl::errNone)
    {
      eos_err("error=async requests failed for file path=%s", mPath.c_str());
      async_ok = false;
    }
  }

  XrdCl::XRootDStatus status = mXrdFile->Close(timeout);

  if (!status.IsOK())
  {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  // If any of the async requests failed then we have an error
  if (!async_ok)
  {
    return SFS_ERROR;
  }

  return SFS_OK;
}


//------------------------------------------------------------------------------
// Remove file
//------------------------------------------------------------------------------
int
XrdIo::Remove (uint16_t timeout)
{
  if(!mXrdFile)
  {
    errno = EIO;
    return SFS_ERROR;
  }

  //............................................................................
  // Remove the file by truncating using the special value offset
  //............................................................................
  XrdCl::XRootDStatus status = mXrdFile->Truncate(EOS_FST_DELETE_FLAG_VIA_TRUNCATE_LEN, timeout);

  if (!status.IsOK())
  {
    eos_err("failed to mark the file for deletion:%s", mPath.c_str());
    mLastErrMsg = "failed to truncate file with deletion offset";
    return SFS_ERROR;
  }

  return SFS_OK;
}


//------------------------------------------------------------------------------
// Prefetch block using the readahead mechanism
//------------------------------------------------------------------------------
bool 
XrdIo::PrefetchBlock (int64_t offset, bool isWrite, uint16_t timeout)
{
  bool done = true;
  XrdCl::XRootDStatus status;
  ReadaheadBlock* block = NULL;
  
  eos_debug("try to prefetch with offset: %lli, length: %4u",
            offset, mBlocksize);

  if (!mQueueBlocks.empty())
  {
    block = mQueueBlocks.front();
    mQueueBlocks.pop();
  }
  else
  {
    done = false;
    return done;
  }

  block->handler->Update(offset, mBlocksize, isWrite);
  status = mXrdFile->Read(offset,
                          mBlocksize,
                          block->buffer,
                          block->handler,
                          timeout);

  if (!status.IsOK())
  {
    // Create tmp status which is deleted in the HandleResponse method
    XrdCl::XRootDStatus* tmp_status = new XrdCl::XRootDStatus(status);
    block->handler->HandleResponse(tmp_status, NULL);
    mQueueBlocks.push(block);
    done = false;
  }
  else
  {
    mMapBlocks.insert(std::make_pair(offset, block));
  }

  return done;
}


//------------------------------------------------------------------------------
// Get pointer to async meta handler object 
//------------------------------------------------------------------------------
void*
XrdIo::GetAsyncHandler ()
{
  return static_cast<void*>(mMetaHandler);
}

EOSFSTNAMESPACE_END

