/*
 * Copyright (c) 2018 MariaDB Corporation Ab
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

#include <atomic>
#include <string>
#include <memory>
#include <vector>

#include <maxbase/jansson.h>
#include <maxscale/protocol.hh>
#include <maxscale/ssl.hh>
#include <maxscale/service.hh>
#include <maxscale/routingworker.hh>

struct DCB;
class SERVICE;

class Listener;
using SListener = std::shared_ptr<Listener>;

/**
 * The Listener class is used to link a network port to a service. It defines the name of the
 * protocol module that should be loaded as well as the authenticator that is used.
 */
class Listener : public MXB_POLL_DATA
{
public:

    ~Listener();

    enum class Type
    {
        UNIX_SOCKET,    // UNIX domain socket shared between workers
        SHARED_TCP,     // TCP listening socket shared between workers
        UNIQUE_TCP,     // Unique TCP listening socket for each worker
        MAIN_WORKER,    // Listener that always moves the execution to the main worker
    };

    /**
     * Create a new listener
     *
     * @param name     Name of the listener
     * @param protocol Protocol module to use
     * @param params   Parameters for the listener
     *
     * @return New listener or nullptr on error
     */
    static SListener create(const std::string& name,
                            const std::string& protocol,
                            const MXS_CONFIG_PARAMETER& params);

    /**
     * Destroy a listener
     *
     * This removes the listener from the global list of active listeners. Once destroyed, the port used
     * by a listener is open for immediate reuse.
     *
     * @param listener Listener to destroy
     */
    static void destroy(const SListener& listener);

    /**
     * Start listening on the configured port
     *
     * @return True if the listener was able to start listening
     */
    bool listen();

    /**
     * Stop the listener
     *
     * @return True if the listener was successfully stopped
     */
    bool stop();

    /**
     * Start a stopped listener
     *
     * @return True if the listener was successfully started
     */
    bool start();

    /**
     * Listener name
     */
    const char* name() const;

    /**
     * Network address the listener listens on
     */
    const char* address() const;

    /**
     * Network port the listener listens on
     */
    uint16_t port() const;

    /**
     * Service the listener points to
     */
    SERVICE* service() const;

    /**
     * The authenticator module name
     */
    const char* authenticator() const;

    /**
     * The protocol module name
     */
    const char* protocol() const;

    /**
     * The protocol module entry points
     */
    const MXS_PROTOCOL& protocol_func() const;

    /**
     * The authenticator module entry points
     */
    const MXS_AUTHENTICATOR& auth_func() const;

    /**
     * The authenticator instance
     */
    void* auth_instance() const;

    /**
     * The state of the listener
     */
    const char* state() const;

    /**
     * Convert to JSON
     *
     * @return JSON representation of the object
     */
    json_t* to_json() const;

    /**
     * Load users for a listener
     *
     * @return The result from the authenticator module
     */
    int load_users();

    /**
     * Print the users into a DCB
     *
     * Note: not const due to authenticator API
     *
     * @param dcb DCB to write into
     */
    void print_users(DCB* dcb);

    Type type() const
    {
        return m_type;
    }

    /**
     * Mark authentication as failed
     *
     * This updates the number of failures that have occurred from this host. If the number of authentications
     * exceeds a certain value, any attempts to connect from the remote in quesion will be rejected.
     *
     * @param remote The address where the connection originated
     */
    void mark_auth_as_failed(const std::string& remote);

    // Functions that are temporarily public
    bool          create_listener_config(const char* filename);
    struct users* users() const;
    void          set_users(struct users* u);

    const mxs::SSLProvider& ssl() const
    {
        return m_ssl_provider;
    }

    mxs::SSLProvider& ssl()
    {
        return m_ssl_provider;
    }

private:
    enum State
    {
        CREATED,
        STARTED,
        STOPPED,
        FAILED,
        DESTROYED
    };

    std::string m_name;             /**< Name of the listener */
    State       m_state;            /**< Listener state */
    std::string m_protocol;         /**< Protocol module to load */
    uint16_t    m_port;             /**< Port to listen on */
    std::string m_address;          /**< Address to listen with */
    std::string m_authenticator;    /**< Name of authenticator */
    std::string m_auth_options;     /**< Authenticator options */
    void*       m_auth_instance;    /**< Authenticator instance */

