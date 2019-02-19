/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file AstSemanticChecker.cpp
 *
 * Implementation of the semantic checker pass.
 *
 ***********************************************************************/

#include "AstSemanticChecker.h"
#include "AstArgument.h"
#include "AstAttribute.h"
#include "AstClause.h"
#include "AstFunctorDeclaration.h"
#include "AstGroundAnalysis.h"
#include "AstIO.h"
#include "AstLiteral.h"
#include "AstNode.h"
#include "AstProgram.h"
#include "AstRelation.h"
#include "AstRelationIdentifier.h"
#include "AstTranslationUnit.h"
#include "AstType.h"
#include "AstTypeAnalysis.h"
#include "AstTypeEnvironmentAnalysis.h"
#include "AstTypes.h"
#include "AstUtils.h"
#include "AstVisitor.h"
#include "BinaryConstraintOps.h"
#include "ErrorReport.h"
#include "Global.h"
#include "GraphUtils.h"
#include "PrecedenceGraph.h"
#include "RelationRepresentation.h"
#include "SrcLocation.h"
#include "TypeLattice.h"
#include "TypeSystem.h"
#include "Util.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <typeinfo>
#include <utility>
#include <vector>

namespace souffle {

bool AstSemanticChecker::transform(AstTranslationUnit& translationUnit) {
    const TypeEnvironment& typeEnv =
            translationUnit.getAnalysis<TypeEnvironmentAnalysis>()->getTypeEnvironment();
    auto* typeAnalysis = translationUnit.getAnalysis<TypeAnalysis>();
    auto* precedenceGraph = translationUnit.getAnalysis<PrecedenceGraph>();
    auto* recursiveClauses = translationUnit.getAnalysis<RecursiveClauses>();
    auto* ioTypes = translationUnit.getAnalysis<IOType>();

    checkProgram(translationUnit.getErrorReport(), *translationUnit.getProgram(), typeEnv, *typeAnalysis,
            *precedenceGraph, *recursiveClauses, *ioTypes);
    return false;
}

void AstSemanticChecker::checkProgram(ErrorReport& report, const AstProgram& program,
        const TypeEnvironment& typeEnv, const TypeAnalysis& typeAnalysis,
        const PrecedenceGraph& precedenceGraph, const RecursiveClauses& recursiveClauses,
        const IOType& ioTypes) {
    // suppress warnings for given relations
    if (Global::config().has("suppress-warnings")) {
        std::vector<std::string> suppressedRelations =
                splitString(Global::config().get("suppress-warnings"), ',');

        if (std::find(suppressedRelations.begin(), suppressedRelations.end(), "*") !=
                suppressedRelations.end()) {
            // mute all relations
            for (AstRelation* rel : program.getRelations()) {
                rel->setQualifier(rel->getQualifier() | SUPPRESSED_RELATION);
            }
        } else {
            // mute only the given relations (if they exist)
            for (auto& relname : suppressedRelations) {
                const std::vector<std::string> comps = splitString(relname, '.');
                if (!comps.empty()) {
                    // generate the relation identifier
                    AstRelationIdentifier relid(comps[0]);
                    for (size_t i = 1; i < comps.size(); i++) {
                        relid.append(comps[i]);
                    }

                    // update suppressed qualifier if the relation is found
                    if (AstRelation* rel = program.getRelation(relid)) {
                        rel->setQualifier(rel->getQualifier() | SUPPRESSED_RELATION);
                    }
                }
            }
        }
    }

    // -- conduct checks --
    // TODO: re-write to use visitors
    checkTypes(report, program);
    checkRules(report, typeEnv, program, recursiveClauses, ioTypes);
    checkNamespaces(report, program);
    checkIODirectives(report, program);
    checkWitnessProblem(report, program);
    checkInlining(report, program, precedenceGraph, ioTypes);

    // get the list of components to be checked
    std::vector<const AstClause*> nodes;
    for (const auto& rel : program.getRelations()) {
        for (const auto& cls : rel->getClauses()) {
            nodes.push_back(cls);
        }
    }

    // -- check grounded variables and records --
    visitDepthFirst(nodes, [&](const AstClause& clause) {
        // only interested in rules
        if (clause.isFact()) {
            return;
        }

        // compute all grounded terms
        auto isGrounded = getGroundedTerms(clause);

        // all terms in head need to be grounded
        std::set<std::string> reportedVars;
        for (const AstVariable* cur : getVariables(clause)) {
            if (!isGrounded[cur] && reportedVars.insert(cur->getName()).second) {
                report.addError("Ungrounded variable " + cur->getName(), cur->getSrcLoc());
            }
        }

        // all records need to be grounded
        for (const AstRecordInit* cur : getRecords(clause)) {
            if (!isGrounded[cur]) {
                report.addError("Ungrounded record", cur->getSrcLoc());
            }
        }
    });

    // -- type checks --

    // type casts name a valid type
    visitDepthFirst(nodes, [&](const AstTypeCast& cast) {
        if (!typeEnv.isType(cast.getType())) {
            report.addError("Type cast is to undeclared type " + toString(cast.getType()), cast.getSrcLoc());
        }
    });

    // record initializations declare valid record types and have correct size
    visitDepthFirst(nodes, [&](const AstRecordInit& record) {
        // TODO (#467) remove the next line to enable subprogram compilation for record types
        Global::config().unset("engine");
        if (typeEnv.isType(record.getType())) {
            const Type& type = typeEnv.getType(record.getType());
            if (!isRecordType(type)) {
                report.addError("Type " + toString(type) + " is not a record type", record.getSrcLoc());
            } else if (record.getArguments().size() !=
                       dynamic_cast<const RecordType*>(&type)->getFields().size()) {
                report.addError("Wrong number of arguments given to record", record.getSrcLoc());
            }
        } else {
            report.addError(
                    "Type " + toString(record.getType()) + " has not been declared", record.getSrcLoc());
        }
    });

    // number constants are within allowed domain
    visitDepthFirst(nodes, [&](const AstNumberConstant& cnst) {
        AstDomain idx = cnst.getIndex();
        if (idx > MAX_AST_DOMAIN || idx < MIN_AST_DOMAIN) {
            report.addError("Number constant not in range [" + std::to_string(MIN_AST_DOMAIN) + ", " +
                                    std::to_string(MAX_AST_DOMAIN) + "]",
                    cnst.getSrcLoc());
        }
    });

    // check existence and arity of all user defined functors
    visitDepthFirst(nodes, [&](const AstUserDefinedFunctor& fun) {
        const AstFunctorDeclaration* funDecl = program.getFunctorDeclaration(fun.getName());
        if (funDecl == nullptr) {
            report.addError("User-defined functor hasn't been declared", fun.getSrcLoc());
        } else if (funDecl->getArgCount() != fun.getArgCount()) {
            report.addError("Mismatching number of arguments of functor", fun.getSrcLoc());
        }
    });

    const TypeLattice& lattice = typeAnalysis.getLattice();

    // get the list of components to be checked
    if (lattice.isValid()) {
        if (TypeAnalysis::anyInvalidClauses(program)) {
            nodes = TypeAnalysis::getValidClauses(program);
            report.addError("Not all clauses could be typechecked due to other errors present");
        }
    } else {
        report.addError("No type checking could occur due to other errors present");
        nodes = std::vector<const AstClause*>();
    }

    // check all arguments have been declared a valid type
    for (const AstClause* clause : nodes) {
        // compute all grounded terms
        auto isGrounded = getGroundedTerms(*clause);

        visitDepthFirst(*clause, [&](const AstArgument& arg) {
            if (!isGrounded[&arg]) {
                // This argument has already caused an error, so skip it here
                return;
            }
            const AnalysisType* type = typeAnalysis.getType(&arg);
            if (!type->isValid()) {
                if (dynamic_cast<const BotPrimAType*>(type) != nullptr) {
                    report.addError("Unable to deduce valid type for expression, as base types are disjoint",
                            arg.getSrcLoc());
                } else if (dynamic_cast<const BotAType*>(type) != nullptr) {
                    report.addError(
                            "Unable to deduce valid type for expression, as primitive types are disjoint",
                            arg.getSrcLoc());
                } else if (dynamic_cast<const TopAType*>(type) != nullptr) {
                    // this must be equal to a poorly typed but grounded record constructor, which will
                    // produce an error so we don't have to
                    // e.g. A(x) :- x = *R[y], B(y). when y has the wrong type for R, we don't want to also
                    // raise an error for the type of x
                } else {
                    assert(false && "No other type should be invalid");
                }
            }
        });
    }

    // check functor inputs
    visitDepthFirst(nodes, [&](const AstIntrinsicFunctor& fun) {
        for (size_t i = 0; i < fun.getArity(); i++) {
            const AnalysisType* argType = typeAnalysis.getType(fun.getArg(i));
            if (argType->isValid()) {
                if (fun.acceptsSymbols(i)) {
                    if (!lattice.isSubtype(argType, lattice.getPrimitive(Kind::SYMBOL))) {
                        report.addError("Non-symbolic argument for functor, instead argument has type " +
                                                toString(*argType),
                                fun.getArg(i)->getSrcLoc());
                    }
                } else if (fun.acceptsNumbers(i)) {
                    if (!lattice.isSubtype(argType, lattice.getPrimitive(Kind::NUMBER))) {
                        report.addError("Non-numeric argument for functor, instead argument has type " +
                                                toString(*argType),
                                fun.getArg(i)->getSrcLoc());
                    }
                } else {
                    assert(false && "Unsupported functor input type");
                }
            }
        }
    });

    // - user-defined functors -
    visitDepthFirst(nodes, [&](const AstUserDefinedFunctor& fun) {
        const AstFunctorDeclaration* funDecl = program.getFunctorDeclaration(fun.getName());
        assert(funDecl != nullptr && "Functor must have been declared");
        assert(funDecl->getArgCount() == fun.getArgCount() && "Functor arity must match declaration");
        for (size_t i = 0; i < funDecl->getArgCount(); i++) {
            const AnalysisType* argType = typeAnalysis.getType(fun.getArg(i));
            if (argType->isValid()) {
                if (funDecl->acceptsSymbols(i)) {
                    if (!lattice.isSubtype(argType, lattice.getPrimitive(Kind::SYMBOL))) {
                        report.addError("Non-symbolic argument for functor, instead argument has type " +
                                                toString(*argType),
                                fun.getArg(i)->getSrcLoc());
                    }
                } else if (funDecl->acceptsNumbers(i)) {
                    if (!lattice.isSubtype(argType, lattice.getPrimitive(Kind::NUMBER))) {
                        report.addError("Non-numeric argument for functor, instead argument has type " +
                                                toString(*argType),
                                fun.getArg(i)->getSrcLoc());
                    }
                } else {
                    assert(false && "Unsupported functor input type");
                }
            }
        }
    });

    // check records have been assigned the correct type
    for (const AstClause* clause : nodes) {
        // compute all grounded terms
        auto isGrounded = getGroundedTerms(*clause);

        visitDepthFirst(*clause, [&](const AstRecordInit& record) {
            if (!isGrounded[&record]) {
                // Error has already been raised by grounded check
                return;
            }

            auto* recordType = dynamic_cast<const RecordType*>(&typeEnv.getType(record.getType()));
            assert(recordType != nullptr && "Type of record must be a record type");
            assert(record.getArguments().size() == recordType->getFields().size() &&
                    "Constructor has incorrect number of arguments");
            if (dynamic_cast<const TopAType*>(typeAnalysis.getType(&record)) != nullptr) {
                report.addError("Unable to deduce type " + toString(record.getType()) +
                                        " as record is not grounded as a record elsewhere, and at least one "
                                        "of its elements has the wrong type",
                        record.getSrcLoc());
            }
            for (size_t i = 0; i < record.getArguments().size(); ++i) {
                const AstArgument* member = record.getArguments()[i];
                const AnalysisType* fieldType = lattice.getType(recordType->getFields()[i].type);
                const AnalysisType* actualType = typeAnalysis.getType(member);
                if (actualType->isValid() && !lattice.isSubtype(actualType, fieldType)) {
                    report.addError("Record constructor expects element to have type " +
                                            toString(*fieldType) + " but instead it has type " +
                                            toString(*actualType),
                            member->getSrcLoc());
                }
            }
        });
    }

    // check aggregates involve numbers
    visitDepthFirst(nodes, [&](const AstAggregator& aggr) {
        if (aggr.getOperator() != AstAggregator::count) {
            const AnalysisType* targetType = typeAnalysis.getType(aggr.getTargetExpression());
            if (targetType->isValid() && !lattice.isSubtype(targetType, lattice.getPrimitive(Kind::NUMBER))) {
                report.addError(
                        "Aggregation variable is not a number, instead has type " + toString(*targetType),
                        aggr.getTargetExpression()->getSrcLoc());
            }
        }
    });

    // check type cast has correct type
    visitDepthFirst(nodes, [&](const AstTypeCast& cast) {
        if (!typeAnalysis.getType(&cast)->isValid()) {
            return;
        }
        const auto* actualType = dynamic_cast<const InnerAType*>(typeAnalysis.getType(&cast));
        assert(actualType != nullptr && "Valid type should have a kind");
        const AnalysisType* inputType = typeAnalysis.getType(cast.getValue());
        const PrimitiveAType* outputKind = lattice.getType(cast.getType())->getPrimitive();
        if (actualType->isValid() && actualType != lattice.getType(cast.getType())) {
            report.addError("Typecast is to type " + toString(cast.getType()) +
                                    " but is used where the type " + toString(*actualType) + " is expected",
                    cast.getSrcLoc());
        }
        if (!inputType->isValid()) {
            return;
        }
        if (!lattice.isSubtype(inputType, outputKind)) {
            const PrimitiveAType* inputKind = dynamic_cast<const InnerAType*>(inputType)->getPrimitive();
            report.addWarning("Casts from " + toString(*inputKind) + " values to " + toString(*outputKind) +
                                      " types may cause runtime errors",
                    cast.getSrcLoc());
        } else if (outputKind->getKind() == Kind::RECORD &&
                   !lattice.isSubtype(inputType, lattice.getType(cast.getType()))) {
            report.addWarning(
                    "Casting a record to the wrong record type may cause runtime errors", cast.getSrcLoc());
        }
    });

    // check all atoms have correct input types (only negated and head atoms must be checked, but other atoms
    // hold trivially)
    visitDepthFirst(nodes, [&](const AstAtom& atom) {
        AstRelation* relation = program.getRelation(atom.getName());
        assert(relation != nullptr && "Relation must have been declared");
        for (size_t i = 0; i < atom.argSize(); i++) {
            const AnalysisType* argType = typeAnalysis.getType(atom.getArgument(i));
            auto relationType = relation->getAttribute(i)->getTypeName();
            if (argType->isValid() && !lattice.isSubtype(argType, lattice.getType(relationType))) {
                report.addError("Relation expects value of type " + toString(relationType) +
                                        " but got argument of type " + toString(*argType),
                        atom.getArgument(i)->getSrcLoc());
            }
        }
    });

    // check inputs to binary constraint are correct
    visitDepthFirst(nodes, [&](const AstBinaryConstraint& constraint) {
        auto lhs = constraint.getLHS();
        auto rhs = constraint.getRHS();
        auto op = constraint.getOperator();
        if (op == BinaryConstraintOp::EQ) {
            return;
        } else if (op == BinaryConstraintOp::NE) {
            if (typeAnalysis.getType(lhs)->isValid() && typeAnalysis.getType(rhs)->isValid()) {
                const auto* lhsType = dynamic_cast<const InnerAType*>(typeAnalysis.getType(lhs));
                const auto* rhsType = dynamic_cast<const InnerAType*>(typeAnalysis.getType(rhs));
                assert(lhsType != nullptr && rhsType != nullptr && "Both types must have a kind");
                if (lhsType->getKind() != rhsType->getKind()) {
                    report.addError("Cannot compare operands of different kinds, left operand is a " +
                                            toString(*lhsType->getPrimitive()) + " and right operand is a " +
                                            toString(*rhsType->getPrimitive()),
                            constraint.getSrcLoc());
                } else if (lhsType->getKind() == Kind::RECORD) {
                    // TODO (#380): Remove this once record unions are allowed
                    if (!(lattice.isSubtype(lhsType, rhsType) || lattice.isSubtype(rhsType, lhsType))) {
                        report.addError("Cannot compare records of different types", constraint.getSrcLoc());
                    }
                }
            }
        } else {
            const AnalysisType* lhsType = typeAnalysis.getType(lhs);
            const AnalysisType* rhsType = typeAnalysis.getType(rhs);
            if (lhsType->isValid()) {
                if (constraint.isNumerical()) {
                    if (!lattice.isSubtype(lhsType, lattice.getPrimitive(Kind::NUMBER))) {
                        report.addError(
                                "Non-numerical operand for comparison, instead left operand has type " +
                                        toString(*lhsType),
                                lhs->getSrcLoc());
                    }
                } else if (constraint.isSymbolic()) {
                    if (!lattice.isSubtype(lhsType, lattice.getPrimitive(Kind::SYMBOL))) {
                        report.addError(
                                "Non-symbolic operand for comparison, instead left operand has type " +
                                        toString(*lhsType),
                                lhs->getSrcLoc());
                    }
                } else {
                    assert(false && "Unsupported constraint type");
                }
            }
            if (rhsType->isValid()) {
                if (constraint.isNumerical()) {
                    if (!lattice.isSubtype(rhsType, lattice.getPrimitive(Kind::NUMBER))) {
                        report.addError(
                                "Non-numerical operand for comparison, instead right operand has type " +
                                        toString(*rhsType),
                                rhs->getSrcLoc());
                    }
                } else if (constraint.isSymbolic()) {
                    if (!lattice.isSubtype(rhsType, lattice.getPrimitive(Kind::SYMBOL))) {
                        report.addError(
                                "Non-symbolic operand for comparison, instead right operand has type " +
                                        toString(*rhsType),
                                rhs->getSrcLoc());
                    }
                } else {
                    assert(false && "Unsupported constraint type");
                }
            }
        }
    });

    // - stratification --

    // check for cyclic dependencies
    const Graph<const AstRelation*, AstNameComparison>& depGraph = precedenceGraph.graph();
    for (const AstRelation* cur : depGraph.vertices()) {
        if (depGraph.reaches(cur, cur)) {
            AstRelationSet clique = depGraph.clique(cur);
            for (const AstRelation* cyclicRelation : clique) {
                // Negations and aggregations need to be stratified
                const AstLiteral* foundLiteral = nullptr;
                bool hasNegation = hasClauseWithNegatedRelation(cyclicRelation, cur, &program, foundLiteral);
                if (hasNegation ||
                        hasClauseWithAggregatedRelation(cyclicRelation, cur, &program, foundLiteral)) {
                    std::string relationsListStr = toString(join(clique, ",",
                            [](std::ostream& out, const AstRelation* r) { out << r->getName(); }));
                    std::vector<DiagnosticMessage> messages;
                    messages.push_back(
                            DiagnosticMessage("Relation " + toString(cur->getName()), cur->getSrcLoc()));
                    std::string negOrAgg = hasNegation ? "negation" : "aggregation";
                    messages.push_back(
                            DiagnosticMessage("has cyclic " + negOrAgg, foundLiteral->getSrcLoc()));
                    report.addDiagnostic(Diagnostic(Diagnostic::ERROR,
                            DiagnosticMessage("Unable to stratify relation(s) {" + relationsListStr + "}"),
                            messages));
                    break;
                }
            }
        }
    }
}

void AstSemanticChecker::checkAtom(ErrorReport& report, const AstProgram& program, const AstAtom& atom) {
    // check existence of relation
    auto* r = program.getRelation(atom.getName());
    if (!r) {
        report.addError("Undefined relation " + toString(atom.getName()), atom.getSrcLoc());
    }

    // check arity
    if (r && r->getArity() != atom.getArity()) {
        report.addError("Mismatching arity of relation " + toString(atom.getName()), atom.getSrcLoc());
    }

    for (const AstArgument* arg : atom.getArguments()) {
        checkArgument(report, program, *arg);
    }
}

/* Check whether an unnamed variable occurs in an argument (expression) */
// TODO (azreika): use a visitor instead
static bool hasUnnamedVariable(const AstArgument* arg) {
    if (dynamic_cast<const AstUnnamedVariable*>(arg)) {
        return true;
    }
    if (dynamic_cast<const AstVariable*>(arg)) {
        return false;
    }
    if (dynamic_cast<const AstConstant*>(arg)) {
        return false;
    }
    if (dynamic_cast<const AstCounter*>(arg)) {
        return false;
    }
    if (const auto* cast = dynamic_cast<const AstTypeCast*>(arg)) {
        return hasUnnamedVariable(cast->getValue());
    }
    if (const auto* inf = dynamic_cast<const AstIntrinsicFunctor*>(arg)) {
        return any_of(inf->getArguments(), (bool (*)(const AstArgument*))hasUnnamedVariable);
    }
    if (const auto* udf = dynamic_cast<const AstUserDefinedFunctor*>(arg)) {
        return any_of(udf->getArguments(), (bool (*)(const AstArgument*))hasUnnamedVariable);
    }
    if (const auto* ri = dynamic_cast<const AstRecordInit*>(arg)) {
        return any_of(ri->getArguments(), (bool (*)(const AstArgument*))hasUnnamedVariable);
    }
    if (dynamic_cast<const AstAggregator*>(arg)) {
        return false;
    }
    std::cout << "Unsupported Argument type: " << typeid(*arg).name() << "\n";
    assert(false && "Unsupported Argument Type!");
    return false;
}

static bool hasUnnamedVariable(const AstLiteral* lit) {
    if (const auto* at = dynamic_cast<const AstAtom*>(lit)) {
        return any_of(at->getArguments(), (bool (*)(const AstArgument*))hasUnnamedVariable);
    }
    if (const auto* neg = dynamic_cast<const AstNegation*>(lit)) {
        return hasUnnamedVariable(neg->getAtom());
    }
    if (dynamic_cast<const AstConstraint*>(lit)) {
        if (dynamic_cast<const AstBooleanConstraint*>(lit)) {
            return false;
        }
        if (const auto* br = dynamic_cast<const AstBinaryConstraint*>(lit)) {
            return hasUnnamedVariable(br->getLHS()) || hasUnnamedVariable(br->getRHS());
        }
    }
    std::cout << "Unsupported Literal type: " << typeid(lit).name() << "\n";
    assert(false && "Unsupported Argument Type!");
    return false;
}

void AstSemanticChecker::checkLiteral(
        ErrorReport& report, const AstProgram& program, const AstLiteral& literal) {
    // check potential nested atom
    if (auto* atom = literal.getAtom()) {
        checkAtom(report, program, *atom);
    }

    if (const auto* constraint = dynamic_cast<const AstBinaryConstraint*>(&literal)) {
        checkArgument(report, program, *constraint->getLHS());
        checkArgument(report, program, *constraint->getRHS());
    }

    // check for invalid underscore utilization
    if (hasUnnamedVariable(&literal)) {
        if (dynamic_cast<const AstAtom*>(&literal)) {
            // nothing to check since underscores are allowed
        } else if (dynamic_cast<const AstNegation*>(&literal)) {
            // nothing to check since underscores are allowed
        } else if (dynamic_cast<const AstBinaryConstraint*>(&literal)) {
            report.addError("Underscore in binary relation", literal.getSrcLoc());
        } else {
            std::cout << "Unsupported Literal type: " << typeid(literal).name() << "\n";
            assert(false && "Unsupported Argument Type!");
        }
    }
}

void AstSemanticChecker::checkAggregator(
        ErrorReport& report, const AstProgram& program, const AstAggregator& aggregator) {
    for (AstLiteral* literal : aggregator.getBodyLiterals()) {
        checkLiteral(report, program, *literal);
    }
}

void AstSemanticChecker::checkArgument(
        ErrorReport& report, const AstProgram& program, const AstArgument& arg) {
    if (const auto* agg = dynamic_cast<const AstAggregator*>(&arg)) {
        checkAggregator(report, program, *agg);
    } else if (const auto* intrFunc = dynamic_cast<const AstIntrinsicFunctor*>(&arg)) {
        for (size_t i = 0; i < intrFunc->getArity(); i++) {
            checkArgument(report, program, *intrFunc->getArg(i));
        }
    } else if (const auto* userDefFunc = dynamic_cast<const AstUserDefinedFunctor*>(&arg)) {
        for (size_t i = 0; i < userDefFunc->getArgCount(); i++) {
            checkArgument(report, program, *userDefFunc->getArg(i));
        }
    }
}

static bool isConstantArithExpr(const AstArgument& argument) {
    if (dynamic_cast<const AstNumberConstant*>(&argument)) {
        return true;
    }
    if (const auto* inf = dynamic_cast<const AstIntrinsicFunctor*>(&argument)) {
        if (!inf->isNumerical()) {
            return false;
        }

        for (size_t i = 0; i < inf->getArity(); i++) {
            if (!isConstantArithExpr(*inf->getArg(i))) {
                return false;
            }
        }

        // numerical intrinsic functor with all-constant arguments
        return true;
    }
    return false;
}

// TODO (azreika): refactor this (and isConstantArithExpr); confusing name/setup
void AstSemanticChecker::checkConstant(ErrorReport& report, const AstArgument& argument) {
    if (const auto* var = dynamic_cast<const AstVariable*>(&argument)) {
        report.addError("Variable " + var->getName() + " in fact", var->getSrcLoc());
    } else if (dynamic_cast<const AstUnnamedVariable*>(&argument)) {
        report.addError("Underscore in fact", argument.getSrcLoc());
    } else if (dynamic_cast<const AstIntrinsicFunctor*>(&argument)) {
        if (!isConstantArithExpr(argument)) {
            report.addError("Function in fact", argument.getSrcLoc());
        }
    } else if (dynamic_cast<const AstUserDefinedFunctor*>(&argument)) {
        report.addError("User-defined functor in fact", argument.getSrcLoc());
    } else if (auto* cast = dynamic_cast<const AstTypeCast*>(&argument)) {
        checkConstant(report, *cast->getValue());
    } else if (dynamic_cast<const AstCounter*>(&argument)) {
        report.addError("Counter in fact", argument.getSrcLoc());
    } else if (dynamic_cast<const AstConstant*>(&argument)) {
        // this one is fine - type checker will make sure of number and symbol constants
    } else if (auto* ri = dynamic_cast<const AstRecordInit*>(&argument)) {
        for (auto* arg : ri->getArguments()) {
            checkConstant(report, *arg);
        }
    } else {
        std::cout << "Unsupported Argument: " << typeid(argument).name() << "\n";
        assert(false && "Unknown case");
    }
}

/* Check if facts contain only constants */
void AstSemanticChecker::checkFact(ErrorReport& report, const AstProgram& program, const AstClause& fact) {
    assert(fact.isFact());

    AstAtom* head = fact.getHead();
    if (head == nullptr) {
        return;  // checked by clause
    }

    AstRelation* rel = program.getRelation(head->getName());
    if (rel == nullptr) {
        return;  // checked by clause
    }

    // facts must only contain constants
    for (size_t i = 0; i < head->argSize(); i++) {
        checkConstant(report, *head->getArgument(i));
    }
}

void AstSemanticChecker::checkClause(ErrorReport& report, const AstProgram& program, const AstClause& clause,
        const RecursiveClauses& recursiveClauses) {
    // check head atom
    checkAtom(report, program, *clause.getHead());

    // check for absence of underscores in head
    if (hasUnnamedVariable(clause.getHead())) {
        report.addError("Underscore in head of rule", clause.getHead()->getSrcLoc());
    }

    // check body literals
    for (AstLiteral* lit : clause.getAtoms()) {
        checkLiteral(report, program, *lit);
    }
    for (AstNegation* neg : clause.getNegations()) {
        checkLiteral(report, program, *neg);
    }
    for (AstLiteral* lit : clause.getConstraints()) {
        checkLiteral(report, program, *lit);
    }

    // check facts
    if (clause.isFact()) {
        checkFact(report, program, clause);
    }

    // check for use-once variables
    std::map<std::string, int> var_count;
    std::map<std::string, const AstVariable*> var_pos;
    visitDepthFirst(clause, [&](const AstVariable& var) {
        var_count[var.getName()]++;
        var_pos[var.getName()] = &var;
    });

    // check for variables only occurring once
    if (!clause.isGenerated()) {
        for (const auto& cur : var_count) {
            if (cur.second == 1 && cur.first[0] != '_') {
                report.addWarning(
                        "Variable " + cur.first + " only occurs once", var_pos[cur.first]->getSrcLoc());
            }
        }
    }

    // check execution plan
    if (clause.getExecutionPlan()) {
        auto numAtoms = clause.getAtoms().size();
        for (const auto& cur : clause.getExecutionPlan()->getOrders()) {
            if (cur.second->size() != numAtoms || !cur.second->isComplete()) {
                report.addError("Invalid execution plan", cur.second->getSrcLoc());
            }
        }
    }
    // check auto-increment
    if (recursiveClauses.recursive(&clause)) {
        visitDepthFirst(clause, [&](const AstCounter& ctr) {
            report.addError("Auto-increment functor in a recursive rule", ctr.getSrcLoc());
        });
    }
}

void AstSemanticChecker::checkRelationDeclaration(ErrorReport& report, const TypeEnvironment& typeEnv,
        const AstProgram& program, const AstRelation& relation, const IOType& ioTypes) {
    for (size_t i = 0; i < relation.getArity(); i++) {
        AstAttribute* attr = relation.getAttribute(i);
        AstTypeIdentifier typeName = attr->getTypeName();

        /* check whether type exists */
        if (typeName != "number" && typeName != "symbol" && !program.getType(typeName)) {
            report.addError("Undefined type in attribute " + attr->getAttributeName() + ":" +
                                    toString(attr->getTypeName()),
                    attr->getSrcLoc());
        }

        /* check whether name occurs more than once */
        for (size_t j = 0; j < i; j++) {
            if (attr->getAttributeName() == relation.getAttribute(j)->getAttributeName()) {
                report.addError("Doubly defined attribute name " + attr->getAttributeName() + ":" +
                                        toString(attr->getTypeName()),
                        attr->getSrcLoc());
            }
        }

        /* check whether type is a record type */
        if (typeEnv.isType(typeName)) {
            const Type& type = typeEnv.getType(typeName);
            if (isRecordType(type)) {
                // TODO (#467) remove the next line to enable subprogram compilation for record types
                Global::config().unset("engine");

                if (ioTypes.isInput(&relation)) {
                    report.addError(
                            "Input relations must not have record types. "
                            "Attribute " +
                                    attr->getAttributeName() + " has record type " +
                                    toString(attr->getTypeName()),
                            attr->getSrcLoc());
                }
                if (ioTypes.isOutput(&relation)) {
                    report.addWarning(
                            "Record types in output relations are not printed verbatim: attribute " +
                                    attr->getAttributeName() + " has record type " +
                                    toString(attr->getTypeName()),
                            attr->getSrcLoc());
                }
            }
        }
    }
}

void AstSemanticChecker::checkRelation(ErrorReport& report, const TypeEnvironment& typeEnv,
        const AstProgram& program, const AstRelation& relation, const RecursiveClauses& recursiveClauses,
        const IOType& ioTypes) {
    if (relation.getRepresentation() == RelationRepresentation::EQREL) {
        if (relation.getArity() == 2) {
            if (relation.getAttribute(0)->getTypeName() != relation.getAttribute(1)->getTypeName()) {
                report.addError(
                        "Domains of equivalence relation " + toString(relation.getName()) + " are different",
                        relation.getSrcLoc());
            }
        } else {
            report.addError("Equivalence relation " + toString(relation.getName()) + " is not binary",
                    relation.getSrcLoc());
        }
    }

    // start with declaration
    checkRelationDeclaration(report, typeEnv, program, relation, ioTypes);

    // check clauses
    for (AstClause* c : relation.getClauses()) {
        checkClause(report, program, *c, recursiveClauses);
    }

    // check whether this relation is empty
    if (relation.clauseSize() == 0 && !ioTypes.isInput(&relation) && !relation.isSuppressed()) {
        report.addWarning(
                "No rules/facts defined for relation " + toString(relation.getName()), relation.getSrcLoc());
    }
}

void AstSemanticChecker::checkRules(ErrorReport& report, const TypeEnvironment& typeEnv,
        const AstProgram& program, const RecursiveClauses& recursiveClauses, const IOType& ioTypes) {
    for (AstRelation* cur : program.getRelations()) {
        checkRelation(report, typeEnv, program, *cur, recursiveClauses, ioTypes);
    }

    for (AstClause* cur : program.getOrphanClauses()) {
        checkClause(report, program, *cur, recursiveClauses);
    }
}

// ----- types --------

// check if a union contains a number primitive
static bool unionContainsNumber(const AstProgram& program, const AstUnionType& type) {
    // check if any of the elements of the union are or contain a number primitive
    for (const AstTypeIdentifier& elemTypeID : type.getTypes()) {
        if (elemTypeID == "number") {
            return true;
        }
        const AstType* elemType = program.getType(elemTypeID);
        if (const auto* unionT = dynamic_cast<const AstUnionType*>(elemType)) {
            if (unionContainsNumber(program, *unionT)) {
                return true;
            }
            // if union does not contain a number, continue looking
        }
        if (const auto* primitive = dynamic_cast<const AstPrimitiveType*>(elemType)) {
            if (primitive->isNumeric()) {
                return true;
            }
            // if this primitive is not numeric, continue looking
        }
    }
    // no elements returned true, so no numbers
    return false;
}

// check if a union contains a symbol primitive
static bool unionContainsSymbol(const AstProgram& program, const AstUnionType& type) {
    // check if any of the elements of the union are or contain a symbol primitive
    for (const AstTypeIdentifier& elemTypeID : type.getTypes()) {
        if (elemTypeID == "symbol") {
            return true;
        }
        const AstType* elemType = program.getType(elemTypeID);
        if (const auto* unionT = dynamic_cast<const AstUnionType*>(elemType)) {
            if (unionContainsSymbol(program, *unionT)) {
                return true;
            }
            // if the union does not contain a symbol, continue looking
        }
        if (const auto* primitive = dynamic_cast<const AstPrimitiveType*>(elemType)) {
            if (primitive->isSymbolic()) {
                return true;
            }
            // if this primitive is not a symbol, continue looking
        }
    }
    // no elements returned true, so no symbols
    return false;
}

void AstSemanticChecker::checkUnionType(
        ErrorReport& report, const AstProgram& program, const AstUnionType& type) {
    // check presence of all the element types and that all element types are based off a primitive
    for (const AstTypeIdentifier& sub : type.getTypes()) {
        if (sub != "number" && sub != "symbol") {
            const AstType* subt = program.getType(sub);
            if (!subt) {
                report.addError("Undefined type " + toString(sub) + " in definition of union type " +
                                        toString(type.getName()),
                        type.getSrcLoc());
            } else if (!dynamic_cast<const AstUnionType*>(subt) &&
                       !dynamic_cast<const AstPrimitiveType*>(subt)) {
                report.addError("Union type " + toString(type.getName()) +
                                        " contains the non-primitive type " + toString(sub),
                        type.getSrcLoc());
            }
        }
    }

    // check all element types are based on the same primitive
    if (unionContainsSymbol(program, type) && unionContainsNumber(program, type)) {
        report.addError(
                "Union type " + toString(type.getName()) + " contains a mixture of symbol and number types",
                type.getSrcLoc());
    }
}

void AstSemanticChecker::checkRecordType(
        ErrorReport& report, const AstProgram& program, const AstRecordType& type) {
    // check proper definition of all field types
    for (const auto& field : type.getFields()) {
        if (field.type != "number" && field.type != "symbol" && !program.getType(field.type)) {
            report.addError(
                    "Undefined type " + toString(field.type) + " in definition of field " + field.name,
                    type.getSrcLoc());
        }
    }

    // check that field names are unique
    auto& fields = type.getFields();
    std::size_t numFields = fields.size();
    for (std::size_t i = 0; i < numFields; i++) {
        const std::string& cur_name = fields[i].name;
        for (std::size_t j = 0; j < i; j++) {
            if (fields[j].name == cur_name) {
                report.addError("Doubly defined field name " + cur_name + " in definition of type " +
                                        toString(type.getName()),
                        type.getSrcLoc());
            }
        }
    }
}

void AstSemanticChecker::checkType(ErrorReport& report, const AstProgram& program, const AstType& type) {
    if (const auto* u = dynamic_cast<const AstUnionType*>(&type)) {
        checkUnionType(report, program, *u);
    } else if (const auto* r = dynamic_cast<const AstRecordType*>(&type)) {
        checkRecordType(report, program, *r);
    }
}

void AstSemanticChecker::checkTypes(ErrorReport& report, const AstProgram& program) {
    // check each type individually
    for (const auto& cur : program.getTypes()) {
        checkType(report, program, *cur);
    }
}

void AstSemanticChecker::checkIODirectives(ErrorReport& report, const AstProgram& program) {
    auto checkIODirective = [&](const AstIO* directive) {
#ifdef USE_MPI
        // TODO (lyndonhenry): should permit sqlite as an io directive for use with mpi
        auto it = directive->getIODirectiveMap().find("IO");
        if (it != directive->getIODirectiveMap().end() && it->second == "sqlite") {
            Global::config().unset("engine");
        }
#endif
        auto* r = program.getRelation(directive->getName());
        if (r == nullptr) {
            report.addError("Undefined relation " + toString(directive->getName()), directive->getSrcLoc());
        }
    };
    for (const auto& directive : program.getLoads()) {
        checkIODirective(directive.get());
    }
    for (const auto& directive : program.getPrintSizes()) {
        checkIODirective(directive.get());
    }
    for (const auto& directive : program.getStores()) {
        checkIODirective(directive.get());
    }
}

static const std::vector<SrcLocation> usesInvalidWitness(const std::vector<AstLiteral*>& literals,
        const std::set<std::unique_ptr<AstArgument>>& groundedArguments) {
    // Node-mapper that replaces aggregators with new (unique) variables
    struct M : public AstNodeMapper {
        // Variables introduced to replace aggregators
        mutable std::set<std::string> aggregatorVariables;

        const std::set<std::string>& getAggregatorVariables() {
            return aggregatorVariables;
        }

        std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
            static int numReplaced = 0;
            if (dynamic_cast<AstAggregator*>(node.get())) {
                // Replace the aggregator with a variable
                std::stringstream newVariableName;
                newVariableName << "+aggr_var_" << numReplaced++;

                // Keep track of which variables are bound to aggregators
                aggregatorVariables.insert(newVariableName.str());

                return std::make_unique<AstVariable>(newVariableName.str());
            }
            node->apply(*this);
            return node;
        }
    };

