/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2017, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file MagicSet.cpp
 *
 * Define classes and functionality related to the magic set transformation.
 *
 ***********************************************************************/

#include "AstQualifiedName.h"

#include "AstAttribute.h"
#include "AstIO.h"
#include "AstIOTypeAnalysis.h"
#include "AstNode.h"
#include "AstProgram.h"
#include "AstRelation.h"
#include "AstTransforms.h"
#include "AstTranslationUnit.h"
#include "AstUtils.h"
#include "BinaryConstraintOps.h"
#include "Global.h"
#include "MagicSet.h"
#include "RamTypes.h"
#include "SrcLocation.h"
#include "utility/ContainerUtil.h"
#include "utility/MiscUtil.h"
#include "utility/StringUtil.h"
#include <utility>

namespace souffle {

bool NormaliseDatabaseTransformer::transform(AstTranslationUnit& translationUnit) {
    bool changed = false;

    /** (1) Partition input and output relations */
    changed |= partitionIO(translationUnit);
    if (changed) translationUnit.invalidateAnalyses();

    /** (2) Separate the IDB from the EDB */
    changed |= extractIDB(translationUnit);
    if (changed) translationUnit.invalidateAnalyses();

    /** (3) Move constants into new equality constraints */
    changed |= nameConstants(translationUnit);
    if (changed) translationUnit.invalidateAnalyses();

    /** (4) Querify output relations */
    changed |= querifyOutputRelations(translationUnit);
    if (changed) translationUnit.invalidateAnalyses();

    return changed;
}

bool NormaliseDatabaseTransformer::partitionIO(AstTranslationUnit& translationUnit) {
    auto* ioTypes = translationUnit.getAnalysis<IOType>();
    auto& program = *translationUnit.getProgram();

    std::set<AstQualifiedName> relationsToSplit;
    for (auto* rel : program.getRelations()) {
        if (ioTypes->isInput(rel) && (ioTypes->isOutput(rel) || ioTypes->isPrintSize(rel))) {
            relationsToSplit.insert(rel->getQualifiedName());
        }
    }

    for (auto relName : relationsToSplit) {
        const auto* rel = getRelation(program, relName);
        assert(rel != nullptr && "relation does not exist");
        auto newRelName = AstQualifiedName(relName);
        newRelName.prepend("@split_in");

        // Create a new intermediate input relation
        auto newRelation = std::make_unique<AstRelation>(newRelName);
        for (const auto* attr : rel->getAttributes()) {
            newRelation->addAttribute(std::unique_ptr<AstAttribute>(attr->clone()));
        }

        // Read in the input relation into the original relation
        auto newClause = std::make_unique<AstClause>();
        auto newHeadAtom = std::make_unique<AstAtom>(relName);
        auto newBodyAtom = std::make_unique<AstAtom>(newRelName);
        for (size_t i = 0; i < rel->getArity(); i++) {
            std::stringstream varName;
            varName << "@var" << i;
            newHeadAtom->addArgument(std::make_unique<AstVariable>(varName.str()));
            newBodyAtom->addArgument(std::make_unique<AstVariable>(varName.str()));
        }
        newClause->setHead(std::move(newHeadAtom));
        newClause->addToBody(std::move(newBodyAtom));

        // New relation should be input, original should not
        std::set<const AstIO*> iosToDelete;
        std::set<std::unique_ptr<AstIO>> iosToAdd;
        for (const auto* io : program.getIOs()) {
            if (io->getQualifiedName() == relName && io->getType() == AstIoType::input) {
                if (!io->hasDirective("IO") ||
                        (io->getDirective("IO") == "file" && !io->hasDirective("filename"))) {
                    auto newIO = std::make_unique<AstIO>(AstIoType::input, newRelName);
                    std::stringstream defaultFactFile;
                    defaultFactFile << relName << ".facts";
                    newIO->addDirective("IO", "file");
                    newIO->addDirective("filename", defaultFactFile.str());
                    iosToAdd.insert(std::move(newIO));
                } else {
                    auto newIO = std::unique_ptr<AstIO>(io->clone());
                    newIO->setQualifiedName(newRelName);
                    iosToAdd.insert(std::move(newIO));
                }
                iosToDelete.insert(io);
            }
        }

        for (const auto* io : iosToDelete) {
            program.removeIO(io);
        }
        for (auto& io : iosToAdd) {
            program.addIO(std::unique_ptr<AstIO>(io->clone()));
        }

        program.addRelation(std::move(newRelation));
        program.addClause(std::move(newClause));
    }

    return !relationsToSplit.empty();
}

bool NormaliseDatabaseTransformer::extractIDB(AstTranslationUnit& translationUnit) {
    auto* ioTypes = translationUnit.getAnalysis<IOType>();
    auto& program = *translationUnit.getProgram();

    auto isStrictlyIDB = [&](const AstRelation* rel) {
        bool hasRules = false;
        for (const auto* clause : getClauses(program, rel->getQualifiedName())) {
            visitDepthFirst(clause->getBodyLiterals(), [&](const AstAtom& /* atom */) { hasRules = true; });
        }
        return !hasRules;
    };

    // Get all input relations
    std::set<AstQualifiedName> inputRelationNames;
    std::set<AstRelation*> inputRelations;
    for (auto* rel : program.getRelations()) {
        if (ioTypes->isInput(rel) && !isStrictlyIDB(rel)) {
            auto name = rel->getQualifiedName();
            auto usedName = rel->getQualifiedName();
            usedName.prepend("@interm_in");

            auto* newRelation = rel->clone();
            newRelation->setQualifiedName(usedName);
            program.addRelation(std::unique_ptr<AstRelation>(newRelation));

            inputRelations.insert(rel);
            inputRelationNames.insert(name);
        }
    }

    // Rename them systematically
    struct rename_relation : public AstNodeMapper {
        const std::set<AstQualifiedName>& relations;

        rename_relation(const std::set<AstQualifiedName>& relations) : relations(relations) {}

        std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
            if (auto* atom = dynamic_cast<AstAtom*>(node.get())) {
                if (contains(relations, atom->getQualifiedName())) {
                    auto newName = atom->getQualifiedName();
                    newName.prepend("@interm_in");
                    auto* renamedAtom = atom->clone();
                    renamedAtom->setQualifiedName(newName);
                    return std::unique_ptr<AstAtom>(renamedAtom);
                }
            }
            node->apply(*this);
            return node;
        }
    };
    rename_relation update(inputRelationNames);
    program.apply(update);

    // Add the new simple query output relations
    for (auto* rel : inputRelations) {
        auto name = rel->getQualifiedName();
        auto newName = rel->getQualifiedName();
        newName.prepend("@interm_in");

        auto queryHead = std::make_unique<AstAtom>(newName);
        auto queryLiteral = std::make_unique<AstAtom>(name);
        for (size_t i = 0; i < rel->getArity(); i++) {
            std::stringstream var;
            var << "@query_x" << i;
            queryHead->addArgument(std::make_unique<AstVariable>(var.str()));
            queryLiteral->addArgument(std::make_unique<AstVariable>(var.str()));
        }
        auto query = std::make_unique<AstClause>(std::move(queryHead));
        query->addToBody(std::move(queryLiteral));
        program.addClause(std::move(query));
    }

    return !inputRelationNames.empty();
}

bool NormaliseDatabaseTransformer::nameConstants(AstTranslationUnit& translationUnit) {
    auto& program = *translationUnit.getProgram();

    // Replace all constants and underscores with named variables
    struct constant_normaliser : public AstNodeMapper {
        std::set<std::unique_ptr<AstBinaryConstraint>>& constraints;
        int& changeCount;

        constant_normaliser(std::set<std::unique_ptr<AstBinaryConstraint>>& constraints, int& changeCount)
                : constraints(constraints), changeCount(changeCount) {}

        std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
            node->apply(*this);
            if (auto* arg = dynamic_cast<AstArgument*>(node.get())) {
                if (dynamic_cast<AstVariable*>(arg) == nullptr) {
                    std::stringstream name;
                    name << "@abdul" << changeCount++;
                    if (dynamic_cast<AstUnnamedVariable*>(arg) == nullptr) {
                        constraints.insert(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                                std::make_unique<AstVariable>(name.str()),
                                std::unique_ptr<AstArgument>(arg->clone())));
                    }
                    return std::make_unique<AstVariable>(name.str());
                }
            }
            return node;
        }
    };

    bool changed = false;
    for (auto* clause : program.getClauses()) {
        int changeCount = 0;
        std::set<std::unique_ptr<AstBinaryConstraint>> constraintsToAdd;
        constant_normaliser update(constraintsToAdd, changeCount);
        clause->getHead()->apply(update);
        for (AstLiteral* lit : clause->getBodyLiterals()) {
            if (auto* bc = dynamic_cast<AstBinaryConstraint*>(lit)) {
                if (bc->getOperator() == BinaryConstraintOp::EQ &&
                        dynamic_cast<AstVariable*>(bc->getLHS()) != nullptr) {
                    continue;
                }
            }
            lit->apply(update);
        }
        visitDepthFirst(*clause, [&](const AstAtom& atom) { const_cast<AstAtom&>(atom).apply(update); });
        changed |= changeCount != 0;
        for (auto& constraint : constraintsToAdd) {
            clause->addToBody(std::unique_ptr<AstLiteral>(constraint->clone()));
        }
    }

    return changed;
}

bool NormaliseDatabaseTransformer::querifyOutputRelations(AstTranslationUnit& translationUnit) {
    auto& program = *translationUnit.getProgram();

    auto isStrictlyOutput = [&](const AstRelation* rel) {
        bool strictlyOutput = true;
        size_t ruleCount = 0;

        for (const auto* clause : program.getClauses()) {
            visitDepthFirst(clause->getBodyLiterals(), [&](const AstAtom& atom) {
                if (atom.getQualifiedName() == rel->getQualifiedName()) {
                    strictlyOutput = false;
                }
            });
            if (clause->getHead()->getQualifiedName() == rel->getQualifiedName()) {
                ruleCount++;
            }
        }

        return strictlyOutput && ruleCount <= 1;
    };

    // Get all output relations
    auto* ioTypes = translationUnit.getAnalysis<IOType>();
    std::set<AstQualifiedName> outputRelationNames;
    std::set<AstRelation*> outputRelations;
    for (auto* rel : program.getRelations()) {
        if ((ioTypes->isOutput(rel) || ioTypes->isPrintSize(rel)) && !isStrictlyOutput(rel)) {
            auto name = rel->getQualifiedName();
            auto queryName = rel->getQualifiedName();
            queryName.prepend("@interm_out");

            auto* newRelation = rel->clone();
            newRelation->setQualifiedName(queryName);
            program.addRelation(std::unique_ptr<AstRelation>(newRelation));

            outputRelations.insert(rel);
            outputRelationNames.insert(name);
        }
    }

    // Rename them systematically
    struct rename_relation : public AstNodeMapper {
        const std::set<AstQualifiedName>& relations;

        rename_relation(const std::set<AstQualifiedName>& relations) : relations(relations) {}

        std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
            if (auto* atom = dynamic_cast<AstAtom*>(node.get())) {
                if (contains(relations, atom->getQualifiedName())) {
                    auto newName = atom->getQualifiedName();
                    newName.prepend("@interm_out");
                    auto* renamedAtom = atom->clone();
                    renamedAtom->setQualifiedName(newName);
                    return std::unique_ptr<AstAtom>(renamedAtom);
                }
            }
            node->apply(*this);
            return node;
        }
    };
    rename_relation update(outputRelationNames);
    program.apply(update);

    // Add the new simple query output relations
    for (auto* rel : outputRelations) {
        auto name = rel->getQualifiedName();
        auto newName = rel->getQualifiedName();
        newName.prepend("@interm_out");

        auto queryHead = std::make_unique<AstAtom>(name);
        auto queryLiteral = std::make_unique<AstAtom>(newName);
        for (size_t i = 0; i < rel->getArity(); i++) {
            std::stringstream var;
            var << "@query_x" << i;
            queryHead->addArgument(std::make_unique<AstVariable>(var.str()));
            queryLiteral->addArgument(std::make_unique<AstVariable>(var.str()));
        }
        auto query = std::make_unique<AstClause>(std::move(queryHead));
        query->addToBody(std::move(queryLiteral));
        program.addClause(std::move(query));
    }

    return !outputRelationNames.empty();
}

