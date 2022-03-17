#include <map>
#include <iostream>
#include <thread>

#include <nix/config.h>
#include <nix/args.hh>
#include <nix/shared.hh>
#include <nix/store-api.hh>
#include <nix/eval.hh>
#include <nix/eval-inline.hh>
#include <nix/util.hh>
#include <nix/get-drvs.hh>
#include <nix/globals.hh>
#include <nix/common-eval-args.hh>
#include <nix/attr-path.hh>
#include <nix/derivations.hh>
#include <nix/local-store.hh>
#include <nix/logging.hh>

#include <nix/value-to-json.hh>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <nlohmann/json.hpp>

using namespace nix;

typedef enum { evalAuto, evalImpure, evalPure } pureEval;

struct MyArgs : MixEvalArgs, MixCommonArgs
{
    Path releaseExpr;
    bool flake = false;
    bool meta = false;
    bool showTrace = false;
    size_t nrWorkers = 1;
    size_t maxMemorySize = 4096;
    pureEval evalMode = evalAuto;

    MyArgs() : MixCommonArgs("nix-eval-jobs")
    {
        mkFlag()
            .longName("help")
            .description("show usage information")
            .handler({[&]() {
                printf("USAGE: nix-eval-jobs [options] expr\n\n");
                for (const auto & [name, flag] : longFlags) {
                    if (hiddenCategories.count(flag->category)) {
                        continue;
                    }
                    printf("  --%-20s %s\n", name.c_str(), flag->description.c_str());
                }
                ::exit(0);
            }});

        mkFlag()
            .longName("impure")
            .description("set evaluation mode")
            .handler({[&]() {
                evalMode = evalImpure;
            }});

        mkFlag()
            .longName("workers")
            .description("number of evaluate workers")
            .labels({"workers"})
            .handler({[=](std::string s) {
                nrWorkers = std::stoi(s);
            }});

        mkFlag()
            .longName("max-memory-size")
            .description("maximum evaluation memory size")
            .labels({"size"})
            .handler({[=](std::string s) {
                maxMemorySize = std::stoi(s);
            }});

        mkFlag()
            .longName("meta")
            .description("include derivation meta field in output")
            .handler({[=](){
                meta = true;
            }});

        mkFlag()
            .longName("show-trace")
            .description("print out a stack trace in case of evaluation errors")
            .handler({[=]() {
                showTrace = true;
            }});

        expectArg("expr", &releaseExpr);
    }
};

static MyArgs myArgs;

static Value* releaseExprTopLevelValue(EvalState & state, Bindings & autoArgs) {
    Value vTop;

    state.evalFile(lookupFileArg(state, myArgs.releaseExpr), vTop);

    auto vRoot = state.allocValue();

    state.autoCallFunction(autoArgs, vTop, *vRoot);

    return vRoot;
}

static void worker(
    EvalState & state,
    Bindings & autoArgs,
    AutoCloseFD & to,
    AutoCloseFD & from)
{
    auto vRoot = releaseExprTopLevelValue(state, autoArgs);

    while (true) {
        /* Wait for the master to send us a job name. */
        writeLine(to.get(), "next");

        auto s = readLine(from.get());
        if (s == "exit") break;
        if (!hasPrefix(s, "do ")) abort();
        std::string attrName(s, 3);

        debug("worker process %d at '%s'", getpid(), attrName);

        /* Evaluate it and send info back to the master. */
        nlohmann::json reply;
        reply["attr"] = attrName;

        try {
            auto v = state.allocValue();

            state.autoCallFunction(autoArgs, *vRoot, *v);
            state.forceValue(*v);

            if (v->type != tAttrs)
                throw TypeError("root is of type '%s', expected a set", showType(*v));

            if (attrName.empty()) throw Error("empty attribute name");

            auto a = v->attrs->find(state.symbols.create(attrName));

            if (a == v->attrs->end()) throw Error("attribute '%s' not found", attrName);

            auto attrVal = state.allocValue();

            state.autoCallFunction(autoArgs, *a->value, *attrVal);
            state.forceValue(*attrVal);

            if (auto drv = getDerivation(state, *attrVal, false)) {

                // Workaround for nixos "systems"
                //
                //  ... which have "system" attributes that are
                // themselves derivations.
                std::string system;

                auto systemAttr = attrVal->attrs->find(state.symbols.create("system"));

                if (systemAttr == a->value->attrs->end()) {
                    throw EvalError("derivation must have a 'system' attribute");

                } else if (auto systemDrv = getDerivation(state, *systemAttr->value, false)) {
                    auto systemV = state.allocValue();

                    state.autoCallFunction(autoArgs, *systemAttr->value, *systemV);
                    state.forceValue(*systemV);

                    system = systemDrv->querySystem();

                } else {
                    system = drv->querySystem();

                }

                if (system == "unknown")
                    throw EvalError("derivation must not have unknown system type");

                auto drvPath = drv->queryDrvPath();
                auto outputs = drv->queryOutputs(false);

                reply["name"] = drv->queryName();
                reply["system"] = system;
                reply["drvPath"] = drvPath;
                for (auto out : outputs){
                    reply["outputs"][out.first] = out.second;
                }

                if (myArgs.meta) {
                    nlohmann::json meta;
                    for (auto & name : drv->queryMetaNames()) {
                      PathSet context;
                      std::stringstream ss;

                      auto metaValue = drv->queryMeta(name);
                      // Skip non-serialisable types
                      // TODO: Fix serialisation of derivations to store paths
                      if (metaValue == 0) {
                        continue;
                      }

                      printValueAsJSON(state, true, *metaValue, ss, context);
                      nlohmann::json field = nlohmann::json::parse(ss.str());
                      meta[name] = field;
                    }
                    reply["meta"] = meta;
                }

            }

            else if (attrVal->type == tAttrs)
              {
                auto attrs = nlohmann::json::array();
                StringSet ss;
                for (auto & i : attrVal->attrs->lexicographicOrder()) {
                    attrs.push_back(i->name);
                }
                reply["attrs"] = std::move(attrs);
            }

            else if (attrVal->type == tNull)
                ;

            else throw TypeError("attribute '%s' is %s, which is not supported", attrName, showType(*attrVal));

        } catch (EvalError & e) {
            // Transmits the error we got from the previous evaluation
            // in the JSON output.
            reply["error"] = filterANSIEscapes(e.msg(), true);
            // Don't forget to print it into the STDERR log, this is
            // what's shown in the Hydra UI.
            printError(e.msg());
        }

        writeLine(to.get(), reply.dump());

        /* If our RSS exceeds the maximum, exit. The master will
           start a new process. */
        struct rusage r;
        getrusage(RUSAGE_SELF, &r);
        if ((size_t) r.ru_maxrss > myArgs.maxMemorySize * 1024) break;
    }

    writeLine(to.get(), "restart");
}

