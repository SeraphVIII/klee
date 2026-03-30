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
#include "klee/Expr/ExprBuilder.h"
#include "klee/Expr/ExprPPrinter.h"
#include "klee/Solver/ConstraintCanonicalizer.h"
#include "klee/Solver/DiskCexCache.h"
#include "klee/Solver/DiskMapOfSets.h"
#include "klee/Solver/MapOfSetsDiskBuilder.h"
#include "klee/Solver/Solver.h"
#include "klee/Solver/SolverCmdLine.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

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

TEST(DiskMapOfSetsTest, RoundTrip) {
  klee::MapOfSets<std::string, std::string> mem;
  mem.insert({"a"}, "UNSAT");
  mem.insert({"a", "b"}, "SAT:1");
  mem.insert({"a", "c"}, "SAT:2");
  mem.insert({"x"}, "SAT:42");

  const std::string testFile = "test_disk_cache.mapo";
  klee::MapOfSetsDiskBuilder::build(mem, testFile);
  klee::mapofsets::DiskMapOfSets disk(testFile);

  // Exact lookup
  {
    auto exact = disk.lookup({"a"});
    ASSERT_TRUE(exact.has_value());
    EXPECT_EQ("UNSAT", *exact);
  }

  // supersets({"a"}) → {"a"}, {"a","b"}, {"a","c"} — not {"x"}
  {
    auto supers = disk.supersets({"a"});
    ASSERT_EQ(3u, supers.size());
    std::set<std::string> values;
    for (const auto &e : supers)
      values.insert(e.value);
    EXPECT_TRUE(values.count("UNSAT"));
    EXPECT_TRUE(values.count("SAT:1"));
    EXPECT_TRUE(values.count("SAT:2"));
    EXPECT_FALSE(values.count("SAT:42")); // {"x"} is not a superset of {"a"}
  }

  // subsets({"a","b","c"}) → {"a"}, {"a","b"}, {"a","c"}
  // Under new code find_subsets returns ALL entries (SAT and UNSAT).
  {
    auto subs = disk.subsets({"a", "b", "c"});
    ASSERT_EQ(3u, subs.size());
    std::set<std::set<std::string>> gotKeys;
    std::set<std::string> gotValues;
    for (const auto &e : subs) {
      gotKeys.insert(e.key_set);
      gotValues.insert(e.value);
    }
    EXPECT_TRUE(gotKeys.count({"a"}));
    EXPECT_TRUE(gotKeys.count({"a", "b"}));
    EXPECT_TRUE(gotKeys.count({"a", "c"}));
    EXPECT_TRUE(gotValues.count("UNSAT"));
    EXPECT_TRUE(gotValues.count("SAT:1"));
    EXPECT_TRUE(gotValues.count("SAT:2"));
  }

  // Miss
  {
    auto miss = disk.lookup({"missing"});
    EXPECT_FALSE(miss.has_value());
  }

  // subsets of a key not in the map returns only what is actually a subset
  {
    auto subs = disk.subsets({"x", "y"});
    ASSERT_EQ(1u, subs.size()); // only {"x"} -> "SAT:42"
    EXPECT_EQ(std::set<std::string>{"x"}, subs[0].key_set);
    EXPECT_EQ("SAT:42", subs[0].value);
  }

  std::remove(testFile.c_str());
}

