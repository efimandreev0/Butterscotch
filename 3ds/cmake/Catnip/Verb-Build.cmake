
function(__catnip_generator var)
	get_property(generator GLOBAL PROPERTY CATNIP_GENERATOR)
	if(NOT generator)
		find_program(PROG_NINJA ninja)
		if(PROG_NINJA)
			set(generator "Ninja")
		else()
			set(generator "Unix Makefiles")
		endif()
		set_property(GLOBAL PROPERTY CATNIP_GENERATOR "${generator}")
	endif()

	set(${var} "${generator}" PARENT_SCOPE)
endfunction()

function(__catnip_fix_compiler_commands_json filename)
	# This fixing only applies to msys
	if (NOT CATNIP_IS_MSYS)
		return()
	endif()

	message(STATUS "Translating ${filename}")
	file(READ "${filename}" injson)

	string(JSON nument LENGTH "${injson}")
	if (nument EQUAL 0)
		return()
	endif()

	set(outjson "[]")
	math(EXPR loopmax "${nument}-1")
	foreach(i RANGE "${loopmax}")
		string(JSON curent GET "${injson}" "${i}")
		string(JSON curdir GET "${curent}" "directory")
		string(JSON curcmd GET "${curent}" "command")
		string(JSON curfil GET "${curent}" "file")
		string(JSON curout GET "${curent}" "output")

		separate_arguments(curcmd UNIX_COMMAND "${curcmd}")

		catnip_xlate_path(curdir "${curdir}")
		catnip_xlate_args(curcmd "${curcmd}")
		catnip_xlate_path(curfil "${curfil}")
		catnip_xlate_path(curout "${curout}")

		# XX: is there any way to do this properly? as in the inverse of separate_arguments
		list(JOIN curcmd " " curcmd)

		catnip_str_to_json(curdir "${curdir}")
		catnip_str_to_json(curcmd "${curcmd}")
		catnip_str_to_json(curfil "${curfil}")
		catnip_str_to_json(curout "${curout}")

		set(curent "{}")
		string(JSON curent  SET "${curent}"  "directory" "${curdir}")
		string(JSON curent  SET "${curent}"  "command"   "${curcmd}")
		string(JSON curent  SET "${curent}"  "file"      "${curfil}")
		string(JSON curent  SET "${curent}"  "output"    "${curout}")
		string(JSON outjson SET "${outjson}" "${i}"      "${curent}")
	endforeach()

	file(WRITE "${filename}" "${outjson}")
endfunction()

function(__catnip_gen_clangd pkgname preset)
	get_property(srcdir GLOBAL PROPERTY CATNIP_${pkgname}_SOURCE)
	set(builddir "${CATNIP_BUILD_DIR}/${pkgname}.${preset}")
	file(RELATIVE_PATH builddir_srcrel "${srcdir}" "${builddir}")

	set(clangd_cf "{}")
	catnip_str_to_json(cf_dbdir "${builddir_srcrel}")
	string(JSON clangd_cf SET "${clangd_cf}" "CompilationDatabase" "${cf_dbdir}")
	catnip_list_to_json(cf_remove "${CATNIP_CLANGD_FLAGS_REMOVE}")
	string(JSON clangd_cf SET "${clangd_cf}" "Remove" "${cf_remove}")

	set(clangd_config "{}")
	string(JSON clangd_config SET "${clangd_config}" "CompileFlags" "${clangd_cf}")

	file(WRITE "${srcdir}/.clangd" "${clangd_config}")
endfunction()

