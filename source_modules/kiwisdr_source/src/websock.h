#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <map>
#include <atomic>
#include <mutex>
#include <memory>
#include <string>
#include <utils/net.h>
#include <stdio.h>
#include <random>
#include <stdexcept>
#include "kiwi_strings.h"
#include <utils/flog.h>

namespace net::websock {

    struct WSClient {
        std::shared_ptr<::net::Socket> socket;
        std::string path;
        std::random_device rd;
        std::default_random_engine e1;
        std::uniform_int_distribution<int> uniform_dist;
        std::atomic<bool> stopped{ false };
        std::mutex sockMutex;

        WSClient() : socket(), rd(), e1(rd()), uniform_dist(0, 255) {
            flog::info("WSClient instance: {}", (const void*)this);
        }

        int count = 0;

        int maybeDecodeBuffer(const std::vector<uint8_t> &data) {
            std::string buffer;
            if (data.empty()) {
                return 0;
            }
            buffer.resize(data.size() + 200, ' ');
            int outLen = 0;
            int skipSize = 0;
            int frameType = getFrame((unsigned char*)data.data(), (int)data.size(), (unsigned char*)buffer.data(), (int)buffer.length(), &outLen, &skipSize);
//            printf("(%d) Handling frame type: %x, skipsize = %d ...\n", count, frameType, skipSize);
            switch(frameType) {
            case TEXT_FRAME:
                if (outLen > 3) {
                    buffer.resize(outLen - 1);
                    onTextMessage(buffer);
                }
                break;
            case BINARY_FRAME:
                if (outLen > 3) {
                    buffer.resize(outLen - 1);
                    onBinaryMessage(buffer);
                }
                break;
            case INCOMPLETE_FRAME:
                return 0;
            case ERROR_FRAME:
                printf("ERROR FRAME: %x\n", frameType);
                break;
            case PING_FRAME:
                sendPong();
                break;
            default:
                printf("Unknown frame type: %x\n", frameType);
                break;
            }
            count++;
            return skipSize;
        }

        void sendPong() {
            std::string str = " ";
            std::string buffer;
            buffer.resize(200, ' ');
            int len = makeFrame(PONG_FRAME, (unsigned char*)str.c_str(), 0, (unsigned char *)buffer.data(), buffer.size());
            buffer.resize(len);
            socket->sendstr(buffer);
            //
        }

        void sendString(const std::string &str) {
            std::string buffer;
            buffer.resize(str.length() + 200, ' ');
            int len = makeFrame(TEXT_FRAME, (unsigned char*)str.c_str(), str.length(), (unsigned char *)buffer.data(), buffer.size());
            buffer.resize(len);
            socket->sendstr(buffer);
//            flog::info("<= {}", str);
        }

        void sendBinary(const std::vector<uint8_t>& data) {
            std::string buffer;
            buffer.resize(data.size() + 200, ' ');
            int len = makeFrame(BINARY_FRAME, (unsigned char*)data.data(), data.size(), (unsigned char *)buffer.data(), buffer.size());
            buffer.resize(len);
            socket->sendstr(buffer);
        }

        // PARTS were taken from https://github.com/katzarsky/WebSocket

        enum WebSocketFrameType {
            ERROR_FRAME=0xFF00,
            INCOMPLETE_FRAME=0xFE00,

            OPENING_FRAME=0x3300,
            CLOSING_FRAME=0x3400,

            INCOMPLETE_TEXT_FRAME=0x01,
            INCOMPLETE_BINARY_FRAME=0x02,

            TEXT_FRAME=0x81,
            BINARY_FRAME=0x82,

            PING_FRAME=0x19,
            PONG_FRAME=0x1A
        };

