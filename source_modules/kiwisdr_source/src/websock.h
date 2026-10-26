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
#include <chrono>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <functional>
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

        // sendString() 会被 GUI 线程（tune）和接收线程（keepalive/pong）同时调用
        std::mutex sendMutex;
        std::string userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
        // 可选 Cookie，例如 "ident=xxx"；为空则不发送 Cookie 头
        std::string cookie;
        // 可选 Referer 覆盖；为空时自动用 http://hostAndPort/
        std::string refererOverride;

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
                flog::warn("websock: non-data frame, opcode={}, payload={} bytes", (int)(data[0] & 0x0F), outLen - 1);
                if ((data[0] & 0x0F) == 0x8 && outLen >= 3) {
                    int closeCode = ((unsigned char)buffer[0] << 8) | (unsigned char)buffer[1];
                    flog::warn("websock: server sent CLOSE code={} reason={}", closeCode, buffer.substr(2, outLen - 3));
                }
                break;
            case PING_FRAME:
                // 浏览器会把 ping 的 payload 原样放进带掩码的 pong
                sendPong((const unsigned char*)buffer.data(), outLen > 0 ? std::min(outLen - 1, 125) : 0);
                break;
            default:
                printf("Unknown frame type: %x\n", frameType);
                break;
            }
            count++;
            return skipSize;
        }

        void sendPong(const unsigned char* payload, int payloadLen) {
            std::lock_guard<std::mutex> lck(sendMutex);
            std::string buffer;
            buffer.resize(payloadLen + 200, ' ');
            int len = makeFrame(PONG_FRAME, (unsigned char*)payload, payloadLen, (unsigned char *)buffer.data(), buffer.size());
            buffer.resize(len);
            if (socket) { socket->sendstr(buffer); }
        }

        void sendString(const std::string &str) {
            std::lock_guard<std::mutex> lck(sendMutex);
            std::string buffer;
            buffer.resize(str.length() + 200, ' ');
            int len = makeFrame(TEXT_FRAME, (unsigned char*)str.c_str(), str.length(), (unsigned char *)buffer.data(), buffer.size());
            buffer.resize(len);
            if (socket) { socket->sendstr(buffer); }
//            flog::info("<= {}", str);
        }

        void sendBinary(const std::vector<uint8_t>& data) {
            std::lock_guard<std::mutex> lck(sendMutex);
            std::string buffer;
            buffer.resize(data.size() + 200, ' ');
            int len = makeFrame(BINARY_FRAME, (unsigned char*)data.data(), data.size(), (unsigned char *)buffer.data(), buffer.size());
            buffer.resize(len);
            if (socket) { socket->sendstr(buffer); }
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
            buffer[pos++] = (frame_type == PONG_FRAME) ? 0x8A : (unsigned char)frame_type; // fin included

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
            {
                buffer[1] |= 0x80; // 客户端帧必须带掩码
                auto maskIndex = pos;
                buffer[pos++] = uniform_dist(e1);
                buffer[pos++] = uniform_dist(e1);
                buffer[pos++] = uniform_dist(e1);
                buffer[pos++] = uniform_dist(e1);
                if (size > 0) { memcpy((void*)(buffer+pos), msg, size); }
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

        static std::string base64Encode(const uint8_t* data, size_t len) {
            static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve(((len + 2) / 3) * 4);
            for (size_t i = 0; i < len; i += 3) {
                uint32_t v = (uint32_t)data[i] << 16;
                if (i + 1 < len) { v |= (uint32_t)data[i + 1] << 8; }
                if (i + 2 < len) { v |= (uint32_t)data[i + 2]; }
                out += tbl[(v >> 18) & 63];
                out += tbl[(v >> 12) & 63];
                out += (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
                out += (i + 2 < len) ? tbl[v & 63] : '=';
            }
            return out;
        }

        // 浏览器每次连接都发 16 个随机字节的 base64
        std::string makeWebSocketKey() {
            uint8_t raw[16];
            for (auto& b : raw) { b = (uint8_t)uniform_dist(e1); }
            return base64Encode(raw, sizeof(raw));
        }

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

            const std::string hostForHeader = (host.find(':') != std::string::npos && host.front() != '[') ? ("[" + host + "]") : host;
            const std::string hostAndPort = (port == 80) ? hostForHeader : (hostForHeader + ":" + std::to_string(port));
            const std::string originAndReferer = "http://" + hostAndPort;
            const std::string referer = refererOverride.empty() ? (originAndReferer + "/") : refererOverride;

            // 按 Chrome 的常见顺序发送，尽量不显得异常；补充 Sec-CH-UA / Sec-Fetch-* 等字段
            std::string initHeaders = "GET " + path + " HTTP/1.1\r\n";
            initHeaders += "Host: " + hostAndPort + "\r\n";
            initHeaders += "Connection: Upgrade\r\n";
            initHeaders += "Pragma: no-cache\r\n";
            initHeaders += "Cache-Control: no-cache\r\n";
            initHeaders += "User-Agent: " + userAgent + "\r\n";
            initHeaders += "Upgrade: websocket\r\n";
            initHeaders += "Origin: " + originAndReferer + "\r\n";
            initHeaders += "Sec-WebSocket-Version: 13\r\n";
            initHeaders += "Accept-Encoding: gzip, deflate, br, zstd\r\n";
            initHeaders += "Accept-Language: en-US,en;q=0.9\r\n";
            initHeaders += "Accept: */*\r\n";
            initHeaders += "Referer: " + referer + "\r\n";
            initHeaders += "Sec-CH-UA: \"Chromium\";v=\"130\", \"Google Chrome\";v=\"130\", \"Not?A_Brand\";v=\"99\"\r\n";
            initHeaders += "Sec-CH-UA-Mobile: ?0\r\n";
            initHeaders += "Sec-CH-UA-Platform: \"Windows\"\r\n";
            initHeaders += "Sec-Fetch-Dest: websocket\r\n";
            initHeaders += "Sec-Fetch-Mode: websocket\r\n";
            initHeaders += "Sec-Fetch-Site: same-origin\r\n";
            if (!cookie.empty()) { initHeaders += "Cookie: " + cookie + "\r\n"; }
            initHeaders += "Sec-WebSocket-Key: " + makeWebSocketKey() + "\r\n";
            initHeaders += "\r\n";

            int len = socket->sendstr(initHeaders);
            int senderr = errno;
            flog::info("sent: {} of {}", len, (int64_t)initHeaders.size());

            uint8_t buf[100000];

            std::string bufs;
            size_t pos = std::string::npos;
            int recvd = 0;
            while ((pos = bufs.find("\r\n\r\n")) == std::string::npos) {
                recvd = socket->recv(buf, sizeof(buf), false, NO_TIMEOUT);
                if (recvd <= 0) {
                    std::string msg = "websock: recv failed, errno=" + std::to_string(errno) + " (recvd=" + std::to_string(recvd) +
                                      " sent=" + std::to_string(len) + " senderr=" + std::to_string(senderr) + ")";
                    socket->close();
                    throw std::runtime_error(msg);
                }
                bufs.append((const char*)buf, (size_t)recvd);
                if (bufs.size() > 16384) {
                    socket->close();
                    throw std::runtime_error("websock: invalid response");
                }
            }
            flog::info("recvd: {}", (int)bufs.size());
            {
                std::string statusLine = bufs.substr(0, bufs.find("\r\n"));
                if (statusLine.find(" 101") == std::string::npos) {
                    socket->close();
                    throw std::runtime_error("server refused websocket: " + statusLine.substr(0, 80));
                }
            }
            std::vector<std::string> recvHeaders;
            splitStringV(bufs.substr(0, pos + 2), "\r\n", recvHeaders);
            for (size_t i = 0; i < recvHeaders.size(); i++) {
                flog::info("websock: < {}", recvHeaders[i]);
            }
            std::vector<uint8_t> data(bufs.begin() + pos + 4, bufs.end());

            auto connectedAt = std::chrono::steady_clock::now();
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
                    onEveryReceive(); // 空闲 tick 也要驱动 keepalive
                    continue;
                }
//                printf("recvd bytes in loop: %d\n", recvd);
                if (recvd <= 0) {
                    int lastErr = 0;
#ifdef _WIN32
                    lastErr = WSAGetLastError();
#else
                    lastErr = errno;
#endif
                    auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - connectedAt).count();
                    flog::warn("websock: recv returned {}, lasterr={}, after {} s, stopped_by_us={}", recvd, lastErr, (int64_t)secs, (int)stopped.load());
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