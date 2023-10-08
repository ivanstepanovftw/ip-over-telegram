#include <boost/circular_buffer.hpp>
#include <boost/program_options.hpp>
#include <boost/asio.hpp>
#include <ctime>
#include <random>

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <string_view>

#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <utility>
#include <optional>
#include <thread>
#include <atomic>
#include <iostream>
#include <fstream>
#include <variant>
#include <mutex>
#include <fmt/chrono.h>
#include <regex>
#include "tdutils/td/utils/overloaded.h"
#include "base91x.hpp"
#include <tuntap++.hh>


// ryml can be used as a single header, or as a simple library:
#if defined(RYML_SINGLE_HEADER) // using the single header directly in the executable
    #define RYML_SINGLE_HDR_DEFINE_NOW
    #include <ryml_all.hpp>
#elif defined(RYML_SINGLE_HEADER_LIB) // using the single header from a library
    #include <ryml_all.hpp>
#else
    #include <ryml.hpp>
    // <ryml_std.hpp> is needed if interop with std containers is
    // desired; ryml itself does not use any STL container.
    // For this sample, we will be using std interop, so...
    #include <ryml_std.hpp> // optional header, provided for std:: interop
    #include <c4/format.hpp> // needed for the examples below
#endif

#define print(...) do { fmt::print(__VA_ARGS__); std::fflush(stdout); } while (0)
#define println(...) do { fmt::println(__VA_ARGS__); std::fflush(stdout); } while (0)


std::string stringToHex(const std::string & input) {
    std::ostringstream oss;
    for (char c : input) {
        oss << fmt::format("{:02x} ", static_cast<unsigned char>(c));
    }
    std::string result = oss.str();
    if (!result.empty()) {
        result.pop_back();  // Remove trailing space
    }
    return result;
}


struct TDConfig {
    std::string files_directory;
    std::string token;
    int api_id;
    std::string api_hash;
    std::string database_encryption_key;
};

struct TUNConfig {
    std::string name;
    int mtu;
    std::string ip;
};

class Config {
public:
    Config() = default;

    explicit Config(std::string & yaml_str) {
        parse_config(yaml_str);
    }

    void parse_config(std::string & yaml_str) {
        ryml::Tree tree = ryml::parse_in_place(c4::to_substr(yaml_str));
        ryml::ConstNodeRef root = tree.rootref();

        root["tdconfig"]["files_directory"] >> tdconfig.files_directory;
        root["tdconfig"]["token"] >> tdconfig.token;
        root["tdconfig"]["api_id"] >> tdconfig.api_id;
        root["tdconfig"]["api_hash"] >> tdconfig.api_hash;
        root["tdconfig"]["database_encryption_key"] >> tdconfig.database_encryption_key;

        root["tun"]["name"] >> tun.name;
        root["tun"]["mtu"] >> tun.mtu;
        root["tun"]["ip"] >> tun.ip;

        // root["cache_size"] >> cache_size;
        root["cache_flush_rate"] >> cache_flush_rate;
        root["wrap_in_proxy"] >> wrap_in_proxy;
        root["receive_from_user_id"] >> receive_from_user_id;
        root["send_to_chat_id"] >> send_to_chat_id;
    }
public:
    TDConfig tdconfig;
    TUNConfig tun;
    // size_t cache_size;
    float cache_flush_rate;
    bool wrap_in_proxy;
    int receive_from_user_id;
    int send_to_chat_id;
};


class TdClient {
    const std::string MESSAGE_HEADER_WELCOME = "#iot ";
    const std::string MESSAGE_HEADER_TEXT_SINGLE = "#iotts ";
    const std::string MESSAGE_HEADER_TEXT_MULTIPLE = "#iottm ";
    const size_t MESSAGE_MAX_SIZE = 4096;
    const size_t IPV4_PACKET_HEADER_MAX_SIZE = 60;

    Config _config;

    tuntap::tun _tun;

    std::atomic<bool> _network_thread_running;
    std::thread _network_thread;
    std::thread _tun_thread;
    std::thread _cache_flush_thread;
    std::thread stats_thread_;

    std::mutex stats_mutex;
    std::unordered_map<std::string, size_t> stats_;

    std::mutex cache_mutex;
    // boost::circular_buffer<std::string> cache;
    std::vector<std::string> cache;

    using Object = td::td_api::object_ptr<td::td_api::Object>;
    std::unique_ptr<td::ClientManager> _client_manager;
    std::int32_t _client_id{0};

