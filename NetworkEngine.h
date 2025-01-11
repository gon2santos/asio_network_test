#pragma once

#include <memory>
#include <pthread.h>
#include <deque>
#include <optional>
#include <vector>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstdint>

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include "Asio/asio.hpp"
#include "Asio/asio/ts/buffer.hpp"
#include "Asio/asio/ts/internet.hpp"

namespace Net
{
    template <typename T>
    struct message_header
    {
        T id{};
        uint32_t size = 0;
    };

    template <typename T>
    struct message
    {
        message_header<T> header{};
        std::vector<uint8_t> body;

        // returns size of entire message packet in bytes
        size_t size() const
        {
            return sizeof(message_header<T>) + body.size();
        }

        // Override for std::cout compatibility - produces friendly description of message
        friend std::ostream &operator<<(std::ostream &os, const message<T> &msg)
        {
            os << "ID:" << int(msg.header.id) << " Size:" << msg.header.size;
            return os;
        }

        // pushes any POD-like data into the message buffer
        template <typename DataType>
        friend message<T> &operator<<(message<T> &msg, const DataType &data)
        {
            // check that the type of the data is trivially copyable
            static_assert(std::is_standard_layout<DataType>::value, "Data is too complex to be pushed into vector");
            // cache current size of the vector, as this will be the point we insert the data
            size_t i = msg.body.size();
            // resize the vector by the size of the data being pushed
            msg.body.resize(msg.body.size() + sizeof(DataType));
            // copy the data into the newly allocated vector space
            std::memcpy(msg.body.data() + i, &data, sizeof(DataType));

            msg.header.size = msg.body.size();

            return msg;
        }

        // extracts any POD-like data from the message buffer
        template <typename DataType>
        friend message<T> &operator>>(message<T> &msg, DataType &data)
        {
            // check that the type of the data is trivially copyable
            static_assert(std::is_standard_layout<DataType>::value, "Data is too complex to be pushed into vector");
            // cache current size of the vector, as this will be the point we insert the data
            size_t i = msg.body.size() - sizeof(DataType);
            // copy the data from the vector into the user variable
            std::memcpy(&data, msg.body.data() + i, sizeof(DataType));
            // shrink the vector to remove read bytes, and reset end position
            msg.body.resize(i);

            msg.header.size = msg.body.size();

            return msg;
        }
    };

    // Forward declare the connection
    template <typename T>
    class connection;

    template <typename T>
    struct owned_message
    {
        std::shared_ptr<connection<T>> remote = nullptr;
        message<T> msg;

        // Again, a friendly string maker
        friend std::ostream &operator<<(std::ostream &os, const owned_message<T> &msg)
        {
            os << msg.msg;
            return os;
        }
    };

    template <typename T>
    class tsqueue
    {
    public:
        tsqueue()
        {
            pthread_mutex_init(&muxQueue, nullptr);
            pthread_mutex_init(&muxBlocking, nullptr);
            pthread_cond_init(&cvBlocking, nullptr);
        }

        tsqueue(const tsqueue<T> &) = delete;

        virtual ~tsqueue()
        {
            clear();
            pthread_mutex_destroy(&muxQueue);
            pthread_mutex_destroy(&muxBlocking);
            pthread_cond_destroy(&cvBlocking);
        }

    public:
        // Returns and maintains item at front of Queue
        const T &front()
        {
            pthread_mutex_lock(&muxQueue);
            const T &item = deqQueue.front();
            pthread_mutex_unlock(&muxQueue);
            return item;
        }

        // Returns and maintains item at back of Queue
        const T &back()
        {
            pthread_mutex_lock(&muxQueue);
            const T &item = deqQueue.back();
            pthread_mutex_unlock(&muxQueue);
            return item;
        }

        // Removes and returns item from front of Queue
        T pop_front()
        {
            pthread_mutex_lock(&muxQueue);
            auto t = std::move(deqQueue.front());
            deqQueue.pop_front();
            pthread_mutex_unlock(&muxQueue);
            return t;
        }