std::set<AstQualifiedName> AdornDatabaseTransformer::getIgnoredRelations(
        AstTranslationUnit& translationUnit) {
    auto& program = *translationUnit.getProgram();
    auto* ioTypes = translationUnit.getAnalysis<IOType>();

    std::set<AstQualifiedName> relationsToIgnore;

    // - Any relations not specified to magic-set
    std::vector<std::string> specifiedRelations = splitString(Global::config().get("magic-transform"), ',');
    if (!contains(specifiedRelations, "*")) {
        for (const AstRelation* rel : program.getRelations()) {
            if (!contains(specifiedRelations, toString(rel->getQualifiedName()))) {
                relationsToIgnore.insert(rel->getQualifiedName());
            }
        }
    }

    // - Any relations known in constant time (IDB relations)
    for (auto* rel : program.getRelations()) {
        // Input relations
        if (ioTypes->isInput(rel)) {
            relationsToIgnore.insert(rel->getQualifiedName());
            continue;
        }

        // Any relations not dependent on any atoms
        bool hasRules = false;
        for (const auto* clause : getClauses(program, rel->getQualifiedName())) {
            visitDepthFirst(clause->getBodyLiterals(), [&](const AstAtom& /* atom */) { hasRules = true; });
        }
        if (!hasRules) {
            relationsToIgnore.insert(rel->getQualifiedName());
        }
    }

    // - Any relation with a neglabel
    visitDepthFirst(program, [&](const AstAtom& atom) {
        const auto& qualifiers = atom.getQualifiedName().getQualifiers();
        if (!qualifiers.empty() && qualifiers[0] == "@neglabel") {
            relationsToIgnore.insert(atom.getQualifiedName());
        }
    });

    // - Any relation with a clause containing float-related binary constraints
    const std::set<BinaryConstraintOp> floatOps(
            {BinaryConstraintOp::FEQ, BinaryConstraintOp::FNE, BinaryConstraintOp::FLE,
                    BinaryConstraintOp::FGE, BinaryConstraintOp::FLT, BinaryConstraintOp::FGT});
    for (const auto* clause : program.getClauses()) {
        visitDepthFirst(*clause, [&](const AstBinaryConstraint& bc) {
            if (contains(floatOps, bc.getOperator())) {
                relationsToIgnore.insert(clause->getHead()->getQualifiedName());
            }
        });
    }

    // - Any relation with a clause containing order-dependent functors
    const std::set<FunctorOp> orderDepFuncOps(
            {FunctorOp::MOD, FunctorOp::FDIV, FunctorOp::DIV, FunctorOp::UMOD});
    for (const auto* clause : program.getClauses()) {
        visitDepthFirst(*clause, [&](const AstIntrinsicFunctor& functor) {
            if (contains(orderDepFuncOps, functor.getFunctionInfo()->op)) {
                relationsToIgnore.insert(clause->getHead()->getQualifiedName());
            }
        });
    }

    // - Any eqrel relation
    for (auto* rel : program.getRelations()) {
        if (rel->getRepresentation() == RelationRepresentation::EQREL) {
            relationsToIgnore.insert(rel->getQualifiedName());
        }
    }

    // - Any relation with execution plans
    for (auto* clause : program.getClauses()) {
        if (clause->getExecutionPlan() != nullptr) {
            relationsToIgnore.insert(clause->getHead()->getQualifiedName());
        }
    }

    return relationsToIgnore;
}

bool AdornDatabaseTransformer::transform(AstTranslationUnit& translationUnit) {
    auto& program = *translationUnit.getProgram();
    auto* ioTypes = translationUnit.getAnalysis<IOType>();

    // Get relations to ignore
    auto relationsToIgnore = getIgnoredRelations(translationUnit);

    // Adorned predicate structure
    using adorned_predicate = std::pair<AstQualifiedName, std::string>;
    auto getAdornmentID = [&](const adorned_predicate& pred) {
        if (pred.second == "") return pred.first;
        AstQualifiedName adornmentID(pred.first);
        std::stringstream adornmentMarker;
        adornmentMarker << "{" << pred.second << "}";
        adornmentID.append(adornmentMarker.str());
        return adornmentID;
    };

    // Process data-structures
    std::vector<std::unique_ptr<AstClause>> adornedClauses;
    std::vector<std::unique_ptr<AstClause>> redundantClauses;
    std::vector<std::unique_ptr<AstRelation>> relationsToAdd;

    std::set<adorned_predicate> headAdornmentsToDo;
    std::set<AstQualifiedName> headAdornmentsSeen;
    std::set<AstQualifiedName> outputRelations;

    // Output relations trigger the adornment process
    for (const auto* rel : program.getRelations()) {
        if (ioTypes->isOutput(rel) || ioTypes->isPrintSize(rel)) {
            auto adornment = std::make_pair(rel->getQualifiedName(), "");
            auto adornmentID = getAdornmentID(adornment);
            assert(!contains(headAdornmentsSeen, adornmentID) && "unexpected repeat output relation");
            headAdornmentsToDo.insert(adornment);
            headAdornmentsSeen.insert(adornmentID);
            outputRelations.insert(rel->getQualifiedName());
        } else if (contains(relationsToIgnore, rel->getQualifiedName())) {
            auto adornment = std::make_pair(rel->getQualifiedName(), "");
            auto adornmentID = getAdornmentID(adornment);
            headAdornmentsToDo.insert(adornment);
            headAdornmentsSeen.insert(adornmentID);
        }
    }

    // Keep going while there's things to adorn
    while (!headAdornmentsToDo.empty()) {
        // Pop off the next head adornment to do
        auto headAdornment = *(headAdornmentsToDo.begin());
        headAdornmentsToDo.erase(headAdornmentsToDo.begin());
        const auto& relName = headAdornment.first;
        const auto* rel = getRelation(program, relName);
        assert(rel != nullptr && "relation does not exist");
        const auto& adornmentMarker = headAdornment.second;

        // Add the adorned relation if needed
        if (adornmentMarker != "") {
            auto adornedRelation = std::make_unique<AstRelation>(getAdornmentID(headAdornment));
            for (const auto* attr : rel->getAttributes()) {
                adornedRelation->addAttribute(std::unique_ptr<AstAttribute>(attr->clone()));
            }
            program.addRelation(std::move(adornedRelation));
        }

        // Adorn every clause correspondingly
        for (const AstClause* clause : getClauses(program, relName)) {
            const auto* headAtom = clause->getHead();
            const auto& headArguments = headAtom->getArguments();
            BindingStore variableBindings(clause);

            // Create the adorned clause with an empty body
            auto adornedClause = std::make_unique<AstClause>();
            auto adornedHeadAtomName = adornmentMarker == "" ? relName : getAdornmentID(headAdornment);
            if (adornmentMarker == "") {
                redundantClauses.push_back(std::unique_ptr<AstClause>(clause->clone()));
            }
            auto adornedHeadAtom = std::make_unique<AstAtom>(adornedHeadAtomName);
            assert((adornmentMarker == "" || headAtom->getArity() == adornmentMarker.length()) &&
                    "adornment marker should correspond to head atom variables");
            for (size_t i = 0; i < adornmentMarker.length(); i++) {
                const auto* var = dynamic_cast<AstVariable*>(headArguments[i]);
                assert(var != nullptr && "expected only variables in head");
                if (adornmentMarker[i] == 'b') {
                    variableBindings.bindHeadVariable(var->getName());
                }
            }

            for (const auto* arg : headArguments) {
                const auto* var = dynamic_cast<const AstVariable*>(arg);
                assert(var != nullptr && "expected only variables in head");
                adornedHeadAtom->addArgument(std::unique_ptr<AstArgument>(var->clone()));
            }

            adornedClause->setHead(std::move(adornedHeadAtom));

            // Check through for variables bound in the body
            visitDepthFirst(*clause, [&](const AstBinaryConstraint& constr) {
                if (constr.getOperator() == BinaryConstraintOp::EQ &&
                        dynamic_cast<AstVariable*>(constr.getLHS()) &&
                        dynamic_cast<AstConstant*>(constr.getRHS())) {
                    const auto* var = dynamic_cast<AstVariable*>(constr.getLHS());
                    variableBindings.bindVariable(var->getName());
                }
            });

            // Add in adorned body literals
            std::vector<std::unique_ptr<AstLiteral>> adornedBodyLiterals;
            for (const auto* lit : clause->getBodyLiterals()) {
                if (const auto* atom = dynamic_cast<const AstAtom*>(lit)) {
                    // Form the appropriate adornment marker
                    std::stringstream atomAdornment;

                    if (!contains(relationsToIgnore, atom->getQualifiedName())) {
                        for (const auto* arg : atom->getArguments()) {
                            const auto* var = dynamic_cast<const AstVariable*>(arg);
                            assert(var != nullptr && "expected only variables in atom");
                            atomAdornment << (variableBindings.isBound(var->getName()) ? "b" : "f");
                        }
                    }

                    auto currAtomAdornment = std::make_pair(atom->getQualifiedName(), atomAdornment.str());
                    auto currAtomAdornmentID = getAdornmentID(currAtomAdornment);

                    // Add the adorned version to the clause
                    auto adornedBodyAtom = std::unique_ptr<AstAtom>(atom->clone());
                    adornedBodyAtom->setQualifiedName(currAtomAdornmentID);
                    adornedBodyLiterals.push_back(std::move(adornedBodyAtom));

                    // Add to the ToDo queue if needed
                    if (!contains(headAdornmentsSeen, currAtomAdornmentID)) {
                        headAdornmentsSeen.insert(currAtomAdornmentID);
                        headAdornmentsToDo.insert(currAtomAdornment);
                    }

                    // All arguments are now bound
                    for (const auto* arg : atom->getArguments()) {
                        const auto* var = dynamic_cast<const AstVariable*>(arg);
                        assert(var != nullptr && "expected only variables in atom");
                        variableBindings.bindVariable(var->getName());
                    }
                } else {
                    adornedBodyLiterals.push_back(std::unique_ptr<AstLiteral>(lit->clone()));
                }
            }
            adornedClause->setBodyLiterals(std::move(adornedBodyLiterals));

            // Add in plans if needed
            if (clause->getExecutionPlan() != nullptr) {
                assert(contains(relationsToIgnore, clause->getHead()->getQualifiedName()) &&
                        "clauses with plans should be ignored");
                adornedClause->setExecutionPlan(
                        std::unique_ptr<AstExecutionPlan>(clause->getExecutionPlan()->clone()));
            }

            adornedClauses.push_back(std::move(adornedClause));
        }
    }

    // Swap over the redundant clauses with the adorned clauses
    for (const auto& clause : redundantClauses) {
        program.removeClause(clause.get());
    }

    for (auto& clause : adornedClauses) {
        program.addClause(std::unique_ptr<AstClause>(clause->clone()));
    }

    return !adornedClauses.empty() || !redundantClauses.empty();
}

