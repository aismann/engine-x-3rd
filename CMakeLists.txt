cmake_minimum_required(VERSION 3.6)
project(external)

option(BUILD_EXT_BOX2D "Build with internal Box2D support" ON)
option(BUILD_EXT_CHIPMUNK "Build with internal chipmunk support" ON)
option(BUILD_EXT_FREETYPE2 "Build with internal freetype2 support" ON)
option(BUILD_EXT_RECAST "Build with internal recast support" ON)
option(BUILD_EXT_BULLET "Build with internal bullet support" ON)
option(BUILD_EXT_JPEG "Build with internal jpeg support" ON)
option(BUILD_EXT_OPENSSL "Build with internal openssl support" ON)
option(BUILD_EXT_WEBP "Build with internal webp support" ON)
option(BUILD_EXT_PUGIXML "Build with internal pugixml support" ON)
option(BUILD_EXT_XXHASH "Build with internal xxhash support" ON)
option(BUILD_EXT_CLIPPER "Build with internal clipper support" ON)
option(BUILD_EXT_EDTAA3FUNC "Build with internal edtaa3func support" ON)
option(BUILD_EXT_CONVERTUTF "Build with internal ConvertUTF support" ON)
option(BUILD_EXT_POLY2TRI "Build with internal poly2tri support" ON)
option(BUILD_EXT_MD5 "Build with internal md5 support" ON)
option(BUILD_EXT_ZLIB "Build with internal zlib support" ON)
option(BUILD_EXT_CURL "Build with internal curl support" ON)
option(BUILD_EXT_UNZIP "Build with internal unzip support" ON)
option(BUILD_EXT_SPINE_CPP "Build with internal spine cpp runtime" ON)
option(BUILD_EXT_ASTC "Build with internal ASTC support" ON)
option(BUILD_EXT_CARES "Build with internal c-ares support" OFF)
option(BUILD_EXT_KCP "Build with internal kcp support" OFF)

if(NOT BUILD_EXT_KCP)
    add_library(external empty.cpp)
else()
    add_library(external kcp/kcp.c)
endif()

if(BUILD_EXT_BOX2D)
    add_subdirectory(Box2D)
    target_link_libraries(external ext_box2d)
endif(BUILD_EXT_BOX2D)
if(BUILD_EXT_CHIPMUNK)
    set(CP_BUILD_SHARED OFF CACHE BOOL "Build chipmunk as shared library" FORCE)
    set(CP_BUILD_STATIC ON CACHE BOOL "Build chipmunk as static library" FORCE)
    set(CP_BUILD_DEMOS OFF CACHE BOOL "Build chipmunk demos" FORCE)
    set(CP_INSTALL_STATIC OFF CACHE BOOL "Install chipmunk static" FORCE)
    add_subdirectory(chipmunk)
    set_target_properties(chipmunk_static PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/chipmunk/include"
    )
    
    # !important engine-x not use double precision
    target_compile_definitions(chipmunk_static PUBLIC CP_USE_CGTYPES=0)
    target_compile_definitions(chipmunk_static PUBLIC CP_USE_DOUBLES=0)

    #~ include_directories("${CMAKE_CURRENT_SOURCE_DIR}/chipmunk/include")
    target_link_libraries(external chipmunk_static)
endif(BUILD_EXT_CHIPMUNK)
if(BUILD_EXT_FREETYPE2)
    add_subdirectory(freetype2)
    target_link_libraries(external ext_freetype)
endif(BUILD_EXT_FREETYPE2)
if(BUILD_EXT_RECAST)
    add_subdirectory(recast)
    target_link_libraries(external ext_recast)
endif(BUILD_EXT_RECAST)
if(BUILD_EXT_BULLET)
    add_subdirectory(bullet)
    target_link_libraries(external ext_bullet)
endif(BUILD_EXT_BULLET)
if(BUILD_EXT_JPEG)
    add_subdirectory(jpeg)
    target_link_libraries(external ext_jpeg)
endif(BUILD_EXT_JPEG)
if(BUILD_EXT_OPENSSL)
    add_subdirectory(openssl)
    if(ANDROID)
        target_link_libraries(ext_ssl INTERFACE ext_crypto)
        target_link_libraries(external ext_ssl)
    else()
        target_link_libraries(external ext_crypto ext_ssl)
    endif()
endif(BUILD_EXT_OPENSSL)
if(BUILD_EXT_WEBP)
    add_subdirectory(webp)
    target_link_libraries(external ext_webp)
endif(BUILD_EXT_WEBP)
if(BUILD_EXT_PUGIXML)
    add_subdirectory(pugixml)
    target_link_libraries(external ext_pugixml)
endif(BUILD_EXT_PUGIXML)
if(BUILD_EXT_XXHASH)
    add_subdirectory(xxhash)
    target_link_libraries(external ext_xxhash)
endif(BUILD_EXT_XXHASH)
if(BUILD_EXT_CLIPPER)
    add_subdirectory(clipper)
    target_link_libraries(external ext_clipper)