    struct users*        m_users;           /**< The user data for this listener */
    SERVICE*             m_service;         /**< The service which used by this listener */
    std::atomic<bool>    m_active;          /**< True if the port has not been deleted */
    MXS_PROTOCOL         m_proto_func;      /**< Preloaded protocol functions */
    MXS_AUTHENTICATOR    m_auth_func;       /**< Preloaded authenticator functions */
    MXS_CONFIG_PARAMETER m_params;          /**< Configuration parameters */
    mxs::SSLProvider     m_ssl_provider;

    Type m_type;    /**< The type of the listener */

    mxs::rworker_local<int> m_fd {-1};      /**< File descriptor the listener listens on */

    /** A shared pointer to the listener itself that is passed as the argument to
     * the protocol's accept function. This allows client connections to live
     * longer than the listener they started on.
     *
     * This will eventually be replaced with a shared_ptr of the authenticator instance as that is
     * what is actually required by the client sessions.
     *
     * In practice as a service must outlive all sessions on it, the reference could be owned by the service
     * instead of each individual client. This would remove the need to increment the listener reference
     * count every time a client is accepted.
     */
    SListener m_self;

    /**
     * Creates a new listener that points to a service
     *
     * @param service       Service where the listener points to
     * @param name          Name of the listener
     * @param address       The address where the listener listens
     * @param port          The port on which the listener listens
     * @param protocol      The protocol module to use
     * @param authenticator The authenticator module to use
     * @param auth_opts     Options for the authenticator
     * @param auth_instance The authenticator instance
     * @param ssl           The SSL configuration
     */
    Listener(SERVICE* service, const std::string& name, const std::string& address, uint16_t port,
             const std::string& protocol, const std::string& authenticator,
             const std::string& auth_opts, void* auth_instance, std::unique_ptr<mxs::SSLContext> ssl,
             const MXS_CONFIG_PARAMETER& params);

    /**
     * Listen on a file descriptor shared between all workers
     *
     * @return True if the listening was started successfully
     */
    bool listen_shared();

    /**
     * Listen with a unique file descriptor for each worker
     *
     * @return True if the listening was started successfully
     */
    bool listen_unique();

    /**
     * Close all opened file descriptors for this listener
     */
    void close_all_fds();

    /**
     * Accept a single client connection
     *
     * @param fd The opened file descriptor to which the client is connected to
     * @param addr The network information
     * @param host The host where the client is connecting from
     *
     * @return The new DCB or nullptr on error
     */
    DCB* accept_one_dcb(int fd, const sockaddr_storage* addr, const char* host);

    /**
     * Accept all available client connections
     */
    void accept_connections();

    /**
     * Reject a client connection
     *
     * Writes an error message to the fd if the protocol supports it and then closes it.
     *
     * @param fd   The file descriptor to close
     * @param host The host where the connection originated from
     */
    void reject_connection(int fd, const char* host);

    /**
     * The file descriptor for accepting new connections
     *
     * @return The worker-local file descriptor
     */
    int fd() const
    {
        return *m_fd;
    }

    // Handler for EPOLL_IN events
    static uint32_t poll_handler(MXB_POLL_DATA* data, MXB_WORKER* worker, uint32_t events);
};

/**
 * @brief Serialize a listener to a file
 *
 * This converts @c listener into an INI format file. This allows created listeners
 * to be persisted to disk. This will replace any existing files with the same
 * name.
 *
 * @param listener Listener to serialize
 * @return True if the serialization of the listener was successful, false if it fails
 */
bool listener_serialize(const SListener& listener);

/**
 * Find a listener
 *
 * @param name Name of the listener
 *
 * @return The listener if it exists or an empty SListener if it doesn't
 */
SListener listener_find(const std::string& name);

/**
 * Find all listeners that point to a service
 *
 * @param service Service whose listeners are returned
 *
 * @return The listeners that point to the service
 */
std::vector<SListener> listener_find_by_service(const SERVICE* service);

/**
 * Find listener by socket
 *
 * @param socket  Path to a socket file
 *
 * @return The matching listener if one was found
 */
SListener listener_find_by_socket(const std::string& socket);

/**
 * Find listener by address and port
 *
 * @param address Network address
 * @param port    Network port
 *
 * @return The matching listener if one was found
 */
SListener listener_find_by_address(const std::string& address, unsigned short port);
