#include <map>
#include <iostream>
#include <thread>

#include <nix/config.h>
#include <nix/shared.hh>
#include <nix/store-api.hh>
#include <nix/eval.hh>
#include <nix/eval-inline.hh>
#include <nix/util.hh>
#include <nix/get-drvs.hh>
#include <nix/globals.hh>
#include <nix/common-eval-args.hh>
#include <nix/flake/flakeref.hh>
#include <nix/flake/flake.hh>
#include <nix/attr-path.hh>
#include <nix/derivations.hh>
#include <nix/local-fs-store.hh>
#include <nix/logging.hh>
#include <nix/error.hh>

#include <nix/value-to-json.hh>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <nlohmann/json.hpp>

using namespace nix;

/* `nix-eval-jobs` is meant as an alternative to
   `nix-instantiate`. `nix-instantiate` can use a *lot* of memory
   which is unacceptable in settings where multiple instantiations may
   be happening at the same time. As an example, `nix-eval-jobs` is a
   great program for use in continuous integration (CI). It was
   actually originally extracted from the `hydra` nix CI program.

   `nix-eval-jobs` trades throughput of evaluation for memory by
   forking processes and killing them if they go above a specified
   threshold. This way, the operating system is taking the role of
   garbage collector by simply freeing the whole heap when required.
 */

typedef enum { evalAuto, evalImpure, evalPure } pureEval;

// Safe to ignore - the args will be static.
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma clang diagnostic ignored "-Wnon-virtual-dtor"
struct MyArgs : MixEvalArgs, MixCommonArgs
{
    Path releaseExpr;
    Path gcRootsDir;
    bool flake = false;
    bool meta = false;
    bool showTrace = false;
    size_t nrWorkers = 1;
    size_t maxMemorySize = 4096;
    pureEval evalMode = evalAuto;

    MyArgs() : MixCommonArgs("nix-eval-jobs")
    {
        addFlag({
            .longName = "help",
            .description = "show usage information",
            .handler = {[&]() {
                printf("USAGE: nix-eval-jobs [options] expr\n\n");
                for (const auto & [name, flag] : longFlags) {
                    if (hiddenCategories.count(flag->category)) {
                        continue;
                    }
                    printf("  --%-20s %s\n", name.c_str(), flag->description.c_str());
                }
                ::exit(0);
            }},
        });

        addFlag({
            .longName = "impure",
            .description = "set evaluation mode",
            .handler = {[&]() {
                evalMode = evalImpure;
            }},
        });

        addFlag({
            .longName = "gc-roots-dir",
            .description = "garbage collector roots directory",
            .labels = {"path"},
            .handler = {&gcRootsDir}
        });

        addFlag({
            .longName = "workers",
            .description = "number of evaluate workers",
            .labels = {"workers"},
            .handler = {[=](std::string s) {
                nrWorkers = std::stoi(s);
            }}
        });

        addFlag({
            .longName = "max-memory-size",
            .description = "maximum evaluation memory size",
            .labels = {"size"},
            .handler = {[=](std::string s) {
                maxMemorySize = std::stoi(s);
            }}
        });

        addFlag({
            .longName = "flake",
            .description = "build a flake",
            .handler = {&flake, true}
        });

        addFlag({
            .longName = "meta",
            .description = "include derivation meta field in output",
            .handler = {&meta, true}
        });

        addFlag({
            .longName = "show-trace",
            .description = "print out a stack trace in case of evaluation errors",
            .handler = {&showTrace, true}
        });

        expectArg("expr", &releaseExpr);
    }
};
#pragma GCC diagnostic warning "-Wnon-virtual-dtor"
#pragma clang diagnostic warning "-Wnon-virtual-dtor"

static MyArgs myArgs;

static Value* releaseExprTopLevelValue(EvalState & state, Bindings & autoArgs) {
    Value vTop;

    state.evalFile(lookupFileArg(state, myArgs.releaseExpr), vTop);

    auto vRoot = state.allocValue();

    state.autoCallFunction(autoArgs, vTop, *vRoot);

    return vRoot;
}

static Value* flakeTopLevelValue(EvalState & state, Bindings & autoArgs) {
    using namespace flake;

    auto [flakeRef, fragment] = parseFlakeRefWithFragment(myArgs.releaseExpr, absPath("."));

    auto vFlake = state.allocValue();

    auto lockedFlake = lockFlake(state, flakeRef,
        LockFlags {
            .updateLockFile = false,
            .useRegistries = false,
            .allowMutable = false,
        });

    callFlake(state, lockedFlake, *vFlake);

    auto vOutputs = vFlake->attrs->get(state.symbols.create("outputs"))->value;
    state.forceValue(*vOutputs, noPos);
    auto vTop = *vOutputs;

    if (fragment.length() > 0) {
        Bindings & bindings(*state.allocBindings(0));
        auto [nTop, pos] = findAlongAttrPath(state, fragment, bindings, vTop);
        if (!nTop)
            throw Error("error: attribute '%s' missing", nTop);
        vTop = *nTop;
    }

    auto vRoot = state.allocValue();
    state.autoCallFunction(autoArgs, vTop, *vRoot);

    return vRoot;
}