AstQualifiedName getNegativeLabel(const AstQualifiedName& name) {
    AstQualifiedName newName(name);
    newName.prepend("@neglabel");
    return newName;
}

bool LabelDatabaseTransformer::transform(AstTranslationUnit& translationUnit) {
    bool changed = false;
    changed |= runNegativeLabelling(translationUnit);
    if (changed) translationUnit.invalidateAnalyses();
    changed |= runPositiveLabelling(translationUnit);
    return changed;
}

bool LabelDatabaseTransformer::runNegativeLabelling(AstTranslationUnit& translationUnit) {
    const auto& sccGraph = *translationUnit.getAnalysis<SCCGraph>();
    const auto& ioTypes = *translationUnit.getAnalysis<IOType>();
    auto& program = *translationUnit.getProgram();

    std::set<AstQualifiedName> relationsToLabel;
    std::set<AstQualifiedName> inputRelations;
    std::set<AstClause*> clausesToAdd;

    for (auto* rel : program.getRelations()) {
        if (ioTypes.isInput(rel)) {
            inputRelations.insert(rel->getQualifiedName());
        }
    }

    // Rename appearances of negated predicates
    visitDepthFirst(program, [&](const AstNegation& neg) {
        auto* atom = neg.getAtom();
        auto relName = atom->getQualifiedName();
        if (contains(inputRelations, relName)) return;
        atom->setQualifiedName(getNegativeLabel(relName));
        relationsToLabel.insert(relName);
    });
    visitDepthFirst(program, [&](const AstAggregator& aggr) {
        visitDepthFirst(aggr, [&](const AstAtom& atom) {
            auto relName = atom.getQualifiedName();
            if (contains(inputRelations, relName)) return;
            const_cast<AstAtom&>(atom).setQualifiedName(getNegativeLabel(relName));
            relationsToLabel.insert(relName);
        });
    });

    // Add the rules for negatively-labelled predicates

    /* Atom labeller */
    struct labelAtoms : public AstNodeMapper {
        const std::set<AstQualifiedName>& sccFriends;
        std::set<AstQualifiedName>& relsToLabel;
        labelAtoms(const std::set<AstQualifiedName>& sccFriends, std::set<AstQualifiedName>& relsToLabel)
                : sccFriends(sccFriends), relsToLabel(relsToLabel) {}
        std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
            node->apply(*this);
            if (auto* atom = dynamic_cast<AstAtom*>(node.get())) {
                if (contains(sccFriends, atom->getQualifiedName())) {
                    auto labelledAtom = std::unique_ptr<AstAtom>(atom->clone());
                    labelledAtom->setQualifiedName(getNegativeLabel(atom->getQualifiedName()));
                    relsToLabel.insert(atom->getQualifiedName());
                    return labelledAtom;
                }
            }
            return node;
        }
    };

    // Copy over the rules for negatively-labelled relations one stratum at a time
    for (size_t stratum = 0; stratum < sccGraph.getNumberOfSCCs(); stratum++) {
        const auto& rels = sccGraph.getInternalRelations(stratum);
        std::set<AstQualifiedName> relNames;
        for (const auto* rel : rels) {
            relNames.insert(rel->getQualifiedName());
        }

        for (const auto* rel : rels) {
            const auto& relName = rel->getQualifiedName();
            for (auto* clause : getClauses(program, relName)) {
                auto* neggedClause = clause->clone();
                labelAtoms update(relNames, relationsToLabel);
                neggedClause->apply(update);
                clausesToAdd.insert(neggedClause);
            }
        }
    }

    // Add in all the relations that were labelled
    for (const auto& relName : relationsToLabel) {
        const auto* originalRel = getRelation(program, relName);
        assert(originalRel != nullptr && "unlabelled relation does not exist");
        auto labelledRelation = std::unique_ptr<AstRelation>(originalRel->clone());
        labelledRelation->setQualifiedName(getNegativeLabel(relName));
        program.addRelation(std::move(labelledRelation));
    }

    // Add in all the negged clauses
    for (auto* clause : clausesToAdd) {
        program.addClause(std::unique_ptr<AstClause>(clause));
    }

    return !relationsToLabel.empty();
}

bool LabelDatabaseTransformer::runPositiveLabelling(AstTranslationUnit& translationUnit) {
    bool changed = false;

    std::set<AstClause*> clausesToAdd;

    auto& program = *translationUnit.getProgram();
    const auto& sccGraph = *translationUnit.getAnalysis<SCCGraph>();
    const auto& precedenceGraph = translationUnit.getAnalysis<PrecedenceGraph>()->graph();
    const auto& ioTypes = *translationUnit.getAnalysis<IOType>();

    auto isNegativelyLabelled = [&](const AstQualifiedName& name) {
        auto qualifiers = name.getQualifiers();
        assert(!qualifiers.empty() && "unexpected empty qualifier list");
        return qualifiers[0] == "@neglabel";
    };

    /* Atom labeller */
    struct labelAtoms : public AstNodeMapper {
        const AstProgram& program;
        const SCCGraph& sccGraph;
        const std::map<size_t, size_t>& stratumCounts;
        const std::set<AstQualifiedName>& atomsToRelabel;
        labelAtoms(const AstProgram& program, const SCCGraph& sccGraph, const std::map<size_t, size_t>& stratumCounts, const std::set<AstQualifiedName>& atomsToRelabel) : program(program), sccGraph(sccGraph), stratumCounts(stratumCounts), atomsToRelabel(atomsToRelabel) {}

        std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
            node->apply(*this);
            if (auto* atom = dynamic_cast<AstAtom*>(node.get())) {
                auto relName = atom->getQualifiedName();
                if (contains(atomsToRelabel, relName)) {
                    size_t relStratum = sccGraph.getSCC(getRelation(program, relName));
                    auto relabelledAtom = std::unique_ptr<AstAtom>(atom->clone());
                    auto newName = AstQualifiedName(relName);
                    std::stringstream label;
                    label << "@poscopy_" << stratumCounts.at(relStratum) + 1;
                    newName.prepend(label.str());
                    relabelledAtom->setQualifiedName(newName);
                    return relabelledAtom;
                }
            }
            return node;
        }
    };

    std::set<AstQualifiedName> inputRelations;
    for (auto* rel : program.getRelations()) {
        if (ioTypes.isInput(rel)) {
            inputRelations.insert(rel->getQualifiedName());
        }
    }

    std::set<size_t> labelledStrata;
    std::map<size_t, size_t> labelledStrataCopyCount;
    std::map<size_t, std::set<size_t>> dependentStrata;
    for (size_t stratum = 0; stratum < sccGraph.getNumberOfSCCs(); stratum++) {
        dependentStrata[stratum] = std::set<size_t>();
        size_t neglabelCount = 0;
        const auto& stratumRels = sccGraph.getInternalRelations(stratum);
        for (const auto* rel : stratumRels) {
            if (isNegativelyLabelled(rel->getQualifiedName())) {
                neglabelCount++;
            }
        }
        assert((neglabelCount == 0 || neglabelCount == stratumRels.size()) &&
                "stratum cannot contain a mix of neglabelled and unlabelled relations");
        if (neglabelCount > 0) {
            labelledStrata.insert(stratum);
        } else {
            labelledStrataCopyCount[stratum] = 0;
        }
    }
    for (const auto* rel : program.getRelations()) {
        size_t stratum = sccGraph.getSCC(rel);
        precedenceGraph.visitDepthFirst(rel, [&](const auto* dependentRel) {
            dependentStrata[stratum].insert(sccGraph.getSCC(dependentRel));
        });
    }

    for (size_t stratum = 0; stratum < sccGraph.getNumberOfSCCs(); stratum++) {
        if (!contains(labelledStrata, stratum)) continue;

        const auto& stratumRels = sccGraph.getInternalRelations(stratum);

        // Number the positive derived literals in the associated clauses
        for (const auto* rel : stratumRels) {
            assert(isNegativelyLabelled(rel->getQualifiedName()) && "should only be looking at neglabelled strata");
            const auto& clauses = getClauses(program, *rel);
            std::set<AstQualifiedName> relsToCopy;
            for (const auto* clause : clauses) {
                visitDepthFirst(*clause, [&](const AstAtom& atom) {
                    const auto& name = atom.getQualifiedName();
                    if (!contains(inputRelations, name) && !isNegativelyLabelled(name)) {
                        relsToCopy.insert(name);
                    }
                });
            }
            for (auto* clause : clauses) {
                labelAtoms update(program, sccGraph, labelledStrataCopyCount, relsToCopy);
                clause->apply(update);
            }
        }

        // Create the rules for the newly positive labelled literals
        std::set<AstQualifiedName> relsToCopy;
        for (const auto* rel : program.getRelations()) {
            const auto& relName = rel->getQualifiedName();
            if (!contains(inputRelations, relName) && !isNegativelyLabelled(relName)) {
                relsToCopy.insert(relName);
            }
        }

        for (int preStratum = stratum - 1; preStratum >= 0; preStratum--) {
            if (contains(labelledStrata, preStratum)) continue;
            if (contains(dependentStrata[preStratum], stratum)) {
                const auto& preStratumRels = sccGraph.getInternalRelations(preStratum);
                std::set<AstQualifiedName> relsToLabel;
                for (const auto* rel : preStratumRels) {
                    relsToLabel.insert(rel->getQualifiedName());
                }
                for (const auto* rel : preStratumRels) {
                    if (contains(inputRelations, rel->getQualifiedName())) continue;
                    for (const auto* clause : getClauses(program, rel->getQualifiedName())) {
                        auto* labelledClause = clause->clone();
                        labelAtoms update(program, sccGraph, labelledStrataCopyCount, relsToCopy);
                        labelledClause->apply(update);
                        program.addClause(std::unique_ptr<AstClause>(labelledClause));
                    }
                }
                labelledStrataCopyCount[preStratum]++;
            }
        }
    }

    // Add the new relations in
    for (auto& pair : labelledStrataCopyCount) {
        size_t stratum = pair.first;
        const auto& stratumRels = sccGraph.getInternalRelations(stratum);
        for (size_t copy = 0; copy < pair.second; copy++) {
            for (auto* rel : stratumRels) {
                std::stringstream label;
                label << "@poscopy_" << copy+1;
                auto newName = AstQualifiedName(rel->getQualifiedName());
                newName.prepend(label.str());
                auto* newRelation = rel->clone();
                newRelation->setQualifiedName(newName);
                program.addRelation(std::unique_ptr<AstRelation>(newRelation));
            }
        }
    }

    return changed;
}

