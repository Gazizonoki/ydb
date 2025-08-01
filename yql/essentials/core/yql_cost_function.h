#pragma once

#include <util/generic/hash.h>
#include <util/generic/vector.h>
#include <util/generic/string.h>

#include <set>

/**
 * The cost function for cost based optimizer currently consists of methods for computing
 * both the cost and cardinalities of individual plan operators
*/
namespace NYql {

struct IProviderContext;

enum class EJoinAlgoType {
    Undefined,
    LookupJoin,
    LookupJoinReverse,
    MapJoin,
    GraceJoin,
    StreamLookupJoin, //Right part can be updated during an operation. Used mainly for joining streams with lookup tables. Currently impplemented in Dq by LookupInputTransform
    MergeJoin  // To be used in YT
};

//StreamLookupJoin is not a subject for CBO and not not included here
static constexpr auto AllJoinAlgos = { EJoinAlgoType::LookupJoin, EJoinAlgoType::LookupJoinReverse, EJoinAlgoType::MapJoin, EJoinAlgoType::GraceJoin, EJoinAlgoType::MergeJoin };

namespace NDq {

/**
 * Join column is a struct that records the relation label and
 * attribute name, used in join conditions
*/
struct TJoinColumn {
    TString RelName{};  // TODO: this should be a list. now list of relations is separated by comma in this string
    TString AttributeName{};
    TString AttributeNameWithAliases{};
    std::optional<TString> OriginalRelName{};
    std::optional<ui32> EquivalenceClass{};
    bool IsConstant = false;

    TJoinColumn() = default;

    TJoinColumn(TString relName, TString attributeName) :
        RelName(relName),
        AttributeName(attributeName),
        AttributeNameWithAliases(attributeName) {}

    bool operator == (const TJoinColumn& other) const {
        return RelName == other.RelName && AttributeName == other.AttributeName;
    }

    static TJoinColumn FromString(const TString& column) {
        if (column.find('.') != TString::npos) {
            return TJoinColumn(column.substr(0, column.find('.')), column.substr(column.find('.') + 1));
        }

        return TJoinColumn("", column);
    }

    struct THashFunction
    {
        size_t operator()(const TJoinColumn& c) const
        {
            return THash<TString>{}(c.RelName) ^ THash<TString>{}(c.AttributeName);
        }
    };
};

bool operator < (const TJoinColumn& c1, const TJoinColumn& c2);

}  // namespace NDq

}  // namespace NYql
