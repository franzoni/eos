#-------------------------------------------------------------------------------
# File: CMakeLists.txt
# Author: Elvin-Alin Sindrilaru <esindril@cern.ch> CERN
#-------------------------------------------------------------------------------

# ************************************************************************
# * EOS - the CERN Disk Storage System                                   *
# * Copyright (C) 2013 CERN/Switzerland                                  *
# *                                                                      *
# * This program is free software: you can redistribute it and/or modify *
# * it under the terms of the GNU General Public License as published by *
# * the Free Software Foundation, either version 3 of the License, or    *
# * (at your option) any later version.                                  *
# *                                                                      *
# * This program is distributed in the hope that it will be useful,      *
# * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
# * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
# * GNU General Public License for more details.                         *
# *                                                                      *
# * You should have received a copy of the GNU General Public License    *
# * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
# ************************************************************************

include_directories(
  ${CMAKE_SOURCE_DIR}
  ${ZMQ_INCLUDE_DIRS}
  ${PROTOBUF_INCLUDE_DIRS}
  ${XROOTD_INCLUDE_DIRS}
  ${CMAKE_CURRENT_BINARY_DIR})

if (CPPUNIT_FOUND)
  add_subdirectory(tests)
endif(CPPUNIT_FOUND)


#-------------------------------------------------------------------------------
# Generate all protocol buffer files
#-------------------------------------------------------------------------------
PROTOBUF_GENERATE_CPP(XSE_SRCS XSE_HDRS PROTOFILES proto/XrdSecEntity.proto)
PROTOBUF_GENERATE_CPP(XOEI_SRCS XOEI_HDRS PROTOFILES proto/XrdOucErrInfo.proto)
PROTOBUF_GENERATE_CPP(XSFS_SRCS XSFS_HDRS PROTOFILES proto/XrdSfsFSctl.proto)
PROTOBUF_GENERATE_CPP(STAT_SRCS STAT_HDRS PROTOFILES proto/Stat.proto)
PROTOBUF_GENERATE_CPP(FSCTL1_SRCS FSCTL1_HDRS PROTOFILES proto/Fsctl.proto)
PROTOBUF_GENERATE_CPP(FSCTL2_SRCS FSCTL2_HDRS PROTOFILES proto/FS_ctl.proto)
PROTOBUF_GENERATE_CPP(CHMOD_SRCS CHMOD_HDRS PROTOFILES proto/Chmod.proto)
PROTOBUF_GENERATE_CPP(CHKSUM_SRCS CHKSUM_HDRS PROTOFILES proto/Chksum.proto)
PROTOBUF_GENERATE_CPP(EXISTS_SRCS EXISTS_HDRS PROTOFILES proto/Exists.proto)
PROTOBUF_GENERATE_CPP(MKDIR_SRCS MKDIR_HDRS PROTOFILES proto/Mkdir.proto)
PROTOBUF_GENERATE_CPP(REMDIR_SRCS REMDIR_HDRS PROTOFILES proto/Remdir.proto)
PROTOBUF_GENERATE_CPP(REM_SRCS REM_HDRS PROTOFILES proto/Rem.proto)
PROTOBUF_GENERATE_CPP(RENAME_SRCS RENAME_HDRS PROTOFILES proto/Rename.proto)
PROTOBUF_GENERATE_CPP(XSP_SRCS XSP_HDRS PROTOFILES proto/XrdSfsPrep.proto)
PROTOBUF_GENERATE_CPP(PREPARE_SRCS PREPARE_HDRS PROTOFILES proto/Prepare.proto)
PROTOBUF_GENERATE_CPP(TRUNCATE_SRCS TRUNCATE_HDRS PROTOFILES proto/Truncate.proto)
PROTOBUF_GENERATE_CPP(DOPEN_SRCS DOPEN_HDRS PROTOFILES proto/DirOpen.proto)
PROTOBUF_GENERATE_CPP(DREAD_SRCS DREAD_HDRS PROTOFILES proto/DirRead.proto)
PROTOBUF_GENERATE_CPP(DFNAME_SRCS DFNAME_HDRS PROTOFILES proto/DirFname.proto)
PROTOBUF_GENERATE_CPP(DCLOSE_SRCS DCLOSE_HDRS PROTOFILES proto/DirClose.proto)
PROTOBUF_GENERATE_CPP(FOPEN_SRCS FOPEN_HDRS PROTOFILES proto/FileOpen.proto)
PROTOBUF_GENERATE_CPP(FFNAME_SRCS FFNAME_HDRS PROTOFILES proto/FileFname.proto)
PROTOBUF_GENERATE_CPP(FSTAT_SRCS FSTAT_HDRS PROTOFILES proto/FileStat.proto)
PROTOBUF_GENERATE_CPP(FREAD_SRCS FREAD_HDRS PROTOFILES proto/FileRead.proto)
PROTOBUF_GENERATE_CPP(FWRITE_SRCS FWRITE_HDRS PROTOFILES proto/FileWrite.proto)
PROTOBUF_GENERATE_CPP(FCLOSE_SRCS FCLOSE_HDRS PROTOFILES proto/FileClose.proto)
PROTOBUF_GENERATE_CPP(REQ_SRCS REQ_HDRS PROTOFILES proto/Request.proto)
PROTOBUF_GENERATE_CPP(RESP_SRCS RESP_HDRS PROTOFILES proto/Response.proto)

