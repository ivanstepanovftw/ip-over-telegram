#include <boost/circular_buffer.hpp>
#include <boost/program_options.hpp>
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

        root["cache_size"] >> cache_size;
        root["cache_flush_rate"] >> cache_flush_rate;
        root["wrap_in_proxy"] >> wrap_in_proxy;
        root["receive_from_user_id"] >> receive_from_user_id;
        root["send_to_chat_id"] >> send_to_chat_id;
    }
public:
    TDConfig tdconfig;
    TUNConfig tun;
    size_t cache_size;
    float cache_flush_rate;
    bool wrap_in_proxy;
    int receive_from_user_id;
    int send_to_chat_id;
};




void serialize(const boost::circular_buffer<std::string>& buffer, std::ostream& os) {
    for (const auto& str : buffer) {
        std::int64_t length = str.size();
        os.write(reinterpret_cast<const char*>(&length), sizeof(length));
        os.write(str.data(), length);
    }
}

bool deserialize(boost::circular_buffer<std::string>& buffer, std::istream& is) {
    std::int64_t size;
    is.read(reinterpret_cast<char*>(&size), sizeof(size));

    for (std::int64_t i = 0; i < size; ++i) {
        std::int64_t length;
        is.read(reinterpret_cast<char*>(&length), sizeof(length));

        std::string str(length, '\0');
        is.read(&str[0], length);

        buffer.push_back(std::move(str));
    }

    return !is.fail();
}

class TdClient {
    const std::string MESSAGE_HEADER = "#iot ";
    const std::string MESSAGE_CACHED_HEADER = "#iotc ";
    const size_t MESSAGE_MAX_SIZE = 4096;
    const size_t IPV4_PACKET_HEADER_MAX_SIZE = 60;
public:
    explicit TdClient(Config & config) : config_(config), network_thread_running_{false} {
        td::ClientManager::execute(td::td_api::make_object<td::td_api::setLogVerbosityLevel>(1));
        client_manager_ = std::make_unique<td::ClientManager>();
        client_id_ = client_manager_->create_client_id();
        _send_query(td::td_api::make_object<td::td_api::getOption>("version"), {});
        tun_device_.name(config_.tun.name);
        tun_device_.mtu(config_.tun.mtu);
        tun_device_.up();
        tun_device_.ip(config_.tun.ip, 24);
        tun_device_.nonblocking(true);
    }

    void start() {
        if (network_thread_running_) {
            return;
        }
        network_thread_running_ = true;
        network_thread_ = std::thread([this]() {
            fmt::println("Begin to wait for updates");
            while (network_thread_running_) {
                auto response = client_manager_->receive(1);
                if (response.object) {
                    _process_response(std::move(response));
                }
            }
            fmt::println("Ended to wait for updates");
        });

        std::mutex cache_mutex;
        boost::circular_buffer<std::string> cache(config_.cache_size);

        tun_thread_ = std::thread([this, &cache_mutex, &cache]() {
            fmt::println("Begin to listen for TUN device");
            while (network_thread_running_) {
                std::string packet(config_.tun.mtu + IPV4_PACKET_HEADER_MAX_SIZE, '\0');
                auto len = tun_device_.read(packet.data(), packet.size());
                if (len < 0) {
                    fmt::println("Error while reading from TUN device: {}", len);
                    continue;
                }
                packet.resize(len);

                if (config_.cache_size) {
                    std::lock_guard<std::mutex> lock(cache_mutex);
                    cache.push_back(packet);
                } else {
                    std::string packet_encoded;
                    base91x::encode(packet, packet_encoded);

                    _send_query(
                        td::td_api::make_object<td::td_api::sendMessage>(
                            config_.send_to_chat_id,
                            0,
                            nullptr,
                            td::td_api::make_object<td::td_api::messageSendOptions>(true, false, false, false, nullptr, 0),
                            nullptr,
                            td::td_api::make_object<td::td_api::inputMessageText>(
                                td::td_api::make_object<td::td_api::formattedText>(
                                    fmt::format("{}{}", MESSAGE_HEADER, packet_encoded),
                                    td::td_api::array<td::tl::unique_ptr<td::td_api::textEntity>>()
                                ),
                                false,
                                false
                            )
                        ),
                        [](Object object) {
                            if (object->get_id() == td::td_api::error::ID) {
                                fmt::println("{}", td::td_api::to_string(object));
                            }
                        }
                    );
                }
            }
            fmt::println("Ended to listen for TUN device");
        });
        cache_flush_thread_ = std::thread([this, &cache_mutex, &cache]() {
            fmt::println("Begin to flush cache");
            auto next_flush = std::chrono::steady_clock::now();
            while (network_thread_running_) {
                std::this_thread::sleep_until(next_flush);
                next_flush += std::chrono::milliseconds(int64_t(1000.f / config_.cache_flush_rate));

                std::ostringstream oss;
                {
                    std::lock_guard<std::mutex> lock(cache_mutex);
                    for (const auto & str : cache) {
                        auto length = (uint16_t) str.size();
                        oss.write(reinterpret_cast<const char*>(&length), sizeof(length));
                        oss.write(str.data(), length);
                    }
                    cache.clear();
                }

                std::string packets = oss.str();

                std::string packets_encoded;
                base91x::encode(packets, packets_encoded);

                _send_query(
                    td::td_api::make_object<td::td_api::sendMessage>(
                        config_.send_to_chat_id,
                        0,
                        nullptr,
                        td::td_api::make_object<td::td_api::messageSendOptions>(true, false, false, false, nullptr, 0),
                        nullptr,
                        td::td_api::make_object<td::td_api::inputMessageText>(
                            td::td_api::make_object<td::td_api::formattedText>(
                                fmt::format("{}{}", MESSAGE_CACHED_HEADER, packets_encoded),
                                td::td_api::array<td::tl::unique_ptr<td::td_api::textEntity>>()
                            ),
                            false,
                            false
                        )
                    ),
                    [](Object object) {
                        if (object->get_id() == td::td_api::error::ID) {
                            fmt::println("{}", td::td_api::to_string(object));
                        }
                    }
                );
            }
            fmt::println("Ended to flush cache");
        });
    }