function(__catnip_build selector)
	string(FIND "${selector}" "." dotpos)
	string(SUBSTRING "${selector}" 0 ${dotpos} pkgname)
	math(EXPR dotpos "${dotpos}+1")
	string(SUBSTRING "${selector}" ${dotpos} -1 preset)
	set(scope CATNIP_${pkgname}__${preset})

	get_property(verb GLOBAL PROPERTY ${scope}_VERB)
	get_property(srcdir GLOBAL PROPERTY CATNIP_${pkgname}_SOURCE)
	get_property(stamp GLOBAL PROPERTY ${scope}_STAMP)
	set(builddir "${CATNIP_BUILD_DIR}/${selector}")
	set(stampfile "${builddir}/.catnip_stamp")
	set(jsonfile "${builddir}/compile_commands.json")

	if("${verb}" STREQUAL "config" AND CATNIP_FORCE_FLAG)
		file(REMOVE_RECURSE "${builddir}")
	endif()

	if(EXISTS "${stampfile}")
		file(READ "${stampfile}" existingstamp)
		if(NOT "${existingstamp}" STREQUAL "${stamp}")
			message(STATUS "Config for ${selector} changed, deleting stale build dir")
			file(REMOVE_RECURSE "${builddir}")
		endif()
	endif()

	file(MAKE_DIRECTORY "${builddir}")
	message(STATUS "Entering ${builddir}")

	file(TIMESTAMP "${jsonfile}" old_jsontime)

	if(NOT EXISTS "${stampfile}")
		get_property(cmakeargs GLOBAL PROPERTY ${scope}_CMAKE_ARGS)
		__catnip_generator(generator)

		if(NOT CATNIP_VERBOSE)
			set(silence OUTPUT_QUIET)
		endif()

		execute_process(
			COMMAND ${CMAKE_COMMAND} -E env
				"DKP_BUILD_TOOL_HOOK=dkp-catnip-utils"
				"CATNIP_BUILD_DIR=${CATNIP_BUILD_DIR}"
				"CATNIP_PRESET=${preset}"
				${CMAKE_COMMAND} ${cmakeargs}
				-D CMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE
				-G "${generator}"
				-S "${srcdir}"
				-B "${builddir}"
			RESULT_VARIABLE error
			${silence}
		)

		if(error)
			file(REMOVE_RECURSE "${builddir}")
			message(FATAL_ERROR "Failed to configure ${selector}")
		endif()

		file(WRITE "${stampfile}" "${stamp}")
	endif()

	if (NOT "${verb}" STREQUAL "config")
		set(buildargs --build "${builddir}")

		if(NOT "${verb}" STREQUAL "build")
			list(APPEND buildargs --target "${verb}")
		endif()

		if(CATNIP_FORCE_FLAG)
			list(APPEND buildargs --clean-first)
		endif()

		if(CATNIP_VERBOSE AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.14)
			list(APPEND buildargs --verbose)
		endif()

		if(CATNIP_PARALLEL_JOBS)
			list(APPEND buildargs -j${CATNIP_PARALLEL_JOBS})
		endif()

		execute_process(
			COMMAND ${CMAKE_COMMAND} ${buildargs}
			RESULT_VARIABLE error
		)
	endif()

	file(TIMESTAMP "${jsonfile}" new_jsontime)
	if(NOT "${old_jsontime}" STREQUAL "${new_jsontime}")
		__catnip_fix_compiler_commands_json("${jsonfile}")
	endif()

	if(CATNIP_GEN_CLANGD)
		get_property(already_done GLOBAL PROPERTY CATNIP_${pkgname}_GEN_CLANGD SET)
		if (NOT already_done)
			message(STATUS "Generating .clangd for ${selector}")
			set_property(GLOBAL PROPERTY CATNIP_${pkgname}_GEN_CLANGD TRUE)
			__catnip_gen_clangd("${pkgname}" "${preset}")
		endif()
	endif()

	if(error)
		message(FATAL_ERROR "Failed to build ${selector}")
	endif()
endfunction()

if(NOT DEFINED CATNIP_GEN_CLANGD AND DEFINED ENV{CATNIP_GEN_CLANGD})
	set(CATNIP_GEN_CLANGD "$ENV{CATNIP_GEN_CLANGD}")
endif()

if("${CATNIP_VERB}" STREQUAL "install" AND DEFINED ENV{DESTDIR})
	# Fixup DESTDIR so that it matches the correct CWD
	get_filename_component(DESTDIR "$ENV{DESTDIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
	set(ENV{DESTDIR} "${DESTDIR}")
endif()

__catnip_planner()
foreach(sel IN LISTS CATNIP_SELECTORS)
	__catnip_build("${sel}")
endforeach()