    std::vector<SrcLocation> result;

    // Create two versions of the original clause

    // Clause 1 - will remain equivalent to the original clause in terms of variable groundedness
    auto originalClause = std::make_unique<AstClause>();
    originalClause->setHead(std::make_unique<AstAtom>("*"));

    // Clause 2 - will have aggregators replaced with intrinsically grounded variables
    auto aggregatorlessClause = std::make_unique<AstClause>();
    aggregatorlessClause->setHead(std::make_unique<AstAtom>("*"));

    // Construct both clauses in the same manner to match the original clause
    // Must keep track of the subnode in Clause 1 that each subnode in Clause 2 matches to
    std::map<const AstArgument*, const AstArgument*> identicalSubnodeMap;
    for (const AstLiteral* lit : literals) {
        auto firstClone = std::unique_ptr<AstLiteral>(lit->clone());
        auto secondClone = std::unique_ptr<AstLiteral>(lit->clone());

        // Construct the mapping between equivalent literal subnodes
        std::vector<const AstArgument*> firstCloneArguments;
        visitDepthFirst(*firstClone, [&](const AstArgument& arg) { firstCloneArguments.push_back(&arg); });

        std::vector<const AstArgument*> secondCloneArguments;
        visitDepthFirst(*secondClone, [&](const AstArgument& arg) { secondCloneArguments.push_back(&arg); });

        for (size_t i = 0; i < firstCloneArguments.size(); i++) {
            identicalSubnodeMap[secondCloneArguments[i]] = firstCloneArguments[i];
        }

        // Actually add the literal clones to each clause
        originalClause->addToBody(std::move(firstClone));
        aggregatorlessClause->addToBody(std::move(secondClone));
    }

