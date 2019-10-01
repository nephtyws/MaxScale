/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file session.c  - A representation of the session within the gateway.
 */
#include <maxscale/session.hh>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <algorithm>
#include <string>
#include <sstream>

#include <maxbase/atomic.hh>
#include <maxbase/host.hh>
#include <maxbase/alloc.h>
#include <maxscale/clock.h>
#include <maxscale/cn_strings.hh>
#include <maxscale/dcb.hh>
#include <maxscale/housekeeper.h>
#include <maxscale/json_api.hh>
#include <maxscale/modutil.hh>
#include <maxscale/poll.hh>
#include <maxscale/router.hh>
#include <maxscale/routingworker.hh>
#include <maxscale/service.hh>
#include <maxscale/utils.h>
#include <maxscale/protocol/mariadb/mysql.hh>

#include "internal/filter.hh"
#include "internal/session.hh"
#include "internal/service.hh"
#include "internal/listener.hh"

using std::string;
using std::stringstream;
using maxbase::Worker;
using namespace maxscale;

namespace
{

struct
{
    /* Global session id counter. Must be updated atomically. Value 0 is reserved for
     *  dummy/unused sessions.
     */
    uint64_t                  next_session_id;
    uint32_t                  retain_last_statements;
    session_dump_statements_t dump_statements;
    uint32_t                  session_trace;
} this_unit =
{
    1,
    0,
    SESSION_DUMP_STATEMENTS_NEVER,
    0
};
}

MXS_SESSION::MXS_SESSION(const SListener& listener)
    : m_state(MXS_SESSION::State::CREATED)
    , m_id(session_get_next_id())
    , client_dcb(nullptr)
    , listener(listener)
    , stats{time(0)}
    , service(listener ? listener->service() : nullptr)
    , refcount(1)
    , trx_state(SESSION_TRX_INACTIVE)
    , autocommit(listener->sql_mode() == QC_SQL_MODE_ORACLE ? false : true)
    , client_protocol_data(0)
    , qualifies_for_pooling(false)
    , response{}
    , close_reason(SESSION_CLOSE_NONE)
    , load_active(false)
{
    mxs_rworker_register_session(this);
}

MXS_SESSION::~MXS_SESSION()
{
    MXB_AT_DEBUG(bool removed = ) mxs_rworker_deregister_session(this);
    mxb_assert(removed);
}

void MXS_SESSION::terminate(GWBUF* error)
{
    if (m_state == State::STARTED)
    {
        mxb_assert(!client_connection()->dcb()->is_closed());
        m_state = State::STOPPING;

        if (error)
        {
            // Write the error to the client before closing the DCB
            client_connection()->write(error);
        }

        DCB::close(client_dcb);
    }
}

MXS_SESSION::ProtocolData* MXS_SESSION::protocol_data() const
{
    return m_protocol_data.get();
}

void MXS_SESSION::set_protocol_data(std::unique_ptr<ProtocolData> new_data)
{
    m_protocol_data = std::move(new_data);
}

const char* MXS_SESSION::client_remote() const
{
    auto conn = client_connection();
    if (conn)
    {
        return conn->dcb()->remote().c_str();
    }
    return nullptr;
}

bool session_start(MXS_SESSION* ses)
{
    Session* session = static_cast<Session*>(ses);
    return session->start();
}

void session_link_backend_dcb(MXS_SESSION* session, BackendDCB* dcb)
{
    mxb_assert(dcb->owner == session->client_connection()->dcb()->owner);
    mxb_assert(dcb->role() == DCB::Role::BACKEND);

    mxb::atomic::add(&session->refcount, 1);
    dcb->reset(session);

    Session* ses = static_cast<Session*>(session);
    ses->link_backend_dcb(dcb);
}

void session_unlink_backend_dcb(MXS_SESSION* session, DCB* dcb)
{
    Session* ses = static_cast<Session*>(session);
    ses->unlink_backend_dcb(dcb);
    session_put_ref(session);
}

void session_close(MXS_SESSION* ses)
{
    Session* session = static_cast<Session*>(ses);
    session->close();
}

/**
 * Deallocate the specified session
 *
 * @param session       The session to deallocate
 */
static void session_free(MXS_SESSION* session)
{
    MXS_INFO("Stopped %s client session [%" PRIu64 "]", session->service->name(), session->id());
    Service* service = static_cast<Service*>(session->service);

    delete static_cast<Session*>(session);
}

/**
 * Print details of an individual session
 *
 * @param session       Session to print
 */
void printSession(MXS_SESSION* session)
{
    struct tm result;
    char timebuf[40];

    printf("Session %p\n", session);
    printf("\tState:        %s\n", session_state_to_string(session->state()));
    printf("\tService:      %s (%p)\n", session->service->name(), session->service);
    printf("\tClient DCB:   %p\n", session->client_connection()->dcb());
    printf("\tConnected:    %s\n",
           asctime_r(localtime_r(&session->stats.connect, &result), timebuf));
}

