## our modules are without the "lib" prefix

## refactor me
MACRO(XCONFIG _package _include_DIR _link_DIR _link_FLAGS _link_r_FLAGS _cflags)
# reset the variables at the beginning
  SET(${_include_DIR})
  SET(${_link_DIR})
  SET(${_link_FLAGS})
  SET(${_link_r_FLAGS})
  SET(${_cflags})

  FIND_PROGRAM(${_package}CONFIG_EXECUTABLE NAMES ${_package} PATHS /usr/local/bin )

  # if pkg-config has been found
  IF(${_package}CONFIG_EXECUTABLE)
    SET(XCONFIG_EXECUTABLE "${${_package}CONFIG_EXECUTABLE}")
    MESSAGE(STATUS "found ${_package}: ${XCONFIG_EXECUTABLE}")

    EXEC_PROGRAM(${XCONFIG_EXECUTABLE} ARGS --libs OUTPUT_VARIABLE __link_FLAGS)
    STRING(REPLACE "\n" "" ${_link_FLAGS} ${__link_FLAGS})
    EXEC_PROGRAM(${XCONFIG_EXECUTABLE} ARGS --libs_r OUTPUT_VARIABLE __link_r_FLAGS)
    STRING(REPLACE "\n" "" ${_link_r_FLAGS} ${__link_r_FLAGS})
    EXEC_PROGRAM(${XCONFIG_EXECUTABLE} ARGS --cflags OUTPUT_VARIABLE __cflags)
    STRING(REPLACE "\n" "" ${_cflags} ${__cflags})

  ELSE(${_package}CONFIG_EXECUTABLE)
    MESSAGE(STATUS "found ${_package}: no")
  ENDIF(${_package}CONFIG_EXECUTABLE)
ENDMACRO(XCONFIG _package _include_DIR _link_DIR _link_FLAGS _cflags)

MACRO(ADD_AND_INSTALL_LIBRARY LIBNAME SRCFILES)
	IF(BUILD_STATIC)
		ADD_LIBRARY(${LIBNAME} STATIC ${SRCFILES})
		TARGET_LINK_LIBRARIES(lighttpd2 ${LIBNAME})
	ELSE(BUILD_STATIC)
		ADD_LIBRARY(${LIBNAME} MODULE ${SRCFILES})
		SET(L_INSTALL_TARGETS ${L_INSTALL_TARGETS} ${LIBNAME})

		ADD_TARGET_PROPERTIES(${LIBNAME} LINK_FLAGS ${COMMON_LDFLAGS})
		ADD_TARGET_PROPERTIES(${LIBNAME} COMPILE_FLAGS ${COMMON_CFLAGS})
		SET_TARGET_PROPERTIES(${LIBNAME} PROPERTIES CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

		TARGET_LINK_LIBRARIES(${LIBNAME} lighttpd-${PACKAGE_VERSION}-common lighttpd-${PACKAGE_VERSION}-shared)

		IF(APPLE)
			SET_TARGET_PROPERTIES(${LIBNAME} PROPERTIES LINK_FLAGS "-flat_namespace -undefined suppress")
		ENDIF(APPLE)
	ENDIF(BUILD_STATIC)
ENDMACRO(ADD_AND_INSTALL_LIBRARY)

MACRO(ADD_TARGET_PROPERTIES _target _name)
	SET(_properties)
	FOREACH(_prop ${ARGN})
		SET(_properties "${_properties} ${_prop}")
	ENDFOREACH(_prop)
	GET_TARGET_PROPERTY(_old_properties ${_target} ${_name})
	MESSAGE(STATUS "adding property to ${_target} ${_name}:" ${_properties})
	IF(NOT _old_properties)
		# in case it's NOTFOUND
		SET(_old_properties)
	ENDIF(NOT _old_properties)
	SET_TARGET_PROPERTIES(${_target} PROPERTIES ${_name} "${_old_properties} ${_properties}")
ENDMACRO(ADD_TARGET_PROPERTIES)

MACRO(ADD_PREFIX _target _prefix)
	SET(_oldtarget ${${_target}})
	SET(_newtarget)
	FOREACH(_t ${_oldtarget})
		SET(_newtarget ${_newtarget} "${_prefix}${_t}")
	ENDFOREACH(_t)
	SET(${_target} ${_newtarget})
ENDMACRO(ADD_PREFIX)