TEST(DiskMapOfSetsTest, ComplexRoundTrip) {
  klee::MapOfSets<std::string, std::string> mem;
  mem.insert({}, "UNSAT_root");
  mem.insert({"a"}, "UNSAT_a");
  mem.insert({"a", "b"}, "UNSAT_ab");
  mem.insert({"a", "b", "c"}, "SAT_abc");
  mem.insert({"x"}, "SAT_x");
  mem.insert({"x", "y"}, "SAT_xy");

  const std::string testFile = "complex_disk_cache.mapo";
  klee::MapOfSetsDiskBuilder::build(mem, testFile);
  klee::mapofsets::DiskMapOfSets disk(testFile);

  // Exact lookups
  EXPECT_EQ("UNSAT_root", *disk.lookup({}));
  EXPECT_EQ("UNSAT_a",    *disk.lookup({"a"}));
  EXPECT_EQ("UNSAT_ab",   *disk.lookup({"a", "b"}));
  EXPECT_EQ("SAT_abc",    *disk.lookup({"a", "b", "c"}));
  EXPECT_EQ("SAT_x",      *disk.lookup({"x"}));
  EXPECT_EQ("SAT_xy",     *disk.lookup({"x", "y"}));

  // supersets({"a"}) → {"a"}, {"a","b"}, {"a","b","c"} — not {}
  {
    auto supers = disk.supersets({"a"});
    ASSERT_EQ(3u, supers.size());
    std::set<std::string> values;
    for (const auto &e : supers)
      values.insert(e.value);
    EXPECT_TRUE(values.count("UNSAT_a"));
    EXPECT_TRUE(values.count("UNSAT_ab"));
    EXPECT_TRUE(values.count("SAT_abc"));
    EXPECT_FALSE(values.count("UNSAT_root")); // {} is not a superset of {"a"}
  }

  // subsets({"a","b","c"}) → {}, {"a"}, {"a","b"}, {"a","b","c"}
  // find_subsets now returns ALL entries including SAT.
  // The set itself is also a subset of itself.
  {
    auto subs = disk.subsets({"a", "b", "c"});
    ASSERT_EQ(4u, subs.size());
    std::set<std::set<std::string>> gotKeys;
    std::set<std::string> gotValues;
    for (const auto &e : subs) {
      gotKeys.insert(e.key_set);
      gotValues.insert(e.value);
    }
    EXPECT_TRUE(gotKeys.count({}));
    EXPECT_TRUE(gotKeys.count({"a"}));
    EXPECT_TRUE(gotKeys.count({"a", "b"}));
    EXPECT_TRUE(gotKeys.count({"a", "b", "c"}));
    // Mix of UNSAT and SAT
    EXPECT_TRUE(gotValues.count("UNSAT_root"));
    EXPECT_TRUE(gotValues.count("UNSAT_a"));
    EXPECT_TRUE(gotValues.count("UNSAT_ab"));
    EXPECT_TRUE(gotValues.count("SAT_abc"));
  }

  // subsets({"x","y"}) → {}, {"x"}, {"x","y"} — all subsets including SAT
  {
    auto subs = disk.subsets({"x", "y"});
    ASSERT_EQ(3u, subs.size());
    std::set<std::set<std::string>> gotKeys;
    for (const auto &e : subs)
      gotKeys.insert(e.key_set);
    EXPECT_TRUE(gotKeys.count({}));
    EXPECT_TRUE(gotKeys.count({"x"}));
    EXPECT_TRUE(gotKeys.count({"x", "y"}));
  }

  // supersets({}) → everything in the map
  {
    auto supers = disk.supersets({});
    EXPECT_EQ(6u, supers.size());
  }

  // subsets of a key with no entry in the map returns only proper subsets
  {
    auto subs = disk.subsets({"a", "z"});
    // Only {} and {"a"} are subsets — {"a","z"} not in map, {"z"} not in map
    ASSERT_EQ(2u, subs.size());
    std::set<std::set<std::string>> gotKeys;
    for (const auto &e : subs)
      gotKeys.insert(e.key_set);
    EXPECT_TRUE(gotKeys.count({}));
    EXPECT_TRUE(gotKeys.count({"a"}));
  }

  // Miss
  {
    EXPECT_FALSE(disk.lookup({"missing"}).has_value());
    EXPECT_FALSE(disk.lookup({"a", "b", "c", "d"}).has_value());
  }

  std::remove(testFile.c_str());
}

TEST(DiskMapOfSetsTest, SupersetBacktracking) {
  klee::MapOfSets<std::string, std::string> mem;
  mem.insert({"b", "c"}, "SAT_bc");
  mem.insert({"c"},       "SAT_c");
  mem.insert({"c", "d"}, "SAT_cd");

  const std::string testFile = "backtrack_disk_cache.mapo";
  klee::MapOfSetsDiskBuilder::build(mem, testFile);
  klee::mapofsets::DiskMapOfSets disk(testFile);

  // supersets({"c"}) must find {"b","c"}, {"c"}, and {"c","d"} — all three
  // contain "c", so all are valid supersets.
  {
    auto supers = disk.supersets({"c"});
    ASSERT_EQ(3u, supers.size());
    std::set<std::string> values;
    for (const auto &e : supers)
      values.insert(e.value);
    EXPECT_TRUE(values.count("SAT_bc"));
    EXPECT_TRUE(values.count("SAT_c"));
    EXPECT_TRUE(values.count("SAT_cd"));
  }

  // Every returned key_set must contain "c" — this is what actually
  // exercises the backtracking correctness: accum must not be corrupted
  // when "b" is inserted and erased before we find "c".
  {
    auto supers = disk.supersets({"c"});
    for (const auto &e : supers)
      EXPECT_TRUE(e.key_set.count("c"))
          << "key_set missing 'c': accum was corrupted by backtracking";
  }

  // Sanity: {"b"} alone is not in the map and {"b"} is not a superset of {"c"}
  EXPECT_FALSE(disk.lookup({"b"}).has_value());

  std::remove(testFile.c_str());
}

