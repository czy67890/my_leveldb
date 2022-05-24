#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#ifndef __Fuchsia__
#include <sys/resource.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/posix_logger.h"

namespace czy_leveldb{
    int g_open_read_only_file_limit = -1;
    constexpr const int kDefaultMmapLimit = (sizeof(void*) >= 8) ?1000:0;
    int g_mmap_limit = kDefaultMmapLimit;
// Common flags defined for all posix open operations
#if defined(HAVE_O_CLOEXEC)
constexpr const int kOpenBaseFlags = O_CLOEXEC;
#else
constexpr const int kOpenBaseFlags = 0;
#endif  // defined(HAVE_O_CLOEXEC)

    constexpr const size_t KWritableFileBufferSize = 65536;
    Status PosixError(const std::string &context,int error_number){
        if(error_number == ENOENT){
            return Status::NotFound(context,std::strerror(error_number));
        }
        else{
            return Status::IOError(context,std::strerror(error_number));
        }
    }
class Limiter{
public:
    Limiter(int max_acquires)
     :
#if !defined(NDEBUG)
    max_acquires_(max_acquires),
#endif
    acquires_allowed_(max_acquires){
        assert(max_acquires >= 0);
    }
    Limiter(const Limiter &) = delete;
    Limiter operator=(const Limiter &) = delete;
    //获取资源
    bool Acquire(){
        //先获取然后减
        //std::memory_ordered_relaxed是最宽松的内存控制
        //只用于程序计数器之类的东西
        int old_acquires_allowed = acquires_allowed_.fetch_sub(1,std::memory_order_relaxed);
        if(old_acquires_allowed > 0) return true;
        int pre_increment_allowed = acquires_allowed_.fetch_add(1,std::memory_order_relaxed);
        (void) pre_increment_allowed;
        assert(pre_increment_allowed < max_acquires_);
        return false;
    }

    void Release(){
        int old_acquires_allowed = acquires_allowed_.fetch_add(1,std::memory_order_relaxed);
        (void) old_acquires_allowed;
        assert(old_acquires_allowed < max_acquires_);
    }
private:

#if !defined(NDEBUG)
    const int max_acquires_;
#endif
    std::atomic<int> acquires_allowed_;
};

//to implement the sequentialFile
class PosixSquentialFile final : public SequentialFile{
public:
    PosixSquentialFile(std::string filename,int fd)
    //in string we use std::move
     :fd_(fd),filename_(std::move(filename))
    { }
    ~PosixSquentialFile() override {close(fd_);}

    Status Read(size_t n,Slice * result,char * scratch) override{
        Status status;
        while(true){
            ::ssize_t read_size = ::read(fd_,scratch,n);
            if(read_size < 0){
                //EINTR代表系统调用被中断
                if(errno == EINTR){
                    continue;
                }
                status = PosixError(filename_,errno);
                break;
            }
            *result = Silce(scratch,read_size);
            break;
        }
        return status;
    }

    Status Skip(uint64_t n) override{
        if(::lseek(fd_,n,SEEK_CUR) == static_cast<off_t> (-1)){
            return PosixError(filename_,errno);
        }
        return Status::OK();
    }
private: 
    const int fd_;
    const std::string filename_;
};

}
