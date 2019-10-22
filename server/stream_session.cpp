/***
    This file is part of snapcast
    Copyright (C) 2014-2019  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include "stream_session.hpp"

#include "common/aixlog.hpp"
#include "message/pcm_chunk.hpp"
#include <iostream>
#include <mutex>

using namespace std;


StreamSession::StreamSession(boost::asio::io_context& ioc, MessageReceiver* receiver, tcp::socket&& socket)
    : buffer_pos_(0), socket_(std::move(socket)), messageReceiver_(receiver), pcmStream_(nullptr), strand_(ioc)
{
    base_msg_size_ = baseMessage_.getSize();
    buffer_.resize(base_msg_size_);
}


StreamSession::~StreamSession()
{
    stop();
}


void StreamSession::read_next()
{
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(buffer_, base_msg_size_),
                            boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t length) mutable {
                                if (ec)
                                {
                                    LOG(ERROR) << "Error reading message header: " << ec.message() << "\n";
                                    messageReceiver_->onDisconnect(this);
                                    return;
                                }

                                baseMessage_.deserialize(buffer_.data());
                                LOG(DEBUG) << "getNextMessage: " << baseMessage_.type << ", size: " << baseMessage_.size << ", id: " << baseMessage_.id
                                           << ", refers: " << baseMessage_.refersTo << "\n";
                                if (baseMessage_.type > message_type::kLast)
                                {
                                    stringstream ss;
                                    ss << "unknown message type received: " << baseMessage_.type << ", size: " << baseMessage_.size;
                                    throw std::runtime_error(ss.str().c_str());
                                }
                                else if (baseMessage_.size > msg::max_size)
                                {
                                    stringstream ss;
                                    ss << "received message of type " << baseMessage_.type << " to large: " << baseMessage_.size;
                                    throw std::runtime_error(ss.str().c_str());
                                }

                                if (baseMessage_.size > buffer_.size())
                                    buffer_.resize(baseMessage_.size);

                                boost::asio::async_read(
                                    socket_, boost::asio::buffer(buffer_, baseMessage_.size),
                                    boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t length) mutable {
                                        if (ec)
                                        {
                                            LOG(ERROR) << "Error reading message body: " << ec.message() << "\n";
                                            messageReceiver_->onDisconnect(this);
                                            return;
                                        }

                                        tv t;
                                        baseMessage_.received = t;
                                        if (messageReceiver_ != nullptr)
                                            messageReceiver_->onMessageReceived(this, baseMessage_, buffer_.data());
                                        read_next();
                                    }));
                            }));
}


void StreamSession::setPcmStream(PcmStreamPtr pcmStream)
{
    pcmStream_ = pcmStream;
}


const PcmStreamPtr StreamSession::pcmStream() const
{
    return pcmStream_;
}


void StreamSession::start()
{
    strand_.post([this]() { read_next(); });
}


void StreamSession::stop()
{
    LOG(DEBUG) << "StreamSession::stop\n";
    boost::system::error_code ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    if (ec)
        LOG(ERROR) << "Error in socket shutdown: " << ec.message() << "\n";
    socket_.close(ec);
    if (ec)
        LOG(ERROR) << "Error in socket close: " << ec.message() << "\n";
    LOG(DEBUG) << "StreamSession stopped\n";
}


void StreamSession::send_next()
{
    auto self(shared_from_this());
    auto buffer = messages_.front();

    boost::asio::async_write(socket_, buffer, boost::asio::bind_executor(strand_, [this, self, buffer](boost::system::error_code ec, std::size_t length) {
                                 // boost::asio::async_write(socket_, *buffer, [this, self](boost::system::error_code ec, std::size_t length) {
                                 messages_.pop_front();
                                 if (ec)
                                 {
                                     LOG(ERROR) << "StreamSession write error: " << ec.message() << "\n";
                                     messageReceiver_->onDisconnect(this);
                                     return;
                                 }
                                 if (!messages_.empty())
                                     send_next();
                             }));
}


void StreamSession::sendAsync(shared_const_buffer const_buf)
{
    strand_.post([this, const_buf]() {
        messages_.push_back(const_buf);
        if (messages_.size() > 1)
        {
            LOG(DEBUG) << "outstanding async_write\n";
            return;
        }
        send_next();
    });
}


void StreamSession::sendAsync(msg::message_ptr message, bool sendNow)
{
    if (!message)
        return;

    tv t;
    message->sent = t;
    std::ostringstream oss;
    message->serialize(oss);

    sendAsync(shared_const_buffer(oss.str()));
}


void StreamSession::setBufferMs(size_t bufferMs)
{
    bufferMs_ = bufferMs;
}


bool StreamSession::send(msg::message_ptr message)
{
    sendAsync(message);
    // // TODO on exception: set active = false
    // //	LOG(INFO) << "send: " << message->type << ", size: " << message->getSize() << ", id: " << message->id << ", refers: " << message->refersTo <<
    // "\n"; boost::asio::streambuf streambuf; std::ostream stream(&streambuf); tv t; message->sent = t; message->serialize(stream); boost::asio::write(socket_,
    // streambuf);
    // //	LOG(INFO) << "done: " << message->type << ", size: " << message->size << ", id: " << message->id << ", refers: " << message->refersTo << "\n";
    // return true;
}
