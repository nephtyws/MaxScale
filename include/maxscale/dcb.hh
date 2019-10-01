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

/**
 * @file dcb.h  The Descriptor Control Block
 */

#include <maxscale/ccdefs.hh>

#include <openssl/ssl.h>
#include <netinet/in.h>

#include <maxbase/poll.h>
#include <maxscale/authenticator.hh>
#include <maxscale/buffer.hh>
#include <maxscale/dcbhandler.hh>
#include <maxscale/modinfo.hh>
#include <maxscale/protocol2.hh>
#include <maxscale/target.hh>

#include <memory>

class MXS_PROTOCOL_SESSION;
class MXS_SESSION;
class SERVER;
class SERVICE;

namespace maxscale
{
class ClientProtocol;
class BackendProtocol;
class SSLContext;
}

/**
 * Descriptor Control Block
 *
 * A wrapper for a socket descriptor within MaxScale. For each client
 * session there will be one ClientDCB and several BackendDCBs.
 */
class DCB : public MXB_POLL_DATA
{
public:
    static const int FD_CLOSED = -1;

    using Handler = DCBHandler;

    class Manager
    {
    public:
        /**
         * Called by DCB when created.
         */
        virtual void add(DCB* dcb) = 0;

        /**
         * Called by DCB when destroyed.
         */
        virtual void remove(DCB* dcb) = 0;

        /**
         * Called by DCB when it needs to be destroyed.
         */
        virtual void destroy(DCB* dcb) = 0;

    protected:
        static void call_destroy(DCB* dcb)
        {
            dcb->destroy();
        }
    };

    struct Stats
    {
        int n_reads = 0;        /*< Number of reads on this descriptor */
        int n_writes = 0;       /*< Number of writes on this descriptor */
        int n_accepts = 0;      /*< Number of accepts on this descriptor */
        int n_buffered = 0;     /*< Number of buffered writes */
        int n_high_water = 0;   /*< Number of crosses of high water mark */
        int n_low_water = 0;    /*< Number of crosses of low water mark */
    };

    enum class Role
    {
        CLIENT,         /*< Serves dedicated client */
        BACKEND,        /*< Serves back end connection */
        INTERNAL        /*< Internal DCB not connected to the outside */
    };

    enum class State
    {
        CREATED,        /*< Created but not added to the poll instance */
        POLLING,        /*< Added to the poll instance */
        DISCONNECTED,   /*< Socket closed */
        NOPOLLING       /*< Removed from the poll instance */
    };

    enum class Reason
    {
        DRAINED,        /*< The write delay queue has drained */
        HIGH_WATER,     /*< Cross high water mark */
        LOW_WATER       /*< Cross low water mark */
    };

    enum class SSLState
    {
        HANDSHAKE_UNKNOWN,   /*< The DCB has unknown SSL status */
        HANDSHAKE_REQUIRED,  /*< SSL handshake is needed */
        HANDSHAKE_DONE,      /*< The SSL handshake completed OK */
        ESTABLISHED,         /*< The SSL connection is in use */
        HANDSHAKE_FAILED     /*< The SSL handshake failed */
    };

    /**
     * Close the DCB. The called should treat the DCB as having been
     * deleted although in practice the actual deletion may take place
     * at a later point in time.
     *
     * @param dcb  The dcb to be closed.
     */
    static void close(DCB* dcb);

    /**
     * @return The unique identifier of the DCB.
     */
    uint64_t uid() const
    {
        return m_uid;
    }

    /**
     * File descriptor of DCB.
     *
     * Accessing and using the file descriptor directly should only be
     * used as a last resort, as external usage may break the assumptions
     * of the DCB.
     *
     * @return The file descriptor.
     */
    int fd() const
    {
        return m_fd;
    }

    /**
     * @return The remote host of the DCB.
     */
    const std::string& remote() const
    {
        return m_remote;
    }

    /**
     * @return The role of the DCB.
     */
    Role role() const
    {
        return m_role;
    }

    /**
     * @return The session of the DCB.
     */
    MXS_SESSION* session() const
    {
        return m_session;
    }

    /**
     * @return The event handler of the DCB.
     */
    Handler* handler() const
    {
        return m_handler;
    }

    /**
     * Set the handler of the DCB.
     *
     * @param handler  The new handler of the DCB.
     */
    void set_handler(Handler* handler)
    {
        m_handler = handler;
    }

    /**
     * @return The state of the DCB.
     */
    State state() const
    {
        return m_state;
    }

    /**
     * @return The protocol of the DCB.
     */
    virtual MXS_PROTOCOL_SESSION* protocol() const = 0;