        // Removes and returns item from back of Queue
        T pop_back()
        {
            pthread_mutex_lock(&muxQueue);
            auto t = std::move(deqQueue.back());
            deqQueue.pop_back();
            pthread_mutex_unlock(&muxQueue);
            return t;
        }

        // Adds an item to back of Queue
        void push_back(const T &item)
        {
            pthread_mutex_lock(&muxQueue);
            deqQueue.emplace_back(std::move(item));
            pthread_mutex_unlock(&muxQueue);

            pthread_mutex_lock(&muxBlocking);
            pthread_cond_signal(&cvBlocking);
            pthread_mutex_unlock(&muxBlocking);
        }

        // Adds an item to front of Queue
        void push_front(const T &item)
        {
            pthread_mutex_lock(&muxQueue);
            deqQueue.emplace_front(std::move(item));
            pthread_mutex_unlock(&muxQueue);

            pthread_mutex_lock(&muxBlocking);
            pthread_cond_signal(&cvBlocking);
            pthread_mutex_unlock(&muxBlocking);
        }

        // Returns true if Queue has no items
        bool empty()
        {
            pthread_mutex_lock(&muxQueue);
            bool isEmpty = deqQueue.empty();
            pthread_mutex_unlock(&muxQueue);
            return isEmpty;
        }

        // Returns number of items in Queue
        size_t count()
        {
            pthread_mutex_lock(&muxQueue);
            size_t size = deqQueue.size();
            pthread_mutex_unlock(&muxQueue);
            return size;
        }

        // Clears Queue
        void clear()
        {
            pthread_mutex_lock(&muxQueue);
            deqQueue.clear();
            pthread_mutex_unlock(&muxQueue);
        }

        void wait()
        {
            pthread_mutex_lock(&muxBlocking);
            while (empty())
            {
                pthread_cond_wait(&cvBlocking, &muxBlocking);
            }
            pthread_mutex_unlock(&muxBlocking);
        }

    protected:
        pthread_mutex_t muxQueue;
        std::deque<T> deqQueue;
        pthread_cond_t cvBlocking;
        pthread_mutex_t muxBlocking;
    };

    template <typename T>
    class server_interface
    {
    public:
        // Create a server, ready to listen on specified port
        server_interface(uint16_t port)
            : m_asioAcceptor(m_asioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port))
        {
        }

        virtual ~server_interface()
        {
            // May as well try and tidy up
            Stop();
        }

        // Starts the server!
        bool Start()
        {
            try
            {
                // Issue a task to the asio context - This is important
                // as it will prime the context with "work", and stop it
                // from exiting immediately. Since this is a server, we
                // want it primed ready to handle clients trying to
                // connect.
                WaitForClientConnection();

                // Launch the asio context in its own thread
                m_threadContext = std::thread([this]()
                                              { m_asioContext.run(); });
            }
            catch (std::exception &e)
            {
                // Something prohibited the server from listening
                std::cerr << "[SERVER] Exception: " << e.what() << "\n";
                return false;
            }

            std::cout << "[SERVER] Started!\n";
            return true;
        }

        // Stops the server!
        void Stop()
        {
            // Request the context to close
            m_asioContext.stop();

            // Tidy up the context thread
            if (m_threadContext.joinable())
                m_threadContext.join();

            // Inform someone, anybody, if they care...
            std::cout << "[SERVER] Stopped!\n";
        }

