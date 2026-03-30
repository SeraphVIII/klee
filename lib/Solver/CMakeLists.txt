#===------------------------------------------------------------------------===#
#
#                     The KLEE Symbolic Virtual Machine
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
#
#===------------------------------------------------------------------------===#
find_package(Protobuf REQUIRED)

# Generate mapofsets.pb.h and mapofsets.pb.cc into the build directory.
# PROTO_HDRS / PROTO_SRCS are absolute paths under CMAKE_CURRENT_BINARY_DIR.
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS mapofsets.proto)

add_library(kleaverSolver
  AssignmentValidatingSolver.cpp
  CachingSolver.cpp
  CexCachingSolver.cpp
  ConstantDivision.cpp
  ConstraintCanonicalizer.cpp
  ConstructSolverChain.cpp
  CoreSolver.cpp
  DiskCexCache.cpp
  DiskMapOfSets.cpp
  DummySolver.cpp
  FastCexSolver.cpp
  IncompleteSolver.cpp
  IndependentSolver.cpp
  MapOfSetsDiskBuilder.cpp
  MetaSMTSolver.cpp
  KQueryLoggingSolver.cpp
  QueryLoggingSolver.cpp
  SMTLIBLoggingSolver.cpp
  Solver.cpp
  SolverCmdLine.cpp
  SolverImpl.cpp
  SolverStats.cpp
  STPBuilder.cpp
  STPSolver.cpp
  ValidatingSolver.cpp
  Z3Builder.cpp
  Z3Solver.cpp
  ${PROTO_SRCS}
)

llvm_config(kleaverSolver "${USE_LLVM_SHARED}" support)
target_link_libraries(kleaverSolver PRIVATE
  kleeBasic
  kleaverExpr
  kleeSupport
  ${KLEE_SOLVER_LIBRARIES}
  protobuf::libprotobuf)

# CMAKE_CURRENT_BINARY_DIR is where the generated pb.h lands.
# PUBLIC so that targets linking kleaverSolver (e.g. SolverTest) can include
# "mapofsets.pb.h" via DiskMapOfSets.h without listing the build dir themselves.
target_include_directories(kleaverSolver
  PUBLIC  ${CMAKE_CURRENT_BINARY_DIR}
  PRIVATE ${KLEE_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${KLEE_SOLVER_INCLUDE_DIRS})

target_compile_options(kleaverSolver PRIVATE ${KLEE_COMPONENT_CXX_FLAGS})
target_compile_definitions(kleaverSolver PRIVATE ${KLEE_COMPONENT_CXX_DEFINES})
