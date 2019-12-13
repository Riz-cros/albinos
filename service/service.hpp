//
// Created by milerius on 19/10/18.
//

#pragma once

#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <uvw.hpp>
#include <uv.h>
#include <loguru.hpp>
#include "client.hpp"
#include "protocol.hpp"
#include "db.hpp"

namespace raven
{
  class service
  {
  public:
    explicit service(std::filesystem::path db_path = std::filesystem::current_path() / "albinos_service.db") noexcept
    : db_{std::move(db_path)}
    {
        VLOG_SCOPE_F(loguru::Verbosity_INFO, "service constructor");
        DVLOG_F(loguru::Verbosity_INFO, "registering error_event libuv listener");
        server_->on<uvw::ErrorEvent>([this](auto const &error_event, auto &) {
            LOG_SCOPE_F(ERROR, __PRETTY_FUNCTION__);
            DVLOG_F(loguru::Verbosity_ERROR, "%s", error_event.what());
            this->error_occurred = true;
        });
        DVLOG_F(loguru::Verbosity_INFO, "registering listen_event libuv listener");
        server_->on<uvw::ListenEvent>([this](uvw::ListenEvent const &, uvw::PipeHandle &handle) {
            LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
            std::shared_ptr<uvw::PipeHandle> socket = handle.loop().resource<uvw::PipeHandle>();
            DVLOG_F(loguru::Verbosity_INFO, "registering close_event libuv listener");
            socket->on<uvw::CloseEvent>([this](uvw::CloseEvent const &, uvw::PipeHandle &handle) {
                LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
                DVLOG_F(loguru::Verbosity_INFO, "socket closed.");
                handle.close();
#ifdef DOCTEST_LIBRARY_INCLUDED
                this->uv_loop_->stop();
#endif
            });

            DVLOG_F(loguru::Verbosity_INFO, "registering end_event libuv listener");
            socket->on<uvw::EndEvent>([this](const uvw::EndEvent &, uvw::PipeHandle &sock) {
                LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
                DVLOG_F(loguru::Verbosity_INFO, "closing socket: %d", static_cast<int>(sock.fileno()));
                //! Since the client will disconnect, we unload every config related to him
                DVLOG_F(loguru::Verbosity_INFO, "unload every config for the client -> %d",
                        static_cast<int>(sock.fileno()));
                for (size_t i = 1; i <= this->config_clients_registry_.at(sock.fileno()).get_last_id().value(); ++i) {
                    if (this->config_clients_registry_.at(sock.fileno()).has_loaded(config_id_st{i})) {
                      config_id_st db_id = this->config_clients_registry_.at(sock.fileno()).get_db_id_from(config_id_st{i});
                      load_ref_counter_[db_id.value()] -= 1;
                      if (load_ref_counter_.at(db_id.value()) <= 0) {
                          if (destroyed_configs_.find(db_id.value()) != destroyed_configs_.end()) {
                              db_.config_destroy(db_id);
                              destroyed_configs_.erase(db_id.value());
                          } else
                              db_.update_config(loaded_configs_.at(db_id.value()), db_id);
                          loaded_configs_.erase(db_id.value());
                          load_ref_counter_.erase(db_id.value());
                      }
                    }
                }
                this->config_clients_registry_.erase(sock.fileno());
                sock.close();
            });

            DVLOG_F(loguru::Verbosity_INFO, "registering data_event libuv listener");
            socket->on<uvw::DataEvent>([this](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
                std::string_view data_str(data.data.get(), data.length);
                try {
                    auto json_data = json::json::parse(data_str);
                    auto command_order = json_data.at(raven::request_keyword).get<std::string>();
                    order_registry.at(command_order)(json_data, sock);
                }
                catch (const std::out_of_range &error) {
                    DVLOG_F(loguru::Verbosity_ERROR, "error in received data: %s", error.what());
                    json::json unknown_request_data = R"(
                                                          {"REQUEST_STATE": "UNKNOWN_REQUEST"}
                                                        )"_json;
                    sock.write(unknown_request_data.dump().data(),
                               static_cast<unsigned int>(unknown_request_data.dump().size()));
                }
                catch (const std::exception &error) {
                    DVLOG_F(loguru::Verbosity_ERROR, "error in received data: %s", error.what());
                    json::json internal_error_request_data = R"(
                                                          {"REQUEST_STATE": "INTERNAL_ERROR"}
                                                        )"_json;
                    sock.write(internal_error_request_data.dump().data(),
                               static_cast<unsigned int>(internal_error_request_data.dump().size()));
                }
            });

            handle.accept(*socket);
            config_clients_registry_.emplace(socket->fileno(), raven::client(socket));
            socket->read();
        });
    }

    ~service() noexcept
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        DVLOG_F(loguru::Verbosity_INFO, "destroy service");
    }

    void run() noexcept
    {
        clean_socket();
        create_socket();
        run_loop();
    }

  private:
    void run_loop()
    {
        uv_loop_->run();
    }

    bool clean_socket() noexcept
    {
        if (std::filesystem::exists(socket_path_)) {
            LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
            DVLOG_F(loguru::Verbosity_WARNING, "socket: %s already exist, removing", socket_path_.string().c_str());
            std::filesystem::remove(socket_path_);
            return true;
        }
        return false;
    }

    bool create_socket() noexcept
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        std::string socket = socket_path_.string();
        DVLOG_F(loguru::Verbosity_INFO, "binding to socket: %s", socket.c_str());
        server_->bind(socket);
        DLOG_IF_F(ERROR, this->error_occurred, "an error occurred during the bind");
        if (this->error_occurred) return this->error_occurred;
        server_->listen();
        DLOG_IF_F(ERROR, this->error_occurred, "an error occurred during the listen");
        return this->error_occurred;
    }

    //! Helpers
    static void send_json_answer(json::json &response_json_data, uvw::PipeHandle &sock) noexcept
    {
        auto response_str = response_json_data.dump();
        sock.write(response_str.data(), static_cast<unsigned int>(response_str.size()));
    }

    template <typename ProtocolType>
    static void send_answer(uvw::PipeHandle &sock, const ProtocolType &answer) noexcept
    {
        json::json response_json_data;
        to_json(response_json_data, answer);
        send_json_answer(response_json_data, sock);
    }

    static void send_answer(uvw::PipeHandle &sock, request_state state = request_state::success) noexcept
    {
        json::json response_json_data;
        response_json_data[request_state_keyword] = convert_request_state.at(state);
        send_json_answer(response_json_data, sock);
    }

    template <typename Request>
    static Request fill_request(json::json &json_data)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        Request request;
        from_json(json_data, request);
        DLOG_F(INFO, "json receive:%s", json_data.dump().c_str());
        return request;
    }

    void create_config(json::json &json_data, uvw::PipeHandle &sock)
    {
        auto cfg = fill_request<config_create>(json_data);
        auto config_create_db_result = db_.config_create(json_data[config_name_keyword]);
        if (db_.good()) {
            const config_create_answer answer{config_create_db_result.config_key,
                                              config_create_db_result.readonly_config_key,
                                              convert_request_state.at(request_state::success)};
            send_answer(sock, answer);
        } else {
            const config_create_answer answer{config_key_st{}, config_key_st{},
                                              convert_request_state.at(request_state::db_error)};
            send_answer(sock, answer);
        }
    }

    void destroy_config(json::json &json_data, uvw::PipeHandle &sock)
    {
      LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
      auto cfg = fill_request<config_destroy>(json_data);
      DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
      if (!config_clients_registry_.at(sock.fileno()).has_loaded(cfg.id)) {
          send_answer(sock, request_state::unknown_id);
          return ;
      }

      if (config_clients_registry_.at(sock.fileno()).get_permission(raven::config_id_st{cfg.id}) != config_permission::readwrite) {
          send_answer(sock, request_state::unauthorized);
          return ;
      }

      auto db_id = config_clients_registry_.at(sock.fileno()).get_db_id_from(cfg.id);
      destroyed_configs_.insert(db_id.value());
      config_clients_registry_.at(sock.fileno()).remove_temp_id(cfg.id);
      load_ref_counter_[db_id.value()] -= 1;
      if (load_ref_counter_.at(db_id.value()) <= 0) {
          auto &config = loaded_configs_.at(db_id.value())[config_includes_field_keyword];
          for(auto i = config.rbegin(); i != config.rend(); ++i) {
              _deep_unload(config_id_st{*i});
          }
          if (destroyed_configs_.find(db_id.value()) != destroyed_configs_.end()) {
              db_.config_destroy(db_id);
              destroyed_configs_.erase(db_id.value());
          } else
              db_.update_config(loaded_configs_.at(db_id.value()), db_id);
          loaded_configs_.erase(db_id.value());
          load_ref_counter_.erase(db_id.value());
      }
      if (db_.fail())
        send_answer(sock, request_state::db_error);
      else
        send_answer(sock);
    }

    void load_config(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<config_load>(json_data);
        DLOG_IF_F(INFO, cfg.config_key.has_value(), "cfg.config_key: %s", cfg.config_key.value().value().c_str());
        DLOG_IF_F(INFO, cfg.config_read_only_key.has_value(), "cfg.config_read_only_key: %s",
                  cfg.config_read_only_key.value().value().c_str());

        config_id_st id;
        config_permission perm;
        if (cfg.config_key) {
            auto pair = db_.get_config_id(cfg.config_key.value());
            id = pair.first;
            perm = pair.second;
        } else if (cfg.config_read_only_key) {
            auto pair = db_.get_config_id(cfg.config_read_only_key.value());
            id = pair.first;
            perm = pair.second;
        } else {
            send_answer(sock, request_state::unknown_request);
            return ;
        }
        if (db_.fail()) {
            switch(db_.get_state()) {
                case db_state::unknow_config_key:
                    send_answer(sock, request_state::unknown_key);
                    break;
                default:
                    send_answer(sock, request_state::db_error);
                    break;
            }
            return ;
        }

        if (destroyed_configs_.find(id.value()) != destroyed_configs_.end()) {
            send_answer(sock, request_state::unknown_key);
            return ;
        }
        std::string name = db_.get_config_name(id);
        if (db_.fail()) {
            send_answer(sock, request_state::db_error);
            return ;
        }

        if (!config_clients_registry_.at(sock.fileno()).has_loaded_db_id(id)) {
            /*if (load_ref_counter_.find(id.value()) != load_ref_counter_.end()) {
                load_ref_counter_[id.value()] +=  1;
            } else {
                load_ref_counter_.insert({id.value(), 1});
                loaded_configs_.insert({id.value(), db_.get_config(id)});
            }*/
            std::unordered_set<int> loaded;
            if(!_deep_load(id, loaded)) {
                send_answer(sock, request_state::unauthorized);
                return ;
            }
            config_id_st temp_id = config_clients_registry_.at(sock.fileno()).insert_db_id(id, perm);
            send_answer(sock, config_load_answer{name, temp_id, convert_request_state.at(request_state::success)});
        } else {
            config_id_st temp_id = config_clients_registry_.at(sock.fileno()).get_id_from_db(id);
            send_answer(sock, config_load_answer{name, temp_id, convert_request_state.at(request_state::success)});
        }
    }

    void unload_config(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<config_unload>(json_data);
        if (!config_clients_registry_.at(sock.fileno()).has_loaded(cfg.id))
        {
          send_answer(sock, request_state::unknown_id);
          return ;
        }
        config_id_st db_id = config_clients_registry_.at(sock.fileno()).get_db_id_from(cfg.id);
        config_clients_registry_.at(sock.fileno()).remove_temp_id(cfg.id);
        load_ref_counter_[db_id.value()] -= 1;
        if (load_ref_counter_.at(db_id.value()) <= 0) {
            auto &config = loaded_configs_.at(db_id.value())[config_includes_field_keyword];
            for(auto i = config.rbegin(); i != config.rend(); ++i) {
              _deep_unload(config_id_st{*i});
            }
            if (destroyed_configs_.find(db_id.value()) != destroyed_configs_.end()) {
                db_.config_destroy(db_id);
                destroyed_configs_.erase(db_id.value());
            } else
                db_.update_config(loaded_configs_.at(db_id.value()), db_id);
            loaded_configs_.erase(db_id.value());
            load_ref_counter_.erase(db_id.value());
        }
        if (db_.fail())
          send_answer(sock, request_state::db_error);
        else
          send_answer(sock);
    }

    void _deep_unload(config_id_st to_unload)
    {
        load_ref_counter_[to_unload.value()] -= 1;
        if (load_ref_counter_.at(to_unload.value()) <= 0) {
            auto &config = loaded_configs_.at(to_unload.value())[config_includes_field_keyword];
            for(auto i = config.rbegin(); i != config.rend(); ++i) {
                _deep_unload(config_id_st{*i});
            }
            if (destroyed_configs_.find(to_unload.value()) != destroyed_configs_.end()) {
                db_.config_destroy(to_unload);
                destroyed_configs_.erase(to_unload.value());
            } else
                db_.update_config(loaded_configs_.at(to_unload.value()), config_id_st{to_unload});
            loaded_configs_.erase(to_unload.value());
            load_ref_counter_.erase(to_unload.value());
        }
    }

    bool _deep_check(config_id_st to_load, std::unordered_set<int> &loaded)
    {
      auto &config = loaded_configs_.at(to_load.value())[config_includes_field_keyword];
        for(auto i = config.rbegin(); i != config.rend(); ++i) {
            if (loaded.find(*i) != loaded.end() || !_deep_check(config_id_st{*i}, loaded))
               return false;
        }
        return true;
    }

    bool _deep_load(config_id_st to_load, std::unordered_set<int> &loaded)
    {
        bool failure = false;
        if (loaded.find(to_load.value()) != loaded.end()) {
           return false;
        }

        loaded.insert(to_load.value());
        if (load_ref_counter_.find(to_load.value()) != load_ref_counter_.end()) {
            if (!_deep_check(to_load, loaded))
                return false;
            load_ref_counter_[to_load.value()] +=  1;
        } else {
            load_ref_counter_.insert({to_load.value(), 1});
            loaded_configs_.insert({to_load.value(), db_.get_config(to_load)});
            auto &config = loaded_configs_.at(to_load.value())[config_includes_field_keyword];
            for (auto i = config.rbegin(); i != config.rend(); ++i) {
                if (!_deep_load(config_id_st{*i}, loaded)) {
                    for(auto j = i.base() + 1; j != config.end(); ++j) {
                        _deep_unload(config_id_st{*j});
                    }
                    failure = true;
                    break ;
                }
            }
            if (failure) {
                load_ref_counter_[to_load.value()] -= 1;
                if (load_ref_counter_.at(to_load.value()) <= 0) {
                    if (destroyed_configs_.find(to_load.value()) != destroyed_configs_.end()) {
                        db_.config_destroy(to_load);
                        destroyed_configs_.erase(to_load.value());
                    } else
                        db_.update_config(loaded_configs_.at(to_load.value()), config_id_st{to_load});
                    loaded_configs_.erase(to_load.value());
                    load_ref_counter_.erase(to_load.value());
                }
            }
        }
        loaded.erase(to_load.value());
        return !failure;
    }

    void include_config(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<config_include>(json_data);
        DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
        DLOG_F(INFO, "cfg.src_id: %lu", cfg.src_id.value());

        if (!config_clients_registry_.at(sock.fileno()).has_loaded(raven::config_id_st{cfg.id}) ||
            !config_clients_registry_.at(sock.fileno()).has_loaded(raven::config_id_st{cfg.src_id})) {
            send_answer(sock, request_state::unknown_id);
            return ;
        }

        auto db_id = config_clients_registry_.at(sock.fileno()).get_db_id_from(cfg.id);
        config_id_st db_id_to_include = config_clients_registry_.at(sock.fileno()).get_db_id_from({cfg.src_id});

        if (config_clients_registry_.at(sock.fileno()).get_permission(raven::config_id_st{cfg.id}) != config_permission::readwrite ||
            db_id == db_id_to_include) {
            send_answer(sock, request_state::unauthorized);
            return ;
        }

        auto &config_json_data = loaded_configs_.at(db_id.value());
        /*if (load_ref_counter_.find(db_id_to_include.value()) != load_ref_counter_.end()) {
          load_ref_counter_[db_id_to_include.value()] +=  1;
        } else {
          load_ref_counter_.insert({db_id_to_include.value(), 1});
          loaded_configs_.insert({db_id_to_include.value(), db_.get_config(db_id_to_include)});
        }*/
        std::unordered_set<int> loaded;
        if(!_deep_load(db_id_to_include, loaded)) {
            send_answer(sock, request_state::unauthorized);
            return ;
        }
        config_json_data[config_includes_field_keyword].push_back(db_id_to_include.value());
        send_answer(sock, request_state::success);
    }

    void uninclude_config(json::json &json_data, uvw::PipeHandle &sock)
    {
      LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
      auto cfg = fill_request<config_include>(json_data);
      DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
      DLOG_F(INFO, "cfg.src_id: %lu", cfg.src_id.value());

      if (!config_clients_registry_.at(sock.fileno()).has_loaded(raven::config_id_st{cfg.id}) ||
          !config_clients_registry_.at(sock.fileno()).has_loaded(raven::config_id_st{cfg.src_id})) {
        send_answer(sock, request_state::unknown_id);
        return ;
      }

      auto db_id = config_clients_registry_.at(sock.fileno()).get_db_id_from(cfg.id);
      config_id_st db_id_to_uninclude = config_clients_registry_.at(sock.fileno()).get_db_id_from({cfg.src_id});

      auto &config_json_data = loaded_configs_.at(db_id.value());
      /*if (load_ref_counter_.find(db_id_to_include.value()) != load_ref_counter_.end()) {
        load_ref_counter_[db_id_to_include.value()] +=  1;
      } else {
        load_ref_counter_.insert({db_id_to_include.value(), 1});
        loaded_configs_.insert({db_id_to_include.value(), db_.get_config(db_id_to_include)});
      }*/
      _deep_unload(db_id_to_uninclude);
      auto it = std::find(config_json_data[config_includes_field_keyword].begin(), config_json_data[config_includes_field_keyword].end(), db_id_to_uninclude.value());
      if (it != config_json_data[config_includes_field_keyword].end())
          config_json_data[config_includes_field_keyword].erase(it);
      send_answer(sock, request_state::success);
    }

    void update_setting(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<setting_update>(json_data);
        DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
        DLOG_F(INFO, "settings_to_update: %s", cfg.settings_to_update.dump().c_str());
        if (!config_clients_registry_.at(sock.fileno()).has_loaded(cfg.id)) {
            send_answer(sock, request_state::unknown_id);
            return;
        }

        if (config_clients_registry_.at(sock.fileno()).get_permission(raven::config_id_st{cfg.id}) != config_permission::readwrite) {
            send_answer(sock, request_state::unauthorized);
            return ;
        }

        auto db_id = config_clients_registry_.at(sock.fileno()).get_db_id_from(cfg.id);

        /*
        auto config_json_data = db_.get_config(db_id);
        if (db_.fail()) {
            send_answer(sock, request_state::db_error);
            return ;
        }
         */
        auto &config_json_data = loaded_configs_.at(db_id.value());

        for (auto &[key, value] : cfg.settings_to_update.items()) {
            config_json_data[config_settings_field_keyword][key] = value;
        }
        DLOG_F(INFO, "config after update: %s", config_json_data.dump().c_str());

        /*
        db_.update_config(config_json_data, db_id);
        if (db_.fail()) {
            send_answer(sock, request_state::db_error);
            return ;
        }
         */

        send_answer(sock, request_state::success);
        // TODO : lookup de la db pour associer l'id temporaire du client qui a update au vrai id dans la db puis retrouver l'id temporaire du client courant dans la loop associer a ce vrai id
        // Workaround : get db_id from the client class
        for (auto &[fileno, client] : config_clients_registry_)
        {
            for (auto&[key, value] : cfg.settings_to_update.items()) {

                if (client.is_subscribed(db_id, key)) {
                    const subscribe_event answer{client.get_id_from_db(db_id),
                                                 key,
                                                 subscribe_event_type::update_setting};
                    send_answer(*client.get_socket(), answer);
                }
            }
        }
    }

    void remove_setting(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<setting_remove>(json_data);
        DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
        DLOG_F(INFO, "cfg.setting_name: %s", cfg.setting_name.c_str());
        // TODO : lookup de la db pour associer l'id temporaire du client qui a update au vrai id dans la db puis retrouver l'id temporaire du client courant dans la loop associer a ce vrai id
        // Workaround : get db_id from the client class
        if (!config_clients_registry_.at(sock.fileno()).has_loaded(cfg.id)) {
            send_answer(sock, request_state::unknown_id);
            return;
        }

        if (config_clients_registry_.at(sock.fileno()).get_permission(raven::config_id_st{cfg.id}) != config_permission::readwrite) {
            send_answer(sock, request_state::unauthorized);
            return ;
        }

        auto db_id = config_clients_registry_.at(sock.fileno()).get_db_id_from(cfg.id);

        auto &config_json_data = loaded_configs_.at(db_id.value());
        if (config_json_data[config_settings_field_keyword].erase(cfg.setting_name) == 0)
          send_answer(sock, request_state::unknown_setting);
        else
          send_answer(sock);

        for (auto &[fileno, client] : config_clients_registry_)
        {
            if (client.is_subscribed(db_id, cfg.setting_name)) {
                const subscribe_event answer{client.get_id_from_db(db_id),
                                             cfg.setting_name,
                                             subscribe_event_type::delete_setting};
                send_answer(*client.get_socket(), answer);
            }
        }
    }

    bool _deep_get(config_id_st db_id, const std::string &setting, std::string &answer)
    {
        const auto &config_json_data = loaded_configs_.at(db_id.value());
        if(config_json_data[config_settings_field_keyword].find(setting) != config_json_data[config_settings_field_keyword].end()) {
            answer = config_json_data[config_settings_field_keyword].at(setting);
            return true;
        }
        for(auto i = config_json_data[config_includes_field_keyword].rbegin(); i != config_json_data[config_includes_field_keyword].rend(); ++i) {
            if (_deep_get(config_id_st{*i}, setting, answer)) {
              return true;
            }
        }
        return false;
    }

    void get_setting(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<setting_get>(json_data);
        DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
        DLOG_F(INFO, "cfg.setting_name: %s", cfg.setting_name.c_str());
        if (!config_clients_registry_.at(sock.fileno()).has_loaded(raven::config_id_st{cfg.id})) {
            send_answer(sock, request_state::unknown_id);
            return ;
        }
        auto db_id = config_clients_registry_.at(sock.fileno()).get_db_id_from(cfg.id);
        //const auto &config_json_data = loaded_configs_.at(db_id.value());

        setting_get_answer answer;
        /*try {
            answer.setting_value = config_json_data[config_settings_field_keyword].at(cfg.setting_name);
        }
        catch (const json::json::out_of_range &error) {
            send_answer(sock, request_state::unknown_setting);
            return ;
        }*/
        if (!_deep_get(db_id, cfg.setting_name, answer.setting_value)){
            send_answer(sock, request_state::unknown_setting);
            return ;
        }
        answer.request_state = convert_request_state.at(request_state::success);
        send_answer(sock, answer);
    }

    void _deep_get_all(config_id_st db_id, json::json &json_data)
    {
        const auto &config_json_data = loaded_configs_.at(db_id.value());
        for(auto i = config_json_data[config_includes_field_keyword].rbegin(); i != config_json_data[config_includes_field_keyword].rend(); ++i) {
            for(auto &[setting, value] : loaded_configs_.at(*i)[config_settings_field_keyword].items()) {
                if(json_data.find(setting) == json_data.end()) {
                    json_data[setting] = value;
                }
            }
            _deep_get_all(config_id_st{*i}, json_data);
        }
    }

    void get_settings_names(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<config_get_settings_names>(json_data);
        DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
        if (!config_clients_registry_.at(sock.fileno()).has_loaded(raven::config_id_st{cfg.id})) {
            send_answer(sock, request_state::unknown_id);
            return ;
        }
        /*
        auto config_json_data = db_.get_config(config_clients_registry_.at(sock.fileno()).get_db_id_from(raven::config_id_st{cfg.id}));
        if (db_.fail()) {
            send_answer(sock, request_state::db_error);
            return ;
        }
         */
        auto db_id = config_clients_registry_.at(sock.fileno()).get_db_id_from(cfg.id);
        const auto &config_json_data = loaded_configs_.at(db_id.value());

        json::json all_configs = config_json_data[config_settings_field_keyword];
        _deep_get_all(db_id, all_configs);
        json::json settings_name;
        for (auto &[key, value] : all_configs.items()) {
            settings_name.push_back(key);
        }
        /*for (auto &[key, value] : config_json_data[config_settings_field_keyword].items()) {
            settings_name.push_back(key);
        }*/

        config_get_settings_names_answer answer{std::move(settings_name), convert_request_state.at(request_state::success)};
        send_answer(sock, answer);
    }

    void get_all_settings(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<config_get_settings>(json_data);
        DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
        if (!config_clients_registry_.at(sock.fileno()).has_loaded(raven::config_id_st{cfg.id})) {
            send_answer(sock, request_state::unknown_id);
            return ;
        }
        /*
        auto config_json_data = db_.get_config(config_clients_registry_.at(sock.fileno()).get_db_id_from(raven::config_id_st{cfg.id}));
        if (db_.fail()) {
            send_answer(sock, request_state::db_error);
            return ;
        }
         */
        auto db_id = config_clients_registry_.at(sock.fileno()).get_db_id_from(cfg.id);
        const auto &config_json_data = loaded_configs_.at(db_id.value());
        json::json all_configs = config_json_data[config_settings_field_keyword];
        _deep_get_all(db_id, all_configs);
        config_get_settings_answer answer{all_configs, convert_request_state.at(request_state::success)};
        //config_get_settings_answer answer{config_json_data[config_settings_field_keyword], convert_request_state.at(request_state::success)};
        send_answer(sock, answer);
    }

    void set_alias(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<alias_set>(json_data);
        DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
        DLOG_F(INFO, "cfg.alias_name: %s", cfg.alias_name.c_str());
        DLOG_F(INFO, "cfg.setting_name: %s", cfg.setting_name.c_str());
        send_answer(sock);
    }

    void unset_alias(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<alias_unset>(json_data);
        DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
        DLOG_F(INFO, "cfg.alias_name: %s", cfg.alias_name.c_str());
        send_answer(sock);
    }

    void subscribe_setting(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<setting_subscribe>(json_data);
        DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
        DLOG_IF_F(INFO, cfg.setting_name.has_value(), "cfg.setting_name: %s", cfg.setting_name.value().c_str());
        DLOG_IF_F(INFO, cfg.alias_name.has_value(), "cfg.alias_name: %s", cfg.alias_name.value().c_str());
        if (!config_clients_registry_.at(sock.fileno()).has_loaded(cfg.id)) {
            send_answer(sock, request_state::unknown_id);
            return ;
        }
        if (cfg.setting_name.has_value())
        {
            config_clients_registry_.at(sock.fileno()).subscribe(cfg.id, cfg.setting_name.value());
            // TODO
            // if setting doesn't exist in config
            //      send_answer(sock, request_state::unknown_setting);
            // else
            send_answer(sock);
        } else // TODO handle alias case
            send_answer(sock, request_state::internal_error);
    }

    void unsubscribe_setting(json::json &json_data, uvw::PipeHandle &sock)
    {
        LOG_SCOPE_F(INFO, __PRETTY_FUNCTION__);
        auto cfg = fill_request<setting_subscribe>(json_data);
        DLOG_F(INFO, "cfg.id: %lu", cfg.id.value());
        DLOG_IF_F(INFO, cfg.setting_name.has_value(), "cfg.setting_name: %s", cfg.setting_name.value().c_str());
        DLOG_IF_F(INFO, cfg.alias_name.has_value(), "cfg.alias_name: %s", cfg.alias_name.value().c_str());
        if (!config_clients_registry_.at(sock.fileno()).has_loaded(cfg.id)) {
            send_answer(sock, request_state::unknown_id);
            return ;
        }
        if (cfg.setting_name.has_value())
        {
            config_clients_registry_.at(sock.fileno()).unsubscribe(cfg.id, cfg.setting_name.value());
            send_answer(sock);
        } else // TODO handle alias case
            send_answer(sock, request_state::internal_error);
    }

    std::shared_ptr<uvw::Loop> uv_loop_{uvw::Loop::getDefault()};
    std::shared_ptr<uvw::PipeHandle> server_{uv_loop_->resource<uvw::PipeHandle>()};
    std::filesystem::path socket_path_{(std::filesystem::temp_directory_path() / "raven-os_service_albinos.sock")};
    std::unordered_map<uvw::OSFileDescriptor::Type, raven::client> config_clients_registry_;
    std::unordered_map<config_id_st::value_type, unsigned int> load_ref_counter_;
    std::unordered_map<config_id_st::value_type, json::json> loaded_configs_;
    std::unordered_set<config_id_st::value_type> destroyed_configs_;
    config_db db_;
    bool error_occurred{false};
    const std::unordered_map<std::string, std::function<void(json::json &, uvw::PipeHandle &)>>
        order_registry
        {
            {
                "CONFIG_CREATE",       [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->create_config(json_data, sock);
            }},
            {
                "CONFIG_DESTROY",       [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->destroy_config(json_data, sock);
            }},
            {
                "CONFIG_LOAD",         [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->load_config(json_data, sock);
            }},
            {
                "CONFIG_UNLOAD",       [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->unload_config(json_data, sock);
            }},
            {
                "CONFIG_INCLUDE",      [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->include_config(json_data, sock);
            }},
            {
                "CONFIG_UNINCLUDE",      [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->uninclude_config(json_data, sock);
            }},
            {
                "SETTING_UPDATE",      [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->update_setting(json_data, sock);
            }},
            {
                "SETTING_REMOVE",      [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->remove_setting(json_data, sock);
            }},
            {
                "SETTING_GET",         [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->get_setting(json_data, sock);
            }},
            {
                "CONFIG_GET_SETTINGS",         [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->get_all_settings(json_data, sock);
            }},
            {
                "CONFIG_GET_SETTINGS_NAMES",         [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->get_settings_names(json_data, sock);
            }},
            {
                "ALIAS_SET",           [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->set_alias(json_data, sock);
            },
            },
            {
                "ALIAS_UNSET",         [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->unset_alias(json_data, sock);
            },
            },
            {
                "SUBSCRIBE_SETTING",   [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->subscribe_setting(json_data, sock);
            },
            },
            {
                "UNSUBSCRIBE_SETTING", [this](json::json &json_data, uvw::PipeHandle &sock) {
                this->unsubscribe_setting(json_data, sock);
            },
            }
        };

