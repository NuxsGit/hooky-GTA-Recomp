# Generate the same shaders for the Windows runtime and the optional headless Vulkan test.
function(gtav_generate_fsr1 out_dir)
    set(shader_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/native/shaders")
    set(hlsl "${shader_dir}/native_fsr1.hlsl")
    # AMD's upstream header uses HLSL 2018 vector ternaries. Keep the vendor files untouched.
    add_custom_command(
        OUTPUT "${out_dir}/native_fsr_easu_ps.h" "${out_dir}/native_fsr_rcas_ps.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND ${ARGN} -T ps_6_0 -E PSFsrEasu -D GTAV_FSR_EASU=1 -HV 2018 -WX
                -spirv -fspv-target-env=vulkan1.1 -Vn g_native_fsr_easu_ps
                -Fh "${out_dir}/native_fsr_easu_ps.h" "${hlsl}"
        COMMAND ${ARGN} -T ps_6_0 -E PSFsrRcas -D GTAV_FSR_EASU=0 -HV 2018 -WX
                -spirv -fspv-target-env=vulkan1.1 -Vn g_native_fsr_rcas_ps
                -Fh "${out_dir}/native_fsr_rcas_ps.h" "${hlsl}"
        DEPENDS "${hlsl}" "${shader_dir}/fsr1/ffx_a.h" "${shader_dir}/fsr1/ffx_fsr1.h"
        VERBATIM)
endfunction()
