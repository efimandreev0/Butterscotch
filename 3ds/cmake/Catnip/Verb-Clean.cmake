
function(__catnip_clean selector)
	string(FIND "${selector}" "." dotpos)
	string(SUBSTRING "${selector}" 0 ${dotpos} pkgname)
	math(EXPR dotpos "${dotpos}+1")
	string(SUBSTRING "${selector}" ${dotpos} -1 preset)
	set(scope CATNIP_${pkgname}__${preset})

	get_property(verb GLOBAL PROPERTY ${scope}_VERB)
	get_property(srcdir GLOBAL PROPERTY CATNIP_${pkgname}_SOURCE)
	set(builddir "${CATNIP_BUILD_DIR}/${selector}")
	set(stampfile "${builddir}/.catnip_stamp")

	if(NOT IS_DIRECTORY "${builddir}")
		message(STATUS "${selector}: already clean")
		return()
	endif()

	if(CATNIP_FORCE_FLAG)
		file(REMOVE "${stampfile}")
	endif()

	message(STATUS "Cleaning ${selector}...")
	if(EXISTS "${stampfile}")
		execute_process(COMMAND ${CMAKE_COMMAND} --build "${builddir}" --target clean)
	else()
		file(REMOVE_RECURSE "${builddir}")
	endif()
endfunction()

__catnip_planner()
foreach(sel IN LISTS CATNIP_SELECTORS)
	__catnip_clean("${sel}")
endforeach()
