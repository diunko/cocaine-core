#include <iomanip>
#include <sstream>

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>

#include "cocaine/drivers.hpp"
#include "cocaine/engine.hpp"
#include "cocaine/registry.hpp"

using namespace cocaine::engine;
using namespace cocaine::lines;

bool engine_t::shortest_queue_t::operator()(pool_t::reference left, pool_t::reference right) {
    return ((left.second->queue().size() < right.second->queue().size()) &&
             left.second->active());
}

void engine_t::pause_t::operator()(task_map_t::reference task) {
    task.second->pause();
}

void engine_t::resume_t::operator()(task_map_t::reference task) {
    task.second->resume();
}

engine_t::engine_t(zmq::context_t& context, const std::string& name):
    m_context(context),
    m_messages(m_context, ZMQ_ROUTER)
{
    m_app_cfg.name = name;

    syslog(LOG_DEBUG, "engine [%s]: constructing", m_app_cfg.name.c_str());
    
    m_messages.bind("ipc:///var/run/cocaine/engines/" + m_app_cfg.name);
    
    m_watcher.set<engine_t, &engine_t::message>(this);
    m_watcher.start(m_messages.fd(), ev::READ);
    m_processor.set<engine_t, &engine_t::process>(this);
    m_processor.start();
}

engine_t::~engine_t() {
    syslog(LOG_DEBUG, "engine [%s]: destructing", m_app_cfg.name.c_str()); 
}

// Operations
// ----------

Json::Value engine_t::start(const Json::Value& manifest) {
    static std::map<const std::string, unsigned int> types = boost::assign::map_list_of
        ("auto", AUTO)
        ("fs", FILESYSTEM)
        ("server", SERVER);

    // Application configuration
    // -------------------------

    m_app_cfg.type = manifest["type"].asString();
    m_app_cfg.args = manifest["args"].asString();

    if(!core::registry_t::instance()->exists(m_app_cfg.type)) {
        throw std::runtime_error("no plugin for '" + m_app_cfg.type + "' is available");
    }
    
    syslog(LOG_INFO, "engine [%s]: starting", m_app_cfg.name.c_str()); 
    
    // Pool configuration
    // ------------------

    m_pool_cfg.backend = manifest["engine"].get("backend",
        config_t::get().engine.backend).asString();
    
    if(m_pool_cfg.backend != "thread" && m_pool_cfg.backend != "process") {
        throw std::runtime_error("invalid backend type");
    }
    
    m_pool_cfg.heartbeat_timeout = manifest["engine"].get("heartbeat-timeout",
        config_t::get().engine.heartbeat_timeout).asDouble();
    m_pool_cfg.suicide_timeout = manifest["engine"].get("suicide-timeout",
        config_t::get().engine.suicide_timeout).asDouble();
    m_pool_cfg.pool_limit = manifest["engine"].get("pool-limit",
        config_t::get().engine.pool_limit).asUInt();
    m_pool_cfg.queue_limit = manifest["engine"].get("queue-limit",
        config_t::get().engine.queue_limit).asUInt();
    m_pool_cfg.spawn_threshold = manifest["engine"].get("spawn-threshold",
        config_t::get().engine.spawn_threshold).asUInt();
    
    // Tasks configuration
    // -------------------

    Json::Value tasks(manifest["tasks"]);

    if(!tasks.isNull() && tasks.size()) {
        std::string endpoint = manifest["pubsub"]["endpoint"].asString();
        
        if(!endpoint.empty()) {
            m_pubsub.reset(new socket_t(m_context, ZMQ_PUB));
            m_pubsub->bind(endpoint);
        }

        Json::Value::Members names(tasks.getMemberNames());

        for(Json::Value::Members::iterator it = names.begin(); it != names.end(); ++it) {
            std::string type(tasks[*it]["type"].asString());
            
            if(types.find(type) == types.end()) {
               throw std::runtime_error("no driver for '" + type + "' is available");
            }

            switch(types[type]) {
                case AUTO:
                    schedule<drivers::auto_t>(*it, tasks[*it]);
                    break;
                case FILESYSTEM:
                    schedule<drivers::fs_t>(*it, tasks[*it]);
                    break;
                case SERVER:
                    schedule<drivers::server_t>(*it, tasks[*it]);
                    break;
            }
        }
    } else {
        throw std::runtime_error("no tasks has been specified");
    }

    return info();
}

template<class DriverType>
void engine_t::schedule(const std::string& method, const Json::Value& args) {
    m_tasks.insert(std::make_pair(
        method,
        new DriverType(method, shared_from_this(), args)));
}

Json::Value engine_t::stop() {
    Json::Value result;

    syslog(LOG_INFO, "engine [%s]: stopping", m_app_cfg.name.c_str()); 
    
    for(pool_t::iterator it = m_pool.begin(); it != m_pool.end(); ++it) {
        m_messages.send_multi(boost::make_tuple(
            protect(it->first),
            unique_id_t().id(),
            TERMINATE));
        m_pool.erase(it);
    }
    
    m_tasks.clear();
    
    m_watcher.stop();
    m_processor.stop();

    result["engine"]["status"] = "stopped";
    
    return result;
}