Value * topLevelValue(EvalState & state, Bindings & autoArgs) {
    return myArgs.flake
        ? flakeTopLevelValue(state, autoArgs)
        : releaseExprTopLevelValue(state, autoArgs);
}

/* The fields of a derivation that are printed in json form */
struct Drv {
    std::string name;
    std::string system;
    std::string drvPath;
    std::map<std::string, std::string> outputs;
    std::optional<nlohmann::json> meta;

    Drv (EvalState & state, DrvInfo & drvInfo) {
        if (drvInfo.querySystem() == "unknown")
            throw EvalError("derivation must have a 'system' attribute");

        auto localStore = state.store.dynamic_pointer_cast<LocalFSStore>();

        for (auto out : drvInfo.queryOutputs(true)) {
            if (out.second)
                outputs[out.first] = localStore->printStorePath(*out.second);

        }

        if (myArgs.meta) {
            nlohmann::json meta_;
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

                meta_[name] = nlohmann::json::parse(ss.str());
            }
            meta = meta_;
        }

        name = drvInfo.queryName();
        system = drvInfo.querySystem();
        drvPath = localStore->printStorePath(drvInfo.requireDrvPath());
    }
};

static void to_json(nlohmann::json & json, const Drv & drv) {
    json = nlohmann::json{
        { "name", drv.name },
        { "system", drv.system },
        { "drvPath", drv.drvPath },
        { "outputs", drv.outputs },
    };

    if (drv.meta.has_value())
        json["meta"] = drv.meta.value();

}

/* Ways to look into the top-level value */
class Accessor {
public:
    virtual Value * getIn(EvalState & state, Bindings & autoArgs, Value & v) = 0;

    virtual std::string key() = 0;

    virtual nlohmann::json toJson() = 0;

    virtual ~Accessor() {}
};

/* An index into a list */
struct Index : Accessor {
    unsigned long val;

    Index(const nlohmann::json & json) {
        try {
            val = json;
        } catch (...)  {
            throw TypeError("could not make an index out of json: %s", json.dump());
        }
    }

    Value * getIn(EvalState & state, Bindings & autoArgs, Value & v) {
        unsigned long i = val;

        if (v.type() != nList)
            throw TypeError("tried to get an index in %s", showType(v));

        for (auto el : v.listItems()) {
            if (i == 0) {
                auto x = state.allocValue();
                state.autoCallFunction(autoArgs, *el, *x);
                return x;
            }
            else --i;
        }

        throw EvalError("index %d out of bounds", val);
    }

    std::string key() {
        return "index";
    }

    nlohmann::json toJson() {
        return val;
    }
};

/* An attribute name in an attrset */
struct Name : Accessor {
    std::string val;

    Name(const nlohmann::json & json) {
        try {
            val = json;
            if (val.empty()) throw EvalError("empty attribute name");
        } catch (...) {
            throw TypeError("could not create an attrname out of json: %s", json.dump());
        }
    }

    Value * getIn(EvalState & state, Bindings & autoArgs, Value & v) {
        if (v.type() != nAttrs)
            throw TypeError("tried to get an attrname in %s", showType(v));

        auto pair = v.attrs->find(state.symbols.create(val));

        if (pair) return pair->value;
        else throw EvalError("name not in attrs: '%s'", val);
    }

    std::string key() {
        return "attr";
    }

    nlohmann::json toJson() {
        return val;
    }
};