bool MagicSetTransformer::transform(AstTranslationUnit& translationUnit) {
    auto& program = *translationUnit.getProgram();
    std::set<std::unique_ptr<AstClause>> clausesToRemove;
    std::set<std::unique_ptr<AstClause>> clausesToAdd;

    std::set<AstQualifiedName> magicPredicatesSeen;

    /** Checks if a given relation name is adorned */
    auto isAdorned = [&](const AstQualifiedName& name) {
        auto qualifiers = name.getQualifiers();
        assert(!qualifiers.empty() && "unexpected empty qualifier list");
        auto finalQualifier = qualifiers[qualifiers.size() - 1];
        assert(finalQualifier.length() > 0 && "unexpected empty qualifier");
        if (finalQualifier[0] == '{') {
            assert(finalQualifier[finalQualifier.length() - 1] == '}' && "unterminated adornment string");
            for (size_t i = 1; i < finalQualifier.length() - 1; i++) {
                char curBindingType = finalQualifier[i];
                assert((curBindingType == 'b' || curBindingType == 'f') &&
                        "unexpected binding type in adornment");
            }
            return true;
        }
        return false;
    };

    /** Retrieves the adornment encoded in a given relation name */
    auto getAdornment = [&](const AstQualifiedName& name) {
        assert(isAdorned(name) && "relation not adorned");
        auto qualifiers = name.getQualifiers();
        auto finalQualifier = qualifiers[qualifiers.size() - 1];
        std::stringstream binding;
        for (size_t i = 1; i < finalQualifier.length() - 1; i++) {
            binding << finalQualifier[i];
        }
        return binding.str();
    };

    /** Create the magic atom associated with the given (relation, adornment) pair */
    auto createMagicAtom = [&](const AstAtom* atom) {
        auto name = atom->getQualifiedName();
        auto magicRelName = AstQualifiedName(name);
        magicRelName.prepend("@magic");

        auto args = atom->getArguments();
        auto adornmentMarker = getAdornment(name);
        auto magicAtom = std::make_unique<AstAtom>(magicRelName);
        for (size_t i = 0; i < args.size(); i++) {
            if (adornmentMarker[i] == 'b') {
                magicAtom->addArgument(std::unique_ptr<AstArgument>(args[i]->clone()));
            }
        }

        if (!contains(magicPredicatesSeen, magicRelName)) {
            magicPredicatesSeen.insert(magicRelName);

            auto attributes = getRelation(program, name)->getAttributes();
            auto magicRelation = std::make_unique<AstRelation>(magicRelName);
            for (size_t i = 0; i < attributes.size(); i++) {
                if (adornmentMarker[i] == 'b') {
                    magicRelation->addAttribute(std::unique_ptr<AstAttribute>(attributes[i]->clone()));
                }
            }
            program.addRelation(std::move(magicRelation));
        }

        return magicAtom;
    };

    /** Create magic clause focused on a specific atom */
    auto createMagicClause = [&](const AstAtom* atom,
                                     const std::vector<std::unique_ptr<AstAtom>>& constrainingAtoms,
                                     const std::vector<const AstBinaryConstraint*> eqConstraints) {
        auto magicHead = createMagicAtom(atom);
        auto magicClause = std::make_unique<AstClause>();
        for (const auto& bindingAtom : constrainingAtoms) {
            magicClause->addToBody(std::unique_ptr<AstAtom>(bindingAtom->clone()));
        }

        std::set<std::string> seenVariables;
        visitDepthFirst(
                constrainingAtoms, [&](const AstVariable& var) { seenVariables.insert(var.getName()); });
        visitDepthFirst(*magicHead, [&](const AstVariable& var) { seenVariables.insert(var.getName()); });
        bool fixpointReached = false;
        while (!fixpointReached) {
            fixpointReached = true;
            for (const auto* eqConstraint : eqConstraints) {
                if (dynamic_cast<AstRecordInit*>(eqConstraint->getRHS()) != nullptr) {
                    const auto* var = dynamic_cast<const AstVariable*>(eqConstraint->getLHS());
                    if (var != nullptr && contains(seenVariables, var->getName())) {
                        visitDepthFirst(*eqConstraint, [&](const AstVariable& subVar) {
                            if (!contains(seenVariables, subVar.getName())) {
                                fixpointReached = false;
                                seenVariables.insert(subVar.getName());
                            }
                        });
                    }
                }
                if (dynamic_cast<AstRecordInit*>(eqConstraint->getLHS()) != nullptr) {
                    const auto* var = dynamic_cast<const AstVariable*>(eqConstraint->getRHS());
                    if (var != nullptr && contains(seenVariables, var->getName())) {
                        visitDepthFirst(*eqConstraint, [&](const AstVariable& subVar) {
                            if (!contains(seenVariables, subVar.getName())) {
                                fixpointReached = false;
                                seenVariables.insert(subVar.getName());
                            }
                        });
                    }
                }
            }
        }

        for (const auto* eqConstraint : eqConstraints) {
            bool addConstraint = true;
            visitDepthFirst(*eqConstraint, [&](const AstVariable& var) {
                if (!contains(seenVariables, var.getName())) {
                    addConstraint = false;
                }
            });

            if (addConstraint) {
                magicClause->addToBody(std::unique_ptr<AstBinaryConstraint>(eqConstraint->clone()));
            }
        }

        magicClause->setHead(std::move(magicHead));
        return magicClause;
    };

    /** Get all equality constraints in a clause */
    auto getEqualityConstraints = [&](const AstClause* clause) {
        std::vector<const AstBinaryConstraint*> equalityConstraints;
        for (const auto* lit : clause->getBodyLiterals()) {
            const auto* bc = dynamic_cast<const AstBinaryConstraint*>(lit);
            if (bc == nullptr || bc->getOperator() != BinaryConstraintOp::EQ) continue;
            if (dynamic_cast<AstVariable*>(bc->getLHS()) != nullptr ||
                    dynamic_cast<AstConstant*>(bc->getRHS()) != nullptr) {
                bool containsAggrs = false;
                visitDepthFirst(*bc, [&](const AstAggregator& /* aggr */) { containsAggrs = true; });
                if (!containsAggrs) {
                    equalityConstraints.push_back(bc);
                }
            }
        }
        return equalityConstraints;
    };

    /** Perform the Magic Set Transformation */
    for (const auto* clause : program.getClauses()) {
        clausesToRemove.insert(std::unique_ptr<AstClause>(clause->clone()));

        const auto* head = clause->getHead();
        auto relName = head->getQualifiedName();

        // (1) Add the refined clause
        if (!isAdorned(relName)) {
            // Unadorned relations need not be refined, as every possible tuple is relevant
            clausesToAdd.insert(std::unique_ptr<AstClause>(clause->clone()));
        } else {
            // Refine the clause with a prepended magic atom
            auto magicAtom = createMagicAtom(head);
            auto refinedClause = std::make_unique<AstClause>();
            refinedClause->setHead(std::unique_ptr<AstAtom>(head->clone()));
            refinedClause->addToBody(std::unique_ptr<AstAtom>(magicAtom->clone()));
            for (auto* literal : clause->getBodyLiterals()) {
                refinedClause->addToBody(std::unique_ptr<AstLiteral>(literal->clone()));
            }
            clausesToAdd.insert(std::move(refinedClause));
        }

        // (2) Add the associated magic rules
        std::vector<const AstBinaryConstraint*> eqConstraints = getEqualityConstraints(clause);
        std::vector<std::unique_ptr<AstAtom>> atomsToTheLeft;
        if (isAdorned(relName)) {
            // Add the specialising head atom
            // Output relations are not specialised, and so the head will not contribute to specialisation
            atomsToTheLeft.push_back(createMagicAtom(clause->getHead()));
        }
        for (const auto* lit : clause->getBodyLiterals()) {
            const auto* atom = dynamic_cast<const AstAtom*>(lit);
            if (atom == nullptr) continue;
            if (!isAdorned(atom->getQualifiedName())) {
                atomsToTheLeft.push_back(std::unique_ptr<AstAtom>(atom->clone()));
                continue;
            }
            auto magicClause = createMagicClause(atom, atomsToTheLeft, eqConstraints);
            atomsToTheLeft.push_back(std::unique_ptr<AstAtom>(atom->clone()));
            clausesToAdd.insert(std::move(magicClause));
        }
    }

    for (auto& clause : clausesToAdd) {
        program.addClause(std::unique_ptr<AstClause>(clause->clone()));
    }
    for (const auto& clause : clausesToRemove) {
        program.removeClause(clause.get());
    }

    return !clausesToRemove.empty() || !clausesToAdd.empty();
}

}  // namespace souffle