    void stop() {
        if (!network_thread_running_) {
            return;
        }
        network_thread_running_ = false;
        network_thread_.join();
        tun_thread_.join();
        cache_flush_thread_.join();
    }

    void send_history_query(
            ssize_t chat_id,
            ssize_t from_message_id,
            ssize_t offset,
            ssize_t history_size,
            bool only_local,
            std::function<void(td::td_api::object_ptr<td::td_api::Object>)> on_messages_received,
            std::function<void()> on_complete) {
        _send_query(td::td_api::make_object<td::td_api::getChatHistory>(chat_id, from_message_id, offset, history_size, only_local), [this, chat_id, from_message_id, offset, history_size, only_local, on_messages_received = std::move(on_messages_received), on_complete = std::move(on_complete)](td::td_api::object_ptr<td::td_api::Object> object) {
            if (object->get_id() == td::td_api::error::ID) {
                on_messages_received(std::move(object));
                return on_complete();
            }
            auto messages = td::move_tl_object_as<td::td_api::messages>(object);
            ssize_t next_from_message_id = messages->messages_.back()->id_;
            ssize_t remaining_history_size = history_size - messages->messages_.size();
            bool are_messages_empty = messages->messages_.empty();
            on_messages_received(std::move(messages));
            if (remaining_history_size > 0 && !are_messages_empty) {
                send_history_query(chat_id, next_from_message_id, offset, remaining_history_size, only_local, on_messages_received, on_complete);
            } else {
                on_complete();
            }
        });
    }