    /**
     * Clears the DCB; all queues and callbacks are freed and the session
     * pointer is set to null.
     */
    void clear();

    virtual std::string diagnostics() const;

    virtual json_t* to_json() const;

    SERVICE* service() const;

    /**
     * @return DCB statistics.
     */
    const Stats& stats() const
    {
        return m_stats;
    }

    /**
     * @return True, if SSL has been enabled, false otherwise.
     */
    bool ssl_enabled() const
    {
        return m_encryption.handle != nullptr;
    }

    /**
     * @return The current SSL state.
     */
    SSLState ssl_state() const
    {
        return m_encryption.state;
    }

    void set_ssl_state(SSLState ssl_state)
    {
        m_encryption.state = ssl_state;
    }

    /**
     * Perform SSL handshake.
     *
     * @return -1 if an error occurred,
     *          0 if the handshaking is still ongoing and another call to ssl_handshake() is needed, and
     *          1 if the handshaking succeeded
     *
     * @see ClientDCB::ssl_handshake
     * @see BackendDCB::ssl_handshake
     */
    virtual int ssl_handshake() = 0;

    /**
     * Find the number of bytes available on the socket.
     *
     * @return -1 in case of error, otherwise the total number of bytes available.
     */
    int socket_bytes_readable() const;

    /**
     * Read data from the DCB.
     *
     * @param ppHead    Pointer to pointer to GWBUF to append to. The GWBUF pointed to
     *                  may be NULL in which case it will be non-NULL after a successful read.
     * @param maxbytes  Maximum amount of bytes to read, 0 means no limit.
     *
     * @return -1 on error, otherwise the total length of the GWBUF. That is, not only
     *         the amount of data appended to the GWBUF.
     *
     * @note The read operation will return data from the readq and the network.
     */
    int read(GWBUF** ppHead, int maxbytes);

    /**
     * Write data to the DCB.
     *
     * @param pData  The GWBUF to write.
     * @param drain  Whether the writeq should be drained or not.
     *
     * @return False on failure, true on success.
     */
    enum class Drain
    {
        YES, // Drain the writeq.
        NO   // Do not drain the writeq.
    };

    /**
     * Append data to the write queue.
     *
     * @param data   The data to be appended to the write queue.
     * @param drain  Whether the write queue should be drained, that is,
     *               written to the socket.
     *
     * @return True if the data could be appended, false otherwise.
     */
    bool writeq_append(GWBUF* data, Drain drain = Drain::YES);

    /**
     * Drain the write queue of the DCB.
     *
     * This is called as part of the EPOLLOUT handling of a socket and will try
     * to send any buffered data from the write queue up until the point the
     * write would block.
     *
     * @return The number of bytes written.
     */
    int writeq_drain();

    int32_t protocol_write(GWBUF* pData);

    // TODO: Should probably be made protected.
    virtual void shutdown() = 0;

    /**
     * Adds the DCB to the epoll set of the current worker, which in practice
     * means that the DCB will receive I/O events related to its file descriptor,
     * and that corresponding handlers will be called.
     *
     * NOTE: The current worker *must* be the owner of the DCB.
     *
     * @return True on success, false on error.
     */
    virtual bool enable_events();

    /**
     * Remove the DCB from the epoll set of the current worker, which in practice
     * means that the DCB will no longer receive I/O events related to its file
     * descriptor and that corresponding handlers will no longer be called.
     *
     * NOTE: The current worker *must* be the owner of the DCB.
     *
     * @return True on success, false on error.
     */
    virtual bool disable_events();

    /**
     * Add a callback to the DCB.
     *
     * @reason     When the callback should be called.
     * @cb         The callback.
     * @user_data  The data to provide to the callback when called.
     *
     * @return True, if the callback was added, false otherwise. False will
     *         be returned if the callback could not be added or if the callback
     *         has been added already.
     */
    bool add_callback(Reason reason, int (*cb)(DCB*, Reason, void*), void* user_data);

    /**
     * Remove a callback from the DCB.
     *
     * @reason     The reason provided when the callback was added
     * @cb         The callback provided when the callback was added.
     * @user_data  The user_data provided when the callback was added.
     *
     * @return True, if the callback could be removed, false if the callback
     *         was not amongst the added ones.
     */
    bool remove_callback(Reason reason, int (*cb)(DCB*, Reason, void*), void* user_data);

    /**
     * Remove all callbacks
     */
    void remove_callbacks();

    static void process_timeouts(int thr);

    inline GWBUF* writeq()
    {
        return m_writeq;
    }

