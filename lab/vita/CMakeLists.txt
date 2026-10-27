cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()


project(vitacompanion)
enable_language(ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITACOMPANION_FTP_PORT 1337 CACHE STRING "FTP control port")
set(VITACOMPANION_CMD_PORT 1338 CACHE STRING "Command server port")
set(VITACOMPANION_MODULE_NAME "vitacompanion" CACHE STRING "Vita module name")

if(NOT VITACOMPANION_FTP_PORT MATCHES "^[0-9]+$" OR
   VITACOMPANION_FTP_PORT LESS 1 OR VITACOMPANION_FTP_PORT GREATER 65535)
  message(FATAL_ERROR "VITACOMPANION_FTP_PORT must be between 1 and 65535")
endif()
if(NOT VITACOMPANION_CMD_PORT MATCHES "^[0-9]+$" OR
   VITACOMPANION_CMD_PORT LESS 1 OR VITACOMPANION_CMD_PORT GREATER 65535)
  message(FATAL_ERROR "VITACOMPANION_CMD_PORT must be between 1 and 65535")
endif()
if(NOT VITACOMPANION_MODULE_NAME MATCHES "^[A-Za-z0-9_]+$")
  message(FATAL_ERROR "VITACOMPANION_MODULE_NAME must contain only letters, digits, and underscores")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -fno-pic -fno-PIC -std=gnu99")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fno-rtti -fno-exceptions")

include_directories(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/vendor/libftpvita
)

link_directories(
  ${CMAKE_CURRENT_BINARY_DIR}
)

add_library(ftpvita STATIC
        vendor/libftpvita/ftpvita.c
        vendor/libftpvita/ftpvita.h
        vendor/libftpvita/ftpvita_io.c
        vendor/libftpvita/ftpvita_io.h
        vendor/libftpvita/ftpvita_path.c
        vendor/libftpvita/ftpvita_path.h
        vendor/libftpvita/ftpvita_protocol.c
        vendor/libftpvita/ftpvita_protocol.h)

add_executable(vitacompanion_kernel
        kernel/main.c
        kernel/reboot.c
        kernel/reboot.h
        kernel/threadmgr_event_flag_list.S
        kernel/touch_patch.c
        kernel/lab.c)

target_compile_definitions(vitacompanion_kernel PRIVATE
  __VITA_KERNEL__
)

target_link_libraries(vitacompanion_kernel
  taihenForKernel_stub
  SceCtrlForDriver_stub
  SceKernelSuspendForDriver_stub
  SceSysmemForDriver_stub
  SceSysconForDriver_stub
  SceThreadmgrForDriver_stub
  SceDisplayForDriver_stub
  ScePowerForDriver_stub
  SceSysclibForDriver_stub
  gcc
)

set_target_properties(vitacompanion_kernel
  PROPERTIES LINK_FLAGS "-nostdlib"
)

vita_create_self(vitacompanion_kernel.skprx vitacompanion_kernel
  CONFIG ${CMAKE_SOURCE_DIR}/kernel/vitacompanion_kernel.yml
)
vita_create_stubs(vitacompanion_kernel_stubs vitacompanion_kernel
  ${CMAKE_SOURCE_DIR}/kernel/vitacompanion_kernel.yml KERNEL
)

add_executable(vitacompanion
        src/main.c
        src/main.h
        src/log.c
        src/log.h
        src/cmd.c
        src/cmd.h
        src/net.c
        src/net.h
        src/nosleep.c
        src/nosleep.h
        src/parser.c
        src/parser.h
        src/promote.c
        src/promote.h
        src/promote_head_template.c
        src/promote_head_template.h
        src/reboot.c
        src/reboot.h
        src/sha1.c
        src/sha1.h
        src/input.c
        src/input.h
        src/input_parse.c
        src/version.c
        src/version.h
        src/cmd_definitions.c
        src/cmd_definitions.h
        src/lab.c
        src/lab.h)

target_compile_definitions(ftpvita PRIVATE
  FTP_PORT=${VITACOMPANION_FTP_PORT}
)
target_compile_definitions(vitacompanion PRIVATE
  CMD_PORT=${VITACOMPANION_CMD_PORT}
)

add_dependencies(vitacompanion vitacompanion_kernel_stubs)

target_link_libraries(vitacompanion
  taihen_stub
  taipool
  ftpvita
  k
  gcc
  SceLibKernel_stub_weak
  SceNet_stub_weak
  SceNetCtl_stub_weak
  SceSysmodule_stub
  SceKernelThreadMgr_stub_weak
  SceIofilemgr_stub_weak
  SceRtc_stub_weak
  SceAppMgr_stub
  SceKernelModulemgr_stub
  SceProcessmgr_stub_weak
  ScePower_stub
  SceSysmem_stub
  ${CMAKE_CURRENT_BINARY_DIR}/libvitacompanion_kernel_stub.a
)

set_target_properties(vitacompanion
  PROPERTIES LINK_FLAGS "-nostdlib"
)

file(READ "${CMAKE_SOURCE_DIR}/vitacompanion.yml" VITACOMPANION_EXPORTS)
string(REGEX REPLACE "^vitacompanion:"
  "${VITACOMPANION_MODULE_NAME}:"
  VITACOMPANION_EXPORTS "${VITACOMPANION_EXPORTS}")
set(VITACOMPANION_EXPORTS_FILE
  "${CMAKE_CURRENT_BINARY_DIR}/${VITACOMPANION_MODULE_NAME}.yml")
file(WRITE "${VITACOMPANION_EXPORTS_FILE}" "${VITACOMPANION_EXPORTS}")

vita_create_self(${VITACOMPANION_MODULE_NAME}.suprx vitacompanion
  UNSAFE
  CONFIG ${VITACOMPANION_EXPORTS_FILE}
)

# ---- Vita Lab: the installer app, packaged with both plugins ----
add_executable(vitalab_app app/main.c app/config.c)
target_link_libraries(vitalab_app
  vita2d
  SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub SceTouch_stub
  ScePgf_stub ScePvf_stub SceCommonDialog_stub SceAppUtil_stub
  SceNet_stub SceNetCtl_stub ScePower_stub SceAppMgr_stub
  png z m c
)
# vita_create_self is a macro: the plugins' "-e <exports.yml>" would leak into the app
set(VITA_ELF_CREATE_FLAGS "")
set(VITA_MAKE_FSELF_FLAGS "")
vita_create_self(eboot.bin vitalab_app UNSAFE)
add_dependencies(eboot.bin-self ${VITACOMPANION_MODULE_NAME}.suprx-self vitacompanion_kernel.skprx-self)
vita_create_vpk(VitaLab.vpk VLAB00001 eboot.bin
  VERSION 01.00
  NAME "Vita Lab"
  FILE ${CMAKE_CURRENT_BINARY_DIR}/${VITACOMPANION_MODULE_NAME}.suprx plugins/vitacompanion.suprx
  FILE ${CMAKE_CURRENT_BINARY_DIR}/vitacompanion_kernel.skprx plugins/vitacompanion_kernel.skprx
  FILE ${CMAKE_SOURCE_DIR}/app/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/app/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_SOURCE_DIR}/app/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_SOURCE_DIR}/app/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE ${CMAKE_SOURCE_DIR}/LICENSE LICENSE.txt
  FILE ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md THIRD_PARTY_NOTICES.md
)