namespace {
    struct nonempty_queue {
        bool operator()(engine_t::pool_t::reference worker) {
            return worker->second->queue().size() != 0;
        }
    };
}

Json::Value engine_t::info() {
    Json::Value results(Json::objectValue);

    results["engine"]["status"] = "running";

    results["pool"]["total"] = static_cast<Json::UInt>(m_pool.size());
    results["pool"]["active"] = static_cast<Json::UInt>(std::count_if(
        m_pool.begin(),
        m_pool.end(),
        nonempty_queue()));
    results["pool"]["limit"] = m_pool_cfg.pool_limit;

    for(pool_t::const_iterator it = m_pool.begin(); it != m_pool.end(); ++it) {
        results["pool"]["queues"][it->first] = 
            static_cast<Json::UInt>(it->second->queue().size());
    }

    for(task_map_t::const_iterator it = m_tasks.begin(); it != m_tasks.end(); ++it) {
        results["tasks"][it->second->method()] = it->second->info();
    }
    
    return results;
}

void engine_t::reap(unique_id_t::reference worker_id) {
    pool_t::iterator worker(m_pool.find(worker_id));

    // TODO: Re-assign tasks
    if(worker != m_pool.end()) {
        for(backend_t::deferred_queue_t::iterator it = worker->second->queue().begin(); 
            it != worker->second->queue().end();
            ++it) 
        {
            it->second->abort("timeout");
        }
        
        m_pool.erase(worker);
    }
}

// Deferred support
// ----------------

void engine_t::publish(const std::string& key, const Json::Value& object) {
    if(m_pubsub && object.isObject()) {
        zmq::message_t message;
        ev::tstamp now = ev::get_default_loop().now();

        // Disassemble and send in the envelopes
        Json::Value::Members members(object.getMemberNames());

        for(Json::Value::Members::iterator it = members.begin(); it != members.end(); ++it) {
            std::string field(*it);
            
            std::ostringstream envelope;
            envelope << key << " " << field << " " << config_t::get().core.hostname << " "
                     << std::fixed << std::setprecision(3) << now;

            message.rebuild(envelope.str().length());
            memcpy(message.data(), envelope.str().data(), envelope.str().length());
            m_pubsub->send(message, ZMQ_SNDMORE);

            Json::Value value(object[field]);
            std::string result;

            switch(value.type()) {
                case Json::booleanValue:
                    result = value.asBool() ? "true" : "false";
                    break;
                case Json::intValue:
                case Json::uintValue:
                    result = boost::lexical_cast<std::string>(value.asInt());
                    break;
                case Json::realValue:
                    result = boost::lexical_cast<std::string>(value.asDouble());
                    break;
                case Json::stringValue:
                    result = value.asString();
                    break;
                default:
                    result = boost::lexical_cast<std::string>(value);
            }

            message.rebuild(result.length());
            memcpy(message.data(), result.data(), result.length());
            m_pubsub->send(message);
        }
    }
}

// Thread I/O
// ----------

void engine_t::message(ev::io& w, int revents) {
    if(m_messages.pending()) {
        m_processor.start();
        m_watcher.stop();
    }
}

void engine_t::process(ev::idle& w, int revents) {
    if(m_messages.pending()) {
        std::string worker_id;
        unsigned int code = 0;

        boost::tuple<raw<std::string>, unsigned int&> tier(protect(worker_id), code);
        m_messages.recv_multi(tier);

        pool_t::iterator worker(m_pool.find(worker_id));

        if(worker != m_pool.end()) {
            // NOTE: Any type of message is suitable to rearm the timeout
            // and we don't have to do anything special about HEARBEAT messages at all.
            worker->second->rearm(m_pool_cfg.heartbeat_timeout);
           
            switch(code) {
                case CHUNK: {
                    unique_id_t::type deferred_id;
                    zmq::message_t chunk;

                    boost::tuple<std::string&, zmq::message_t*> tier(deferred_id, &chunk);
                    m_messages.recv_multi(tier);

                    backend_t::deferred_queue_t::iterator deferred(
                        worker->second->queue().find(deferred_id));

                    if(deferred != worker->second->queue().end()) {
                        deferred->second->send(chunk);
                    } else {
                        syslog(LOG_ERR, "engine [%s]: receiving chunks from an unexpected worker",
                            m_app_cfg.name.c_str());
                    }
                    
                    break;
                }
              
                case CHOKE: {
                    unique_id_t::type deferred_id;
                    
                    m_messages.recv(deferred_id);

                    worker->second->queue().erase(deferred_id);
                    
                    break;
                }

                case SUICIDE:
                    if(worker->second->queue().size()) {
                        syslog(LOG_WARNING, "engine [%s]: a suicide with requests in the queue detected",
                            m_app_cfg.name.c_str());
                    }

                    reap(worker->first);

                    break;
            }
        } else {
            syslog(LOG_WARNING, "engine [%s]: dropping messages for worker %s", 
                m_app_cfg.name.c_str(), worker_id.c_str());
            m_messages.ignore();
        }
    } else {
        m_processor.stop();
        m_watcher.start(m_messages.fd(), ev::READ);
    }
}

