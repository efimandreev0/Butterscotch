
if ("${CMAKE_HOST_SYSTEM_NAME}" STREQUAL "MSYS")
	set(CATNIP_IS_MSYS TRUE)

	function(catnip_xlate_path outvar inpath)
		if(NOT IS_ABSOLUTE "${inpath}")
			# Pass through absolute paths
			set(out "${inpath}")
		elseif(IS_DIRECTORY "${inpath}")
			# Real directories can be translated directly
			string(MD5 hash "${inpath}")
			get_property(out GLOBAL PROPERTY CATNIP_XLATE_${hash})
			if(NOT out)
				execute_process(
					COMMAND cygpath -ma "${inpath}"
					OUTPUT_VARIABLE out
					OUTPUT_STRIP_TRAILING_WHITESPACE
					ERROR_QUIET
					COMMAND_ERROR_IS_FATAL ANY
				)
				set_property(GLOBAL PROPERTY CATNIP_XLATE_${hash} "${out}")
			endif()
		else()
			# Other paths can be decomposed into parent dir + child
			get_filename_component(indir "${inpath}" DIRECTORY)
			get_filename_component(infil "${inpath}" NAME)
			catnip_xlate_path(indir "${indir}")
			set(out "${indir}/${infil}")
		endif()

		set(${outvar} "${out}" PARENT_SCOPE)
	endfunction()

	function(catnip_xlate_args outvar inlist)
		set(out "")
		foreach(arg IN LISTS inlist)
			if("${arg}" MATCHES "^(-I|-iquote|-isystem)?(/.+)$")
				catnip_xlate_path(xlatedarg "${CMAKE_MATCH_2}")
				set(arg "${CMAKE_MATCH_1}${xlatedarg}")
			endif()
			list(APPEND out "${arg}")
		endforeach()

		set(${outvar} "${out}" PARENT_SCOPE)
	endfunction()
else()
	macro(catnip_xlate_path outvar inpath)
		set(${outvar} "${inpath}")
	endmacro()

	macro(catnip_xlate_args outvar inlist)
		set(${outvar} "${inlist}")
	endmacro()
endif()

function(catnip_str_to_json outvar str)
	string(REPLACE "\\" "\\\\" str "${str}")
	string(REPLACE "\"" "\\\"" str "${str}")
	set(${outvar} "\"${str}\"" PARENT_SCOPE)
endfunction()

function(catnip_list_to_json outvar inlist)
	set(out "[]")
	set(i "0")
	foreach(item IN LISTS inlist)
		catnip_str_to_json(item "${item}")
		string(JSON out SET "${out}" "${i}" "${item}")
		math(EXPR i "${i}+1")
	endforeach()

	set(${outvar} "${out}" PARENT_SCOPE)
endfunction()