    td::td_api::object_ptr<td::td_api::AuthorizationState> _authorization_state;
    bool _are_authorized{false};
    bool _need_restart{false};
    std::uint64_t _current_query_id{0};
    std::uint64_t _authentication_query_id{0};

    std::map<std::uint64_t, std::function<void(Object)>> _handlers;

public:
    explicit TdClient(Config & config) : _config(config), _network_thread_running{false} {
        td::ClientManager::execute(td::td_api::make_object<td::td_api::setLogVerbosityLevel>(1));
        _client_manager = std::make_unique<td::ClientManager>();
        _client_id = _client_manager->create_client_id();
        _sendQuery(td::td_api::make_object<td::td_api::getOption>("version"), {});

        _tun.name(_config.tun.name);
        _tun.mtu(_config.tun.mtu);
        _tun.up();
        _tun.ip(_config.tun.ip, 24);
        _tun.nonblocking(true);
        println("TUN device {} is up", _config.tun.name);
        //
        // cache.resize(config_.cache_size);
    }

    std::atomic<bool> _listen{true};

    auto _createSendMessageHandler() {
        return [this](Object object) {
            td::td_api::downcast_call(*object, td::overloaded(
                [this](td::td_api::ok &) {
                    std::scoped_lock lock(stats_mutex);
                    stats_["out_send_ok"]++;
                },
                [this](td::td_api::error &) {
                    std::scoped_lock lock(stats_mutex);
                    stats_["out_send_error"]++;
                },
                [this](td::td_api::message & message) {
                    std::scoped_lock lock(stats_mutex);
                    if (message.is_outgoing_) {
                        stats_["out_send_outgoing"]++;
                    } else {
                        stats_["out_send_other"]++;
                    }
                },
                [this](auto &) {
                    std::scoped_lock lock(stats_mutex);
                    stats_["out_send_unknown"]++;
                }
            ));
        };
    }

    void start() {
        if (_network_thread_running) {
            println("Already started");
            return;
        }
        println("Starting...");
        _network_thread_running = true;
        _network_thread = std::thread([this]() {
            println("Begin to wait for updates");
            while (_network_thread_running) {
                auto response = _client_manager->receive(1);
                if (response.object) {
                    _processResponse(std::move(response));
                }
            }
            println("Ended to wait for updates");
        });

        _tun_thread = std::thread([this]() {
            println("Begin to listen for TUN device");
            auto next_flush = std::chrono::steady_clock::now();
            while (_network_thread_running) {
                std::this_thread::sleep_until(next_flush);
                next_flush += std::chrono::milliseconds(1);

                if (!_listen) {
                    continue;
                }

                std::string packet(_config.tun.mtu + IPV4_PACKET_HEADER_MAX_SIZE, '\0');
                auto len = _tun.read(packet.data(), packet.size());
                if (len < 0) {
                    println("Error while reading from TUN device: {}", len);
                    std::scoped_lock lock(stats_mutex);
                    stats_["out_tun_read_error"]++;
                    continue;
                }
                 if (len == 0) {
                     // std::scoped_lock lock(stats_mutex);
                     // stats_["out_tun_read_zero"]++;
                     continue;
                 }
                packet.resize(len);
                {
                    std::scoped_lock lock(stats_mutex);
                    stats_["out_tun_read_ok"]++;
                }

                if (_config.cache_flush_rate > 0) {
                    std::scoped_lock lock(cache_mutex);
                    cache.push_back(packet);
                    std::scoped_lock lock2(stats_mutex);
                    stats_["out_cache_inserted"]++;
                } else {
                    std::string packet_encoded;
                    base91x::encode(packet, packet_encoded);

                    _sendTextMessage(_config.send_to_chat_id,
                        fmt::format("{}{}", MESSAGE_HEADER_TEXT_SINGLE, packet_encoded),
                        _createSendMessageHandler());
                }
            }
            println("Ended to listen for TUN device");
        });

        if (_config.cache_flush_rate > 0) {
            _cache_flush_thread = std::thread([this]() {
                println("Begin to flush cache");
                auto next_flush = std::chrono::steady_clock::now();
                while (_network_thread_running) {
                    std::this_thread::sleep_until(next_flush);
                    next_flush += std::chrono::milliseconds(int64_t(1000.f / _config.cache_flush_rate));

                    if (!_listen) {
                        continue;
                    }

                    std::ostringstream oss;
                    {
                        std::scoped_lock lock(cache_mutex);

                        if (cache.empty()) {
                            continue;
                        }

                        std::scoped_lock lock2(stats_mutex);
                        for (const auto & str : cache) {
                            auto length = (uint16_t) str.size();
                            oss.write(reinterpret_cast<const char *>(&length), sizeof(length));
                            oss.write(str.data(), length);
                        }
                        // stats_["out_tun_cache_flushed"] += cache.size();
                        // stats_["out_tun_cache_flushes"]++;
                        cache.clear();
                    }

                    std::string packets = oss.str();

                    std::string packets_encoded;
                    base91x::encode(packets, packets_encoded);

                    _sendTextMessage(_config.send_to_chat_id,
                        fmt::format("{}{}", MESSAGE_HEADER_TEXT_MULTIPLE, packets_encoded),
                        _createSendMessageHandler());
                }
                println("Ended to flush cache");
            });
        }

        stats_thread_ = std::thread([this]() {
            auto next_flush = std::chrono::steady_clock::now();
            while (_network_thread_running) {
                std::this_thread::sleep_until(next_flush);
                next_flush += std::chrono::seconds(5);

                if (!_listen) {
                    continue;
                }

                print("Stats: ");
                {
                    std::scoped_lock lock(stats_mutex);
                    for (const auto & [key, value] : stats_) {
                        print("{}: {}, ", key, value);
                    }
                }
                println("");
            }
        });

        welcome();
        println("Started");
    }