    bool update() {
        if (need_restart_) {
            return false;
        }
        if (!are_authorized_) {
            _process_response(client_manager_->receive(10));
            return true;
        }

        fmt::println("Enter action [start/stop], [me] show self, [c] show chats, [m <chat_id> <text>] send message, [l] logout, [q] quit: ");
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
        // else if (action == "start" || action == "s") {
        //     start();
        // }
        // else if (action == "stop") {
        //     stop();
        // }
        // else if (action == "c") {
        //     fmt::println("Loading chat list...");
        //     send_query(td::td_api::make_object<td::td_api::getChats>(nullptr, 20), [this](Object object) {
        //         if (object->get_id() == td::td_api::error::ID) {
        //             return;
        //         }
        //         auto chats = td::move_tl_object_as<td::td_api::chats>(object);
        //         for (auto chat_id: chats->chat_ids_) {
        //             fmt::println("[chat_id:{}] [title:{}]", chat_id, chat_title_[chat_id]);
        //         }
        //     });
        // }
        // else if (action == "h") {
        //     auto chat_id = 829534074; // self
        //     size_t history_size = 200;
        //
        //     std::shared_ptr<std::vector<std::string>> history = std::make_shared<std::vector<std::string>>();
        //     auto history_fill = [this, chat_id, history_size, history](Object object) {
        //         if (object->get_id() == td::td_api::error::ID) {
        //             fmt::println("{}", td::td_api::to_string(object));
        //             return;
        //         }
        //         auto messages = td::move_tl_object_as<td::td_api::messages>(object);
        //         fmt::println("I've got {} messages", messages->messages_.size());
        //         for (auto & message : messages->messages_) {
        //             std::string message_str;
        //             if (message->content_->get_id() == td::td_api::messageText::ID) {
        //                 message_str = static_cast<td::td_api::messageText *>(message->content_.get())->text_->text_;
        //             } else {
        //                 message_str = td::td_api::to_string(message->content_);
        //                 message_str = message_str.substr(0, message_str.find(' '));
        //                 message_str = fmt::format("Can't display {}", message_str);
        //             }
        //             // strip message to first newline and limit to 100 chars
        //             message_str = message_str.substr(0, message_str.find('\n'));
        //             if (message_str.size() > 100) {
        //                 message_str = message_str.substr(0, 100) + "...";
        //             }
        //             history->push_back(message_str);
        //         }
        //     };
        //     auto history_done = [this, history]() {
        //         fmt::println("I've got {} history messages!", history->size());
        //         for (const auto & message : *history) {
        //             fmt::println("Message: {}", message);
        //         }
        //         fmt::println("I've done= )");
        //     };
        //     send_history_query(chat_id, 0, 0, history_size, false, history_fill, history_done);
        // }
        // else if (action == "close") {
        //     std::cout << "Closing..." << std::endl;
        //     send_query(td::td_api::make_object<td::td_api::close>(), {});
        // }
        else if (action == "me") {
            _send_query(td::td_api::make_object<td::td_api::getMe>(),
                        [this](Object object) {
                            fmt::println("{}", to_string(object));
                        });
        }
        // else if (action == "m") {
        //     auto chat_id = -1001447502236; // 420
        //     // auto chat_id = 829534074; // self
        //
        //     auto send_message = td::td_api::make_object<td::td_api::sendMessage>();
        //     send_message->chat_id_ = chat_id;
        //     auto message_options = td::td_api::make_object<td::td_api::messageSendOptions>();
        //     message_options->disable_notification_ = true;
        //     send_message->options_ = std::move(message_options);
        //     auto message_content = td::td_api::make_object<td::td_api::inputMessageText>();
        //     message_content->text_ = td::td_api::make_object<td::td_api::formattedText>();
        //     message_content->text_->text_ = "Hello, world!";
        //
        //     auto button = td::td_api::make_object<td::td_api::inlineKeyboardButton>();
        //     button->text_ = "Click me!";
        //     auto type = td::td_api::make_object<td::td_api::inlineKeyboardButtonTypeCallback>();
        //     type->data_ = "callback_data";
        //     button->type_ = std::move(type);
        //
        //     // Create a row of buttons and add the button to the row
        //     auto row = std::vector<td::td_api::object_ptr<td::td_api::inlineKeyboardButton>>();
        //     row.push_back(std::move(button));
        //
        //     // Add the row to the inline keyboard markup
        //     auto reply_markup = td::td_api::make_object<td::td_api::replyMarkupInlineKeyboard>();
        //     reply_markup->rows_.push_back(std::move(row));
        //
        //     send_message->reply_markup_ = std::move(reply_markup);
        //     send_message->input_message_content_ = std::move(message_content);
        //     fmt::println("1 {}", td::td_api::to_string(send_message));
        //     send_query(std::move(send_message), [](Object object){
        //         fmt::println("2 {}", td::td_api::to_string(object));
        //     });
        // }
        else if (action == "l") {
            fmt::println("Logging out...");
            _send_query(td::td_api::make_object<td::td_api::logOut>(), [](Object object) {
                fmt::println("{}", td::td_api::to_string(object));
            });
        }
        else {
            fmt::println("Unsupported action: {}", action);
        }
        return true;
    }

    void loop() {
        while (update());
    }

private:
    Config config_;

    tuntap::tun tun_device_;

    std::atomic<bool> network_thread_running_;
    std::thread network_thread_;
    std::thread tun_thread_;
    std::thread cache_flush_thread_;

    using Object = td::td_api::object_ptr<td::td_api::Object>;
    std::unique_ptr<td::ClientManager> client_manager_;
    std::int32_t client_id_{0};

