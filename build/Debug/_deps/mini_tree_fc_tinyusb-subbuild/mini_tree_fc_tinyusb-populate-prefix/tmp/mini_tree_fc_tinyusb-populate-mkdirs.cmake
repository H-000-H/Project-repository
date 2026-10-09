# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/device_platform/CH32V307/Host-Device-Architecture-CH32V307-node/build/Debug/_deps/mini_tree_fc_tinyusb-src")
  file(MAKE_DIRECTORY "C:/device_platform/CH32V307/Host-Device-Architecture-CH32V307-node/build/Debug/_deps/mini_tree_fc_tinyusb-src")
endif()
file(MAKE_DIRECTORY
  "C:/device_platform/CH32V307/Host-Device-Architecture-CH32V307-node/build/Debug/_deps/mini_tree_fc_tinyusb-build"
  "C:/device_platform/CH32V307/Host-Device-Architecture-CH32V307-node/build/Debug/_deps/mini_tree_fc_tinyusb-subbuild/mini_tree_fc_tinyusb-populate-prefix"
  "C:/device_platform/CH32V307/Host-Device-Architecture-CH32V307-node/build/Debug/_deps/mini_tree_fc_tinyusb-subbuild/mini_tree_fc_tinyusb-populate-prefix/tmp"
  "C:/device_platform/CH32V307/Host-Device-Architecture-CH32V307-node/build/Debug/_deps/mini_tree_fc_tinyusb-subbuild/mini_tree_fc_tinyusb-populate-prefix/src/mini_tree_fc_tinyusb-populate-stamp"
  "C:/device_platform/CH32V307/Host-Device-Architecture-CH32V307-node/build/Debug/_deps/mini_tree_fc_tinyusb-subbuild/mini_tree_fc_tinyusb-populate-prefix/src"
  "C:/device_platform/CH32V307/Host-Device-Architecture-CH32V307-node/build/Debug/_deps/mini_tree_fc_tinyusb-subbuild/mini_tree_fc_tinyusb-populate-prefix/src/mini_tree_fc_tinyusb-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/device_platform/CH32V307/Host-Device-Architecture-CH32V307-node/build/Debug/_deps/mini_tree_fc_tinyusb-subbuild/mini_tree_fc_tinyusb-populate-prefix/src/mini_tree_fc_tinyusb-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/device_platform/CH32V307/Host-Device-Architecture-CH32V307-node/build/Debug/_deps/mini_tree_fc_tinyusb-subbuild/mini_tree_fc_tinyusb-populate-prefix/src/mini_tree_fc_tinyusb-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
