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

std::unique_ptr<Job> getJob(EvalState & state, Bindings & autoArgs, Value & v) {
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

Drvs::Drvs(EvalState & state, Bindings & autoArgs, Value & v) {
    DrvInfos drvInfos;
    getDerivations(state, v, "", autoArgs, drvInfos, false);

    for (auto & drvInfo : drvInfos)
        this->drvs.push_back(std::make_shared<Drv>(state, drvInfo));

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

/* JobEvalResult := Drv | JobChildren  */

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

Drv::Drv(const json & j) {
    this->name = j["name"] ;
    this->system = j["system"];
    this->drvPath = j["drvPath"];

    std::map<std::string, std::string> outs = j["outputs"];
    for (auto & output : outs)
        this->outputs.insert(output);

    if (j.contains("meta"))
        this->meta = j["meta"];
}

JobChildren::JobChildren(const json & j) {
    try {
        std::vector<json> vec = j;
        for (auto i : vec)
            this->children->push_back(accessorFromJson(i));

    } catch (json::exception & e) {
        throw TypeError("could not make job children out of json, expected a list of accessors: %s", j.dump());
    }
}

/* children : HasChildren -> vector<Accessor> */

JobChildren JobAttrs::children() {
    std::shared_ptr<std::vector<std::unique_ptr<Accessor>>> children;

    for (auto & a : this->v->attrs->lexicographicOrder())
        children->push_back(std::make_unique<Name>(a->name));

    return JobChildren(children);
}

JobChildren JobList::children() {
    std::shared_ptr<std::vector<std::unique_ptr<Accessor>>> children;
    unsigned long i = 0;

    #ifdef __GNUC__
    #pragma GCC diagnostic ignored "-Wunused-variable"
    #elif __clang__
    #pragma clang diagnostic ignored "-Wunused-variable"
    #endif
    for (auto & _ : v->listItems())
        children->push_back(std::make_unique<Index>(i++));
    #ifdef __GNUC__
    #pragma GCC diagnostic warning "-Wunused-variable"
    #elif __clang__
    #pragma clang diagnostic warning "-Wunused-variable"
    #endif

    return JobChildren(children);
}

/* eval : Job -> EvalState -> JobEvalResult */

JobEvalResults Drvs::eval(EvalState & state) {
    /* Register the derivation as a GC root.  !!! This
       registers roots for jobs that we may have already
       done. */
    if (myArgs.gcRootsDir != "") {
        auto localStore = state.store.dynamic_pointer_cast<LocalFSStore>();

        for (auto & drv : this->drvs) {
            auto storePath = localStore->parseStorePath(drv->drvPath);
            Path root = myArgs.gcRootsDir + "/" + std::string(baseNameOf(drv->drvPath));
            if (!pathExists(root)) {
                localStore->addPermRoot(storePath, root);
            }
        }
    }

    JobEvalResults res;
    for (auto & d : this->drvs)
        res.push_back(d);

    return res;
}

/* toJson : JobEvalResult -> json */

json JobChildren::toJson() {
    std::vector<json> children;

    for (auto & child : *this->children)
        children.push_back(child->toJson());

    return children;
}

void to_json(json & j, const Drv & drv) {
    j["name"]  = drv.name;
    j["system"] = drv.system;
    j["drvPath"] = drv.drvPath;
    j["outputs"] = drv.outputs;

    if (drv.meta.has_value())
        j["meta"] = drv.meta.value();
}

json Drv::toJson() {
    json j;
    to_json(j, *this);
    return j;
}

}