    td::td_api::object_ptr<td::td_api::AuthorizationState> authorization_state_;
    bool are_authorized_{false};
    bool need_restart_{false};
    std::uint64_t current_query_id_{0};
    std::uint64_t authentication_query_id_{0};

    std::map<std::uint64_t, std::function<void(Object)>> handlers_;
    std::map<std::int64_t, td::td_api::object_ptr<td::td_api::user>> users_;
    std::map<std::int64_t, std::string> chat_title_;

    [[nodiscard]] std::string _get_user_name(std::int64_t user_id) const {
        auto it = users_.find(user_id);
        if (it == users_.end()) {
            return "unknown user";
        }
        return it->second->first_name_ + " " + it->second->last_name_;
    }

    [[nodiscard]] std::string _get_chat_title(std::int64_t chat_id) const {
        auto it = chat_title_.find(chat_id);
        if (it == chat_title_.end()) {
            return "unknown chat";
        }
        return it->second;
    }

    // [[nodiscard]] std::string get_instruction(std::int64_t chat_id, std::int64_t message_id) const {
    //     auto it = instruction_.find(std::make_pair(chat_id, message_id));
    //     if (it == instruction_.end()) {
    //         return "unknown instruction";
    //     }
    //     return it->second;
    // }

    void _send_query(td::td_api::object_ptr<td::td_api::Function> f, const std::function<void(Object)> & handler) {
        ++current_query_id_;
        if (handler) {
            handlers_.emplace(current_query_id_, handler);
        }
        client_manager_->send(client_id_, current_query_id_, std::move(f));
    }

    [[deprecated("use _send_query instead")]]
    void _send_message(ssize_t chat_id, ssize_t reply_to_message_id, std::string text, const std::function<void(Object)> & handler) {
        auto send_message = td::td_api::make_object<td::td_api::sendMessage>();
        send_message->chat_id_ = chat_id;
        // send_message->reply_to_message_id_ = reply_to_message_id;
        auto message_options = td::td_api::make_object<td::td_api::messageSendOptions>();
        message_options->disable_notification_ = true;
        send_message->options_ = std::move(message_options);
        auto message_content = td::td_api::make_object<td::td_api::inputMessageText>();
        message_content->text_ = td::td_api::make_object<td::td_api::formattedText>();
        message_content->text_->text_ = std::move(text);
        send_message->input_message_content_ = std::move(message_content);
        _send_query(std::move(send_message), std::move(handler));
    }

