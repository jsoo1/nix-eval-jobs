#pragma once

#include <nix/eval.hh>
#include <nlohmann/json.hpp>

using namespace nix;

namespace nix_eval_jobs {

class Job;
struct MyArgs;

/* Ways to look into a value.  This is how nix-eval-jobs "recurses"
   over nix exprs. Accessor gets the next elem, AccessorPath finds a
   value in nested exprs.

   Accessor := Index | Name
 */
class Accessor {
public:
    virtual nlohmann::json toJson() = 0;
    virtual ~Accessor() { }
};

/* An index into a list */
struct Index : Accessor {
    unsigned long val;
    Index(const nlohmann::json & json);
    nlohmann::json toJson() override;
    ~Index() { }
};

/* An attribute name in an attrset */
struct Name : Accessor {
    std::string val;
    Name(const nlohmann::json & json);
 nlohmann::json toJson() override;
    ~Name() { }
};

/* Follow a path into a nested nixexpr */
struct AccessorPath {
    std::vector<std::unique_ptr<Accessor>> path;
    AccessorPath(std::string & s);
    /* walk : AccessorPath -> EvalState -> Bindings -> Value -> Job */
    std::unique_ptr<Job> walk(MyArgs & myArgs, EvalState & state, Bindings & autoArgs, Value & vRoot);
    nlohmann::json toJson();
    ~AccessorPath() { }
};

}