        int makeFrame(WebSocketFrameType frame_type, unsigned char* msg, int msg_length, unsigned char* buffer, int buffer_size)
        {
            int pos = 0;
            int size = msg_length;
            buffer[pos++] = (unsigned char)frame_type; // fin included

            if(size <= 125) {
                buffer[pos++] = size; // set mask bit
            }
            else if(size <= 65535) {
                buffer[pos++] = 126; //16 bit length follows

                buffer[pos++] = (size >> 8) & 0xFF; // leftmost first
                buffer[pos++] = size & 0xFF;
            }
            else { // >2^16-1 (65535)
                buffer[pos++] = 127; //64 bit length follows

                // write 8 bytes length (significant first)

                // since msg_length is int it can be no longer than 4 bytes = 2^32-1
                // padd zeroes for the first 4 bytes
                for(int i=3; i>=0; i--) {
                    buffer[pos++] = 0;
                }
                // write the actual 32bit msg_length in the next 4 bytes
                for(int i=3; i>=0; i--) {
                    buffer[pos++] = ((size >> 8*i) & 0xFF);
                }
            }
            if (frame_type != PONG_FRAME) {
                buffer[1] |= 0x80; // set mask bit
                auto maskIndex = pos;
                buffer[pos++] = uniform_dist(e1);
                buffer[pos++] = uniform_dist(e1);
                buffer[pos++] = uniform_dist(e1);
                buffer[pos++] = uniform_dist(e1);
                memcpy((void*)(buffer+pos), msg, size);
                for(int q=0; q<size; q++) {
                    buffer[pos+q] ^= buffer[maskIndex + (q%4)];
                }
            }
            return (size+pos);
        }

        WebSocketFrameType getFrame(unsigned char* in_buffer, int in_length, unsigned char* out_buffer, int out_size, int* out_length, int *skipSize)
        {
            //printf("getTextFrame()\n");
            if(in_length < 2) return INCOMPLETE_FRAME;

            unsigned char msg_opcode = in_buffer[0] & 0x0F;
            unsigned char msg_fin = (in_buffer[0] >> 7) & 0x01;
            unsigned char msg_masked = (in_buffer[1] >> 7) & 0x01;

            // *** message decoding

            int64_t payload_length = 0;
            int pos = 2;
            int length_field = in_buffer[1] & (~0x80);
            unsigned int mask = 0;

            //printf("IN:"); for(int i=0; i<20; i++) printf("%02x ",buffer[i]); printf("\n");

            if(length_field <= 125) {
                payload_length = length_field;
            }
            else if(length_field == 126) { //msglen is 16bit!
                //payload_length = in_buffer[2] + (in_buffer[3]<<8);
                payload_length = (
                    ((int64_t)in_buffer[2] << 8) |
                    ((int64_t)in_buffer[3])
                );
                pos += 2;
            }
            else if(length_field == 127) { //msglen is 64bit!
                payload_length = (
                    ((int64_t)in_buffer[2] << 56) |
                    ((int64_t)in_buffer[3] << 48) |
                    ((int64_t)in_buffer[4] << 40) |
                    ((int64_t)in_buffer[5] << 32) |
                    ((int64_t)in_buffer[6] << 24) |
                    ((int64_t)in_buffer[7] << 16) |
                    ((int64_t)in_buffer[8] << 8) |
                    ((int64_t)in_buffer[9])
                );
                pos += 8;
            }

            //printf("PAYLOAD_LEN: %08x\n", payload_length);
            if(in_length < payload_length+pos) {
                return INCOMPLETE_FRAME;
            }

            if(msg_masked) {
                mask = *((unsigned int*)(in_buffer+pos));
                //printf("MASK: %08x\n", mask);
                pos += 4;

                // unmask data:
                unsigned char* c = in_buffer+pos;
                for(int i=0; i<payload_length; i++) {
                    c[i] = c[i] ^ ((unsigned char*)(&mask))[i%4];
                }
            }

            if(payload_length > out_size) {
                flog::error("ERROR: output buffer is too small for the payload");
                abort();
                //TODO: if output buffer is too small -- ERROR or resize(free and allocate bigger one) the buffer ?
            }

            memcpy((void*)out_buffer, (void*)(in_buffer+pos), payload_length);
            out_buffer[payload_length] = 0;
            *out_length = payload_length+1;
            *skipSize = payload_length+pos;

            //printf("TEXT: %s\n", out_buffer);

            if(msg_opcode == 0x0) return (msg_fin)?TEXT_FRAME:INCOMPLETE_TEXT_FRAME; // continuation frame ?
            if(msg_opcode == 0x1) return (msg_fin)?TEXT_FRAME:INCOMPLETE_TEXT_FRAME;
            if(msg_opcode == 0x2) return (msg_fin)?BINARY_FRAME:INCOMPLETE_BINARY_FRAME;
            if(msg_opcode == 0x9) return PING_FRAME;
            if(msg_opcode == 0xA) return PONG_FRAME;

            return ERROR_FRAME;
        }

