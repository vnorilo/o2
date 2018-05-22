cmake_minimum_required(VERSION 3.9.0)

project(o2)

set(BUILD_TESTS ON CACHE BOOL "Compile regression and other tests")

set(BUILD_TESTS_WITH_LIBLO OFF CACHE BOOL "Compile tests that use liblo,
requiring liblo library (only enabled if BUILD_TESTS is ON)")

set(BUILD_MIDI_EXAMPLE OFF CACHE BOOL "Compile midiclient & midiserver,
requiring portmidi library")

set(BUILD_CXX_WRAPPER ON CACHE BOOL "Build the C++ bindings into the library")

# O2 intentionally writes outside of declared array bounds (and
#  carefully insures that space is allocated beyond array bounds,
#  especially for message data, which is declared char[4], but can
#  be arbitrariily long -- well, at least up to O2_MAX_MSG_SIZE)
# Newer compilers will try to enforce char[4] in memcpy and strcpy
#  unless we turn off this behavior with the following macro definition:
add_definitions("-D_FORTIFY_SOURCE=0")

if(WIN32)
  add_definitions("-D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS -DIS_BIG_ENDIAN=0")
  set(EXTRA_LIBS winmm.lib ws2_32.lib Iphlpapi.lib)
endif(WIN32)

if(UNIX)
  if(APPLE)
    set(FRAMEWORK_PATH ${CMAKE_OSX_SYSROOT}/System/Library/Frameworks) 
    set(EXTRA_LIBS "${FRAMEWORK_PATH}/CoreAudio.framework") 
  else(APPLE) # must be Linux
    # set(EXTRA_LIBS "-lm") # needed by liblo
  endif(APPLE)
endif(UNIX)

#set(CMAKE_CXX_FLAGS "-stdlib=libc++")
#set(CMAKE_EXE_LINKER_FLAGS "-stdlib=libc++")

# o2
 
set(O2_SRC  
  src/o2_dynamic.c src/o2_dynamic.h
  src/o2.c src/o2.h src/o2_internal.h 
  src/o2_discovery.c src/o2_discovery.h
  src/o2_message.c src/o2_message.h 
  src/o2_sched.c src/o2_sched.h
  src/o2_search.c src/o2_search.h 
  src/o2_send.c src/o2_send.h 
  src/o2_socket.c src/o2_socket.h 
  src/o2_clock.c src/o2_clock.h
  # src/o2_debug.c src/o2_debug.h
  src/o2_interoperation.c src/o2_interoperation.h)

if (BUILD_CXX_WRAPPER)
	list(APPEND O2_SRC 
		src/o2_audio.cpp src/o2_audio.h
		src/o2xx.cpp src/o2xx.h)
    set(CMAKE_CXX_STANDARD 14)
endif()
 
add_library(o2_static STATIC ${O2_SRC})
target_link_libraries(o2_static ${EXTRA_LIBS})

#target_include_directories(o2_static PRIVATE ${no include directories})

#######################
# BUILD CONFIGURATION #
#######################

set(LIBRARIES o2_static CACHE INTERNAL "") 

if(BUILD_TESTS)
message(STATUS "Building test programs")

set(BUILD_STATIC_LIB TRUE CACHE BOOL "Build a static lib -- currently the only thing supported, turning OFF probably gives you a static library without a static runtime library option.")

include_directories(src)

# DEAL WITH WINDOWS OPTIONS: DEATH BY A THOUSAND CUTS
if(WIN32)
  if(USE_STATIC_LIBS)
    # release will use static runtime library
    foreach(flag_var CMAKE_CXX_FLAGS_RELEASE CMAKE_C_FLAGS_RELEASE
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_MINSIZEREL
        CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CFLAGS CMAKE_C_FLAGS_MINSIZEREL
        CMAKE_C_FLAGS_RELWITHDEBINFO)
      if(${flag_var} MATCHES "/MD")
        string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
      endif(${flag_var} MATCHES "/MD")
    endforeach(flag_var)
    message(STATUS 
      "Note: overriding CMAKE_*_FLAGS_* to use Visual C static multithread library")
    set(VERBOSE_WARNINGS 0 CACHE BOOL "Do not use _CRT_SECURE_NO_WARNINGS to disable suggestions to use 'secure' versions of strcpy, etc.")
  endif(USE_STATIC_LIBS)
endif(WIN32)


# EXECUTABLE
# Create executables 
set(SINGLE_C_FILE_TESTS 
	dispatchtest 
	typestest
	taptest 
	coercetest
	longtest
	arraytest
	o2client
	o2server
	statusclient
	statusserver
	tcpclient
	tcpserver
	clockslave
	clockmaster
	oscsendtest
	oscrecvtest
	oscanytest
	bundletest
	oscbndlsend
	oscbndlrecv
	infotest1
	infotest2)

set(SINGLE_CXX_FILE_TESTS 
	cxxtest
	audiotest)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)	

foreach(C_FILE ${SINGLE_C_FILE_TESTS})
	add_executable(${C_FILE} test/${C_FILE}.c)
	target_link_libraries(${C_FILE} ${LIBRARIES})
	set_target_properties(${C_FILE} PROPERTIES FOLDER tests)
endforeach()

if (BUILD_CXX_WRAPPER)
foreach(CXX_FILE ${SINGLE_CXX_FILE_TESTS})
	add_executable(${CXX_FILE} test/${CXX_FILE}.cpp)
	target_link_libraries(${CXX_FILE} ${LIBRARIES})
	set_target_properties(${CXX_FILE} PROPERTIES FOLDER tests)
endforeach()
endif()
endif()
 

if(UNIX)
  if(APPLE)
    # EXTRA_LO_LIBS is libraries needed by Liblo applications
    set(EXTRA_LO_LIBS ${COREAUDIO_LIB} ${COREFOUNDATION_LIB}
                      ${COREMIDI_LIB} ${CORESERVICES_LIB})
  else(APPLE)
    # linux
    set(CMAKE_C_FLAGS "-std=gnu99")
    set(PTHREAD_LIB pthread)
    # EXTRA_LO_LIBS is libraries needed by Liblo applications
    set(EXTRA_LO_LIBS asound ${PTHREAD_LIB} m)
  endif(APPLE)
  if(BUILD_MIDI_EXAMPLE)
    # Use PortMidi Library
    set(PORTMIDI_DBG_LIB PORTMIDI_DBG_LIB-NOTFOUND)
    set(PORTMIDI_OPT_LIB PORTMIDI_OPT_LIB-NOTFOUND)

    if(APPLE)
      set(COREMIDI_LIB "${FRAMEWORK_PATH}/CoreMIDI.framework")
    else(APPLE)
    endif(APPLE)

    set(PORTMIDI_BASE_PATH ../portmedia/portmidi 
        CACHE STRING "Where is portmidi?")
    set(PORTMIDI_PATH ${PORTMIDI_BASE_PATH}/pm_common CACHE INTERNAL 
                      "Where is portmidi.h?" FORCE)
    message(STATUS "PORTMIDI_BASE_PATH is " ${PORTMIDI_BASE_PATH})
  
    if(USE_STATIC_LIBS)
      if(UNIX)
        find_library(PORTMIDI_DBG_LIB portmidi_s ${PORTMIDI_BASE_PATH}
                     ${PORTMIDI_BASE_PATH}/Debug
                     ${PORTMIDI_PATH} ${PORTMIDI_PATH}/Debug)
      else(UNIX) # always use dll for windows debug
        find_library(PORTMIDI_DBG_LIB portmidi HINTS
                     ${PORTMIDI_BASE_PATH} ${PORTMIDI_BASE_PATH}/Debug
                     ${PORTMIDI_PATH} ${PORTMIDI_PATH}/Debug)
      endif(UNIX)

      message(STATUS "*** in USE_STATIC_LIBS, USE_MIDI ${USE_MIDI} PORTMIDI_DBG_LIB ${PORTMIDI_DBG_LIB}")

    else(USE_STATIC_LIBS)
      find_library(PORTMIDI_DBG_LIB portmidi HINTS
                   ${PORTMIDI_BASE_PATH} ${PORTMIDI_BASE_PATH}/Debug
	           ${PORTMIDI_BASE_PATH}/x64/Debug
                   ${PORTMIDI_PATH} ${PORTMIDI_PATH}/Debug
                   ${PORTMIDI_PATH}/x64/Debug)
    endif(USE_STATIC_LIBS)

    add_executable(midiclient test/midiclient.c test/cmtio.c test/cmtio.h) 
    target_include_directories(midiclient PRIVATE ${CMAKE_SOURCE_DIR}/src) 
    target_link_libraries(midiclient ${LIBRARIES})

    add_executable(midiserver test/midiserver.c)
    target_include_directories(midiserver PRIVATE ${CMAKE_SOURCE_DIR}/src
          ${PORTMIDI_PATH} ${PORTMIDI_PATH}/../porttime)
    target_link_libraries(midiserver ${LIBRARIES} ${PORTMIDI_DBG_LIB}) 
  endif(BUILD_MIDI_EXAMPLE) 
endif(UNIX)

message(STATUS LIBRARIES=${LIBRARIES}) 

# this is some networking test code, not part of O2
#add_executable(broadcastclient test/broadcastclient.c) 
#add_executable(broadcastserver test/broadcastserver.c) 
#add_executable(tcppollclient test/tcppollclient.c) 
#add_executable(tcppollserver test/tcppollserver.c) 

if(BUILD_TESTS)
if(BUILD_TESTS_WITH_LIBLO)
  set(LIBLO_PATH "${CMAKE_SOURCE_DIR}/../liblo-0.28" CACHE PATH
                 "Where to find liblo_64s.a, the liblo library.") 
  set(LIBLO_LIB LIBLO_LIB-NOTFOUND CACHE FILEPATH
             "The liblo library; should be set automatically if LIBLO_PATH is correct and liblo_s64.a exists.") 
  find_library(LIBLO_LIB lo_s64 ${LIBLO_PATH})
  # where to find liblo include files:
  set(LIBLO_INCLUDE_PATH ${LIBLO_PATH} CACHE PATH "where to find liblo include files") 

  message(STATUS "LIBLO_LIB is ${LIBLO_LIB}") 

  # Create executables 
  add_executable(lo_benchmk_server  test/lo_benchmk_server.c) 
  target_include_directories(lo_benchmk_server PRIVATE ${LIBLO_INCLUDE_PATH})
  target_link_libraries(lo_benchmk_server ${LIBLO_LIB} ${EXTRA_LO_LIBS}) 

  add_executable(lo_benchmk_client  test/lo_benchmk_client.c) 
  target_include_directories(lo_benchmk_client PRIVATE ${LIBLO_INCLUDE_PATH})  
  target_link_libraries(lo_benchmk_client ${LIBLO_LIB}  ${EXTRA_LO_LIBS})  

  add_executable(lo_oscrecv test/lo_oscrecv.c) 
  target_include_directories(lo_oscrecv PRIVATE ${LIBLO_INCLUDE_PATH})  
  target_link_libraries(lo_oscrecv ${LIBLO_LIB}  ${EXTRA_LO_LIBS})   

  add_executable(lo_oscsend test/lo_oscsend.c) 
  target_include_directories(lo_oscsend PRIVATE ${LIBLO_INCLUDE_PATH})  
  target_link_libraries(lo_oscsend ${LIBLO_LIB}  ${EXTRA_LO_LIBS})    

  add_executable(lo_bndlsend test/lo_bndlsend.c) 
  target_include_directories(lo_bndlsend PRIVATE ${LIBLO_INCLUDE_PATH})  
  target_link_libraries(lo_bndlsend ${LIBLO_LIB}  ${EXTRA_LO_LIBS})     

  add_executable(lo_bndlrecv test/lo_bndlrecv.c) 
  target_include_directories(lo_bndlrecv PRIVATE ${LIBLO_INCLUDE_PATH})  
  target_link_libraries(lo_bndlrecv ${LIBLO_LIB}  ${EXTRA_LO_LIBS})     
endif(BUILD_TESTS_WITH_LIBLO)
endif(BUILD_TESTS)

install( TARGETS o2_static EXPORT O2Targets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib 
  PUBLIC_HEADER DESTINATION include)
