if (WITH_COVERAGE)
    if (CMAKE_CXX_COMPILER_ID STREQUAL Clang)
        set(test_launcher
            ${CMAKE_COMMAND} -E env
            LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/yosys-slang.%p.profraw
        )

        add_custom_target(
            coverage
            COMMAND
                llvm-profdata merge -sparse
                yosys-slang.*.profraw
                -o yosys-slang.profdata
            COMMAND
                llvm-cov export -format lcov
                $<TARGET_FILE:yosys-slang>
                -instr-profile=yosys-slang.profdata
                --ignore-filename-regex=/third_party/
                --ignore-filename-regex=^/usr
                > yosys-slang.info
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
    endif()
endif()

set(ALL_TESTS
    liveness/lowering.ys
    liveness/composition.ys
    liveness/equivalence.ys
    liveness/unsupported.ys
    liveness/unbounded_sequences.ys
    liveness/regular_sequences.ys
    sva/seqequiv.tcl
    sva/range_guard.ys
    sva/unsupported_sampled_clocking.ys
    sva/unsupported_clocking.ys
    sva/vector_delay_init.ys
    unit/async.ys
    unit/dualedge.ys
    unit/function_call.ys
    unit/expression_attrs.ys
    unit/latch.ys
    unit/selftests.tcl
    various/assign_mixing.ys
    various/assignment_pattern_lhs.ys
    various/bb_detect.ys
    various/blackbox_scenarios.ys
    various/bus_range.ys
    various/concurrent_assert.ys
    various/defaults.ys
    various/delays.ys
    various/dualedge.ys
    various/expr.ys
    various/flop_naming.ys
    various/formal_stmts.ys
    various/hierref_error.ys
    various/huge_reg.ys
    various/ignore_asserts.ys
    various/init.ys
    various/intf_array_naming.ys
    various/intf_w_hierarchy.ys
    various/issue142.ys
    various/issue143.ys
    various/issue208.ys
    various/issue227.ys
    various/issue240.ys
    various/issue266.ys
    various/issue300.ys
    various/issue50.ys
    various/mem_inference.ys
    various/meminit.ys
    various/pragmas.ys
    various/readmem.ys
    various/readmem_diag.ys
    various/regress.ys
    various/stringattrs.ys
    various/stringparams.ys
    various/timescale.ys
    various/tranif.ys
    various/top_attr.ys
    various/unimplemented.ys
    various/unknown_cells.ys
    various/toplevel_intf.ys
    various/wait_test.ys
    various/expect_test.ys
    various/program_test.ys
    various/synthesis_define.ys
)

foreach(test_script ${ALL_TESTS})
    set(test_fullpath ${CMAKE_CURRENT_SOURCE_DIR}/${test_script})
    get_filename_component(test_dir ${test_fullpath} DIRECTORY)
    add_test(
        NAME ${test_script}
        WORKING_DIRECTORY ${test_dir}
        COMMAND
            ${test_launcher}
            ${YOSYS_BINDIR}/yosys -m $<TARGET_FILE:yosys-slang> ${test_fullpath}
    )
endforeach()

# Distribution packages do not consistently name the standalone ABC binary.
# Keep the test local to the discovered executable instead of installing a
# global yosys-abc compatibility symlink.
find_program(YOSYS_ABC_EXECUTABLE
    NAMES yosys-abc abc
    HINTS ${YOSYS_BINDIR}
)
if (YOSYS_ABC_EXECUTABLE)
    set_tests_properties(sva/seqequiv.tcl PROPERTIES
        ENVIRONMENT "YOSYS_ABC=${YOSYS_ABC_EXECUTABLE}"
    )
endif()

option(ENABLE_LIVENESS_PROOFS
       "Enable unbounded SVA liveness proofs with the external suprove engine"
       OFF)
set(SUPROVE_EXECUTABLE "" CACHE FILEPATH
    "Path to a pinned suprove executable for ENABLE_LIVENESS_PROOFS")

if (ENABLE_LIVENESS_PROOFS)
    if (NOT EXISTS "${SUPROVE_EXECUTABLE}")
        message(FATAL_ERROR
            "ENABLE_LIVENESS_PROOFS requires SUPROVE_EXECUTABLE; "
            "run tools/setup_liveness_tools.sh or select an existing pinned binary")
    endif()
    add_test(
        NAME liveness/response_proof
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/liveness
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/liveness/run_response_proof.sh
            $<TARGET_FILE:yosys-slang>
            ${SUPROVE_EXECUTABLE}
    )
    set_tests_properties(
        liveness/response_proof
        PROPERTIES
            LABELS "formal;liveness"
            TIMEOUT 120
    )
    add_test(
        NAME opentitan_liveness/arbiter
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/opentitan_liveness
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/opentitan_liveness/run_arbiter.sh
            $<TARGET_FILE:yosys-slang>
            ${SUPROVE_EXECUTABLE}
    )
    set_tests_properties(
        opentitan_liveness/arbiter
        PROPERTIES
            LABELS "formal;liveness;opentitan"
            TIMEOUT 180
    )
    add_test(
        NAME opentitan_liveness/stress_targets
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/opentitan_liveness
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/opentitan_liveness/run_stress_targets.sh
            $<TARGET_FILE:yosys-slang>
            ${SUPROVE_EXECUTABLE}
    )
    set_tests_properties(
        opentitan_liveness/stress_targets
        PROPERTIES
            LABELS "formal;liveness;opentitan;stress"
            TIMEOUT 600
    )
endif()

option(ENABLE_OPENTITAN_SVA_STRESS "Enable OpenTitan SVA stress tests" OFF)

if (ENABLE_OPENTITAN_SVA_STRESS)
    add_test(
        NAME opentitan_sva_stress
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/opentitan_sva_stress
        COMMAND
            ${CMAKE_COMMAND} -E env
            PLUGIN=$<TARGET_FILE:yosys-slang>
            ${CMAKE_CURRENT_SOURCE_DIR}/opentitan_sva_stress/run_all.sh
    )
    set_tests_properties(
        opentitan_sva_stress
        PROPERTIES
            LABELS "opentitan;sva;stress"
    )
endif()
