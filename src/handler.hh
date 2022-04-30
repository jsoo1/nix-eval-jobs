#pragma once

namespace nix_eval_jobs {

struct CollectExit;
struct CollectRestart;
struct CollectDo;

struct WorkRestart;
struct WorkNext;
struct WorkDrv;
struct WorkChildren;
struct WorkDone;
struct WorkError;

struct Drv;
struct JobChildren;

/* How to handle a CollectMsg */
struct HandleCollect {
    std::function<void(const CollectExit & msg)> exit;
    std::function<void(const CollectDo & msg)> do_;
    ~HandleCollect() { };
};

/* How to handle a WorkJob */
struct HandleJob {
    std::function<void(const WorkDrv & msg)> drv;
    std::function<void(const WorkChildren & msg)> children;
    std::function<void(const WorkDone & msg)> done;
    std::function<void(const WorkError & msg)> error;
    ~HandleJob() { };
};

/* How to handle a WorkMsg */
struct HandleWork {
    std::function<void(const WorkRestart & msg)> restart;
    std::function<void(const WorkNext & msg)> next;
    std::function<void(const WorkError & msg)> error;
    ~HandleWork() { };
};

/* How to handle an EvalResult */
struct HandleEvalResult {
    std::function<void(const Drv & drv)> drv;
    std::function<void(const JobChildren & children)> children;
    ~HandleEvalResult() { };
};

}
