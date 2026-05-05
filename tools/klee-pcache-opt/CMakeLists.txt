#===------------------------------------------------------------------------===#
#
#                     The KLEE Symbolic Virtual Machine
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
#
#===------------------------------------------------------------------------===#

add_executable(klee-pcache-opt
  main.cpp
  OfflineEngine.cpp)

llvm_config(klee-pcache-opt "${USE_LLVM_SHARED}" core support)

target_compile_features(klee-pcache-opt PRIVATE cxx_std_17)
target_include_directories(klee-pcache-opt PRIVATE
  ${KLEE_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS}
  # lib/Solver carries the private Z3Builder.h that we reuse for the
  # parallel-context unsat-core path.
  ${CMAKE_SOURCE_DIR}/lib/Solver
  ${KLEE_SOLVER_INCLUDE_DIRS})
target_link_libraries(klee-pcache-opt PRIVATE kleaverSolver)
target_compile_options(klee-pcache-opt PRIVATE ${KLEE_COMPONENT_CXX_FLAGS})
target_compile_definitions(klee-pcache-opt PRIVATE ${KLEE_COMPONENT_CXX_DEFINES})

install(TARGETS klee-pcache-opt DESTINATION bin)
