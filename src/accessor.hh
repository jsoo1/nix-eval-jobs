#pragma once

#include <nix/eval.hh>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace nix;

namespace nix_eval_jobs {

class Job;

/* Ways to look into a value.  This is how nix-eval-jobs "recurses"
   over nix exprs. Accessor gets the next elem, AccessorPath finds a
   value in nested exprs.

   Accessor := Index | Name
 */
class Accessor {
public:
    virtual json toJson() = 0;
    virtual ~Accessor() { }
};

/* Parse an Accessor from json */
std::unique_ptr<Accessor> accessorFromJson(const json & json);

/* An index into a list */
struct Index : Accessor {
    unsigned long val;
    Index(const json & json);
    Index(const Index & that) = default;
    json toJson() override;
    ~Index() { }
};

/* An attribute name in an attrset */
struct Name : Accessor {
    std::string val;
    Name(const json & json);
    Name(const Name & that) = default;
    json toJson() override;
    ~Name() { }
};

/* Follow a path into a nested nixexpr */
struct AccessorPath {
    std::shared_ptr<std::vector<std::unique_ptr<Accessor>>> path;
    AccessorPath()
        : path(std::make_shared<std::vector<std::unique_ptr<Accessor>>>(std::vector<std::unique_ptr<Accessor>>{})) { };
    AccessorPath(std::string & s);
    AccessorPath(const json & j);
    AccessorPath(const AccessorPath & that) = default;
    /* walk : AccessorPath -> EvalState -> Bindings -> Value -> Job */
    std::unique_ptr<Job> walk(EvalState & state, Bindings & autoArgs, Value & vRoot);
    json toJson();
    ~AccessorPath() { }
};

void to_json(json & j, const AccessorPath & accessors);

}
