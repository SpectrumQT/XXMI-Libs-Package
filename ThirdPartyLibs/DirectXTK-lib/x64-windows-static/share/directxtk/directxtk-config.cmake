
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was DirectXTK-config.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

include(${CMAKE_CURRENT_LIST_DIR}/DirectXTK-targets.cmake)
include(CMakeFindDependencyMacro)

set(BUILD_XAUDIO_WIN10 OFF)
set(BUILD_XAUDIO_WIN8 OFF)
set(BUILD_XAUDIO_WIN7 )
if (BUILD_XAUDIO_WIN7 AND (NOT BUILD_XAUDIO_WIN10) AND (NOT BUILD_XAUDIO_WIN8) AND (NOT WINDOWS_STORE))
    find_dependency(xaudio2redist CONFIG)
endif()

if(MINGW)
    find_dependency(directxmath)
else()
    find_package(directxmath CONFIG QUIET)
endif()

check_required_components("DirectXTK")
