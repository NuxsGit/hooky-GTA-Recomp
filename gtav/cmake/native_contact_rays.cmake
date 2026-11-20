# Keep this pass optional for builds that supply older precompiled shader headers.
function(gtav_generate_contact_rays out_dir)
    set(source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/native/shaders/native_contact_rays.hlsl")
    add_custom_command(OUTPUT "${out_dir}/native_contact_rays_ps.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND ${ARGN} -T ps_6_0 -E PSContactRays -spirv -fspv-target-env=vulkan1.1
                -Vn g_native_contact_rays_ps -Fh "${out_dir}/native_contact_rays_ps.h" "${source}"
        DEPENDS "${source}"
        VERBATIM)
endfunction()
