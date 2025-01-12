#include <iostream>
#include "Asio/asio.hpp"
#include "NetworkEngine.h"

#include <functional>

asio::io_context io_context;
asio::steady_timer timer(io_context, std::chrono::seconds(1));

enum class CustomMsgTypes : uint32_t
{
    ServerAccept,
    ServerDeny,
    ServerPing,
    MessageAll,
    ServerMessage,
};

class CustomClient : public Net::client_interface<CustomMsgTypes>
{
public:
    void PingServer()
    {
        Net::message<CustomMsgTypes> msg;
        msg.header.id = CustomMsgTypes::ServerPing;

        // Caution with this...
        std::chrono::system_clock::time_point timeNow = std::chrono::system_clock::now();

        msg << timeNow;
        Send(msg);
    }

    void MessageAll()
    {
        Net::message<CustomMsgTypes> msg;
        msg.header.id = CustomMsgTypes::MessageAll;
        Send(msg);
    }
};

CustomClient c;

void check_connection()
{
    if (c.IsConnected())
    {
        if (!c.Incoming().empty())
        {
            auto msg = c.Incoming().pop_front().msg;

            switch (msg.header.id)
            {
            case CustomMsgTypes::ServerAccept:
            {
                std::cout << "Server Accepted Connection\n";
            }
            break;

            case CustomMsgTypes::ServerPing:
            {
                std::chrono::system_clock::time_point timeNow = std::chrono::system_clock::now();
                std::chrono::system_clock::time_point timeThen;
                msg >> timeThen;
                std::cout << "Ping: " << std::chrono::duration<double>(timeNow - timeThen).count() << "\n";
            }
            break;

            case CustomMsgTypes::ServerMessage:
            {
                uint32_t clientID;
                msg >> clientID;
                std::cout << "Hello from [" << clientID << "]\n";
            }
            break;
            }
        }
    }
    else
    {
        std::cout << "Server Down, shutting down...\n";
        io_context.stop();
        return;
    }

    // Reset the timer and call check_connection again after 1 second
    timer.expires_after(std::chrono::seconds(1));
    timer.async_wait([](const asio::error_code & /*e*/)
                     { check_connection(); });
}

int main()
{
    asio::posix::stream_descriptor stream(io_context, STDIN_FILENO);

    char buf[1] = {};

    c.Connect("127.0.0.1", 60000);

    std::function<void(asio::error_code, size_t)> read_handler;

    read_handler = [&](asio::error_code ec, size_t len)
    {
        if (ec)
        {
            std::cerr << "exit with " << ec.message() << std::endl;
        }
        else
        {
            if (len == 1)
            {
                if (buf[0] == '0')
                {
                    std::cout << "MessageAll" << std::endl;
                    c.MessageAll();
                }
                else if (buf[0] == '1')
                {
                    std::cout << "Pinging" << std::endl;
                    c.PingServer();
                }
                else if (buf[0] == '2')
                {
                    std::cout << "Stopping" << std::endl;
                    io_context.stop();
                    return;
                }
            }
            // Introduce a small delay before reading again
            timer.expires_after(std::chrono::milliseconds(50));
            timer.async_wait([&](const asio::error_code & /*e*/)
                             { asio::async_read(stream, asio::buffer(buf), read_handler); });
        }
    };
    // Start the periodic check with a delay
    timer.async_wait([](const asio::error_code & /*e*/)
                     { check_connection(); });
    asio::async_read(stream, asio::buffer(buf), read_handler);

    check_connection();

    io_context.run();
}