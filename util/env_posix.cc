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


class PosixRandomAccessFile final : public RandomAccessFile{
public:
    //random File 的构造函数
    //如果达到limiter
    //那么不能再打开文件
    PosixRandomAccessFile(std::string filename,int fd, Limiter * fd_limiter)
    :has_permanent_fd_(fd_limiter->Acquire()),fd_(has_permanent_fd_ ? fd : -1),
     fd_limiter_(fd_limiter),file_name_(std::move(filename))
    { 
        if(!has_permanent_fd_){
            assert(fd_ == -1);
            ::close(fd);
        }
    }
    ~PosixRandomAccessFile() override{
        if(has_permanent_fd_){
            assert(fd_ != -1);
            ::close(fd_);
            fd_limiter_->Release();
        }
    }

    Status Read(uint64_t offset,size_t n ,Slice * result,char * scratch) const override{
        int fd = fd_;
        if(!has_permanent_fd_){
            fd = ::open(file_name_.c_str(),O_RDONLY | kOpenBaseFlags);
            if(fd < 0){
                return PosixError(file_name_,errno);
            }
        }
        assert(fd != -1);
        Status status;
        //pread 就是lseek和read的联合体
        //这样做的时候lseek和read是一个原子操作
        ssize_t read_size = :: pread(fd,scratch,n,static_cast<off_t>(offset));
        *result = Slice(scratch,(read_size < 0) ? 0:read_size);
        if(read_size < 0){
            status = PosixError (file_name_,errno);
        }
        if(!has_permanent_fd_){
            assert(fd != fd_);
            close(fd);
        }
        return status;
    }
private:
    const bool has_permanent_fd_;
    const int fd_;
    Limiter * const fd_limiter_;
    const std::string file_name_;
};

class PosixMmapReadableFile final : public RandomAccessFile{
public: 
    PosixMmapReadableFile(std::string filename, char * mmap_base,size_t length,
                            Limiter * mmap_limiter
    )
     :mmap_base_(mmap_base),
      length_(length),
      mmap_limiter_(mmap_limiter),
      filename_(std::move(filename))
    { }
    ~PosixMmapReadableFile() override{
        ::munmap(static_cast<void *> (mmap_base_),length_);
        mmap_limiter_->Release();
    }

    Status Read (uint64_t offset,size_t n ,Slice * result,char * scratch) const override{
        if(offset + n > length_){
            *result = Slice();
            //EINVAL::向函数传递了无效参数
            return PosixError(filename_,EINVAL);
        }
        *result = Slice(mmap_base_ + offset ,n);
        return Status::OK();
    }
private:
    char * const mmap_base_;
    const size_t length_;
    Limiter* const mmap_limiter_;
    const std:: string filename_;
};

class PosixWritableFile final : public WritableFile{

private:
    Status FlushBuffer(){
        Status status = WriteUnbuffered(buf_,pos_);
        pos_ = 0;
        return status;
    }

    Status WriteUnbuffered(const char * data,size_t size){
        while(size > 0){
            ssize_t write_result = :: write(fd_ ,data,size);
            while(write_result < 0){
                if(errno  == EINTR){
                    continue;
                }
                return PosixError(filename_,errno);
            }
            data += write_result;
            size -= write_result;
        }
        return Status::OK();
    }

    Status SyncDirIfManifest(){
        
    }
    char buf_[KWritableFileBufferSize];
    size_t pos_;
    int fd_;

    const bool is_manifest_;//是否为显式文件
    const std::string filename_;
    const std::string dirname_;
};
}
