#include <nix/config.h>
#include <nix/util.hh>

#include "msg.hh"
#include "handler.hh"
#include "accessor.hh"
#include "job.hh"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace nix_eval_jobs {

/* Parse a CollectMsg */
std::unique_ptr<CollectMsg> parseCollectMsg(std::string & s) {
    try {
        return std::make_unique<CollectExit>(s);
    } catch (TypeError & e1) {
        try {
            return std::make_unique<CollectDo>(s);
        } catch (TypeError & e2) {
            throw TypeError("could not parse CollectMsg: %s, %s", e1.msg(), e2.msg());
        }
    }
}

/* CollectMsg */

CollectExit::CollectExit(std::string & s) {
    if (s != "exit")
        throw TypeError("expecting \"exit\", got: %s", s);
}

CollectDo::CollectDo(std::string & s) {
    if (hasPrefix(s, "do ")) {
        auto s_ = std::string(s, 3);
        this->path = AccessorPath(s_);
    }

    else throw TypeError("expecting \"do\" followed by AccessorPath, got: %s", s);
}

std::unique_ptr<WorkMsg> parseWorkMsg(std::string & s) {
    try {
        return std::make_unique<WorkRestart>(s);
    } catch (TypeError & e1) {
        try {
            return std::make_unique<WorkNext>(s);
        } catch (TypeError & e2) {
            try {
                return std::make_unique<WorkError>(s);
            } catch (TypeError & e3) {
                throw TypeError("could not parse WorkMsg: %s, %s, %s",
                                e1.msg(), e2.msg(), e3.msg());
            }
        }
    }
}

/* WorkMsg */

WorkRestart::WorkRestart(std::string & s) {
    if (s != "restart")
        throw TypeError("expecting \"restart\", got: %s", s);
}

WorkNext::WorkNext(std::string & s) {
    if (s != "next")
        throw TypeError("expecting \"next\", got: %s", s);
}

std::unique_ptr<WorkJob> parseWorkJob(std::string & s) {
    try {
        return std::make_unique<WorkDrv>(s);
    } catch (TypeError & e1) {
        try {
            return std::make_unique<WorkChildren>(s);
        } catch (TypeError & e2) {
            try {
                return std::make_unique<WorkDone>(s);
            } catch(TypeError & e3) {
                try {
                    return std::make_unique<WorkError>(s);
                } catch (TypeError & e4) {
                    throw TypeError("could not parse WorkJob: %s, %s, %s, %s",
                                    e1.msg(), e2.msg(), e3.msg(), e4.msg());
                }
            }
        }
    }
}

/* WorkJob */

WorkDrv::WorkDrv(const std::string & s) try : drv(Drv(json::parse(s))) { }
    catch (json::exception & e) {
        throw TypeError("could not parse WorkDrv as json, got: %s", s);
    }

WorkChildren::WorkChildren(const std::string & s) {
    try {
        auto j = json::parse(s);
        json c = j["children"];
        json p = j["path"];
        this->path = AccessorPath(p);
        this->children = JobChildren(c);
    } catch (json::exception & e) {
        throw TypeError("could not parse WorkDrv as json, got: %s", s);
    }
}

WorkDone::WorkDone(const std::string & s) {
    if (s != "done")
        throw TypeError("expecting \"done\", got: %s", s);
}

WorkError::WorkError(const std::string & s) {
    try {
        auto j = json::parse(s);
        if (j.find("error") != j.end())
            this->detail = j["error"];
    } catch (json::exception & e) {
        throw TypeError("could not parse WorkError as json, got: %s", s);
    }
}

}