bool printAllSessions_cb(DCB* dcb, void* data)
{
    if (dcb->role() == DCB::Role::CLIENT)
    {
        printSession(dcb->session());
    }

    return true;
}

/**
 * Print all sessions
 *
 * Designed to be called within a debugger session in order
 * to display all active sessions within the gateway
 */
void printAllSessions()
{
    dcb_foreach(printAllSessions_cb, NULL);
}

/** Callback for dprintAllSessions */
bool dprintAllSessions_cb(DCB* dcb, void* data)
{
    if (dcb->role() == DCB::Role::CLIENT)
    {
        DCB* out_dcb = (DCB*)data;
        dprintSession(out_dcb, dcb->session());
    }
    return true;
}

/**
 * Print all sessions to a DCB
 *
 * Designed to be called within a debugger session in order
 * to display all active sessions within the gateway
 *
 * @param dcb   The DCB to print to
 */
void dprintAllSessions(DCB* dcb)
{
    dcb_foreach(dprintAllSessions_cb, dcb);
}

/**
 * Print a particular session to a DCB
 *
 * Designed to be called within a debugger session in order
 * to display all active sessions within the gateway
 *
 * @param dcb   The DCB to print to
 * @param print_session   The session to print
 */
void dprintSession(DCB* dcb, MXS_SESSION* print_session)
{
    struct tm result;
    char buf[30];
    int i;

    dcb_printf(dcb, "Session %" PRIu64 "\n", print_session->id());
    dcb_printf(dcb, "\tState:               %s\n", session_state_to_string(print_session->state()));
    dcb_printf(dcb, "\tService:             %s\n", print_session->service->name());

    if (print_session->client_connection())
    {
        auto client_dcb = print_session->client_connection()->dcb();
        double idle = (mxs_clock() - client_dcb->last_read());
        idle = idle > 0 ? idle / 10.f : 0;
        dcb_printf(dcb,
                   "\tClient Address:          %s@%s\n",
                   print_session->user().c_str(), client_dcb->remote().c_str());
        dcb_printf(dcb,
                   "\tConnected:               %s\n",
                   asctime_r(localtime_r(&print_session->stats.connect, &result), buf));
        if (client_dcb->state() == DCB::State::POLLING)
        {
            dcb_printf(dcb, "\tIdle:                %.0f seconds\n", idle);
        }
    }

    Session* session = static_cast<Session*>(print_session);

    for (const auto& f : session->get_filters())
    {
        dcb_printf(dcb, "\tFilter: %s\n", f.filter->name.c_str());
        f.filter->obj->diagnostics(f.instance, f.session, dcb);
    }
}

bool dListSessions_cb(DCB* dcb, void* data)
{
    if (dcb->role() == DCB::Role::CLIENT)
    {
        DCB* out_dcb = (DCB*)data;
        MXS_SESSION* session = dcb->session();
        dcb_printf(out_dcb,
                   "%-16" PRIu64 " | %-15s | %-14s | %s\n",
                   session->id(),
                   session->client_remote(),
                   session->service && session->service->name() ?
                   session->service->name() : "",
                   session_state_to_string(session->state()));
    }

    return true;
}
/**
 * List all sessions in tabular form to a DCB
 *
 * Designed to be called within a debugger session in order
 * to display all active sessions within the gateway
 *
 * @param dcb   The DCB to print to
 */
void dListSessions(DCB* dcb)
{
    dcb_printf(dcb, "-----------------+-----------------+----------------+--------------------------\n");
    dcb_printf(dcb, "Session          | Client          | Service        | State\n");
    dcb_printf(dcb, "-----------------+-----------------+----------------+--------------------------\n");

    dcb_foreach(dListSessions_cb, dcb);

    dcb_printf(dcb, "-----------------+-----------------+----------------+--------------------------\n\n");
}

/**
 * Convert a session state to a string representation
 *
 * @param state         The session state
 * @return A string representation of the session state
 */
const char* session_state_to_string(MXS_SESSION::State state)
{
    switch (state)
    {
    case MXS_SESSION::State::CREATED:
        return "Session created";

    case MXS_SESSION::State::STARTED:
        return "Session started";

    case MXS_SESSION::State::STOPPING:
        return "Stopping session";

    case MXS_SESSION::State::FAILED:
        return "Session creation failed";

    case MXS_SESSION::State::FREE:
        return "Freed session";

    default:
        return "Invalid State";
    }
}

/**
 * Return the client connection address or name
 *
 * @param session       The session whose client address to return
 */
const char* session_get_remote(const MXS_SESSION* session)
{
    return session ? session->client_remote() : nullptr;
}

