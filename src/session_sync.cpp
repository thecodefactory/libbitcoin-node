/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/node/session_sync.hpp>

#include <cstddef>
#include <memory>
#include <bitcoin/blockchain.hpp>
#include <bitcoin/node/configuration.hpp>
#include <bitcoin/node/define.hpp>
#include <bitcoin/node/protocol_header_sync.hpp>

INITIALIZE_TRACK(bc::node::session_sync);

namespace libbitcoin {
namespace node {

#define CLASS session_sync

using namespace config;
using namespace network;
using std::placeholders::_1;
using std::placeholders::_2;

session_sync::session_sync(threadpool& pool, p2p& network,
    const checkpoint& start, const configuration& configuration)
  : session(pool, network, configuration.network, false, true),
    votes_(0),
    headers_({ start.hash() }),
    start_height_(start.height()),
    configuration_(configuration),
    checkpoints_(configuration.chain.checkpoints),
    CONSTRUCT_TRACK(session_sync, LOG_NETWORK)
{
    config::checkpoint::sort(checkpoints_);
}

void session_sync::start(result_handler handler)
{
    if (!stopped())
    {
        handler(error::operation_failed);
        return;
    }

    votes_ = 0;
    session::start();
    new_connection(create_connector(), handler);
}

void session_sync::new_connection(connector::ptr connect,
    result_handler handler)
{
    fetch_address(ORDERED4(start_syncing, _1, _2, connect, handler));
}

// This session does not support concurrent channels.
void session_sync::start_syncing(const code& ec, const config::authority& sync,
    connector::ptr connect, result_handler handler)
{
    if (stopped())
    {
        handler(error::channel_stopped);
        return;
    }

    log::info(LOG_NETWORK)
        << "Contacting sync [" << sync << "]";


    // SYNCHRONIZE CONNECT
    connect->connect(sync,
        ORDERED5(handle_connect, _1, _2, sync, connect, handler));
}

void session_sync::handle_connect(const code& ec, channel::ptr channel,
    const authority& sync, connector::ptr connect, result_handler handler)
{
    if (ec)
    {
        log::debug(LOG_NETWORK)
            << "Failure connecting [" << sync << "] sync: " << ec.message();
        new_connection(connect, handler);
        return;
    }

    log::info(LOG_NETWORK)
        << "Connected to sync [" << channel->authority() << "]";

    register_channel(channel,
        BIND4(handle_channel_start, _1, connect, channel, handler),
        BIND1(handle_channel_stop, _1));
}

void session_sync::handle_channel_start(const code& ec, connector::ptr connect,
    channel::ptr channel, result_handler handler)
{
    // Treat a start failure just like a completion failure.
    if (ec)
    {
        handle_complete(ec, connect, handler);
        return;
    }

    const auto rate = configuration_.node.headers_per_second;

    attach<protocol_ping>(channel)->start(settings_);
    attach<protocol_address>(channel)->start(settings_);
    attach<protocol_header_sync>(channel, rate, start_height_, headers_,
        checkpoints_)->start(BIND3(handle_complete, _1, connect, handler));
};

// The handler is passed on to the next start call.
void session_sync::handle_complete(const code& ec,
    network::connector::ptr connect, result_handler handler)
{
    if (ec == error::service_stopped)
    {
        handler(ec);
        return;
    }

    if (!ec)
        ++votes_;

    // We require a number successful peer syncs, for maximizing height.
    if (ec || votes_ < configuration_.node.quorum)
    {
        new_connection(connect, handler);
        return;
    }

    // This is the end of the header sync cycle.
    handler(error::success);
}

void session_sync::handle_channel_stop(const code& ec)
{
}

} // namespace node
} // namespace libbitcoin