#ifdef DOCTEST_LIBRARY_INCLUDED
    TEST_CASE_CLASS ("test create socket")
    {
        service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
        if (std::filesystem::exists(service_.socket_path_)) {
            std::filesystem::remove(service_.socket_path_);
        }
        CHECK_FALSE(service_.create_socket());
        CHECK(service_.create_socket());
        CHECK(service_.clean_socket());
        std::filesystem::remove(std::filesystem::current_path() / "albinos_service_test_internal.db");
    }

    TEST_CASE_CLASS ("test clean socket")
    {
        service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
        CHECK_FALSE(service_.clean_socket());
        CHECK_FALSE(service_.create_socket());
        CHECK(service_.clean_socket());
        std::filesystem::remove(std::filesystem::current_path() / "albinos_service_test_internal.db");
    }

    static void
    test_setup_client(const nlohmann::json &request, const nlohmann::json &expected_answer, bool consider_only_state,
                      const service &service_, std::shared_ptr<uvw::PipeHandle> &client) noexcept
    {
        client->once<uvw::ConnectEvent>([&request](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
            CHECK(handle.writable());
            CHECK(handle.readable());
            auto request_str = request.dump();
            handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
            handle.read();
        });

        client->once<uvw::DataEvent>(
            [&expected_answer, &consider_only_state](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                std::string_view data_str(data.data.get(), data.length);
                auto json_data = json::json::parse(data_str);
                auto json_data_str = json_data.dump();
                if (!consider_only_state) {
                    CHECK(json_data == expected_answer);
                } else {
                    CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                    expected_answer.at("REQUEST_STATE").get<std::string>());
                }
                sock.close();
            });

        client->connect(service_.socket_path_.string());
    }

    static void test_run_and_clean_client(service &service_, std::shared_ptr<uvw::Loop> &loop) noexcept
    {
        loop->run();
        service_.clean_socket();
        std::filesystem::remove(std::filesystem::current_path() / "albinos_service_test_internal.db");
    }

    static void test_client_server_communication(json::json &&request, json::json &&expected_answer,
                                                 bool consider_only_state = false) noexcept
    {
        service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
        CHECK_FALSE(service_.create_socket());
        auto loop = uvw::Loop::getDefault();
        auto client = loop->resource<uvw::PipeHandle>();
        test_setup_client(request, expected_answer, consider_only_state, service_, client);
        test_run_and_clean_client(service_, loop);
    }

    TEST_CASE_CLASS ("unknown request")
    {
        auto data = R"({"REQUEST_NAME": "HELLOBRUH"})"_json;
        auto answer = R"({"REQUEST_STATE":"UNKNOWN_REQUEST"})"_json;
        test_client_server_communication(std::move(data), std::move(answer));
    }

    TEST_CASE_CLASS ("create_config request")
    {
        auto data = R"({"REQUEST_NAME": "CONFIG_CREATE","CONFIG_NAME": "ma_config"})"_json;
        auto answer = R"({"CONFIG_KEY":"","READONLY_CONFIG_KEY":"","REQUEST_STATE":"SUCCESS"})"_json;
        test_client_server_communication(std::move(data), std::move(answer), true);
    }

    TEST_CASE_CLASS ("load_config request")
    {
        SUBCASE("read only key unknown in empty config table") {
            auto data = R"({"REQUEST_NAME": "CONFIG_LOAD","READONLY_CONFIG_KEY": "422Key"})"_json;
            auto answer = R"({"CONFIG_NAME":"Foo","CONFIG_ID":42,"REQUEST_STATE":"UNKNOWN_KEY"})"_json;
            test_client_server_communication(std::move(data), std::move(answer), true);
        }

        SUBCASE("read/write key unknown in empty config table") {
            auto data = R"({"REQUEST_NAME": "CONFIG_LOAD","CONFIG_KEY": "42Key"})"_json;
            auto answer = R"({"CONFIG_NAME":"Foo","CONFIG_ID":42,"REQUEST_STATE":"UNKNOWN_KEY"})"_json;
            test_client_server_communication(std::move(data), std::move(answer), true);
        }

        SUBCASE ("read only key unknown") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config_to_load");
            auto request = R"({"REQUEST_NAME": "CONFIG_LOAD","READONLY_CONFIG_KEY": "unknown_readonly_config_key"})"_json;
            auto expected_answer = R"({"CONFIG_NAME":"","CONFIG_ID":0,"REQUEST_STATE":"UNKNOWN_KEY"})"_json;
            CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();
            test_setup_client(request, expected_answer, true, service_, client);
            test_run_and_clean_client(service_, loop);
        };

        SUBCASE ("load_config request with known readonly_config_key") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config_to_load_ro");
            auto request = R"({"REQUEST_NAME": "CONFIG_LOAD","READONLY_CONFIG_KEY": "42Key"})"_json;
            request["READONLY_CONFIG_KEY"] = answer_create.readonly_config_key.value();
            auto expected_answer = R"({"CONFIG_NAME":"ma_config_to_load_ro","CONFIG_ID":42,"REQUEST_STATE":"SUCCESS"})"_json;
            expected_answer["CONFIG_ID"] = answer_create.config_id.value();
            CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();
            test_setup_client(request, expected_answer, true, service_, client);
            test_run_and_clean_client(service_, loop);
        };

        SUBCASE ("load_config request with known config_key") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config_to_load_rw");
            auto request = R"({"REQUEST_NAME": "CONFIG_LOAD","CONFIG_KEY": "42Key"})"_json;
            request["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"CONFIG_NAME":"ma_config_to_load_rw","CONFIG_ID":42,"REQUEST_STATE":"SUCCESS"})"_json;
            expected_answer["CONFIG_ID"] = answer_create.config_id.value();
            CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();
            test_setup_client(request, expected_answer, true, service_, client);
            test_run_and_clean_client(service_, loop);
        };
    }

    TEST_CASE_CLASS ("unload_config request")
    {
        SUBCASE ("unload_config request with unknown config_key") {
            auto data = R"({"REQUEST_NAME": "CONFIG_UNLOAD","CONFIG_ID": 42})"_json;
            auto answer = R"({"REQUEST_STATE":"UNKNOWN_ID"})"_json;
            test_client_server_communication(std::move(data), std::move(answer));
        };

        SUBCASE ("unload_config request with known config_key") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
            CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &service_](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static size_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_UNLOAD","CONFIG_ID": 42})"_json;
                            id = json_data.at("CONFIG_ID").get<std::size_t>();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            //expected answer unchanged.
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1: // unload config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_UNLOAD","CONFIG_ID": 42})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2: // unload invalid config
                        {
                            expected_answer = R"({"REQUEST_STATE" : "UNKNOWN_ID"})"_json;
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        };

    }

    TEST_CASE_CLASS ("include_config request")
    {
        SUBCASE("include nonexistent config src && dst") {
            auto data = R"({"REQUEST_NAME": "CONFIG_INCLUDE","CONFIG_ID": 42, "SRC": 31})"_json;
            auto answer = R"({"REQUEST_STATE":"UNKNOWN_ID"})"_json;
            test_client_server_communication(std::move(data), std::move(answer));
        }

        SUBCASE ("self include") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
            CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &service_](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static size_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_INCLUDE","CONFIG_ID": 42, "SRC": 31})"_json;
                            id = json_data.at("CONFIG_ID").get<std::size_t>();
                            request["CONFIG_ID"] = id;
                            request["SRC"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1: // include config
                        {
                            expected_answer = R"({"REQUEST_STATE":"UNAUTHORIZED"})"_json;
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        };

        SUBCASE ("circular inclusion") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto answer_create2 = service_.db_.config_create("ma_config2");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
            CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &service_, &answer_create2](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static size_t id = 0;
                    static size_t id2 = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
                            request["CONFIG_KEY"] = answer_create2.config_key.value();
                            id = json_data.at("CONFIG_ID").get<std::size_t>();
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// load second config
                        {
                            expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_INCLUDE", "CONFIG_ID": 42, "SRC": 31})"_json;
                            id2 = json_data.at("CONFIG_ID").get<std::size_t>();
                            request["CONFIG_ID"] = id2;
                            request["SRC"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2:// include first into second
                        {
                            expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_INCLUDE", "CONFIG_ID": 42, "SRC": 31})"_json;
                            request["CONFIG_ID"] = id;
                            request["SRC"] = id2;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 3: // include second into first
                        {
                            expected_answer = R"({"REQUEST_STATE":"UNAUTHORIZED"})"_json;
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        };
    }

    static void
    test_setup_client_update_setting(const nlohmann::json &request, const nlohmann::json &expected_answer, bool consider_only_state,
                      const service &service_, std::shared_ptr<uvw::PipeHandle> &client) noexcept
    {
        client->once<uvw::ConnectEvent>([&request](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                CHECK(handle.writable());
                CHECK(handle.readable());
            auto request_str = request.dump();
            handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
            handle.read();
        });

        client->on<uvw::DataEvent>(
            [&expected_answer, &consider_only_state](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                std::string_view data_str(data.data.get(), data.length);
                auto json_data = json::json::parse(data_str);
                auto json_data_str = json_data.dump();
                if (!consider_only_state) {
                        CHECK(json_data == expected_answer);
                } else {
                        CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                              expected_answer.at("REQUEST_STATE").get<std::string>());
                }
                sock.close();
            });

        client->connect(service_.socket_path_.string());
    }

    TEST_CASE_CLASS ("update_setting request")
    {
        SUBCASE("update_setting with unknown id") {
            auto data = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"foo": "bar","titi": 1}})"_json;
            auto answer = R"({"REQUEST_STATE":"UNKNOWN_ID"})"_json;
            test_client_server_communication(std::move(data), std::move(answer), true);
        }

        SUBCASE ("update_setting with valid id") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
            CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &service_](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static size_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"foo": "bar"}})"_json;
                            id = json_data.at("CONFIG_ID").get<std::size_t>();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            //expected answer unchanged.
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1: // update setting
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto &config_json_data = service_.loaded_configs_.at(id);
                            auto setting = config_json_data["SETTINGS"].find("foo");
                            CHECK_NE(setting, config_json_data["SETTINGS"].end());
                            CHECK_EQ(setting.value(), "bar");
                            sock.close();
                            break;
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        };

        SUBCASE ("update_setting with readonly key") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.readonly_config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
            CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &service_](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static size_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"foo": "bar"}})"_json;
                            id = json_data.at("CONFIG_ID").get<std::size_t>();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            //expected answer unchanged.
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1: // update setting
                        {
                            expected_answer = R"({"REQUEST_STATE":"UNAUTHORIZED"})"_json;
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        };

        SUBCASE ("update_setting with valid id and multiple settings") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
            CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &service_](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static size_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"foo": "bar","titi": "1"}})"_json;
                            id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            //expected answer unchanged.
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1: // update setting
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto &config_json_data = service_.loaded_configs_.at(id);
                            auto setting = config_json_data["SETTINGS"].find("foo");
                            CHECK_NE(setting, config_json_data["SETTINGS"].end());
                            CHECK_EQ(setting.value(), "bar");
                            setting = config_json_data["SETTINGS"].find("titi");
                            CHECK_NE(setting, config_json_data["SETTINGS"].end());
                            CHECK_EQ(setting.value(), "1");
                            sock.close();
                            break;
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        };
    }

    TEST_CASE_CLASS ("config_destroy request")
    {
        SUBCASE("destroy_config with unknown id") {
            auto data = R"({"REQUEST_NAME": "CONFIG_DESTROY","CONFIG_ID": 42})"_json;
            auto answer = R"({"REQUEST_STATE":"UNKNOWN_ID"})"_json;
            test_client_server_communication(std::move(data), std::move(answer), true);
        }

        SUBCASE ("destroy_config with valid id") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
            CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &service_, &answer_create](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static size_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_DESTROY", "CONFIG_ID": 42})"_json;
                            id = json_data.at("CONFIG_ID").get<std::size_t>();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            //expected answer unchanged.
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// destroy config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"foo": "bar"}})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2:// update setting
                        {
                            expected_answer = R"({"REQUEST_STATE":"UNKNOWN_ID"})"_json;
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_LOAD","CONFIG_KEY": 42})"_json;
                            request["CONFIG_KEY"] = answer_create.config_key.value();
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 3: // reload setting
                        {
                            expected_answer = R"({"REQUEST_STATE":"UNKNOWN_KEY"})"_json;
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });


            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        };

        SUBCASE ("destroy_config with readonly key") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.readonly_config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
            CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &service_, &answer_create](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static size_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_DESTROY", "CONFIG_ID": 42})"_json;
                            id = json_data.at("CONFIG_ID").get<std::size_t>();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// destroy config
                        {
                            expected_answer = R"({"REQUEST_STATE":"UNAUTHORIZED"})"_json;
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            std::cout << "DDDDDDDDDD" << std::endl;
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        };
    }

    TEST_CASE_CLASS ("remove_setting request")
    {
        SUBCASE("remove_setting with unknown id") {
            auto data = R"({"REQUEST_NAME": "SETTING_REMOVE","CONFIG_ID": 42, "SETTING_NAME": "titi"})"_json;
            auto answer = R"({"REQUEST_STATE":"UNKNOWN_ID"})"_json;
            test_client_server_communication(std::move(data), std::move(answer), true);
        }

        SUBCASE("remove unknow setting") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static size_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_REMOVE","CONFIG_ID": 42,"SETTING_NAME": "titi"})"_json;
                            id = json_data.at("CONFIG_ID").get<std::size_t>();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1: // remove setting
                        {
                            expected_answer["REQUEST_STATE"] = "UNKNOWN_SETTING";
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }

        SUBCASE("remove with readonly key") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.readonly_config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static size_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_REMOVE","CONFIG_ID": 42,"SETTING_NAME": "titi"})"_json;
                            id = json_data.at("CONFIG_ID").get<std::size_t>();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1: // remove setting
                        {
                            expected_answer["REQUEST_STATE"] = "UNAUTHORIZED";
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }

        SUBCASE("remove_setting with valid request") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static uint32_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                                CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"titi": "1"}})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            // unchanged expected answer
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// update config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_REMOVE","CONFIG_ID": 42,"SETTING_NAME": "titi"})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2:// remove setting
                        {
                            expected_answer = R"({"SETTING_VALUE" : "1", "REQUEST_STATE" : "SUCCESS"})"_json;
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_REMOVE","CONFIG_ID": 42,"SETTING_NAME": "titi"})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 3: // remove setting (which should fail)
                        {
                            expected_answer = R"({"REQUEST_STATE" : "UNKNOWN_SETTING"})"_json;
                            CHECK(json_data == expected_answer);
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }
    }

    TEST_CASE_CLASS ("get_setting request")
    {
        SUBCASE("get_setting with unknown id") {
            auto data = R"({"REQUEST_NAME": "SETTING_GET","CONFIG_ID": 42, "SETTING_NAME": "titi"})"_json;
            auto answer = R"({"REQUEST_STATE":"UNKNOWN_ID"})"_json;
            test_client_server_communication(std::move(data), std::move(answer), true);
        }

        SUBCASE("get unknow setting") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static size_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_GET","CONFIG_ID": 42,"SETTING_NAME": "titi"})"_json;
                            id = json_data.at("CONFIG_ID").get<std::size_t>();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            expected_answer["REQUEST_STATE"] = "UNKNOWN_SETTING";
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1: // get setting
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            sock.close();
                            break;
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }

        SUBCASE("get_setting with valid request") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static uint32_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                                CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"titi": "1"}})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            // unchanged expected answer
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// update config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_GET","CONFIG_ID": 42,"SETTING_NAME": "titi"})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2: // get setting
                            expected_answer = R"({"SETTING_VALUE" : "1", "REQUEST_STATE" : "SUCCESS"})"_json;
                            CHECK(json_data == expected_answer);
                            sock.close();
                            break;
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }

        SUBCASE("get included setting") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto answer_create2 = service_.db_.config_create("ma_config2");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &answer_create2](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static uint32_t id = 0;
                    static uint32_t id2 = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                                CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
                            request["CONFIG_KEY"] = answer_create2.config_key.value();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// load second config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id2 = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"titi": "1"}})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            // unchanged expected answer
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2:// update config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_INCLUDE","CONFIG_ID": 42,"SRC": 31})"_json;
                            request["CONFIG_ID"] = id2;
                            request["SRC"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 3:// include config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_GET","CONFIG_ID": 42,"SETTING_NAME": "titi"})"_json;
                            request["CONFIG_ID"] = id2;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 4: // get setting
                        {
                            expected_answer = R"({"SETTING_VALUE" : "1", "REQUEST_STATE" : "SUCCESS"})"_json;
                            CHECK(json_data == expected_answer);
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }

        SUBCASE("get unincluded setting") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto answer_create2 = service_.db_.config_create("ma_config2");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &answer_create2](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static uint32_t id = 0;
                    static uint32_t id2 = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
                            request["CONFIG_KEY"] = answer_create2.config_key.value();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// load second config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id2 = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"titi": "1"}})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            // unchanged expected answer
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2:// update config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_INCLUDE","CONFIG_ID": 42,"SRC": 31})"_json;
                            request["CONFIG_ID"] = id2;
                            request["SRC"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 3:// include config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_GET","CONFIG_ID": 42,"SETTING_NAME": "titi"})"_json;
                            request["CONFIG_ID"] = id2;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 4:// get setting
                        {
                            expected_answer = R"({"SETTING_VALUE" : "1", "REQUEST_STATE" : "SUCCESS"})"_json;
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_UNINCLUDE","CONFIG_ID": 42, "SRC": 31})"_json;
                            request["CONFIG_ID"] = id2;
                            request["SRC"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 5:// uninclude config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_GET","CONFIG_ID": 42,"SETTING_NAME": "titi"})"_json;
                            request["CONFIG_ID"] = id2;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 6: // get setting
                        {
                            expected_answer = R"({"REQUEST_STATE" : "UNKNOWN_SETTING"})"_json;
                            CHECK(json_data == expected_answer);
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }
    }

    TEST_CASE_CLASS ("get_settings_names request")
    {
        SUBCASE("get_settings_names with unknown id") {
            auto data = R"({"REQUEST_NAME": "CONFIG_GET_SETTINGS_NAMES","CONFIG_ID": 42, "SETTING_NAME": "titi"})"_json;
            auto answer = R"({"REQUEST_STATE":"UNKNOWN_ID"})"_json;
            test_client_server_communication(std::move(data), std::move(answer), true);
        }

        SUBCASE("get_settings_names with a single settings") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static uint32_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                                CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"titi": "1"}})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            // unchanged expected answer
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// update config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_GET_SETTINGS_NAMES","CONFIG_ID": 42})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2: // get setting
                            expected_answer = R"({"SETTINGS_NAMES" : ["titi"], "REQUEST_STATE" : "SUCCESS"})"_json;
                            CHECK(json_data == expected_answer);
                            sock.close();
                            break;
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }

        SUBCASE("get_settings_names with multiple settings") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static uint32_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                                CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"titi": "1", "lala": "lala"}})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            // unchanged expected answer
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// update config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_GET_SETTINGS_NAMES","CONFIG_ID": 42})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2: // get setting
                        {
                            expected_answer = R"({"SETTINGS_NAMES" : ["lala", "titi"], "REQUEST_STATE" : "SUCCESS"})"_json;
                            CHECK(json_data == expected_answer);
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }

        SUBCASE("get included setting names") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto answer_create2 = service_.db_.config_create("ma_config2");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &answer_create2](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static uint32_t id = 0;
                    static uint32_t id2 = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                                CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
                            request["CONFIG_KEY"] = answer_create2.config_key.value();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// load second config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id2 = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"titi": "1", "lala": "lala"}})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            // unchanged expected answer
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2:// update config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_INCLUDE","CONFIG_ID": 42,"SRC": 31})"_json;
                            request["CONFIG_ID"] = id2;
                            request["SRC"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 3:// include config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_GET_SETTINGS_NAMES","CONFIG_ID": 42})"_json;
                            request["CONFIG_ID"] = id2;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 4: // get setting
                        {
                            expected_answer = R"({"SETTINGS_NAMES" : ["lala", "titi"], "REQUEST_STATE" : "SUCCESS"})"_json;
                            CHECK(json_data == expected_answer);
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }
    }

    TEST_CASE_CLASS ("get_all_settings request")
    {
        SUBCASE("get_settings_names with unknown id") {
            auto data = R"({"REQUEST_NAME": "CONFIG_GET_SETTINGS","CONFIG_ID": 42, "SETTING_NAME": "titi"})"_json;
            auto answer = R"({"REQUEST_STATE":"UNKNOWN_ID"})"_json;
            test_client_server_communication(std::move(data), std::move(answer), true);
        }

        SUBCASE("get_all_settings with a single settings") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static uint32_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                                CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"titi": "1"}})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            // unchanged expected answer
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// update config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_GET_SETTINGS","CONFIG_ID": 42})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2: // get setting
                            expected_answer = R"({"SETTINGS" : {"titi": "1"}, "REQUEST_STATE" : "SUCCESS"})"_json;
                            CHECK(json_data == expected_answer);
                            sock.close();
                            break;
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }

        SUBCASE("get_all_settings with multiple settings") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static uint32_t id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                                CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"titi": "1", "lala": "lala"}})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            // unchanged expected answer
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// update config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_GET_SETTINGS","CONFIG_ID": 42})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2: // get setting
                            expected_answer = R"({"SETTINGS" : {"titi": "1", "lala": "lala"}, "REQUEST_STATE" : "SUCCESS"})"_json;
                            CHECK(json_data == expected_answer);
                            sock.close();
                            break;
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }

        SUBCASE("get included setting names") {
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto answer_create2 = service_.db_.config_create("ma_config2");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer, &answer_create2](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static uint32_t id = 0;
                    static uint32_t id2 = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                                CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
                            request["CONFIG_KEY"] = answer_create2.config_key.value();
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1:// load second config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            id2 = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"titi": "1", "lala": "lala"}})"_json;
                            request["CONFIG_ID"] = id;
                            auto request_str = request.dump();
                            // unchanged expected answer
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2:// update config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_INCLUDE","CONFIG_ID": 42,"SRC": 31})"_json;
                            request["CONFIG_ID"] = id2;
                            request["SRC"] = id;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 3:// include config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "CONFIG_GET_SETTINGS","CONFIG_ID": 42})"_json;
                            request["CONFIG_ID"] = id2;
                            auto request_str = request.dump();
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 4: // get setting
                        {
                            expected_answer = R"({"SETTINGS" : {"titi": "1", "lala": "lala"}, "REQUEST_STATE" : "SUCCESS"})"_json;
                            CHECK(json_data == expected_answer);
                            sock.close();
                            break;
                        }
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }
    }

    TEST_CASE_CLASS ("alias_set request")
    {
        auto data = R"({"REQUEST_NAME": "ALIAS_SET","CONFIG_ID": 43,"SETTING_NAME": "foobar","ALIAS_NAME": "barfoo"})"_json;
        auto answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
        test_client_server_communication(std::move(data), std::move(answer));
    }

    TEST_CASE_CLASS ("alias_unset request")
    {
        auto data = R"({"REQUEST_NAME": "ALIAS_UNSET","CONFIG_ID": 43,"ALIAS_NAME": "barfoo"})"_json;
        auto answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
        test_client_server_communication(std::move(data), std::move(answer));
    }

    TEST_CASE_CLASS ("setting_subscribe request")
    {
        SUBCASE("with existing setting") {
            using namespace std::string_literals;
            service service_{std::filesystem::current_path() / "albinos_service_test_internal.db"};
            auto answer_create = service_.db_.config_create("ma_config");
            auto request_load = R"({"REQUEST_NAME": "CONFIG_LOAD", "CONFIG_KEY" : 42})"_json;
            request_load["CONFIG_KEY"] = answer_create.config_key.value();
            auto expected_answer = R"({"REQUEST_STATE":"SUCCESS"})"_json;
                CHECK_FALSE(service_.create_socket());
            auto loop = uvw::Loop::getDefault();
            auto client = loop->resource<uvw::PipeHandle>();

            client->once<uvw::ConnectEvent>([&request_load](const uvw::ConnectEvent &, uvw::PipeHandle &handle) {
                    CHECK(handle.writable());
                    CHECK(handle.readable());
                auto request_str = request_load.dump();
                handle.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                handle.read();
            });

            client->on<uvw::DataEvent>(
                [&expected_answer](const uvw::DataEvent &data, uvw::PipeHandle &sock) {
                    static int step = 0;
                    static std::uint32_t config_id = 0;
                    std::string_view data_str(data.data.get(), data.length);
                    std::istringstream json_stream;
                    json_stream.str(std::string(data_str));
                    nlohmann::json json_data;
                    json_stream >> json_data;
                    //auto json_data = json::json::parse(data_str);
                    auto json_data_str = json_data.dump();
                    switch (step)
                    {
                        case 0:// load config
                        {
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SUBSCRIBE_SETTING","CONFIG_ID": 42,"SETTING_NAME": "titi"})"_json;
                            config_id = json_data.at("CONFIG_ID").get<std::uint32_t>();
                            request["CONFIG_ID"] = config_id;
                            auto request_str = request.dump();
                            //expected answer unchanged.
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 1: // subscribe setting
                        {
                                CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                      expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto request = R"({"REQUEST_NAME": "SETTING_UPDATE","CONFIG_ID": 42,"SETTINGS_TO_UPDATE": {"foo": "bar","titi": 1}})"_json;
                            request["CONFIG_ID"] = config_id;
                            auto request_str = request.dump();
                            //expected answer unchanged.
                            sock.write(request_str.data(), static_cast<unsigned int>(request_str.size()));
                            sock.read();
                            break;
                        }
                        case 2: // update setting and response from the subscribe event
                            CHECK(json_data.at("REQUEST_STATE").get<std::string>() ==
                                  expected_answer.at("REQUEST_STATE").get<std::string>());
                            auto expected = R"({"CONFIG_ID": 42, "SETTING_NAME": "titi", "SUBSCRIPTION_EVENT_TYPE" : "UPDATE"})"_json;
                            expected["CONFIG_ID"] = config_id;
                            CHECK_FALSE(json_stream.eof());
                            json_stream >> json_data;
                            CHECK(json_data == expected);
                            sock.close();
                            break;
                    }
                    step += 1;
                });

            client->connect(service_.socket_path_.string());
            test_run_and_clean_client(service_, loop);
        }

        /*SUBCASE("with alias") {
            auto data = R"({"REQUEST_NAME": "SUBSCRIBE_SETTING","CONFIG_ID": 43,"ALIAS_NAME": "barfoo"})"_json;
            auto answer = R"({"REQUEST_STATE":"INTERNAL_ERROR"})"_json;
            test_client_server_communication(std::move(data), std::move(answer));
        }*/
    }

#endif
  };
}
