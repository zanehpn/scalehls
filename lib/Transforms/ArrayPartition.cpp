//===------------------------------------------------------------*- C++ -*-===//
//
//===----------------------------------------------------------------------===//

#include "Analysis/Utils.h"
#include "Transforms/Passes.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"

using namespace std;
using namespace mlir;
using namespace scalehls;
using namespace hlscpp;

namespace {
struct ArrayPartition : public ArrayPartitionBase<ArrayPartition> {
  void runOnOperation() override;
};
} // namespace

static mlir::AffineForOp getPipelineLoop(mlir::AffineForOp root) {
  SmallVector<mlir::AffineForOp, 4> nestedLoops;
  root.walk([&](mlir::AffineForOp loop) {
    if (auto attr = loop.getAttrOfType<BoolAttr>("pipeline")) {
      if (attr.getValue())
        nestedLoops.push_back(loop);
    }
  });
  if (nestedLoops.empty())
    return nullptr;
  else
    return nestedLoops.back();
}

template <typename OpType>
static void applyArrayPartition(MemAccessesMap &map, OpBuilder &builder) {
  for (auto pair : map) {
    auto memref = pair.first;
    auto memrefType = memref.getType().cast<MemRefType>();
    auto loadStores = pair.second;

    // Walk through each dimension of the targeted array.
    SmallVector<AffineExpr, 4> partitionIndices;
    SmallVector<AffineExpr, 4> addressIndices;

    for (unsigned dim = 0; dim < memrefType.getRank(); ++dim) {
      // Collect all array access indices of the current dimension.
      SmallVector<AffineExpr, 4> indices;
      for (auto accessOp : loadStores) {
        auto concreteOp = cast<OpType>(accessOp);
        auto index = concreteOp.getAffineMap().getResult(dim);
        // Only add unique index.
        if (std::find(indices.begin(), indices.end(), index) == indices.end())
          indices.push_back(index);
      }
      auto accessNum = indices.size();

      // Find the max array access distance in the current block.
      unsigned maxDistance = 0;

      for (unsigned i = 0; i < accessNum; ++i) {
        for (unsigned j = i + 1; j < accessNum; ++j) {
          // TODO: this expression can't be simplified.
          auto expr = indices[j] - indices[i];

          if (auto constDistance = expr.dyn_cast<AffineConstantExpr>()) {
            unsigned distance = abs(constDistance.getValue());
            maxDistance = max(maxDistance, distance);
          }
        }
      }

      // Determine array partition strategy.
      maxDistance += 1;
      if (maxDistance == 1) {
        // This means all accesses have the same index, and this dimension
        // should not be partitioned.
        partitionIndices.push_back(builder.getAffineConstantExpr(0));
        addressIndices.push_back(builder.getAffineDimExpr(dim));

      } else if (accessNum >= maxDistance) {
        // This means some elements are accessed more than once or exactly
        // once, and successive elements are accessed. In most cases,
        // apply "cyclic" partition should be the best solution.
        unsigned factor = maxDistance;

        partitionIndices.push_back(builder.getAffineDimExpr(dim) % factor);
        addressIndices.push_back(
            builder.getAffineDimExpr(dim).floorDiv(factor));

      } else {
        // This means discrete elements are accessed. Typically, "block"
        // partition will be most benefit for this occasion.
        unsigned factor = accessNum;

        auto blockFactor = (memrefType.getShape()[dim] + factor - 1) / factor;
        partitionIndices.push_back(
            builder.getAffineDimExpr(dim).floorDiv(blockFactor));
        addressIndices.push_back(builder.getAffineDimExpr(dim) % blockFactor);
      }
    }

    // Construct new layout map.
    partitionIndices.append(addressIndices.begin(), addressIndices.end());
    auto layoutMap = AffineMap::get(memrefType.getRank(), 0, partitionIndices,
                                    builder.getContext());

    // Construct new memref type.
    auto newType =
        MemRefType::get(memrefType.getShape(), memrefType.getElementType(),
                        layoutMap, memrefType.getMemorySpace());

    // Set new type.
    memref.setType(newType);
  }
}

void ArrayPartition::runOnOperation() {
  auto func = getOperation();
  auto builder = OpBuilder(func);

  // Apply array partition.
  for (auto forOp : func.getOps<mlir::AffineForOp>()) {
    // TODO: support imperfect loop.
    if (auto outermost = getPipelineLoop(forOp)) {
      // Collect memory access information.
      MemAccessesMap loadMap;
      outermost.walk([&](mlir::AffineLoadOp loadOp) {
        loadMap[loadOp.getMemRef()].push_back(loadOp);
      });

      MemAccessesMap storeMap;
      outermost.walk([&](mlir::AffineStoreOp storeOp) {
        storeMap[storeOp.getMemRef()].push_back(storeOp);
      });

      // Apply array partition pragma.
      // TODO: how to decide which to pick?
      applyArrayPartition<mlir::AffineLoadOp>(loadMap, builder);
      applyArrayPartition<mlir::AffineStoreOp>(storeMap, builder);
    }
  }

  // Align function type with entry block argument types.
  auto resultTypes = func.front().getTerminator()->getOperandTypes();
  auto inputTypes = func.front().getArgumentTypes();
  func.setType(builder.getFunctionType(inputTypes, resultTypes));
}

std::unique_ptr<mlir::Pass> scalehls::createArrayPartitionPass() {
  return std::make_unique<ArrayPartition>();
}