void Session::deliver_response()
{
    MXS_FILTER* filter_instance = response.up.instance;

    if (filter_instance)
    {
        MXS_FILTER_SESSION* filter_session = response.up.session;
        GWBUF* buffer = response.buffer;

        mxb_assert(filter_session);
        mxb_assert(buffer);

        // The reply will always be complete
        mxs::ReplyRoute route;
        mxs::Reply reply(response.service);
        response.up.clientReply(filter_instance, filter_session, buffer, route, reply);

        response.up.instance = NULL;
        response.up.session = NULL;
        response.up.clientReply = NULL;
        response.buffer = NULL;

        // If some filter short-circuits the routing, then there will
        // be no response from a server and we need to ensure that
        // subsequent book-keeping targets the right statement.
        book_last_as_complete();
    }

    mxb_assert(!response.up.instance);
    mxb_assert(!response.up.session);
    mxb_assert(!response.up.clientReply);
    mxb_assert(!response.buffer);
}

bool mxs_route_query(MXS_SESSION* ses, GWBUF* buffer)
{
    Session* session = static_cast<Session*>(ses);
    mxb_assert(session);

    bool rv = session->routeQuery(buffer);

    return rv;
}

bool mxs_route_reply(mxs::Upstream* up, GWBUF* buffer, DCB* dcb)
{
    mxs::ReplyRoute route;
    mxs::Reply reply(dcb->session()->listener->service());
    return up->clientReply(up->instance, up->session, buffer, route, reply);
}

/**
 * Return the username of the user connected to the client side of the
 * session.
 *
 * @param session               The session pointer.
 * @return      The user name or NULL if it can not be determined.
 */
const char* session_get_user(const MXS_SESSION* session)
{
    return session ? session->user().c_str() : NULL;
}

bool dcb_iter_cb(DCB* dcb, void* data)
{
    if (dcb->role() == DCB::Role::CLIENT)
    {
        ResultSet* set = static_cast<ResultSet*>(data);
        MXS_SESSION* ses = dcb->session();
        char buf[20];
        snprintf(buf, sizeof(buf), "%p", ses);

        set->add_row({buf, ses->client_remote(), ses->service->name(),
                      session_state_to_string(ses->state())});
    }

    return true;
}

/**
 * Return a resultset that has the current set of sessions in it
 *
 * @return A Result set
 */
/* Lint is not convinced that the new memory for data is always tracked
 * because it does not see what happens within the resultset_create function,
 * so we suppress the warning. In fact, the function call results in return
 * of the set structure which includes a pointer to data
 */
std::unique_ptr<ResultSet> sessionGetList()
{
    std::unique_ptr<ResultSet> set = ResultSet::create({"Session", "Client", "Service", "State"});
    dcb_foreach(dcb_iter_cb, set.get());
    return set;
}

mxs_session_trx_state_t session_get_trx_state(const MXS_SESSION* ses)
{
    return ses->trx_state;
}

mxs_session_trx_state_t session_set_trx_state(MXS_SESSION* ses, mxs_session_trx_state_t new_state)
{
    mxs_session_trx_state_t prev_state = ses->trx_state;

    ses->trx_state = new_state;

    return prev_state;
}

const char* session_trx_state_to_string(mxs_session_trx_state_t state)
{
    switch (state)
    {
    case SESSION_TRX_INACTIVE:
        return "SESSION_TRX_INACTIVE";

    case SESSION_TRX_ACTIVE:
        return "SESSION_TRX_ACTIVE";

    case SESSION_TRX_READ_ONLY:
        return "SESSION_TRX_READ_ONLY";

    case SESSION_TRX_READ_WRITE:
        return "SESSION_TRX_READ_WRITE";

    case SESSION_TRX_READ_ONLY_ENDING:
        return "SESSION_TRX_READ_ONLY_ENDING";

    case SESSION_TRX_READ_WRITE_ENDING:
        return "SESSION_TRX_READ_WRITE_ENDING";
    }

    MXS_ERROR("Unknown session_trx_state_t value: %d", (int)state);
    return "UNKNOWN";
}

static bool ses_find_id(DCB* dcb, void* data)
{
    void** params = (void**)data;
    MXS_SESSION** ses = (MXS_SESSION**)params[0];
    uint64_t* id = (uint64_t*)params[1];
    bool rval = true;

    if (dcb->session()->id() == *id)
    {
        *ses = session_get_ref(dcb->session());
        rval = false;
    }

    return rval;
}

MXS_SESSION* session_get_by_id(uint64_t id)
{
    MXS_SESSION* session = NULL;
    void* params[] = {&session, &id};

    dcb_foreach(ses_find_id, params);

    return session;
}

MXS_SESSION* session_get_ref(MXS_SESSION* session)
{
    mxb::atomic::add(&session->refcount, 1);
    return session;
}

void session_put_ref(MXS_SESSION* session)
{
    if (session)
    {
        /** Remove one reference. If there are no references left, free session */
        if (mxb::atomic::add(&session->refcount, -1) == 1)
        {
            session_free(session);
        }
    }
}

uint64_t session_get_next_id()
{
    return mxb::atomic::add(&this_unit.next_session_id, 1, mxb::atomic::RELAXED);
}

