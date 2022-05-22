#pragma once
#include<cstdint>

#include"leveldb/export.h"
#include"leveldb/slice.h"

namespace czy_leveldb{

class LEVELDB_EXPORT Cache;

LEVELDB_EXPORT Cache * NewLRUCache(size_t capacity);

class LEVELDB_EXPORT Cache{
public:
    Cache() = default;
    
};
}