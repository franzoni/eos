// ----------------------------------------------------------------------
// File: HttpServer.hh
// Author: Andreas-Joachim Peters & Justin Lewis Salmon - CERN
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

/**
 * @file   HttpServer.hh
 *
 * @brief  Creates an Http redirector instance running on the MGM
 */

#ifndef __EOSMGM_HTTPSERVER__HH__
#define __EOSMGM_HTTPSERVER__HH__

/*----------------------------------------------------------------------------*/
#include "mgm/Namespace.hh"
#include "common/http/HttpServer.hh"
#include "common/Mapping.hh"
/*----------------------------------------------------------------------------*/
#include "XrdSys/XrdSysPthread.hh"
/*----------------------------------------------------------------------------*/
#include <map>
#include <string>
/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_BEGIN

class HttpServer : public eos::common::HttpServer
{

private:
  std::string     mGridMapFile;            //!< contents of the gridmap file
  struct timespec mGridMapFileLastModTime; //!< last modification time of the
                                           //!< gridmap file

public:
  /**
   * Constructor
   */
  HttpServer (int port = 8000) :
    eos::common::HttpServer(port), mGridMapFileLastModTime{0} {}

  /**
   * Destructor
   */
  virtual ~HttpServer () {};

#ifdef EOS_MICRO_HTTPD
  /**
   * HTTP object handler function on MGM
   *
   * @return see implementation
   */
  virtual int
  Handler (void                  *cls,
           struct MHD_Connection *connection,
           const char            *url,
           const char            *method,
           const char            *version,
           const char            *upload_data,
           size_t                *upload_data_size,
           void                 **ptr);
#endif

  /**
   * Authenticate the client request by inspecting the SSL headers which were
   * transmitted by the reverse proxy server and attempting to map the client
   * DN to the gridmap file.
   *
   * @param headers  the map of client request headers
   *
   * @return an appropriately filled virtual identity
   */
  eos::common::Mapping::VirtualIdentity*
  Authenticate (std::map<std::string, std::string> &headers);

};

/*----------------------------------------------------------------------------*/
EOSMGMNAMESPACE_END

#endif