json_t* session_json_data(const Session* session, const char* host, bool rdns)
{
    json_t* data = json_object();

    /** ID must be a string */
    stringstream ss;
    ss << session->id();

    /** ID and type */
    json_object_set_new(data, CN_ID, json_string(ss.str().c_str()));
    json_object_set_new(data, CN_TYPE, json_string(CN_SESSIONS));

    /** Relationships */
    json_t* rel = json_object();

    /** Service relationship (one-to-one) */
    json_t* services = mxs_json_relationship(host, MXS_JSON_API_SERVICES);
    mxs_json_add_relation(services, session->service->name(), CN_SERVICES);
    json_object_set_new(rel, CN_SERVICES, services);

    /** Filter relationships (one-to-many) */
    auto filter_list = session->get_filters();

    if (!filter_list.empty())
    {
        json_t* filters = mxs_json_relationship(host, MXS_JSON_API_FILTERS);

        for (const auto& f : filter_list)
        {
            mxs_json_add_relation(filters, f.filter->name.c_str(), CN_FILTERS);
        }
        json_object_set_new(rel, CN_FILTERS, filters);
    }

    json_object_set_new(data, CN_RELATIONSHIPS, rel);

    /** Session attributes */
    json_t* attr = json_object();
    json_object_set_new(attr, "state", json_string(session_state_to_string(session->state())));

    if (!session->user().empty())
    {
        json_object_set_new(attr, CN_USER, json_string(session->user().c_str()));
    }

    string result_address;
    auto client_dcb = session->client_connection()->dcb();
    auto& remote = client_dcb->remote();
    if (rdns)
    {
        maxbase::reverse_name_lookup(remote, &result_address);
    }
    else
    {
        result_address = remote;
    }

    json_object_set_new(attr, "remote", json_string(result_address.c_str()));

    struct tm result;
    char buf[60];

    asctime_r(localtime_r(&session->stats.connect, &result), buf);
    mxb::trim(buf);

    json_object_set_new(attr, "connected", json_string(buf));

    if (client_dcb->state() == DCB::State::POLLING)
    {
        double idle = (mxs_clock() - client_dcb->last_read());
        idle = idle > 0 ? idle / 10.f : 0;
        json_object_set_new(attr, "idle", json_real(idle));
    }

    json_t* dcb_arr = json_array();
    for (auto d : session->dcb_set())
    {
        json_array_append_new(dcb_arr, dcb_to_json(d));
    }

    json_object_set_new(attr, "connections", dcb_arr);

    json_t* queries = session->queries_as_json();
    json_object_set_new(attr, "queries", queries);

    json_t* log = session->log_as_json();
    json_object_set_new(attr, "log", log);

    json_object_set_new(data, CN_ATTRIBUTES, attr);
    json_object_set_new(data, CN_LINKS, mxs_json_self_link(host, CN_SESSIONS, ss.str().c_str()));

    return data;
}

json_t* session_to_json(const MXS_SESSION* session, const char* host, bool rdns)
{
    stringstream ss;
    ss << MXS_JSON_API_SESSIONS << session->id();
    const Session* s = static_cast<const Session*>(session);
    return mxs_json_resource(host, ss.str().c_str(), session_json_data(s, host, rdns));
}

struct SessionListData
{
    SessionListData(const char* host, bool rdns)
        : json(json_array())
        , host(host)
        , rdns(rdns)
    {
    }

    json_t*     json {nullptr};
    const char* host {nullptr};
    bool        rdns {false};
};

bool seslist_cb(DCB* dcb, void* data)
{
    if (dcb->role() == DCB::Role::CLIENT)
    {
        SessionListData* d = (SessionListData*)data;
        Session* session = static_cast<Session*>(dcb->session());
        json_array_append_new(d->json, session_json_data(session, d->host, d->rdns));
    }

    return true;
}

json_t* session_list_to_json(const char* host, bool rdns)
{
    SessionListData data(host, rdns);
    dcb_foreach(seslist_cb, &data);
    return mxs_json_resource(host, MXS_JSON_API_SESSIONS, data.json);
}

void session_qualify_for_pool(MXS_SESSION* session)
{
    session->qualifies_for_pooling = true;
}

bool session_valid_for_pool(const MXS_SESSION* session)
{
    return session->qualifies_for_pooling;
}

MXS_SESSION* session_get_current()
{
    DCB* dcb = dcb_get_current();

    return dcb ? dcb->session() : NULL;
}

uint64_t session_get_current_id()
{
    MXS_SESSION* session = session_get_current();

    return session ? session->id() : 0;
}

bool session_add_variable(MXS_SESSION* session,
                          const char* name,
                          session_variable_handler_t handler,
                          void* context)
{
    Session* pSession = static_cast<Session*>(session);
    return pSession->add_variable(name, handler, context);
}

char* session_set_variable_value(MXS_SESSION* session,
                                 const char* name_begin,
                                 const char* name_end,
                                 const char* value_begin,
                                 const char* value_end)
{
    Session* pSession = static_cast<Session*>(session);
    return pSession->set_variable_value(name_begin, name_end, value_begin, value_end);
}

bool session_remove_variable(MXS_SESSION* session,
                             const char* name,
                             void** context)
{
    Session* pSession = static_cast<Session*>(session);
    return pSession->remove_variable(name, context);
}

