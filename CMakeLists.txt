cmake_minimum_required(VERSION 3.16)
project(Coprocessor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(vcix_cp INTERFACE)
target_include_directories(vcix_cp INTERFACE include)

function(vcix_cp_add_model name source)
  add_library(${name} SHARED ${source})
  target_link_libraries(${name} PRIVATE vcix_cp)
  set_target_properties(${name} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
endfunction()

vcix_cp_add_model(print_args examples/print_args/print_args.cc)
# examples/exp_unit: the smallest cp model, the README's tutorial: one component, one instruction, one pipeline.
vcix_cp_add_model(exp_unit examples/exp_unit/exp_unit.cc)
vcix_cp_add_model(tpu examples/tpu/tpu.cc)
vcix_cp_add_model(tpuv6e examples/tpuv6e/tpuv6e.cc)
vcix_cp_add_model(tpuv3 examples/tpuv3/tpuv3.cc)
# examples/xlane_all_reduce: sum across the lanes, a row a cycle through a tree; Hands-on 1 of the tutorial.
# One model, libxlane_all_reduce.so: the unit beside the TPU v6e's units, run by Hands-on 1 and compiled for by PTB.
vcix_cp_add_model(xlane_all_reduce examples/xlane_all_reduce/xlane.cc)
target_include_directories(xlane_all_reduce PRIVATE examples)
# Hands-on 1's test programs, in build/xlane_all_reduce: values (values.c, with kernels.S's vector code at .kernel
# 0x2000000) and timing (timing.S). They need the RISC-V toolchain (TOOLCHAIN_ROOT, which scripts/build.sh passes) and
# are skipped without it. They do not include xlane_all_reduce.hpp, so they build while the hands-on's start does not.
find_program(RISCV_CLANG clang HINTS ${TOOLCHAIN_ROOT}/bin)
find_program(RISCV_GCC riscv64-unknown-elf-gcc HINTS ${TOOLCHAIN_ROOT}/bin)
if(RISCV_CLANG AND RISCV_GCC)
  set(xlane_tests ${CMAKE_SOURCE_DIR}/tests/xlane_all_reduce)
  set(xlane_out ${CMAKE_BINARY_DIR}/xlane_all_reduce)
  set(rv_clang ${RISCV_CLANG} --target=riscv64 -march=rv64gcv_xsfvcp)
  add_custom_command(OUTPUT ${xlane_out}/kernels.o
    COMMAND ${CMAKE_COMMAND} -E make_directory ${xlane_out}
    COMMAND ${rv_clang} -c ${xlane_tests}/kernels.S -o ${xlane_out}/kernels.o
    DEPENDS ${xlane_tests}/kernels.S COMMENT "Assembling xlane_all_reduce/kernels.o" VERBATIM)
  add_custom_command(OUTPUT ${xlane_out}/values
    COMMAND ${RISCV_GCC} -O1 -Wall -Werror -static -march=rv64gc -mabi=lp64d -DLANES=128u
            -Wl,--section-start=.kernel=0x2000000 ${xlane_tests}/values.c ${xlane_out}/kernels.o -o ${xlane_out}/values
    DEPENDS ${xlane_tests}/values.c ${xlane_out}/kernels.o COMMENT "Linking xlane_all_reduce/values" VERBATIM)
  add_custom_command(OUTPUT ${xlane_out}/timing
    COMMAND ${CMAKE_COMMAND} -E make_directory ${xlane_out}
    COMMAND ${rv_clang} -c ${xlane_tests}/timing.S -o ${xlane_out}/timing.o
    COMMAND ${RISCV_GCC} -static ${xlane_out}/timing.o -o ${xlane_out}/timing
    DEPENDS ${xlane_tests}/timing.S COMMENT "Linking xlane_all_reduce/timing" VERBATIM)
  add_custom_target(xlane_all_reduce_programs ALL DEPENDS ${xlane_out}/values ${xlane_out}/timing)
else()
  message(STATUS "RISC-V toolchain not found (TOOLCHAIN_ROOT): Hands-on 1's test programs are not built")
endif()
# examples/tpuv6e and examples/tpuv3 are the tpu example's units, its matrix unit among them, as tpu/<unit>.hpp.
target_include_directories(tpuv6e PRIVATE examples)
target_include_directories(tpuv3 PRIVATE examples)
vcix_cp_add_model(pipelined tests/pipeline/pipelined.cc)

# examples/rtl: models that are one Verilated unit each, built when Verilator is found (VERILATOR_ROOT, or verilator on
# PATH) and skipped otherwise; -DVCIX_RTL=OFF skips them too. The array is built three times; tests/rtl shows that the
# variant whose weight push does not wait gives other values, and that a wrong out_tag ends a gem5 run.
option(VCIX_RTL "Build examples/rtl's models when Verilator is found" ON)
if(VCIX_RTL)
  find_package(verilator QUIET HINTS $ENV{VERILATOR_ROOT} ${VERILATOR_ROOT})
endif()
if(VCIX_RTL AND verilator_FOUND)
  set(rtl_systolic_lanes 4)
  function(vcix_cp_add_rtl_model name source sv top)
    vcix_cp_add_model(${name} ${source})
    target_include_directories(${name} PRIVATE examples/rtl)
    verilate(${name} SOURCES ${sv} TOP_MODULE ${top} PREFIX V${top}
             VERILATOR_ARGS --savable -Wall ${ARGN})
  endfunction()
  foreach(variant rtl_systolic:1:0 rtl_systolic_nowait:0:0 rtl_systolic_badtag:1:1)
    string(REPLACE ":" ";" variant ${variant})
    list(GET variant 0 name)
    list(GET variant 1 waits)
    list(GET variant 2 bad_tag)
    vcix_cp_add_rtl_model(${name} examples/rtl/systolic/systolic.cc examples/rtl/systolic/systolic.sv systolic
                             -GL=${rtl_systolic_lanes} -GWEIGHT_WAITS=${waits} -GBAD_TAG=${bad_tag})
    target_compile_definitions(${name} PRIVATE SYSTOLIC_LANES=${rtl_systolic_lanes})
  endforeach()
elseif(VCIX_RTL)
  message(STATUS "Verilator not found (VERILATOR_ROOT, PATH): examples/rtl is not built")
endif()
vcix_cp_add_model(reports tests/contract/reports.cc)
vcix_cp_add_model(timing tests/contract/timing.cc)

# tests/contract: one source built twice under the same class name, hidden and not.
foreach(which 1 2)
  vcix_cp_add_model(same_name_${which} tests/contract/same_name.cc)
  target_compile_definitions(same_name_${which} PRIVATE WHICH=${which})
  vcix_cp_add_model(same_name_default_${which} tests/contract/same_name.cc)
  target_compile_definitions(same_name_default_${which} PRIVATE WHICH=${which})
  set_target_properties(same_name_default_${which} PROPERTIES CXX_VISIBILITY_PRESET default VISIBILITY_INLINES_HIDDEN OFF)
endforeach()

# tests/contract: a table written by hand, with `ready` and without; three libraries a caller must refuse.
vcix_cp_add_model(ready_only tests/contract/by_hand.cc)
target_compile_definitions(ready_only PRIVATE READY=1)
vcix_cp_add_model(no_optional tests/contract/by_hand.cc)
target_compile_definitions(no_optional PRIVATE READY=0)
vcix_cp_add_model(null_table tests/contract/refused.cc)
target_compile_definitions(null_table PRIVATE WHICH=1)
vcix_cp_add_model(other_abi tests/contract/refused.cc)
target_compile_definitions(other_abi PRIVATE WHICH=2)

# tests/contract: the checks that need no simulator.
add_executable(direct tests/contract/direct.cc)
target_link_libraries(direct PRIVATE vcix_cp ${CMAKE_DL_LIBS})
add_executable(counters tests/contract/counters.cc)
target_link_libraries(counters PRIVATE vcix_cp)
add_executable(route_once tests/contract/route_once.cc)
target_link_libraries(route_once PRIVATE vcix_cp)

# tests/tpu: the tpu units' counters, read through the table with no simulator.
add_executable(tpu_ports tests/tpu/ports.cc)
target_link_libraries(tpu_ports PRIVATE vcix_cp ${CMAKE_DL_LIBS})
# tests/tpuv6e: the tpuv6e model's counters as its statistics list them, with no simulator.
add_executable(tpuv6e_ports tests/tpuv6e/ports.cc)
target_link_libraries(tpuv6e_ports PRIVATE vcix_cp ${CMAKE_DL_LIBS})
# tests/tpuv3: the same, of the tpuv3 model.
add_executable(tpuv3_ports tests/tpuv3/ports.cc)
target_link_libraries(tpuv3_ports PRIVATE vcix_cp ${CMAKE_DL_LIBS})
# tests/rtl: libtpu.so's RTL keys against librtl_systolic.so where the array's stall mode shows, with no simulator.
add_executable(rtl_stall tests/rtl/stall.cc)
target_link_libraries(rtl_stall PRIVATE vcix_cp ${CMAKE_DL_LIBS})

add_executable(timing_probe tools/timing_probe.cc)
target_link_libraries(timing_probe PRIVATE vcix_cp ${CMAKE_DL_LIBS})
