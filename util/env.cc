#include "leveldb/env.h"

#include<cstdarg>

namespace czy_leveldb{

Env::Env() = default;

Env::~Env() = default;

Status Env::NewAppendableFile(const std::string & fname, WritableFile** result){
    return Status::NotSupported("NewAppendableFile",fname);
}

Status Env::RemoveDir(const std::string & dirname) {return DeleteDir(dirname);}

Status Env::RemoveFile(const std::string& fname) { return DeleteFile(fname); }
Status Env::DeleteFile(const std::string& fname) { return RemoveFile(fname); }

SequentialFile::~SequentialFile() = default;

RandomAccessFile::~RandomAccessFile() = default;

WritableFile::~WritableFile() = default;

Logger::~Logger() = default;

FileLock::~FileLock() = default;


//Logger ��һ���������
//����ָ��ʵ�ֶ�̬

void Log(Logger * info_log,const char * format, ...){
    if(info_log != nullptr){
        //�߳�����ʹ�ù���
        std::va_list ap;
        va_start(ap,format);
        info_log->Logv(format,ap);
        va_end(ap);
    }
}

//ֻ�Ǹ������д�����ṩһ���ӿ�
static Status DoWriteStringToFile(Env * env ,const Slice & data,const std::string &fname,bool should_sync){
    WritableFile *file;
    Status s =env->NewWritableFile(fname,&file);
    if(!s.ok()){
        return s;
    }
    s = file->Append(data);
    if(s.ok() && should_sync ){
        s = file->Sync();
    }
    if(s.ok()){
        s = file->Close();
    }
    delete file;//need to close the temp file
    if(!s.ok()){
        env->RemoveFile(fname);
    }
    return s;
}

//ToDo::ʹ��Ĭ�ϲ�����
Status WriteStringToFile(Env * env,const Slice & data,const std::string & fname){
    return DoWriteStringToFile(env,data,fname,false);
}

Status WriteStringToFileSync(Env * env,const Slice & data,const std::string & fname){
    return DoWriteStringToFile(env,data,fname,true);
}

Status ReadFileToString(Env * env,const std::string &fname,std::string * data){
    data->clear();
    SequentialFile* file;
    Status s = env->NewSequentialFile(fname,&file);
    if(!s.ok()){
        return s;
    }
    static const int KBufferSize = 8198;
    char * space = new char [KBufferSize];
    while(true){
        Slice fragment;
        s = file->Read(KBufferSize,&fragment,space);
        if(!s.ok()){
            break;
        }
        data->append(fragment.data(),fragment.size());
        if(fragment.empty()){
            break;
        }
    }
    delete[] space;
    delete file;
    return s;
}
EnvWrapper::~EnvWrapper() {}
}