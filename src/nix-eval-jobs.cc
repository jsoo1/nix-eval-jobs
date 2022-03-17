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

static void showPath_(std::vector<std::string> & path, std::ostringstream & dest) {
    int size = path.size();

    for (auto i = 0; i < size; i++) {
        dest << "\"" << path[i] << "\"";

        if (i != size - 1) {
          dest << ".";
        }
    }
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
    AutoCloseFD & from)
{
    auto vRoot = releaseExprTopLevelValue(state, autoArgs);

    while (true) {
        /* Wait for the master to send us a job name. */
        writeLine(to.get(), "next");

        auto s = readLine(from.get());
        if (s == "exit") break;
        if (!hasPrefix(s, "do ")) abort();
        std::string msg(s, 3);

        debug("worker process %d at '%s'", getpid(), msg);

        nlohmann::json::array_t msgJson = nlohmann::json::parse(msg);
        std::vector<std::string> attrPath = {};

        for (auto json : msgJson) {
            if (!json.is_string()) {
                throw TypeError("Unexpected message from coordinator: %s", msg);

            } else {
                attrPath.push_back(json);

            }
        }

        /* Evaluate it and send info back to the master. */

        std::ostringstream attrNameS;
        showPath_(attrPath, attrNameS);
        auto attrName = attrNameS.str();

        try {
            auto v = state.allocValue();

            state.autoCallFunction(autoArgs, *vRoot, *v);
            state.forceValue(*v);

            if (v->type != tAttrs)
                throw TypeError("root is of type '%s', expected a set", showType(*v));

            if (attrPath.empty()) throw Error("empty attribute path");

            auto attrVal = state.allocValue();

            attrVal = v;

            for (auto attrName : attrPath) {
                auto a = attrVal->attrs->find(state.symbols.create(attrName));

                if (a != attrVal->attrs->end()) {
                    std::ostringstream oss;
                    showPath_(attrPath, oss);
                    throw Error("attribute '%s' not found along path %s", attrName, oss.str());
                }

                auto attrValTemp = state.allocValue();

                state.autoCallFunction(autoArgs, *a->value, *attrValTemp);
                state.forceValue(*attrValTemp);

                attrVal = attrValTemp;

            }


            DrvInfos drvs;
            getDerivations(state, *attrVal, "", autoArgs, drvs, false);

            if (!drvs.empty()) {
                for (auto drv : drvs) {
                    std::string system = drv.querySystem();

                    if (system == "unknown")
                        throw EvalError("derivation must not have unknown system type");

                    auto drvPath = drv.queryDrvPath();
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

                          printValueAsJSON(state, true, *metaValue, ss, context);
                          nlohmann::json field = nlohmann::json::parse(ss.str());
                          meta[name] = field;
                        }
                        reply["meta"] = meta;
                    }

                    writeLine(to.get(), reply.dump());

                }
            }

            else if (attrVal->type == tAttrs)
              {
                auto paths = nlohmann::json::array();
                for (auto & i : attrVal->attrs->lexicographicOrder()) {
                    auto attrs = nlohmann::json::array();
                    for (auto & a : attrPath) {
                      attrs.push_back(a);
                    }

                    attrs.push_back(i->name);

                    paths.push_back(attrs);
                }
                auto reply = response(attrName);
                reply["attrs"] = std::move(paths);

                writeLine(to.get(), reply.dump());
            }

            else if (attrVal->type == tNull)
                ;

            else {
                std::ostringstream oss;
                showPath_(attrPath, oss);
                throw TypeError("attribute %s is of type '%s', which is not supported", oss.str(), showType(*attrVal));
            }

        } catch (EvalError & e) {
            auto reply = response(attrName);

            // Transmits the error we got from the previous evaluation
            // in the JSON output.
            reply["error"] = filterANSIEscapes(e.msg(), true);
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

        if (myArgs.showTrace) {
            settings.showTrace.assign(true);
        }

        struct State
        {
            std::set<nlohmann::json::array_t> todo{};
            std::set<nlohmann::json::array_t> active;
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

                        if (json.find("error") != json.end())
                            throw Error("worker error: %s", (std::string) json["error"]);
                    }

                    /* Wait for a job name to become available. */
                    nlohmann::json::array_t attrPath = nlohmann::json::array();

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
                    auto msg = nlohmann::json::array();

                    for (auto p : attrPath) msg.push_back(p);

                    writeLine(to.get(), "do " + msg.dump());

                    /* Wait for the response. */
                    auto respString = readLine(from.get());
                    auto response = nlohmann::json::parse(respString);

                    /* Handle the response. */
                    nlohmann::json::array_t newAttrs = nlohmann::json::array();
                    if (response.find("attrs") != response.end()) {
                        for (auto & i : response["attrs"]) {
                          if (i.is_array()) {
                            newAttrs.push_back(i);
                          } else {
                            throw TypeError("Got an unexpected message from a worker: %s", response.dump());
                          }
                        }
                    } else {
                        auto state(state_.lock());
                        std::cout << respString << "\n" << std::flush;
                    }

                    /* Add newly discovered job names to the queue. */
                    {
                        auto state(state_.lock());
                        state->active.erase(attrPath);
                        for (nlohmann::json::array_t s : newAttrs)
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
            std::ostringstream oss;
            oss << a->name;
            nlohmann::json::array_t path = nlohmann::json::array({ oss.str() });
            state->todo.insert(path);
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