set(
  PROTO_SRCS
  ${XSE_SRCS}     ${XOEI_SRCS}     ${XSFS_SRCS}   ${STAT_SRCS}
  ${FSCTL1_SRCS}  ${FSCTL2_SRCS}   ${REQ_SRCS}    ${RESP_SRCS}
  ${CHMOD_SRCS}   ${CHKSUM_SRCS}   ${EXISTS_SRCS} ${MKDIR_SRCS}
  ${REMDIR_SRCS}  ${REM_SRCS}      ${RENAME_SRCS} ${XSP_SRCS}
  ${PREPARE_SRCS} ${TRUNCATE_SRCS} ${DOPEN_SRCS}  ${DREAD_SRCS}
  ${DFNAME_SRCS}  ${DCLOSE_SRCS}   ${FOPEN_SRCS}  ${FCLOSE_SRCS}
  ${FFNAME_SRCS}  ${FSTAT_SRCS}    ${FREAD_SRCS}  ${FWRITE_SRCS})

set(
  PROTO_HDRS
  ${XSE_HDRS}     ${XOEI_HDRS}     ${XSFS_HDRS}   ${STAT_HDRS}
  ${FSCTL1_HDRS}  ${FSCTL2_HDRS}   ${REQ_HDRS}    ${RESP_HDRS}
  ${CHMOD_HDRS}   ${CHKSUM_HDRS}   ${EXITS_HDRS}  ${MKDIR_HDRS}
  ${REMDIR_HDRS}  ${REM_HDRS}      ${RENAME_HDRS} ${XSP_HDRS}
  ${PREPARE_HDRS} ${TRUNCATE_HDRS} ${DOPEN_HDRS}  ${DREAD_HDRS}
  ${DFNAME_HDRS}  ${DCLOSE_HDRS}   ${FOPEN_HDRS}  ${FCLOSE_HDRS}
  ${FFNAME_HDRS}  ${FSTAT_HDRS}    ${FREAD_HDRS}  ${FWRITE_HDRS})

set_source_files_properties(
  ${PROTO_SRCS}
  ${PROTO_HDRS}
  PROPERTIES GENERATED 1)

#-------------------------------------------------------------------------------
# EosAuthProto library
#-------------------------------------------------------------------------------
add_library(
  EosAuthProto SHARED
  ProtoUtils.cc ProtoUtils.hh
  ${PROTO_SRCS} ${PROTO_HDRS})

target_link_libraries(
   EosAuthProto
   eosCommon
   ${PROTOBUF_LIBRARIES})

#-------------------------------------------------------------------------------
# EosAuthOfs library
#-------------------------------------------------------------------------------
add_library(
  EosAuthOfs MODULE
  EosAuthOfs.cc  EosAuthOfs.hh
  EosAuthOfsFile.cc EosAuthOfsFile.hh
  EosAuthOfsDirectory.cc EosAuthOfsDirectory.hh)

target_link_libraries(
  EosAuthOfs
  eosCommon
  EosAuthProto
  ${ZMQ_LIBRARIES}
  ${XROOTD_CL_LIBRARY}
  ${XROOTD_UTILS_LIBRARY})

set_target_properties(
  EosAuthProto
  PROPERTIES
  VERSION ${VERSION}
  SOVERSION ${VERSION_MAJOR}
  INTERFACE_LINK_LIBRARIES ""
  LINK_INTERFACE_LIBRARIES "")

install(
  TARGETS EosAuthOfs EosAuthProto
  LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR})
