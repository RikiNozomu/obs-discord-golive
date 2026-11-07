cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  # Qt's FindWrapOpenGL links the AGL framework unconditionally, but the macOS 27 SDK no longer
  # ships it ("ld: framework 'AGL' not found"). The plugin doesn't use AGL, so drop it.
  if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_wrap_gl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    list(FILTER _wrap_gl_libs EXCLUDE REGEX "AGL")
    set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_wrap_gl_libs}")
    unset(_wrap_gl_libs)
  endif()
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/discord-ids.h
    src/discord-service.c
    src/discord-output.c
    src/helper-process.c
    src/helper-process.h
    src/discord-dock.hpp
    src/discord-dock.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# Ship the Node helper inside the plugin, so users never point at it themselves
# (GUIDELINE.md §5.5). Until M5 builds a single executable, the sources and
# node_modules are copied and the plugin runs helper/src/index.ts with Node.
# Release builds point HELPER_NODE_MODULES at a staged, production-only copy
# with native modules for every target architecture (scripts/release-macos.sh).
set(_helper_src "${CMAKE_CURRENT_SOURCE_DIR}/helper")
set(HELPER_NODE_MODULES "${_helper_src}/node_modules" CACHE PATH "node_modules shipped with the helper")
set(_helper_parts "${_helper_src}/src" "${_helper_src}/package.json")
if(EXISTS "${HELPER_NODE_MODULES}")
  list(APPEND _helper_parts "${HELPER_NODE_MODULES}")
else()
  message(WARNING "helper/node_modules is missing: run 'npm ci' in helper/, or the plugin cannot reach Discord.")
endif()

if(APPLE)
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_NAME}>/Resources/helper"
    COMMAND
      rsync -a --delete --exclude .DS_Store ${_helper_parts}
      "$<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_NAME}>/Resources/helper/"
    COMMENT "Copy the helper into the plugin bundle"
    VERBATIM
  )
else()
  if(OS_WINDOWS)
    set(_helper_dest "${CMAKE_PROJECT_NAME}/data/helper")
  else()
    set(_helper_dest "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${CMAKE_PROJECT_NAME}/helper")
  endif()
  list(REMOVE_ITEM _helper_parts "${_helper_src}/package.json")
  install(DIRECTORY ${_helper_parts} DESTINATION "${_helper_dest}" USE_SOURCE_PERMISSIONS)
  install(FILES "${_helper_src}/package.json" DESTINATION "${_helper_dest}")
endif()
