#pragma once
#include <string>
#include <cstring>

class String {
public:
    String() : _str() {}
    String(const char* s) : _str(s ? s : "") {}
    String(const String& other) = default;
    String(String&& other) = default;

    String& operator=(const String& other) = default;
    String& operator=(String&& other) = default;
    String& operator=(const char* s) { _str = s ? s : ""; return *this; }

    const char* c_str() const { return _str.c_str(); }
    unsigned int length() const { return (unsigned int)_str.length(); }

    bool startsWith(const String& prefix) const {
        return _str.compare(0, prefix._str.size(), prefix._str) == 0;
    }

    bool operator==(const String& other) const { return _str == other._str; }
    bool operator!=(const String& other) const { return _str != other._str; }
    bool operator<(const String& other) const { return _str < other._str; }
    bool operator>(const String& other) const { return _str > other._str; }
    bool operator<=(const String& other) const { return _str <= other._str; }
    bool operator>=(const String& other) const { return _str >= other._str; }

    String operator+(const String& other) const {
        return String((_str + other._str).c_str());
    }
    String& operator+=(const String& other) { _str += other._str; return *this; }
    String& operator+=(char c) { _str += c; return *this; }
    String& operator+=(const char* s) { if (s) _str += s; return *this; }

    bool concat(const char* s) { if (s) _str += s; return true; }
    bool concat(char c) { _str += c; return true; }
    bool concat(const char* s, unsigned int len) {
        if (s && len) _str.append(s, len);
        return true;
    }

    void reserve(unsigned int size) { _str.reserve(size); }

private:
    std::string _str;
};
