#include <nix/eval.hh>
#include <nix/get-drvs.hh>
#include <nix/local-fs-store.hh>
#include <nix/value-to-json.hh>

#include <nlohmann/json.hpp>
#include "accessor.hh"

using json = nlohmann::json;
using namespace nix;

namespace nix_eval_jobs {

class Accessor;
struct AccessorPath;
struct MyArgs;

/* JobEvalResult := JobChildren (Vector Accessor) | Drvs

   What you get from evaluating a Job. Might be more children to
   evaluate or leaf Drvs.
 */
class JobEvalResult {
public:
    /* toJson : JobEvalResult -> json */
    virtual json toJson() = 0;
    /* clone : JobEvalResult -> JobEvalResult */
    virtual std::unique_ptr<JobEvalResult> clone() = 0;
    virtual ~JobEvalResult() { };
};

typedef std::vector<std::unique_ptr<JobEvalResult>> JobEvalResults;

/* Job := Drvs | JobAttrs | JobList

   Drvs := vector Drv

   JobAttrs := Attrs Job

   JobList := List Job

   The types of expressions nix-eval-jobs can evaluate

   The implementation (i.e. with JobChildren as children) is a
   different than the grammar because of the way AccessorPath is used
   to walk Jobs.

   There may be multiple Drv because of `recurseForDerivations`.

   Create one with `getJob` or by traversing a Value with
   `AccessorPath::walk`.

   Use it by `eval`ing it.
 */
class Job {
public:
    /* eval : Job -> EvalState -> vector JobEvalResult */
    virtual JobEvalResults eval(EvalState & state) = 0;
    virtual ~Job() { };
};

/* a plain drv - (almost) the primitive for nix-eval-jobs */
struct Drv : JobEvalResult {
    std::string name;
    std::string system;
    std::string drvPath;
    std::map<std::string, std::string> outputs;
    std::optional<json> meta;
    Drv(EvalState & state, DrvInfo & drvInfo);
    Drv(const Drv & that) = default;
    std::unique_ptr<JobEvalResult> clone() override;
    json toJson() override;
    ~Drv() { };
};

/* The leaf on the tree of derivations (there may be multiple due to
   `recurseForDerivations`)
 */
struct Drvs : Job {
    std::vector<Drv> drvs;
    Drvs(EvalState & state, Bindings & autoArgs, Value & v);
    Drvs(const Drvs & that);
    JobEvalResults eval(EvalState & state) override;
    ~Drvs() { };
};

/* which Jobs are collections */
class HasChildren {
public:
    virtual std::vector<std::unique_ptr<Accessor>> children() = 0;
    virtual ~HasChildren() { };
};

/* The forest Jobs when Job is a collection

   Get one by `eval`ing a Job.
 */
struct JobChildren : JobEvalResult {
    std::vector<std::unique_ptr<Accessor>> children;
    JobChildren(HasChildren & children);
    JobChildren(const JobChildren & that);
    std::unique_ptr<JobEvalResult> clone() override;
    json toJson() override;
    ~JobChildren() { };
};

/* An attrset of Job */
struct JobAttrs : Job, HasChildren {
    Value * v;
    JobAttrs(EvalState & state, Bindings & autoArgs, Value & vIn);
    std::vector<std::unique_ptr<Accessor>> children() override;
    JobEvalResults eval(EvalState & state) override;
    ~JobAttrs() { };
};

/* A list of Job */
struct JobList : Job, HasChildren {
    Value * v;
    JobList(EvalState & state, Bindings & autoArgs, Value & vIn);
    std::vector<std::unique_ptr<Accessor>> children() override;
    JobEvalResults eval(EvalState & state) override;
    ~JobList() { };
};

/* Parse a Job from a nix value */
/* Ignore unused function warnings, as it is actually used  in `main` */
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wunused-function"
#elif __clang__
#pragma clang diagnostic ignored "-Wunused-function"
#endif
static std::unique_ptr<Job> getJob(EvalState & state, Bindings & autoArgs, Value & v) {
    try {
        return std::make_unique<Drvs>(state, autoArgs, v);
    } catch (TypeError & _) {
        try {
            return std::make_unique<JobAttrs>(state, autoArgs, v);
        } catch (TypeError & _) {
            try {
                return std::make_unique<JobList>(state, autoArgs, v);
            } catch (TypeError & _) {
                throw TypeError("error creating job, expecting one of a derivation, an attrset or a derivation, got: %s", showType(v));
            }
        }
    }
}
#ifdef __GNUC__
#pragma GCC diagnostic warning "-Wunused-function"
#elif __clang__
#pragma clang diagnostic warning "-Wunused-function"
#endif

}
