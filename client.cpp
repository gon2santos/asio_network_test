#include <iostream>
#include "Asio/asio.hpp"
#include "NetworkEngine.h"

#include <functional>

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

int main()
{

    asio::io_service ioservice;
    asio::posix::stream_descriptor stream(ioservice, STDIN_FILENO);

    char buf[1] = {};

    CustomClient c;
    c.Connect("127.0.0.1", 60000);

    bool bQuit = false;

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
                    ioservice.stop();
                    return;
                }
            }
            asio::async_read(stream, asio::buffer(buf), read_handler);
        }
    };

    if (c.IsConnected())
    {
        if (!c.Incoming().empty())
        {

            auto msg = c.Incoming().pop_front().msg;

            switch (msg.header.id)
            {
            case CustomMsgTypes::ServerAccept:
            {
                // Server has responded to a ping request
                std::cout << "Server Accepted Connection\n";
            }
            break;

            case CustomMsgTypes::ServerPing:
            {
                // Server has responded to a ping request
                std::chrono::system_clock::time_point timeNow = std::chrono::system_clock::now();
                std::chrono::system_clock::time_point timeThen;
                msg >> timeThen;
                std::cout << "Ping: " << std::chrono::duration<double>(timeNow - timeThen).count() << "\n";
            }
            break;

            case CustomMsgTypes::ServerMessage:
            {
                // Server has responded to a ping request
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
        std::cout << "Server Down\n";
        bQuit = true;
    }

    asio::async_read(stream, asio::buffer(buf), read_handler);

    ioservice.run();
}