namespace souffle {

/* general functions */

// checks whether the adorned version of two predicates is equal
bool isEqualAdornment(const AstQualifiedName& pred1, const std::string& adorn1, const AstQualifiedName& pred2,
        const std::string& adorn2) {
    return ((pred1 == pred2) && (adorn1 == adorn2));
}

// checks whether a given adorned predicate is contained within a set
bool contains(std::set<AdornedPredicate> adornedPredicates, const AstQualifiedName& atomName,
        const std::string& atomAdornment) {
    for (AdornedPredicate seenPred : adornedPredicates) {
        if (isEqualAdornment(seenPred.getQualifiedName(), seenPred.getAdornment(), atomName, atomAdornment)) {
            return true;
        }
    }
    return false;
}

// checks whether a string begins with a given string
bool hasPrefix(const std::string& str, const std::string& prefix) {
    return str.substr(0, prefix.size()) == prefix;
}

// checks whether the given relation is generated by an aggregator
bool isAggRel(const AstQualifiedName& rel) {
    // TODO (azreika): this covers too much (e.g. user-defined __agg_rel_x)
    //                 need a way to determine if created by aggregates
    return hasPrefix(rel.getQualifiers()[0], "__agg_rel_");
}

// gets the position of the final underscore in a given string
int getEndpoint(std::string mainName) {
    int endpt = mainName.size() - 1;
    while (endpt >= 0 && mainName[endpt] != '_') {
        endpt--;
    }
    if (endpt == -1) {
        endpt = mainName.size();
    }
    return endpt;
}

/* argument-related functions */

// returns the string representation of a given argument
std::string getString(const AstArgument* arg) {
    std::stringstream argStream;
    argStream << *arg;
    return argStream.str();
}

// checks whether a given record or functor is bound
bool isBoundComposite(const AstVariable* compositeVariable, const std::set<std::string>& boundArgs,
        OldBindingStore& compositeBindings) {
    std::string variableName = compositeVariable->getName();
    if (contains(boundArgs, variableName)) {
        return true;
    }

    bool bound = true;

    // a composite argument is bound iff all its subvariables are bound
    auto dependencies = compositeBindings.getVariableDependencies(variableName);
    for (const std::string& var : dependencies) {
        if (!contains(boundArgs, var)) {
            bound = false;
        }
    }

    if (bound) {
        // composite variable bound only because its constituent variables are bound
        compositeBindings.addVariableBoundComposite(variableName);
    }

    return bound;
}

bool isBoundArgument(
        AstArgument* arg, const std::set<std::string>& boundArgs, OldBindingStore& compositeBindings) {
    if (auto* var = dynamic_cast<AstVariable*>(arg)) {
        std::string variableName = var->getName();
        if (hasPrefix(variableName, "+functor") || hasPrefix(variableName, "+record")) {
            if (isBoundComposite(var, boundArgs, compositeBindings)) {
                return true;
            }
        }

        if (contains(boundArgs, variableName)) {
            return true;  // found a bound argument, so can stop
        }
    } else {
        fatal("incomplete checks (MST)");
    }

    return false;
}

// checks whether a given atom has a bound argument
bool hasBoundArgument(
        AstAtom* atom, const std::set<std::string>& boundArgs, OldBindingStore& compositeBindings) {
    for (AstArgument* arg : atom->getArguments()) {
        if (isBoundArgument(arg, boundArgs, compositeBindings)) {
            return true;
        }
    }
    return false;
}

// checks whether the lhs is bound by a binary constraint (and is not bound yet)
bool isBindingConstraint(AstArgument* lhs, AstArgument* rhs, std::set<std::string> boundArgs) {
    std::string lhs_name = getString(lhs);
    std::string rhs_name = getString(rhs);

    // only want to check variables we have not bound yet
    if ((dynamic_cast<AstVariable*>(lhs) != nullptr) && (boundArgs.find(lhs_name) == boundArgs.end())) {
        // return true if the rhs is a bound variable or a constant
        if ((dynamic_cast<AstVariable*>(rhs) != nullptr) && (boundArgs.find(rhs_name) != boundArgs.end())) {
            return true;
        } else if (dynamic_cast<AstConstant*>(rhs) != nullptr) {
            return true;
        }
    }
    return false;
}

// checks whether the clause involves aggregators
bool containsAggregators(AstClause* clause) {
    bool found = false;

    // check for aggregators
    visitDepthFirst(*clause, [&](const AstAggregator&) { found = true; });

    return found;
}

/* program-adding related functions */

// returns the new source location of a newly-created node
SrcLocation nextSrcLoc(SrcLocation orig) {
    static int pos = 0;
    pos += 1;

    SrcLocation newLoc;
    newLoc.filenames = orig.filenames;
    if (orig.filenames.empty()) {
        newLoc.filenames.emplace_back("[MAGIC_FILE]");
    } else {
        newLoc.filenames.back() = orig.filenames.back() + "[MAGIC_FILE]";
    }
    newLoc.start.line = pos;
    newLoc.end.line = pos;
    newLoc.start.column = 0;
    newLoc.end.column = 1;

    return newLoc;
}

// returns the next available relation name prefixed by "newedb"
std::string getNextEdbName(AstProgram* program) {
    static int edbNum = 0;
    std::stringstream newEdbName;

    // find the next unused relation name of the form "newedbX", X an integer
    do {
        newEdbName.str("");  // check
        edbNum++;
        newEdbName << "newedb" << edbNum;
    } while (getRelation(*program, newEdbName.str()) != nullptr);

    return newEdbName.str();
}

// create a new relation with a given name based on a previous relation
AstRelation* createNewRelation(AstRelation* original, const AstQualifiedName& newName) {
    // duplicate the relation, but without any qualifiers
    auto* newRelation = new AstRelation();
    newRelation->setSrcLoc(nextSrcLoc(original->getSrcLoc()));
    newRelation->setQualifiedName(newName);
    newRelation->setAttributes(clone(original->getAttributes()));
    newRelation->setRepresentation(original->getRepresentation());
    return newRelation;
}

// returns the magic-set identifier corresponding to a given relation (mX_relation)
AstQualifiedName createMagicIdentifier(const AstQualifiedName& relationName, size_t outputNumber) {
    std::vector<std::string> relationNames = relationName.getQualifiers();

    // change the base name to magic-relation format
    std::stringstream newMainName;
    newMainName.str("");
    newMainName << "+m" << outputNumber << "_" << relationNames[0];  // use "+m" to avoid conflicts
    AstQualifiedName newRelationName(newMainName.str());

    // copy over the other relation names
    for (size_t i = 1; i < relationNames.size(); i++) {
        newRelationName.append(relationNames[i]);
    }

    return newRelationName;
}

// returns the adorned identifier corresponding to a given relation and adornment (relationName_adornment)
AstQualifiedName createAdornedIdentifier(const AstQualifiedName& relationName, const std::string& adornment) {
    std::vector<std::string> relationNames = relationName.getQualifiers();

    // change the base name
    std::stringstream newMainName;
    newMainName.str("");
    // add a '+' to avoid name conflict
    newMainName << relationNames[0] << "+_" << adornment;
    AstQualifiedName newRelationName(newMainName.str());

    // add in the other names
    for (size_t i = 1; i < relationNames.size(); i++) {
        newRelationName.append(relationNames[i]);
    }

    return newRelationName;
}

// returns the requested substring of a given identifier
AstQualifiedName createSubIdentifier(const AstQualifiedName& relationName, size_t start, size_t length) {
    std::vector<std::string> relationNames = relationName.getQualifiers();

    // get the substring of the base name
    std::stringstream newMainName;
    newMainName.str("");
    newMainName << relationNames[0].substr(start, length);
    AstQualifiedName newRelationName(newMainName.str());

    // add in the remaining names
    for (size_t i = 1; i < relationNames.size(); i++) {
        newRelationName.append(relationNames[i]);
    }

    return newRelationName;
}

/* functions to find atoms to ignore */

// add all atoms within a clause that contain aggregators to the ignored relations list
std::set<AstQualifiedName> addAggregators(AstClause* clause, std::set<AstQualifiedName> ignoredNames) {
    std::set<AstQualifiedName> retVal = std::move(ignoredNames);

    visitDepthFirst(*clause, [&](const AstAggregator& aggregator) {
        visitDepthFirst(aggregator, [&](const AstAtom& atom) { retVal.insert(atom.getQualifiedName()); });
    });

    return retVal;
}

// Given a set of relations R, add in all relations that use one of these
// relations in their clauses. Repeat until a fixed point is reached.
std::set<AstQualifiedName> addBackwardDependencies(
        const AstProgram* program, std::set<AstQualifiedName> relations) {
    bool relationsAdded = false;
    std::set<AstQualifiedName> result;

    for (AstQualifiedName relName : relations) {
        // Add the relation itself
        result.insert(relName);
    }

    // Add in all relations that need to use an ignored relation
    for (AstRelation* rel : program->getRelations()) {
        for (AstClause* clause : getClauses(*program, *rel)) {
            AstQualifiedName clauseHeadName = clause->getHead()->getQualifiedName();
            if (!contains(relations, clauseHeadName)) {
                // Clause hasn't been added yet, so check if it needs to be added
                visitDepthFirst(*clause, [&](const AstAtom& subatom) {
                    AstQualifiedName atomName = subatom.getQualifiedName();
                    if (contains(relations, atomName)) {
                        // Clause uses one of the given relations
                        result.insert(clauseHeadName);

                        // Clause name hasn't been seen yet, so fixed point not reached
                        relationsAdded = true;
                    }
                });
            }
        }
    }

    if (relationsAdded) {
        // Keep going until we reach a fixed point
        return addBackwardDependencies(program, result);
    } else {
        return result;
    }
}

// Given a set of relations R, add in all relations that they use in their clauses.
// Repeat until a fixed point is reached.
std::set<AstQualifiedName> addForwardDependencies(
        const AstProgram* program, std::set<AstQualifiedName> relations) {
    bool relationsAdded = false;
    std::set<AstQualifiedName> result;

    for (AstQualifiedName relName : relations) {
        // Add the relation itself
        result.insert(relName);

        // Add in all the relations that it needs to use
        AstRelation* associatedRelation = getRelation(*program, relName);
        for (AstClause* clause : getClauses(*program, *associatedRelation)) {
            visitDepthFirst(*clause, [&](const AstAtom& subatom) {
                AstQualifiedName atomName = subatom.getQualifiedName();
                result.insert(atomName);
                if (!contains(relations, atomName)) {
                    // Hasn't been seen yet, so fixed point not reached
                    relationsAdded = true;
                }
            });
        }
    }

    if (relationsAdded) {
        // Keep going until we reach a fixed point
        return addForwardDependencies(program, result);
    } else {
        return result;
    }
}

// ensures that every relation not specified by the magic-transform option
// is ignored by the transformation
std::set<AstQualifiedName> addIgnoredRelations(
        const AstProgram* program, std::set<AstQualifiedName> relations) {
    // get a vector of all relations specified by the option
    std::vector<std::string> specifiedRelations = splitString(Global::config().get("magic-transform"), ',');

    // if a star was used as a relation, then magic set will be performed for all nodes
    if (contains(specifiedRelations, "*")) {
        return relations;
    }

    // find all specified relations
    std::set<AstQualifiedName> targetRelations;
    for (AstRelation* rel : program->getRelations()) {
        std::string mainName = rel->getQualifiedName().getQualifiers()[0];
        if (contains(specifiedRelations, mainName)) {
            targetRelations.insert(rel->getQualifiedName());
        }
    }

    // add all backward-dependencies to the list of relations to transform;
    // if we want to magic transform 'a', then we also have to magic transform
    // every relation that (directly or indirectly) uses 'a' in its clauses
    targetRelations = addBackwardDependencies(program, targetRelations);

    // ignore all relations not specified by the option
    std::set<AstQualifiedName> retVal(relations);
    for (AstRelation* rel : program->getRelations()) {
        if (!contains(targetRelations, rel->getQualifiedName())) {
            retVal.insert(rel->getQualifiedName());
        }
    }

    return retVal;
}

/* =======================  *
 *        Adornment         *
 * =======================  */

// reorders a vector of integers to fit the clause atom-reordering function
std::vector<unsigned int> reorderOrdering(std::vector<unsigned int> order) {
    // when the adornment is computed, the atoms are numbered based on
    // which was chosen by the SIPS first - this is the 'order' vector.
    // want to reorder clause atoms so that the atom labelled 0 is first, and so on.
    // i.e. order[i] denotes where labels[i] should move
    // e.g.: [a, b, c] with label [1, 2, 0] should become [c, a, b]

    // the atom reordering function for clauses, however, moves it as follows:
    // [a, b, c] with label [1, 2, 0] becomes [b, c, a]
    // i.e. labels[i] goes to the position of i in the order vector

    // this function reorders the ordering scheme to match the second type
    std::vector<unsigned int> neworder(order.size());
    for (size_t i = 0; i < order.size(); i++) {
        // this took embarrassingly long to figure out
        neworder[order[i]] = i;
    }
    return neworder;
}

// reorders an adornment based on a given ordering scheme
std::vector<std::string> reorderAdornment(
        std::vector<std::string> adornment, std::vector<unsigned int> order) {
    // order[i] denotes where labels[i] should move
    // [a, b, c] with order [1, 2, 0] -> [c, a, b]
    std::vector<std::string> result(adornment.size());
    for (size_t i = 0; i < adornment.size(); i++) {
        result[order[i]] = adornment[i];
    }
    return result;
}

// computes the adornment of a newly chosen atom
// returns both the adornment and the new list of bound arguments
std::pair<std::string, std::set<std::string>> bindArguments(
        AstAtom* currAtom, std::set<std::string> boundArgs, OldBindingStore& compositeBindings) {
    std::set<std::string> newlyBoundArgs;
    std::string atomAdornment = "";

    for (AstArgument* arg : currAtom->getArguments()) {
        if (isBoundArgument(arg, boundArgs, compositeBindings)) {
            atomAdornment += "b";  // bound
        } else {
            atomAdornment += "f";  // free
            std::string argName = getString(arg);
            newlyBoundArgs.insert(argName);  // now bound
        }
    }

    // add newly bound arguments to the list of bound arguments
    for (std::string newArg : newlyBoundArgs) {
        boundArgs.insert(newArg);
    }

    return std::make_pair(atomAdornment, boundArgs);
}

// SIPS #1:
// Choose the left-most body atom with at least one bound argument
// If none exist, prioritise EDB predicates.
int getNextAtomNaiveSIPS(std::vector<AstAtom*> atoms, const std::set<std::string>& boundArgs,
        const std::set<AstQualifiedName>& edb, OldBindingStore& compositeBindings) {
    // find the first available atom with at least one bound argument
    int firstedb = -1;
    int firstidb = -1;
    for (size_t i = 0; i < atoms.size(); i++) {
        AstAtom* currAtom = atoms[i];
        if (currAtom == nullptr) {
            // already done - move on
            continue;
        }

        AstQualifiedName atomName = currAtom->getQualifiedName();

        // check if this is the first edb or idb atom met
        if (contains(edb, atomName)) {
            if (firstedb < 0) {
                firstedb = i;
            }
        } else if (firstidb < 0) {
            firstidb = i;
        }

        // if it has at least one bound argument, then adorn this atom next
        if (hasBoundArgument(currAtom, boundArgs, compositeBindings)) {
            return i;
        }
    }

    // all unadorned body atoms only have free arguments
    // choose the first edb remaining if available
    if (firstedb >= 0) {
        return firstedb;
    } else {
        return firstidb;
    }
}

// SIPS #2:
// Choose the body atom with the maximum number of bound arguments
// If equal boundness, prioritise left-most EDB
int getNextAtomMaxBoundSIPS(std::vector<AstAtom*>& atoms, const std::set<std::string>& boundArgs,
        const std::set<AstQualifiedName>& edb, OldBindingStore& compositeBindings) {
    int maxBound = -1;
    int maxIndex = 0;
    bool maxIsEDB = false;  // checks if current max index is an EDB predicate

    for (size_t i = 0; i < atoms.size(); i++) {
        AstAtom* currAtom = atoms[i];
        if (currAtom == nullptr) {
            // already done - move on
            continue;
        }

        int numBound = 0;
        for (AstArgument* arg : currAtom->getArguments()) {
            if (isBoundArgument(arg, boundArgs, compositeBindings)) {
                numBound++;
            }
        }

        if (numBound > maxBound) {
            maxBound = numBound;
            maxIndex = i;
            maxIsEDB = contains(edb, currAtom->getQualifiedName());
        } else if (!maxIsEDB && numBound == maxBound && contains(edb, currAtom->getQualifiedName())) {
            // prioritise EDB predicates
            maxIsEDB = true;
            maxIndex = i;
        }
    }

    return maxIndex;
}

// Choose the SIP Strategy to be used
// Current choice is the max ratio SIPS
int getNextAtomSIPS(std::vector<AstAtom*>& atoms, std::set<std::string> boundArgs,
        std::set<AstQualifiedName> edb, OldBindingStore& compositeBindings) {
    return getNextAtomMaxBoundSIPS(atoms, boundArgs, edb, compositeBindings);
}

// Find and stores all composite arguments (namely records and functors) along
// with their variable dependencies
OldBindingStore bindComposites(const AstProgram* program) {
    struct M : public AstNodeMapper {
        OldBindingStore& compositeBindings;
        std::set<AstBinaryConstraint*>& constraints;
        mutable int changeCount;

        M(OldBindingStore& compositeBindings, std::set<AstBinaryConstraint*>& constraints, int changeCount)
                : compositeBindings(compositeBindings), constraints(constraints), changeCount(changeCount) {}

        int getChangeCount() const {
            return changeCount;
        }

        std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
            if (auto* functor = dynamic_cast<AstFunctor*>(node.get())) {
                // functor found
                changeCount++;

                // create new variable name (with appropriate suffix)
                std::stringstream newVariableName;
                newVariableName << "+functor" << changeCount;

                // add the binding to the OldBindingStore
                compositeBindings.addBinding(newVariableName.str(), functor);

                // create new constraint (+functorX = original-functor)
                auto newVariable = std::make_unique<AstVariable>(newVariableName.str());
                auto opEq = functor->getReturnType() == TypeAttribute::Float ? BinaryConstraintOp::FEQ
                                                                             : BinaryConstraintOp::EQ;
                constraints.insert(
                        new AstBinaryConstraint(opEq, std::unique_ptr<AstArgument>(newVariable->clone()),
                                std::unique_ptr<AstArgument>(functor->clone())));

                // update functor to be the variable created
                return newVariable;
            } else if (auto* record = dynamic_cast<AstRecordInit*>(node.get())) {
                // record found
                changeCount++;

                // create new variable name (with appropriate suffix)
                std::stringstream newVariableName;
                newVariableName << "+record" << changeCount;

                // add the binding to the OldBindingStore
                compositeBindings.addBinding(newVariableName.str(), record);

                // create new constraint (+recordX = original-record)
                auto newVariable = std::make_unique<AstVariable>(newVariableName.str());
                constraints.insert(new AstBinaryConstraint(BinaryConstraintOp::EQ,
                        std::unique_ptr<AstArgument>(newVariable->clone()),
                        std::unique_ptr<AstArgument>(record->clone())));

                // update record to be the variable created
                return newVariable;
            }
            node->apply(*this);
            return node;
        }
    };