    // Replace the aggregators in Clause 2 with variables
    M update;
    aggregatorlessClause->apply(update);

    // Create a dummy atom to force certain arguments to be grounded in the aggregatorlessClause
    auto groundingAtomAggregatorless = std::make_unique<AstAtom>("grounding_atom");
    auto groundingAtomOriginal = std::make_unique<AstAtom>("grounding_atom");

    // Force the new aggregator variables to be grounded in the aggregatorless clause
    const std::set<std::string>& aggregatorVariables = update.getAggregatorVariables();
    for (const std::string& str : aggregatorVariables) {
        groundingAtomAggregatorless->addArgument(std::make_unique<AstVariable>(str));
    }

    // Force the given grounded arguments to be grounded in both clauses
    for (const std::unique_ptr<AstArgument>& arg : groundedArguments) {
        groundingAtomAggregatorless->addArgument(std::unique_ptr<AstArgument>(arg->clone()));
        groundingAtomOriginal->addArgument(std::unique_ptr<AstArgument>(arg->clone()));
    }

    aggregatorlessClause->addToBody(std::move(groundingAtomAggregatorless));
    originalClause->addToBody(std::move(groundingAtomOriginal));

    // Compare the grounded analysis of both generated clauses
    // All added arguments in Clause 2 were forced to be grounded, so if an ungrounded argument
    // appears in Clause 2, it must also appear in Clause 1. Consequently, have two cases:
    //   - The argument is also ungrounded in Clause 1 - handled by another check
    //   - The argument is grounded in Clause 1 => the argument was grounded in the
    //     first clause somewhere along the line by an aggregator-body - not allowed!
    std::set<std::unique_ptr<AstArgument>> newlyGroundedArguments;
    std::map<const AstArgument*, bool> originalGrounded = getGroundedTerms(*originalClause);
    std::map<const AstArgument*, bool> aggregatorlessGrounded = getGroundedTerms(*aggregatorlessClause);
    for (auto pair : aggregatorlessGrounded) {
        if (!pair.second && originalGrounded[identicalSubnodeMap[pair.first]]) {
            result.push_back(pair.first->getSrcLoc());
        }

        // Otherwise, it can now be considered grounded
        newlyGroundedArguments.insert(std::unique_ptr<AstArgument>(pair.first->clone()));
    }

