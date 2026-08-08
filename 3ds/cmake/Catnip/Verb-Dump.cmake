if(NOT "${CATNIP_VERB}" STREQUAL "dump-info")
	message(FATAL_ERROR "--${CATNIP_VERB} not implemented")
endif()

function(__catnip_dump_package outvar pkgname)
	set(scope CATNIP_${pkgname})
	get_property(srcdir GLOBAL PROPERTY ${scope}_SOURCE)
	get_property(prlist GLOBAL PROPERTY ${scope}_PRESETS)
	get_property(default GLOBAL PROPERTY ${scope}_DEFAULT)

	set(out "{}")
	catnip_str_to_json(p_name "${pkgname}")
	string(JSON out SET "${out}" "name" "${p_name}")
	catnip_xlate_path(p_srcdir "${srcdir}")
	catnip_str_to_json(p_srcdir "${p_srcdir}")
	string(JSON out SET "${out}" "source" "${p_srcdir}")
	catnip_list_to_json(p_def "${default}")
	string(JSON out SET "${out}" "default" "${p_def}")

	set(j "0")
	set(p_presets "[]")
	foreach(preset IN LISTS prlist)
		set(scope CATNIP_${pkgname}__${preset})
		get_property(args GLOBAL PROPERTY ${scope}_CMAKE_ARGS)
		get_property(deps GLOBAL PROPERTY ${scope}_DEPENDS)

		set(pr_dump "{}")
		catnip_str_to_json(pr_name "${preset}")
		string(JSON pr_dump SET "${pr_dump}" "name" "${pr_name}")
		catnip_list_to_json(pr_args "${args}")
		string(JSON pr_dump SET "${pr_dump}" "cmake_args" "${pr_args}")
		catnip_list_to_json(pr_deps "${deps}")
		string(JSON pr_dump SET "${pr_dump}" "depends" "${pr_deps}")

		string(JSON p_presets SET "${p_presets}" "${j}" "${pr_dump}")
		math(EXPR j "${j}+1")
	endforeach()

	string(JSON out SET "${out}" "presets" "${p_presets}")

	set(${outvar} "${out}" PARENT_SCOPE)
endfunction()

set(outjson "[]")
set(i "0")
get_property(pkglist GLOBAL PROPERTY CATNIP_PACKAGES)
foreach(pkgname IN LISTS pkglist)
	__catnip_dump_package(pkgdump "${pkgname}")

	string(JSON outjson SET "${outjson}" "${i}" "${pkgdump}")
	math(EXPR i "${i}+1")
endforeach()

message(STATUS "catnip dump begin --\n${outjson}")