    void _process_update(td::td_api::object_ptr<td::td_api::Object> update) {
        auto func = td::overloaded(
            [this](td::td_api::updateAuthorizationState & update_authorization_state) {
                authorization_state_ = std::move(update_authorization_state.authorization_state_);
                _on_authorization_state_update();
            },
            [this](td::td_api::updateNewChat & update_new_chat) {
                chat_title_[update_new_chat.chat_->id_] = update_new_chat.chat_->title_;
            },
            [this](td::td_api::updateChatTitle & update_chat_title) {
                chat_title_[update_chat_title.chat_id_] = update_chat_title.title_;
            },
            [this](td::td_api::updateUser & update_user) {
                auto user_id = update_user.user_->id_;
                users_[user_id] = std::move(update_user.user_);
            },
            // [this](td::td_api::updateNewMessage & update_new_message) {
            //     // fmt::println("{}", to_string(update_new_message));
            //     // if (update_new_message.message_->content_->get_id() != td_api::messageText::ID) {
            //     //     return;
            //     // }
            //     auto chat_id = update_new_message.message_->chat_id_;
            //     if (chat_id != config_.receive_from_user_id) {
            //         return;
            //     }
            //
            //     fmt::println("{}", to_string(update_new_message));
            //     return;
            //
            //
            //     std::string sender_name;
            //     td::td_api::int53 sender_id;
            //     auto func = td::overloaded(
            //         [this, &sender_name, &sender_id](td::td_api::messageSenderUser & user) {
            //             sender_id = user.user_id_;
            //             sender_name = _get_user_name(user.user_id_);
            //         },
            //         [this, &sender_name, &sender_id](td::td_api::messageSenderChat & chat) {
            //             sender_id = chat.chat_id_;
            //             sender_name = _get_chat_title(chat.chat_id_);
            //         });
            //     td::td_api::downcast_call(*update_new_message.message_->sender_id_, func);
            //
            //     std::string text;
            //     if (update_new_message.message_->content_->get_id() == td::td_api::messageText::ID) {
            //         text = static_cast<td::td_api::messageText &>(*update_new_message.message_->content_).text_->text_;
            //     } else {
            //         return;
            //     }
            //
            //     _send_message(chat_id, update_new_message.message_->id_, "#bot #system #error\n```\nmodel not initialized\n```", {});
            //
            //     // send_history_query(chat_id, 0, 0, history_size, false, history_fill, history_done);
            //
            //     std::cout << "Receive message: [chat_id:" << chat_id << "] [from:" << sender_id << "]: " << sender_name << ": " << text << std::endl;
            // },
            // [this](td::td_api::updateMessageSendSucceeded & update_message_send_succeeded) {
            //     auto chat_id = update_message_send_succeeded.message_->chat_id_;
            //     auto message_id = update_message_send_succeeded.message_->id_;
            //     auto old_message_id = update_message_send_succeeded.old_message_id_;
            //     // std::cout << "Receive send: [chat_id:" << chat_id << "] [message_id:" << message_id << "]" << std::endl;
            //
            //     // auto edit_message = td::td_api::make_object<td::td_api::editMessageText>();
            //     // edit_message->chat_id_ = chat_id;
            //     // edit_message->message_id_ = message_id;
            //     // auto message_content = td::td_api::make_object<td::td_api::inputMessageText>();
            //     // message_content->text_ = td::td_api::make_object<td::td_api::formattedText>();
            //     // message_content->text_->text_ = "#bot\n" + prediction;
            //     // edit_message->input_message_content_ = std::move(message_content);
            //     // send_query(std::move(edit_message), {});
            // },
            [](auto & update) {});
        td::td_api::downcast_call(*update, func);

        td::td_api::downcast_call(*update, td::overloaded(
            [this](td::td_api::updateNewMessage & update_new_message) {
                auto chat_id = update_new_message.message_->chat_id_;
                if (chat_id != config_.receive_from_user_id) {
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
                if (sender_id != config_.receive_from_user_id) {
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
                if (text.find(MESSAGE_HEADER) == 0) {
                    // Strip "#iot " from text to get packet
                    auto packet_encoded = std::string_view(text).substr(MESSAGE_HEADER.size());

                    // Decode from base64
                    std::string packet;
                    base91x::decode(packet_encoded, packet);

                    // Send packet to TUN
                    auto b = tun_device_.write(packet.data(), packet.size());
                    if (b != packet.size()) {
                        fmt::println(stderr, "Failed to write to TUN, wrote {} bytes instead of {}", b, packet.size());
                    }
                }

                if (text.find(MESSAGE_CACHED_HEADER) == 0) {
                    // Strip "#iotc " from text to get packet
                    auto packets_encoded = std::string_view(text).substr(MESSAGE_CACHED_HEADER.size());

                    // Decode from base64
                    std::string packets;
                    base91x::decode(packets_encoded, packets);

                    // Read packets from string
                    std::istringstream iss(packets);
                    while (iss) {
                        uint16_t length;
                        iss.read(reinterpret_cast<char*>(&length), sizeof(length));

                        std::string packet(length, '\0');
                        iss.read(&packet[0], length);

                        // Send packet to TUN
                        auto b = tun_device_.write(packet.data(), packet.size());
                        if (b != packet.size()) {
                            fmt::println(stderr, "Failed to write to TUN");
                        }
                    }
                }
            },
            [](auto & update) {}));
    }

    void _process_response(td::ClientManager::Response response) {
        if (!response.object) {
            return;
        }
        // std::cout << response.request_id << " " << to_string(response.object) << std::endl;
        if (response.request_id == 0) {
            return _process_update(std::move(response.object));
        }
        auto it = handlers_.find(response.request_id);
        if (it != handlers_.end()) {
            it->second(std::move(response.object));
            handlers_.erase(it);
        }
    }

    //
    // Authentication
    //
    void _check_authentication_error(Object object) {
        if (object->get_id() == td::td_api::error::ID) {
            auto error = td::move_tl_object_as<td::td_api::error>(object);
            fmt::println("Error: {}", to_string(error));
            _on_authorization_state_update();
        }
    }

    auto _create_authentication_query_handler() {
        return [this, id = authentication_query_id_](Object object) {
            if (id == authentication_query_id_) {
                _check_authentication_error(std::move(object));
            }
        };
    }

    void _on_authorization_state_update() {
        authentication_query_id_++;
        auto func = td::overloaded(
            [this](td::td_api::authorizationStateReady &) {
                are_authorized_ = true;
                fmt::println("Authorization is completed");
                start();
            },
            [this](td::td_api::authorizationStateLoggingOut &) {
                are_authorized_ = false;
                fmt::println("Logging out");
            },
            [this](td::td_api::authorizationStateClosing &) {
                fmt::println("Closing");
            },
            [this](td::td_api::authorizationStateClosed &) {
                are_authorized_ = false;
                need_restart_ = true;
                fmt::println("Terminated");
            },
            [this](td::td_api::authorizationStateWaitPhoneNumber &) {
                std::string phone_number;
                fmt::print("Enter phone number: ");
                if (!config_.tdconfig.token.empty()) {
                    phone_number = config_.tdconfig.token;
                    fmt::println("{} (loaded from config)", phone_number);
                } else {
                    std::cin >> phone_number;
                }
                _send_query(td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>(phone_number, nullptr),
                            _create_authentication_query_handler());
            },
            [this](td::td_api::authorizationStateWaitEmailAddress &) {
                fmt::print("Enter email address: ");
                std::string email_address;
                std::cin >> email_address;
                _send_query(td::td_api::make_object<td::td_api::setAuthenticationEmailAddress>(email_address),
                            _create_authentication_query_handler());
            },
            [this](td::td_api::authorizationStateWaitEmailCode &) {
                fmt::print("Enter email authentication code: ");
                std::string code;
                std::cin >> code;
                _send_query(td::td_api::make_object<td::td_api::checkAuthenticationEmailCode>(td::td_api::make_object<td::td_api::emailAddressAuthenticationCode>(code)),
                            _create_authentication_query_handler());
            },
            [this](td::td_api::authorizationStateWaitCode &) {
                fmt::print("Enter authentication code: ");
                std::string code;
                std::cin >> code;
                _send_query(td::td_api::make_object<td::td_api::checkAuthenticationCode>(code),
                            _create_authentication_query_handler());
            },
            [this](td::td_api::authorizationStateWaitRegistration &) {
                std::string first_name;
                std::string last_name;
                fmt::print("Enter your first name: ");
                std::cin >> first_name;
                fmt::print("Enter your last name: ");
                std::cin >> last_name;
                _send_query(td::td_api::make_object<td::td_api::registerUser>(first_name, last_name),
                            _create_authentication_query_handler());
            },
            [this](td::td_api::authorizationStateWaitPassword &) {
                fmt::print("Enter authentication password: ");
                std::string password;
                std::getline(std::cin, password);
                _send_query(td::td_api::make_object<td::td_api::checkAuthenticationPassword>(password),
                            _create_authentication_query_handler());
            },
            [this](td::td_api::authorizationStateWaitOtherDeviceConfirmation & state) {
                fmt::println("Confirm this login link on another device: {}", state.link_);
            },
            [this](td::td_api::authorizationStateWaitTdlibParameters &) {
                auto request = td::td_api::make_object<td::td_api::setTdlibParameters>();
                request->database_directory_ = "tdlib";
                request->use_message_database_ = true;
                request->use_secret_chats_ = true;
                request->api_id_ = config_.tdconfig.api_id;
                request->api_hash_ = config_.tdconfig.api_hash;
                request->system_language_code_ = "en";
                request->device_model_ = "Desktop";
                request->application_version_ = "1.0";
                request->enable_storage_optimizer_ = true;
                _send_query(std::move(request), _create_authentication_query_handler());
            });
        td::td_api::downcast_call(*authorization_state_, func);
    }
};


int main(int argc, char * argv[], [[maybe_unused]] char * envp[]) {
    namespace po = boost::program_options;

    try {
        po::options_description desc("Allowed options");
        desc.add_options()
            ("config", po::value<std::string>(), "path to config file")
            ("help", "show help message and exit as")
            ;

        po::positional_options_description p;
        p.add("config", 1);

        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::ostringstream oss;
            oss << desc;
            fmt::println(stderr, "{}", oss.str());
            return 1;
        }

        if (vm.count("config")) {
            auto config_path = vm["config"].as<std::string>();
            std::ifstream ifs(config_path);
            auto yaml_str = std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
            Config config(yaml_str);

            TdClient tc(config);
            tc.loop();
        } else {
            fmt::println(stderr, "ASD Config path was not set.");
        }
    } catch (std::exception & e) {
        fmt::println(stderr, "error: {}", e.what());
        return 1;
    } catch (...) {
        fmt::println(stderr, "Exception of unknown type!");
    }
}
