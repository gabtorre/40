/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/abt/named_slots.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/exchange.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/spool.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

static sbe::EExpression::Vector toInlinedVector(
    std::vector<std::unique_ptr<sbe::EExpression>> args) {
    sbe::EExpression::Vector inlined;
    for (auto&& arg : args) {
        inlined.emplace_back(std::move(arg));
    }
    return inlined;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::optimize(const ABT& n) {
    return algebra::transport<false>(n, *this);
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(const Constant& c) {
    auto [tag, val] = c.get();
    auto [copyTag, copyVal] = sbe::value::copyValue(tag, val);
    sbe::value::ValueGuard guard(copyTag, copyVal);

    auto result = sbe::makeE<sbe::EConstant>(copyTag, copyVal);

    guard.reset();
    return result;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(const Source&) {
    tasserted(6624202, "not yet implemented");
    return nullptr;
}

void SBEExpressionLowering::prepare(const Let& let) {
    // Assign a frame ID for the local variable bound by this Let expression.
    _letMap[&let] = ++_frameCounter;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const Let& let, std::unique_ptr<sbe::EExpression> bind, std::unique_ptr<sbe::EExpression> in) {
    auto it = _letMap.find(&let);
    tassert(6624206, "incorrect let map", it != _letMap.end());
    auto frameId = it->second;
    _letMap.erase(it);

    // ABT let binds only a single variable. When we extend it to support multiple binds then we
    // have to revisit how we map variable names to sbe slot ids.
    return sbe::makeE<sbe::ELocalBind>(frameId, sbe::makeEs(std::move(bind)), std::move(in));
}

void SBEExpressionLowering::prepare(const LambdaAbstraction& lam) {
    // Assign a frame ID for the local variable bound by this LambdaAbstraction.
    _lambdaMap[&lam] = ++_frameCounter;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const LambdaAbstraction& lam, std::unique_ptr<sbe::EExpression> body) {
    auto it = _lambdaMap.find(&lam);
    tassert(6624207, "incorrect lambda map", it != _lambdaMap.end());
    auto frameId = it->second;
    _lambdaMap.erase(it);

    return sbe::makeE<sbe::ELocalLambda>(frameId, std::move(body));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const LambdaApplication&,
    std::unique_ptr<sbe::EExpression> lam,
    std::unique_ptr<sbe::EExpression> arg) {
    // lambda applications are not directly supported by SBE (yet) and must not be present.
    tasserted(6624208, "lambda application is not implemented");
    return nullptr;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(const Variable& var) {
    auto def = _env.getDefinition(var);

    if (!def.definedBy.empty()) {
        // If this variable was defined by a Let expression, use the frame ID that was defined in
        // the prepare() step for the Let.
        if (auto let = def.definedBy.cast<Let>(); let) {
            auto it = _letMap.find(let);
            tassert(6624203, "incorrect let map", it != _letMap.end());

            return sbe::makeE<sbe::EVariable>(it->second, 0, _env.isLastRef(var));
        } else if (auto lam = def.definedBy.cast<LambdaAbstraction>(); lam) {
            // Similarly if the variable was defined by a lambda abstraction, use a frame ID rather
            // than a slot.
            auto it = _lambdaMap.find(lam);
            tassert(6624204, "incorrect lambda map", it != _lambdaMap.end());

            return sbe::makeE<sbe::EVariable>(it->second, 0, _env.isLastRef(var));
        }
    }
    // If variable was not defined in the scope of the local expression via a Let or
    // LambdaAbstraction, it must be a reference that will be in the slotMap.
    if (auto it = _slotMap.find(var.name()); it != _slotMap.end()) {
        // Found the slot.
        return sbe::makeE<sbe::EVariable>(it->second);
    }
    tasserted(6624205, str::stream() << "undefined variable: " << var.name());
    return nullptr;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const BinaryOp& op,
    std::unique_ptr<sbe::EExpression> lhs,
    std::unique_ptr<sbe::EExpression> rhs) {

    sbe::EPrimBinary::Op sbeOp = [](const auto abtOp) {
        switch (abtOp) {
            case Operations::Eq:
                return sbe::EPrimBinary::eq;
            case Operations::Neq:
                return sbe::EPrimBinary::neq;
            case Operations::Gt:
                return sbe::EPrimBinary::greater;
            case Operations::Gte:
                return sbe::EPrimBinary::greaterEq;
            case Operations::Lt:
                return sbe::EPrimBinary::less;
            case Operations::Lte:
                return sbe::EPrimBinary::lessEq;
            case Operations::Add:
                return sbe::EPrimBinary::add;
            case Operations::Sub:
                return sbe::EPrimBinary::sub;
            case Operations::FillEmpty:
                return sbe::EPrimBinary::fillEmpty;
            case Operations::And:
                return sbe::EPrimBinary::logicAnd;
            case Operations::Or:
                return sbe::EPrimBinary::logicOr;
            case Operations::Cmp3w:
                return sbe::EPrimBinary::cmp3w;
            case Operations::Div:
                return sbe::EPrimBinary::div;
            case Operations::Mult:
                return sbe::EPrimBinary::mul;
            default:
                MONGO_UNREACHABLE;
        }
    }(op.op());

    if (sbe::EPrimBinary::isComparisonOp(sbeOp)) {
        boost::optional<sbe::value::SlotId> collatorSlot =
            _namedSlots.getSlotIfExists("collator"_sd);
        if (collatorSlot) {
            return sbe::makeE<sbe::EPrimBinary>(
                sbeOp, std::move(lhs), std::move(rhs), sbe::makeE<sbe::EVariable>(*collatorSlot));
        }
    }

    return sbe::makeE<sbe::EPrimBinary>(sbeOp, std::move(lhs), std::move(rhs));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const UnaryOp& op, std::unique_ptr<sbe::EExpression> arg) {

    sbe::EPrimUnary::Op sbeOp = [](const auto abtOp) {
        switch (abtOp) {
            case Operations::Neg:
                return sbe::EPrimUnary::negate;
            case Operations::Not:
                return sbe::EPrimUnary::logicNot;
            default:
                MONGO_UNREACHABLE;
        }
    }(op.op());

    return sbe::makeE<sbe::EPrimUnary>(sbeOp, std::move(arg));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const If&,
    std::unique_ptr<sbe::EExpression> cond,
    std::unique_ptr<sbe::EExpression> thenBranch,
    std::unique_ptr<sbe::EExpression> elseBranch) {
    return sbe::makeE<sbe::EIf>(std::move(cond), std::move(thenBranch), std::move(elseBranch));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const FunctionCall& fn, std::vector<std::unique_ptr<sbe::EExpression>> args) {
    auto name = fn.name();

    if (name == "fail") {
        uassert(6250200, "Invalid number of arguments to fail()", fn.nodes().size() == 2);
        const auto* codeConstPtr = fn.nodes().at(0).cast<Constant>();
        const auto* messageConstPtr = fn.nodes().at(1).cast<Constant>();

        uassert(6250201,
                "First argument to fail() must be a 32-bit integer constant",
                codeConstPtr != nullptr && codeConstPtr->isValueInt32());
        uassert(6250202,
                "Second argument to fail() must be a string constant",
                messageConstPtr != nullptr && messageConstPtr->isString());

        return sbe::makeE<sbe::EFail>(static_cast<ErrorCodes::Error>(codeConstPtr->getValueInt32()),
                                      messageConstPtr->getString());
    }

    if (name == "convert") {
        uassert(6250203, "Invalid number of arguments to convert()", fn.nodes().size() == 2);
        const auto* constPtr = fn.nodes().at(1).cast<Constant>();

        uassert(6250204,
                "Second argument to convert() must be a 32-bit integer constant",
                constPtr != nullptr && constPtr->isValueInt32());
        int32_t constVal = constPtr->getValueInt32();

        uassert(6250205,
                "Second argument to convert() must be a numeric type tag",
                constVal >= static_cast<int32_t>(std::numeric_limits<uint8_t>::min()) &&
                    constVal <= static_cast<int32_t>(std::numeric_limits<uint8_t>::max()) &&
                    sbe::value::isNumber(static_cast<sbe::value::TypeTags>(constVal)));

        return sbe::makeE<sbe::ENumericConvert>(std::move(args.at(0)),
                                                static_cast<sbe::value::TypeTags>(constVal));
    }

    if (name == "typeMatch") {
        uassert(6250206, "Invalid number of arguments to typeMatch()", fn.nodes().size() == 2);
        const auto* constPtr = fn.nodes().at(1).cast<Constant>();

        uassert(6250207,
                "Second argument to typeMatch() must be a 32-bit integer constant",
                constPtr != nullptr && constPtr->isValueInt32());

        return sbe::makeE<sbe::EFunction>(
            "typeMatch",
            sbe::makeEs(std::move(args.at(0)),
                        sbe::makeE<sbe::EConstant>(
                            sbe::value::TypeTags::NumberInt64,
                            sbe::value::bitcastFrom<int64_t>(constPtr->getValueInt32()))));
    }

    // TODO - this is an open question how to do the name mappings.
    if (name == "$sum") {
        name = "sum";
    } else if (name == "$first") {
        name = "first";
    } else if (name == "$last") {
        name = "last";
    } else if (name == "$min") {
        name = "min";
    } else if (name == "$max") {
        name = "max";
    } else if (name == "$addToSet") {
        name = "addToSet";
    } else if (name == "$push") {
        name = "addToArray";
    }

    return sbe::makeE<sbe::EFunction>(name, toInlinedVector(std::move(args)));
}

sbe::value::SlotVector SBENodeLowering::convertProjectionsToSlots(
    const SlotVarMap& slotMap, const ProjectionNameVector& projectionNames) {
    sbe::value::SlotVector result;
    for (const ProjectionName& projectionName : projectionNames) {
        auto it = slotMap.find(projectionName);
        tassert(6624211,
                str::stream() << "undefined variable: " << projectionName,
                it != slotMap.end());
        result.push_back(it->second);
    }
    return result;
}

sbe::value::SlotVector SBENodeLowering::convertRequiredProjectionsToSlots(
    const SlotVarMap& slotMap, const NodeProps& props, const sbe::value::SlotVector& toExclude) {
    using namespace properties;

    sbe::value::SlotSet toExcludeSet;
    for (const auto slot : toExclude) {
        toExcludeSet.insert(slot);
    }

    sbe::value::SlotVector result;
    const auto& projections =
        getPropertyConst<ProjectionRequirement>(props._physicalProps).getProjections();
    for (const auto slot : convertProjectionsToSlots(slotMap, projections.getVector())) {
        if (toExcludeSet.count(slot) == 0) {
            result.push_back(slot);
        }
    }
    return result;
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::optimize(
    const ABT& n, SlotVarMap& slotMap, boost::optional<sbe::value::SlotId>& ridSlot) {
    return generateInternal(n, slotMap, ridSlot);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::generateInternal(
    const ABT& n, SlotVarMap& slotMap, boost::optional<sbe::value::SlotId>& ridSlot) {
    tassert(
        7239200, "Should not be lowering only logical ABT node", !n.cast<ExclusivelyLogicalNode>());
    return algebra::walk<false>(n, *this, slotMap, ridSlot);
}

void SBENodeLowering::mapProjToSlot(SlotVarMap& slotMap,
                                    const ProjectionName& projName,
                                    const sbe::value::SlotId slot,
                                    const bool canOverwrite) {
    const bool inserted = slotMap.insert_or_assign(projName, slot).second;
    if (!canOverwrite) {
        tassert(6624263, "Cannot overwrite slot map", inserted);
    }
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const RootNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& refs) {
    using namespace properties;
    auto input = generateInternal(child, slotMap, ridSlot);

    auto output = refs.cast<References>();
    tassert(6624212, "refs expected", output);

    SlotVarMap finalMap;
    for (auto& o : output->nodes()) {
        auto var = o.cast<Variable>();
        tassert(6624213, "var expected", var);
        if (auto it = slotMap.find(var->name()); it != slotMap.end()) {
            finalMap.emplace(var->name(), it->second);
        }
    }

    if (const auto& props = _nodeToGroupPropsMap.at(&n);
        hasProperty<ProjectionRequirement>(props._physicalProps)) {
        if (const auto& ridProjName = props._ridProjName) {
            // If we required rid on the Root node, populate ridSlot.
            const auto& projections =
                getPropertyConst<ProjectionRequirement>(props._physicalProps).getProjections();
            if (projections.find(*ridProjName)) {
                // Deliver the ridSlot separate from the slotMap.
                ridSlot = slotMap.at(*ridProjName);
                finalMap.erase(*ridProjName);
            }
        }
    }

    std::swap(slotMap, finalMap);
    return input;
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const EvaluationNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& binds) {
    auto input = generateInternal(child, slotMap, ridSlot);

    // If the evaluation node is only renaming a variable, do not place a project stage.
    if (auto varPtr = n.getProjection().cast<Variable>(); varPtr != nullptr) {
        mapProjToSlot(slotMap, n.getProjectionName(), slotMap.at(varPtr->name()));
        return input;
    }

    auto& binder = n.binder();
    auto& names = binder.names();
    auto& exprs = binder.exprs();

    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;

    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        auto expr = lowerExpression(exprs[idx], slotMap);
        auto slot = _slotIdGenerator.generate();

        mapProjToSlot(slotMap, names[idx], slot);
        projects.emplace(slot, std::move(expr));
    }

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::ProjectStage>(std::move(input), std::move(projects), planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const FilterNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& filter) {
    auto input = generateInternal(child, slotMap, ridSlot);
    auto expr = lowerExpression(filter, slotMap);
    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;

    // Check if the filter expression is 'constant' (i.e., does not depend on any variables); then
    // create FilterStage<true> if it is constant, or FilterStage<false> otherwise.
    bool isConstant = true;
    VariableEnvironment::walkVariables(filter, [&](const Variable&) { isConstant = false; });

    if (isConstant) {
        return sbe::makeS<sbe::FilterStage<true>>(std::move(input), std::move(expr), planNodeId);
    } else {
        return sbe::makeS<sbe::FilterStage<false>>(std::move(input), std::move(expr), planNodeId);
    }
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const LimitSkipNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child) {
    auto input = generateInternal(child, slotMap, ridSlot);

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::LimitSkipStage>(
        std::move(input), n.getProperty().getLimit(), n.getProperty().getSkip(), planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ExchangeNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& refs) {
    using namespace std::literals;
    using namespace properties;

    // The DOP is obtained from the child (number of producers).
    const auto& childProps = _nodeToGroupPropsMap.at(n.getChild().cast<Node>())._physicalProps;
    const auto& childDistribution = getPropertyConst<DistributionRequirement>(childProps);
    tassert(6624330,
            "Parent and child distributions are the same",
            !(childDistribution == n.getProperty()));

    const size_t localDOP =
        (childDistribution.getDistributionAndProjections()._type == DistributionType::Centralized)
        ? 1
        : _metadata._numberOfPartitions;
    tassert(6624215, "invalid DOP", localDOP >= 1);

    auto input = generateInternal(child, slotMap, ridSlot);

    // Initialized to arbitrary placeholder
    sbe::ExchangePolicy localPolicy{};
    std::unique_ptr<sbe::EExpression> partitionExpr;

    const auto& distribAndProjections = n.getProperty().getDistributionAndProjections();
    switch (distribAndProjections._type) {
        case DistributionType::Centralized:
        case DistributionType::Replicated:
            localPolicy = sbe::ExchangePolicy::broadcast;
            break;

        case DistributionType::RoundRobin:
            localPolicy = sbe::ExchangePolicy::roundrobin;
            break;

        case DistributionType::RangePartitioning:
            // We set 'localPolicy' to 'ExchangePolicy::rangepartition' here, but there is more
            // that we need to do to actually support the RangePartitioning distribution.
            // TODO SERVER-62523: Implement real support for the RangePartitioning distribution
            // and add some test coverage.
            localPolicy = sbe::ExchangePolicy::rangepartition;
            break;

        case DistributionType::HashPartitioning: {
            localPolicy = sbe::ExchangePolicy::hashpartition;
            std::vector<std::unique_ptr<sbe::EExpression>> args;
            for (const ProjectionName& proj : distribAndProjections._projectionNames) {
                auto it = slotMap.find(proj);
                tassert(6624216, str::stream() << "undefined var: " << proj, it != slotMap.end());

                args.emplace_back(sbe::makeE<sbe::EVariable>(it->second));
            }
            partitionExpr = sbe::makeE<sbe::EFunction>("hash"_sd, toInlinedVector(std::move(args)));
            break;
        }

        case DistributionType::UnknownPartitioning:
            tasserted(6624217, "Cannot partition into unknown distribution");

        default:
            MONGO_UNREACHABLE;
    }

    const auto& nodeProps = _nodeToGroupPropsMap.at(&n);
    auto fields = convertRequiredProjectionsToSlots(slotMap, nodeProps);

    return sbe::makeS<sbe::ExchangeConsumer>(std::move(input),
                                             localDOP,
                                             std::move(fields),
                                             localPolicy,
                                             std::move(partitionExpr),
                                             nullptr,
                                             nodeProps._planNodeId);
}

static sbe::value::SortDirection collationOpToSBESortDirection(const CollationOp collOp) {
    switch (collOp) {
        // TODO: is there a more efficient way to compute clustered collation op than sort?
        case CollationOp::Ascending:
        case CollationOp::Clustered:
            return sbe::value::SortDirection::Ascending;
        case CollationOp::Descending:
            return sbe::value::SortDirection::Descending;
    }
    MONGO_UNREACHABLE;
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const CollationNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& refs) {
    auto input = generateInternal(child, slotMap, ridSlot);

    sbe::value::SlotVector orderBySlots;
    std::vector<sbe::value::SortDirection> directions;
    for (const auto& entry : n.getProperty().getCollationSpec()) {
        auto it = slotMap.find(entry.first);

        tassert(6624219,
                str::stream() << "undefined orderBy var: " << entry.first,
                it != slotMap.end());
        orderBySlots.push_back(it->second);

        directions.push_back(collationOpToSBESortDirection(entry.second));
    }

    const auto& nodeProps = _nodeToGroupPropsMap.at(&n);
    const auto& physProps = nodeProps._physicalProps;

    size_t limit = std::numeric_limits<std::size_t>::max();
    if (properties::hasProperty<properties::LimitSkipRequirement>(physProps)) {
        const auto& limitSkipReq =
            properties::getPropertyConst<properties::LimitSkipRequirement>(physProps);
        tassert(6624221, "We should not have skip set here", limitSkipReq.getSkip() == 0);
        limit = limitSkipReq.getLimit();
    }

    // TODO: obtain defaults for these.
    const size_t memoryLimit = 100 * (1ul << 20);  // 100MB
    const bool allowDiskUse = false;

    auto vals = convertRequiredProjectionsToSlots(slotMap, nodeProps, orderBySlots);
    return sbe::makeS<sbe::SortStage>(std::move(input),
                                      std::move(orderBySlots),
                                      std::move(directions),
                                      std::move(vals),
                                      limit,
                                      memoryLimit,
                                      allowDiskUse,
                                      nodeProps._planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const UniqueNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& refs) {
    auto input = generateInternal(child, slotMap, ridSlot);

    sbe::value::SlotVector keySlots;
    for (const ProjectionName& projectionName : n.getProjections()) {
        auto it = slotMap.find(projectionName);
        tassert(6624222,
                str::stream() << "undefined variable: " << projectionName,
                it != slotMap.end());
        keySlots.push_back(it->second);
    }

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::UniqueStage>(std::move(input), std::move(keySlots), planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const SpoolProducerNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& filter,
                                                      const ABT& binder,
                                                      const ABT& refs) {
    auto input = generateInternal(child, slotMap, ridSlot);

    sbe::value::SlotVector vals;
    for (const ProjectionName& projectionName : n.binder().names()) {
        auto it = slotMap.find(projectionName);
        tassert(6624139,
                str::stream() << "undefined variable: " << projectionName,
                it != slotMap.end());
        vals.push_back(it->second);
    }

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    switch (n.getType()) {
        case SpoolProducerType::Eager:
            return sbe::makeS<sbe::SpoolEagerProducerStage>(
                std::move(input), n.getSpoolId(), std::move(vals), planNodeId);

        case SpoolProducerType::Lazy: {
            auto expr = lowerExpression(filter, slotMap);
            return sbe::makeS<sbe::SpoolLazyProducerStage>(
                std::move(input), n.getSpoolId(), std::move(vals), std::move(expr), planNodeId);
        }
    }

    MONGO_UNREACHABLE;
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const SpoolConsumerNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& binder) {
    sbe::value::SlotVector vals;
    for (const ProjectionName& projectionName : n.binder().names()) {
        auto slot = _slotIdGenerator.generate();
        mapProjToSlot(slotMap, projectionName, slot);
        vals.push_back(slot);
    }

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    switch (n.getType()) {
        case SpoolConsumerType::Stack:
            return sbe::makeS<sbe::SpoolConsumerStage<true /*isStack*/>>(
                n.getSpoolId(), std::move(vals), planNodeId);

        case SpoolConsumerType::Regular:
            return sbe::makeS<sbe::SpoolConsumerStage<false /*isStack*/>>(
                n.getSpoolId(), std::move(vals), planNodeId);
    }

    MONGO_UNREACHABLE;
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const GroupByNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& aggBinds,
                                                      const ABT& aggRefs,
                                                      const ABT& gbBind,
                                                      const ABT& gbRefs) {

    auto input = generateInternal(child, slotMap, ridSlot);

    // Ideally, we should make a distinction between gbBind and gbRefs; i.e. internal references
    // used by the hash agg to determinte the group by values from its input and group by values as
    // outputted by the hash agg after the grouping. However, SBE hash agg uses the same slot it to
    // represent both so that distinction is kind of moot.
    sbe::value::SlotVector gbs;
    auto gbCols = gbRefs.cast<References>();
    tassert(6624223, "refs expected", gbCols);
    for (auto& o : gbCols->nodes()) {
        auto var = o.cast<Variable>();
        tassert(6624224, "var expected", var);
        auto it = slotMap.find(var->name());
        tassert(6624225, str::stream() << "undefined var: " << var->name(), it != slotMap.end());
        gbs.push_back(it->second);
    }

    // Similar considerations apply to the agg expressions as to the group by columns.
    auto& names = n.binderAgg().names();

    auto refsAgg = aggRefs.cast<References>();
    tassert(6624227, "refs expected", refsAgg);
    auto& exprs = refsAgg->nodes();

    sbe::SlotExprPairVector aggs;
    aggs.reserve(exprs.size());

    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        auto expr = lowerExpression(exprs[idx], slotMap);
        auto slot = _slotIdGenerator.generate();

        mapProjToSlot(slotMap, names[idx], slot);
        aggs.push_back({slot, std::move(expr)});
    }

    boost::optional<sbe::value::SlotId> collatorSlot = _namedSlots.getSlotIfExists("collator"_sd);
    // Unused
    sbe::value::SlotVector seekKeysSlots;

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::HashAggStage>(std::move(input),
                                         std::move(gbs),
                                         std::move(aggs),
                                         std::move(seekKeysSlots),
                                         true /*optimizedClose*/,
                                         collatorSlot,
                                         false /*allowDiskUse*/,
                                         // Since we are always disallowing disk use for this stage,
                                         // we need not provide merging expressions. Once spilling
                                         // is permitted here, we will need to generate merging
                                         // expressions during lowering.
                                         sbe::makeSlotExprPairVec() /*mergingExprs*/,
                                         planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const NestedLoopJoinNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& leftChild,
                                                      const ABT& rightChild,
                                                      const ABT& filter) {
    auto outerStage = generateInternal(leftChild, slotMap, ridSlot);
    auto innerStage = generateInternal(rightChild, slotMap, ridSlot);

    // List of correlated projections (bound in outer side and referred to in the inner side).
    sbe::value::SlotVector correlatedSlots;
    for (const ProjectionName& projectionName : n.getCorrelatedProjectionNames()) {
        correlatedSlots.push_back(slotMap.at(projectionName));
    }
    // Soring is not essential. Here we sort only for SBE plan stability.
    std::sort(correlatedSlots.begin(), correlatedSlots.end());

    auto expr = lowerExpression(filter, slotMap);

    const auto& leftChildProps = _nodeToGroupPropsMap.at(n.getLeftChild().cast<Node>());
    auto outerProjects = convertRequiredProjectionsToSlots(slotMap, leftChildProps);

    const auto& rightChildProps = _nodeToGroupPropsMap.at(n.getRightChild().cast<Node>());
    auto innerProjects = convertRequiredProjectionsToSlots(slotMap, rightChildProps);

    sbe::JoinType joinType = [&]() {
        switch (n.getJoinType()) {
            case JoinType::Inner:
                return sbe::JoinType::Inner;
            case JoinType::Left:
                return sbe::JoinType::Left;
            default:
                MONGO_UNREACHABLE;
        }
    }();

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::LoopJoinStage>(std::move(outerStage),
                                          std::move(innerStage),
                                          std::move(outerProjects),
                                          std::move(correlatedSlots),
                                          std::move(innerProjects),
                                          std::move(expr),
                                          joinType,
                                          planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const HashJoinNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& leftChild,
                                                      const ABT& rightChild,
                                                      const ABT& refs) {
    // Note the inner and outer sides here are reversed. The HashJoinNode assumes the build side is
    // the inner side while sbe hash join stage assumes the build side is the outer side.

    auto innerStage = generateInternal(leftChild, slotMap, ridSlot);
    auto outerStage = generateInternal(rightChild, slotMap, ridSlot);

    tassert(6624228, "Only inner joins supported for now", n.getJoinType() == JoinType::Inner);

    const auto& leftProps = _nodeToGroupPropsMap.at(n.getLeftChild().cast<Node>());
    const auto& rightProps = _nodeToGroupPropsMap.at(n.getRightChild().cast<Node>());

    // Add RID projection only from outer side.
    auto innerKeys = convertProjectionsToSlots(slotMap, n.getLeftKeys());
    auto innerProjects = convertRequiredProjectionsToSlots(slotMap, leftProps, innerKeys);
    auto outerKeys = convertProjectionsToSlots(slotMap, n.getRightKeys());
    auto outerProjects = convertRequiredProjectionsToSlots(slotMap, rightProps, outerKeys);

    boost::optional<sbe::value::SlotId> collatorSlot = _namedSlots.getSlotIfExists("collator"_sd);
    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::HashJoinStage>(std::move(outerStage),
                                          std::move(innerStage),
                                          std::move(outerKeys),
                                          std::move(outerProjects),
                                          std::move(innerKeys),
                                          std::move(innerProjects),
                                          collatorSlot,
                                          planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const MergeJoinNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& leftChild,
                                                      const ABT& rightChild,
                                                      const ABT& refs) {
    auto outerStage = generateInternal(leftChild, slotMap, ridSlot);
    auto innerStage = generateInternal(rightChild, slotMap, ridSlot);

    const auto& leftProps = _nodeToGroupPropsMap.at(n.getLeftChild().cast<Node>());
    const auto& rightProps = _nodeToGroupPropsMap.at(n.getRightChild().cast<Node>());

    std::vector<sbe::value::SortDirection> sortDirs;
    for (const CollationOp op : n.getCollation()) {
        sortDirs.push_back(collationOpToSBESortDirection(op));
    }

    // Add RID projection only from outer side.
    auto outerKeys = convertProjectionsToSlots(slotMap, n.getLeftKeys());
    auto outerProjects = convertRequiredProjectionsToSlots(slotMap, leftProps, outerKeys);
    auto innerKeys = convertProjectionsToSlots(slotMap, n.getRightKeys());
    auto innerProjects = convertRequiredProjectionsToSlots(slotMap, rightProps, innerKeys);

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::MergeJoinStage>(std::move(outerStage),
                                           std::move(innerStage),
                                           std::move(outerKeys),
                                           std::move(outerProjects),
                                           std::move(innerKeys),
                                           std::move(innerProjects),
                                           std::move(sortDirs),
                                           planNodeId);
}


std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const SortedMergeNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABTVector& children,
                                                      const ABT& binder,
                                                      const ABT& /*refs*/) {
    const auto& names = n.binder().names();

    const ProjectionCollationSpec& collSpec = n.getCollationReq().getCollationSpec();

    std::vector<sbe::value::SortDirection> keyDirs;
    for (const auto& collEntry : collSpec) {
        keyDirs.push_back(collationOpToSBESortDirection(collEntry.second));
    }

    sbe::PlanStage::Vector loweredChildren;
    std::vector<sbe::value::SlotVector> inputKeys;
    std::vector<sbe::value::SlotVector> inputVals;
    for (const ABT& child : children) {
        // Use a fresh map to prevent same projections for every child being overwritten. We
        // initialize with the current map in order to be able to use correlated slots.
        SlotVarMap localMap = slotMap;
        boost::optional<sbe::value::SlotId> localRIDSlot;
        auto loweredChild = optimize(child, localMap, localRIDSlot);
        tassert(7063700, "Unexpected rid slot", !localRIDSlot);

        loweredChildren.push_back(std::move(loweredChild));

        // Find the slots for the collation keys. Also find slots for other values passed.
        sbe::value::SlotVector childKeys(collSpec.size());
        sbe::value::SlotVector childVals;
        // Note that lowering for SortedMergeNode does not take into account required projections
        // from the Cascade props for this node. Like UnionNode, it's expected that all fields that
        // should be visible above a SortedMergeNode should be added to the exprBinder explicitly
        // before lowering.
        for (const auto& name : names) {
            const auto it = std::find_if(
                collSpec.begin(), collSpec.end(), [&](const auto& x) { return x.first == name; });
            if (it != collSpec.end()) {
                const size_t index = std::distance(collSpec.begin(), it);
                childKeys.at(index) = localMap.at(name);
            }
            childVals.push_back(localMap.at(name));
        }
        inputKeys.emplace_back(std::move(childKeys));
        inputVals.emplace_back(std::move(childVals));
    }

    sbe::value::SlotVector outputVals;
    for (const auto& name : names) {
        const auto outputSlot = _slotIdGenerator.generate();
        mapProjToSlot(slotMap, name, outputSlot);
        outputVals.push_back(outputSlot);
    }

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::SortedMergeStage>(std::move(loweredChildren),
                                             std::move(inputKeys),
                                             std::move(keyDirs),
                                             std::move(inputVals),
                                             std::move(outputVals),
                                             planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const UnionNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABTVector& children,
                                                      const ABT& binder,
                                                      const ABT& /*refs*/) {
    const auto& names = n.binder().names();

    sbe::PlanStage::Vector loweredChildren;
    std::vector<sbe::value::SlotVector> inputVals;

    for (const ABT& child : children) {
        // Use a fresh map to prevent same projections for every child being overwritten. We
        // initialize with the current map in order to be able to use correlated slots.
        SlotVarMap localMap = slotMap;
        boost::optional<sbe::value::SlotId> localRIDSlot;
        auto loweredChild = optimize(child, localMap, localRIDSlot);
        tassert(6624258, "Unexpected rid slot", !localRIDSlot);

        if (children.size() == 1) {
            // Union with one child is used to restrict projections. Do not place a union stage.
            for (const auto& name : names) {
                mapProjToSlot(slotMap, name, localMap.at(name));
            }
            return loweredChild;
        }
        loweredChildren.push_back(std::move(loweredChild));

        sbe::value::SlotVector childSlots;
        for (const auto& name : names) {
            childSlots.push_back(localMap.at(name));
        }
        inputVals.emplace_back(std::move(childSlots));
    }

    sbe::value::SlotVector outputVals;
    for (const auto& name : names) {
        const auto outputSlot = _slotIdGenerator.generate();
        mapProjToSlot(slotMap, name, outputSlot);
        outputVals.push_back(outputSlot);
    }

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::UnionStage>(
        std::move(loweredChildren), std::move(inputVals), std::move(outputVals), planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const UnwindNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& pidBind,
                                                      const ABT& refs) {
    auto input = generateInternal(child, slotMap, ridSlot);

    auto it = slotMap.find(n.getProjectionName());
    tassert(6624230,
            str::stream() << "undefined unwind variable: " << n.getProjectionName(),
            it != slotMap.end());

    auto inputSlot = it->second;
    auto outputSlot = _slotIdGenerator.generate();
    auto outputPidSlot = _slotIdGenerator.generate();

    // The unwind is overwriting the output projection.
    mapProjToSlot(slotMap, n.getProjectionName(), outputSlot, true /*canOverwrite*/);
    mapProjToSlot(slotMap, n.getPIDProjectionName(), outputPidSlot);

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::UnwindStage>(
        std::move(input), inputSlot, outputSlot, outputPidSlot, n.getRetainNonArrays(), planNodeId);
}

void SBENodeLowering::generateSlots(SlotVarMap& slotMap,
                                    const FieldProjectionMap& fieldProjectionMap,
                                    boost::optional<sbe::value::SlotId>& ridSlot,
                                    boost::optional<sbe::value::SlotId>& rootSlot,
                                    std::vector<std::string>& fields,
                                    sbe::value::SlotVector& vars) {
    if (const auto& projName = fieldProjectionMap._ridProjection) {
        ridSlot = _slotIdGenerator.generate();
        // Allow overwriting slots for rid projections only. We have a single rid projection per
        // collection.
        mapProjToSlot(slotMap, *projName, ridSlot.value(), true /*canOverwrite*/);
    }
    if (const auto& projName = fieldProjectionMap._rootProjection) {
        rootSlot = _slotIdGenerator.generate();
        mapProjToSlot(slotMap, *projName, rootSlot.value());
    }

    // Soring is not essential. Here we sort only for SBE plan stability.
    std::map<FieldNameType, ProjectionName> ordered;
    for (const auto& entry : fieldProjectionMap._fieldProjections) {
        ordered.insert(entry);
    }
    for (const auto& [fieldName, projectionName] : ordered) {
        vars.push_back(_slotIdGenerator.generate());
        mapProjToSlot(slotMap, projectionName, vars.back());
        fields.push_back(fieldName.value().toString());
    }
}

static NamespaceStringOrUUID parseFromScanDef(const ScanDefinition& def) {
    const auto& dbName = def.getOptionsMap().at("database");
    const auto& uuidStr = def.getOptionsMap().at("uuid");
    return {dbName, UUID::parse(uuidStr).getValue()};
}


std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const PhysicalScanNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& /*binds*/) {
    const ScanDefinition& def = _metadata._scanDefs.at(n.getScanDefName());
    tassert(6624231, "Collection must exist to lower Scan", def.exists());
    auto& typeSpec = def.getOptionsMap().at("type");

    boost::optional<sbe::value::SlotId> scanRidSlot;
    boost::optional<sbe::value::SlotId> rootSlot;
    std::vector<std::string> fields;
    sbe::value::SlotVector vars;
    generateSlots(slotMap, n.getFieldProjectionMap(), scanRidSlot, rootSlot, fields, vars);

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    if (typeSpec == "mongod") {
        NamespaceStringOrUUID nss = parseFromScanDef(def);

        // Unused.
        boost::optional<sbe::value::SlotId> seekKeySlot;

        sbe::ScanCallbacks callbacks({}, {}, {});
        if (n.useParallelScan()) {
            return sbe::makeS<sbe::ParallelScanStage>(nss.uuid().value(),
                                                      rootSlot,
                                                      scanRidSlot,
                                                      boost::none,
                                                      boost::none,
                                                      boost::none,
                                                      boost::none,
                                                      fields,
                                                      vars,
                                                      nullptr /*yieldPolicy*/,
                                                      planNodeId,
                                                      callbacks);
        }

        bool forwardScan = [&]() {
            switch (_scanOrder) {
                case ScanOrder::Forward:
                case ScanOrder::Random:
                    return true;
                case ScanOrder::Reverse:
                    return false;
            }
            MONGO_UNREACHABLE;
        }();
        return sbe::makeS<sbe::ScanStage>(nss.uuid().value(),
                                          rootSlot,
                                          scanRidSlot,
                                          boost::none,
                                          boost::none,
                                          boost::none,
                                          boost::none,
                                          boost::none,
                                          fields,
                                          vars,
                                          seekKeySlot,
                                          forwardScan,
                                          nullptr /*yieldPolicy*/,
                                          planNodeId,
                                          callbacks,
                                          false, /* lowPriority */
                                          _scanOrder == ScanOrder::Random);
    } else {
        tasserted(6624355, "Unknown scan type.");
    }
    return nullptr;
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(
    const CoScanNode& n, SlotVarMap& slotMap, boost::optional<sbe::value::SlotId>& ridSlot) {
    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::CoScanStage>(planNodeId);
}

std::unique_ptr<sbe::EExpression> SBENodeLowering::convertBoundsToExpr(
    SlotVarMap& slotMap,
    const bool isLower,
    const bool reversed,
    const IndexDefinition& indexDef,
    const CompoundBoundRequirement& bound) {
    std::vector<std::unique_ptr<sbe::EExpression>> ksFnArgs;
    ksFnArgs.emplace_back(
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(indexDef.getVersion())));

    // TODO: ordering is unsigned int32??
    ksFnArgs.emplace_back(
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                   sbe::value::bitcastFrom<uint32_t>(indexDef.getOrdering())));

    auto exprLower = getExpressionLowering(slotMap);
    for (const auto& expr : bound.getBound()) {
        ksFnArgs.emplace_back(exprLower.optimize(expr));
    }
    if (!isLower && (bound.isMinusInf() || bound.isPlusInf())) {
        // We can skip if fully infinite only for upper bound. For lower bound we need to generate
        // minkeys.
        return nullptr;
    };

    KeyString::Discriminator discriminator;
    // For a reverse scan, we start from the high bound and iterate until the low bound.
    if (isLower != reversed) {
        // For the start point, we want to seek ExclusiveBefore iff the bound is inclusive,
        // so that values equal to the seek value are included.
        discriminator = bound.isInclusive() ? KeyString::Discriminator::kExclusiveBefore
                                            : KeyString::Discriminator::kExclusiveAfter;
    } else {
        // For the end point we want the opposite.
        discriminator = bound.isInclusive() ? KeyString::Discriminator::kExclusiveAfter
                                            : KeyString::Discriminator::kExclusiveBefore;
    }

    ksFnArgs.emplace_back(sbe::makeE<sbe::EConstant>(
        sbe::value::TypeTags::NumberInt64,
        sbe::value::bitcastFrom<int64_t>(static_cast<int64_t>(discriminator))));
    return sbe::makeE<sbe::EFunction>("ks", toInlinedVector(std::move(ksFnArgs)));
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const IndexScanNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT&) {
    const auto& fieldProjectionMap = n.getFieldProjectionMap();

    const std::string& indexDefName = n.getIndexDefName();
    const ScanDefinition& scanDef = _metadata._scanDefs.at(n.getScanDefName());
    tassert(6624232, "Collection must exist to lower IndexScan", scanDef.exists());
    const IndexDefinition& indexDef = scanDef.getIndexDefs().at(indexDefName);

    NamespaceStringOrUUID nss = parseFromScanDef(scanDef);

    boost::optional<sbe::value::SlotId> scanRidSlot;
    boost::optional<sbe::value::SlotId> rootSlot;
    std::vector<std::string> fields;
    sbe::value::SlotVector vars;
    generateSlots(slotMap, fieldProjectionMap, scanRidSlot, rootSlot, fields, vars);
    tassert(6624233, "Cannot deliver root projection in this context", !rootSlot.has_value());

    std::vector<std::pair<size_t, sbe::value::SlotId>> indexVars;
    sbe::IndexKeysInclusionSet indexKeysToInclude;

    for (size_t index = 0; index < fields.size(); index++) {
        const size_t indexFieldPos = decodeIndexKeyName(fields.at(index));
        indexVars.emplace_back(indexFieldPos, vars.at(index));
        indexKeysToInclude.set(indexFieldPos, true);
    }

    // Make sure vars are in sorted order on index field position.
    std::sort(indexVars.begin(), indexVars.end());
    vars.clear();
    for (const auto& [indexFieldPos, slot] : indexVars) {
        vars.push_back(slot);
    }

    const auto& interval = n.getIndexInterval();
    const auto* lowBoundPtr = &interval.getLowBound();
    const auto* highBoundPtr = &interval.getHighBound();

    const bool reverse = n.isIndexReverseOrder();
    if (reverse) {
        std::swap(lowBoundPtr, highBoundPtr);
    }

    auto lowerBoundExpr =
        convertBoundsToExpr(slotMap, true /*isLower*/, reverse, indexDef, *lowBoundPtr);
    auto upperBoundExpr =
        convertBoundsToExpr(slotMap, false /*isLower*/, reverse, indexDef, *highBoundPtr);
    tassert(6624234,
            "Invalid bounds combination",
            lowerBoundExpr != nullptr || upperBoundExpr == nullptr);

    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;

    // Unused.
    boost::optional<sbe::value::SlotId> resultSlot;

    return sbe::makeS<sbe::SimpleIndexScanStage>(nss.uuid().value(),
                                                 indexDefName,
                                                 !reverse,
                                                 resultSlot,
                                                 scanRidSlot,
                                                 boost::none,
                                                 indexKeysToInclude,
                                                 vars,
                                                 std::move(lowerBoundExpr),
                                                 std::move(upperBoundExpr),
                                                 nullptr /*yieldPolicy*/,
                                                 planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const SeekNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& /*binds*/,
                                                      const ABT& /*refs*/) {
    const ScanDefinition& def = _metadata._scanDefs.at(n.getScanDefName());
    tassert(6624235, "Collection must exist to lower Seek", def.exists());

    auto& typeSpec = def.getOptionsMap().at("type");
    tassert(6624236, "SeekNode only supports mongod collections", typeSpec == "mongod");
    NamespaceStringOrUUID nss = parseFromScanDef(def);

    boost::optional<sbe::value::SlotId> seekRidSlot;
    boost::optional<sbe::value::SlotId> rootSlot;
    std::vector<std::string> fields;
    sbe::value::SlotVector vars;
    generateSlots(slotMap, n.getFieldProjectionMap(), seekRidSlot, rootSlot, fields, vars);

    boost::optional<sbe::value::SlotId> seekKeySlot = slotMap.at(n.getRIDProjectionName());

    sbe::ScanCallbacks callbacks({}, {}, {});
    const PlanNodeId planNodeId = _nodeToGroupPropsMap.at(&n)._planNodeId;
    return sbe::makeS<sbe::ScanStage>(nss.uuid().value(),
                                      rootSlot,
                                      seekRidSlot,
                                      boost::none,
                                      boost::none,
                                      boost::none,
                                      boost::none,
                                      boost::none,
                                      fields,
                                      vars,
                                      seekKeySlot,
                                      true /*forward*/,
                                      nullptr /*yieldPolicy*/,
                                      planNodeId,
                                      callbacks);
}

}  // namespace mongo::optimizer