static std::unique_ptr<Accessor> accessorFromJson(const nlohmann::json & json) {
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

static void accessorCollector(
    EvalState & state,
    Bindings & autoArgs,
    AutoCloseFD & to,
    AutoCloseFD & from)
{
    auto vRoot = topLevelValue(state, autoArgs);

    nlohmann::json reply;

    if (auto drv = getDerivation(state, *vRoot, false)) {
        reply = Drv(state, *drv);

    } else {
        std::vector<std::string> attrs;
        std::vector<unsigned long> indices;
        unsigned long i = 0;

        switch (vRoot->type()) {

        case nAttrs:
            for (auto & a : vRoot->attrs->lexicographicOrder())
                attrs.push_back(a->name);

            reply["attrs"] = attrs;
            break;

        case nList:
            #pragma GCC diagnostic ignored "-Wunused-variable"
            #pragma clang diagnostic ignored "-Wunused-variable"
            for (auto & _ : vRoot->listItems())
                indices.push_back(i++);
            #pragma GCC diagnostic warning "-Wunused-variable"
            #pragma clang diagnostic warning "-Wunused-variable"

            reply["indices"] = indices;
            break;

        default:
            std::stringstream ss;
            ss << "top level value is '" << showType(*vRoot) << "', expected a derivation or a set or list of derivations.";

            reply["error"] = ss.str();
        }
    }

    writeLine(to.get(), reply.dump());
}

static void worker(
    EvalState & state,
    Bindings & autoArgs,
    AutoCloseFD & to,
    AutoCloseFD & from)
{
    auto vRoot = topLevelValue(state, autoArgs);

    while (true) {
        /* Wait for the collector to send us a job name. */
        writeLine(to.get(), "next");

        auto s = readLine(from.get());
        if (s == "exit") break;
        if (!hasPrefix(s, "do ")) abort();
        auto accessorStr = std::string(s, 3);

        debug("worker process %d at '%s'", getpid(), accessorStr);

        std::unique_ptr<Accessor> accessor;
        try {
            accessor = accessorFromJson(nlohmann::json::parse(accessorStr));
        } catch (nlohmann::json::exception & e) {
            throw EvalError("parsing accessor json from %s: %s", accessorStr, e.what());
        }

        /* Evaluate it and send info back to the collector. */
        nlohmann::json reply = nlohmann::json{ { accessor->key(), accessor->toJson() } };
        try {
            auto res = accessor->getIn(state, autoArgs, *vRoot);
            auto v = state.allocValue();
            state.autoCallFunction(autoArgs, *res, *v);

            if (auto drvInfo = getDerivation(state, *v, false)) {

                auto drv = Drv(state, *drvInfo);
                reply.update(drv);

                /* Register the derivation as a GC root.  !!! This
                   registers roots for jobs that we may have already
                   done. */
                if (myArgs.gcRootsDir != "") {
                    Path root = myArgs.gcRootsDir + "/" + std::string(baseNameOf(drv.drvPath));
                    if (!pathExists(root)) {
                        auto localStore = state.store.dynamic_pointer_cast<LocalFSStore>();
                        auto storePath = localStore->parseStorePath(drv.drvPath);
                        localStore->addPermRoot(storePath, root);
                    }
                }

            }

            else throw TypeError("element at '%s' is not a derivation", accessor->toJson().dump());

        } catch (EvalError & e) {
            auto err = e.info();

            std::ostringstream oss;
            showErrorInfo(oss, err, loggerSettings.showTrace.get());
            auto msg = oss.str();

            // Transmits the error we got from the previous evaluation
            // in the JSON output.
            reply["error"] = filterANSIEscapes(msg, true);
            // Don't forget to print it into the STDERR log, this is
            // what's shown in the Hydra UI.
            printError(e.msg());
        }

        writeLine(to.get(), reply.dump());

        /* If our RSS exceeds the maximum, exit. The collector will
           start a new process. */
        struct rusage r;
        getrusage(RUSAGE_SELF, &r);
        if ((size_t) r.ru_maxrss > myArgs.maxMemorySize * 1024) break;
    }

    writeLine(to.get(), "restart");
}

typedef std::function<void(EvalState & state, Bindings & autoArgs, AutoCloseFD & to, AutoCloseFD & from)>
    Processor;

/* Auto-cleanup of fork's process and fds. */
struct Proc {
    AutoCloseFD to, from;
    Pid pid;

    Proc(const Processor & proc) {
        Pipe toPipe, fromPipe;
        toPipe.create();
        fromPipe.create();
        auto p = startProcess(
            [&,
             to{std::make_shared<AutoCloseFD>(std::move(fromPipe.writeSide))},
             from{std::make_shared<AutoCloseFD>(std::move(toPipe.readSide))}
             ]()
            {
                debug("created worker process %d", getpid());
                try {
                    EvalState state(myArgs.searchPath, openStore());
                    Bindings & autoArgs = *myArgs.getAutoArgs(state);
                    proc(state, autoArgs, *to, *from);
                } catch (Error & e) {
                    nlohmann::json err;
                    auto msg = e.msg();
                    err["error"] = filterANSIEscapes(msg, true);
                    printError(msg);
                    writeLine(to->get(), err.dump());
                    // Don't forget to print it into the STDERR log, this is
                    // what's shown in the Hydra UI.
                    writeLine(to->get(), "restart");
                }
            },
            ProcessOptions { .allowVfork = false });

        to = std::move(toPipe.writeSide);
        from = std::move(fromPipe.readSide);
        pid = p;
    }

    ~Proc() { }
};

struct State
{
    std::set<nlohmann::json> todo{};
    std::set<nlohmann::json> active;
    std::exception_ptr exc;
};

std::function<void()> collector(Sync<State> & state_, std::condition_variable & wakeup) {
    return [&]() {
        try {
            std::optional<std::unique_ptr<Proc>> proc_;

            while (true) {

                auto proc = proc_.has_value()
                    ? std::move(proc_.value())
                    : std::make_unique<Proc>(worker);

                /* Check whether the existing worker process is still there. */
                auto s = readLine(proc->from.get());
                if (s == "restart") {
                    proc_ = std::nullopt;
                    continue;
                } else if (s != "next") {
                    auto json = nlohmann::json::parse(s);
                    throw Error("worker error: %s", (std::string) json["error"]);
                }

                /* Wait for a job name to become available. */
                nlohmann::json accessor;

                while (true) {
                    checkInterrupt();
                    auto state(state_.lock());
                    if ((state->todo.empty() && state->active.empty()) || state->exc) {
                        writeLine(proc->to.get(), "exit");
                        return;
                    }
                    if (!state->todo.empty()) {
                        accessor = *state->todo.begin();
                        state->todo.erase(state->todo.begin());
                        state->active.insert(accessor);
                        break;
                    } else
                        state.wait(wakeup);
                }

                /* Tell the worker to evaluate it. */
                writeLine(proc->to.get(), "do " + accessor.dump());

                /* Wait for the response. */
                auto respString = readLine(proc->from.get());
                auto response = nlohmann::json::parse(respString);

                proc_ = std::move(proc);

                /* Print the response. */
                {
                    auto state(state_.lock());
                    std::cout << response << "\n" << std::flush;
                    state->active.erase(accessor);
                    wakeup.notify_all();
                }
            }
        } catch (...) {
            auto state(state_.lock());
            state->exc = std::current_exception();
            wakeup.notify_all();
        }
    };
}

void initState(Sync<State> & state_) {
    /* Collect initial attributes to evaluate. This must be done in a
       separate fork to avoid spawning a download in the parent
       process. If that happens, worker processes will try to enqueue
       downloads on their own download threads (which will not
       exist). Then the worker processes will hang forever waiting for
       downloads.
    */
    auto proc = Proc(accessorCollector);

    auto s = readLine(proc.from.get());
    auto json = nlohmann::json::parse(s);

    if (json.find("error") != json.end()) {
        throw Error("getting initial attributes: %s", (std::string) json["error"]);

    } else if (json.find("attrs") != json.end()) {
        auto state(state_.lock());
        for (auto a : json["attrs"])
            state->todo.insert(a);

    } else if (json.find("indices") != json.end()){
        auto state(state_.lock());
        for (auto i : json["indices"])
            state->todo.insert(i);

    } else if (json.find("drvPath") != json.end()) {
        std::cout << json.dump() << "\n" << std::flush;

    } else {
        throw Error("expected object with \"error\", \"indices\", \"attrs\", or a derivation, got: %s", s);

    }
}

int main(int argc, char * * argv)
{
    /* Prevent undeclared dependencies in the evaluation via
       $NIX_PATH. */
    unsetenv("NIX_PATH");

    /* We are doing the garbage collection by killing forks */
    setenv("GC_DONT_GC", "1", 1);

    return handleExceptions(argv[0], [&]() {
        initNix();
        initGC();

        myArgs.parseCmdline(argvToStrings(argc, argv));

        /* FIXME: The build hook in conjunction with import-from-derivation is causing "unexpected EOF" during eval */
        settings.builders = "";

        /* Prevent access to paths outside of the Nix search path and
           to the environment. */
        evalSettings.restrictEval = false;

        /* When building a flake, use pure evaluation (no access to
           'getEnv', 'currentSystem' etc. */
        evalSettings.pureEval = myArgs.evalMode == evalAuto ? myArgs.flake : myArgs.evalMode == evalPure;

        if (myArgs.releaseExpr == "") throw UsageError("no expression specified");

        if (myArgs.gcRootsDir == "") printMsg(lvlError, "warning: `--gc-roots-dir' not specified");

        if (myArgs.showTrace) {
            loggerSettings.showTrace.assign(true);
        }

        Sync<State> state_;
        initState(state_);

        /* Start a collector thread per worker process. */
        std::vector<std::thread> threads;
        std::condition_variable wakeup;
        for (size_t i = 0; i < myArgs.nrWorkers; i++)
            threads.emplace_back(std::thread(collector(state_, wakeup)));

        for (auto & thread : threads)
            thread.join();

        auto state(state_.lock());

        if (state->exc)
            std::rethrow_exception(state->exc);

    });
}
