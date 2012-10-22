// -----------------------------------------------------------------------------
// File: RaidDpFile.cc
// Author: Elvin-Alin Sindrilaru - CERN
// -----------------------------------------------------------------------------

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
#include <set>
#include <map>
#include <cassert>
#include <cmath>
#include <fcntl.h>
/*----------------------------------------------------------------------------*/
#include "common/Timing.hh"
#include "fst/io/RaidDpFile.hh"
/*----------------------------------------------------------------------------*/

EOSFSTNAMESPACE_BEGIN

typedef long v2do __attribute__( ( vector_size( VECTOR_SIZE ) ) );

// -----------------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------------
RaidDpFile::RaidDpFile( std::vector<std::string> stripeurl,
                        int                      nparity,
                        bool                     storerecovery,
                        bool                     isstreaming, 
                        off_t                    targetsize,
                        std::string              bookingopaque )
  : RaidIO( "raidDP", stripeurl, nparity, storerecovery, 
            isstreaming, targetsize, bookingopaque )
{
  assert( nParityFiles = 2 );

  nDataBlocks = static_cast<int>( pow( nDataFiles, 2 ) );
  nTotalBlocks = nDataBlocks + 2 * nDataFiles;
  sizeGroup = nDataBlocks * stripeWidth;

  //allocate memory for blocks
  for ( unsigned int i = 0; i < nTotalBlocks; i++ ) {
    dataBlocks.push_back( new char[stripeWidth] );
  }
}


// -----------------------------------------------------------------------------
// Destructor
// -----------------------------------------------------------------------------
RaidDpFile::~RaidDpFile()
{
  for ( unsigned int i = 0; i < nTotalBlocks; i++ ) {
    delete[] dataBlocks[i];
  }
}


// -----------------------------------------------------------------------------
// Compute simple and double parity blocks
// -----------------------------------------------------------------------------
void
RaidDpFile::computeParity()
{
  int indexPBlock;
  int currentBlock;

  // ---------------------------------------------------------------------------
  // Compute simple parity
  // ---------------------------------------------------------------------------
  for ( unsigned int i = 0; i < nDataFiles; i++ ) {
    indexPBlock = ( i + 1 ) * nDataFiles + 2 * i;
    currentBlock = i * ( nDataFiles + 2 );   //beginning of current line
    operationXOR( dataBlocks[currentBlock],
                  dataBlocks[currentBlock + 1],
                  dataBlocks[indexPBlock],
                  stripeWidth );
    currentBlock += 2;

    while ( currentBlock < indexPBlock ) {
      operationXOR( dataBlocks[indexPBlock],
                    dataBlocks[currentBlock],
                    dataBlocks[indexPBlock],
                    stripeWidth );
      currentBlock++;
    }
  }

  // ---------------------------------------------------------------------------
  // Compute double parity
  // ---------------------------------------------------------------------------
  unsigned int auxBlock;
  unsigned int nextBlock;
  unsigned int indexDPBlock;
  unsigned int jumpBlocks = nTotalFiles + 1;
  vector<int> usedBlocks;

  for ( unsigned int i = 0; i < nDataFiles; i++ ) {
    indexDPBlock = ( i + 1 ) * ( nDataFiles + 1 ) +  i;
    usedBlocks.push_back( indexDPBlock );
  }

  for ( unsigned int i = 0; i < nDataFiles; i++ ) {
    indexDPBlock = ( i + 1 ) * ( nDataFiles + 1 ) +  i;
    nextBlock = i + jumpBlocks;
    operationXOR( dataBlocks[i], dataBlocks[nextBlock], dataBlocks[indexDPBlock], stripeWidth );
    usedBlocks.push_back( i );
    usedBlocks.push_back( nextBlock );

    for ( unsigned int j = 0; j < nDataFiles - 2; j++ ) {
      auxBlock = nextBlock + jumpBlocks;

      if ( ( auxBlock < nTotalBlocks ) &&
           ( find( usedBlocks.begin(), usedBlocks.end(), auxBlock ) == usedBlocks.end() ) )
      {
        nextBlock = auxBlock;
      } else {
        nextBlock++;

        while ( find( usedBlocks.begin(), usedBlocks.end(), nextBlock ) != usedBlocks.end() ) {
          nextBlock++;
        }
      }

      operationXOR( dataBlocks[indexDPBlock],
                    dataBlocks[nextBlock],
                    dataBlocks[indexDPBlock],
                    stripeWidth );
      
      usedBlocks.push_back( nextBlock );
    }
  }
}


