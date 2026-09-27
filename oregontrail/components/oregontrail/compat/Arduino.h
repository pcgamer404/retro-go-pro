#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include <chrono>
#include <thread>
#include <type_traits>
using byte = uint8_t;
using boolean = bool;
class String {
    std::string s_;
public:
    String() = default; String(const char *s):s_(s?s:""){} String(const std::string&s):s_(s){} String(char c):s_(1,c){}
    String(int v):s_(std::to_string(v)){} String(unsigned v):s_(std::to_string(v)){} String(long v):s_(std::to_string(v)){} String(unsigned long v):s_(std::to_string(v)){}
    String(long long v):s_(std::to_string(v)){} String(unsigned long long v):s_(std::to_string(v)){} String(float v):s_(std::to_string(v)){} String(double v):s_(std::to_string(v)){}
    const char*c_str()const{return s_.c_str();} size_t length()const{return s_.length();}
    int indexOf(char c,int from=0)const{auto p=s_.find(c,from<0?0:(size_t)from);return p==std::string::npos?-1:(int)p;}
    int indexOf(const char*n,int from=0)const{auto p=s_.find(n?n:"",from<0?0:(size_t)from);return p==std::string::npos?-1:(int)p;}
    String substring(int from,int to=-1)const{if(from<0)from=0;if((size_t)from>s_.size())return String();if(to<0||(size_t)to>s_.size())to=(int)s_.size();if(to<from)to=from;return String(s_.substr(from,to-from));}
    char operator[](size_t i)const{return s_[i];}
    void clear(){s_.clear();}
    void remove(size_t idx){if(idx<s_.size())s_.erase(idx);}
    void trim(){size_t a=s_.find_first_not_of(" \t\r\n");size_t b=s_.find_last_not_of(" \t\r\n");if(a==std::string::npos)s_.clear();else s_=s_.substr(a,b-a+1);} operator const char*()const{return s_.c_str();}
    String&operator+=(const String&o){s_+=o.s_;return *this;} String&operator+=(const char*o){if(o)s_+=o;return *this;} String&operator+=(char c){s_+=c;return *this;}
    bool operator==(const String&o)const{return s_==o.s_;} bool operator!=(const String&o)const{return s_!=o.s_;}
};
inline String operator+(String a,const String&b){a+=b;return a;} inline String operator+(String a,const char*b){a+=b;return a;} inline String operator+(const char*a,const String&b){String s(a);s+=b;return s;} inline String operator+(String a,char b){a+=b;return a;}
template<class T, typename std::enable_if<std::is_arithmetic<T>::value,int>::type = 0> inline String operator+(String a,T b){a+=String(b);return a;}
template<class T, typename std::enable_if<std::is_arithmetic<T>::value,int>::type = 0> inline String operator+(const char*a,T b){String s(a);s+=String(b);return s;}
namespace arduino_compat { inline uint32_t millis(){static const auto t=std::chrono::steady_clock::now();return(uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t).count();} inline void delay(uint32_t ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));} }
using arduino_compat::millis; using arduino_compat::delay;
inline long random(long max){return max>0?::rand()%max:0;} inline long random(long min,long max){return max>min?min+(::rand()%(max-min)):min;} inline void randomSeed(unsigned long s){::srand((unsigned)s);}
inline long map(long x,long a,long b,long c,long d){return b==a?c:(x-a)*(d-c)/(b-a)+c;} template<class T> inline T constrain(T x,T a,T b){return x<a?a:(x>b?b:x);}
