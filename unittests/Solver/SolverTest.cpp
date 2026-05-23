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
#include "klee/Expr/ExprPPrinter.h"
#include "klee/Solver/ConstraintCanonicalizer.h"
#include "klee/Solver/DiskCexCache.h"
#include "klee/Solver/DiskMapOfSets.h"
#include "klee/Solver/MapOfSetsDiskBuilder.h"
#include "klee/Solver/Solver.h"
#include "klee/Solver/SolverCmdLine.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#include <fstream>
#include <iostream>
#include <iterator>

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
    std::set<std::string> values(supers.begin(), supers.end());
    EXPECT_TRUE(values.count("UNSAT"));
    EXPECT_TRUE(values.count("SAT:1"));
    EXPECT_TRUE(values.count("SAT:2"));
    EXPECT_FALSE(values.count("SAT:42")); // {"x"} is not a superset of {"a"}
  }

  // subsets({"a","b","c"}) → {"a"}, {"a","b"}, {"a","c"}
  {
    auto subs = disk.subsets({"a", "b", "c"});
    ASSERT_EQ(3u, subs.size());
    std::set<std::string> gotValues(subs.begin(), subs.end());
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
    EXPECT_EQ("SAT:42", subs[0]);
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
    std::set<std::string> values(supers.begin(), supers.end());
    EXPECT_TRUE(values.count("UNSAT_a"));
    EXPECT_TRUE(values.count("UNSAT_ab"));
    EXPECT_TRUE(values.count("SAT_abc"));
    EXPECT_FALSE(values.count("UNSAT_root")); // {} is not a superset of {"a"}
  }

  // subsets({"a","b","c"}) → {}, {"a"}, {"a","b"}, {"a","b","c"}
  // The set itself is also a subset of itself.
  {
    auto subs = disk.subsets({"a", "b", "c"});
    ASSERT_EQ(4u, subs.size());
    std::set<std::string> gotValues(subs.begin(), subs.end());
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
    std::set<std::string> gotValues(subs.begin(), subs.end());
    EXPECT_TRUE(gotValues.count("UNSAT_root"));
    EXPECT_TRUE(gotValues.count("SAT_x"));
    EXPECT_TRUE(gotValues.count("SAT_xy"));
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
    std::set<std::string> gotValues(subs.begin(), subs.end());
    EXPECT_TRUE(gotValues.count("UNSAT_root"));
    EXPECT_TRUE(gotValues.count("UNSAT_a"));
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
    std::set<std::string> values(supers.begin(), supers.end());
    EXPECT_TRUE(values.count("SAT_bc"));
    EXPECT_TRUE(values.count("SAT_c"));
    EXPECT_TRUE(values.count("SAT_cd"));
  }

  // Backtracking correctness: all three supersets of {"c"} must be found.
  // Returning the wrong count (< 3) would indicate accum corruption.
  {
    auto supers = disk.supersets({"c"});
    EXPECT_EQ(3u, supers.size())
        << "Wrong superset count — backtracking may have corrupted traversal";
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
    EXPECT_EQ("UNSAT_empty", subs[0]);
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
  klee::MapOfSetsDiskBuilder::build(mem, testFile, /*metadata=*/{}, /*chunkSize=*/1);

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
  ArrayCache canonAC; // separate cache so canonical arrays live long enough
  std::vector<ref<Expr>> vec(constraintSet.begin(), constraintSet.end());
  CanonicalizationResult canon =
      canonicalizeConstraintSet(vec, canonAC);

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

  // Sanity-check the v2 binary format: [0x01][n=1][idx=0][len=4][42,0,0,0]
  ASSERT_GE(serialized.size(), 8u) << "Serialized value too short";
  EXPECT_EQ('\x01', serialized[0]) << "Expected SAT sentinel 0x01";
  EXPECT_EQ('\x01', serialized[1]) << "Expected n_arrays=1";
  EXPECT_EQ('\x00', serialized[2]) << "Expected name_index=0 (A0)";
  uint32_t storedLen = 0;
  memcpy(&storedLen, &serialized[3], 4);
  EXPECT_EQ(4u, storedLen) << "Expected data_len=4";
  EXPECT_EQ(42u, static_cast<unsigned char>(serialized[7])) << "Expected byte[0]==42";

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

// WriteCacheRoundTrip exercises the exact write-back pipeline used by
// CexCachingSolver::writeCacheToDisk:
//   in-memory MapOfSets<ref<Expr>, Assignment*>
//     -> iterate -> canonicalize -> serialize -> MapOfSetsDiskBuilder::build
//     -> DiskCexCache::findSubset -> reconstructed Assignment
//
// Two entries are written (SAT + UNSAT) so we verify both cases.
TEST(DiskCexCacheTest, WriteCacheRoundTrip) {
  // --- Build two symbolic arrays and constraints ---
  const Array *arrX = diskCexAC.CreateArray("arrX", 4);
  const Array *arrY = diskCexAC.CreateArray("arrY", 4);

  // constraint1: arrX[0] == 7
  ref<Expr> idx0    = ConstantExpr::create(0, Expr::Int32);
  ref<Expr> readX   = ReadExpr::create(UpdateList(arrX, nullptr), idx0);
  ref<Expr> c7      = ConstantExpr::create(7, Expr::Int8);
  ref<Expr> constrX = EqExpr::create(readX, c7);

  // constraint2: arrY[0] == 0  (the one we'll mark UNSAT)
  ref<Expr> readY   = ReadExpr::create(UpdateList(arrY, nullptr), idx0);
  ref<Expr> c0      = ConstantExpr::create(0, Expr::Int8);
  ref<Expr> constrY = EqExpr::create(readY, c0);

  // Build the in-memory cache: two entries.
  // Entry A (SAT): {constrX} -> assignment arrX[0]=7
  // Entry B (UNSAT): {constrY} -> nullptr
  MapOfSets<ref<Expr>, Assignment *> inMemCache;

  std::vector<unsigned char> xBytes = {7, 0, 0, 0};
  std::vector<const Array *> satObjs = {arrX};
  std::vector<std::vector<unsigned char>> satVals = {xBytes};
  Assignment *satA = new Assignment(satObjs, satVals);
  inMemCache.insert({constrX}, satA);
  inMemCache.insert({constrY}, nullptr);

  // --- Replicate the writeCacheToDisk loop ---
  MapOfSetsDiskBuilder::UBTree diskTree;
  ArrayCache writeAC;

  for (auto it = inMemCache.begin(); it != inMemCache.end(); ++it) {
    auto [keySet, assignment] = *it;

    std::vector<ref<Expr>> vec(keySet.begin(), keySet.end());
    CanonicalizationResult canon =
        canonicalizeConstraintSet(vec, writeAC);

    std::set<std::string> diskKey;
    for (const auto &e : canon.constraints) {
      std::string s;
      llvm::raw_string_ostream os(s);
      ExprPPrinter::printSingleExpr(os, e);
      os.flush();
      diskKey.insert(s);
    }

    std::string value = assignment
                            ? DiskCexCache::serializeAssignment(
                                  assignment, canon.forwardArrayMap)
                            : ""; // empty = UNSAT sentinel in v2 binary format
    diskTree.insert(diskKey, value);
  }

  const std::string testFile = "disk_cex_cache_writeback.mapo";
  MapOfSetsDiskBuilder::build(diskTree, testFile);

  // --- Read back via DiskCexCache ---
  DiskCexCache readCache(testFile);

  // SAT entry: query {constrX} should hit and return arrX[0]==7.
  std::set<ref<Expr>> queryX = {constrX};
  Assignment *resultX = nullptr;
  ASSERT_TRUE(readCache.findSubset(queryX, resultX))
      << "Expected SAT hit for constrX query";
  ASSERT_NE(nullptr, resultX) << "Expected non-null assignment for SAT entry";
  EXPECT_TRUE(resultX->satisfies(queryX.begin(), queryX.end()))
      << "Returned assignment does not satisfy constrX";
  auto bX = resultX->bindings.find(arrX);
  ASSERT_NE(resultX->bindings.end(), bX);
  ASSERT_GE(bX->second.size(), 1u);
  EXPECT_EQ(7u, bX->second[0]) << "Expected arrX[0] == 7";

  // UNSAT entry: query {constrY} should hit and return nullptr.
  std::set<ref<Expr>> queryY = {constrY};
  Assignment *resultY = reinterpret_cast<Assignment *>(0xdeadbeef);
  ASSERT_TRUE(readCache.findSubset(queryY, resultY))
      << "Expected UNSAT hit for constrY query";
  EXPECT_EQ(nullptr, resultY) << "Expected nullptr for UNSAT entry";

  delete satA;
  std::remove(testFile.c_str());
}

// MergeRoundTrip verifies that allEntries() faithfully recovers every stored
// entry, and that the offline merge path (read existing file, add new entries,
// rebuild via MapOfSetsDiskBuilder) produces a file that contains both sets.
//
// Generation 1: {constrX -> SAT}, {constrY -> UNSAT}
// Generation 2: merge gen1 + add {constrZ -> SAT}
// Verify gen2 file has all three entries accessible via DiskCexCache.
TEST(DiskCexCacheTest, MergeRoundTrip) {
  // Reuse arrays from the shared diskCexAC (they outlive the test).
  const Array *arrX = diskCexAC.CreateArray("arrX_m", 4);
  const Array *arrY = diskCexAC.CreateArray("arrY_m", 4);
  const Array *arrZ = diskCexAC.CreateArray("arrZ_m", 4);

  ref<Expr> idx0 = ConstantExpr::create(0, Expr::Int32);
  // constrX: arrX_m[0] == 7
  ref<Expr> readX   = ReadExpr::create(UpdateList(arrX, nullptr), idx0);
  ref<Expr> constrX = EqExpr::create(readX, ConstantExpr::create(7, Expr::Int8));
  // constrY: arrY_m[0] == 0  (will be UNSAT)
  ref<Expr> readY   = ReadExpr::create(UpdateList(arrY, nullptr), idx0);
  ref<Expr> constrY = EqExpr::create(readY, ConstantExpr::create(0, Expr::Int8));
  // constrZ: arrZ_m[0] == 42  (new in gen2)
  ref<Expr> readZ   = ReadExpr::create(UpdateList(arrZ, nullptr), idx0);
  ref<Expr> constrZ = EqExpr::create(readZ, ConstantExpr::create(42, Expr::Int8));

  const std::string gen1File = "merge_round_trip_gen1.mapo";
  const std::string gen2File = "merge_round_trip_gen2.mapo";

  // --- Build gen1 ---
  {
    MapOfSetsDiskBuilder::UBTree tree;
    ArrayCache ac;

    auto addEntry = [&](const std::set<ref<Expr>> &constraints, Assignment *a) {
      std::vector<ref<Expr>> vec(constraints.begin(), constraints.end());
      CanonicalizationResult canon = canonicalizeConstraintSet(vec, ac);
      std::set<std::string> diskKey;
      for (const auto &e : canon.constraints) {
        std::string s; llvm::raw_string_ostream os(s);
        ExprPPrinter::printSingleExpr(os, e); os.flush();
        diskKey.insert(s);
      }
      std::string val = a ? DiskCexCache::serializeAssignment(a, canon.forwardArrayMap)
                          : ""; // empty = UNSAT sentinel in v2 binary format
      tree.insert(diskKey, val);
    };

    std::vector<const Array *> xObjs = {arrX};
    std::vector<std::vector<unsigned char>> xVals = {{7,0,0,0}};
    Assignment satX(xObjs, xVals);
    addEntry({constrX}, &satX);
    addEntry({constrY}, nullptr);
    MapOfSetsDiskBuilder::build(tree, gen1File);
  }

  // --- Verify allEntries() recovers both gen1 entries ---
  {
    klee::mapofsets::DiskMapOfSets disk(gen1File);
    ASSERT_TRUE(disk.isValid());
    auto entries = disk.allEntries();
    EXPECT_EQ(2u, entries.size()) << "allEntries() should return 2 gen1 entries";
    // Check we have one UNSAT (empty value) and one SAT (0x01-prefixed) entry
    int unsatCount = 0, satCount = 0;
    for (const auto &e : entries) {
      if (e.value.empty()) ++unsatCount;
      else if (!e.value.empty() && static_cast<unsigned char>(e.value[0]) == 0x01) ++satCount;
    }
    EXPECT_EQ(1, unsatCount) << "Expected 1 UNSAT entry";
    EXPECT_EQ(1, satCount)   << "Expected 1 SAT entry";
  }

  // --- Build gen2: merge gen1 + add constrZ ---
  {
    MapOfSetsDiskBuilder::UBTree tree;
    ArrayCache ac;

    // Seed from gen1 (simulates the merge-on-write constructor logic)
    klee::mapofsets::DiskMapOfSets existing(gen1File);
    ASSERT_TRUE(existing.isValid());
    for (auto &e : existing.allEntries())
      tree.insert(e.key_set, e.value);

    // Add new gen2 entry
    {
      std::vector<ref<Expr>> vec = {constrZ};
      CanonicalizationResult canon = canonicalizeConstraintSet(vec, ac);
      std::set<std::string> diskKey;
      for (const auto &e : canon.constraints) {
        std::string s; llvm::raw_string_ostream os(s);
        ExprPPrinter::printSingleExpr(os, e); os.flush();
        diskKey.insert(s);
      }
      std::vector<const Array *> zObjs = {arrZ};
      std::vector<std::vector<unsigned char>> zVals = {{42,0,0,0}};
      Assignment satZ(zObjs, zVals);
      tree.insert(diskKey,
                  DiskCexCache::serializeAssignment(&satZ, canon.forwardArrayMap));
    }
    MapOfSetsDiskBuilder::build(tree, gen2File);
  }

  // --- Verify gen2 has all three entries ---
  {
    klee::mapofsets::DiskMapOfSets disk(gen2File);
    ASSERT_TRUE(disk.isValid());
    EXPECT_EQ(3u, disk.allEntries().size())
        << "gen2 should contain 3 merged entries";
  }

  // --- Verify gen2 entries are queryable via DiskCexCache ---
  DiskCexCache gen2Cache(gen2File);

  std::set<ref<Expr>> qX = {constrX};
  std::set<ref<Expr>> qY = {constrY};
  std::set<ref<Expr>> qZ = {constrZ};

  Assignment *res = nullptr;
  ASSERT_TRUE(gen2Cache.findSubset(qX, res)) << "constrX should hit";
  ASSERT_NE(nullptr, res);
  EXPECT_TRUE(res->satisfies(qX.begin(), qX.end()));

  res = reinterpret_cast<Assignment *>(0xdeadbeef);
  ASSERT_TRUE(gen2Cache.findSubset(qY, res)) << "constrY should hit (UNSAT)";
  EXPECT_EQ(nullptr, res);

  res = nullptr;
  ASSERT_TRUE(gen2Cache.findSubset(qZ, res)) << "constrZ should hit (gen2 entry)";
  ASSERT_NE(nullptr, res);
  EXPECT_TRUE(res->satisfies(qZ.begin(), qZ.end()));

  std::remove(gen1File.c_str());
  std::remove(gen2File.c_str());
}

// ---------------------------------------------------------------------------
// Constraint canonicalizer tests
//
// canonicalizeExprTree / buildConstraintDiskKey were only exercised indirectly
// (through the DiskCexCache round-trips above). These cover the canonicaliser
// directly: every kid-bearing Expr kind must rebuild, and equivalent constraint
// sets must collapse to one key.
// ---------------------------------------------------------------------------

// Canonical arrays are created into the passed ArrayCache and referenced by the
// result expressions, so the cache must outlive them.
static ArrayCache canonAC;

static ref<Expr> readByte0(const Array *a) {
  return ReadExpr::create(UpdateList(a, nullptr),
                          ConstantExpr::create(0, Expr::Int32));
}

// Regression: NotExpr and NotOptimizedExpr both carry kids but were missing
// from rebuildWithKids' switch, so canonicalising any expression containing
// them hit llvm_unreachable (abort with assertions on, UB otherwise). They
// reach the canonicaliser on the live query path via the negated query
// expression and klee_assume optimisation barriers.
TEST(CanonicalizerTest, NotAndNotOptimizedDoNotCrash) {
  const Array *a = canonAC.CreateArray("not_a", 4);
  ref<Expr> r0 = readByte0(a);
  ref<Expr> r1 = ReadExpr::create(UpdateList(a, nullptr),
                                  ConstantExpr::create(1, Expr::Int32));

  // Symbolic operands, so create() yields real Not / NotOptimized nodes
  // (NotExpr::create only folds constants; NotOptimizedExpr::create never folds).
  ref<Expr> notE =
      NotExpr::create(EqExpr::create(r0, ConstantExpr::create(0, Expr::Int8)));
  ASSERT_EQ(Expr::Not, notE->getKind()) << "test needs a real NotExpr";

  ref<Expr> noE = NotOptimizedExpr::create(
      EqExpr::create(r1, ConstantExpr::create(5, Expr::Int8)));
  ASSERT_EQ(Expr::NotOptimized, noE->getKind())
      << "test needs a real NotOptimizedExpr";

  // Pre-fix, each of these aborts inside rebuildWithKids.
  ref<Expr> cNot = canonicalizeExprTree(notE);
  ASSERT_FALSE(cNot.isNull());
  EXPECT_EQ(Expr::Not, cNot->getKind()) << "NotExpr must be rebuilt, not dropped";

  ref<Expr> cNo = canonicalizeExprTree(noE);
  ASSERT_FALSE(cNo.isNull());
  EXPECT_EQ(Expr::NotOptimized, cNo->getKind())
      << "NotOptimizedExpr must be rebuilt, not dropped";

  // The full key-building pipeline must also survive and emit one string each.
  std::vector<ref<Expr>> vec = {notE, noE};
  auto [key, canon] = buildConstraintDiskKey(vec, canonAC);
  EXPECT_EQ(2u, key.size());
}

// (a+b)+c and c+(b+a) are equal modulo commutativity and associativity. The
// base ExprBuilder does not reassociate or sort symbolic Add operands
// (AddExpr_create only pulls out constants), so unifying these is the
// canonicaliser's job (flattenAndRebuildAssoc).
TEST(CanonicalizerTest, CommutativeAssociativeSameKey) {
  ref<Expr> ra = readByte0(canonAC.CreateArray("comm_a", 4));
  ref<Expr> rb = readByte0(canonAC.CreateArray("comm_b", 4));
  ref<Expr> rc = readByte0(canonAC.CreateArray("comm_c", 4));
  ref<Expr> zero = ConstantExpr::create(0, Expr::Int8);

  ref<Expr> e1 = EqExpr::create(
      AddExpr::create(AddExpr::create(ra, rb), rc), zero); // (a+b)+c == 0
  ref<Expr> e2 = EqExpr::create(
      AddExpr::create(rc, AddExpr::create(rb, ra)), zero); // c+(b+a) == 0

  std::vector<ref<Expr>> v1 = {e1}, v2 = {e2};
  auto k1 = buildConstraintDiskKey(v1, canonAC).first;
  auto k2 = buildConstraintDiskKey(v2, canonAC).first;
  EXPECT_EQ(k1, k2)
      << "commutative + associative reshaping must canonicalize identically";
}

// Structurally isomorphic constraint sets -- same shape, different arrays,
// supplied in a different order -- must produce identical keys: alpha-renaming
// to A0,A1,... plus order-independent constraint sorting. Distinct constants
// pin the canonical array numbering so the renaming is deterministic.
TEST(CanonicalizerTest, RenamingAndOrderIndependence) {
  ref<Expr> one = ConstantExpr::create(1, Expr::Int8);
  ref<Expr> two = ConstantExpr::create(2, Expr::Int8);

  const Array *p = canonAC.CreateArray("ren_p", 4);
  const Array *q = canonAC.CreateArray("ren_q", 4);
  std::vector<ref<Expr>> setA = {EqExpr::create(readByte0(p), one),
                                 EqExpr::create(readByte0(q), two)};
  std::vector<ref<Expr>> setB = {setA[1], setA[0]}; // reversed input order

  const Array *r = canonAC.CreateArray("ren_r", 4);
  const Array *s = canonAC.CreateArray("ren_s", 4);
  std::vector<ref<Expr>> setC = {EqExpr::create(readByte0(r), one),
                                 EqExpr::create(readByte0(s), two)};

  auto kA = buildConstraintDiskKey(setA, canonAC).first;
  auto kB = buildConstraintDiskKey(setB, canonAC).first;
  auto kC = buildConstraintDiskKey(setC, canonAC).first;

  EXPECT_EQ(kA, kB) << "input ordering must not affect the canonical key";
  EXPECT_EQ(kA, kC) << "array identity must not affect the canonical key";
  EXPECT_EQ(2u, kA.size()) << "the two distinct constraints must stay distinct";
}

// ---------------------------------------------------------------------------
// Malformed-file / robustness tests for DiskMapOfSets and DiskCexCache.
// ---------------------------------------------------------------------------

// Patch a built cache file so node 1's first child points back to node 1,
// forming a child_id cycle the tree builder can never produce. The new
// child_id (1) and the original (2) are both single-byte non-zero varints, so
// the chunk's serialized size is unchanged and the directory offsets stay
// valid. Returns false if anything about that assumption does not hold.
static bool patchSelfLoopOnNode1(const std::string &file) {
  std::ifstream in(file, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  in.close();
  if (data.size() < 16)
    return false;

  uint64_t headerSize = 0;
  memcpy(&headerSize, data.data() + 8, 8);

  klee::mapofsets::MapOfSetsFile mf;
  if (!mf.ParseFromArray(data.data() + 16, static_cast<int>(headerSize)))
    return false;
  uint64_t dirOff = mf.header().directory_offset();
  if (dirOff + 12 > data.size())
    return false;

  uint64_t chunkOff = 0;
  uint32_t chunkSz = 0;
  memcpy(&chunkOff, data.data() + dirOff, 8);
  memcpy(&chunkSz, data.data() + dirOff + 8, 4);
  if (chunkOff + chunkSz > data.size())
    return false;

  klee::mapofsets::NodeChunk nc;
  if (!nc.ParseFromArray(data.data() + chunkOff, static_cast<int>(chunkSz)))
    return false;
  if (nc.nodes_size() < 2 || nc.nodes(1).children_size() < 1)
    return false;

  nc.mutable_nodes(1)->mutable_children(0)->set_child_id(1); // self-loop

  std::string patched;
  nc.SerializeToString(&patched);
  if (patched.size() != chunkSz)
    return false; // size drift would corrupt downstream offsets

  data.replace(chunkOff, chunkSz, patched);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return true;
}

// A child_id cycle in a corrupt file must be detected, not infinitely recursed.
// find_supersets' empty-query / extra-element branches recurse without
// shrinking the query, so they rely on the visited guard to terminate.
TEST(DiskMapOfSetsTest, SupersetCycleGuard) {
  klee::MapOfSets<std::string, std::string> mem;
  mem.insert({"a", "b"}, "SAT_ab");
  mem.insert({"a", "c"}, "SAT_ac");

  const std::string testFile = "cycle_disk_cache.mapo";
  klee::MapOfSetsDiskBuilder::build(mem, testFile);

  // The intact file answers supersets({"a"}) with both stored sets.
  {
    klee::mapofsets::DiskMapOfSets ok(testFile);
    ASSERT_TRUE(ok.isValid());
    EXPECT_EQ(2u, ok.supersets({"a"}).size());
  }

  ASSERT_TRUE(patchSelfLoopOnNode1(testFile))
      << "failed to inject a child_id cycle into the cache file";

  klee::mapofsets::DiskMapOfSets disk(testFile);
  ASSERT_TRUE(disk.isValid()); // constructor does not traverse nodes
  disk.supersets({"a"});       // must terminate, not stack-overflow
  EXPECT_FALSE(disk.isValid())
      << "a child_id cycle must invalidate the cache, not loop forever";

  std::remove(testFile.c_str());
}

// A max_cache_size of 0 must be clamped to >= 1: otherwise the first get_chunk
// evicts from an empty LRU list (back()/pop_back() on an empty std::list = UB).
// chunkSize=1 forces a fresh chunk load (and an eviction) on every lookup.
TEST(DiskMapOfSetsTest, ZeroLruCacheSizeClamped) {
  klee::MapOfSets<std::string, std::string> mem;
  mem.insert({"a"}, "val_a");
  mem.insert({"a", "b"}, "val_ab");

  const std::string testFile = "zero_lru_disk_cache.mapo";
  klee::MapOfSetsDiskBuilder::build(mem, testFile, /*metadata=*/{},
                                    /*chunkSize=*/1);

  klee::mapofsets::DiskMapOfSets disk(testFile, /*max_cache_size=*/0);
  EXPECT_EQ("val_a", *disk.lookup({"a"}));
  EXPECT_EQ("val_ab", *disk.lookup({"a", "b"}));
  EXPECT_EQ(2u, disk.supersets({"a"}).size()); // repeated load+evict cycles

  std::remove(testFile.c_str());
}

// The canonical key does not encode array size, so a stored witness can be
// wider than the live array when their key strings collide. The returned
// assignment must be resized to exactly the live array's size (truncate/
// zero-pad) and re-verified -- handing back the stored width would trip
// IndependentSolver's byte-count assertion.
TEST(DiskCexCacheTest, ResizesWitnessToLiveArraySize) {
  auto keyOf = [](const CanonicalizationResult &c) {
    std::set<std::string> k;
    for (const auto &e : c.constraints) {
      std::string s;
      llvm::raw_string_ostream os(s);
      ExprPPrinter::printSingleExpr(os, e);
      os.flush();
      k.insert(s);
    }
    return k;
  };

  // Store a witness for an 8-byte array: big[0] == 42.
  const Array *big = diskCexAC.CreateArray("mm_big", 8);
  ref<Expr> readBig = ReadExpr::create(
      UpdateList(big, nullptr), ConstantExpr::create(0, Expr::Int32));
  ref<Expr> cBig = EqExpr::create(readBig, ConstantExpr::create(42, Expr::Int8));

  ArrayCache canonBig;
  std::vector<ref<Expr>> vbig = {cBig};
  CanonicalizationResult canon = canonicalizeConstraintSet(vbig, canonBig);
  std::set<std::string> diskKey = keyOf(canon);

  std::vector<unsigned char> bigBytes(8, 0);
  bigBytes[0] = 42;
  std::vector<const Array *> objs = {big};
  std::vector<std::vector<unsigned char>> vals = {bigBytes};
  Assignment a(objs, vals);
  std::string value =
      DiskCexCache::serializeAssignment(&a, canon.forwardArrayMap);

  klee::MapOfSets<std::string, std::string> mem;
  mem.insert(diskKey, value);
  const std::string testFile = "size_mismatch.mapo";
  klee::MapOfSetsDiskBuilder::build(mem, testFile);

  // Query with a 4-byte array, same constraint shape.
  const Array *small = diskCexAC.CreateArray("mm_small", 4);
  ref<Expr> readSmall = ReadExpr::create(
      UpdateList(small, nullptr), ConstantExpr::create(0, Expr::Int32));
  ref<Expr> cSmall =
      EqExpr::create(readSmall, ConstantExpr::create(42, Expr::Int8));

  // Setup invariant: the canonical keys really do collide (size is not in the
  // key), so the miss below is attributable to the size guard, not a key miss.
  ArrayCache canonSmall;
  std::vector<ref<Expr>> vsmall = {cSmall};
  ASSERT_EQ(diskKey, keyOf(canonicalizeConstraintSet(vsmall, canonSmall)))
      << "test setup: keys must collide for this to exercise the size guard";

  DiskCexCache cache(testFile);

  // The 8-byte witness truncates to [42,0,0,0], which satisfies small[0]==42.
  std::set<ref<Expr>> qSmall = {cSmall};
  Assignment *res = nullptr;
  ASSERT_TRUE(cache.findSubset(qSmall, res))
      << "resized witness should still satisfy small[0]==42";
  ASSERT_NE(nullptr, res);
  auto it = res->bindings.find(small);
  ASSERT_NE(res->bindings.end(), it);
  EXPECT_EQ(4u, it->second.size())
      << "witness must be resized to the live array's size, not the stored 8";
  EXPECT_EQ(42u, it->second[0]);
  EXPECT_TRUE(res->satisfies(qSmall.begin(), qSmall.end()));

  std::remove(testFile.c_str());
}

}