    /**
     * @brief Returns the read queue of the DCB.
     *
     * @note The read queue remains the property of the DCB.
     *
     * @return A buffer of NULL if there is no read queue.
     */
    inline GWBUF* readq()
    {
        return m_readq;
    }

    /**
     * @brief Append a buffer the DCB's readqueue
     *
     * Usually data is stored into the DCB's readqueue when not enough data is
     * available and the processing needs to be deferred until more data is available.
     *
     * @param buffer The buffer to append.
     */
    void readq_append(GWBUF* buffer)
    {
        m_readq = gwbuf_append(m_readq, buffer);
    }

    /**
     * @brief Prepend a buffer the DCB's readqueue
     *
     * @param buffer The buffer to prepend
     */
    void readq_prepend(GWBUF* buffer)
    {
        m_readq = m_readq ? gwbuf_append(buffer, m_readq) : buffer;
    }

    /**
     * @brief Returns the read queue of the DCB and sets the read queue to NULL.
     *
     * @note The read queue becomes the property of the caller.
     *
     * @return A buffer of NULL if there is no read queue.
     */
    GWBUF* readq_release()
    {
        GWBUF* readq = m_readq;
        m_readq = NULL;
        return readq;
    }

    /**
     * @brief Set read queue of a DCB
     *
     * The expectation is that there is no readqueue when this is done.
     * The ownership of the provided buffer moved to the DCB.
     *
     * @param buffer The buffer to reset with
     */
    void readq_set(GWBUF* buffer)
    {
        mxb_assert(!m_readq);
        if (m_readq)
        {
            MXS_ERROR("Read-queue set when there already is a read-queue.");
            // TODO: Conceptually this should be freed here. However, currently
            // TODO: the code just assigns without checking, so we do the same
            // TODO: for now. If this is not set to NULL when it has been consumed,
            // TODO: we would get a double free.
            // TODO: gwbuf_free(m_readq);
        }
        m_readq = buffer;
    }

    GWBUF* delayq()
    {
        return m_delayq;
    }

    void delayq_append(GWBUF* buffer)
    {
        m_delayq = gwbuf_append(m_delayq, buffer);
    }

    /**
     * @brief Returns the read queue of the DCB and sets the read queue to NULL.
     *
     * @note The read queue becomes the property of the caller.
     *
     * @return A buffer of NULL if there is no read queue.
     */
    GWBUF* delayq_release()
    {
        GWBUF* delayq = m_delayq;
        m_delayq = NULL;
        return delayq;
    }

    int64_t last_read() const
    {
        return m_last_read;
    }

    int64_t last_write() const
    {
        return m_last_write;
    }

    bool is_closed() const
    {
        return m_nClose != 0;
    }

    bool hanged_up() const
    {
        return m_hanged_up;
    }

    /**
     * Will cause an EPOLL[R]HUP event to be delivered when the current
     * event handling finishes, just before the the control returns
     * back to epoll_wait().
     *
     * @note During one callback, only one event can be triggered.
     *       If there are multiple trigger_...()-calls, only the
     *       last one will be honoured.
     */
    void trigger_hangup_event();

    /**
     * Will cause an EPOLLIN event to be delivered when the current
     * event handling finishes, just before the the control returns
     * back to epoll_wait().
     *
     * @note During one callback, only one event can be triggered.
     *       If there are multiple trigger_...()-calls, only the
     *       last one will be honoured.
     */
    void trigger_read_event();

    /**
     * Will cause an EPOLLOUT event to be delivered when the current
     * event handling finishes, just before the the control returns
     * back to epoll_wait().
     *
     * @note During one callback, only one event can be triggered.
     *       If there are multiple trigger_...()-calls, only the
     *       last one will be honoured.
     */
    void trigger_write_event();

    struct CALLBACK
    {
        Reason           reason;    /*< The reason for the callback */
        int              (* cb)(DCB* dcb, Reason reason, void* userdata);
        void*            userdata;  /*< User data to be sent in the callback */
        struct CALLBACK* next;      /*< Next callback for this DCB */
    };

protected:
    DCB(int fd,
        const std::string& remote,
        Role role,
        MXS_SESSION* session,
        Handler* handler,
        Manager* manager);

    virtual ~DCB();

    bool create_SSL(mxs::SSLContext* ssl);

    static void destroy(DCB* dcb)
    {
        dcb->destroy();
    }

    /**
     * Release the instance from the associated session.
     *
     * @param session The session to release the DCB from.
     *
     * @return True, if the DCB was released and can be deleted, false otherwise.
     */
    virtual bool release_from(MXS_SESSION* session) = 0;

    /**
     * Prepare the instance for destruction.
     *
     * @return True if it was prepared and can be destroyed, false otherwise.
     */
    virtual bool prepare_for_destruction() = 0;

