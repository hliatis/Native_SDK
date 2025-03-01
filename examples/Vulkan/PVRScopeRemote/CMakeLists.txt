cmake_minimum_required(VERSION 3.3)
project(VulkanPVRScopeRemote)
if(APPLE)
	if(IOS)
		message("Skipping VulkanPVRScopeRemote : Not supported on iOS")
		return()
	endif()
endif()
if(APPLE)
	if (NOT IOS)
		message("Skipping VulkanPVRScopeRemote : Not supported on macOS")
		return()
	endif()
endif()
if(APPLE)
	message ("Vulkan is not supported on Apple platforms.")
	return()
endif()
add_subdirectory(../../.. ${CMAKE_CURRENT_BINARY_DIR}/sdk)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT VulkanPVRScopeRemote)

set(SRC_FILES VulkanPVRScopeRemote.cpp)

set(ASSET_FOLDER ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Assets_VulkanPVRScopeRemote)

# ASSET FILES (textures, scene files, textures etc) - Packaged with the executable for runtime use
set(ASSET_FILES
	${CMAKE_CURRENT_SOURCE_DIR}/../../assets/Satyr/Marble.pvr
	${CMAKE_CURRENT_SOURCE_DIR}/../../assets/Satyr/Satyr.pod
	)
list(APPEND SRC_FILES ${ASSET_FILES})
source_group("assets" FILES ${ASSET_FILES})

# SHADER SOURCE FILES (Vulkan) - Compiled to spir-v at build-time
set(SHADER_NAMES
FragShader.fsh
VertShader.vsh
	)

foreach (SHADER_FILE ${SHADER_NAMES})
	list(APPEND SHADER_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_FILE}")
	list(APPEND SHADER_GENERATED_FILES "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_FILE}.spv")
endforeach()
list(APPEND SRC_FILES ${SHADER_SOURCE_FILES})
list(APPEND SRC_FILES ${SHADER_GENERATED_FILES})
source_group("shaders_source" FILES ${SHADER_SOURCE_FILES})
source_group("shaders_generated" FILES ${SHADER_GENERATED_FILES})

set(RESOURCE_FILES ${ASSET_FILES} ${SHADER_SOURCE_FILES} ${SHADER_GENERATED_FILES})

# Adds Windows resouces.rc, macOS plists etc. For macOS/iOS, also set the opengl dynamic libs in the "frameworks" group
add_platform_specific_resource_files(SRC_FILES RESOURCE_FILES)

# Generate a pre-build step for compiling glsl shaders to spir-v
add_rule_generate_spirv_from_shaders("${SHADER_NAMES}")

# Adds a platform specific executable
add_platform_specific_executable(VulkanPVRScopeRemote "${SRC_FILES}" "${RESOURCE_FILES}")

# Copies the assets to our assets folder
add_rule_copy_assets_to_asset_folder("${RESOURCE_FILES}" "${ASSET_FOLDER}")

target_link_libraries(VulkanPVRScopeRemote
	PVRShell
	PVRUtilsVk
	${SCOPE_LIB_FOLDER}/${CMAKE_STATIC_LIBRARY_PREFIX}PVRScopeDeveloper${CMAKE_STATIC_LIBRARY_SUFFIX}
)
if(WIN32)
	add_custom_command(TARGET VulkanPVRScopeRemote POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCOPE_LIB_FOLDER}/${CMAKE_SHARED_LIBRARY_PREFIX}PVRScopeDeveloper${CMAKE_SHARED_LIBRARY_SUFFIX}" $<TARGET_FILE_DIR:VulkanPVRScopeRemote>)
endif()