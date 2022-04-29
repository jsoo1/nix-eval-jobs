#include <map>
#include <iostream>
#include <thread>

#include <nix/config.h>
#include <nix/shared.hh>
#include <nix/flake/flake.hh>
#include <nix/attr-path.hh>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <nlohmann/json.hpp>

#include "args.hh"
#include "proc.hh"
#include "job.hh"

using namespace nix;
using namespace nlohmann;

using namespace nix_eval_jobs;

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

static Value *releaseExprTopLevelValue(EvalState &state, Bindings &autoArgs) {
    Value vTop;

    state.evalFile(lookupFileArg(state, myArgs.releaseExpr), vTop);

    auto vRoot = state.allocValue();

    state.autoCallFunction(autoArgs, vTop, *vRoot);

    return vRoot;
}

static Value *flakeTopLevelValue(EvalState &state, Bindings &autoArgs) {
    using namespace flake;

    auto [flakeRef, fragment] =
        parseFlakeRefWithFragment(myArgs.releaseExpr, absPath("."));

    auto vFlake = state.allocValue();

    auto lockedFlake = lockFlake(state, flakeRef,
                                 LockFlags{
                                     .updateLockFile = false,
                                     .useRegistries = false,
                                     .allowMutable = false,
                                 });

    callFlake(state, lockedFlake, *vFlake);

    auto vOutputs = vFlake->attrs->get(state.symbols.create("outputs"))->value;
    state.forceValue(*vOutputs, noPos);
    auto vTop = *vOutputs;

    if (fragment.length() > 0) {
        Bindings &bindings(*state.allocBindings(0));
        auto [nTop, pos] = findAlongAttrPath(state, fragment, bindings, vTop);
        if (!nTop)
            throw Error("error: attribute '%s' missing", nTop);
        vTop = *nTop;
    }

    auto vRoot = state.allocValue();
    state.autoCallFunction(autoArgs, vTop, *vRoot);

    return vRoot;
}

Value *topLevelValue(EvalState &state, Bindings &autoArgs) {
    return myArgs.flake ? flakeTopLevelValue(state, autoArgs)
                        : releaseExprTopLevelValue(state, autoArgs);
}

static void worker(EvalState &state, Bindings &autoArgs,
                   AutoCloseFD &to, AutoCloseFD &from)
{
    auto vRoot = topLevelValue(state, autoArgs);

    while (true) {
        /* Wait for the collector to send us a job name. */
        writeLine(to.get(), "next");

        auto s = readLine(from.get());
        if (s == "exit") break;
        if (!hasPrefix(s, "do ")) abort();
        auto pathStr = std::string(s, 3);

        debug("worker process %d at '%s'", getpid(), pathStr);

        /* Evaluate it and send info back to the collector. */
        try {
            auto path = AccessorPath(pathStr);

            auto pathJson = json{ { "path", path.toJson() } };

            auto job = path.walk(state, autoArgs, *vRoot);

            for (auto & res : job->eval(state)) {
              auto reply = pathJson;

              reply.update(res->toJson());

              writeLine(to.get(), reply.dump());
            }

        } catch (EvalError &e) {
            auto err = e.info();

            std::ostringstream oss;
            showErrorInfo(oss, err, loggerSettings.showTrace.get());
            auto msg = oss.str();

            // Transmits the error we got from the previous evaluation
            // in the JSON output.
            auto reply = json{ { "error", filterANSIEscapes(msg, true) } } ;
            // Don't forget to print it into the STDERR log, this is
            // what's shown in the Hydra UI.
            printError(e.msg());

            writeLine(to.get(), reply.dump());
        }

        /* If our RSS exceeds the maximum, exit. The collector will
           start a new process. */
        struct rusage r;
        getrusage(RUSAGE_SELF, &r);
        if ((size_t)r.ru_maxrss > myArgs.maxMemorySize * 1024)
            break;
    }

    writeLine(to.get(), "restart");
}

struct State
{
    std::set<json> todo{json::array()};
    std::set<json> active;
    std::exception_ptr exc;
};

void collector(Sync<State> & state_, std::condition_variable & wakeup) {
    try {
        std::optional<std::unique_ptr<Proc>> proc_;

        while (true) {

            auto proc = proc_.has_value() ? std::move(proc_.value())
                                          : std::make_unique<Proc>(worker);

            /* Check whether the existing worker process is still there. */
            auto s = readLine(proc->from.get());
            if (s == "restart") {
                proc_ = std::nullopt;
                continue;
            } else if (s != "next") {
                auto json = json::parse(s);
                if (json.find("error") != json.end())
                    throw Error("worker error: %s", (std::string) json["error"]);
            }

            /* Wait for a job name to become available. */
            json accessor;

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
            auto response = json::parse(respString);

            if (response.find("children") != response.end()) {
                if (response.find("path") != response.end()) {
                    try {
                        std::vector<json> children = response["children"];
                        std::vector<json> path = response["path"];

                        auto state(state_.lock());
                        for (auto & child : children) {
                            path.push_back(child);
                            state->todo.insert(path);
                            path.pop_back();
                        }
                    } catch (json::exception & e) {
                        throw EvalError("expected an array of children and a path from worker, got %s", response.dump());
                    }

                } else {
                    throw EvalError("worker returned children with no path, got: %s", response.dump());
                }
            } else {
                auto state(state_.lock());
                std::cout << response << "\n" << std::flush;
            }

            proc_ = std::move(proc);

            /* Print the response. */
            {
                auto state(state_.lock());
                state->active.erase(accessor);
                wakeup.notify_all();
            }
        }
    } catch (...) {
        auto state(state_.lock());
        state->exc = std::current_exception();
        wakeup.notify_all();
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

        /* FIXME: The build hook in conjunction with import-from-derivation is
         * causing "unexpected EOF" during eval */
        settings.builders = "";

        /* Prevent access to paths outside of the Nix search path and
           to the environment. */
        evalSettings.restrictEval = false;

        /* When building a flake, use pure evaluation (no access to
           'getEnv', 'currentSystem' etc. */
        evalSettings.pureEval = myArgs.evalMode == evalAuto
                                    ? myArgs.flake
                                    : myArgs.evalMode == evalPure;

        if (myArgs.releaseExpr == "")
            throw UsageError("no expression specified");

        if (myArgs.gcRootsDir == "")
            printMsg(lvlError, "warning: `--gc-roots-dir' not specified");

        if (myArgs.showTrace) {
            loggerSettings.showTrace.assign(true);
        }

        Sync<State> state_;

        /* Start a collector thread per worker process. */
        std::vector<std::thread> threads;
        std::condition_variable wakeup;
        for (size_t i = 0; i < myArgs.nrWorkers; i++)
            threads.emplace_back(std::thread([&] {
                collector(state_, wakeup);
            }));

        for (auto &thread : threads)
            thread.join();

        auto state(state_.lock());

        if (state->exc)
            std::rethrow_exception(state->exc);
    });
}
