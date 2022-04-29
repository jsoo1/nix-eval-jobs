#include <nix/config.h>
#include <nix/eval.hh>
#include <nix/get-drvs.hh>
#include <nix/globals.hh>
#include <nix/local-fs-store.hh>
#include <nix/shared.hh>
#include <nix/value-to-json.hh>

#include <nlohmann/json.hpp>

#include "args.hh"
#include "accessor.hh"
#include "job.hh"

using json = nlohmann::json;
using namespace nix;

namespace nix_eval_jobs {

/* Job */

Drvs::Drvs(EvalState & state, Bindings & autoArgs, Value & v) {
    DrvInfos drvInfos;
    getDerivations(state, v, "", autoArgs, drvInfos, false);

    for (auto & drvInfo : drvInfos)
        this->drvs.push_back(Drv(state, drvInfo));

}

Drvs::Drvs(const Drvs & that) {
    this->drvs = that.drvs;
}

Drv::Drv(EvalState & state, DrvInfo & drvInfo) {
    if (drvInfo.querySystem() == "unknown")
        throw EvalError("derivation must have a 'system' attribute");

    auto localStore = state.store.dynamic_pointer_cast<LocalFSStore>();

    for (auto out : drvInfo.queryOutputs(true)) {
        if (out.second)
            this->outputs[out.first] = localStore->printStorePath(*out.second);

    }

    if (myArgs.meta) {
        json meta_;
        for (auto & name : drvInfo.queryMetaNames()) {
            PathSet context;
            std::stringstream ss;

            auto metaValue = drvInfo.queryMeta(name);
            // Skip non-serialisable types
            // TODO: Fix serialisation of derivations to store paths
            if (metaValue == 0) {
                continue;
            }

            printValueAsJSON(state, true, *metaValue, noPos, ss, context);

            meta_[name] = json::parse(ss.str());
        }
        this->meta = meta_;
    }

    this->name = drvInfo.queryName();
    this->system = drvInfo.querySystem();
    this->drvPath = localStore->printStorePath(drvInfo.requireDrvPath());

}

JobAttrs::JobAttrs(EvalState & state, Bindings & autoArgs, Value & vIn) {
    v = state.allocValue();
    state.autoCallFunction(autoArgs, vIn, *v);

    if (v->type() != nAttrs)
        throw TypeError("wanted a JobAttrs, got %s", showType(vIn));
}

JobList::JobList(EvalState & state, Bindings & autoArgs, Value & vIn) {
    v = state.allocValue();
    state.autoCallFunction(autoArgs, vIn, *v);

    if (v->type() != nList)
        throw TypeError("wanted a JobList, got %s", showType(vIn));
}

/* children : HasChildren -> vector<Accessor> */

std::vector<std::unique_ptr<Accessor>> JobAttrs::children() {
    std::vector<std::unique_ptr<Accessor>> children;

    for (auto & a : this->v->attrs->lexicographicOrder())
        children.push_back(std::make_unique<Name>(a->name));

    return children;
}

std::vector<std::unique_ptr<Accessor>> JobList::children() {
    std::vector<std::unique_ptr<Accessor>> children;
    unsigned long i = 0;

    #ifdef __GNUC__
    #pragma GCC diagnostic ignored "-Wunused-variable"
    #elif __clang__
    #pragma clang diagnostic ignored "-Wunused-variable"
    #endif
    for (auto & _ : v->listItems())
        children.push_back(std::make_unique<Index>(i++));
    #ifdef __GNUC__
    #pragma GCC diagnostic warning "-Wunused-variable"
    #elif __clang__
    #pragma clang diagnostic warning "-Wunused-variable"
    #endif

    return children;
}

/* eval : Job -> EvalState -> JobEvalResult */

JobEvalResults Drvs::eval(EvalState & state) {
    /* Register the derivation as a GC root.  !!! This
       registers roots for jobs that we may have already
       done. */
    if (myArgs.gcRootsDir != "") {
        auto localStore = state.store.dynamic_pointer_cast<LocalFSStore>();

        for (auto & drv : this->drvs) {
            auto storePath = localStore->parseStorePath(drv.drvPath);
            Path root = myArgs.gcRootsDir + "/" + std::string(baseNameOf(drv.drvPath));
            if (!pathExists(root)) {
                localStore->addPermRoot(storePath, root);
            }
        }
    }

    JobEvalResults res;
    for (auto & d : this->drvs)
        res.push_back(d.clone());

    return res;
}

JobEvalResults JobAttrs::eval(EvalState & state) {
    JobEvalResults res;
    res.push_back(JobChildren(*this).clone());
    return res;
}

JobEvalResults JobList::eval(EvalState & state) {
    JobEvalResults res;
    res.push_back(JobChildren(*this).clone());
    return res;
}

/* JobEvalResult */

JobChildren::JobChildren(HasChildren & parent) {
    this->children = parent.children();
}

JobChildren::JobChildren(const JobChildren & that) {
    for (auto & p : that.children)
        this->children.push_back(p->clone());
}

/* clone : JobChildren -> JobChildren */

std::unique_ptr<JobEvalResult> Drv::clone() {
    return std::make_unique<Drv>(*this);
}

std::unique_ptr<JobEvalResult> JobChildren::clone() {
    return std::make_unique<JobChildren>(*this);
}

/* toJson : JobEvalResult -> json */

json JobChildren::toJson() {
    std::vector<json> children;

    for (auto & child : this->children)
        children.push_back(child->toJson());

    return json{ {"children", children } };

}

json Drv::toJson() {
    json j;

    j["name"]  = this->name;
    j["system"] = this->system;
    j["drvPath"] = this->drvPath;
    j["outputs"] = this->outputs;

    if (this->meta.has_value())
        j["meta"] = this->meta.value();

    return j;
}

}