    void welcome() {
        _sendTextMessage(_config.send_to_chat_id,
            fmt::format("{}{} started tun device: {} dev {}",
                MESSAGE_HEADER_WELCOME,
                boost::asio::ip::host_name(),
                _config.tun.ip,
                _config.tun.name),
            _createSendMessageHandler());
    }

    void stop() {
        if (!_network_thread_running) {
            return;
        }
        _network_thread_running = false;
        _network_thread.join();
        _tun_thread.join();
        if (_config.cache_flush_rate > 0) {
           _cache_flush_thread.join();
        }
        stats_thread_.join();
    }

    void sendHistoryQuery(
        ssize_t chat_id,
        ssize_t from_message_id,
        int32_t offset,
        int32_t history_size,
        bool only_local,
        std::function<void(td::td_api::object_ptr<td::td_api::Object>)> on_messages_received,
        std::function<void()> on_complete)
    {
        if (history_size <= 0) {
            on_complete();
            return;
        }
        _sendQuery(td::td_api::make_object<td::td_api::getChatHistory>(
            chat_id,
            from_message_id,
            offset,
            history_size,
            only_local
        ), [this, chat_id, from_message_id, offset, history_size, only_local, on_messages_received = std::move(on_messages_received), on_complete = std::move(on_complete)](td::td_api::object_ptr<td::td_api::Object> object) {
            if (object->get_id() == td::td_api::error::ID) {
                on_messages_received(std::move(object));
                on_complete();
                return;
            }
            auto messages = td::move_tl_object_as<td::td_api::messages>(object);
            ssize_t next_from_message_id = messages->messages_.back()->id_;
            auto remaining_history_size = (int32_t) (static_cast<size_t>(history_size) - messages->messages_.size());
            bool are_messages_empty = messages->messages_.empty();
            on_messages_received(std::move(messages));
            if (remaining_history_size > 0 && !are_messages_empty) {
                sendHistoryQuery(chat_id, next_from_message_id, offset, remaining_history_size, only_local, on_messages_received, on_complete);
            } else {
                on_complete();
            }
        });
    }

