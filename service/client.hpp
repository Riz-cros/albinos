//
// Created by milerius on 15/03/19.
//

#pragma once

#include <unordered_set>
#include "service_strong_types.hpp"

namespace raven
{
  class client
  {
  private:
    using client_ptr = std::shared_ptr<uvw::PipeHandle>;
  public:
    client(client_ptr sock) noexcept : sock_(sock)
    {
    };

    client() = delete;

    client &operator+=(raven::config_id_st id)
    {
        config_ids_.insert(id.value());
        return *this;
    }

    client &operator-=(raven::config_id_st id)
    {
        DLOG_F(INFO, "erasing id: %d from config: %d", id.value(), static_cast<int>(this->sock_->fileno()));
        config_ids_.erase(id.value());
        return *this;
    }

  private:
    client_ptr sock_;
    std::unordered_set<raven::config_id_st::value_type> config_ids_;
  };
};