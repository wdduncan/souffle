/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2014, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file RelationStatement.h
 *
 ***********************************************************************/

#pragma once

#include "ram/Node.h"
#include "ram/Relation.h"
#include "ram/Statement.h"
#include "ram/utility/NodeMapper.h"
#include "souffle/utility/ContainerUtil.h"
#include <cassert>
#include <memory>
#include <utility>
#include <vector>

namespace souffle::ram {

/**
 * @class RelationStatement
 * @brief RAM Statements with a single relation
 */
class RelationStatement : public Statement {
public:
    RelationStatement(std::string rel) : relation(rel) {
    }

    /** @brief Get RAM relation */
    const std::string &getRelation() const {
        return relation;
    }

protected:
    bool equal(const Node& node) const override {
        const auto& other = static_cast<const RelationStatement&>(node);
        return relation == other.relation;
    }

protected:
    /** relation */
    std::string relation;
};

}  // namespace souffle::ram
