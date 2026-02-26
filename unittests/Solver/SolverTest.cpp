//===-- SolverTest.cpp ----------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "klee/Expr/ArrayCache.h"
#include "klee/Expr/Constraints.h"
#include "klee/Expr/Expr.h"
#include "klee/Solver/Solver.h"
#include "klee/Solver/SolverCmdLine.h"
#include "klee/Solver/DiskMapOfSets.h"
#include "klee/Solver/MapOfSetsDiskBuilder.h"

#include "llvm/ADT/StringExtras.h"

#include <iostream>

using namespace klee;

namespace {

const int g_constants[] = { -1, 1, 4, 17, 0 };
const Expr::Width g_types[] = { Expr::Bool,
				Expr::Int8,
				Expr::Int16,
				Expr::Int32,
				Expr::Int64 };

ref<Expr> getConstant(int value, Expr::Width width) {
  int64_t ext = value;
  uint64_t trunc = ext & (((uint64_t) -1LL) >> (64 - width));
  return ConstantExpr::create(trunc, width);
}

// We have to have the cache globally scopped (and not in ``testOperation``)
// because the Solver (i.e. in STP's case the STPBuilder) holds on to pointers
// to allocated Arrays.
ArrayCache ac;

template<class T>
void testOperation(Solver &solver,
                   int value,
                   Expr::Width operandWidth,
                   Expr::Width resultWidth) {
  std::vector<Expr::CreateArg> symbolicArgs;
  
  for (unsigned i = 0; i < T::numKids; i++) {
    if (!T::isValidKidWidth(i, operandWidth))
      return;

    unsigned size = Expr::getMinBytesForWidth(operandWidth);
    static uint64_t id = 0;
    const Array *array = ac.CreateArray("arr" + llvm::utostr(++id), size);
    symbolicArgs.push_back(Expr::CreateArg(Expr::createTempRead(array, 
                                                                operandWidth)));
  }
  
  if (T::needsResultType())
    symbolicArgs.push_back(Expr::CreateArg(resultWidth));
  
  ref<Expr> fullySymbolicExpr = Expr::createFromKind(T::kind, symbolicArgs);

  // For each kid, replace the kid with a constant value and verify
  // that the fully symbolic expression is equivalent to it when the
  // replaced value is appropriated constrained.
  for (unsigned kid = 0; kid < T::numKids; kid++) {
    std::vector<Expr::CreateArg> partiallyConstantArgs(symbolicArgs);
    partiallyConstantArgs[kid] = getConstant(value, operandWidth);

    ref<Expr> expr = 
      NotOptimizedExpr::create(EqExpr::create(partiallyConstantArgs[kid].expr,
                                              symbolicArgs[kid].expr));
    
    ref<Expr> partiallyConstantExpr =
      Expr::createFromKind(T::kind, partiallyConstantArgs);
    
    ref<Expr> queryExpr = EqExpr::create(fullySymbolicExpr, 
                                         partiallyConstantExpr);

    ConstraintSet constraints;
    ConstraintManager cm(constraints);
    cm.addConstraint(expr);
    bool res;
    bool success = solver.mustBeTrue(Query(constraints, queryExpr), res);
    EXPECT_EQ(true, success) << "Constraint solving failed";

    if (success) {
      EXPECT_EQ(true, res) << "Evaluation failed!\n" 
                           << "query " << queryExpr 
                           << " with " << expr;
    }
  }
}

template<class T>
void testOpcode(Solver &solver, bool tryBool = true, bool tryZero = true, 
                unsigned maxWidth = 64) {
  for (unsigned j=0; j<sizeof(g_types)/sizeof(g_types[0]); j++) {
    Expr::Width type = g_types[j]; 

    if (type > maxWidth) continue;

    for (unsigned i=0; i<sizeof(g_constants)/sizeof(g_constants[0]); i++) {
      int value = g_constants[i];
      if (!tryZero && !value) continue;
      if (type == Expr::Bool && !tryBool) continue;

      if (!T::needsResultType()) {
        testOperation<T>(solver, value, type, type);
        continue;
      }

      for (unsigned k=0; k<sizeof(g_types)/sizeof(g_types[0]); k++) {
        Expr::Width resultType = g_types[k];
          
        // nasty hack to give only Trunc/ZExt/SExt the right types
        if (T::kind == Expr::SExt || T::kind == Expr::ZExt) {
          if (Expr::getMinBytesForWidth(type) >= 
              Expr::getMinBytesForWidth(resultType)) 
            continue;
        }
            
        testOperation<T>(solver, value, type, resultType);
      }
    }
  }
}

TEST(SolverTest, Evaluation) {
  auto solver = klee::createCoreSolver(CoreSolverToUse);

  solver = createCexCachingSolver(std::move(solver));
  solver = createCachingSolver(std::move(solver));
  solver = createIndependentSolver(std::move(solver));

  testOpcode<SelectExpr>(*solver);
  testOpcode<ZExtExpr>(*solver);
  testOpcode<SExtExpr>(*solver);
  
  testOpcode<AddExpr>(*solver);
  testOpcode<SubExpr>(*solver);
  testOpcode<MulExpr>(*solver, false, true, 8);
  testOpcode<SDivExpr>(*solver, false, false, 8);
  testOpcode<UDivExpr>(*solver, false, false, 8);
  testOpcode<SRemExpr>(*solver, false, false, 8);
  testOpcode<URemExpr>(*solver, false, false, 8);
  testOpcode<ShlExpr>(*solver, false);
  testOpcode<LShrExpr>(*solver, false);
  testOpcode<AShrExpr>(*solver, false);
  testOpcode<AndExpr>(*solver);
  testOpcode<OrExpr>(*solver);
  testOpcode<XorExpr>(*solver);

  testOpcode<EqExpr>(*solver);
  testOpcode<NeExpr>(*solver);
  testOpcode<UltExpr>(*solver);
  testOpcode<UleExpr>(*solver);
  testOpcode<UgtExpr>(*solver);
  testOpcode<UgeExpr>(*solver);
  testOpcode<SltExpr>(*solver);
  testOpcode<SleExpr>(*solver);
  testOpcode<SgtExpr>(*solver);
  testOpcode<SgeExpr>(*solver);
}


// Add this test case at the end, before the closing brace:
TEST(DiskMapOfSetsTest, RoundTrip) {
  // 1. Build in-memory MapOfSets
  klee::MapOfSets<std::string,std::string> mem;
  mem.insert({"a"}, "UNSAT");
  mem.insert({"a","b"}, "SAT:1");
  mem.insert({"a","c"}, "SAT:2");
  mem.insert({"x"}, "SAT:42");

  // 2. Serialize to disk
  const std::string testFile = "test_disk_cache.mapo";
  klee::MapOfSetsDiskBuilder::build(mem, testFile);

  // 3. Load from disk
  klee::mapofsets::DiskMapOfSets disk(testFile);

  // 4. Test exact lookup
  {
    auto exact = disk.lookup({"a"});
    ASSERT_TRUE(exact.has_value());
    EXPECT_EQ("UNSAT", *exact);
  }

  // 5. Test supersets({"a"})
  {
    auto supers = disk.supersets({"a"});
    ASSERT_EQ(3, supers.size());  // {"a"}, {"a","b"}, {"a","c"}
    std::set<std::string> foundValues;
    for (const auto& e : supers) {
      foundValues.insert(e.value);
      if (e.key_set == std::set<std::string>{"a"})
        EXPECT_EQ("UNSAT", e.value);
    }
    EXPECT_NE(foundValues.find("SAT:1"), foundValues.end());
    EXPECT_NE(foundValues.find("SAT:2"), foundValues.end());
  }

  // 6. Test subsets({"a","b","c"})
  {
    auto subs = disk.subsets({"a","b","c"});
    ASSERT_EQ(1, subs.size());
    EXPECT_EQ("UNSAT", subs[0].value);
    EXPECT_EQ(std::set<std::string>{"a"}, subs[0].key_set);
  }

  // 7. Test empty/miss
  {
    auto miss = disk.lookup({"missing"});
    EXPECT_FALSE(miss.has_value());
  }

  printf("✅ DiskMapOfSets roundtrip PASSED\n");
}

// Updated test with correct semantics (subsets = UNSAT only)
TEST(DiskMapOfSetsTest, ComplexRoundTrip) {
  klee::MapOfSets<std::string,std::string> mem;

  // UNSAT prefixes (subsets hits)
  mem.insert({}, "UNSAT_root");            // Empty set = UNSAT
  mem.insert({"a"}, "UNSAT_a");            // Prefix = UNSAT
  mem.insert({"a", "b"}, "UNSAT_ab");      // Deeper UNSAT

  // SAT leaves (exact/supersets hits)
  mem.insert({"a", "b", "c"}, "SAT_abc");
  mem.insert({"x"}, "SAT_x");
  mem.insert({"x", "y"}, "SAT_xy");

  const std::string testFile = "complex_disk_cache.mapo";
  klee::MapOfSetsDiskBuilder::build(mem, testFile);

  klee::mapofsets::DiskMapOfSets disk(testFile);

  // Exact lookups (UNSAT + SAT)
  EXPECT_EQ("UNSAT_root", *disk.lookup({}));
  EXPECT_EQ("UNSAT_a", *disk.lookup({"a"}));
  EXPECT_EQ("UNSAT_ab", *disk.lookup({"a","b"}));
  EXPECT_EQ("SAT_abc", *disk.lookup({"a","b","c"}));
  EXPECT_EQ("SAT_x", *disk.lookup({"x"}));

  // supersets({"a"}) → ALL cached supersets (UNSAT + SAT)
  {
    auto supers = disk.supersets({"a"});
    ASSERT_EQ(3u, supers.size());  // {}, isn't a superset of {"a"}
    std::set<std::string> values;
    for (const auto& e : supers) values.insert(e.value);

    EXPECT_TRUE(values.count("UNSAT_a"));
    EXPECT_TRUE(values.count("UNSAT_ab"));
    EXPECT_TRUE(values.count("SAT_abc"));
  }

  // subsets({"a","b","c"}) → UNSAT subsets only
  {
    auto subs = disk.subsets({"a", "b", "c"});
    ASSERT_EQ(3u, subs.size());  // {}, {"a"}, {"a","b"}

    // Compare returned key_sets exactly
    std::set<std::set<std::string>> got;
    for (const auto& e : subs) {
      EXPECT_TRUE(e.value.rfind("UNSAT", 0) == 0); // starts with "UNSAT"
      got.insert(e.key_set);
    }

    EXPECT_TRUE(got.count(std::set<std::string>{}));
    EXPECT_TRUE(got.count(std::set<std::string>{"a"}));
    EXPECT_TRUE(got.count(std::set<std::string>{"a","b"}));
  }

  // subsets({"x","y"}) → UNSAT subsets (none)
  {
    auto subs = disk.subsets({"x", "y"});
    ASSERT_EQ(1u, subs.size());
    EXPECT_TRUE(subs[0].key_set.empty());
    EXPECT_TRUE(subs[0].value.rfind("UNSAT", 0) == 0);
  }

  unlink(testFile.c_str());
}

// Cleanup test file (optional)
TEST(DiskMapOfSetsTest, Cleanup) {
  std::remove("test_disk_cache.mapo");
}


}
