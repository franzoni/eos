# ----------------------------------------------------------------------
# File: CMakeLists.txt
# Author: Andreas-Joachim Peters - CERN
# ----------------------------------------------------------------------

# ************************************************************************
# * EOS - the CERN Disk Storage System                                   *
# * Copyright (C) 2016 CERN/Switzerland                                  *
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

include_directories(${CPPUNIT_INCLUDE_DIRS})

#-------------------------------------------------------------------------------
# EosNsOnRedisTests library
#-------------------------------------------------------------------------------
add_library(
  EosNsOnRedisTests MODULE
  ContainerMDSvcTest.cc
  FileMDSvcTest.cc
  HierarchicalViewTest.cc
  FileSystemViewTest.cc
  OtherTests.cc)


target_link_libraries(
  EosNsOnRedisTests
  EosNsOnRedis-Static
  ${CPPUNIT_LIBRARIES})


#-------------------------------------------------------------------------------
# eosnsbench executable
#-------------------------------------------------------------------------------
add_executable(eosnsbench EosNamespaceBenchmark.cc)

target_compile_options(
  eosnsbench
  PUBLIC -DFILE_OFFSET_BITS=64)

target_link_libraries(eosnsbench EosNsOnRedis-Static eosCommon-Static)

install(
  TARGETS
  eosnsbench
  LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR})