int main(int argc, char * * argv)
{
    /* Prevent undeclared dependencies in the evaluation via
       $NIX_PATH. */
    unsetenv("NIX_PATH");

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

        if (myArgs.showTrace) {
            settings.showTrace.assign(true);
        }

        struct State
        {
            std::set<std::string> todo{};
            std::set<std::string> active;
            std::exception_ptr exc;
        };

        std::condition_variable wakeup;

        Sync<State> state_;

        /* Start a handler thread per worker process. */
        auto handler = [&]()
        {
            try {
                pid_t pid = -1;
                AutoCloseFD from, to;

                while (true) {

                    /* Start a new worker process if necessary. */
                    if (pid == -1) {
                        Pipe toPipe, fromPipe;
                        toPipe.create();
                        fromPipe.create();
                        pid = startProcess(
                            [&,
                             to{std::make_shared<AutoCloseFD>(std::move(fromPipe.writeSide))},
                             from{std::make_shared<AutoCloseFD>(std::move(toPipe.readSide))}
                            ]()
                            {
                                auto tmpdir = createTempDir("", "nix-eval-jobs", true, true, S_IRWXU);
                                setenv("XDG_CACHE_HOME", tmpdir.c_str(), 1);

                                try {
                                    EvalState state(myArgs.searchPath, openStore());
                                    Bindings & autoArgs = *myArgs.getAutoArgs(state);
                                    worker(state, autoArgs, *to, *from);
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
                        from = std::move(fromPipe.readSide);
                        to = std::move(toPipe.writeSide);
                        debug("created worker process %d", pid);
                    }

                    /* Check whether the existing worker process is still there. */
                    auto s = readLine(from.get());
                    if (s == "restart") {
                        pid = -1;
                        continue;
                    } else if (s != "next") {
                        auto json = nlohmann::json::parse(s);
                        throw Error("worker error: %s", (std::string) json["error"]);
                    }

                    /* Wait for a job name to become available. */
                    std::string attrPath;

                    while (true) {
                        checkInterrupt();
                        auto state(state_.lock());
                        if ((state->todo.empty() && state->active.empty()) || state->exc) {
                            writeLine(to.get(), "exit");
                            return;
                        }
                        if (!state->todo.empty()) {
                            attrPath = *state->todo.begin();
                            state->todo.erase(state->todo.begin());
                            state->active.insert(attrPath);
                            break;
                        } else
                            state.wait(wakeup);
                    }

                    /* Tell the worker to evaluate it. */
                    writeLine(to.get(), "do " + attrPath);

                    /* Wait for the response. */
                    auto respString = readLine(from.get());
                    auto response = nlohmann::json::parse(respString);

                    /* Handle the response. */
                    StringSet newAttrs;
                    if (response.find("attrs") != response.end()) {
                        for (auto & i : response["attrs"]) {
                            auto s = (attrPath.empty() ? "" : attrPath + ".") + (std::string) i;
                            newAttrs.insert(s);
                        }
                    } else {
                        auto state(state_.lock());
                        std::cout << respString << "\n" << std::flush;
                    }

                    /* Add newly discovered job names to the queue. */
                    {
                        auto state(state_.lock());
                        state->active.erase(attrPath);
                        for (auto & s : newAttrs)
                            state->todo.insert(s);
                        wakeup.notify_all();
                    }
                }
            } catch (...) {
                auto state(state_.lock());
                state->exc = std::current_exception();
                wakeup.notify_all();
            }
        };

        EvalState initialState(myArgs.searchPath, openStore());
        Bindings & autoArgs = *myArgs.getAutoArgs(initialState);

        auto topLevelValue = releaseExprTopLevelValue(initialState, autoArgs);

        if (topLevelValue->type == tAttrs) {
          auto state(state_.lock());
          for (auto & a : topLevelValue->attrs->lexicographicOrder()) {
            state->todo.insert(a->name);
          }
        }

        std::vector<std::thread> threads;
        for (size_t i = 0; i < myArgs.nrWorkers; i++)
            threads.emplace_back(std::thread(handler));

        for (auto & thread : threads)
            thread.join();

        auto state(state_.lock());

        if (state->exc)
            std::rethrow_exception(state->exc);

    });
}
