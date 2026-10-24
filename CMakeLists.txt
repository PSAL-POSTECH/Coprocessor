cmake_minimum_required(VERSION 3.16)
project(vcix_accelerator CXX)

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
# examples/exp_sfu: the smallest cp model, the README's tutorial: one component, one instruction, one pipeline.
vcix_cp_add_model(exp_sfu examples/exp_sfu/exp_sfu.cc)
vcix_cp_add_model(tpu examples/tpu/tpu.cc)
vcix_cp_add_model(tpuv6e examples/tpuv6e/tpuv6e.cc)
vcix_cp_add_model(tpuv3 examples/tpuv3/tpuv3.cc)
# examples/copier: examples/rtl/copier/copier.sv's timing as a cp component, no Verilator needed.
vcix_cp_add_model(copier examples/copier/copier.cc)
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
             VERILATOR_ARGS --savable -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL ${ARGN})
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
  vcix_cp_add_rtl_model(rtl_copier examples/rtl/copier/copier.cc examples/rtl/copier/copier.sv copier)
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
vcix_cp_add_model(cannot_be_made tests/contract/refused.cc)
target_compile_definitions(cannot_be_made PRIVATE WHICH=3)

# tests/contract: the checks that need no simulator.
add_executable(direct tests/contract/direct.cc)
target_link_libraries(direct PRIVATE vcix_cp ${CMAKE_DL_LIBS})
add_executable(port_stats tests/contract/port_stats.cc)
target_link_libraries(port_stats PRIVATE vcix_cp)
add_executable(route_once tests/contract/route_once.cc)
target_link_libraries(route_once PRIVATE vcix_cp)

# tests/tpu: the tpu units' ports, read through the table with no simulator.
add_executable(tpu_ports tests/tpu/ports.cc)
target_link_libraries(tpu_ports PRIVATE vcix_cp ${CMAKE_DL_LIBS})
# tests/tpuv6e: the tpuv6e model's units as its statistics list them, with no simulator.
add_executable(tpuv6e_ports tests/tpuv6e/ports.cc)
target_link_libraries(tpuv6e_ports PRIVATE vcix_cp ${CMAKE_DL_LIBS})
# tests/tpuv3: the same, of the tpuv3 model.
add_executable(tpuv3_ports tests/tpuv3/ports.cc)
target_link_libraries(tpuv3_ports PRIVATE vcix_cp ${CMAKE_DL_LIBS})
# tests/rtl: libtpu.so's RTL keys against librtl_systolic.so where the array's stall mode shows, with no simulator.
add_executable(rtl_stall tests/rtl/stall.cc)
target_link_libraries(rtl_stall PRIVATE vcix_cp ${CMAKE_DL_LIBS})
# tests/rtl: libcopier.so against librtl_copier.so in lockstep, with no simulator.
add_executable(copier_lockstep tests/rtl/copier.cc)
target_link_libraries(copier_lockstep PRIVATE vcix_cp ${CMAKE_DL_LIBS})

add_executable(timing_probe tools/timing_probe.cc)
target_link_libraries(timing_probe PRIVATE vcix_cp ${CMAKE_DL_LIBS})