void session_set_response(MXS_SESSION* session, SERVICE* service, const mxs::Upstream* up, GWBUF* buffer)
{
    // Valid arguments.
    mxb_assert(session && up && buffer);

    // Valid state. Only one filter may terminate the execution and exactly once.
    mxb_assert(!session->response.up.instance
               && !session->response.up.session
               && !session->response.buffer);

    session->response.up = *up;
    session->response.buffer = buffer;
    session->response.service = service;
}

void session_set_retain_last_statements(uint32_t n)
{
    this_unit.retain_last_statements = n;
}

uint32_t session_get_retain_last_statements()
{
    return this_unit.retain_last_statements;
}

void session_set_dump_statements(session_dump_statements_t value)
{
    this_unit.dump_statements = value;
}

session_dump_statements_t session_get_dump_statements()
{
    return this_unit.dump_statements;
}

const char* session_get_dump_statements_str()
{
    switch (this_unit.dump_statements)
    {
    case SESSION_DUMP_STATEMENTS_NEVER:
        return "never";

    case SESSION_DUMP_STATEMENTS_ON_CLOSE:
        return "on_close";

    case SESSION_DUMP_STATEMENTS_ON_ERROR:
        return "on_error";

    default:
        mxb_assert(!true);
        return "unknown";
    }
}

void session_retain_statement(MXS_SESSION* pSession, GWBUF* pBuffer)
{
    static_cast<Session*>(pSession)->retain_statement(pBuffer);
}

void session_book_server_response(MXS_SESSION* pSession, SERVER* pServer, bool final_response)
{
    static_cast<Session*>(pSession)->book_server_response(pServer, final_response);
}

void session_reset_server_bookkeeping(MXS_SESSION* pSession)
{
    static_cast<Session*>(pSession)->reset_server_bookkeeping();
}

void session_dump_statements(MXS_SESSION* session)
{
    Session* pSession = static_cast<Session*>(session);
    pSession->dump_statements();
}

void session_set_session_trace(uint32_t value)
{
    this_unit.session_trace = value;
}

uint32_t session_get_session_trace()
{
    return this_unit.session_trace;
}

void session_append_log(MXS_SESSION* pSession, std::string log)
{
    {
        static_cast<Session*>(pSession)->append_session_log(log);
    }
}

void session_dump_log(MXS_SESSION* pSession)
{
    static_cast<Session*>(pSession)->dump_session_log();
}

class DelayedRoutingTask
{
    DelayedRoutingTask(const DelayedRoutingTask&) = delete;
    DelayedRoutingTask& operator=(const DelayedRoutingTask&) = delete;

public:
    DelayedRoutingTask(MXS_SESSION* session, mxs::Downstream down, GWBUF* buffer)
        : m_session(session_get_ref(session))
        , m_down(down)
        , m_buffer(buffer)
    {
    }

    ~DelayedRoutingTask()
    {
        session_put_ref(m_session);
        gwbuf_free(m_buffer);
    }

    void execute()
    {
        if (m_session->state() == MXS_SESSION::State::STARTED)
        {
            GWBUF* buffer = m_buffer;
            m_buffer = NULL;

            if (m_down.routeQuery(m_down.instance, m_down.session, buffer) == 0)
            {
                // Routing failed, send a hangup to the client.
                m_session->client_connection()->dcb()->trigger_hangup_event();
            }
        }
    }

private:
    MXS_SESSION*    m_session;
    mxs::Downstream m_down;
    GWBUF*          m_buffer;
};

static bool delayed_routing_cb(Worker::Call::action_t action, DelayedRoutingTask* task)
{
    if (action == Worker::Call::EXECUTE)
    {
        task->execute();
    }

    delete task;
    return false;
}

bool session_delay_routing(MXS_SESSION* session, mxs::Downstream down, GWBUF* buffer, int seconds)
{
    bool success = false;

    try
    {
        Worker* worker = Worker::get_current();
        mxb_assert(worker == session->client_connection()->dcb()->owner);
        std::unique_ptr<DelayedRoutingTask> task(new DelayedRoutingTask(session, down, buffer));

        // Delay the routing for at least a millisecond
        int32_t delay = 1 + seconds * 1000;
        worker->delayed_call(delay, delayed_routing_cb, task.release());

        success = true;
    }
    catch (std::bad_alloc&)
    {
        MXS_OOM();
    }

    return success;
}

const char* session_get_close_reason(const MXS_SESSION* session)
{
    switch (session->close_reason)
    {
    case SESSION_CLOSE_NONE:
        return "";

    case SESSION_CLOSE_TIMEOUT:
        return "Timed out by MaxScale";

    case SESSION_CLOSE_HANDLEERROR_FAILED:
        return "Router could not recover from connection errors";

    case SESSION_CLOSE_ROUTING_FAILED:
        return "Router could not route query";

    case SESSION_CLOSE_KILLED:
        return "Killed by another connection";

    case SESSION_CLOSE_TOO_MANY_CONNECTIONS:
        return "Too many connections";

    default:
        mxb_assert(!true);
        return "Internal error";
    }
}