        std::function<void(const std::string&)> onTextMessage = [](auto){};
        std::function<void(const std::string&)> onBinaryMessage = [](auto){};
        std::function<void()> onConnected = [](){};
        std::function<void()> onDisconnected = [](){};
        std::function<void()> onEveryReceive = [](){};

        void connectAndReceiveLoop(const std::string& host, int port, const std::string& path) {
            flog::info("WSClient connectAndReceiveLoop: inst={}", (const void*)this);
            // 'stopped' is cleared by the caller before the thread starts, so a stop request that arrives
            // while we are still inside net::connect() is not lost.
            auto z = net::connect(Address(host, port));
            {
                std::lock_guard<std::mutex> lck(sockMutex);
                socket = z;
            }
            if (stopped) {
                if (z) { z->close(); }
                std::lock_guard<std::mutex> lck(sockMutex);
                socket.reset();
                return;
            }
            flog::info("WSClient socket connected");

            std::string initHeaders =
                "Accept-Encoding: gzip, deflate\r\n"
                "Accept-Language: en-US,en\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: Upgrade\r\n"
                "Cookie: ident=\r\n"
                "Pragma: no-cache\r\n"
                "Sec-GPC: 1\r\n"
                "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n"
                "Sec-WebSocket-Key: tXZvmn8MbkVRhAoczhuyVQ==\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Upgrade: websocket\r\n"
                "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36\r\n";
            initHeaders = "GET " + path + " HTTP/1.1\r\n" + initHeaders;
            initHeaders += "Host: " + host + ":" + std::to_string(port) + "\r\n";
            initHeaders += "Origin: http://" + host + ":" + std::to_string(port) + "\r\n";
            initHeaders += "\r\n";

            int len = socket->sendstr(initHeaders);
            int senderr = errno;
            flog::info("sent: {} of {}", len, (int64_t)initHeaders.size());

            uint8_t buf[100000];

            int recvd = socket->recv(buf, sizeof(buf) - 1, false, NO_TIMEOUT);
            if (recvd <= 0) {
                std::string msg = "websock: recv failed, errno=" + std::to_string(errno)+" (recvd="+std::to_string(recvd)+
                                  " sent="+std::to_string(len)+" senderr="+std::to_string(senderr)+")";
                socket->close();
                throw std::runtime_error(msg);
            }
            buf[recvd] = 0;
            flog::info("recvd: {}", recvd);
            std::vector<std::string> recvHeaders;
            std::string bufs = (char*)buf;
            auto pos = bufs.find("\r\n\r\n");
            if (pos == std::string::npos) {
                socket->close();
                throw std::runtime_error("websock: invalid response");
            }
            {
                // The server must answer "HTTP/1.1 101 Switching Protocols"; anything else (busy, 4xx/5xx) is an error.
                std::string statusLine = bufs.substr(0, bufs.find("\r\n"));
                if (statusLine.find(" 101") == std::string::npos) {
                    socket->close();
                    throw std::runtime_error("server refused websocket: " + statusLine.substr(0, 80));
                }
            }
            bufs.resize(pos + 2);
            splitStringV(bufs, "\r\n", recvHeaders);
            for (int i = 0; i < recvHeaders.size(); i++) {
                printf("%s\n", recvHeaders[i].c_str());
            }
            std::vector<uint8_t> data;
            for (int i = (int)pos + 4; i < recvd; i++) {
                data.push_back(buf[i]);
            }
            onConnected();
            while (true) {
                int len0 = maybeDecodeBuffer(data);
                if (len0 > 0) {
//                    printf("decoded/dropping bytes: %d\n", len0);
                    data.erase(data.begin(), data.begin() + len0);
                    continue;
                }
                recvd = socket->recv(buf, sizeof(buf), false, 100); // 100 msec
                if (recvd == 0) {
                    continue;
                }
//                printf("recvd bytes in loop: %d\n", recvd);
                if (recvd <= 0) {
                    socket->close();
                    onDisconnected();
                    break;
                }
                onEveryReceive();
                for (int i = 0; i < recvd; i++) {
                    data.push_back(buf[i]);
                }
            }
        }
        void stopSocket() {
            stopped = true;
            std::shared_ptr<::net::Socket> sock;
            {
                std::lock_guard<std::mutex> lck(sockMutex);
                sock = socket;
            }
            if (sock) {
                sock->close();
            }
        }
    };

}
