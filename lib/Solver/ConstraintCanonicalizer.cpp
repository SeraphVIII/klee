#include "klee/Solver/ConstraintCanonicalizer.h"

#include "klee/Expr/ArrayExprVisitor.h"
#include "klee/Expr/ArrayCache.h"
#include "klee/Expr/ExprPPrinter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SHA1.h"

#include <algorithm>

using namespace klee;

using namespace klee;

///===----------------------------------------------------------------------===///
/// ExprCanonicalOrder
///===----------------------------------------------------------------------===///

bool ExprCanonicalOrder::operator()(const ref<Expr> &a,
                                    const ref<Expr> &b) const {
  if (a.get() == b.get())
    return false;

  if (a->getKind() != b->getKind())
    return a->getKind() < b->getKind();

  if (a->getWidth() != b->getWidth())
    return a->getWidth() < b->getWidth();

  unsigned ak = a->getNumKids();
  unsigned bk = b->getNumKids();
  if (ak != bk)
    return ak < bk;

  for (unsigned i = 0; i < ak; ++i) {
    const ref<Expr> &akid = a->getKid(i);
    const ref<Expr> &bkid = b->getKid(i);
    if (operator()(akid, bkid))
      return true;
    if (operator()(bkid, akid))
      return false;
  }

  return a->hash() < b->hash();
}

///===----------------------------------------------------------------------===///
/// Alpha-renaming support
///===----------------------------------------------------------------------===///

/// Visitor that assigns deterministic positions to Arrays based on first
/// appearance during DFS.
namespace {

class ArrayOrderCollector : public ExprVisitor {
  std::map<const Array *, unsigned> &order_;
  unsigned &nextIndex_;

public:
  ArrayOrderCollector(std::map<const Array *, unsigned> &order,
                      unsigned &nextIndex)
      : ExprVisitor(true), order_(order), nextIndex_(nextIndex) {}

  Action visitRead(const ReadExpr &re) override {
    const Array *root = re.updates.root;
    if (root && !order_.count(root))
      order_[root] = nextIndex_++;
    return ExprVisitor::visitRead(re);
  }
};

class ArraySubstitutionVisitor : public ExprVisitor {
  const std::map<const Array *, const Array *> &subst_;

public:
  ArraySubstitutionVisitor(
      const std::map<const Array *, const Array *> &subst)
      : ExprVisitor(true), subst_(subst) {}

  Action visitRead(const ReadExpr &re) override {
    const UpdateList &ul = re.updates;
    const Array *root = ul.root;
    auto it = subst_.find(root);
    if (it == subst_.end())
      return ExprVisitor::visitRead(re);

    const Array *newRoot = it->second;
    UpdateList newUL(newRoot, ul.head);

    ref<Expr> newIndex = re.index;
    newIndex = visit(newIndex);

    ref<Expr> n = ReadExpr::create(newUL, newIndex);
    return Action::changeTo(n);
  }
};

} // anonymous namespace

///===----------------------------------------------------------------------===///
/// Expression tree canonicalization
///===----------------------------------------------------------------------===///
static bool isCommutativeKind(Expr::Kind k) {
  switch (k) {
  case Expr::Add:
  case Expr::And:
  case Expr::Or:
  case Expr::Xor:
  case Expr::Mul:
  case Expr::Eq:
    return true;
  default:
    return false;
  }
}

static ref<Expr> rebuildWithKids(const ref<Expr> &orig,
                                 const std::vector<ref<Expr>> &kids) {
  Expr::Kind k = orig->getKind();
  switch (k) {
  case Expr::Add: return AddExpr::create(kids[0], kids[1]);
  case Expr::And: return AndExpr::create(kids[0], kids[1]);
  case Expr::Or:  return OrExpr::create(kids[0], kids[1]);
  case Expr::Xor: return XorExpr::create(kids[0], kids[1]);
  case Expr::Mul: return MulExpr::create(kids[0], kids[1]);
  case Expr::Eq:  return EqExpr::create(kids[0], kids[1]);
  default:
    return orig;
  }
}

/// Forward declaration
klee::ref<Expr> canonicalizeExprTree(klee::ref<Expr> e);