    bool update() {
        if (_need_restart) {
            return false;
        }
        if (!_are_authorized) {
            _processResponse(_client_manager->receive(10));
            return true;
        }

        println("Help:\n"
            "[start/stop] td client,\n"
            "[on/off] the messaging,\n"
            "[close] connection,\n"
            "[me] show self,\n"
            "[welcome] sends welcome message,\n"
            "[clean] IP packets in Telegram messages,\n"
            "[l] logout,\n"
            "[q] quit\n"
            "\n"
            "Enter action: ");

        std::string line;
        std::getline(std::cin, line);
        std::istringstream ss(line);
        std::string action;
        if (!(ss >> action)) {
            return true;
        }
        if (action == "q") {
            return false;
        }
        else if (action == "start") {
            start();
        }
        else if (action == "stop") {
            stop();
        }
        else if (action == "on") {
            _listen = true;
        }
        else if (action == "off") {
            _listen = false;
        }
        else if (action == "close") {
            println("Closing...");
            _sendQuery(td::td_api::make_object<td::td_api::close>(), {});
        }
        else if (action == "me") {
            _sendQuery(td::td_api::make_object<td::td_api::getMe>(),
                       [this](Object object) {
                           println("{}", to_string(object));
                       });
        }
        else if (action == "welcome") {
            welcome();
        }
        else if (action == "clean") {
            std::shared_ptr<size_t> count = std::make_shared<size_t>(0);
            std::shared_ptr<bool> done = std::make_shared<bool>(false);
            sendHistoryQuery(
                _config.send_to_chat_id,
                0,
                0,
                std::numeric_limits<std::int32_t>::max(),
                false,
                [this, count, done](td::td_api::object_ptr<td::td_api::Object> object) {
                    if (object->get_id() == td::td_api::error::ID) {
                        println("{}", td::td_api::to_string(object));
                        return;
                    }

                    auto messages = td::move_tl_object_as<td::td_api::messages>(object);
                    std::vector<td::td_api::int53> message_ids;
                    for (td::td_api::object_ptr<td::td_api::message> & message: messages->messages_) {
                        std::string text;
                        td::td_api::downcast_call(*message->content_, td::overloaded(
                            [&text](td::td_api::messageText & message_text) {
                                text = message_text.text_->text_;
                            },
                            [](auto & update) {}
                        ));
                        if (text.find(MESSAGE_HEADER_WELCOME) == 0
                            || text.find(MESSAGE_HEADER_TEXT_SINGLE) == 0
                            || text.find(MESSAGE_HEADER_TEXT_MULTIPLE) == 0)
                        {
                            message_ids.push_back(message->id_);
                        }
                    }

                    std::shared_ptr<size_t> messages_size = std::make_shared<size_t>(message_ids.size());
                    _sendQuery(td::td_api::make_object<td::td_api::deleteMessages>(
                        _config.send_to_chat_id,
                        std::move(message_ids),
                        true
                    ), [count, done, messages_size](Object object) {
                        *count += *messages_size;
                        if (*done) {
                            println("Cleaned {} messages!", *count);
                        }
                    });
                },
                [count, done]() {
                    *done = true;
                    println("on end: {} messages", *count);
                }
            );
        }
        else if (action == "l") {
            println("Logging out...");
            _sendQuery(td::td_api::make_object<td::td_api::logOut>(), [](Object object) {
                println("{}", td::td_api::to_string(object));
            });
        }
        else {
            println("Unsupported action: {}", action);
        }
        return true;
    }

    void loop() {
        while (update());
    }

private:
    void _sendQuery(td::td_api::object_ptr<td::td_api::Function> f, const std::function<void(Object)> & handler) {
        ++_current_query_id;
        if (handler) {
            _handlers.emplace(_current_query_id, handler);
        }
        _client_manager->send(_client_id, _current_query_id, std::move(f));
    }

    void _sendTextMessage(ssize_t chat_id, const std::string & text, const std::function<void(Object)> & handler) {
        _sendQuery(
            td::td_api::make_object<td::td_api::sendMessage>(
                chat_id,
                0,
                nullptr,
                td::td_api::make_object<td::td_api::messageSendOptions>(true, false, false, false, nullptr, 0),
                nullptr,
                td::td_api::make_object<td::td_api::inputMessageText>(
                    td::td_api::make_object<td::td_api::formattedText>(
                        text,
                        td::td_api::array<td::tl::unique_ptr<td::td_api::textEntity>>()
                    ),
                    false,
                    false
                )
            ),
            handler);
    }