endif(BUILD_EXT_CLIPPER)
if(BUILD_EXT_EDTAA3FUNC)
    add_subdirectory(edtaa3func)
    target_link_libraries(external ext_edtaa3func)
endif(BUILD_EXT_EDTAA3FUNC)
if(BUILD_EXT_CONVERTUTF)
    add_subdirectory(ConvertUTF)
    target_link_libraries(external ext_convertUTF)
endif(BUILD_EXT_CONVERTUTF)
if(BUILD_EXT_POLY2TRI)
    add_subdirectory(poly2tri)
    target_link_libraries(external ext_poly2tri)
endif(BUILD_EXT_POLY2TRI)

if(BUILD_EXT_MD5)
    add_subdirectory(md5)
    target_link_libraries(external ext_md5)
endif(BUILD_EXT_MD5)

if(BUILD_EXT_ASTC)
    add_subdirectory(astc)
    target_link_libraries(external ext_astc)
endif(BUILD_EXT_ASTC)

# use lua/js specific libs by property to prevent conflict
if(BUILD_LUA_LIBS)
    add_subdirectory(lua/luajit)
    add_subdirectory(lua/tolua)
    add_subdirectory(lua/luasocket)
    set_property(TARGET external APPEND PROPERTY 
        CC_LUA_DEPEND ext_luajit ext_tolua ext_luasocket
    )
endif()

if(NOT LINUX)
    if(BUILD_EXT_CURL)
        add_subdirectory(curl)
        if(ANDROID)
            target_link_libraries(ext_curl INTERFACE ext_ssl)
        endif()
        target_link_libraries(external ext_curl)
    endif(BUILD_EXT_CURL)
    add_subdirectory(png)
    target_link_libraries(external 
        ext_png
    )
endif(NOT LINUX)

if(ANDROID)
    add_subdirectory(android-specific/cpufeatures)
    target_link_libraries(external 
        ext_cpufeatures 
    )
endif()

# OpenAL streaming play family: ogg
add_subdirectory(ogg)
if(WINDOWS)
    target_link_libraries(external
        ext_vorbis
        ext_vorbisfile
    )
else()
    if(ANDROID)
        target_link_libraries(ext_vorbis INTERFACE ext_ogg)
        target_link_libraries(ext_vorbisfile INTERFACE ext_vorbis)
        target_link_libraries(external ext_vorbisfile)
    else()
        target_link_libraries(external 
            ext_ogg
            ext_vorbis
            ext_vorbisfile
        )
    endif()
endif()

# OpenAL streaming play family: openal & mpg123
if(WINDOWS OR ANDROID OR LINUX)
    add_subdirectory(openal)
    add_subdirectory(mpg123)
    target_link_libraries(external 
        ext_openal
        ext_mpg123
    )
elseif(APPLE AND CC_USE_ALSOFT_ON_APPLE) 
    add_subdirectory(openal)
    target_link_libraries(external
        ext_openal
    )
endif()

# TODO: linux
if(WINDOWS)
    add_subdirectory(glad)
    add_subdirectory(win32-specific/iconv)
    target_link_libraries(external 
        ext_glad
        ext_iconv
    )
endif()

if(WINDOWS OR MACOSX OR LINUX)
    add_subdirectory(glfw3)
    target_link_libraries(external 
        ext_glfw3
    )
endif()

if(MACOSX OR ANDROID OR WINDOWS)
    if(BUILD_EXT_ZLIB)
        add_subdirectory(zlib)
        target_link_libraries(external 
            ext_zlib
        )
    endif(BUILD_EXT_ZLIB)
endif()

# unzip depend on zlib
if(BUILD_EXT_UNZIP)
    add_subdirectory(unzip)
    target_link_libraries(external ext_unzip)
endif(BUILD_EXT_UNZIP)

# since 3.6/3.7 c runtime support build as DLL
# since 3.8 cpp runtime is preferred for cocos2d-x
if(BUILD_EXT_SPINE_CPP)
    add_subdirectory(spine-cpp)
    target_link_libraries(external spine-cpp)
    set_target_properties(spine-cpp
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    FOLDER "External"
)

endif(BUILD_EXT_SPINE_CPP)

if(BUILD_EXT_CARES)
    set(CARES_STATIC ON CACHE BOOL "Build c-ares as static library" FORCE)
    set(CARES_SHARED OFF CACHE BOOL "Build c-ares as shared library" FORCE)
    set(CARES_BUILD_TOOLS OFF CACHE BOOL "Build c-ares tools" FORCE)
    set(CARES_INSTALL OFF CACHE BOOL "Create installation targets (chain builders may want to disable this)" FORCE)
    add_subdirectory(c-ares)
    target_link_libraries(external c-ares)
    include_directories($<TARGET_FILE_DIR:c-ares>)
endif(BUILD_EXT_CARES)

# put "external" into External folder, too
set_target_properties(external
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    FOLDER "External"
)

if(MACOSX OR IOS)
    add_subdirectory(glsl-optimizer)
    target_link_libraries(external 
    ext_glcpp_library
    ext_glsl_optimizer
    ext_libmesa
    )
endif()