    // All previously grounded are still grounded
    for (const std::unique_ptr<AstArgument>& arg : groundedArguments) {
        newlyGroundedArguments.insert(std::unique_ptr<AstArgument>(arg->clone()));
    }

    // Everything on this level is fine, check subaggregators of each literal
    for (const AstLiteral* lit : literals) {
        visitDepthFirst(*lit, [&](const AstAggregator& aggr) {
            // Check recursively if an invalid witness is used
            std::vector<AstLiteral*> aggrBodyLiterals = aggr.getBodyLiterals();
            std::vector<SrcLocation> subresult = usesInvalidWitness(aggrBodyLiterals, newlyGroundedArguments);
            for (SrcLocation argloc : subresult) {
                result.push_back(argloc);
            }
        });
    }

    return result;
}

void AstSemanticChecker::checkWitnessProblem(ErrorReport& report, const AstProgram& program) {
    // Visit each clause to check if an invalid aggregator witness is used
    visitDepthFirst(program, [&](const AstClause& clause) {
        // Body literals of the clause to check
        std::vector<AstLiteral*> bodyLiterals = clause.getBodyLiterals();

        // Add in all head variables as new ungrounded body literals
        auto headVariables = std::make_unique<AstAtom>("*");
        visitDepthFirst(*clause.getHead(), [&](const AstVariable& var) {
            headVariables->addArgument(std::unique_ptr<AstVariable>(var.clone()));
        });
        AstNegation* headNegation = new AstNegation(std::move(headVariables));
        bodyLiterals.push_back(headNegation);

        // Perform the check
        std::set<std::unique_ptr<AstArgument>> groundedArguments;
        std::vector<SrcLocation> invalidArguments = usesInvalidWitness(bodyLiterals, groundedArguments);
        for (SrcLocation invalidArgument : invalidArguments) {
            report.addError(
                    "Witness problem: argument grounded by an aggregator's inner scope is used ungrounded in "
                    "outer scope",
                    invalidArgument);
        }

        delete headNegation;
    });
}

