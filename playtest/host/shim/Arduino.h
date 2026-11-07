// Just enough Arduino for gamestate.cpp and components.cpp on a computer.
#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

extern uint32_t g_millis;
inline uint32_t millis() { return g_millis; }
long random(long max);
long random(long min, long max);
inline void randomSeed(unsigned long) {}
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))

class String {
  public:
    String() {}
    String(const char* s) : s_(s ? s : "") {}
    String(const std::string& s) : s_(s) {}
    String(char c) : s_(1, c) {}
    template <typename T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    String(T v) : s_(std::to_string(v)) {}
    String(float v, int decimals = 2) { fmt(v, decimals); }
    String(double v, int decimals = 2) { fmt(v, decimals); }

    const char* c_str() const { return s_.c_str(); }
    size_t length() const { return s_.size(); }
    bool isEmpty() const { return s_.empty(); }
    bool concat(const char* s) { s_ += s; return true; }
    int indexOf(char c) const { auto i = s_.find(c); return i == std::string::npos ? -1 : (int)i; }
    String substring(int a, int b = -1) const {
        return String(b < 0 ? s_.substr(a) : s_.substr(a, b - a));
    }
    void trim() {
        size_t a = s_.find_first_not_of(" \t\r\n"), b = s_.find_last_not_of(" \t\r\n");
        s_ = a == std::string::npos ? "" : s_.substr(a, b - a + 1);
    }
    float toFloat() const { return strtof(s_.c_str(), nullptr); }
    String& operator+=(const String& o) { s_ += o.s_; return *this; }
    bool operator==(const char* o) const { return s_ == o; }

    friend String operator+(const String& a, const String& b) { return String(a.s_ + b.s_); }
    friend String operator+(const String& a, const char* b) { return String(a.s_ + b); }
    friend String operator+(const char* a, const String& b) { return String(std::string(a) + b.s_); }
    template <typename T, typename std::enable_if<std::is_arithmetic<T>::value, int>::type = 0>
    friend String operator+(const String& a, T n) { return a + String(n); }

  private:
    void fmt(double v, int d) { char b[48]; snprintf(b, sizeof b, "%.*f", d, v); s_ = b; }
    std::string s_;
};

struct SerialShim {
    int printf(const char* fmt, ...);
    void println(const char* s = "");
    void print(const char* s);
};
extern SerialShim Serial;
