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
#include "msg.hh"
#include "handler.hh"
#include "job.hh"
#include "util.hh"

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
                                     .inputOverrides = {},
                                     .inputUpdates = {},
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

        std::string s;
        try {
            s = readLine(from.get());
        } catch (std::exception & e) {
            throw EvalError("collector died unexpectedly: %s", e.what());
        }
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

              writeLine(to.get(), "done");
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
    std::set<json> todo;
    std::set<json> active;
    std::exception_ptr exc;
};

/* Can't goto out of a lambda or scope, so enumerate transitions when handling msgs  */
typedef enum { waitForJob, waitForWorker, errUnsetW } waitForWorkerLabel;
typedef enum { finished, awaitResponsesJ, waitForJobJ, errUnsetJ } waitForJobLabel;
typedef enum { awaitResponses, saveProc, errUnsetR } awaitResponsesLabel;

void collector(Sync<State> & state_, std::condition_variable & wakeup) {
    try {
        std::optional<std::unique_ptr<Proc>> proc_;

        waitForWorkerLabel waitForWorkerJump = errUnsetW;
        waitForJobLabel waitForJobJump = errUnsetJ;
        awaitResponsesLabel awaitResponsesJump = errUnsetR;

        std::optional<json> current;


    wait_for_worker:
        waitForWorkerJump = errUnsetW;

        auto proc = proc_.has_value() ? std::move(proc_.value())
                                      : std::make_unique<Proc>(worker);

        /* Check whether the existing worker process is still there. */
        std::string s;
        try {
            s = readLine(proc->from.get());
        } catch (std::exception & e) {
            throw EvalError("worker process died unexpectedly: %s", e.what());
        }

        auto controlMsg = parseWorkMsg(s);

        controlMsg->handle(HandleWork {
            .restart = [&](const WorkRestart & msg) {
                proc_ = std::nullopt;
                waitForWorkerJump = waitForWorker;
            },
            .next = [&](const WorkNext & msg) {
                waitForWorkerJump = waitForJob;
            },
            .error = [&](const WorkError & msg) {
                throw Error("worker error: %s", msg.detail);
            }
        });

        switch (waitForWorkerJump) {
        case waitForJob: goto wait_for_job;
        case waitForWorker: goto wait_for_worker;
        case errUnsetW:
            throw Error("something awful happened managing jumps waiting for worker");
        }

    wait_for_job: /* Wait for a job name to become available. */
        waitForJobJump = errUnsetJ;
        {
            checkInterrupt();
            auto state(state_.lock());
            if ((state->todo.empty() && state->active.empty()) || state->exc) {
                CollectExit().send(proc->to);

                waitForJobJump = finished;
            }
            if (!state->todo.empty()) {
                auto begin = state->todo.begin();
                current = *begin;
                state->todo.erase(begin);
                state->active.insert(*current);

                CollectDo(AccessorPath(*begin)).send(proc->to);

                waitForJobJump = awaitResponsesJ;
            } else {
                state.wait(wakeup);

                waitForJobJump = waitForJobJ;
            }
        }

        switch (waitForJobJump) {
        case finished: return;
        case waitForJobJ: goto wait_for_job;
        case awaitResponsesJ: goto await_responses;
        case errUnsetJ:
            throw Error("something awful happened managing jumps waiting for job");
        }


    await_responses:
        awaitResponsesJump = errUnsetR;

        std::string respString;
        try {
            respString = readLine(proc->from.get());
        } catch (std::exception & e) {
            throw EvalError("worker process died unexpectedly while processing results: %s", e.what());
        }


        auto jobMsg = parseWorkJob(respString);

        jobMsg->handle(HandleJob {
            .drv = [&](const WorkDrv & msg) {
                auto state(state_.lock());
                json d;
                nix_eval_jobs::to_json(d, msg.drv);
                std::cout << d << std::endl << std::flush;

                json p;
                nix_eval_jobs::to_json(p, msg.path);
                state->active.erase(p);
                wakeup.notify_all();

                awaitResponsesJump = awaitResponses;
            },
            .children = [&](const WorkChildren & msg) {
                json j;
                nix_eval_jobs::to_json(j, msg.path);
                std::vector<json> path = j;
                auto state(state_.lock());
                for (auto & child : *msg.children.children) {
                    path.push_back(child->toJson());
                    state->todo.insert(path);
                    path.pop_back();
                }

                awaitResponsesJump = saveProc;
            },
            .done = [&](const WorkDone & msg) {
                awaitResponsesJump = saveProc;
            },
            .error = [&](const WorkError & msg) {
                throw Error("worker error: %s", msg.detail);
            },
        });

        switch (awaitResponsesJump) {
        case awaitResponses: goto await_responses;
        case saveProc: goto save_proc;
        case errUnsetR:
            throw Error("something awful happened managing jumps awaiting responses");
        }

    save_proc:
        proc_ = std::move(proc);
        goto wait_for_worker;

    } catch (...) {
        auto state(state_.lock());
        state->exc = std::current_exception();
        wakeup.notify_all();
    }
}

static void collectTopLevelJob(
    EvalState & state,
    Bindings & autoArgs,
    AutoCloseFD & to,
    AutoCloseFD & from)
{
    try {
	using namespace std::chrono_literals;
        std::this_thread::sleep_for(20s);
        auto vRoot = topLevelValue(state, autoArgs);

        auto job = getJob(state, autoArgs, *vRoot);

        for (auto & res : job->eval(state)) res->handle(HandleEvalResult {
            .drv = [&](const Drv & drv) {
                WorkDrv(drv, AccessorPath()).send(to);
            },
            .children = [&](const JobChildren & children) {
                WorkChildren(AccessorPath(), children).send(to);
            },
        });

    } catch (Error & e) {
        WorkError(e.msg()).send(to);
    }

    WorkDone().send(to);
}

void initState(Sync<State> & state_) {
    /* Collect initial attributes to evaluate. This must be done in a
       separate fork to avoid spawning a download in the parent
       process. If that happens, worker processes will try to enqueue
       downloads on their own download threads (which will not
       exist). Then the worker processes will hang forever waiting for
       downloads.
    */
    auto proc = Proc(collectTopLevelJob);
    bool keepGoing = true;

    while (keepGoing) {
        std::string s;
        try {
            s = readLine(proc.from.get());
        } catch (std::exception & e) {
            throw EvalError("top level collector process died unexpectedly: %s", e.what());
        }


        auto msg = parseWorkJob(s);

        auto handler = HandleJob {
            .drv = [&](const WorkDrv & msg) {
                json j;
                nix_eval_jobs::to_json(j, msg.drv);
                std::cout << j << std::endl << std::flush;
            },
            .children = [&](const WorkChildren & msg) {
                auto state(state_.lock());
                for (auto & a : *msg.children.children)
                    state->todo.insert(std::vector({a->toJson()}));
            },
            .done = [&](const WorkDone & msg) {
                keepGoing = false;
            },
            .error = [&](const WorkError & msg) {
                throw Error("getting initial attributes: %s", msg.detail);
            }
        };

        msg->handle(handler);
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
        initState(state_);

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