    void _processUpdate(td::td_api::object_ptr<td::td_api::Object> update) {
        td::td_api::downcast_call(*update, td::overloaded(
            // [this](td::td_api::error & error) {
            //     println("Receive an error: {}", to_string(error));
            // },
            [this](td::td_api::updateAuthorizationState & update_authorization_state) {
                _authorization_state = std::move(update_authorization_state.authorization_state_);
                _onAuthorizationStateUpdate();
            },
            [this](td::td_api::updateMessageSendAcknowledged & update_message_send_acknowledged) {
                std::scoped_lock lock(stats_mutex);
                stats_["out_send_acknowledged"]++;
            },
            [this](td::td_api::updateMessageSendSucceeded & update_message_send_succeeded) {
                std::scoped_lock lock(stats_mutex);
                stats_["out_send_successed"]++;
            },
            [this](td::td_api::updateNewMessage & update_new_message) {
                {
                    std::scoped_lock lock(stats_mutex);
                    stats_["in_receive"]++;
                }
                auto chat_id = update_new_message.message_->chat_id_;
                if (chat_id != _config.receive_from_user_id) {
                    return;
                }

                td::td_api::int53 sender_id;
                td::td_api::downcast_call(*update_new_message.message_->sender_id_, td::overloaded(
                    [this, &sender_id](td::td_api::messageSenderUser & user) {
                        sender_id = user.user_id_;
                    },
                    [this, &sender_id](td::td_api::messageSenderChat & chat) {
                        sender_id = chat.chat_id_;
                    }));
                if (sender_id != _config.receive_from_user_id) {
                    return;
                }

                std::string text;
                td::td_api::downcast_call(*update_new_message.message_->content_, td::overloaded(
                    [&text](td::td_api::messageText & message_text) {
                        text = message_text.text_->text_;
                    },
                    [](auto & update) {}));

                if (text.empty()) {
                    return;
                }

                // Check if text starts with "#iot "
                if (text.find(MESSAGE_HEADER_TEXT_SINGLE) == 0) {
                    // Strip header from text to get packet
                    auto packet_encoded = std::string_view(text).substr(MESSAGE_HEADER_TEXT_SINGLE.size());

                    // Decode from base64
                    std::string packet;
                    base91x::decode(packet_encoded, packet);

                    // Send packet to TUN
                    auto b = _tun.write(packet.data(), packet.size());
                    if (b != packet.size()) {
                        println(stderr, "Failed to write direct packet to TUN, wrote {} bytes instead of {}", b, packet.size());
                        std::scoped_lock lock(stats_mutex);
                        stats_["in_write_error"]++;
                    } else {
                        std::scoped_lock lock(stats_mutex);
                        stats_["in_write_ok"]++;
                    }
                }

                if (text.find(MESSAGE_HEADER_TEXT_MULTIPLE) == 0) {
                    // Strip header from text to get packet
                    auto packets_encoded = std::string_view(text).substr(MESSAGE_HEADER_TEXT_MULTIPLE.size());

                    // Decode from base64
                    std::string packets;
                    base91x::decode(packets_encoded, packets);

                    // Read packets from string
                    std::istringstream iss(packets);
                    size_t i = 0;
                    while (iss) {
                        uint16_t length;
                        if (iss.read(reinterpret_cast<char*>(&length), sizeof(length)).gcount() != sizeof(length))
                            break;

                        if (!length) {
                            println(stderr,
                                "Zero length cache message #{}\n"
                                "  text: {}\n"
                                "  packets_encoded: {}\n"
                                "  packets: {}",
                                i,
                                text,
                                packets_encoded,
                                stringToHex(packets)
                            );
                            break;
                        }

                        std::string packet(length, '\0');
                        if (iss.read(packet.data(), length).gcount() != length)
                            break;

                        // Send packet to TUN
                        auto b = _tun.write(packet.data(), packet.size());
                        if (b != packet.size()) {
                            println(stderr, "Failed to write cached packet #{} to TUN, wrote {} bytes instead of {}", i, b, packet.size());
                            std::scoped_lock lock(stats_mutex);
                            stats_["in_write_error"]++;
                        } else {
                            std::scoped_lock lock(stats_mutex);
                            stats_["in_write_ok"]++;
                        }
                        i++;
                    }
                }
            },
            [](auto & update) {
                println("Receive an update: {}", to_string(update));
            }
        ));
    }

    void _processResponse(td::ClientManager::Response response) {
        if (!response.object) {
            return;
        }
        if (response.request_id == 0) {
            return _processUpdate(std::move(response.object));
        }
        auto it = _handlers.find(response.request_id);
        if (it != _handlers.end()) {
            it->second(std::move(response.object));
            _handlers.erase(it);
        }
    }

    //
    // Authentication
    //
    void _checkAuthenticationError(Object object) {
        if (object->get_id() == td::td_api::error::ID) {
            auto error = td::move_tl_object_as<td::td_api::error>(object);
            println("Error: {}", td::td_api::to_string(error));
            _onAuthorizationStateUpdate();
        }
    }

    auto _createAuthenticationQueryHandler() {
        return [this, id = _authentication_query_id](Object object) {
            if (id == _authentication_query_id) {
                _checkAuthenticationError(std::move(object));
            }
        };
    }