/**
 * Find a cycle consisting entirely of inlined relations.
 * If no cycle exists, then an empty vector is returned.
 */
std::vector<AstRelationIdentifier> findInlineCycle(const PrecedenceGraph& precedenceGraph,
        std::map<const AstRelation*, const AstRelation*>& origins, const AstRelation* current,
        AstRelationSet& unvisited, AstRelationSet& visiting, AstRelationSet& visited) {
    std::vector<AstRelationIdentifier> result;

    if (current == nullptr) {
        // Not looking at any nodes at the moment, so choose any node from the unvisited list

        if (unvisited.empty()) {
            // Nothing left to visit - so no cycles exist!
            return result;
        }

        // Choose any element from the unvisited set
        current = *unvisited.begin();
        origins[current] = nullptr;

        // Move it to "currently visiting"
        unvisited.erase(current);
        visiting.insert(current);

        // Check if we can find a cycle beginning from this node
        std::vector<AstRelationIdentifier> subresult =
                findInlineCycle(precedenceGraph, origins, current, unvisited, visiting, visited);

        if (subresult.empty()) {
            // No cycle found, try again from another node
            return findInlineCycle(precedenceGraph, origins, nullptr, unvisited, visiting, visited);
        } else {
            // Cycle found! Return it
            return subresult;
        }
    }

    // Check neighbours
    const AstRelationSet& successors = precedenceGraph.graph().successors(current);
    for (const AstRelation* successor : successors) {
        // Only care about inlined neighbours in the graph
        if (successor->isInline()) {
            if (visited.find(successor) != visited.end()) {
                // The neighbour has already been visited, so move on
                continue;
            }

            if (visiting.find(successor) != visiting.end()) {
                // Found a cycle!!
                // Construct the cycle in reverse
                while (current != nullptr) {
                    result.push_back(current->getName());
                    current = origins[current];
                }
                return result;
            }

            // Node has not been visited yet
            origins[successor] = current;

            // Move from unvisited to visiting
            unvisited.erase(successor);
            visiting.insert(successor);

            // Visit recursively and check if a cycle is formed
            std::vector<AstRelationIdentifier> subgraphCycle =
                    findInlineCycle(precedenceGraph, origins, successor, unvisited, visiting, visited);

            if (!subgraphCycle.empty()) {
                // Found a cycle!
                return subgraphCycle;
            }
        }
    }

    // Visited all neighbours with no cycle found, so done visiting this node.
    visiting.erase(current);
    visited.insert(current);
    return result;
}

