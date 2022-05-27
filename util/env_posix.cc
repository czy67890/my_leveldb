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
public:
    PosixWritableFile(std::string filename ,int fd)
        :pos_(0),fd_(fd),is_manifest_(IsMainifest(filename)),filename_(std::move(filename)),dirname_(Dirname(filename_))
    {

    }

    ~PosixWritableFile() override {
        if(fd_ >= 0){
            Close();
        }
    }

    Status Append(const Slice & data) override{
        size_t write_size = data.size();
        const char * write_data = data.data();

        size_t copy_size = std::min(write_size,KWritableFileBufferSize);
        std::memcpy(buf_ + pos_,write_data,copy_size);
        write_data += copy_size;
        write_size -= copy_size;
        pos_ += copy_size;

        if(write_size == 0){
            return Status::OK();
        }

        Status status = FlushBuffer();
        if(!status.ok()){
            return status;
        }


        //once can write down
        if(write_size < KWritableFileBufferSize){
            std::memcpy(buf_,write_data,write_size);
            pos_ = write_size;
            return Status::OK();
        }
        return WriteUnbuffered(write_data,write_size);
    }

    Status Close() override{
        Status status = FlushBuffer();
        const int close_result = ::close(fd_);
        if(close_result < 0 && status.ok()){
            status = PosixError(filename_,errno);
        }
        fd_ = -1;
        return status;
    }

    Status Flush() override{ return FlushBuffer();}
    Status Sync() override{
        Status status = SyncDirIfManifest();
        if(!status.ok()){
            return status;
        }
        return SyncFd(fd_,filename_);
    }
private:
    Status FlushBuffer(){
        Status status = WriteUnbuffered(buf_,pos_);
        pos_ = 0;
        return status;
    }

    //写未缓冲的到文件描述符
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
        Status status;
        if(!is_manifest_){
            return status;
        }
        int fd = ::open(dirname_.c_str(),O_RDONLY|kOpenBaseFlags);
        if(fd < 0){
            status = PosixError(dirname_,errno);
        }
        else{
            status = SyncFd(fd,dirname_);
            ::close(fd);
        }
        return status;
    }
    static Status SyncFd(int fd,const std::string &fd_path){
        //fsync会将修改的数据和文件描述符永久的存放到disk中
        bool sync_success = ::fsync(fd) == 0;
        if(sync_success){
            return Status::OK();
        }
        return PosixError(fd_path,errno);
    }

    static std::string Dirname(const std::string &filename){
        std::string::size_type separator_pos = filename.rfind('/');
        if(separator_pos == std::string::npos){
            return std::string(".");
        }
        assert(filename.find('/',separator_pos + 1) == std::string::npos);
        return filename.substr(0,separator_pos);
    }

    static Slice Basename(const std::string & filename){
        std::string::size_type separator_pos = filename.rfind('/');
        if(separator_pos == std::string::npos){
            return Slice(filename);
        }
        assert(filename.find('/',separator_pos + 1) == std::string::npos);
        return Slice(filename.data() + separator_pos + 1 ,filename.length() - separator_pos - 1);
    }

    static bool IsMainifest(const std::string & filename){
        return Basename(filename ).starts_with("MANIFEST");
    }
    char buf_[KWritableFileBufferSize];
    size_t pos_;
    int fd_;

    const bool is_manifest_;//是否为显式文件
    const std::string filename_;
    const std::string dirname_;
};
}
