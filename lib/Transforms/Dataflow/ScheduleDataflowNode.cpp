//===----------------------------------------------------------------------===//
//
// Copyright 2020-2021 The ScaleHLS Authors.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/Dominance.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "scalehls/Transforms/Passes.h"
#include "scalehls/Transforms/Utils.h"

using namespace mlir;
using namespace scalehls;
using namespace hls;

namespace {
struct ALAPScheduleNode : public OpRewritePattern<NodeOp> {
  ALAPScheduleNode(MLIRContext *context, bool ignoreViolations)
      : OpRewritePattern<NodeOp>(context), ignoreViolations(ignoreViolations) {}

  LogicalResult matchAndRewrite(NodeOp node,
                                PatternRewriter &rewriter) const override {
    if (node.getLevel())
      return failure();

    DominanceInfo domInfo;
    unsigned level = 0;
    for (auto output : node.getOutputs()) {
      // Stop to schedule the node if an internal buffer has multi-producer or
      // multi-consumer violation.
      if (!ignoreViolations)
        if (getDependentConsumers(output, node).size() > 1 ||
            getProducers(output).size() > 1)
          return failure();

      for (auto consumer : getDependentConsumers(output, node)) {
        if (!consumer.getLevel())
          return failure();
        level = std::max(level, consumer.getLevel().value() + 1);
      }
    }
    node.setLevelAttr(rewriter.getI32IntegerAttr(level));
    return success();
  }

private:
  bool ignoreViolations;
};
} // namespace

namespace {
struct ScheduleDataflowNode
    : public ScheduleDataflowNodeBase<ScheduleDataflowNode> {
  ScheduleDataflowNode() = default;
  explicit ScheduleDataflowNode(bool argIgnoreViolations) {
    ignoreViolations = argIgnoreViolations;
  }

  void runOnOperation() override {
    auto func = getOperation();
    auto context = func.getContext();

    mlir::RewritePatternSet patterns(context);
    patterns.add<ALAPScheduleNode>(context, ignoreViolations.getValue());
    (void)applyPatternsAndFoldGreedily(func, std::move(patterns));

    // If we considered multi-consumer and producer violations in this pass, set
    // schedule ops as legal if applicable.
    if (!ignoreViolations.getValue())
      func.walk([&](ScheduleOp schedule) {
        if (llvm::all_of(schedule.getOps<NodeOp>(),
                         [](NodeOp node) { return node.getLevel(); }))
          schedule.setIsLegalAttr(UnitAttr::get(context));
      });
  }
};
} // namespace

std::unique_ptr<Pass>
scalehls::createScheduleDataflowNodePass(bool ignoreViolations) {
  return std::make_unique<ScheduleDataflowNode>(ignoreViolations);
}