void AstSemanticChecker::checkInlining(ErrorReport& report, const AstProgram& program,
        const PrecedenceGraph& precedenceGraph, const IOType& ioTypes) {
    // Find all inlined relations
    AstRelationSet inlinedRelations;
    for (const auto& relation : program.getRelations()) {
        if (relation->isInline()) {
            inlinedRelations.insert(relation);
            if (ioTypes.isIO(relation)) {
                report.addError("IO relation " + toString(relation->getName()) + " cannot be inlined",
                        relation->getSrcLoc());
            }
        }
    }

    // Check 1:
    // Let G' be the subgraph of the precedence graph G containing only those nodes
    // which are marked with the inline directive.
    // If G' contains a cycle, then inlining cannot be performed.

    AstRelationSet unvisited;  // nodes that have not been visited yet
    AstRelationSet visiting;   // nodes that we are currently visiting
    AstRelationSet visited;    // nodes that have been completely explored

    // All nodes are initially unvisited
    for (const AstRelation* rel : inlinedRelations) {
        unvisited.insert(rel);
    }

    // Remember the parent node of each visited node to construct the found cycle
    std::map<const AstRelation*, const AstRelation*> origins;

    std::vector<AstRelationIdentifier> result =
            findInlineCycle(precedenceGraph, origins, nullptr, unvisited, visiting, visited);

    // If the result contains anything, then a cycle was found
    if (!result.empty()) {
        AstRelation* cycleOrigin = program.getRelation(result[result.size() - 1]);

        // Construct the string representation of the cycle
        std::stringstream cycle;
        cycle << "{" << cycleOrigin->getName();

        // Print it backwards to preserve the initial cycle order
        for (int i = result.size() - 2; i >= 0; i--) {
            cycle << ", " << result[i];
        }

        cycle << "}";

        report.addError(
                "Cannot inline cyclically dependent relations " + cycle.str(), cycleOrigin->getSrcLoc());
    }

    // Check 2:
    // Cannot use the counter argument ('$') in inlined relations

    // Check if an inlined literal ever takes in a $
    visitDepthFirst(program, [&](const AstAtom& atom) {
        AstRelation* associatedRelation = program.getRelation(atom.getName());
        if (associatedRelation != nullptr && associatedRelation->isInline()) {
            visitDepthFirst(atom, [&](const AstArgument& arg) {
                if (dynamic_cast<const AstCounter*>(&arg)) {
                    report.addError(
                            "Cannot inline literal containing a counter argument '$'", arg.getSrcLoc());
                }
            });
        }
    });

    // Check if an inlined clause ever contains a $
    for (const AstRelation* rel : inlinedRelations) {
        for (AstClause* clause : rel->getClauses()) {
            visitDepthFirst(*clause, [&](const AstArgument& arg) {
                if (dynamic_cast<const AstCounter*>(&arg)) {
                    report.addError(
                            "Cannot inline clause containing a counter argument '$'", arg.getSrcLoc());
                }
            });
        }
    }

    // Check 3:
    // Suppose the relation b is marked with the inline directive, but appears negated
    // in a clause. Then, if b introduces a new variable in its body, we cannot inline
    // the relation b.

    // Find all relations with the inline declarative that introduce new variables in their bodies
    AstRelationSet nonNegatableRelations;
    for (const AstRelation* rel : inlinedRelations) {
        bool foundNonNegatable = false;
        for (const AstClause* clause : rel->getClauses()) {
            // Get the variables in the head
            std::set<std::string> headVariables;
            visitDepthFirst(
                    *clause->getHead(), [&](const AstVariable& var) { headVariables.insert(var.getName()); });

            // Get the variables in the body
            std::set<std::string> bodyVariables;
            visitDepthFirst(clause->getBodyLiterals(),
                    [&](const AstVariable& var) { bodyVariables.insert(var.getName()); });

            // Check if all body variables are in the head
            // Do this separately to the above so only one error is printed per variable
            for (const std::string& var : bodyVariables) {
                if (headVariables.find(var) == headVariables.end()) {
                    nonNegatableRelations.insert(rel);
                    foundNonNegatable = true;
                    break;
                }
            }

            if (foundNonNegatable) {
                break;
            }
        }
    }

    // Check that these relations never appear negated
    visitDepthFirst(program, [&](const AstNegation& neg) {
        AstRelation* associatedRelation = program.getRelation(neg.getAtom()->getName());
        if (associatedRelation != nullptr &&
                nonNegatableRelations.find(associatedRelation) != nonNegatableRelations.end()) {
            report.addError(
                    "Cannot inline negated relation which may introduce new variables", neg.getSrcLoc());
        }
    });

    // Check 4:
    // Don't support inlining atoms within aggregators at this point.

    // Reasoning: Suppose we have an aggregator like `max X: a(X)`, where `a` is inlined to `a1` and `a2`.
    // Then, `max X: a(X)` will become `max( max X: a1(X),  max X: a2(X) )`. Suppose further that a(X) has
    // values X where it is true, while a2(X) does not. Then, the produced argument
    // `max( max X: a1(X),  max X: a2(X) )` will not return anything (as one of its arguments fails), while
    // `max X: a(X)` will.

    // This corner case prevents generalising aggregator inlining with the current set up.

    visitDepthFirst(program, [&](const AstAggregator& aggr) {
        visitDepthFirst(aggr, [&](const AstAtom& subatom) {
            const AstRelation* rel = program.getRelation(subatom.getName());
            if (rel != nullptr && rel->isInline()) {
                report.addError("Cannot inline relations that appear in aggregator", subatom.getSrcLoc());
            }
        });
    });

    // Check 5:
    // Suppose a relation `a` is inlined, appears negated in a clause, and contains a
    // (possibly nested) unnamed variable in its arguments. Then, the atom can't be
    // inlined, as unnamed variables are named during inlining (since they may appear
    // multiple times in an inlined-clause's body) => ungroundedness!

    // Exception: It's fine if the unnamed variable appears in a nested aggregator, as
    // the entire aggregator will automatically be grounded.

    // TODO (azreika): special case where all rules defined for `a` use the
    // underscored-argument exactly once: can workaround by remapping the variable
    // back to an underscore - involves changes to the actual inlining algo, though

    // Returns the pair (isValid, lastSrcLoc) where:
    //  - isValid is true if and only if the node contains an invalid underscore, and
    //  - lastSrcLoc is the source location of the last visited node
    std::function<std::pair<bool, SrcLocation>(const AstNode*)> checkInvalidUnderscore =
            [&](const AstNode* node) {
                if (dynamic_cast<const AstUnnamedVariable*>(node)) {
                    // Found an invalid underscore
                    return std::make_pair(true, node->getSrcLoc());
                } else if (dynamic_cast<const AstAggregator*>(node)) {
                    // Don't care about underscores within aggregators
                    return std::make_pair(false, node->getSrcLoc());
                }

                // Check if any children nodes use invalid underscores
                for (const AstNode* child : node->getChildNodes()) {
                    std::pair<bool, SrcLocation> childStatus = checkInvalidUnderscore(child);
                    if (childStatus.first) {
                        // Found an invalid underscore
                        return childStatus;
                    }
                }

                return std::make_pair(false, node->getSrcLoc());
            };

    // Perform the check
    visitDepthFirst(program, [&](const AstNegation& negation) {
        const AstAtom* associatedAtom = negation.getAtom();
        const AstRelation* associatedRelation = program.getRelation(associatedAtom->getName());
        if (associatedRelation != nullptr && associatedRelation->isInline()) {
            std::pair<bool, SrcLocation> atomStatus = checkInvalidUnderscore(associatedAtom);
            if (atomStatus.first) {
                report.addError(
                        "Cannot inline negated atom containing an unnamed variable unless the variable is "
                        "within an aggregator",
                        atomStatus.second);
            }
        }
    });
}