        // ASYNC - Instruct asio to wait for connection
        void WaitForClientConnection()
        {
            // Prime context with an instruction to wait until a socket connects. This
            // is the purpose of an "acceptor" object. It will provide a unique socket
            // for each incoming connection attempt
            m_asioAcceptor.async_accept(
                [this](std::error_code ec, asio::ip::tcp::socket socket)
                {
                    // Triggered by incoming connection request
                    if (!ec)
                    {
                        // Display some useful(?) information
                        std::cout << "[SERVER] New Connection: " << socket.remote_endpoint() << "\n";

                        // Create a new connection to handle this client
                        std::shared_ptr<connection<T>> newconn =
                            std::make_shared<connection<T>>(connection<T>::owner::server,
                                                            m_asioContext, std::move(socket), m_qMessagesIn);

                        // Give the user server a chance to deny connection
                        if (OnClientConnect(newconn))
                        {
                            // Connection allowed, so add to container of new connections
                            m_deqConnections.push_back(std::move(newconn));

                            // And very important! Issue a task to the connection's
                            // asio context to sit and wait for bytes to arrive!
                            m_deqConnections.back()->ConnectToClient(nIDCounter++);

                            std::cout << "[" << m_deqConnections.back()->GetID() << "] Connection Approved\n";
                        }
                        else
                        {
                            std::cout << "[-----] Connection Denied\n";

                            // Connection will go out of scope with no pending tasks, so will
                            // get destroyed automagically due to the wonder of smart pointers
                        }
                    }
                    else
                    {
                        // Error has occurred during acceptance
                        std::cout << "[SERVER] New Connection Error: " << ec.message() << "\n";
                    }

                    // Prime the asio context with more work - again simply wait for
                    // another connection...
                    WaitForClientConnection();
                });
        }

        // Send a message to a specific client
        void MessageClient(std::shared_ptr<connection<T>> client, const message<T> &msg)
        {
            // Check client is legitimate...
            if (client && client->IsConnected())
            {
                // ...and post the message via the connection
                client->Send(msg);
            }
            else
            {
                // If we cant communicate with client then we may as
                // well remove the client - let the server know, it may
                // be tracking it somehow
                OnClientDisconnect(client);

                // Off you go now, bye bye!
                client.reset();

                // Then physically remove it from the container
                m_deqConnections.erase(
                    std::remove(m_deqConnections.begin(), m_deqConnections.end(), client), m_deqConnections.end());
            }
        }

        // Send message to all clients
        void MessageAllClients(const message<T> &msg, std::shared_ptr<connection<T>> pIgnoreClient = nullptr)
        {
            bool bInvalidClientExists = false;

            // Iterate through all clients in container
            for (auto &client : m_deqConnections)
            {
                // Check client is connected...
                if (client && client->IsConnected())
                {
                    // ..it is!
                    if (client != pIgnoreClient)
                        client->Send(msg);
                }
                else
                {
                    // The client couldnt be contacted, so assume it has
                    // disconnected.
                    OnClientDisconnect(client);
                    client.reset();

                    // Set this flag to then remove dead clients from container
                    bInvalidClientExists = true;
                }
            }

            // Remove dead clients, all in one go - this way, we dont invalidate the
            // container as we iterated through it.
            if (bInvalidClientExists)
                m_deqConnections.erase(
                    std::remove(m_deqConnections.begin(), m_deqConnections.end(), nullptr), m_deqConnections.end());
        }

        // Force server to respond to incoming messages
        void Update(size_t nMaxMessages = -1, bool bWait = false)
        {
            if (bWait)
                m_qMessagesIn.wait();

            // Process as many messages as you can up to the value
            // specified
            size_t nMessageCount = 0;
            while (nMessageCount < nMaxMessages && !m_qMessagesIn.empty())
            {
                // Grab the front message
                auto msg = m_qMessagesIn.pop_front();

                // Pass to message handler
                OnMessage(msg.remote, msg.msg);

                nMessageCount++;
            }
        }

    protected:
        // This server class should override thse functions to implement
        // customised functionality

        // Called when a client connects, you can veto the connection by returning false
        virtual bool OnClientConnect(std::shared_ptr<connection<T>> client)
        {
            return false;
        }

        // Called when a client appears to have disconnected
        virtual void OnClientDisconnect(std::shared_ptr<connection<T>> client)
        {
        }