Session::Session(const SListener& listener)
    : MXS_SESSION(listener)
    , m_down(static_cast<Service*>(listener->service())->get_connection(this, this))
{
    if (service->config().retain_last_statements != -1)         // Explicitly set for the service
    {
        m_retain_last_statements = service->config().retain_last_statements;
    }
    else
    {
        m_retain_last_statements = this_unit.retain_last_statements;
    }
}

Session::~Session()
{
    mxb_assert(refcount == 0);
    mxb_assert(!m_down->is_open());

    mxb::atomic::add(&service->stats().n_current, -1, mxb::atomic::RELAXED);

    if (client_dcb)
    {
        delete client_dcb;
        client_dcb = NULL;
    }

    if (this_unit.dump_statements == SESSION_DUMP_STATEMENTS_ON_CLOSE)
    {
        session_dump_statements(this);
    }

    m_state = MXS_SESSION::State::FREE;
}

void Session::set_client_dcb(ClientDCB* dcb)
{
    mxb_assert(client_dcb == nullptr);
    client_dcb = dcb;
}

namespace
{

bool get_cmd_and_stmt(GWBUF* pBuffer, const char** ppCmd, char** ppStmt, int* pLen)
{
    *ppCmd = nullptr;
    *ppStmt = nullptr;
    *pLen = 0;

    bool deallocate = false;
    int len = gwbuf_length(pBuffer);

    if (len > MYSQL_HEADER_LEN)
    {
        uint8_t header[MYSQL_HEADER_LEN + 1];
        uint8_t* pHeader = NULL;

        if (GWBUF_LENGTH(pBuffer) > MYSQL_HEADER_LEN)
        {
            pHeader = GWBUF_DATA(pBuffer);
        }
        else
        {
            gwbuf_copy_data(pBuffer, 0, MYSQL_HEADER_LEN + 1, header);
            pHeader = header;
        }

        int cmd = MYSQL_GET_COMMAND(pHeader);

        *ppCmd = STRPACKETTYPE(cmd);

        if (cmd == MXS_COM_QUERY)
        {
            if (GWBUF_IS_CONTIGUOUS(pBuffer))
            {
                modutil_extract_SQL(pBuffer, ppStmt, pLen);
            }
            else
            {
                *ppStmt = modutil_get_SQL(pBuffer);
                *pLen = strlen(*ppStmt);
                deallocate = true;
            }
        }
    }

    return deallocate;
}
}

void Session::dump_statements() const
{
    if (m_retain_last_statements)
    {
        int n = m_last_queries.size();

        uint64_t current_id = session_get_current_id();

        if ((current_id != 0) && (current_id != id()))
        {
            MXS_WARNING("Current session is %lu, yet statements are dumped for %lu. "
                        "The session id in the subsequent dumped statements is the wrong one.",
                        current_id, id());
        }

        for (auto i = m_last_queries.rbegin(); i != m_last_queries.rend(); ++i)
        {
            const QueryInfo& info = *i;
            GWBUF* pBuffer = info.query().get();
            timespec ts = info.time_completed();
            struct tm* tm = localtime(&ts.tv_sec);
            char timestamp[20];
            strftime(timestamp, 20, "%Y-%m-%d %H:%M:%S", tm);

            const char* pCmd;
            char* pStmt;
            int len;
            bool deallocate = get_cmd_and_stmt(pBuffer, &pCmd, &pStmt, &len);

            if (pStmt)
            {
                if (current_id != 0)
                {
                    MXS_NOTICE("Stmt %d(%s): %.*s", n, timestamp, len, pStmt);
                }
                else
                {
                    // We are in a context where we do not have a current session, so we need to
                    // log the session id ourselves.

                    MXS_NOTICE("(%" PRIu64 ") Stmt %d(%s): %.*s", current_id, n, timestamp, len, pStmt);
                }

                if (deallocate)
                {
                    MXS_FREE(pStmt);
                }
            }

            --n;
        }
    }
}

json_t* Session::queries_as_json() const
{
    json_t* pQueries = json_array();

    for (auto i = m_last_queries.rbegin(); i != m_last_queries.rend(); ++i)
    {
        const QueryInfo& info = *i;

        json_array_append_new(pQueries, info.as_json());
    }

    return pQueries;
}

json_t* Session::log_as_json() const
{
    json_t* pLog = json_array();

    for (const auto& i : m_log)
    {
        json_array_append_new(pLog, json_string(i.c_str()));
    }

    return pLog;
}

bool Session::add_variable(const char* name, session_variable_handler_t handler, void* context)
{
    bool added = false;

    static const char PREFIX[] = "@MAXSCALE.";

    if (strncasecmp(name, PREFIX, sizeof(PREFIX) - 1) == 0)
    {
        string key(name);

        std::transform(key.begin(), key.end(), key.begin(), tolower);

        if (m_variables.find(key) == m_variables.end())
        {
            SESSION_VARIABLE variable;
            variable.handler = handler;
            variable.context = context;

            m_variables.insert(std::make_pair(key, variable));
            added = true;
        }
        else
        {
            MXS_ERROR("Session variable '%s' has been added already.", name);
        }
    }
    else
    {
        MXS_ERROR("Session variable '%s' is not of the correct format.", name);
    }

    return added;
}

