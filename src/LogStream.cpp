#include "LogStream.hpp"
#include <cstdio>

namespace MyServer {
    LogStream& LogStream::operator<<(bool v) {
        buffer_.append(v ? "1" : "0", 1);
        return *this;
    }

    LogStream& LogStream::operator<<(short v) {
        char buf[kMaxNumericSize];
        int len = snprintf(buf, sizeof(buf), "%hd", v);
        buffer_.append(buf, len);
        return *this;
    }

    LogStream& LogStream::operator<<(unsigned short v) {
        char buf[kMaxNumericSize];
        int len = snprintf(buf, sizeof(buf), "%hu", v);
        buffer_.append(buf, len);
        return *this;
    }

    LogStream& LogStream::operator<<(int v) {
        char buf[kMaxNumericSize];
        int len = snprintf(buf, sizeof(buf), "%d", v);
        buffer_.append(buf, len);
        return *this;
    }

    LogStream& LogStream::operator<<(unsigned int v) {
        char buf[kMaxNumericSize];
        int len = snprintf(buf, sizeof(buf), "%u", v);
        buffer_.append(buf, len);
        return *this;
    }

    LogStream& LogStream::operator<<(long v) {
        char buf[kMaxNumericSize];
        int len = snprintf(buf, sizeof(buf), "%ld", v);
        buffer_.append(buf, len);
        return *this;
    }

    LogStream& LogStream::operator<<(unsigned long v) {
        char buf[kMaxNumericSize];
        int len = snprintf(buf, sizeof(buf), "%lu", v);
        buffer_.append(buf, len);
        return *this;
    }

    LogStream& LogStream::operator<<(long long v) {
        char buf[kMaxNumericSize];
        int len = snprintf(buf, sizeof(buf), "%lld", v);
        buffer_.append(buf, len);
        return *this;
    }

    LogStream& LogStream::operator<<(unsigned long long v) {
        char buf[kMaxNumericSize];
        int len = snprintf(buf, sizeof(buf), "%llu", v);
        buffer_.append(buf, len);
        return *this;
    }

    LogStream& LogStream::operator<<(float v) {
        char buf[kMaxNumericSize];
        int len = snprintf(buf, sizeof(buf), "%.12g", v);
        buffer_.append(buf, len);
        return *this;
    }

    LogStream& LogStream::operator<<(double v) {
        char buf[kMaxNumericSize];
        int len = snprintf(buf, sizeof(buf), "%.12g", v);
        buffer_.append(buf, len);
        return *this;
    }

    LogStream& LogStream::operator<<(char v) {
        buffer_.append(&v, 1);
        return *this;
    }

    LogStream& LogStream::operator<<(const char* str) {
        if (str) {
            buffer_.append(str, strlen(str));
        } else {
            buffer_.append("(null)", 6);
        }
        return *this;
    }

    LogStream& LogStream::operator<<(const std::string& v) {
        buffer_.append(v.c_str(), v.size());
        return *this;
    }
} // namespace MyServer