    void stop_polling_and_shutdown();

    int log_errors_SSL(int ret);

    struct Encryption
    {
        SSL*     handle = nullptr;                    /**< SSL handle for connection */
        SSLState state = SSLState::HANDSHAKE_UNKNOWN; /**< Current state of SSL if in use */
        bool     read_want_read = false;
        bool     read_want_write = false;
        bool     write_want_read = false;
        bool     write_want_write = false;
    };

    const uint64_t    m_uid;                        /**< Unique identifier for this DCB */
    int               m_fd;                         /**< The descriptor */
    const std::string m_remote;                     /**< The remote host */
    const Role        m_role;                       /**< The role of the DCB */
    MXS_SESSION*      m_session;                    /**< The owning session */
    Handler*          m_handler;                    /**< The event handler of the DCB */
    Manager*          m_manager;                    /**< The DCB manager to use */
    int64_t           m_last_read;                  /**< Last time the DCB received data */
    int64_t           m_last_write;                 /**< Last time the DCB sent data */
    const uint64_t    m_high_water;                 /**< High water mark of write queue */
    const uint64_t    m_low_water;                  /**< Low water mark of write queue */
    State             m_state = State::CREATED;     /**< Current state */
    Encryption        m_encryption;                 /**< Encryption state */
    Stats             m_stats;                      /**< DCB related statistics */
    CALLBACK*         m_callbacks = nullptr;        /**< The list of callbacks for the DCB */
    bool              m_high_water_reached = false; /**< High water mark reached, to determine
                                                         * whether we need to release throttle */
    uint64_t          m_writeqlen = 0;              /**< Bytes in writeq */
    GWBUF*            m_writeq = nullptr;           /**< Write Data Queue */
    GWBUF*            m_readq = nullptr;            /**< Read queue for incomplete reads */
    GWBUF*            m_delayq = nullptr;           /**< Delay Backend Write Data Queue */
    uint32_t          m_triggered_event = 0;        /**< Triggered event to be delivered to handler */
    uint32_t          m_nClose = 0;                 /**< How many times dcb_close has been called. */
    bool              m_hanged_up = false;          /**< Has thethis can be called only once */

private:
    friend class Manager;

    GWBUF* basic_read(int bytesavailable, int maxbytes, int nreadtotal, int* nsingleread);

    int    read_SSL(GWBUF** head);
    GWBUF* basic_read_SSL(int* nsingleread);

    int socket_write_SSL(GWBUF* writeq, bool* stop_writing);
    int socket_write(GWBUF* writeq, bool* stop_writing);

    void        destroy();
    static void free(DCB* dcb);

    static uint32_t poll_handler(MXB_POLL_DATA* data, MXB_WORKER* worker, uint32_t events);
    static uint32_t event_handler(DCB* dcb, uint32_t events);
    uint32_t process_events(uint32_t events);

    class FakeEventTask;
    friend class FakeEventTask;

    void call_callback(Reason reason);

    void add_event(uint32_t ev);
};

class ClientDCB : public DCB
{
public:
    ~ClientDCB() override;

    static ClientDCB*
    create(int fd,
           const std::string& remote,
           const sockaddr_storage& ip,
           MXS_SESSION* session,
           std::unique_ptr<mxs::ClientProtocol> protocol,
           DCB::Manager* manager = nullptr);

    const sockaddr_storage& ip() const
    {
        return m_ip;
    }

    /**
     * @brief Return the port number this DCB is connected to
     *
     * @return Port number the DCB is connected to or -1 if information is not available
     */
    int port() const;

    mxs::ClientProtocol* protocol() const override;

    /**
     * Accept an SSL connection and perform the SSL authentication handshake.
     *
     * @return -1 if an error occurred,
     *          0 if the handshaking is still ongoing and another call to ssl_handshake() is needed, and
     *          1 if the handshaking succeeded
     */
    int ssl_handshake() override;

    void shutdown() override;

protected:
    // Only for InternalDCB.
    ClientDCB(int fd,
              const std::string& remote,
              const sockaddr_storage& ip,
              DCB::Role role,
              MXS_SESSION* session,
              std::unique_ptr<mxs::ClientProtocol> protocol,
              Manager* manager);

    // Only for Mock DCB.
    ClientDCB(int fd, const std::string& remote, DCB::Role role, MXS_SESSION* session);

private:
    ClientDCB(int fd,
              const std::string& remote,
              const sockaddr_storage& ip,
              MXS_SESSION* session,
              std::unique_ptr<mxs::ClientProtocol> protocol,
              DCB::Manager* manager);

