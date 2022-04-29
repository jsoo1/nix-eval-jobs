#include <nix/config.h>
#include <nix/attr-path.hh>
#include <nix/eval.hh>
#include <nlohmann/json.hpp>

#include "args.hh"
#include "job.hh"
#include "accessor.hh"

using namespace nix;

using json = nlohmann::json;

namespace nix_eval_jobs {

/* Parse an accessor from json, the introduction rule. */
static std::unique_ptr<Accessor> accessorFromJson(const json & json) {
    try {
        return std::make_unique<Index>(json);
    } catch (...) {
        try {
            return std::make_unique<Name>(json);
        } catch (...) {
            throw TypeError("could not make an accessor out of json: %s", json.dump());
        }
    }
}

/* Accessor */

Index::Index(const json & json) {
    try {
        val = json;
    } catch (...)  {
        throw TypeError("could not make an index out of json: %s", json.dump());
    }
}

Index::Index(const Index & that) {
    this->val = that.val;
}

Name::Name(const json & json) {
    try {
        val = json;
        if (val.empty()) throw EvalError("empty attribute name");
    } catch (...) {
        throw TypeError("could not create an attrname out of json: %s", json.dump());
    }
}

Name::Name(const Name & that) {
    this->val = that.val;
}

/* clone : Accessor -> Accessor */
std::unique_ptr<Accessor> Index::clone() {
    return std::make_unique<Index>(*this);
}

std::unique_ptr<Accessor> Name::clone() {
    return std::make_unique<Name>(*this);
}
/* toJson : Accessor -> json */

json Name::toJson() {
    return val;
}

json Index::toJson() {
    return val;
}

/* AccessorPath */

AccessorPath::AccessorPath(std::string & s) {
    json intermediate;
    try {
        intermediate = json::parse(s);

    } catch (json::exception & e) {
        throw TypeError("error parsing accessor path json: %s", s);
    }

    try {
        std::vector<json> vec = intermediate;
        for (auto j : vec)
            this->path.push_back(accessorFromJson(j));

    } catch (json::exception & e) {
        throw TypeError("could not make an accessor path out of json, expected a list of accessors: %s", intermediate.dump());
    }
}

/* Make an AttrPath out of AccessorPath findAlongAttrPath */

static std::string accessorPathToAttrPath(AccessorPath & accessors) {
    std::stringstream ss;

    auto begin = accessors.path.begin();
    auto end = accessors.path.end();

    if (begin != end) {
        ss << begin->get()->toJson();
        ++begin;
    }

    while (begin != end) {
        ss << "." << begin->get()->toJson();
        ++begin;
    }

    return ss.str();
}

/* walk : AccessorPath -> EvalState -> Bindings -> Value -> Job */

std::unique_ptr<Job> AccessorPath::walk(EvalState & state, Bindings & autoArgs, Value & vRoot) {
    auto [vRes, pos] =
        findAlongAttrPath(state, accessorPathToAttrPath(*this), autoArgs, vRoot);

    return getJob(state, autoArgs, *vRes);
}

/* toJson : ToJson -> json */

json AccessorPath::toJson() {
    std::vector<json> res;
    for (auto & a : path)
        res.push_back(a->toJson());

    return res;
}

}