// Empty set edge cases
TEST(DiskMapOfSetsTest, EmptySetEdgeCases) {
  klee::MapOfSets<std::string, std::string> mem;
  mem.insert({}, "UNSAT_empty");
  mem.insert({"a"}, "SAT_a");

  const std::string testFile = "empty_disk_cache.mapo";
  klee::MapOfSetsDiskBuilder::build(mem, testFile);
  klee::mapofsets::DiskMapOfSets disk(testFile);

  // Exact lookup of empty set
  EXPECT_EQ("UNSAT_empty", *disk.lookup({}));

  // subsets({}) → only the empty set itself
  {
    auto subs = disk.subsets({});
    ASSERT_EQ(1u, subs.size());
    EXPECT_TRUE(subs[0].key_set.empty());
  }

  // supersets({}) → everything
  {
    auto supers = disk.supersets({});
    ASSERT_EQ(2u, supers.size());
  }

  std::remove(testFile.c_str());
}

// LRU eviction test.
//
// Build with chunkSize=1 so every node is in its own chunk.  Open with
// max_cache_size=2 so the cache holds at most 2 chunks at a time.  Then drive
// a sequence of lookups that exercises both eviction and re-promotion:
//
//   lookup "a"  → load chunk(a);           cache: [a]
//   lookup "b"  → load chunk(b);           cache: [b, a]   (full)
//   lookup "a"  → hit, promote chunk(a);   cache: [a, b]
//   lookup "c"  → evict chunk(b) (LRU), load chunk(c);  cache: [c, a]
//   lookup "b"  → evict chunk(a) (LRU), load chunk(b);  cache: [b, c]
//   lookup "c"  → hit, promote chunk(c);   cache: [c, b]
//   lookup "a"  → evict chunk(b) (LRU), load chunk(a);  cache: [a, c]
//
// Every step asserts the returned value is correct.  A stale/corrupted LRU
// iterator (e.g. from the old erase-begin() code) would either crash on
// splice() or evict the wrong chunk causing a lookup failure.
TEST(DiskMapOfSetsTest, LRUEviction) {
  klee::MapOfSets<std::string, std::string> mem;
  mem.insert({"a"}, "val_a");
  mem.insert({"b"}, "val_b");
  mem.insert({"c"}, "val_c");

  const std::string testFile = "lru_disk_cache.mapo";
  // chunkSize=1: every trie node lives in its own chunk.
  klee::MapOfSetsDiskBuilder::build(mem, testFile, /*chunkSize=*/1);

  // max_cache_size=2: forces eviction after the second distinct chunk is loaded.
  klee::mapofsets::DiskMapOfSets disk(testFile, /*max_cache_size=*/2);

  // Access pattern designed to exercise promotion and LRU ordering.
  EXPECT_EQ("val_a", *disk.lookup({"a"})); // load a
  EXPECT_EQ("val_b", *disk.lookup({"b"})); // load b  (cache full)
  EXPECT_EQ("val_a", *disk.lookup({"a"})); // hit a, promote → b becomes LRU
  EXPECT_EQ("val_c", *disk.lookup({"c"})); // evict b, load c
  EXPECT_EQ("val_b", *disk.lookup({"b"})); // evict a, load b
  EXPECT_EQ("val_c", *disk.lookup({"c"})); // hit c, promote → b becomes LRU
  EXPECT_EQ("val_a", *disk.lookup({"a"})); // evict b, load a

  // Verify misses still work correctly under a hot LRU cache.
  EXPECT_FALSE(disk.lookup({"z"}).has_value());
  EXPECT_FALSE(disk.lookup({"a", "b"}).has_value()); // not inserted

  std::remove(testFile.c_str());
}

// ---------------------------------------------------------------------------
// DiskCexCache round-trip test
//
// Verifies that concrete assignment byte data survives a serialize → disk →
// deserialize cycle and that DiskCexCache::findSubset returns an Assignment
// whose bindings match what was originally stored.
//
// Setup:
//   Array "myArr" (4 bytes). Constraint: myArr[0] == 42.
//   Assignment:  myArr -> [42, 0, 0, 0]
//
// The test:
//   1. Canonicalize the constraint set to get the disk key and forwardArrayMap.
//   2. Serialize the assignment with DiskCexCache::serializeAssignment.
//   3. Build a MapOfSets with {disk_key_string -> serialized_value} and write
//      it to disk via MapOfSetsDiskBuilder.
//   4. Open the file with DiskCexCache and call findSubset on the original
//      (pre-canonicalization) constraint set.
//   5. Assert a hit is returned, the assignment satisfies the constraint, and
//      bindings[myArr][0] == 42.
// ---------------------------------------------------------------------------

