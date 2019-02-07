# Check which init.d script to install
find_file(RPM_FNC functions PATHS /etc/rc.d/init.d)
find_file(DEB_FNC init-functions PATHS /lib/lsb)
find_file(SLES_FNC SuSE-release PATHS /etc)

if(EXISTS ${RPM_FNC})
  set(USE_RPM TRUE CACHE BOOL "If init.d script uses /lib/lsb/init-functions instead of /etc/rc.d/init.d/functions.")
elseif(EXISTS ${SLES_FNC})
  set(USE_SLES TRUE CACHE BOOL "SLES11 has reduced /lib/lsb/init-functions and needs a special init-script")
elseif(EXISTS ${DEB_FNC})
  set(USE_DEB TRUE CACHE BOOL "If init.d script uses /lib/lsb/init-functions instead of /etc/rc.d/init.d/functions.")
else()
  message(STATUS "Cannot find required init-functions in /lib/lsb/ or /etc/rc.d/init.d/, will not install init scripts.")
endif()

if(USE_DEB)
  configure_file(${CMAKE_SOURCE_DIR}/etc/ubuntu/init.d/maxscale.in ${CMAKE_BINARY_DIR}/maxscale @ONLY)
elseif(USE_RPM)
  configure_file(${CMAKE_SOURCE_DIR}/etc/init.d/maxscale.in ${CMAKE_BINARY_DIR}/maxscale @ONLY)
elseif(USE_SLES)
  configure_file(${CMAKE_SOURCE_DIR}/etc/sles11/init.d/maxscale.in ${CMAKE_BINARY_DIR}/maxscale @ONLY)
else()
  # Use a dummy file that tells the user that init scripts aren't supported on this platform
  configure_file(${CMAKE_SOURCE_DIR}/etc/fallback/maxscale.in ${CMAKE_BINARY_DIR}/maxscale @ONLY)
endif()

configure_file(${CMAKE_SOURCE_DIR}/etc/maxscale.conf.in ${CMAKE_BINARY_DIR}/maxscale.conf @ONLY)

# The systemd service file
if (CMAKE_BUILD_TYPE MATCHES "(D|d)(E|e)(B|b)(U|u)(G|g)")
  # Options enabled only in debug builds (a literal multi-line string)
  set(SERVICE_FILE_DEBUG_OPTIONS
    "LimitCORE=infinity
ExecStartPost=/bin/sh -c 'prlimit -p $(pidof maxscale) --core=unlimited'")
endif()

configure_file(${CMAKE_SOURCE_DIR}/etc/maxscale.service.in ${CMAKE_BINARY_DIR}/maxscale.service @ONLY)

if(PACKAGE)
  message(STATUS "maxscale.conf will unpack to: /etc/ld.so.conf.d")
  message(STATUS "startup scripts will unpack to to: /etc/init.d")
  message(STATUS "systemd service files will unpack to to: /usr/lib/systemd/system")
  message(STATUS "upstart files will unpack to: /etc/init/")
  install_file(${CMAKE_BINARY_DIR}/maxscale core)
  install_file(${CMAKE_BINARY_DIR}/maxscale.conf core)
  install_file(${CMAKE_BINARY_DIR}/maxscale.service core)
  install_file(${CMAKE_BINARY_DIR}/upstart/maxscale.conf core)
else()
  install(PROGRAMS ${CMAKE_BINARY_DIR}/maxscale DESTINATION /etc/init.d COMPONENT core)
  install(FILES ${CMAKE_BINARY_DIR}/maxscale.conf  DESTINATION /etc/ld.so.conf.d COMPONENT core)
  install(FILES ${CMAKE_BINARY_DIR}/maxscale.service  DESTINATION /usr/lib/systemd/system COMPONENT core)
  install(FILES ${CMAKE_BINARY_DIR}/upstart/maxscale.conf DESTINATION /etc/init/)
  message(STATUS "Installing maxscale.conf to: /etc/ld.so.conf.d")
  message(STATUS "Installing startup scripts to: /etc/init.d")
  message(STATUS "Installing systemd service files to: /usr/lib/systemd/system")
  message(STATUS "Installing upstart files to: /etc/init/")
endif()