    OldBindingStore compositeBindings;

    int changeCount = 0;  // number of functors/records seen so far

    // apply the change to all clauses in the program
    for (AstRelation* rel : program->getRelations()) {
        for (AstClause* clause : getClauses(*program, *rel)) {
            std::set<AstBinaryConstraint*> constraints;
            M update(compositeBindings, constraints, changeCount);
            clause->apply(update);

            changeCount = update.getChangeCount();

            for (AstBinaryConstraint* constraint : constraints) {
                clause->addToBody(std::unique_ptr<AstBinaryConstraint>(constraint));
            }
        }
    }

    return compositeBindings;
}

// runs the adornment algorithm on an input program
// Adornment algorithm:

// Let P be the set of all adorned predicates (initially empty)
// Let D' be the set of all adorned clauses (initially empty)
// Let S be the set of all seen predicate adornments

// Get the program
// Get the query
// Adorn the query based on boundness, and add it to P and S
// While P is not empty
// -- Pop the first atom out, call it R^c, where c is the adornment
// -- For every clause Q defining R:
// -- -- Adorn Q using R^c based on the SIPS chosen
// -- -- Add the adorned clause to D'
// -- -- If the body of the adorned clause contains an
//        unseen predicate adornment, add it to S and P

// Output: D' [the set of all adorned clauses]
void Adornment::run(const AstTranslationUnit& translationUnit) {
    // -------------
    // --- Setup ---
    // -------------
    const AstProgram* program = translationUnit.getProgram();
    auto* ioTypes = translationUnit.getAnalysis<IOType>();

    // normalises and tracks bindings of composite arguments (namely records and functors)
    OldBindingStore compositeBindings = bindComposites(program);

    // set up IDB/EDB and the output queries
    std::vector<AstQualifiedName> outputQueries;
    std::vector<std::vector<AdornedClause>> adornedProgram;

    // sort out the relations in the program into EDB/IDB and find computed relations
    for (AstRelation* rel : program->getRelations()) {
        AstQualifiedName relName = rel->getQualifiedName();

        // find computed relations for the topdown part
        if (ioTypes->isOutput(rel) || ioTypes->isPrintSize(rel)) {
            outputQueries.push_back(rel->getQualifiedName());
            // add relation to adornment
            adornmentRelations.push_back(rel->getQualifiedName());
        }

        // check whether edb or idb
        bool is_edb = true;
        for (AstClause* clause : getClauses(*program, *rel)) {
            if (!isFact(*clause)) {
                is_edb = false;
                break;
            }
        }

        if (is_edb) {
            adornmentEdb.insert(relName);
        } else {
            adornmentIdb.insert(relName);
        }
    }

    // find all negated literals
    visitDepthFirst(*program, [&](const AstNegation& negation) {
        negatedAtoms.insert(negation.getAtom()->getQualifiedName());
    });

    // add the relations needed for negated relations to be computed
    negatedAtoms = addForwardDependencies(program, negatedAtoms);

    // find atoms that should be ignored
    for (AstRelation* rel : program->getRelations()) {
        for (AstClause* clause : getClauses(*program, *rel)) {
            // ignore atoms that have rules containing aggregators
            if (containsAggregators(clause)) {
                ignoredAtoms.insert(clause->getHead()->getQualifiedName());
            }

            // ignore all atoms used inside an aggregator within the clause
            ignoredAtoms = addAggregators(clause, ignoredAtoms);
        }
    }

    // find atoms that should be ignored based on magic-transform option
    ignoredAtoms = addIgnoredRelations(program, ignoredAtoms);

    // if a relation is ignored, then all the atoms in its bodies need to be ignored
    ignoredAtoms = addForwardDependencies(program, ignoredAtoms);

    // -----------------
    // --- Adornment ---
    // -----------------
    // begin adornment algorithm
    // adornment is performed for each output query separately
    for (auto outputQuery : outputQueries) {
        std::vector<AdornedPredicate> currentPredicates;
        std::set<AdornedPredicate> seenPredicates;
        std::vector<AdornedClause> adornedClauses;

        // create an adorned predicate of the form outputName_ff..f
        size_t arity = getRelation(*program, outputQuery)->getArity();
        std::string frepeat = std::string(arity, 'f');  // #fs = #args
        AdornedPredicate outputPredicate(outputQuery, frepeat);
        currentPredicates.push_back(outputPredicate);
        seenPredicates.insert(outputPredicate);

        // keep going through the remaining predicates that need to be adorned
        while (!currentPredicates.empty()) {
            // pop out the first element
            AdornedPredicate currPredicate = currentPredicates[0];
            currentPredicates.erase(currentPredicates.begin());

            // don't bother adorning ignored predicates
            if (contains(ignoredAtoms, currPredicate.getQualifiedName())) {
                continue;
            }

            // go through and adorn all IDB clauses defining the relation
            AstRelation* rel = getRelation(*program, currPredicate.getQualifiedName());
            for (AstClause* clause : getClauses(*program, *rel)) {
                if (isFact(*clause)) {
                    continue;
                }

                size_t numAtoms = getBodyLiterals<AstAtom>(*clause).size();
                std::vector<std::string> clauseAtomAdornments(numAtoms);
                std::vector<unsigned int> ordering(numAtoms);
                std::set<std::string> boundArgs;

                // mark all bound arguments in the head as bound
                AstAtom* clauseHead = clause->getHead();
                std::string headAdornment = currPredicate.getAdornment();
                std::vector<AstArgument*> headArguments = clauseHead->getArguments();

                for (size_t argnum = 0; argnum < headArguments.size(); argnum++) {
                    if (headAdornment[argnum] == 'b') {
                        std::string name = getString(headArguments[argnum]);
                        boundArgs.insert(name);
                    }
                }

                // mark all bound arguments from the body
                for (const auto* bc : getBodyLiterals<AstBinaryConstraint>(*clause)) {
                    BinaryConstraintOp op = bc->getOperator();
                    if (!isEqConstraint(op)) {
                        continue;
                    }

                    // have an equality constraint
                    AstArgument* lhs = bc->getLHS();
                    AstArgument* rhs = bc->getRHS();
                    if (isBindingConstraint(lhs, rhs, boundArgs)) {
                        boundArgs.insert(getString(lhs));
                    }
                    if (isBindingConstraint(rhs, lhs, boundArgs)) {
                        boundArgs.insert(getString(rhs));
                    }
                }

                std::vector<AstAtom*> atoms = getBodyLiterals<AstAtom>(*clause);
                int atomsAdorned = 0;
                int atomsTotal = atoms.size();

                while (atomsAdorned < atomsTotal) {
                    // get the next body atom to adorn based on our SIPS
                    int currIndex = getNextAtomSIPS(atoms, boundArgs, adornmentEdb, compositeBindings);
                    AstAtom* currAtom = atoms[currIndex];
                    AstQualifiedName atomName = currAtom->getQualifiedName();

                    // compute the adornment pattern of this atom, and
                    // add all its arguments to the list of bound args
                    std::pair<std::string, std::set<std::string>> result =
                            bindArguments(currAtom, boundArgs, compositeBindings);
                    std::string atomAdornment = result.first;
                    boundArgs = result.second;

                    // check if we've already dealt with this adornment before
                    if (!contains(seenPredicates, atomName, atomAdornment)) {
                        // not seen before, so push it onto the computation list
                        // and mark it as seen
                        currentPredicates.push_back(AdornedPredicate(atomName, atomAdornment));
                        seenPredicates.insert(AdornedPredicate(atomName, atomAdornment));
                    }

                    clauseAtomAdornments[currIndex] = atomAdornment;  // store the adornment
                    ordering[currIndex] = atomsAdorned;               // mark what atom number this is
                    atoms[currIndex] = nullptr;                       // mark as done

                    atomsAdorned++;
                }

                // adornment of this clause is complete - add it to the list of
                // adorned clauses
                adornedClauses.push_back(
                        AdornedClause(clause, headAdornment, clauseAtomAdornments, ordering));
            }
        }

        // add the list of adorned clauses matching the current output relation
        adornmentClauses.push_back(adornedClauses);
    }

    this->bindings = std::move(compositeBindings);
}