// Shared ArrayCache for DiskCexCache tests (Arrays must outlive the cache).
static ArrayCache diskCexAC;

TEST(DiskCexCacheTest, AssignmentRoundTrip) {
  // --- Build the original constraint: myArr[0] == 42 ---
  const Array *myArr = diskCexAC.CreateArray("myArr", 4);

  ref<Expr> idx  = ConstantExpr::create(0, Expr::Int32);
  ref<Expr> read = ReadExpr::create(UpdateList(myArr, nullptr), idx);
  ref<Expr> c42  = ConstantExpr::create(42, Expr::Int8);
  ref<Expr> constraint = EqExpr::create(read, c42);

  std::set<ref<Expr>> constraintSet = {constraint};

  // --- Canonicalize to get the disk key and the array renaming map ---
  std::unique_ptr<ExprBuilder> builder(createDefaultExprBuilder());
  ArrayCache canonAC; // separate cache so canonical arrays live long enough
  std::vector<ref<Expr>> vec(constraintSet.begin(), constraintSet.end());
  CanonicalizationResult canon =
      canonicalizeConstraintSet(vec, *builder, canonAC);

  // Serialize each canonical constraint to produce the disk key strings.
  std::set<std::string> diskKeySet;
  for (const auto &e : canon.constraints) {
    std::string s;
    llvm::raw_string_ostream os(s);
    ExprPPrinter::printSingleExpr(os, e);
    os.flush();
    diskKeySet.insert(s);
  }

  // --- Build the assignment and serialize it ---
  // myArr -> [42, 0, 0, 0]
  std::vector<unsigned char> bytes = {42, 0, 0, 0};
  std::vector<const Array *> objs  = {myArr};
  std::vector<std::vector<unsigned char>> vals = {bytes};
  Assignment assignment(objs, vals);

  std::string serialized =
      DiskCexCache::serializeAssignment(&assignment, canon.forwardArrayMap);

  // Sanity-check the serialized format before writing to disk.
  ASSERT_EQ(0u, serialized.find("SAT_DATA:"))
      << "Serialized value must start with SAT_DATA:";
  EXPECT_NE(std::string::npos, serialized.find("A0=2a000000"))
      << "Expected canonical array A0 with bytes 2a000000";

  // --- Write disk cache ---
  klee::MapOfSets<std::string, std::string> mem;
  mem.insert(diskKeySet, serialized);

  const std::string testFile = "disk_cex_cache_roundtrip.mapo";
  klee::MapOfSetsDiskBuilder::build(mem, testFile);

  // --- Query via DiskCexCache ---
  klee::DiskCexCache cache(testFile);

  Assignment *result = nullptr;
  bool hit = cache.findSubset(constraintSet, result);

  ASSERT_TRUE(hit) << "Expected a cache hit";
  ASSERT_NE(nullptr, result) << "Expected a SAT assignment, not UNSAT";
  EXPECT_TRUE(result->satisfies(constraintSet.begin(), constraintSet.end()))
      << "Returned assignment does not satisfy the original constraint";

  // Check the concrete byte value.
  auto it = result->bindings.find(myArr);
  ASSERT_NE(result->bindings.end(), it)
      << "myArr not found in returned assignment bindings";
  ASSERT_GE(it->second.size(), 1u);
  EXPECT_EQ(42u, it->second[0])
      << "Expected myArr[0] == 42 in returned assignment";

  // A query with an extra unrelated constraint that the stored assignment
  // does NOT satisfy should be a miss (pickEntry validates with satisfies()).
  ref<Expr> c99 = ConstantExpr::create(99, Expr::Int8);
  ref<Expr> extra = EqExpr::create(read, c99); // myArr[0] == 99, contradicts 42
  std::set<ref<Expr>> stricter = {constraint, extra};
  Assignment *missResult = nullptr;
  bool miss = cache.findSubset(stricter, missResult);
  // The stored entry is a subset of stricter, but the assignment (myArr[0]=42)
  // does NOT satisfy myArr[0]==99, so pickEntry must reject it.
  EXPECT_FALSE(miss) << "Assignment satisfying myArr[0]==42 should not be "
                        "returned for a query that also requires myArr[0]==99";

  std::remove(testFile.c_str());
}

}