// Check that type and relation names are disjoint sets.
void AstSemanticChecker::checkNamespaces(ErrorReport& report, const AstProgram& program) {
    std::map<std::string, SrcLocation> names;

    // Find all names and report redeclarations as we go.
    for (const auto& type : program.getTypes()) {
        const std::string name = toString(type->getName());
        if (names.count(name)) {
            report.addError("Name clash on type " + name, type->getSrcLoc());
        } else {
            names[name] = type->getSrcLoc();
        }
    }

    for (const auto& rel : program.getRelations()) {
        const std::string name = toString(rel->getName());
        if (names.count(name)) {
            report.addError("Name clash on relation " + name, rel->getSrcLoc());
        } else {
            names[name] = rel->getSrcLoc();
        }
    }
}

bool AstExecutionPlanChecker::transform(AstTranslationUnit& translationUnit) {
    auto* relationSchedule = translationUnit.getAnalysis<RelationSchedule>();
    auto* recursiveClauses = translationUnit.getAnalysis<RecursiveClauses>();

    for (const RelationScheduleStep& step : relationSchedule->schedule()) {
        const std::set<const AstRelation*>& scc = step.computed();
        for (const AstRelation* rel : scc) {
            for (const AstClause* clause : rel->getClauses()) {
                if (!recursiveClauses->recursive(clause)) {
                    continue;
                }
                if (!clause->getExecutionPlan()) {
                    continue;
                }
                int version = 0;
                for (const AstAtom* atom : clause->getAtoms()) {
                    if (scc.count(getAtomRelation(atom, translationUnit.getProgram()))) {
                        version++;
                    }
                }
                if (version <= clause->getExecutionPlan()->getMaxVersion()) {
                    for (const auto& cur : clause->getExecutionPlan()->getOrders()) {
                        if (cur.first >= version) {
                            translationUnit.getErrorReport().addDiagnostic(Diagnostic(Diagnostic::ERROR,
                                    DiagnosticMessage(
                                            "execution plan for version " + std::to_string(cur.first),
                                            cur.second->getSrcLoc()),
                                    {DiagnosticMessage("only versions 0.." + std::to_string(version - 1) +
                                                       " permitted")}));
                        }
                    }
                }
            }
        }
    }
    return false;
}

}  // end of namespace souffle