static ref<Expr> flattenAndRebuildAssoc(ref<Expr> e) {
  if (!isCommutativeKind(e->getKind()))
    return e;

  Expr::Kind k = e->getKind();
  ExprCanonicalOrder cmp;

  std::vector<ref<Expr>> elems;
  std::vector<ref<Expr>> stack;
  stack.push_back(e);

  while (!stack.empty()) {
    ref<Expr> cur = stack.back();
    stack.pop_back();
    if (cur->getKind() == k && cur->getNumKids() == 2) {
      stack.push_back(cur->getKid(0));
      stack.push_back(cur->getKid(1));
    } else {
      elems.push_back(cur);
    }
  }

  for (auto &c : elems)
    c = klee::canonicalizeExprTree(c);  // <-- add `klee::` here

  std::sort(elems.begin(), elems.end(), cmp);

  while (elems.size() > 1) {
    std::vector<ref<Expr>> next;
    for (size_t i = 0; i + 1 < elems.size(); i += 2) {
      ref<Expr> a = elems[i];
      ref<Expr> b = elems[i + 1];
      next.push_back(rebuildWithKids(elems[i], {a, b}));
    }
    if (elems.size() & 1)
      next.push_back(elems.back());
    elems.swap(next);
  }

  return elems[0];
}

ref<Expr> canonicalizeExprTree(ref<Expr> e) {
  if (e.isNull())
    return e;

  unsigned n = e->getNumKids();
  if (n == 0)
    return e;

  std::vector<ref<Expr>> kids;
  kids.reserve(n);
  for (unsigned i = 0; i < n; ++i) {
    ref<Expr> child = e->getKid(i);
    kids.push_back(klee::canonicalizeExprTree(child)); // fully qualified
  }

  if (isCommutativeKind(e->getKind())) {
    ref<Expr> tmp = rebuildWithKids(e, kids);
    return flattenAndRebuildAssoc(tmp);
  }

  return rebuildWithKids(e, kids);
}

///===----------------------------------------------------------------------===
/// Constraint set canonicalization
///===----------------------------------------------------------------------===///

CanonicalizationResult
canonicalizeConstraintSet(const std::vector<ref<Expr>> &constraints,
                          ExprBuilder &builder) {
  CanonicalizationResult res;
  res.constraints = constraints;

  ExprCanonicalOrder cmp;
  std::sort(res.constraints.begin(), res.constraints.end(), cmp);

  std::map<const Array *, unsigned> order;
  unsigned nextIndex = 0;
  ArrayOrderCollector collector(order, nextIndex);
  for (auto &e : res.constraints)
    collector.visit(e);

  ArrayCache arrayCache; // default ctor
  for (auto &kv : order) {
    const Array *orig = kv.first;
    unsigned pos = kv.second;
    std::string canonName = "A" + llvm::utostr(pos);

    const ref<ConstantExpr> *cbegin = nullptr;
    const ref<ConstantExpr> *cend   = nullptr;
    if (!orig->constantValues.empty()) {
      cbegin = &orig->constantValues[0];
      cend   = cbegin + orig->constantValues.size();
    }

    const Array *canon =
        arrayCache.CreateArray(canonName, orig->size,
                               cbegin, cend,
                               orig->domain, orig->range);

    res.forwardArrayMap[orig] = canon;
    res.inverseArrayMap[canon] = orig;
  }

  ArraySubstitutionVisitor subst(res.forwardArrayMap);
  for (auto &e : res.constraints)
    e = subst.visit(e);

  for (auto &e : res.constraints)
    e = klee::canonicalizeExprTree(e);

  std::sort(res.constraints.begin(), res.constraints.end(), cmp);

  return res;
}

///===----------------------------------------------------------------------===
/// Serialization & key computation
///===----------------------------------------------------------------------===///

std::string serializeCanonicalConstraints(
    const std::vector<ref<Expr>> &canonConstraints) {
  std::string out;
  llvm::raw_string_ostream os(out);

  for (auto &e : canonConstraints) {
    ExprPPrinter::printSingleExpr(os, e);
    os << '\n';
  }
  os.flush();
  return out;
}

std::string computeCanonicalKey(
    const std::vector<ref<Expr>> &canonConstraints) {
  std::string buf =
      klee::serializeCanonicalConstraints(canonConstraints);

  llvm::SHA1 hash;
  hash.update(buf);
  auto digest = hash.final();

  std::string hex;
  llvm::raw_string_ostream os(hex);
  for (auto b : digest)
    os.write_hex(static_cast<unsigned char>(b));
  os.flush();
  return hex;
}