#pragma once

#include <nlohmann/json.hpp>
#include <nix/util.hh>

#include "accessor.hh"
#include "handler.hh"
#include "job.hh"

/* Messages may either be sent or read by a collecting process or
   worker process.

   They are introduced by parsing strings from pipe handles.

   Use them by `handle`ing them with their respective `Handle*`rs.
 */

using json = nlohmann::json;
using namespace nix;

namespace nix_eval_jobs {

/* CollectMsg := CollectExit | CollectDo */
class CollectMsg {
public:
    /* handle : CollectMsg -> HandleCollect -> void */
    virtual void handle(const HandleCollect & handlers) = 0;
    /* send : CollectMsg -> AutoCloseFD -> void */
    virtual void send(AutoCloseFD & d) = 0;
    virtual ~CollectMsg() { };
};

/* Parse a Collect message */
std::unique_ptr<CollectMsg> parseCollectMsg(std::string & s);

struct CollectExit : CollectMsg {
    CollectExit() = default;
    CollectExit(std::string & s);
    CollectExit(const CollectExit & that) = default;
    void handle(const HandleCollect & handlers) override { handlers.exit(*this); };
    void send(AutoCloseFD & d) override { writeLine(d.get(), "exit"); };
    ~CollectExit() { };
};

struct CollectDo : CollectMsg {
private:
    std::string s;
public:
    AccessorPath path;
    CollectDo(std::string & s);
    CollectDo(const AccessorPath & path) : path(path) { };
    CollectDo(const CollectDo & that) : path(that.path) { };
    void handle(const HandleCollect & handlers) override { handlers.do_(*this); };
    void send(AutoCloseFD & d) override { writeLine(d.get(), "do " + this->path.toJson().dump()); };
    ~CollectDo() { };
};

/* Parse a Collect message */
std::unique_ptr<CollectMsg> parseCollectMsg(std::string & s);

/* WorkMsg := WorkRestart | WorkNext | WorkJob */
class WorkMsg {
public:
    /* handle : WorkMsg -> HandleWork -> void */
    virtual void handle(const HandleWork & handlers) = 0;
    /* send : WorkMsg -> AutoCloseFD -> void */
    virtual void send(AutoCloseFD & d) = 0;
    virtual ~WorkMsg() { };
};

/* Parse a Work message */
std::unique_ptr<WorkMsg> parseWorkMsg(std::string & s);

struct WorkRestart : WorkMsg {
    WorkRestart() = default;
    WorkRestart(std::string & s);
    WorkRestart(const WorkRestart & that) = default;
    void handle(const HandleWork & handlers) override { handlers.restart(*this); };
    void send(AutoCloseFD & d) override { writeLine(d.get(), "restart"); };
    ~WorkRestart() { };
};

struct WorkNext : WorkMsg {
    WorkNext() = default;
    WorkNext(std::string & s);
    WorkNext(const WorkNext & that) = default;
    void handle(const HandleWork & handlers) override { handlers.next(*this); };
    void send(AutoCloseFD & d) override { writeLine(d.get(), "next"); };
    ~WorkNext() { };
};

/* WorkJob := WorkDrv | WorkChildren | WorkDone | WorkError */
class WorkJob {
public:
    /* handle : WorkJob -> HandleJob -> void */
    virtual void handle(const HandleJob & handlers) = 0;
    /* send : WorkJob -> AutoCloseFD -> void*/
    virtual void send(AutoCloseFD & d) = 0;
    virtual ~WorkJob() { };
};

/* Parse a WorkJob message */
std::unique_ptr<WorkJob> parseWorkJob(std::string & s);

struct WorkDrv : WorkJob {
    Drv drv;
    AccessorPath path;
    WorkDrv(const std::string & s);
    WorkDrv(const Drv & drv, const AccessorPath & path) : drv(drv), path(path) { };
    void handle(const HandleJob & handlers) override { handlers.drv(*this); };
    void send(AutoCloseFD & d) override {
        auto j = this->drv.toJson();
        j.update(json{ { "path", this->path.toJson() } });
        writeLine(d.get(), j.dump());
    };
    ~WorkDrv() { };
};

struct WorkChildren : WorkJob {
    AccessorPath path;
    JobChildren children;
    WorkChildren(const std::string & s);
    WorkChildren(const AccessorPath & path, const JobChildren & children)
        : path(path), children(children) { };
    void handle(const HandleJob & handlers) override { handlers.children(*this); };
    void send(AutoCloseFD & d) override {
        auto out = json{
            { "path", this->path.toJson() },
            { "children", this->children.toJson() },
        };
        writeLine(d.get(), out.dump());
    };
    ~WorkChildren() { };
};

struct WorkDone : WorkJob {
    WorkDone() = default;
    WorkDone(const std::string & s);
    WorkDone(const WorkDone & that) = default;
    void handle(const HandleJob & handlers) override { handlers.done(*this); };
    void send(AutoCloseFD & d) override { writeLine(d.get(), "done"); };
    ~WorkDone() { };
};

struct WorkError : public WorkJob, public WorkMsg {
    std::string detail;
    WorkError(const std::string & s);
    WorkError(const WorkError & that) = default;
    void handle(const HandleWork & handlers) override { handlers.error(*this); };
    void handle(const HandleJob & handlers) override { handlers.error(*this); };
    void send(AutoCloseFD & d) override { writeLine(d.get(), json{ { "error", detail } }); };
    ~WorkError() { };
};

}