// output the adornment analysis computed
// format: 'Output <outputNumber>: <outputName>' followed by a list of the
// related clause adornments, each on a new line
void Adornment::print(std::ostream& os) const {
    for (size_t i = 0; i < adornmentClauses.size(); i++) {
        std::vector<AdornedClause> clauses = adornmentClauses[i];
        os << "Output " << i + 1 << ": " << adornmentRelations[i] << std::endl;
        for (AdornedClause clause : clauses) {
            os << clause << std::endl;
        }
        os << std::endl;
    }
}

/* =======================  *
 * Magic Set Transformation *
 * =======================  */

// transforms the program so that a relation is either purely made up of
// facts or has no facts at all
void separateDBs(AstProgram* program) {
    for (AstRelation* relation : program->getRelations()) {
        AstQualifiedName relName = relation->getQualifiedName();

        // determine whether the relation fits into the EDB, IDB, or both
        bool is_edb = false;
        bool is_idb = false;

        for (AstClause* clause : getClauses(*program, *relation)) {
            if (isFact(*clause)) {
                is_edb = true;
            } else {
                is_idb = true;
            }
            if (is_edb && is_idb) {
                break;
            }
        }

        if (is_edb && is_idb) {
            // relation is part of EDB and IDB

            // move all the relation's facts to a new relation with a unique name
            std::string newEdbName = getNextEdbName(program);
            AstRelation* newEdbRel = createNewRelation(relation, newEdbName);
            program->addRelation(std::unique_ptr<AstRelation>(newEdbRel));

            // find all facts for the relation
            for (AstClause* clause : getClauses(*program, *relation)) {
                if (isFact(*clause)) {
                    // clause is fact - add it to the new EDB relation
                    AstClause* newEdbClause = clause->clone();
                    newEdbClause->getHead()->setQualifiedName(newEdbName);
                    program->addClause(std::unique_ptr<AstClause>(newEdbClause));
                }
            }

            // add a rule to the old relation that relates it to the new relation
            auto* newIdbClause = new AstClause();
            newIdbClause->setSrcLoc(nextSrcLoc(relation->getSrcLoc()));

            // oldname(arg1...argn) :- newname(arg1...argn)
            auto* headAtom = new AstAtom(relName);
            auto* bodyAtom = new AstAtom(newEdbName);

            size_t numargs = relation->getArity();
            for (size_t j = 0; j < numargs; j++) {
                std::stringstream argName;
                argName.str("");
                argName << "arg" << j;
                headAtom->addArgument(std::make_unique<AstVariable>(argName.str()));
                bodyAtom->addArgument(std::make_unique<AstVariable>(argName.str()));
            }

            newIdbClause->setHead(std::unique_ptr<AstAtom>(headAtom));
            newIdbClause->addToBody(std::unique_ptr<AstAtom>(bodyAtom));

            program->addClause(std::unique_ptr<AstClause>(newIdbClause));
        }
    }
}

// returns the adornment of an (adorned) magic identifier
std::string extractAdornment(const AstQualifiedName& magicRelationName) {
    std::string baseRelationName = magicRelationName.getQualifiers()[0];
    int endpt = getEndpoint(baseRelationName);
    std::string adornment = baseRelationName.substr(endpt + 1, baseRelationName.size() - (endpt + 1));
    return adornment;
}

// returns the constant represented by a variable of the form "+abdulX_variablevalue_X"
AstArgument* extractConstant(const std::string& normalisedConstant) {
    // strip off the prefix up to (and including) the first underscore
    size_t argStart = normalisedConstant.find('_');
    std::string arg = normalisedConstant.substr(argStart + 1, normalisedConstant.size());

    // -- check if string or num constant --
    char indicatorChar = arg[arg.size() - 1];  // 'n' or 's'
    std::string stringRep = arg.substr(0, arg.size() - 2);

    if (indicatorChar == 's') {
        // string argument
        return new AstStringConstant(stringRep);
    } else if (indicatorChar == 'n') {
        // numeric argument
        return new AstNumericConstant(stringRep, AstNumericConstant::Type::Int);
    } else if (indicatorChar == 'u') {
        return new AstNumericConstant(stringRep, AstNumericConstant::Type::Uint);
    } else if (indicatorChar == 'f') {
        return new AstNumericConstant(stringRep, AstNumericConstant::Type::Float);
    } else {
        // invalid format
        return nullptr;
    }
}

// creates a new magic relation based on a given relation and magic base name
AstRelation* createMagicRelation(AstRelation* original, const AstQualifiedName& magicPredName) {
    // get the adornment of this argument
    std::string adornment = extractAdornment(magicPredName);

    // create the relation
    auto* newMagicRelation = new AstRelation();
    newMagicRelation->setQualifiedName(magicPredName);

    // copy over (bound) attributes from the original relation
    std::vector<AstAttribute*> attrs = original->getAttributes();
    for (size_t currentArg = 0; currentArg < original->getArity(); currentArg++) {
        if (adornment[currentArg] == 'b') {
            newMagicRelation->addAttribute(std::unique_ptr<AstAttribute>(attrs[currentArg]->clone()));
        }
    }

    return newMagicRelation;
}

// transforms the program so that all underscores previously transformed
// to a "+underscoreX" are changed back to underscores
void replaceUnderscores(AstProgram* program) {
    struct M : public AstNodeMapper {
        std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
            if (auto* var = dynamic_cast<AstVariable*>(node.get())) {
                if (hasPrefix(var->getName(), "+underscore")) {
                    return std::make_unique<AstUnnamedVariable>();
                }
            }
            node->apply(*this);
            return node;
        }
    };

    M update;
    for (AstRelation* rel : program->getRelations()) {
        for (AstClause* clause : getClauses(*program, *rel)) {
            clause->apply(update);
        }
    }
}

