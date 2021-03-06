cmake_minimum_required(VERSION 3.6)

set(lib_name openal)
set(target_name ext_${lib_name})

project(${lib_name})

include(../cmake/CocosExternalConfig.cmake)

add_library(${target_name} SHARED IMPORTED GLOBAL)

set_target_properties(${target_name} PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WINDOWS)
    set_target_properties(${target_name} PROPERTIES
       IMPORTED_LOCATION "${platform_spec_path}/OpenAL32.dll"
       IMPORTED_IMPLIB "${platform_spec_path}/OpenAL32.lib"
    )
elseif(APPLE)
    if(CC_USE_ALSOFT_ON_APPLE)
      set_target_properties(${target_name} PROPERTIES
        IMPORTED_LOCATION "${platform_spec_path}/lib${lib_name}.a"
      )
    endif()
elseif(ANDROID)
    set_target_properties(${target_name} PROPERTIES
      IMPORTED_LOCATION "${platform_spec_path}/lib${lib_name}.so"
    )
    set(OPENAL_SO_PATH "${platform_spec_path}/lib${lib_name}.so" CACHE INTERNAL "OpenAL Library location" )
    set(OPENAL_SO_NAME "lib${lib_name}.so" CACHE INTERNAL "OpenAL Library name" )
endif()
