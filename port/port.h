#pragma once
#include<string.h>
#include<pthread.h>
#if HAVE_CRC32C
#include <crc32c/crc32c.h>
#endif  // HAVE_CRC32C
#if HAVE_SNAPPY
#include <snappy.h>
#endif  // HAVE_SNAPPY

#include <cassert>
#include <condition_variable>  // NOLINT
#include <cstddef>
#include <cstdint>
#include <mutex>  // NOLINT
#include <string>

#include "port/thread_annotations.h"
//c++17 style namespace
namespace czy_leveldb :: port{
class CondVar;

class LOCKABLE Mutex{
public: 
    Mutex(){
        pthread_mutex_init(&mutex_,nullptr);
    };
    ~Mutex(
        pthread_mutex_destroy(&mutex_);
    )
    Mutex(const Mutex &) = delete;
    Mutex& operator=(const Mutex & ) = delete;
    void Lock() EXCLUSIVE_LOCK_FUNCTION() {pthread_mutex_lock(&mutex_);}
    void UnLock() UNLOCK_FUNCTION() {pthread_mutex_unlock(&mutex_);}
    void AssertHeld() ASSERT_EXCLUSIVE_LOCK() {
    }
    pthread_mutex_t& get(){
        return mutex_;
    }
private:
    friend class CondVar;
    pthread_mutex_t mutex_;
}
class CondVar{
public:
    explicit CondVar(Mutex mu):mu_(mu),{
        assert(mu != nullptr) ;
        pthread_cond_init(&cond_,nullptr);
    }
    ~CondVar() = default;
    CondVar(const CondVar &) = delete;
    CondVar & operator= (const CondVar &) = delete;
    void Wait(){
        pthread_cond_wait(&cond_,&mu_.get());       
    }

private:
    pthread_cond_t cond_;
    Mutex& const mu_;
};

inline bool Snappy_Compress(const char* input, size_t length,
                            std::string* output) {
#if HAVE_SNAPPY
  output->resize(snappy::MaxCompressedLength(length));
  size_t outlen;
  snappy::RawCompress(input, length, &(*output)[0], &outlen);
  output->resize(outlen);
  return true;
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)output;
#endif  // HAVE_SNAPPY

  return false;
}

inline bool Snappy_GetUncompressedLength(const char* input, size_t length,
                                         size_t* result) {
#if HAVE_SNAPPY
  return snappy::GetUncompressedLength(input, length, result);
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)result;
  return false;
#endif  // HAVE_SNAPPY
}

inline bool GetHeapProfile(void (*func)(void*, const char*, int), void* arg) {
  // Silence compiler warnings about unused arguments.
  (void)func;
  (void)arg;
  return false;
}

inline uint32_t AcceleratedCRC32C(uint32_t crc, const char* buf, size_t size) {
#if HAVE_CRC32C
  return ::crc32c::Extend(crc, reinterpret_cast<const uint8_t*>(buf), size);
#else
  // Silence compiler warnings about unused arguments.
  (void)crc;
  (void)buf;
  (void)size;
  return 0;
#endif  // HAVE_CRC32C
}
}