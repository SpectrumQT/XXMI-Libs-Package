
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was DirectXTex-config.cmake.in                            ########

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

include(${CMAKE_CURRENT_LIST_DIR}/DirectXTex-targets.cmake)
include(CMakeFindDependencyMacro)

set(BC_USE_OPENMP ON)
if(BC_USE_OPENMP)
    find_dependency(OpenMP)
endif()

set(ENABLE_OPENEXR_SUPPORT OFF)
if(ENABLE_OPENEXR_SUPPORT)
    find_dependency(OpenEXR)
endif()

set(ENABLE_LIBJPEG_SUPPORT OFF)
if(ENABLE_LIBJPEG_SUPPORT)
    find_dependency(JPEG)
endif()

set(ENABLE_LIBPNG_SUPPORT OFF)
if(ENABLE_LIBPNG_SUPPORT)
    find_dependency(PNG)
endif()

if(MINGW OR (NOT WIN32))
    find_dependency(directx-headers)
    find_dependency(directxmath)
else()
    find_package(directx-headers CONFIG QUIET)
    find_package(directxmath CONFIG QUIET)
endif()

check_required_components("DirectXTex")