char* Session::set_variable_value(const char* name_begin,
                                  const char* name_end,
                                  const char* value_begin,
                                  const char* value_end)
{
    char* rv = NULL;

    string key(name_begin, name_end - name_begin);

    transform(key.begin(), key.end(), key.begin(), tolower);

    auto it = m_variables.find(key);

    if (it != m_variables.end())
    {
        rv = it->second.handler(it->second.context, key.c_str(), value_begin, value_end);
    }
    else
    {
        const char FORMAT[] = "Attempt to set unknown MaxScale user variable %.*s";

        int name_length = name_end - name_begin;
        int len = snprintf(NULL, 0, FORMAT, name_length, name_begin);

        rv = static_cast<char*>(MXS_MALLOC(len + 1));

        if (rv)
        {
            sprintf(rv, FORMAT, name_length, name_begin);
        }

        MXS_WARNING(FORMAT, name_length, name_begin);
    }

    return rv;
}

bool Session::remove_variable(const char* name, void** context)
{
    bool removed = false;
    string key(name);

    transform(key.begin(), key.end(), key.begin(), toupper);
    auto it = m_variables.find(key);

    if (it != m_variables.end())
    {
        if (context)
        {
            *context = it->second.context;
        }

        m_variables.erase(it);
        removed = true;
    }

    return removed;
}

void Session::retain_statement(GWBUF* pBuffer)
{
    if (m_retain_last_statements)
    {
        mxb_assert(m_last_queries.size() <= m_retain_last_statements);

        std::shared_ptr<GWBUF> sBuffer(gwbuf_clone(pBuffer), std::default_delete<GWBUF>());

        m_last_queries.push_front(QueryInfo(sBuffer));

        if (m_last_queries.size() > m_retain_last_statements)
        {
            m_last_queries.pop_back();
        }

        if (m_last_queries.size() == 1)
        {
            mxb_assert(m_current_query == -1);
            m_current_query = 0;
        }
        else
        {
            // If requests are streamed, without the response being waited for,
            // then this may cause the index to grow past the length of the array.
            // That's ok and is dealt with in book_server_response() and friends.
            ++m_current_query;
            mxb_assert(m_current_query >= 0);
        }
    }
}

void Session::book_server_response(SERVER* pServer, bool final_response)
{
    if (m_retain_last_statements && !m_last_queries.empty())
    {
        mxb_assert(m_current_query >= 0);
        // If enough queries have been sent by the client, without it waiting
        // for the responses, then at this point it may be so that the query
        // object has been popped from the size limited queue. That's apparent
        // by the index pointing past the end of the queue. In that case
        // we simply ignore the result.
        if (m_current_query < static_cast<int>(m_last_queries.size()))
        {
            auto i = m_last_queries.begin() + m_current_query;
            QueryInfo& info = *i;

            mxb_assert(!info.complete());

            info.book_server_response(pServer, final_response);
        }

        if (final_response)
        {
            // In case what is described in the comment above has occurred,
            // this will eventually take the index back into the queue.
            --m_current_query;
            mxb_assert(m_current_query >= -1);
        }
    }
}

void Session::book_last_as_complete()
{
    if (m_retain_last_statements && !m_last_queries.empty())
    {
        mxb_assert(m_current_query >= 0);
        // See comment in book_server_response().
        if (m_current_query < static_cast<int>(m_last_queries.size()))
        {
            auto i = m_last_queries.begin() + m_current_query;
            QueryInfo& info = *i;

            info.book_as_complete();
        }
    }
}

void Session::reset_server_bookkeeping()
{
    if (m_retain_last_statements && !m_last_queries.empty())
    {
        mxb_assert(m_current_query >= 0);
        // See comment in book_server_response().
        if (m_current_query < static_cast<int>(m_last_queries.size()))
        {
            auto i = m_last_queries.begin() + m_current_query;
            QueryInfo& info = *i;
            info.reset_server_bookkeeping();
        }
    }
}

Session::QueryInfo::QueryInfo(const std::shared_ptr<GWBUF>& sQuery)
    : m_sQuery(sQuery)
{
    clock_gettime(CLOCK_REALTIME_COARSE, &m_received);
    m_completed.tv_sec = 0;
    m_completed.tv_nsec = 0;
}

namespace
{

static const char ISO_TEMPLATE[] = "2018-11-05T16:47:49.123";
static const int ISO_TIME_LEN = sizeof(ISO_TEMPLATE) - 1;

void timespec_to_iso(char* zIso, const timespec& ts)
{
    tm tm;
    localtime_r(&ts.tv_sec, &tm);

    size_t i = strftime(zIso, ISO_TIME_LEN + 1, "%G-%m-%dT%H:%M:%S", &tm);
    mxb_assert(i == 19);
    long int ms = ts.tv_nsec / 1000000;
    i = sprintf(zIso + i, ".%03ld", ts.tv_nsec / 1000000);
    mxb_assert(i == 4);
}
}

