#pragma once
#include<cassert>
#include<cstddef>
#include<cstring>
#include<string>

#include"leveldb/export.h"

namespace czy_leveldb{
    //LEVELDB_EXPORT
    //用于导出DLL文件时，防止c++链接问题
class LEVELDB_EXPORT Slice{
public:
    Slice() :data_(""),size_(0){}
    Slice(const char *d,size_t n) :data_(d),size_(n){}
    Slice(const std::string &s) : data_(s.data()),size_(s.size()){}
    Slice(const char* s) : data_(s), size_(strlen(s)) {}

    // 内部可拷贝的.
    Slice(const Slice& ) = default;
    Slice& operator=(const Slice & rhs) = default;
    // Return a pointer to the beginning of the referenced data
    const char* data() const { return data_; }

    // Return the length (in bytes) of the referenced data
    size_t size() const { return size_; }

    // Return true iff the length of the referenced data is zero
    bool empty() const { return size_ == 0; }

    char operator[](size_t n) const{
        assert(n<size());
        return data_[n];
    }

    void clear(){
        data_ = "";
        size_ = 0;
    }

    //maybe some mem leak
    void remove_prefix(size_t n){
        //用断言判断不可能发生的情况
        assert(n <= size());
        data_ += n;
        size_ -= n;
    }

    std::string ToString() const{
        return std::string(data_,size_);
    }

    int compare(const Slice &b) const;

    //判断是否为slice的前缀
    bool starts_with(const Slice &x) const {
        return ((size_ >= x.size_) && (memcmp(data_,x.data_,x.size_) == 0));
    }

private:
    const char * data_;
    size_t size_;
};

inline bool operator==(const Slice & x,const Slice & y){
    return ((x.size() == y.size()) && (memcmp(x.data(),y.data(),x.size())) == 0);
}

inline bool operator!=(const Slice & x,const Slice & y ){
    return !(x == y);
}


inline int Slice::compare(const Slice & b) const{
    const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
    int r = memcmp(data_,b.data_,min_len);
    if(r == 0){
        if(size_ < b.size_){
            r = -1;
        }
        else if(size_ >b.size_){
            r = 1;
        }
    }
    return r;   
}

}//namespace leveldb