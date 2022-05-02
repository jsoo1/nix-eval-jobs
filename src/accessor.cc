#include <nix/config.h>
#include <nix/attr-path.hh>
#include <nix/eval.hh>
#include <nlohmann/json.hpp>

#include "args.hh"
#include "job.hh"
#include "accessor.hh"

using namespace nix;

using namespace nlohmann;

namespace nix_eval_jobs {

/* Parse an accessor from json, the introduction rule. */
std::unique_ptr<Accessor> accessorFromJson(const json & json) {
    try {
        return std::make_unique<Index>(json);
    } catch (TypeError & e1) {
        try {
            return std::make_unique<Name>(json);
        } catch (TypeError & e2) {
            throw TypeError("could not make an accessor out of json: %s, %s", e1.msg(), e2.msg());
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

Name::Name(const json & json) {
    try {
        val = json;
        if (val.empty()) throw EvalError("empty attribute name");
    } catch (...) {
        throw TypeError("could not create an attrname out of json: %s", json.dump());
    }
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
            this->path->push_back(accessorFromJson(j));

    } catch (json::exception & e) {
        throw TypeError("could not make an accessor path out of json, expected a list of accessors: %s", intermediate.dump());
    }
}

AccessorPath::AccessorPath(const json & j) {
    try {
        std::vector<json> vec = j;
        for (auto i : vec)
            this->path->push_back(accessorFromJson(i));
    } catch (json::exception & e) {
        throw TypeError("could not make an accessor path out of json, expected a list of accessors: %s", j.dump());
    }
}

/* Make an AttrPath out of AccessorPath findAlongAttrPath */

std::string AccessorPath::toAttrPath() {
    std::stringstream ss;

    auto begin = this->path->begin();
    auto end = this->path->end();

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
        findAlongAttrPath(state, this->toAttrPath(), autoArgs, vRoot);

    return getJob(state, autoArgs, *vRes);
}

/* toJson : AccessorPath -> json */

void to_json(json & j, const AccessorPath & accessors) {
    std::vector<json> res;
    for (auto & a : *accessors.path)
        res.push_back(a->toJson());

    j = res;
}

json AccessorPath::toJson() {
    json j;
    to_json(j, *this);
    return j;
}

}
