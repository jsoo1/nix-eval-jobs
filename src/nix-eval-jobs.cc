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
#include <nix/flake/flakeref.hh>
#include <nix/flake/flake.hh>
#include <nix/attr-path.hh>
#include <nix/derivations.hh>
#include <nix/local-store.hh>
#include <nix/logging.hh>
#include <nix/error.hh>

#include <nix/value-to-json.hh>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <nlohmann/json.hpp>

using namespace nix;

typedef enum { evalAuto, evalImpure, evalPure } pureEval;

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

static MyArgs myArgs;

static Value* releaseExprTopLevelValue(EvalState & state, Bindings & autoArgs) {
    Value vTop;

    state.evalFile(lookupFileArg(state, myArgs.releaseExpr), vTop);

    auto vRoot = state.allocValue();

    state.autoCallFunction(autoArgs, vTop, *vRoot);

    return vRoot;
}

static nlohmann::json response(std::string & attrName) {
    nlohmann::json reply;
    reply["attr"] = attrName;
    return reply;
}

static void worker(
    EvalState & state,
    Bindings & autoArgs,
    AutoCloseFD & to,
    AutoCloseFD & from,
    const Path &gcRootsDir)
{
    Value vTop;
    Value * vRoot;

    if (myArgs.flake) {
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
        vTop = *vOutputs;

        if (fragment.length() > 0) {
            Bindings & bindings(*state.allocBindings(0));
            auto [nTop, pos] = findAlongAttrPath(state, fragment, bindings, vTop);
            if (!nTop)
                throw Error("error: attribute '%s' missing", nTop);
            vTop = *nTop;
        }

        vRoot = state.allocValue();
        state.autoCallFunction(autoArgs, vTop, *vRoot);

    } else {
        vRoot = releaseExprTopLevelValue(state, autoArgs);
    }


    while (true) {
        /* Wait for the master to send us a job name. */
        writeLine(to.get(), "next");

        auto s = readLine(from.get());
        if (s == "exit") break;
        if (!hasPrefix(s, "do ")) abort();
        std::string attrName(s, 3);

        debug("worker process %d at '%s'", getpid(), attrName);

        /* Evaluate it and send info back to the master. */
        try {
            auto v = state.allocValue();

            state.autoCallFunction(autoArgs, *vRoot, *v);
            state.forceValue(*v);

            if (v->type() != nAttrs)
                throw TypeError("root is of type '%s', expected a set", showType(*v));

            if (attrName.empty()) throw Error("empty attribute name");

            auto a = v->attrs->get(state.symbols.create(attrName));

            auto attrVal = state.allocValue();

            state.autoCallFunction(autoArgs, *a->value, *attrVal);
            state.forceValue(*attrVal);

            //  Hacky workaround for nixos systems whose "system" attribute is a drv
            std::optional<std::string> nixosSystemTuple = {};

            auto systemAttr = attrVal->attrs->find(state.symbols.create("system"));

            if (auto nixosDrv = getDerivation(state, *systemAttr->value, false)) {
                nixosSystemTuple = nixosDrv->querySystem();
            }

            DrvInfos drvs;
            getDerivations(state, *attrVal, "", autoArgs, drvs, false);

            if (!drvs.empty()) {
                for (auto drv : drvs) {
                    std::string system;

                    if (auto sys = nixosSystemTuple) {
                        system = *sys;
                    } else {
                        system = drv.querySystem();
                    }

                    if (system == "unknown")
                        throw EvalError("derivation must not have unknown system type");

                    auto drvPath = drv.queryDrvPath();
                    auto localStore = state.store.dynamic_pointer_cast<LocalFSStore>();
                    auto storePath = localStore->parseStorePath(drvPath);
                    auto outputs = drv.queryOutputs(false);

                    auto reply = response(attrName);

                    reply["name"] = drv.queryName();
                    reply["system"] = system;
                    reply["drvPath"] = drvPath;
                    for (auto out : outputs){
                        reply["outputs"][out.first] = out.second;
                    }

                    if (myArgs.meta) {
                        nlohmann::json meta;
                        for (auto & name : drv.queryMetaNames()) {
                          PathSet context;
                          std::stringstream ss;

                          auto metaValue = drv.queryMeta(name);
                          // Skip non-serialisable types
                          // TODO: Fix serialisation of derivations to store paths
                          if (metaValue == 0) {
                            continue;
                          }

                          printValueAsJSON(state, true, *metaValue, noPos, ss, context);
                          nlohmann::json field = nlohmann::json::parse(ss.str());
                          meta[name] = field;
                        }
                        reply["meta"] = meta;
                    }

                    writeLine(to.get(), reply.dump());

                    /* Register the derivation as a GC root.  !!! This
                       registers roots for jobs that we may have already
                       done. */
                    if (gcRootsDir != "") {
                        Path root = gcRootsDir + "/" + std::string(baseNameOf(drvPath));
                        if (!pathExists(root))
                            localStore->addPermRoot(storePath, root);
                    }
                }
            }

            else if (attrVal->type() == nNull)
                ;

            else {
                throw TypeError("attribute '%s' is of type '%s', which is not supported", attrName, showType(*attrVal));
            }

        } catch (EvalError & e) {
            auto err = e.info();

            std::ostringstream oss;
            showErrorInfo(oss, err, loggerSettings.showTrace.get());
            auto msg = oss.str();

            auto reply = response(attrName);

            // Transmits the error we got from the previous evaluation
            // in the JSON output.
            reply["error"] = filterANSIEscapes(msg, true);
            // Don't forget to print it into the STDERR log, this is
            // what's shown in the Hydra UI.
            printError(e.msg());

            writeLine(to.get(), reply.dump());
        }

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

        if (myArgs.gcRootsDir == "") printMsg(lvlError, "warning: `--gc-roots-dir' not specified");

        if (myArgs.showTrace) {
            loggerSettings.showTrace.assign(true);
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
                std::optional<Pid> pid;
                AutoCloseFD from, to;

                while (true) {

                    /* Start a new worker process if necessary. */
                    if (!pid.has_value()) {
                        Pipe toPipe, fromPipe;
                        toPipe.create();
                        fromPipe.create();
                        pid = startProcess(
                            [&,
                             to{std::make_shared<AutoCloseFD>(std::move(fromPipe.writeSide))},
                             from{std::make_shared<AutoCloseFD>(std::move(toPipe.readSide))}
                            ]()
                            {
                                debug("created worker process %d", getpid());
                                try {
                                    EvalState state(myArgs.searchPath, openStore());
                                    Bindings & autoArgs = *myArgs.getAutoArgs(state);
                                    worker(state, autoArgs, *to, *from, myArgs.gcRootsDir);
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
                    }

                    /* Check whether the existing worker process is still there. */
                    auto s = readLine(from.get());
                    if (s == "restart") {
                        pid = std::nullopt;
                        continue;
                    } else if (s != "next") {
                        auto json = nlohmann::json::parse(s);

                        if (json.find("error") != json.end())
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

                    /* Handle the response. */
                    auto respString = readLine(from.get());
                    auto response = nlohmann::json::parse(respString);
                    auto state(state_.lock());
                    std::cout << response << "\n" << std::flush;

                    state->active.erase(attrPath);
                    wakeup.notify_all();
                }
            } catch (...) {
                auto state(state_.lock());
                state->exc = std::current_exception();
                wakeup.notify_all();
            }
        };

        /* Collect initial attributes to evaluate */
        {
            AutoCloseFD from, to;
            Pipe toPipe, fromPipe;
            toPipe.create();
            fromPipe.create();
            Pid p = startProcess(
                [&,
                 to{std::make_shared<AutoCloseFD>(std::move(fromPipe.writeSide))},
                 from{std::make_shared<AutoCloseFD>(std::move(toPipe.readSide))}
                ]()
                {
                    debug("created initial attribute collection process %d", getpid());

                    try {
                        EvalState state(myArgs.searchPath, openStore());
                        Bindings & autoArgs = *myArgs.getAutoArgs(state);

                        auto vRoot = releaseExprTopLevelValue(state, autoArgs);

                        if (vRoot->type() != nAttrs)
                            throw TypeError("top level value is a '%s', expected an attribute set", showType(*vRoot));

                        nlohmann::json reply;

                        try {
                            std::vector<std::string> attrs;
                            for (auto & a : vRoot->attrs->lexicographicOrder())
                                attrs.push_back(a->name);


                            reply["attrs"] = attrs;

                            writeLine(to->get(), reply.dump());

                        } catch (EvalError & e) {
                            reply["error"] = filterANSIEscapes(e.msg(), true);
                            printError(e.msg());

                        }
                        writeLine(to->get(), reply.dump());

                    } catch (Error & e) {
                        nlohmann::json err;
                        auto msg = e.msg();
                        err["error"] = filterANSIEscapes(msg, true);
                        printError(msg);
                        writeLine(to->get(), err.dump());
                    }
                },
                ProcessOptions { .allowVfork = false });
            from = std::move(fromPipe.readSide);
            to = std::move(toPipe.writeSide);

            auto s = readLine(from.get());
            auto json = nlohmann::json::parse(s);

            if (json.find("error") != json.end()) {
                throw Error("getting initial attributes: %s", (std::string) json["error"]);

            } else if (json.find("attrs") != json.end()) {
                auto state(state_.lock());
                for (std::string a : json["attrs"])
                    state->todo.insert(a);

            } else {
                throw Error("expected object with \"error\" or \"attrs\", got: %s", s);

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