json_t* Session::QueryInfo::as_json() const
{
    json_t* pQuery = json_object();

    const char* pCmd;
    char* pStmt;
    int len;
    bool deallocate = get_cmd_and_stmt(m_sQuery.get(), &pCmd, &pStmt, &len);

    if (pCmd)
    {
        json_object_set_new(pQuery, "command", json_string(pCmd));
    }

    if (pStmt)
    {
        json_object_set_new(pQuery, "statement", json_stringn(pStmt, len));

        if (deallocate)
        {
            MXS_FREE(pStmt);
        }
    }

    char iso_time[ISO_TIME_LEN + 1];

    timespec_to_iso(iso_time, m_received);
    json_object_set_new(pQuery, "received", json_stringn(iso_time, ISO_TIME_LEN));

    if (m_complete)
    {
        timespec_to_iso(iso_time, m_completed);
        json_object_set_new(pQuery, "completed", json_stringn(iso_time, ISO_TIME_LEN));
    }

    json_t* pResponses = json_array();

    for (auto& info : m_server_infos)
    {
        json_t* pResponse = json_object();

        // Calculate and report in milliseconds.
        long int received = m_received.tv_sec * 1000 + m_received.tv_nsec / 1000000;
        long int processed = info.processed.tv_sec * 1000 + info.processed.tv_nsec / 1000000;
        mxb_assert(processed >= received);

        long int duration = processed - received;

        json_object_set_new(pResponse, "server", json_string(info.pServer->name()));
        json_object_set_new(pResponse, "duration", json_integer(duration));

        json_array_append_new(pResponses, pResponse);
    }

    json_object_set_new(pQuery, "responses", pResponses);

    return pQuery;
}

void Session::QueryInfo::book_server_response(SERVER* pServer, bool final_response)
{
    // If the information has been completed, no more information may be provided.
    mxb_assert(!m_complete);
    // A particular server may be reported only exactly once.
    mxb_assert(find_if(m_server_infos.begin(), m_server_infos.end(), [pServer](const ServerInfo& info) {
                           return info.pServer == pServer;
                       }) == m_server_infos.end());

    timespec now;
    clock_gettime(CLOCK_REALTIME_COARSE, &now);

    m_server_infos.push_back(ServerInfo {pServer, now});

    m_complete = final_response;

    if (m_complete)
    {
        m_completed = now;
    }
}

void Session::QueryInfo::book_as_complete()
{
    timespec now;
    clock_gettime(CLOCK_REALTIME_COARSE, &m_completed);
    m_complete = true;
}

void Session::QueryInfo::reset_server_bookkeeping()
{
    m_server_infos.clear();
    m_completed.tv_sec = 0;
    m_completed.tv_nsec = 0;
    m_complete = false;
}

bool Session::start()
{
    bool rval = false;

    if (m_down->connect())
    {
        rval = true;
        m_state = MXS_SESSION::State::STARTED;
        mxb::atomic::add(&service->stats().n_connections, 1, mxb::atomic::RELAXED);
        mxb::atomic::add(&service->stats().n_current, 1, mxb::atomic::RELAXED);

        MXS_INFO("Started %s client session [%" PRIu64 "] for '%s' from %s",
                 service->name(), id(),
                 !m_user.empty() ? m_user.c_str() : "<no user>",
                 m_client_conn->dcb()->remote().c_str());
    }

    return rval;
}

void Session::close()
{
    m_state = State::STOPPING;
    m_down->close();
}

void Session::append_session_log(std::string log)
{
    m_log.push_front(log);

    if (m_log.size() >= this_unit.session_trace)
    {
        m_log.pop_back();
    }
}

void Session::dump_session_log()
{
    if (!(m_log.empty()))
    {
        std::string log;

        for (const auto& s : m_log)
        {
            log += s;
        }

        MXS_NOTICE("Session log for session (%" PRIu64 "): \n%s ", id(), log.c_str());
    }
}

int32_t Session::routeQuery(GWBUF* buffer)
{
    auto rv = m_down->routeQuery(buffer);

    if (response.buffer)
    {
        // Something interrupted the routing and queued a response
        deliver_response();
    }

    return rv;
}

int32_t Session::clientReply(GWBUF* buffer, mxs::ReplyRoute& down, const mxs::Reply& reply)
{
    return m_client_conn->write(gwbuf_clone(buffer));
}

bool Session::handleError(GWBUF* error, Endpoint* down, const mxs::Reply& reply)
{
    mxs::ReplyRoute route;
    clientReply(error, route, reply);
    terminate();
    return false;
}

mxs::ClientProtocol* Session::client_connection()
{
    return m_client_conn;
}

const mxs::ClientProtocol* Session::client_connection() const
{
    return m_client_conn;
}

void Session::set_client_connection(mxs::ClientProtocol* client_conn)
{
    m_client_conn = client_conn;
}

