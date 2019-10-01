/*
 * Copyright (c) 2019 MariaDB Corporation Ab
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
#pragma once

#include <maxscale/ccdefs.hh>
#include <maxscale/protocol.hh>

class BackendDCB;
class ClientDCB;
class DCB;
class SERVICE;

namespace maxscale
{
class ClientProtocol;
class BackendProtocol;

class ProtocolModule
{
public:
    /**
     * Allocate new client protocol session
     *
     * @param session   The session to which the connection belongs to
     * @param component The component to use for routeQuery
     *
     * @return New protocol session or null on error
     */
    virtual std::unique_ptr<mxs::ClientProtocol>
    create_client_protocol(MXS_SESSION* session, mxs::Component* component) = 0;

    /**
     * Get the default authenticator for the protocol.
     *
     * @return The default authenticator for the protocol or empty if the protocol
     * does not provide one
     */
    virtual std::string auth_default() const = 0;

    /**
     * Get rejection message. The protocol should return an error indicating that access to MaxScale
     * has been temporarily suspended.
     *
     * @param host The host that is blocked
     * @return A buffer containing the error message
     */
    virtual GWBUF* reject(const std::string& host)
    {
        return nullptr;
    }

    /**
     * Get protocol module name.
     *
     * @return Module name
     */
    virtual std::string name() const = 0;

    /**
     * Load users for all authenticators.
     *
     * @param service The service to load from
     * @return MXS_AUTH_LOADUSERS_OK on success
     */
    virtual int load_auth_users(SERVICE* service) = 0;

    /**
     * Print a list of authenticator users to DCB.
     *
     * @param output Output DCB
     */
    virtual void print_auth_users(DCB* output) = 0;

    /**
     * Print a list of authenticator users to json.
     *
     * @return JSON user list
     */
    virtual json_t* print_auth_users_json() = 0;
};

/**
 * Client protocol class
 */
class ClientProtocol : public MXS_PROTOCOL_SESSION
{
public:
    enum Capabilities
    {
        CAP_BACKEND = (1 << 0)      // The protocol supports backend communication
    };

    virtual ~ClientProtocol() = default;

    /**
     * Initialize a connection.
     *
     * @return True, if the connection could be initialized, false otherwise.
     */
    virtual bool init_connection() = 0;

    /**
     * Finalize a connection. Called right before the DCB itself is closed.
     */
    virtual void finish_connection() = 0;

    /**
     * Handle connection limits. Currently the return value is ignored.
     *
     * @param limit Maximum number of connections
     * @return 1 on success, 0 on error
     */
    virtual int32_t connlimit(int limit)
    {
        return 0;
    }

    virtual int64_t capabilities() const
    {
        return 0;
    }

    /**
     * Allocate new backend protocol session
     *
     * @param session  The session to which the connection belongs to
     * @param server   Server where the connection is made
     *
     * @return New protocol session or null on error
     */
    virtual std::unique_ptr<BackendProtocol>
    create_backend_protocol(MXS_SESSION* session, SERVER* server, mxs::Component* component)
    {
        mxb_assert(!true);
        return nullptr;
    }

    /**
     * Return current database. Only required by query classifier.
     *
     * @return Current database
     */
    virtual std::string current_db() const
    {
        return "";
    }

    virtual ClientDCB* dcb() = 0;
    virtual const ClientDCB* dcb() const = 0;
};

/**
 * Partial client protocol implementation. More fields and functions may follow.
 */
class ClientProtocolBase : public ClientProtocol
{
public:
    void set_dcb(DCB* dcb) override;
    ClientDCB* dcb() override;
    const ClientDCB* dcb() const override;
protected:
    ClientDCB* m_dcb {nullptr};     /**< Dcb used by this protocol connection */
};

/**
 * Backend protocol class
 */
class BackendProtocol : public MXS_PROTOCOL_SESSION
{
public:
    virtual ~BackendProtocol() = default;

    /**
     * Initialize a connection.
     *
     * @return True, if the connection could be initialized, false otherwise.
     */
    virtual bool init_connection() = 0;

    /**
     * Finalize a connection. Called right before the DCB itself is closed.
     */
    virtual void finish_connection() = 0;

    /**
     * Reuse a connection. The connection was in the persistent pool
     * and will now be taken into use again.
     *
     * @param dcb               The connection to be reused.
     * @param upstream          The upstream component.
     * @param client_protocol   The new client protocol this backend protocol should be associated with
     *
     * @return True, if the connection can be reused, false otherwise.
     *         If @c false is returned, the @c dcb should be closed.
     */
    virtual bool
    reuse_connection(BackendDCB* dcb, mxs::Component* upstream, mxs::ClientProtocol* client_protocol) = 0;

    /**
     * Check if the connection has been fully established, used by connection pooling
     *
     * @return True if the connection is fully established and can be pooled
     */
    virtual bool established() = 0;

    virtual const BackendDCB* dcb() const = 0;
};

template<class ProtocolImplementation>
class ProtocolApiGenerator
{
public:
    ProtocolApiGenerator() = delete;
    ProtocolApiGenerator(const ProtocolApiGenerator&) = delete;
    ProtocolApiGenerator& operator=(const ProtocolApiGenerator&) = delete;

    static mxs::ProtocolModule* create_protocol_module(const std::string& auth_name,
                                                       const std::string& auth_opts)
    {
        return ProtocolImplementation::create(auth_name, auth_opts);
    }

    static MXS_PROTOCOL_API s_api;
};

template<class ProtocolImplementation>
MXS_PROTOCOL_API ProtocolApiGenerator<ProtocolImplementation>::s_api =
{
    &ProtocolApiGenerator<ProtocolImplementation>::create_protocol_module,
};
}