// -----------------------------------------------------------------------------
// XOR the two blocks using 128 bits and return the result
// -----------------------------------------------------------------------------
void
RaidDpFile::operationXOR( char* block1, char* block2, char* result, size_t totalBytes )
{
  v2do* xor_res;
  v2do* idx1;
  v2do* idx2;
  char* byte_res;
  char* byte_idx1;
  char* byte_idx2;
  long int noPices = -1;

  idx1 = ( v2do* ) block1;
  idx2 = ( v2do* ) block2;
  xor_res = ( v2do* ) result;

  noPices = totalBytes / sizeof( v2do );

  for ( unsigned int i = 0; i < noPices; idx1++, idx2++, xor_res++, i++ ) {
    *xor_res = *idx1 ^ *idx2;
  }

  // ---------------------------------------------------------------------------
  // If the block does not devide perfectly to 128 ...
  // ---------------------------------------------------------------------------
  if ( totalBytes % sizeof( v2do ) != 0 ) {
    byte_res = ( char* ) xor_res;
    byte_idx1 = ( char* ) idx1;
    byte_idx2 = ( char* ) idx2;

    for ( unsigned int i = noPices * sizeof( v2do );
          i < totalBytes;
          byte_res++, byte_idx1++, byte_idx2++, i++ ) {
      *byte_res = *byte_idx1 ^ *byte_idx2;
    }
  }
}


// -----------------------------------------------------------------------------
// Try to recover the block at the current offset
// -----------------------------------------------------------------------------
bool
RaidDpFile::recoverPieces( off_t                    offset,
                           char*                    buffer,
                           std::map<off_t, size_t>& mapToRecover )
{
  // ---------------------------------------------------------------------------
  // Obs: doubleParityRecover also checks the simple and double parity blocks
  // ---------------------------------------------------------------------------
  doneRecovery = doubleParityRecover( offset, buffer, mapToRecover );
  return doneRecovery;
}


