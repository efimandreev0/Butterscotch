
set(CATNIP_VERBS_WITH_DEPS config build install)

function(__catnip_validate_deplist pkgname preset propname)
	get_property(inlist GLOBAL PROPERTY CATNIP_${pkgname}__${preset}_${propname})
	set(outlist "")

	foreach(dep IN LISTS inlist)
		string(FIND "${dep}" "." dotpos)
		if(dotpos GREATER_EQUAL 0)
			string(SUBSTRING "${dep}" 0 ${dotpos} deppkg)
			math(EXPR dotpos "${dotpos}+1")
			string(SUBSTRING "${dep}" ${dotpos} -1 deppreset)
		else()
			set(deppkg "${dep}")
			set(deppreset "${preset}")
		endif()

		if("${deppreset}" STREQUAL "all")
			get_property(depall GLOBAL PROPERTY CATNIP_${deppkg}_PRESETS)
			if("${depall}" STREQUAL "")
				message(FATAL_ERROR "Preset ${pkgname}.${preset} has unsatisfied dependency: ${deppkg}.all")
			endif()

			foreach(deppreset IN LISTS depall)
				list(APPEND outlist "${deppkg}.${deppreset}")
			endforeach()
		else()
			get_property(valid GLOBAL PROPERTY CATNIP_${deppkg}__${deppreset}_STAMP SET)
			if(NOT valid)
				message(FATAL_ERROR "Preset ${pkgname}.${preset} has unsatisfied dependency: ${deppkg}.${deppreset}")
			endif()

			list(APPEND outlist "${deppkg}.${deppreset}")
		endif()
	endforeach()

	set_property(GLOBAL PROPERTY CATNIP_${pkgname}__${preset}_${propname} "${outlist}")
endfunction()

function(__catnip_validate_packages)
	get_property(pkglist GLOBAL PROPERTY CATNIP_PACKAGES)
	if("${pkglist}" STREQUAL "")
		message(FATAL_ERROR "No packages defined")
	endif()

	foreach(pkgname IN LISTS pkglist)
		set(scope CATNIP_${pkgname})
		get_property(presets GLOBAL PROPERTY ${scope}_PRESETS)
		get_property(default GLOBAL PROPERTY ${scope}_DEFAULT)

		if("${presets}" STREQUAL "")
			message(FATAL_ERROR "Package ${pkgname} has no presets defined")
		endif()

		if("${default}" MATCHES "^[aA][lL][lL]$")
			set(default "${presets}")
			set_property(GLOBAL PROPERTY ${scope}_DEFAULT ${default})
		elseif("${default}" STREQUAL "")
			list(GET presets 0 default)
			set_property(GLOBAL PROPERTY ${scope}_DEFAULT ${default})
		else()
			foreach(preset IN LISTS default)
				__catnip_check_identifier(preset)
				get_property(valid GLOBAL PROPERTY ${scope}__${preset}_STAMP SET)
				if(NOT valid)
					message(FATAL_ERROR "Package ${pkgname} has invalid default preset: ${preset}")
				endif()
			endforeach()
		endif()

		foreach(preset IN LISTS presets)
			__catnip_validate_deplist(${pkgname} ${preset} DEPENDS)
		endforeach()
	endforeach()
endfunction()

function(__catnip_visit selector verb)
	string(REPLACE "." "__" scope "${selector}")
	set(scope CATNIP_${scope})

	get_property(valid GLOBAL PROPERTY ${scope}_STAMP SET)
	if(NOT valid)
		message(FATAL_ERROR "Invalid preset: ${selector}")
	endif()

	# Store verb now in order to allow overriding it even if
	# this selector has already been visited before
	set_property(GLOBAL PROPERTY ${scope}_VERB "${verb}")

	get_property(state GLOBAL PROPERTY ${scope}_VISIT)
	if("${state}" STREQUAL "2")
		return()
	elseif("${state}" STREQUAL "1")
		message(FATAL_ERROR "Dependency cycle detected: ${selector}")
	endif()

	set_property(GLOBAL PROPERTY ${scope}_VISIT 1)

	if("${verb}" IN_LIST CATNIP_VERBS_WITH_DEPS)
		if("${verb}" STREQUAL "install")
			# install decays to build for dependencies
			set(depverb "build")
		else()
			set(depverb "${verb}")
		endif()

		get_property(deps GLOBAL PROPERTY ${scope}_DEPENDS)
		foreach(dep IN LISTS deps)
			__catnip_visit("${dep}" "${depverb}")
		endforeach()
	endif()

	set_property(GLOBAL PROPERTY ${scope}_VISIT 2)
	set_property(GLOBAL APPEND PROPERTY CATNIP_SELECTORS "${selector}")
endfunction()

function(__catnip_extract_verb)
	if(NOT "${CATNIP_VERB}" STREQUAL "")
		return()
	endif()

	if(NOT "${CATNIP_ARGV}" STREQUAL "")
		list(GET CATNIP_ARGV 0 verb)
	else()
		set(verb "")
	endif()

	if("${verb}" IN_LIST CATNIP_VERBS OR "${verb}" MATCHES "^dump-.+")
		list(REMOVE_AT CATNIP_ARGV 0)
	else()
		set(verb "build")
	endif()

	set(CATNIP_VERB "${verb}" PARENT_SCOPE)
	set(CATNIP_ARGV "${CATNIP_ARGV}" PARENT_SCOPE)
endfunction()

function(__catnip_planner)
	if(NOT CATNIP_PACKAGE_SELECTORS)
		get_property(cwdpkg GLOBAL PROPERTY CATNIP_CWD_PACKAGE)
		if("${cwdpkg}" STREQUAL "")
			message(FATAL_ERROR "The current directory does not contain a Catnip package.")
		endif()

		if("${CATNIP_ARGV}" STREQUAL "")
			get_property(CATNIP_ARGV GLOBAL PROPERTY CATNIP_${cwdpkg}_DEFAULT)
		elseif("all" IN_LIST CATNIP_ARGV)
			get_property(CATNIP_ARGV GLOBAL PROPERTY CATNIP_${cwdpkg}_PRESETS)
		endif()

		foreach(selector IN LISTS CATNIP_ARGV)
			__catnip_visit("${cwdpkg}.${selector}" "${CATNIP_VERB}")
		endforeach()
	else()
		message(FATAL_ERROR "Package selectors not implemented")
	endif()

	get_property(out GLOBAL PROPERTY CATNIP_SELECTORS)
	set(CATNIP_SELECTORS "${out}" PARENT_SCOPE)
endfunction()

__catnip_validate_packages()
__catnip_extract_verb()