    bool release_from(MXS_SESSION* session) override;
    bool prepare_for_destruction() override;

    sockaddr_storage                          m_ip;             /**< remote IPv4/IPv6 address */
    std::unique_ptr<mxs::ClientProtocol>      m_protocol;       /**< The protocol session */
};

class BackendDCB : public DCB
{
public:
    class Manager : public DCB::Manager
    {
    public:
        /**
         * Called by BackendDCB when it is about to be destroyed.
         *
         * @param dcb  The dcb about to be destroyed.
         *
         * @return True, if the dcb can be destroyed, false otherwise.
         *
         * If @c false is returned, the state of the DCB will be
         * changed to what it would be if @close() had never been
         * called.
         */
        virtual bool can_be_destroyed(BackendDCB* dcb) = 0;
    };

    static BackendDCB* connect(SERVER* server,
                               MXS_SESSION* session,
                               BackendDCB::Manager* manager,
                               mxs::Component* upstream);

    /**
     * Resets the BackendDCB so that it can be reused.
     *
     * @param session  The new session for the DCB.
     */
    void reset(MXS_SESSION* session);

    mxs::BackendProtocol* protocol() const override;

    /**
     * Hangup all BackendDCBs connected to a particular server.
     *
     * @param server  BackendDCBs connected to this server should be closed.
     */
    static void hangup(const SERVER* server);
    void shutdown() override;
    std::string diagnostics() const override;

    json_t* to_json() const override;

    SERVER* server() const
    {
        return m_server;
    }

    /**
     * Initate an SSL handshake with a server.
     *
     * @return -1 if an error occurred,
     *          0 if the handshaking is still ongoing and another call to ssl_handshake() is needed, and
     *          1 if the handshaking succeeded
     */
    int ssl_handshake() override;

private:
    BackendDCB(SERVER* server, int fd, MXS_SESSION* session,
               std::unique_ptr<mxs::BackendProtocol> protocol,
               DCB::Manager* manager);

    static BackendDCB* create(SERVER* server,
                              int fd,
                              MXS_SESSION* session,
                              DCB::Manager* manager,
                              mxs::Component* upstream);

    bool release_from(MXS_SESSION* session) override;
    bool prepare_for_destruction() override;

    static void hangup_cb(MXB_WORKER* worker, const SERVER* server);


    SERVER* const                         m_server;   /**< The associated backend server */
    std::unique_ptr<mxs::BackendProtocol> m_protocol; /**< The protocol session */
};

namespace maxscale
{

const char* to_string(DCB::Role role);
const char* to_string(DCB::State state);
}

/**
 * Debug printing all DCBs from within a debugger.
 */
void printAllDCBs();

/**
 * Debug printing a DCB from within a debugger.
 *
 * @param dcb   The DCB to print
 */
void printDCB(DCB*);

/**
 * A  DCB based wrapper for printf. Allows formatting printing to
 * a descriptor control block.
 *
 * @param dcb   Descriptor to write to
 * @param fmt   A printf format string
 * @param ...   Variable arguments for the print format
 */
void dcb_printf(DCB*, const char*, ...) __attribute__ ((format(printf, 2, 3)));

/**
 * Return DCB counts filtered by role
 *
 * @param role   What kind of DCBs should be counted.
 *
 * @return  Count of DCBs in the specified role.
 */
int dcb_count_by_role(DCB::Role role);

uint64_t dcb_get_session_id(DCB* dcb);

/**
 * @brief Call a function for each connected DCB
 *
 * @deprecated You should not use this function, use dcb_foreach_parallel instead
 *
 * @warning This must only be called from the main thread, otherwise deadlocks occur
 *
 * @param func Function to call. The function should return @c true to continue iteration
 * and @c false to stop iteration earlier. The first parameter is a DCB and the second
 * is the value of @c data that the user provided.
 * @param data User provided data passed as the second parameter to @c func
 * @return True if all DCBs were iterated, false if the callback returned false
 */
bool dcb_foreach(bool (* func)(DCB* dcb, void* data), void* data);

/**
 * @brief Call a function for each connected DCB on the current worker
 *
 * @param func Function to call. The function should return @c true to continue
 *             iteration and @c false to stop iteration earlier. The first parameter
 *             is the current DCB.
 *
 * @param data User provided data passed as the second parameter to @c func
 */
void dcb_foreach_local(bool (* func)(DCB* dcb, void* data), void* data);

/**
 * @brief Return the DCB currently being handled by the calling thread.
 *
 * @return A DCB, or NULL if the calling thread is not currently handling
 *         a DCB or if the calling thread is not a polling/worker thread.
 */
DCB* dcb_get_current();