// -----------------------------------------------------------------------------
// Use simple and double parity to recover corrpted pieces
// -----------------------------------------------------------------------------
bool
RaidDpFile::doubleParityRecover( off_t                    offsetInit,
                                 char*                    buffer,
                                 std::map<off_t, size_t>& mapToRecover )
{
  bool* statusBlock;
  char* pBuff;
  size_t length;
  off_t offsetLocal;
  unsigned int idStripe;
  vector<int> corruptId;
  vector<int> excludeId;
  off_t offset = mapToRecover.begin()->first;
  off_t offsetGroup = ( offset / sizeGroup ) * sizeGroup;
  std::map<uint64_t, uint32_t> mapErrors;

  vector<unsigned int> sParityIndx = getSimpleParityIndices();
  vector<unsigned int> dParityIndx = getDoubleParityIndices();

  statusBlock = ( bool* ) calloc( nTotalBlocks, sizeof( bool ) );

  // ---------------------------------------------------------------------------
  // Reset the read and write handlers
  // ---------------------------------------------------------------------------
  for ( unsigned int i = 0; i < nTotalFiles; i++ ) {
    vReadHandler[i]->Reset();
    vWriteHandler[i]->Reset();
  }

  for ( unsigned int i = 0; i < nTotalBlocks; i++ ) {
    memset( dataBlocks[i], 0, stripeWidth );
    statusBlock[i] = true;
    idStripe = i % nTotalFiles;
    offsetLocal = ( offsetGroup / ( nDataFiles * stripeWidth ) ) *  stripeWidth +
                  ( ( i / nTotalFiles ) * stripeWidth );
  
    vReadHandler[idStripe]->Increment();
    xrdFile[mapSU[idStripe]]->Read( offsetLocal + sizeHeader, stripeWidth,
                                    dataBlocks[i], vReadHandler[idStripe] );
  }

  // ---------------------------------------------------------------------------
  // Mark the corrupted blocks
  // ---------------------------------------------------------------------------
  for ( unsigned int i = 0; i < nTotalFiles; i++ ) {
    if ( !vReadHandler[i]->WaitOK() ) {
      mapErrors = vReadHandler[i]->GetErrorsMap();

      for ( std::map<uint64_t, uint32_t>::iterator iter = mapErrors.begin();
            iter != mapErrors.end();
            iter++ )
      {
        off_t offStripe = iter->first - sizeHeader;
        int indexStripe = ( offStripe % ( nDataFiles * stripeWidth ) ) / stripeWidth;
        int index = indexStripe * nTotalFiles + i;

        statusBlock[index] = false;
        corruptId.push_back( index );
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Recovery algorithm
  // ---------------------------------------------------------------------------
  unsigned int stripeId;
  unsigned int idBlockCorrupted;

  vector<unsigned int> horizontalStripe;
  vector<unsigned int> diagonalStripe;

  while ( !corruptId.empty() ) {
    idBlockCorrupted = corruptId.back();
    corruptId.pop_back();
    //fprintf( stdout, "After pop there are %zu corrupted elem left\n", corruptId.size() );

    if ( validHorizStripe( horizontalStripe, statusBlock, idBlockCorrupted ) ) {
      // -----------------------------------------------------------------------
      // Try to recover using simple parity
      // -----------------------------------------------------------------------
      memset( dataBlocks[idBlockCorrupted], 0, stripeWidth );

      for ( unsigned int ind = 0;  ind < horizontalStripe.size(); ind++ ) {
        if ( horizontalStripe[ind] != idBlockCorrupted ) {
          operationXOR( dataBlocks[idBlockCorrupted],
                        dataBlocks[horizontalStripe[ind]],
                        dataBlocks[idBlockCorrupted],
                        stripeWidth );
        }
      }

      // -----------------------------------------------------------------------
      // Return recovered block and also write it to the file
      // -----------------------------------------------------------------------
      stripeId = idBlockCorrupted % nTotalFiles;
      offsetLocal = ( ( offsetGroup / ( nDataFiles * stripeWidth ) ) * stripeWidth ) +
                    ( ( idBlockCorrupted / nTotalFiles ) * stripeWidth );

      if ( storeRecovery ) {
        vWriteHandler[stripeId]->Increment();
        xrdFile[mapSU[stripeId]]->Write( offsetLocal + sizeHeader, stripeWidth,
                                         dataBlocks[idBlockCorrupted], vWriteHandler[stripeId] );
      }

      for ( std::map<off_t, size_t>::iterator iter = mapToRecover.begin();
            iter != mapToRecover.end();
            iter++ )
      {
        offset = iter->first;
        length = iter->second;

        // -----------------------------------------------------------------------
        // If not SP or DP, maybe we have to return it
        // -----------------------------------------------------------------------
        if ( find( sParityIndx.begin(), sParityIndx.end(), idBlockCorrupted ) == sParityIndx.end() &&
             find( dParityIndx.begin(), dParityIndx.end(), idBlockCorrupted ) == dParityIndx.end() ) {
          if ( ( offset >= ( off_t )( offsetGroup + mapBigToSmall( idBlockCorrupted ) * stripeWidth ) ) &&
               ( offset < ( off_t )( offsetGroup + ( mapBigToSmall( idBlockCorrupted ) + 1 ) * stripeWidth ) ) ) {
            pBuff = buffer + ( offset - offsetInit );
            memcpy( pBuff, dataBlocks[idBlockCorrupted] + ( offset % stripeWidth ), length );
          }
        }
      }

      // -----------------------------------------------------------------------
      // Copy the unrecoverd blocks back in the queue
      // -----------------------------------------------------------------------
      if ( !excludeId.empty() ) {
        corruptId.insert( corruptId.end(), excludeId.begin(), excludeId.end() );
        excludeId.clear();
      }

      statusBlock[idBlockCorrupted] = true;
    } else {

      // -----------------------------------------------------------------------
      // Try to recover using double parity
      // -----------------------------------------------------------------------
      if ( validDiagStripe( diagonalStripe, statusBlock, idBlockCorrupted ) ) {
        memset( dataBlocks[idBlockCorrupted], 0, stripeWidth );

        for ( unsigned int ind = 0;  ind < diagonalStripe.size(); ind++ ) {
          if ( diagonalStripe[ind] != idBlockCorrupted ) {
            operationXOR( dataBlocks[idBlockCorrupted],
                          dataBlocks[diagonalStripe[ind]],
                          dataBlocks[idBlockCorrupted],
                          stripeWidth );
          }
        }

        // -----------------------------------------------------------------------
        // Return recovered block and also write it to the file
        // -----------------------------------------------------------------------
        stripeId = idBlockCorrupted % nTotalFiles;
        offsetLocal = ( ( offsetGroup / ( nDataFiles * stripeWidth ) ) * stripeWidth ) +
                      ( ( idBlockCorrupted / nTotalFiles ) * stripeWidth );

        if ( storeRecovery ) {
          vWriteHandler[stripeId]->Increment();
          xrdFile[mapSU[stripeId]]->Write( offsetLocal + sizeHeader, stripeWidth,
                                           dataBlocks[idBlockCorrupted], vWriteHandler[stripeId] );
        }

        for ( std::map<off_t, size_t>::iterator iter = mapToRecover.begin();
              iter != mapToRecover.end();
              iter++ )
        {
          offset = iter->first;
          length = iter->second;

          // --------------------------------------------------------------------
          // If not SP or DP, maybe we have to return it
          // --------------------------------------------------------------------
          if ( find( sParityIndx.begin(), sParityIndx.end(), idBlockCorrupted ) == sParityIndx.end() &&
               find( dParityIndx.begin(), dParityIndx.end(), idBlockCorrupted ) == dParityIndx.end() ) {
            if ( ( offset >= ( off_t )( offsetGroup + mapBigToSmall( idBlockCorrupted ) * stripeWidth ) ) &&
                 ( offset < ( off_t )( offsetGroup + ( mapBigToSmall( idBlockCorrupted ) + 1 ) * stripeWidth ) ) ) {
              pBuff = buffer + ( offset - offsetInit );
              memcpy( pBuff, dataBlocks[idBlockCorrupted] + ( offset % stripeWidth ), length );
            }
          }
        }

        // --------------------------------------------------------------------
        // Copy the unrecoverd blocks back in the queue
        // --------------------------------------------------------------------
        if ( !excludeId.empty() ) {
          corruptId.insert( corruptId.end(), excludeId.begin(), excludeId.end() );
          excludeId.clear();
        }

        statusBlock[idBlockCorrupted] = true;
      } else {
        // --------------------------------------------------------------------
        // Current block can not be recoverd in this configuration
        // --------------------------------------------------------------------
        excludeId.push_back( idBlockCorrupted );
      }
    }
  }

  // --------------------------------------------------------------------
  // Wait for write responses and reset all handlers
  // --------------------------------------------------------------------
  for (unsigned int i = 0; i < nTotalFiles; i++) {
    if ( !vWriteHandler[i]->WaitOK() ) {
      free( statusBlock );
      eos_err( "Write stripe %s- write failed", stripeUrls[mapSU[i]].c_str() );
      return false;
    }
    
    vWriteHandler[i]->Reset();
    vReadHandler[i]->Reset();
  }

  free( statusBlock );

  if ( corruptId.empty() && !excludeId.empty() ) {
    return false;
  }

  return true;
}


// -----------------------------------------------------------------------------
// Add a new data used to compute parity block
// -----------------------------------------------------------------------------
void
RaidDpFile::addDataBlock( off_t offset, char* buffer, size_t length )
{
  int indxBlock;
  size_t nwrite;
  off_t offsetInBlock;
  off_t offsetInGroup = offset % sizeGroup;

  if ( ( offGroupParity == -1 ) && ( offset < static_cast<off_t>( sizeGroup ) ) ) {
    offGroupParity = 0;
  }

  if ( offsetInGroup == 0 ) {
    fullDataBlocks = false;

    for ( unsigned int i = 0; i < nTotalBlocks; i++ ) {
      memset( dataBlocks[i], 0, stripeWidth );
    }
  }

  char* ptr;
  size_t availableLength;

  while ( length ) {
    offsetInBlock = offsetInGroup % stripeWidth;
    availableLength = stripeWidth - offsetInBlock;
    indxBlock = mapSmallToBig( offsetInGroup / stripeWidth );

    nwrite = ( length > availableLength ) ? availableLength : length;
    ptr = dataBlocks[indxBlock];
    ptr += offsetInBlock;
    ptr = static_cast<char*>( memcpy( ptr, buffer, nwrite ) );

    offset += nwrite;
    length -= nwrite;
    buffer += nwrite;
    offsetInGroup = offset % sizeGroup;

    if ( offsetInGroup == 0 ) {
      // -----------------------------------------------------------------------
      // We completed a group, we can compute parity
      // -----------------------------------------------------------------------
      offGroupParity = ( ( offset - 1 ) / sizeGroup ) *  sizeGroup;
      fullDataBlocks = true;
      doBlockParity( offGroupParity );
      offGroupParity += sizeGroup;

      for ( unsigned int i = 0; i < nTotalBlocks; i++ ) {
        memset( dataBlocks[i], 0, stripeWidth );
      }
    }
  }
}


//--------------------------------------------------------------------------
// Get a list of the group offsets for which we can compute the parity info
//--------------------------------------------------------------------------
void
RaidDpFile::GetOffsetGroups( std::set<off_t>& offGroups, bool forceAll )
{
  size_t length;
  off_t offset;
  off_t offGroup;
  off_t offPieceEnd;
  bool doneDelete ;

  std::map<off_t, size_t>::iterator it = mapPieces.begin();

  fprintf( stdout, "[%s] Size of map is: %zu.\n", __FUNCTION__, mapPieces.size() );
 
  while ( it != mapPieces.end() ) {
    doneDelete = false;
    offset = it->first;
    length = it->second;
    offPieceEnd = offset + length;
    offGroup = ( offset / sizeGroup ) *  sizeGroup;

    if ( forceAll ) {
      fprintf( stdout, "[%s] Force all groups, \n", __FUNCTION__ );
      mapPieces.erase( it++ );
      offGroups.insert( offGroup );
      fprintf( stdout, "[%s] Insert group offset: %zu \n", __FUNCTION__, offGroup );
      offGroup += sizeGroup;
      
      while ( ( offGroup >= offset ) && ( offGroup <= offPieceEnd ) ) {
        offGroups.insert( offGroup );
        fprintf( stdout, "[%s] Insert group offset: %zu \n", __FUNCTION__, offGroup );
        offGroup += sizeGroup;
      }
    }
    else {

      if ( offGroup < offset ) offGroup += sizeGroup;

      fprintf( stdout, "[%s] Group offset: %zu, offPieceEnd = %zu, sizeGroup = %zu. \n",
               __FUNCTION__, offGroup, offPieceEnd, sizeGroup );
    
      while ( ( offGroup <= offPieceEnd ) &&
              ( static_cast<off_t>( offGroup + sizeGroup ) <= offPieceEnd ) )
        {
          if ( !doneDelete ) {
            mapPieces.erase( it++ );
            doneDelete = true;
          }
          if ( offGroup > offset ) {
            mapPieces.insert( std::make_pair( offset, offGroup - offset ) );
          }

          //------------------------------------------------------------------------
          // Save group offset in the list
          //------------------------------------------------------------------------
          fprintf( stdout, "[%s] Save group offset: %zu. \n", __FUNCTION__, offGroup );
          offGroups.insert( offGroup );
          offGroup += sizeGroup;  
        }

      if ( doneDelete && ( static_cast<off_t>( offGroup + sizeGroup ) > offPieceEnd ) ) {
        mapPieces.insert( std::make_pair( offGroup, offPieceEnd - offGroup ) );
      }

      if ( !doneDelete ) it++;
    }
  }
}


//--------------------------------------------------------------------------
// Read data from the current group for parity computation
//--------------------------------------------------------------------------
bool
RaidDpFile::ReadGroup(off_t offsetGroup)
{
  off_t offsetLocal;
  bool ret = true;
  int idStripe;
  
  for ( unsigned int i = 0; i < nDataFiles; i++ ) {
    vReadHandler[i]->Reset();
  }

  for ( unsigned int i = 0; i < nTotalBlocks; i++ ) {
    memset( dataBlocks[i], 0, stripeWidth );
  }
  
  for ( unsigned int i = 0; i < nDataBlocks; i++ ) {
    idStripe = i % nDataFiles;
    offsetLocal = ( offsetGroup / ( nDataFiles * stripeWidth ) ) *  stripeWidth +
                  ( ( i / nDataFiles ) * stripeWidth );
  
    vReadHandler[idStripe]->Increment();
    fprintf( stdout, "Read stripe: %i, offset: %zu.\n", idStripe, offsetLocal );
    xrdFile[mapSU[idStripe]]->Read( offsetLocal + sizeHeader, stripeWidth,
                                    dataBlocks[mapSmallToBig(i)], vReadHandler[idStripe] );
  }

  for ( unsigned int i = 0; i < nDataFiles; i++ ) {
    if ( !vReadHandler[i]->WaitOK() ) {
      eos_err( "err=error while reading data blocks" );
      ret = false;
    }
  }

  return ret;
}


// -----------------------------------------------------------------------------
// Compute and write parity blocks to files
// -----------------------------------------------------------------------------
void
RaidDpFile::doBlockParity( off_t offsetGroup )
{
  eos::common::Timing up( "parity" );

  COMMONTIMING( "Compute-In", &up );
  // ---------------------------------------------------------------------------
  // Do computations of parity blocks
  // ---------------------------------------------------------------------------
  computeParity();
  COMMONTIMING( "Compute-Out", &up );

  // ---------------------------------------------------------------------------
  // Write parity blocks to files
  // ---------------------------------------------------------------------------
  writeParityToFiles( offsetGroup );
  COMMONTIMING( "WriteParity", &up );

  fullDataBlocks = false;
  //  up.Print();
}


// -----------------------------------------------------------------------------
// Write the parity blocks from dataBlocks to the corresponding file stripes
// -----------------------------------------------------------------------------
int
RaidDpFile::writeParityToFiles( off_t offsetGroup )
{
  unsigned int idPFile;
  unsigned int idDPFile;
  unsigned int indexPBlock;
  unsigned int indexDPBlock;
  off_t offsetParityLocal;

  idPFile = nTotalFiles - 2;
  idDPFile = nTotalFiles - 1;

  vWriteHandler[idPFile]->Reset();
  vWriteHandler[idDPFile]->Reset();

  for ( unsigned int i = 0; i < nDataFiles; i++ ) {
    indexPBlock = ( i + 1 ) * nDataFiles + 2 * i;
    indexDPBlock = ( i + 1 ) * ( nDataFiles + 1 ) +  i;
    offsetParityLocal = ( offsetGroup / nDataFiles ) + ( i * stripeWidth );

    // -----------------------------------------------------------------------------
    // Writing simple parity
    // -----------------------------------------------------------------------------
    vWriteHandler[idPFile]->Increment();
    xrdFile[mapSU[idPFile]]->Write( offsetParityLocal + sizeHeader, stripeWidth,
                                    dataBlocks[indexPBlock], vWriteHandler[idPFile] );

    // -----------------------------------------------------------------------------
    // Writing double parity
    // -----------------------------------------------------------------------------
    vWriteHandler[idDPFile]->Increment();
    xrdFile[mapSU[idDPFile]]->Write( offsetParityLocal + sizeHeader, stripeWidth,
                                     dataBlocks[indexDPBlock], vWriteHandler[idDPFile] );
  }

  if ( !vWriteHandler[idPFile]->WaitOK() || !vWriteHandler[idDPFile]->WaitOK() ) {
    eos_err( "error=error while writing parity information" );
    return -1;
  }

  return SFS_OK;
}


// -----------------------------------------------------------------------------
// Return the indices of the simple parity blocks from a group
// -----------------------------------------------------------------------------
vector<unsigned int>
RaidDpFile::getSimpleParityIndices()
{
  unsigned int val = nDataFiles;
  vector<unsigned int> values;

  values.push_back( val );
  val++;

  for ( unsigned int i = 1; i < nDataFiles; i++ ) {
    val += ( nDataFiles + 1 );
    values.push_back( val );
    val++;
  }

  return values;
}


// -----------------------------------------------------------------------------
// Return the indices of the double parity blocks from a group
// -----------------------------------------------------------------------------
vector<unsigned int>
RaidDpFile::getDoubleParityIndices()
{
  unsigned int val = nDataFiles;
  vector<unsigned int> values;

  val++;
  values.push_back( val );

  for ( unsigned int i = 1; i < nDataFiles; i++ ) {
    val += ( nDataFiles + 1 );
    val++;
    values.push_back( val );
  }

  return values;
}


// -----------------------------------------------------------------------------
// Check if the diagonal stripe is valid in the sense that there is at most one
// corrupted block in the current stripe and this is not the ommited diagonal
// -----------------------------------------------------------------------------
bool
RaidDpFile::validDiagStripe( std::vector<unsigned int>& stripe,
                             bool*                      statusBlock,
                             unsigned int               blockId )
{
  int corrupted = 0;
  stripe.clear();
  stripe = getDiagonalStripe( blockId );

  if ( stripe.size() == 0 ) return false;

  // ---------------------------------------------------------------------------
  // The ommited diagonal contains the block with index nDataFilesBlocks
  // ---------------------------------------------------------------------------
  if ( find( stripe.begin(), stripe.end(), nDataFiles ) != stripe.end() )
    return false;

  for ( std::vector<unsigned int>::iterator iter = stripe.begin();
        iter != stripe.end();
        ++iter )
  {
    if ( statusBlock[*iter] == false ) {
      corrupted++;
    }

    if ( corrupted >= 2 ) {
      return false;
    }
  }

  return true;
}


// -----------------------------------------------------------------------------
// Check if the HORIZONTAL stripe is valid in the sense that there is at
// most one corrupted block in the current stripe
// -----------------------------------------------------------------------------
bool
RaidDpFile::validHorizStripe( std::vector<unsigned int>& stripe,
                              bool*                      statusBlock,
                              unsigned int               blockId )
{
  int corrupted = 0;
  long int baseId = ( blockId / nTotalFiles ) * nTotalFiles;
  stripe.clear();

  // ---------------------------------------------------------------------------
  // If double parity block then no horizontal stripes
  // ---------------------------------------------------------------------------
  if ( blockId == ( baseId + nDataFiles + 1 ) )
    return false;

  for ( unsigned int i = 0; i < nTotalFiles - 1; i++ )
    stripe.push_back( baseId + i );

  //check if it is valid
  for ( std::vector<unsigned int>::iterator iter = stripe.begin();
        iter != stripe.end();
        ++iter ) {
    if ( statusBlock[*iter] == false ) {
      corrupted++;
    }

    if ( corrupted >= 2 ) {
      return false;
    }
  }

  return true;
}


// -----------------------------------------------------------------------------
// Return the blocks corrsponding to the diagonal stripe of blockId
// -----------------------------------------------------------------------------
std::vector<unsigned int>
RaidDpFile::getDiagonalStripe( unsigned int blockId )
{
  bool dpAdded = false;
  std::vector<unsigned int> lastColumn = getDoubleParityIndices();

  unsigned int nextBlock;
  unsigned int jumpBlocks;
  unsigned int idLastBlock;
  unsigned int previousBlock;
  std::vector<unsigned int> stripe;

  // ---------------------------------------------------------------------------
  // If we are on the ommited diagonal, return
  // ---------------------------------------------------------------------------
  if ( blockId == nDataFiles ) {
    stripe.clear();
    return stripe;
  }

  stripe.push_back( blockId );

  // ---------------------------------------------------------------------------
  // If we start with a dp index, then construct the diagonal in a special way
  // ---------------------------------------------------------------------------
  if ( find( lastColumn.begin(), lastColumn.end(), blockId ) != lastColumn.end() ) {
    blockId = blockId % ( nDataFiles + 1 );
    stripe.push_back( blockId );
    dpAdded = true;
  }

  previousBlock = blockId;
  jumpBlocks = nDataFiles + 3;
  idLastBlock = nTotalBlocks - 1;

  for ( unsigned int i = 0 ; i < nDataFiles - 1; i++ ) {
    nextBlock = previousBlock + jumpBlocks;

    if ( nextBlock > idLastBlock ) {
      nextBlock %= idLastBlock;

      if ( nextBlock >= nDataFiles + 1 ) {
        nextBlock = ( previousBlock + jumpBlocks ) % jumpBlocks;
      }
    } else if ( find( lastColumn.begin(), lastColumn.end(), nextBlock ) != lastColumn.end() ) {
      nextBlock = previousBlock + 2;
    }

    stripe.push_back( nextBlock );
    previousBlock = nextBlock;

    // -------------------------------------------------------------------------
    // If on the ommited diagonal return
    // -------------------------------------------------------------------------
    if ( nextBlock == nDataFiles ) {
      eos_debug( "Return empty vector - ommited diagonal" );
      stripe.clear();
      return stripe;
    }
  }

  // ---------------------------------------------------------------------------
  // Add the index from the double parity block
  // ---------------------------------------------------------------------------
  if ( !dpAdded ) {
    nextBlock = getDParityBlockId( stripe );
    stripe.push_back( nextBlock );
  }

  return stripe;
}


// -----------------------------------------------------------------------------
// Return the id of stripe from a nTotalBlocks representation to a nDataBlocks
// representation in which we exclude the parity and double parity blocks
// -----------------------------------------------------------------------------
unsigned int
RaidDpFile::mapBigToSmall( unsigned int idBig )
{
  if ( idBig % ( nDataFiles + 2 ) == nDataFiles  || idBig % ( nDataFiles + 2 ) == nDataFiles + 1 )
    return -1;
  else
    return ( ( idBig / ( nDataFiles + 2 ) ) * nDataFiles + ( idBig % ( nDataFiles + 2 ) ) );
}


// -----------------------------------------------------------------------------
// Return the id of stripe from a nDataBlocks representation in a nTotalBlocks
// representation
// -----------------------------------------------------------------------------
unsigned int
RaidDpFile::mapSmallToBig( unsigned int idSmall )
{
  if ( idSmall >= nDataBlocks ) {
    eos_err( "error=idSmall bugger than expected" );
    return -1;
  }

  return ( idSmall / nDataFiles ) * ( nDataFiles + 2 ) + idSmall % nDataFiles;
}


// -----------------------------------------------------------------------------
// Return the id (out of nTotalBlocks) for the parity block corresponding to
// the current block
// -----------------------------------------------------------------------------
unsigned int
RaidDpFile::getParityBlockId( unsigned int elemFromStripe )
{
  return ( nDataFiles + ( elemFromStripe / ( nDataFiles + 2 ) ) * ( nDataFiles + 2 ) );
}


// -----------------------------------------------------------------------------
// Return the id (out of nTotalBlocks) for the double parity block corresponding
// to the current block
// -----------------------------------------------------------------------------
unsigned int
RaidDpFile::getDParityBlockId( std::vector<unsigned int> stripe )
{
  int min = *( std::min_element( stripe.begin(), stripe.end() ) );
  return ( ( min + 1 ) * ( nDataFiles + 1 ) + min );
}


// -----------------------------------------------------------------------------
// Truncate file
// -----------------------------------------------------------------------------
int
RaidDpFile::truncate( off_t offset )
{

  int rc = SFS_OK;
  off_t truncateOffset = 0;

  if ( !offset ) return rc;

  truncateOffset = ceil( ( offset * 1.0 ) / sizeGroup ) * stripeWidth * nDataFiles;
  truncateOffset += sizeHeader;

  for ( unsigned int i = 0; i < nTotalFiles; i++ ) {
    if ( !( xrdFile[i]->Truncate( truncateOffset ).IsOK() ) ) {
      eos_err( "error=error while truncating" );
      return -1;
    }
  }

  return rc;
}


/*
OBS:: can be used if updates are allowed
// -----------------------------------------------------------------------------
// Recompute and write to files the parity blocks of the groups between the two limits
// -----------------------------------------------------------------------------
int
RaidDpFile::updateParityForGroups(off_t offsetStart, off_t offsetEnd)
{
  off_t offsetGroup;
  off_t offsetBlock;

  eos::common::Timing up("parity");

  for (unsigned int i = (offsetStart / sizeGroup); i < ceil((offsetEnd * 1.0) / sizeGroup); i++)
  {
    offsetGroup = i * sizeGroup;
    for(unsigned int j = 0; j < nDataBlocks; j++)
    {
      XrdOucString block = "block-"; block += (int)j;
      TIMING(block.c_str(),&up);

      offsetBlock = offsetGroup + j * stripeWidth;
      read(offsetBlock, dataBlocks[mapSmallToBig(j)], stripeWidth);
      block += "-read";
      TIMING(block.c_str(),&up);
    }

    TIMING("Compute-In",&up);
    //do computations of parity blocks
    computeParity();
    TIMING("Compute-Out",&up);

    //write parity blocks to files
    writeParityToFiles(offsetGroup);
    TIMING("WriteParity",&up);
  }
  //  up.Print();
  return SFS_OK;
}
  
  
// -----------------------------------------------------------------------------
// Use simple parity to recover the block - NOT USED!!
// -----------------------------------------------------------------------------
bool
RaidDpFile::simpleParityRecover( char*  buffer,
                                 off_t  offset,
                                 size_t length,
                                 int&   blocksCorrupted )
{
  uint32_t aread;
  int idBlockCorrupted = -1;
  off_t offsetLocal = ( offset / ( nDataFiles * stripeWidth ) ) * stripeWidth;

  blocksCorrupted = 0;

  for ( unsigned int i = 0; i < nTotalFiles; i++ ) {
    if ( !( xrdFile[mapSU[i]] ) ||
         !( xrdFile[mapSU[i]]->Read( offsetLocal + sizeHeader,
                                     stripeWidth,
                                     dataBlocks[i],
                                     aread ).IsOK() ) ||
         ( aread != stripeWidth ) ) {
      eos_err( "Read stripe %s - corrupted block", stripeUrls[mapSU[i]].c_str() );
      idBlockCorrupted = i; // [0 - nDataFilesBlocks]
      blocksCorrupted++;
    }

    if ( blocksCorrupted > 1 ) {
      break;
    }
  }

  if ( blocksCorrupted == 0 )
    return true;
  else if ( blocksCorrupted >= 2 )
    return false;

  // ----------------------------------------------------------------------------
  // Use simple parity to recover
  // ----------------------------------------------------------------------------
  operationXOR( dataBlocks[( idBlockCorrupted + 1 ) % ( nDataFiles + 1 )],
                dataBlocks[( idBlockCorrupted + 2 ) % ( nDataFiles + 1 )],
                dataBlocks[idBlockCorrupted],
                stripeWidth );

  for ( unsigned int i = 3, index = ( idBlockCorrupted + i ) % ( nDataFiles + 1 );
        i < ( nDataFiles + 1 ) ;
        i++, index = ( idBlockCorrupted + i ) % ( nDataFiles + 1 ) )
  {
    operationXOR( dataBlocks[idBlockCorrupted],
                  dataBlocks[index],
                  dataBlocks[idBlockCorrupted],
                  stripeWidth );
  }

  // ---------------------------------------------------------------------------
  // Return recovered block and also write it to the file
  // ---------------------------------------------------------------------------
  int idReadBlock;
  unsigned int stripeId;
  off_t offsetBlock;

  idReadBlock = ( offset % ( nDataFiles * stripeWidth ) ) / stripeWidth; // [0-3]
  offsetBlock = ( offset / ( nDataFiles * stripeWidth ) ) * ( nDataFiles * stripeWidth ) +
                idReadBlock * stripeWidth;
  stripeId = ( offsetBlock / stripeWidth ) % nDataFiles;
  offsetLocal = ( ( offsetBlock / ( nDataFiles * stripeWidth ) ) * stripeWidth );

  if ( storeRecovery ) {
    if ( !( xrdFile[mapSU[stripeId]]->Write( offsetLocal + sizeHeader, stripeWidth,
            dataBlocks[idBlockCorrupted] ).IsOK() ) ) {
      eos_err( "Write stripe %s- write failed", stripeUrls[mapSU[stripeId]].c_str() );
      return false;
    }
  }

  // ---------------------------------------------------------------------------
  // Write the correct block to the reading buffer
  // ---------------------------------------------------------------------------
  memcpy( buffer, dataBlocks[idReadBlock] + ( offset % stripeWidth ), length );
  return true;
}
*/


EOSFSTNAMESPACE_END