        // Called when a message arrives
        virtual void OnMessage(std::shared_ptr<connection<T>> client, message<T> &msg)
        {
        }

    protected:
        // Thread Safe Queue for incoming message packets
        tsqueue<owned_message<T>> m_qMessagesIn;

        // Container of active validated connections
        std::deque<std::shared_ptr<connection<T>>> m_deqConnections;

        // Order of declaration is important - it is also the order of initialisation
        asio::io_context m_asioContext;
        std::thread m_threadContext;

        // These things need an asio context
        asio::ip::tcp::acceptor m_asioAcceptor; // Handles new incoming connection attempts...

        // Clients will be identified in the "wider system" via an ID
        uint32_t nIDCounter = 10000;
    };

    template <typename T>
    class connection : public std::enable_shared_from_this<connection<T>>
    {
    public:
        // A connection is "owned" by either a server or a client, and its
        // behaviour is slightly different bewteen the two.
        enum class owner
        {
            server,
            client
        };

    public:
        // Constructor: Specify Owner, connect to context, transfer the socket
        //				Provide reference to incoming message queue
        connection(owner parent, asio::io_context &asioContext, asio::ip::tcp::socket socket, tsqueue<owned_message<T>> &qIn)
            : m_asioContext(asioContext), m_socket(std::move(socket)), m_qMessagesIn(qIn)
        {
            m_nOwnerType = parent;
        }

        virtual ~connection()
        {
        }

        // This ID is used system wide - its how clients will understand other clients
        // exist across the whole system.
        uint32_t GetID() const
        {
            return id;
        }

    public:
        void ConnectToClient(uint32_t uid = 0)
        {
            if (m_nOwnerType == owner::server)
            {
                if (m_socket.is_open())
                {
                    id = uid;
                    ReadHeader();
                }
            }
        }

        void ConnectToServer(const asio::ip::tcp::resolver::results_type &endpoints)
        {
            // Only clients can connect to servers
            if (m_nOwnerType == owner::client)
            {
                // Request asio attempts to connect to an endpoint
                asio::async_connect(m_socket, endpoints,
                                    [this](std::error_code ec, asio::ip::tcp::endpoint endpoint)
                                    {
                                        if (!ec)
                                        {
                                            ReadHeader();
                                        }
                                    });
            }
        }

        void Disconnect()
        {
            if (IsConnected())
                asio::post(m_asioContext, [this]()
                           { m_socket.close(); });
        }

        bool IsConnected() const
        {
            return m_socket.is_open();
        }

        // Prime the connection to wait for incoming messages
        void StartListening()
        {
        }

    public:
        // ASYNC - Send a message, connections are one-to-one so no need to specifiy
        // the target, for a client, the target is the server and vice versa
        void Send(const message<T> &msg)
        {
            asio::post(m_asioContext,
                       [this, msg]()
                       {
                           // If the queue has a message in it, then we must
                           // assume that it is in the process of asynchronously being written.
                           // Either way add the message to the queue to be output. If no messages
                           // were available to be written, then start the process of writing the
                           // message at the front of the queue.
                           bool bWritingMessage = !m_qMessagesOut.empty();
                           m_qMessagesOut.push_back(msg);
                           if (!bWritingMessage)
                           {
                               WriteHeader();
                           }
                       });
        }

    private:
        // ASYNC - Prime context to write a message header
        void WriteHeader()
        {
            // If this function is called, we know the outgoing message queue must have
            // at least one message to send. So allocate a transmission buffer to hold
            // the message, and issue the work - asio, send these bytes
            asio::async_write(m_socket, asio::buffer(&m_qMessagesOut.front().header, sizeof(message_header<T>)),
                              [this](std::error_code ec, std::size_t length)
                              {
                                  // asio has now sent the bytes - if there was a problem
                                  // an error would be available...
                                  if (!ec)
                                  {
                                      // ... no error, so check if the message header just sent also
                                      // has a message body...
                                      if (m_qMessagesOut.front().body.size() > 0)
                                      {
                                          // ...it does, so issue the task to write the body bytes
                                          WriteBody();
                                      }
                                      else
                                      {
                                          // ...it didnt, so we are done with this message. Remove it from
                                          // the outgoing message queue
                                          m_qMessagesOut.pop_front();

                                          // If the queue is not empty, there are more messages to send, so
                                          // make this happen by issuing the task to send the next header.
                                          if (!m_qMessagesOut.empty())
                                          {
                                              WriteHeader();
                                          }
                                      }
                                  }
                                  else
                                  {
                                      // ...asio failed to write the message, we could analyse why but
                                      // for now simply assume the connection has died by closing the
                                      // socket. When a future attempt to write to this client fails due
                                      // to the closed socket, it will be tidied up.
                                      std::cout << "[" << id << "] Write Header Fail.\n";
                                      m_socket.close();
                                  }
                              });
        }

        // ASYNC - Prime context to write a message body
        void WriteBody()
        {
            // If this function is called, a header has just been sent, and that header
            // indicated a body existed for this message. Fill a transmission buffer
            // with the body data, and send it!
            asio::async_write(m_socket, asio::buffer(m_qMessagesOut.front().body.data(), m_qMessagesOut.front().body.size()),
                              [this](std::error_code ec, std::size_t length)
                              {
                                  if (!ec)
                                  {
                                      // Sending was successful, so we are done with the message
                                      // and remove it from the queue
                                      m_qMessagesOut.pop_front();

                                      // If the queue still has messages in it, then issue the task to
                                      // send the next messages' header.
                                      if (!m_qMessagesOut.empty())
                                      {
                                          WriteHeader();
                                      }
                                  }
                                  else
                                  {
                                      // Sending failed, see WriteHeader() equivalent for description :P
                                      std::cout << "[" << id << "] Write Body Fail.\n";
                                      m_socket.close();
                                  }
                              });
        }

        // ASYNC - Prime context ready to read a message header
        void ReadHeader()
        {
            // If this function is called, we are expecting asio to wait until it receives
            // enough bytes to form a header of a message. We know the headers are a fixed
            // size, so allocate a transmission buffer large enough to store it. In fact,
            // we will construct the message in a "temporary" message object as it's
            // convenient to work with.
            asio::async_read(m_socket, asio::buffer(&m_msgTemporaryIn.header, sizeof(message_header<T>)),
                             [this](std::error_code ec, std::size_t length)
                             {
                                 if (!ec)
                                 {
                                     // A complete message header has been read, check if this message
                                     // has a body to follow...
                                     if (m_msgTemporaryIn.header.size > 0)
                                     {
                                         // ...it does, so allocate enough space in the messages' body
                                         // vector, and issue asio with the task to read the body.
                                         m_msgTemporaryIn.body.resize(m_msgTemporaryIn.header.size);
                                         ReadBody();
                                     }
                                     else
                                     {
                                         // it doesn't, so add this bodyless message to the connections
                                         // incoming message queue
                                         AddToIncomingMessageQueue();
                                     }
                                 }
                                 else
                                 {
                                     // Reading form the client went wrong, most likely a disconnect
                                     // has occurred. Close the socket and let the system tidy it up later.
                                     std::cout << "[" << id << "] Read Header Fail.\n";
                                     m_socket.close();
                                 }
                             });
        }

        // ASYNC - Prime context ready to read a message body
        void ReadBody()
        {
            // If this function is called, a header has already been read, and that header
            // request we read a body, The space for that body has already been allocated
            // in the temporary message object, so just wait for the bytes to arrive...
            asio::async_read(m_socket, asio::buffer(m_msgTemporaryIn.body.data(), m_msgTemporaryIn.body.size()),
                             [this](std::error_code ec, std::size_t length)
                             {
                                 if (!ec)
                                 {
                                     // ...and they have! The message is now complete, so add
                                     // the whole message to incoming queue
                                     AddToIncomingMessageQueue();
                                 }
                                 else
                                 {
                                     // As above!
                                     std::cout << "[" << id << "] Read Body Fail.\n";
                                     m_socket.close();
                                 }
                             });
        }

        // Once a full message is received, add it to the incoming queue
        void AddToIncomingMessageQueue()
        {
            // Shove it in queue, converting it to an "owned message", by initialising
            // with the a shared pointer from this connection object
            if (m_nOwnerType == owner::server)
                m_qMessagesIn.push_back({this->shared_from_this(), m_msgTemporaryIn});
            else
                m_qMessagesIn.push_back({nullptr, m_msgTemporaryIn});

            // We must now prime the asio context to receive the next message. It
            // wil just sit and wait for bytes to arrive, and the message construction
            // process repeats itself. Clever huh?
            ReadHeader();
        }

    protected:
        // Each connection has a unique socket to a remote
        asio::ip::tcp::socket m_socket;

        // This context is shared with the whole asio instance
        asio::io_context &m_asioContext;

        // This queue holds all messages to be sent to the remote side
        // of this connection
        tsqueue<message<T>> m_qMessagesOut;

        // This references the incoming queue of the parent object
        tsqueue<owned_message<T>> &m_qMessagesIn;

        // Incoming messages are constructed asynchronously, so we will
        // store the part assembled message here, until it is ready
        message<T> m_msgTemporaryIn;

        // The "owner" decides how some of the connection behaves
        owner m_nOwnerType = owner::server;

        uint32_t id = 0;
    };

    template <typename T>
    class client_interface
    {
    public:
        client_interface()
        {
        }

        virtual ~client_interface()
        {
            // If the client is destroyed, always try and disconnect from server
            Disconnect();
        }

    public:
        // Connect to server with hostname/ip-address and port
        bool Connect(const std::string &host, const uint16_t port)
        {
            try
            {
                // Resolve hostname/ip-address into tangiable physical address
                asio::ip::tcp::resolver resolver(m_context);
                asio::ip::tcp::resolver::results_type endpoints = resolver.resolve(host, std::to_string(port));

                // Create connection
                m_connection = std::make_unique<connection<T>>(connection<T>::owner::client, m_context, asio::ip::tcp::socket(m_context), m_qMessagesIn);

                // Tell the connection object to connect to server
                m_connection->ConnectToServer(endpoints);

                // Start Context Thread
                thrContext = std::thread([this]()
                                         { m_context.run(); });
            }
            catch (std::exception &e)
            {
                std::cerr << "Client Exception: " << e.what() << "\n";
                return false;
            }
            return true;
        }

        // Disconnect from server
        void Disconnect()
        {
            // If connection exists, and it's connected then...
            if (IsConnected())
            {
                // ...disconnect from server gracefully
                m_connection->Disconnect();
            }

            // Either way, we're also done with the asio context...
            m_context.stop();
            // ...and its thread
            if (thrContext.joinable())
                thrContext.join();

            // Destroy the connection object
            m_connection.release();
        }

        // Check if client is actually connected to a server
        bool IsConnected()
        {
            if (m_connection)
                return m_connection->IsConnected();
            else
                return false;
        }

    public:
        // Send message to server
        void Send(const message<T> &msg)
        {
            if (IsConnected())
                m_connection->Send(msg);
        }

        // Retrieve queue of messages from server
        tsqueue<owned_message<T>> &Incoming()
        {
            return m_qMessagesIn;
        }

    protected:
        // asio context handles the data transfer...
        asio::io_context m_context;
        // ...but needs a thread of its own to execute its work commands
        std::thread thrContext;
        // The client has a single instance of a "connection" object, which handles data transfer
        std::unique_ptr<connection<T>> m_connection;

    private:
        // This is the thread safe queue of incoming messages from server
        tsqueue<owned_message<T>> m_qMessagesIn;
    };
}