    void _onAuthorizationStateUpdate() {
        _authentication_query_id++;
        td::td_api::downcast_call(*_authorization_state, td::overloaded(
            [this](td::td_api::authorizationStateReady &) {
                _are_authorized = true;
                println("Authorization is completed");
                start();
            },
            [this](td::td_api::authorizationStateLoggingOut &) {
                _are_authorized = false;
                println("Logging out");
            },
            [this](td::td_api::authorizationStateClosing &) {
                println("Closing");
            },
            [this](td::td_api::authorizationStateClosed &) {
                _are_authorized = false;
                _need_restart = true;
                println("Terminated");
            },
            [this](td::td_api::authorizationStateWaitPhoneNumber &) {
                std::string phone_number;
                print("Enter phone number: ");
                if (!_config.tdconfig.token.empty()) {
                    phone_number = _config.tdconfig.token;
                    println("{} (loaded from config)", phone_number);
                } else {
                    std::cin >> phone_number;
                }
                _sendQuery(td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>(phone_number, nullptr),
                           _createAuthenticationQueryHandler());
            },
            [this](td::td_api::authorizationStateWaitEmailAddress &) {
                print("Enter email address: ");
                std::string email_address;
                std::cin >> email_address;
                _sendQuery(td::td_api::make_object<td::td_api::setAuthenticationEmailAddress>(email_address),
                           _createAuthenticationQueryHandler());
            },
            [this](td::td_api::authorizationStateWaitEmailCode &) {
                print("Enter email authentication code: ");
                std::string code;
                std::cin >> code;
                _sendQuery(td::td_api::make_object<td::td_api::checkAuthenticationEmailCode>(td::td_api::make_object<td::td_api::emailAddressAuthenticationCode>(code)),
                           _createAuthenticationQueryHandler());
            },
            [this](td::td_api::authorizationStateWaitCode &) {
                print("Enter authentication code: ");
                std::string code;
                std::cin >> code;
                _sendQuery(td::td_api::make_object<td::td_api::checkAuthenticationCode>(code),
                           _createAuthenticationQueryHandler());
            },
            [this](td::td_api::authorizationStateWaitRegistration &) {
                std::string first_name;
                std::string last_name;
                print("Enter your first name: ");
                std::cin >> first_name;
                print("Enter your last name: ");
                std::cin >> last_name;
                _sendQuery(td::td_api::make_object<td::td_api::registerUser>(first_name, last_name),
                           _createAuthenticationQueryHandler());
            },
            [this](td::td_api::authorizationStateWaitPassword &) {
                print("Enter authentication password: ");
                std::string password;
                std::getline(std::cin, password);
                _sendQuery(td::td_api::make_object<td::td_api::checkAuthenticationPassword>(password),
                           _createAuthenticationQueryHandler());
            },
            [this](td::td_api::authorizationStateWaitOtherDeviceConfirmation & state) {
                println("Confirm this login link on another device: {}", state.link_);
            },
            [this](td::td_api::authorizationStateWaitTdlibParameters &) {
                auto request = td::td_api::make_object<td::td_api::setTdlibParameters>();
                request->database_directory_ = "tdlib";
                request->use_message_database_ = true;
                request->use_secret_chats_ = true;
                request->api_id_ = _config.tdconfig.api_id;
                request->api_hash_ = _config.tdconfig.api_hash;
                request->system_language_code_ = "en";
                request->device_model_ = "Desktop";
                request->application_version_ = "1.0";
                request->enable_storage_optimizer_ = true;
                _sendQuery(std::move(request), _createAuthenticationQueryHandler());
            }
        ));
    }
};


int main(int argc, char * argv[], [[maybe_unused]] char * envp[]) {
    namespace po = boost::program_options;

    try {
        po::options_description desc("Allowed options");
        desc.add_options()
            ("config", po::value<std::string>(), "path to config file")
            ("help", "show help message and exit")
            ;

        po::positional_options_description p;
        p.add("config", 1);

        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cerr << desc << std::endl;
            return 1;
        }

        if (!vm.count("config")) {
            throw std::runtime_error("Config path was not set.");
        }

        auto config_path = vm["config"].as<std::string>();
        std::ifstream ifs(config_path);
        auto yaml_str = std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        Config config(yaml_str);

        TdClient tc(config);
        tc.loop();
    } catch (std::exception & e) {
        println(stderr, "error: {}", e.what());
        return 1;
    } catch (...) {
        println(stderr, "Exception of unknown type!");
    }
}