// Magic Set Transformation
// STEPS:
// For all output relations G:
// -- Get the adornment S for this clause
// -- Add to S the set of magic rules for all clauses in S:
// -- -- For each clause C = A^a :- A1^a1, A2^a2, ..., An^an
// -- -- -- For each IDB literal A_i in the body of C
// -- -- -- -- Add mag(Ai^ai) :- mag(A^a), A1^a1, ..., Ai-1^ai-1 to the program
// -- For all clauses H :- T in S:
// -- -- Replace the clause with H :- mag(H), T.
// -- Add the fact m_G_f...f to S
// Remove all old idb rules
bool OldMagicSetTransformer::transform(AstTranslationUnit& translationUnit) {
    AstProgram* program = translationUnit.getProgram();
    auto* ioTypes = translationUnit.getAnalysis<IOType>();

    separateDBs(program);  // make EDB int IDB = empty

    auto* adornment = translationUnit.getAnalysis<Adornment>();  // perform adornment
    const OldBindingStore& compositeBindings = adornment->getBindings();

    // edb/idb handling
    std::vector<std::vector<AdornedClause>> allAdornedClauses = adornment->getAdornedClauses();
    std::set<AstQualifiedName> negatedAtoms = adornment->getNegatedAtoms();
    std::set<AstQualifiedName> ignoredAtoms = adornment->getIgnoredAtoms();
    std::set<AstQualifiedName> oldIdb = adornment->getIDB();
    std::set<AstQualifiedName> newIdb;

    // additions
    std::vector<AstQualifiedName> newQueryNames;
    std::vector<AstClause*> newClauses;

    // output handling
    std::vector<AstQualifiedName> outputQueries = adornment->getRelations();

    // ignore negated atoms
    for (AstQualifiedName relation : negatedAtoms) {
        ignoredAtoms.insert(relation);
    }

    // perform magic set algorithm for each output
    for (size_t querynum = 0; querynum < outputQueries.size(); querynum++) {
        AstQualifiedName outputQuery = outputQueries[querynum];
        std::vector<AdornedClause> adornedClauses = allAdornedClauses[querynum];
        AstRelation* originalOutputRelation = getRelation(*program, outputQuery);

        // add a relation for the output query
        // mN_outputname_ff...f()
        auto* magicOutputRelation = new AstRelation();
        std::string frepeat = std::string(originalOutputRelation->getArity(), 'f');
        AstQualifiedName magicOutputName =
                createMagicIdentifier(createAdornedIdentifier(outputQuery, frepeat), querynum);
        magicOutputRelation->setQualifiedName(magicOutputName);
        newQueryNames.push_back(magicOutputName);

        // add the new relation to the program
        program->addRelation(std::unique_ptr<AstRelation>(magicOutputRelation));

        // add an empty fact to the program
        // i.e. mN_outputname_ff...f().
        auto* outputFact = new AstClause();
        outputFact->setSrcLoc(nextSrcLoc(originalOutputRelation->getSrcLoc()));
        outputFact->setHead(std::make_unique<AstAtom>(magicOutputName));
        program->addClause(std::unique_ptr<AstClause>(outputFact));

        // perform the magic transformation based on the adornment for this output query
        for (AdornedClause adornedClause : adornedClauses) {
            AstClause* clause = adornedClause.getClause();
            AstQualifiedName originalName = clause->getHead()->getQualifiedName();

            // dont perform the magic transformation on ignored relations
            if (contains(ignoredAtoms, originalName)) {
                continue;
            }

            // find the adorned version of this relation
            std::string headAdornment = adornedClause.getHeadAdornment();
            AstQualifiedName newRelName = createAdornedIdentifier(originalName, headAdornment);
            AstRelation* adornedRelation = getRelation(*program, newRelName);

            // check if adorned relation already created previously
            if (adornedRelation == nullptr) {
                // adorned relation not created yet, so
                // create the relation with the new adornment
                AstRelation* originalRelation = getRelation(*program, originalName);
                AstRelation* newRelation = createNewRelation(originalRelation, newRelName);

                // add the created adorned relation to the program
                program->addRelation(std::unique_ptr<AstRelation>(newRelation));

                // copy over input directives to new adorned relation
                // also - update input directives to correctly use default fact file names
                if (ioTypes->isInput(originalRelation)) {
                    for (AstIO* io : program->getIOs()) {
                        if (io->getQualifiedName() != originalName || io->getType() != AstIoType::input) {
                            continue;
                        }
                        io->setQualifiedName(newRelName);
                        if (!io->hasDirective("IO")) {
                            io->addDirective("IO", "file");
                        }
                        if (io->getDirective("IO") == "file" && !io->hasDirective("filename")) {
                            io->addDirective("filename", originalName.getQualifiers()[0] + ".facts");
                        }
                    }
                }
                adornedRelation = newRelation;
            }

            // create the adorned version of this clause
            AstClause* newClause = clause->clone();
            newClause->getHead()->setQualifiedName(newRelName);
            // reorder atoms based on SIPS ordering
            AstClause* tmp = reorderAtoms(newClause, reorderOrdering(adornedClause.getOrdering()));
            delete newClause;
            newClause = tmp;

            // get corresponding adornments for each body atom
            std::vector<std::string> bodyAdornment =
                    reorderAdornment(adornedClause.getBodyAdornment(), adornedClause.getOrdering());

            // set the name of each IDB pred in the clause to be the adorned version
            int atomsSeen = 0;
            for (AstLiteral* lit : newClause->getBodyLiterals()) {
                if (dynamic_cast<AstAtom*>(lit) != nullptr) {
                    auto* bodyAtom = dynamic_cast<AstAtom*>(lit);
                    AstQualifiedName atomName = bodyAtom->getQualifiedName();
                    // note that all atoms in the original clause were adorned,
                    // but only the IDB atom adornments should be added here
                    if (contains(oldIdb, atomName)) {
                        if (!contains(ignoredAtoms, atomName)) {
                            // ignored atoms should not be changed
                            AstQualifiedName newAtomName =
                                    createAdornedIdentifier(atomName, bodyAdornment[atomsSeen]);
                            bodyAtom->setQualifiedName(newAtomName);
                            newIdb.insert(newAtomName);
                        } else {
                            newIdb.insert(atomName);
                        }
                    }
                    atomsSeen++;
                }
            }

            // Add the set of magic rules for this clause C = A^a :- A1^a1, A2^a2, ..., An^an
            // -- For each clause C = A^a :- A1^a1, A2^a2, ..., An^an
            // -- -- For each IDB literal A_i in the body of C
            // -- -- -- Add mag(Ai^ai) :- mag(A^a), A1^a1, ..., Ai-1^ai-1 to the program
            std::vector<AstAtom*> body = getBodyLiterals<AstAtom>(*newClause);
            for (size_t i = 0; i < body.size(); i++) {
                AstAtom* currentLiteral = body[i];

                // only care about atoms in the body
                if (dynamic_cast<AstAtom*>(currentLiteral) != nullptr) {
                    auto* atom = dynamic_cast<AstAtom*>(currentLiteral);
                    AstQualifiedName atomName = atom->getQualifiedName();

                    // only IDB atoms that are not being ignored matter
                    if (contains(newIdb, atomName) && !contains(ignoredAtoms, atomName)) {
                        std::string currAdornment = bodyAdornment[i];

                        // generate the name of the magic version of this adorned literal
                        AstQualifiedName newAtomName = createMagicIdentifier(atomName, querynum);

                        // if the magic version does not exist, create it
                        if (getRelation(*program, newAtomName) == nullptr) {
                            auto* magicRelation = new AstRelation();
                            magicRelation->setQualifiedName(newAtomName);

                            // find out the original name of the relation (pre-adornment)
                            std::string baseAtomName = atomName.getQualifiers()[0];
                            int endpt = getEndpoint(baseAtomName);
                            AstQualifiedName originalRelationName = createSubIdentifier(
                                    atomName, 0, endpt - 1);  // get rid of the extra + at the end
                            AstRelation* originalRelation = getRelation(*program, originalRelationName);

                            // copy over the (bound) attributes from the original relation
                            int argcount = 0;
                            for (AstAttribute* attr : originalRelation->getAttributes()) {
                                if (currAdornment[argcount] == 'b') {
                                    magicRelation->addAttribute(std::unique_ptr<AstAttribute>(attr->clone()));
                                }
                                argcount++;
                            }

                            // copy over internal representation
                            magicRelation->setRepresentation(originalRelation->getRepresentation());

                            // add the new magic relation to the program
                            program->addRelation(std::unique_ptr<AstRelation>(magicRelation));
                        }

                        // start setting up the magic rule
                        auto* magicClause = new AstClause();
                        magicClause->setSrcLoc(nextSrcLoc(atom->getSrcLoc()));

                        // create the head of the magic rule
                        auto* magicHead = new AstAtom(newAtomName);

                        // copy over (bound) arguments from the original atom
                        int argCount = 0;
                        for (AstArgument* arg : atom->getArguments()) {
                            if (currAdornment[argCount] == 'b') {
                                magicHead->addArgument(std::unique_ptr<AstArgument>(arg->clone()));
                            }
                            argCount++;
                        }

                        // head complete!
                        magicClause->setHead(std::unique_ptr<AstAtom>(magicHead));

                        // -- create the body --
                        // create the first body argument (mag(origClauseHead^adornment))
                        AstQualifiedName magPredName =
                                createMagicIdentifier(newClause->getHead()->getQualifiedName(), querynum);
                        auto* addedMagicPred = new AstAtom(magPredName);

                        // create the relation if it does not exist
                        if (getRelation(*program, magPredName) == nullptr) {
                            AstRelation* originalRelation =
                                    getRelation(*program, newClause->getHead()->getQualifiedName());
                            AstRelation* newMagicRelation =
                                    createMagicRelation(originalRelation, magPredName);

                            // add the new relation to the prgoram
                            program->addRelation(std::unique_ptr<AstRelation>(newMagicRelation));
                        }

                        // add (bound) arguments to the magic predicate from the clause head
                        argCount = 0;
                        for (AstArgument* arg : newClause->getHead()->getArguments()) {
                            if (headAdornment[argCount] == 'b') {
                                addedMagicPred->addArgument(std::unique_ptr<AstArgument>(arg->clone()));
                            }
                            argCount++;
                        }

                        // first argument complete!
                        magicClause->addToBody(std::unique_ptr<AstAtom>(addedMagicPred));

                        // add the rest of the necessary arguments
                        for (size_t j = 0; j < i; j++) {
                            magicClause->addToBody(std::unique_ptr<AstLiteral>(body[j]->clone()));
                        }

                        // restore memorised bindings for all composite arguments
                        std::vector<const AstArgument*> compositeArguments;
                        visitDepthFirst(*magicClause, [&](const AstArgument& argument) {
                            std::string argName = getString(&argument);
                            if (hasPrefix(argName, "+functor") || hasPrefix(argName, "+record")) {
                                compositeArguments.push_back(&argument);
                            }
                        });

                        for (const AstArgument* compositeArgument : compositeArguments) {
                            std::string argName = getString(compositeArgument);

                            // if the composite argument was bound only because all
                            // of its constituent variables were bound, then bind
                            // the composite variable to the original argument
                            if (compositeBindings.isVariableBoundComposite(argName)) {
                                AstArgument* originalArgument =
                                        compositeBindings.cloneOriginalArgument(argName);
                                magicClause->addToBody(
                                        std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                                                std::unique_ptr<AstArgument>(compositeArgument->clone()),
                                                std::unique_ptr<AstArgument>(originalArgument)));
                            }
                        }

                        // restore bindings for normalised constants
                        std::vector<const AstVariable*> clauseVariables;
                        visitDepthFirst(*magicClause,
                                [&](const AstVariable& variable) { clauseVariables.push_back(&variable); });

                        for (const AstVariable* var : clauseVariables) {
                            std::string varName = getString(var);

                            // all normalised constants begin with "+abdul" (see AstTransforms.cpp)
                            // +abdulX_variablevalue_Y
                            if (hasPrefix(varName, "+abdul")) {
                                AstArgument* embeddedConstant = extractConstant(varName);

                                // add the constraint to the body of the clause
                                magicClause->addToBody(std::make_unique<AstBinaryConstraint>(
                                        BinaryConstraintOp::EQ, std::unique_ptr<AstArgument>(var->clone()),
                                        std::unique_ptr<AstArgument>(embeddedConstant)));
                            }
                        }

                        // magic rule done! add it to the program
                        program->addClause(std::unique_ptr<AstClause>(magicClause));
                    }
                }
            }

            // -- replace with H :- mag(H), T --

            size_t originalNumAtoms = getBodyLiterals<AstAtom>(*newClause).size();

            // create the first argument of this new clause
            const AstAtom* newClauseHead = newClause->getHead();
            AstQualifiedName newMag = createMagicIdentifier(newClauseHead->getQualifiedName(), querynum);
            auto* newMagAtom = new AstAtom(newMag);

            // copy over the bound arguments from the head
            std::vector<AstArgument*> args = newClauseHead->getArguments();
            for (size_t k = 0; k < args.size(); k++) {
                if (headAdornment[k] == 'b') {
                    newMagAtom->addArgument(std::unique_ptr<AstArgument>(args[k]->clone()));
                }
            }

            // add it to the end of the clause
            newClause->addToBody(std::unique_ptr<AstAtom>(newMagAtom));

            // move the new magic argument to the front of the clause,
            // pushing all the rest up one position
            std::vector<unsigned int> newClauseOrder(originalNumAtoms + 1);
            for (size_t k = 0; k < originalNumAtoms; k++) {
                newClauseOrder[k] = k + 1;
            }
            newClauseOrder[originalNumAtoms] = 0;
            tmp = reorderAtoms(newClause, reorderOrdering(newClauseOrder));
            delete newClause;
            newClause = tmp;

            // add the clause to the program and the set of new clauses
            newClause->setSrcLoc(nextSrcLoc(newClause->getSrcLoc()));
            newClauses.push_back(newClause);
            program->addClause(std::unique_ptr<AstClause>(newClause));
        }
    }

    for (AstQualifiedName relationName : oldIdb) {
        // do not delete negated atoms, ignored atoms, or atoms added by aggregate relations
        if (!(contains(ignoredAtoms, relationName) || contains(negatedAtoms, relationName) ||
                    isAggRel(relationName))) {
            program->removeRelation(relationName);
        }
    }

    // add the new output relations
    // in particular, need to rename the adorned output back to the original name
    for (size_t i = 0; i < outputQueries.size(); i++) {
        AstQualifiedName oldName = outputQueries[i];
        AstQualifiedName newName = newQueryNames[i];

        // get the original adorned relation
        std::string newBaseName = newName.getQualifiers()[0];
        size_t prefixpoint = newBaseName.find("_");
        AstQualifiedName newRelationName =
                createSubIdentifier(newName, prefixpoint + 1, newBaseName.size() - (prefixpoint + 1));

        AstRelation* adornedRelation = getRelation(*program, newRelationName);

        if (adornedRelation == nullptr) {
            continue;
        }

        AstRelation* outputRelation = getRelation(*program, oldName);

        // if the corresponding output relation does not exist yet, create it
        if (outputRelation == nullptr) {
            outputRelation = new AstRelation();
            outputRelation->setSrcLoc(nextSrcLoc(adornedRelation->getSrcLoc()));

            // copy over the attributes from the existing adorned version
            for (AstAttribute* attr : adornedRelation->getAttributes()) {
                outputRelation->addAttribute(std::unique_ptr<AstAttribute>(attr->clone()));
            }

            // rename it back to its original name
            outputRelation->setQualifiedName(oldName);
            // add the new output to the program
            program->addRelation(std::unique_ptr<AstRelation>(outputRelation));
        }

        // rules need to be the same
        // easy fix:
        //    oldname(arg1...argn) :- newname(arg1...argn)
        auto* headatom = new AstAtom(oldName);
        auto* bodyatom = new AstAtom(newRelationName);

        for (size_t j = 0; j < adornedRelation->getArity(); j++) {
            std::stringstream argName;
            argName.str("");
            argName << "arg" << j;
            headatom->addArgument(std::make_unique<AstVariable>(argName.str()));
            bodyatom->addArgument(std::make_unique<AstVariable>(argName.str()));
        }

        // add the clause to the program
        auto* referringClause = new AstClause();
        referringClause->setSrcLoc(nextSrcLoc(outputRelation->getSrcLoc()));
        referringClause->setHead(std::unique_ptr<AstAtom>(headatom));
        referringClause->addToBody(std::unique_ptr<AstAtom>(bodyatom));

        program->addClause(std::unique_ptr<AstClause>(referringClause));
    }

    // replace all "+underscoreX" variables with actual underscores
    replaceUnderscores(program);

    // done!
    return true;
}
}  // end of namespace souffle
