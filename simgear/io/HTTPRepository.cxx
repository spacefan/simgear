// HTTPRepository.cxx -- plain HTTP TerraSync remote client
//
// Copyright (C) 20126  James Turner <zakalawe@mac.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#include "HTTPRepository.hxx"

#include <iostream>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <sstream>
#include <map>
#include <set>
#include <fstream>
#include <limits>
#include <cstdlib>

#include <fcntl.h>

#include "simgear/debug/logstream.hxx"
#include "simgear/misc/strutils.hxx"
#include <simgear/misc/sg_dir.hxx>
#include <simgear/io/HTTPClient.hxx>
#include <simgear/io/sg_file.hxx>
#include <simgear/misc/sgstream.hxx>
#include <simgear/structure/exception.hxx>

#include <simgear/misc/sg_hash.hxx>

namespace simgear
{

    class HTTPDirectory;

class HTTPRepoPrivate
{
public:
    struct HashCacheEntry
    {
        std::string filePath;
        time_t modTime;
        size_t lengthBytes;
        std::string hashHex;

    };

    typedef std::vector<HashCacheEntry> HashCache;
    HashCache hashes;

    HTTPRepoPrivate(HTTPRepository* parent) :
    p(parent),
    isUpdating(false),
    status(AbstractRepository::REPO_NO_ERROR)
    { ; }

    HTTPRepository* p; // link back to outer
    HTTP::Client* http;
    std::string baseUrl;
    SGPath basePath;
    bool isUpdating;
    AbstractRepository::ResultCode status;
    HTTPDirectory* rootDir;

    HTTP::Request_ptr updateFile(HTTPDirectory* dir, const std::string& name);
    HTTP::Request_ptr updateDir(HTTPDirectory* dir);

    std::string hashForPath(const SGPath& p);
    void updatedFileContents(const SGPath& p, const std::string& newHash);
    void parseHashCache();
    std::string computeHashForPath(const SGPath& p);
    void writeHashCache();

    void failedToGetRootIndex();

    typedef std::vector<HTTP::Request_ptr> RequestVector;
    RequestVector requests;

    void finishedRequest(const HTTP::Request_ptr& req);

    HTTPDirectory* getOrCreateDirectory(const std::string& path);
    bool deleteDirectory(const std::string& path);

    typedef std::vector<HTTPDirectory*> DirectoryVector;
    DirectoryVector directories;

};

class HTTPDirectory
{
    struct ChildInfo
    {
        enum Type
        {
            FileType,
            DirectoryType
        };

        ChildInfo(Type ty, const char* nameData, const char* hashData) :
            type(ty),
            name(nameData),
            hash(hashData ? hashData : ""),
            sizeInBytes(0)
        {
        }

        ChildInfo(const ChildInfo& other) :
            type(other.type),
            name(other.name),
            hash(other.hash),
            sizeInBytes(other.sizeInBytes)
        { }

        void setSize(const char* sizeData)
        {
            sizeInBytes = ::strtol(sizeData, NULL, 10);
        }

        bool operator<(const ChildInfo& other) const
        {
            return name < other.name;
        }

        Type type;
        std::string name, hash;
        size_t sizeInBytes;
    };

    typedef std::vector<ChildInfo> ChildInfoList;
    ChildInfoList children;

public:
    HTTPDirectory(HTTPRepoPrivate* repo, const std::string& path) :
        _repository(repo),
        _relativePath(path)
  {
      SGPath p(absolutePath());
      if (p.exists()) {
          bool indexValid = false;
          try {
              // already exists on disk
              parseDirIndex(children);
              indexValid = true;
              std::sort(children.begin(), children.end());
          } catch (sg_exception& e) {
              // parsing cache failed
              children.clear();
          }
      }
  }

    HTTPRepoPrivate* repository() const
    {
        return _repository;
    }

    std::string url() const
    {
        if (_relativePath.str().empty()) {
            return _repository->baseUrl;
        }

        return _repository->baseUrl + "/" + _relativePath.str();
    }

    void dirIndexUpdated(const std::string& hash)
    {
        SGPath fpath(_relativePath);
        fpath.append(".dirindex");
        _repository->updatedFileContents(fpath, hash);

        children.clear();
        parseDirIndex(children);
        std::sort(children.begin(), children.end());
    }

    void failedToUpdate()
    {
        if (_relativePath.isNull()) {
            // root dir failed
            _repository->failedToGetRootIndex();
        } else {
            SG_LOG(SG_TERRASYNC, SG_WARN, "failed to update dir:" << _relativePath);
        }
    }

    void updateChildrenBasedOnHash()
    {
        SG_LOG(SG_TERRASYNC, SG_DEBUG, "updated children for:" << relativePath());

        string_list indexNames = indexChildren(),
            toBeUpdated, orphans;
        simgear::Dir d(absolutePath());
        PathList fsChildren = d.children(0);
        PathList::const_iterator it = fsChildren.begin();

        for (; it != fsChildren.end(); ++it) {
            ChildInfo info(it->isDir() ? ChildInfo::DirectoryType : ChildInfo::FileType,
                           it->file().c_str(), NULL);
            std::string hash = hashForChild(info);

            ChildInfoList::iterator c = findIndexChild(it->file());
            if (c == children.end()) {
                orphans.push_back(it->file());
            } else if (c->hash != hash) {
                // file exists, but hash mismatch, schedule update
                if (!hash.empty()) {
                    SG_LOG(SG_TERRASYNC, SG_INFO, "file exists but hash is wrong for:" << c->name);
                }

                toBeUpdated.push_back(c->name);
            } else {
                // file exists and hash is valid. If it's a directory,
                // perform a recursive check.
                if (c->type == ChildInfo::DirectoryType) {
                    SGPath p(relativePath());
                    p.append(c->name);
                    HTTPDirectory* childDir = _repository->getOrCreateDirectory(p.str());
                    childDir->updateChildrenBasedOnHash();
                } else {
                    SG_LOG(SG_TERRASYNC, SG_INFO, "existing file is ok:" << c->name);
                }
            }

            // remove existing file system children from the index list,
            // so we can detect new children
            string_list::iterator it = std::find(indexNames.begin(), indexNames.end(), c->name);
            if (it != indexNames.end()) {
                indexNames.erase(it);
            }
        } // of real children iteration

        // all remaining names in indexChilden are new children
        toBeUpdated.insert(toBeUpdated.end(), indexNames.begin(), indexNames.end());

        removeOrphans(orphans);
        scheduleUpdates(toBeUpdated);
    }

    void removeOrphans(const string_list& orphans)
    {
        string_list::const_iterator it;
        for (it = orphans.begin(); it != orphans.end(); ++it) {
            removeChild(*it);
        }
    }

    string_list indexChildren() const
    {
        string_list r;
        r.reserve(children.size());
        ChildInfoList::const_iterator it;
        for (it=children.begin(); it != children.end(); ++it) {
            r.push_back(it->name);
        }
        return r;
    }

    void scheduleUpdates(const string_list& names)
    {
        string_list::const_iterator it;
        for (it = names.begin(); it != names.end(); ++it) {
            ChildInfoList::iterator cit = findIndexChild(*it);
            if (cit == children.end()) {
                SG_LOG(SG_TERRASYNC, SG_WARN, "scheduleUpdate, unknown child:" << *it);
                continue;
            }

            if (cit->type == ChildInfo::FileType) {
                _repository->updateFile(this, *it);
            } else {
                SGPath p(relativePath());
                p.append(*it);
                HTTPDirectory* childDir = _repository->getOrCreateDirectory(p.str());
                _repository->updateDir(childDir);
            }
        }
    }

    SGPath absolutePath() const
    {
        SGPath r(_repository->basePath);
        r.append(_relativePath.str());
        return r;
    }

    SGPath relativePath() const
    {
        return _relativePath;
    }

    void didUpdateFile(const std::string& file, const std::string& hash)
    {
        SGPath fpath(_relativePath);
        fpath.append(file);
        _repository->updatedFileContents(fpath, hash);
        SG_LOG(SG_TERRASYNC, SG_INFO, "did update:" << fpath);
    }

    void didFailToUpdateFile(const std::string& file)
    {
        SGPath fpath(_relativePath);
        fpath.append(file);
        SG_LOG(SG_TERRASYNC, SG_WARN, "failed to update:" << fpath);
    }
private:

    struct ChildWithName
    {
        ChildWithName(const std::string& n) : name(n) {}
        std::string name;

        bool operator()(const ChildInfo& info) const
        { return info.name == name; }
    };

    ChildInfoList::iterator findIndexChild(const std::string& name)
    {
        return std::find_if(children.begin(), children.end(), ChildWithName(name));
    }

    void parseDirIndex(ChildInfoList& children)
    {
        SGPath p(absolutePath());
        p.append(".dirindex");
        std::ifstream indexStream( p.str(), std::ios::in );

        if ( !indexStream.is_open() ) {
            throw sg_io_exception("cannot open dirIndex file", p);
        }

        char lineBuffer[512];
        char* lastToken;

        while (!indexStream.eof() ) {
            indexStream.getline(lineBuffer, 512);
            lastToken = 0;
            char* typeData = ::strtok_r(lineBuffer, ":", &lastToken);
            if (!typeData) {
                continue; // skip blank line
            }

            if (!typeData) {
                // malformed entry
                throw sg_io_exception("Malformed dir index file", p);
            }

            if (!strcmp(typeData, "version")) {
                continue;
            } else if (!strcmp(typeData, "path")) {
                continue;
            }

            char* nameData = ::strtok_r(NULL, ":", &lastToken);
            char* hashData = ::strtok_r(NULL, ":", &lastToken);
            char* sizeData = ::strtok_r(NULL, ":", &lastToken);

            if (typeData[0] == 'f') {
                children.push_back(ChildInfo(ChildInfo::FileType, nameData, hashData));
            } else if (typeData[0] == 'd') {
                children.push_back(ChildInfo(ChildInfo::DirectoryType, nameData, hashData));
            } else {
                throw sg_io_exception("Malformed line code in dir index file", p);
            }

            if (sizeData) {
                children.back().setSize(sizeData);
            }
        }
    }

    void removeChild(const std::string& name)
    {
        SGPath p(absolutePath());
        p.append(name);
        bool ok;

        SGPath fpath(_relativePath);
        fpath.append(name);

        if (p.isDir()) {
            ok = _repository->deleteDirectory(fpath.str());
        } else {
            // remove the hash cache entry
            _repository->updatedFileContents(fpath, std::string());
            ok = p.remove();
        }

        if (!ok) {
            SG_LOG(SG_TERRASYNC, SG_WARN, "removal failed for:" << p);
        }
    }

    std::string hashForChild(const ChildInfo& child) const
    {
        SGPath p(absolutePath());
        p.append(child.name);
        if (child.type == ChildInfo::DirectoryType) {
            p.append(".dirindex");
        }
        return _repository->hashForPath(p);
    }

  HTTPRepoPrivate* _repository;
  SGPath _relativePath; // in URL and file-system space


};

HTTPRepository::HTTPRepository(const SGPath& base, HTTP::Client *cl) :
    _d(new HTTPRepoPrivate(this))
{
    _d->http = cl;
    _d->basePath = base;
    _d->rootDir = new HTTPDirectory(_d.get(), "");
}

HTTPRepository::~HTTPRepository()
{
}

void HTTPRepository::setBaseUrl(const std::string &url)
{
  _d->baseUrl = url;
}

std::string HTTPRepository::baseUrl() const
{
  return _d->baseUrl;
}

HTTP::Client* HTTPRepository::http() const
{
  return _d->http;
}

SGPath HTTPRepository::fsBase() const
{
  return SGPath();
}

void HTTPRepository::update()
{
    if (_d->isUpdating) {
        return;
    }

    _d->status = REPO_NO_ERROR;
    _d->isUpdating = true;
    _d->updateDir(_d->rootDir);
}

bool HTTPRepository::isDoingSync() const
{
    if (_d->status != REPO_NO_ERROR) {
        return false;
    }

    return _d->isUpdating;
}

AbstractRepository::ResultCode
HTTPRepository::failure() const
{
    return _d->status;
}

    class FileGetRequest : public HTTP::Request
    {
    public:
        FileGetRequest(HTTPDirectory* d, const std::string& file) :
            HTTP::Request(makeUrl(d, file)),
            directory(d),
            fileName(file),
            fd(-1)
        {
            SG_LOG(SG_TERRASYNC, SG_INFO, "will GET file " << url());

        }

    protected:
        virtual void gotBodyData(const char* s, int n)
        {
            if (fd < 0) {
                SGPath p(pathInRepo());
#ifdef SG_WINDOWS
                int mode = 00666;
#else
                mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
#endif
                fd = ::open(p.c_str(), O_CREAT | O_TRUNC | O_RDWR, mode);
                if (fd < 0) {
                    SG_LOG(SG_TERRASYNC, SG_WARN, "unable to create file " << p);
                    // fail
                }
                sha1_init(&hashContext);
            }

            ::write(fd, s, n);
            sha1_write(&hashContext, s, n);

        }

        virtual void onDone()
        {
            ::close(fd);
            if (responseCode() == 200) {
                std::string hash = strutils::encodeHex((char*) sha1_result(&hashContext));
                directory->didUpdateFile(fileName, hash);

                SG_LOG(SG_TERRASYNC, SG_DEBUG, "got file " << fileName << " in " << directory->absolutePath());
            } else {
                directory->didFailToUpdateFile(fileName);
            }

            directory->repository()->finishedRequest(this);
        }
    private:
        static std::string makeUrl(HTTPDirectory* d, const std::string& file)
        {
            return d->url() + "/" + file;
        }

        SGPath pathInRepo() const
        {
            SGPath p(directory->absolutePath());
            p.append(fileName);
            return p;
        }

        HTTPDirectory* directory;
        std::string fileName; // if empty, we're getting the directory itself
        simgear::sha1nfo hashContext;
        int fd;
    };

    class DirGetRequest : public HTTP::Request
    {
    public:
        DirGetRequest(HTTPDirectory* d) :
            HTTP::Request(makeUrl(d)),
            directory(d),
            _isRootDir(false)
        {
            sha1_init(&hashContext);
            SG_LOG(SG_TERRASYNC, SG_INFO, "will GET dir " << url());

        }

        void setIsRootDir()
        {
            _isRootDir = true;
        }

        bool isRootDir() const
        {
            return _isRootDir;
        }

    protected:
        virtual void gotBodyData(const char* s, int n)
        {
            body += std::string(s, n);
            sha1_write(&hashContext, s, n);
        }

        virtual void onDone()
        {
            if (responseCode() == 200) {
                std::string hash = strutils::encodeHex((char*) sha1_result(&hashContext));
                std::string curHash = directory->repository()->hashForPath(path());
                if (hash != curHash) {

                    simgear::Dir d(directory->absolutePath());
                    if (!d.exists()) {
                        if (!d.create(0700)) {
                            throw sg_io_exception("Unable to create directory", d.path());
                        }
                    }

                    // dir index data has changed, so write to disk and update
                    // the hash accordingly
                    std::ofstream of(pathInRepo().str(), std::ios::trunc | std::ios::out);
                    assert(of.is_open());
                    of.write(body.data(), body.size());
                    of.close();
                    directory->dirIndexUpdated(hash);

                    SG_LOG(SG_TERRASYNC, SG_DEBUG, "updated dir index " << directory->absolutePath());

                }

                // either way we've confirmed the index is valid so update
                // children now
                directory->updateChildrenBasedOnHash();
            } else {
                directory->failedToUpdate();
            }

            directory->repository()->finishedRequest(this);
        }
    private:
        static std::string makeUrl(HTTPDirectory* d)
        {
            return d->url() + "/.dirindex";
        }

        SGPath pathInRepo() const
        {
            SGPath p(directory->absolutePath());
            p.append(".dirindex");
            return p;
        }

        HTTPDirectory* directory;
        simgear::sha1nfo hashContext;
        std::string body;
        bool _isRootDir; ///< is this the repository root?
    };


    HTTP::Request_ptr HTTPRepoPrivate::updateFile(HTTPDirectory* dir, const std::string& name)
    {
        HTTP::Request_ptr r(new FileGetRequest(dir, name));
        http->makeRequest(r);
        requests.push_back(r);
        return r;
    }

    HTTP::Request_ptr HTTPRepoPrivate::updateDir(HTTPDirectory* dir)
    {
        HTTP::Request_ptr r(new DirGetRequest(dir));
        http->makeRequest(r);
        requests.push_back(r);
        return r;
    }


    class HashEntryWithPath
    {
    public:
        HashEntryWithPath(const std::string& p) : path(p) {}
        bool operator()(const HTTPRepoPrivate::HashCacheEntry& entry) const
        { return entry.filePath == path; }
    private:
        std::string path;
    };

    std::string HTTPRepoPrivate::hashForPath(const SGPath& p)
    {
        HashCache::iterator it = std::find_if(hashes.begin(), hashes.end(), HashEntryWithPath(p.str()));
        if (it != hashes.end()) {
            // ensure data on disk hasn't changed.
            // we could also use the file type here if we were paranoid
            if ((p.sizeInBytes() == it->lengthBytes) && (p.modTime() == it->modTime)) {
                return it->hashHex;
            }

            // entry in the cache, but it's stale so remove and fall through
            hashes.erase(it);
        }

        std::string hash = computeHashForPath(p);
        updatedFileContents(p, hash);
        return hash;
    }

    std::string HTTPRepoPrivate::computeHashForPath(const SGPath& p)
    {
        if (!p.exists())
            return std::string();
        sha1nfo info;
        sha1_init(&info);
        char* buf = static_cast<char*>(malloc(1024 * 1024));
        size_t readLen;
        int fd = ::open(p.c_str(), O_RDONLY);
        if (fd < 0) {
            throw sg_io_exception("Couldn't open file for compute hash", p);
        }
        while ((readLen = ::read(fd, buf, 1024 * 1024)) > 0) {
            sha1_write(&info, buf, readLen);
        }

        ::close(fd);
        free(buf);
        std::string hashBytes((char*) sha1_result(&info), HASH_LENGTH);
        return strutils::encodeHex(hashBytes);
    }

    void HTTPRepoPrivate::updatedFileContents(const SGPath& p, const std::string& newHash)
    {
        // remove the existing entry
        HashCache::iterator it = std::find_if(hashes.begin(), hashes.end(), HashEntryWithPath(p.str()));
        if (it != hashes.end()) {
            hashes.erase(it);
        }

        if (newHash.empty()) {
            return; // we're done
        }

        // use a cloned SGPath and reset its caching to force one stat() call
        SGPath p2(p);
        p2.set_cached(false);
        p2.set_cached(true);

        HashCacheEntry entry;
        entry.filePath = p.str();
        entry.hashHex = newHash;
        entry.modTime = p2.modTime();
        entry.lengthBytes = p2.sizeInBytes();
        hashes.push_back(entry);

        writeHashCache();
    }

    void HTTPRepoPrivate::writeHashCache()
    {
        SGPath cachePath = basePath;
        cachePath.append(".hashes");

        std::ofstream stream(cachePath.str(),std::ios::out | std::ios::trunc);
        HashCache::const_iterator it;
        for (it = hashes.begin(); it != hashes.end(); ++it) {
            stream << it->filePath << ":" << it->modTime << ":"
            << it->lengthBytes << ":" << it->hashHex << "\n";
        }
        stream.close();
    }

    void HTTPRepoPrivate::parseHashCache()
    {
        hashes.clear();
        SGPath cachePath = basePath;
        cachePath.append(".hashes");
        if (!cachePath.exists()) {
            return;
        }

        std::ifstream stream(cachePath.str(), std::ios::in);
        char buf[2048];
        char* lastToken;

        while (!stream.eof()) {
            stream.getline(buf, 2048);
            lastToken = 0;
            char* nameData = ::strtok_r(buf, ":", &lastToken);
            char* timeData = ::strtok_r(NULL, ":", &lastToken);
            char* sizeData = ::strtok_r(NULL, ":", &lastToken);
            char* hashData = ::strtok_r(NULL, ":", &lastToken);
            if (!nameData || !timeData || !sizeData || !hashData) {
                continue;
            }

            HashCacheEntry entry;
            entry.filePath = nameData;
            entry.hashHex = hashData;
            entry.modTime = strtol(timeData, NULL, 10);
            entry.lengthBytes = strtol(sizeData, NULL, 10);
            hashes.push_back(entry);
        }
    }

    class DirectoryWithPath
    {
    public:
        DirectoryWithPath(const std::string& p) : path(p) {}
        bool operator()(const HTTPDirectory* entry) const
        { return entry->relativePath().str() == path; }
    private:
        std::string path;
    };

    HTTPDirectory* HTTPRepoPrivate::getOrCreateDirectory(const std::string& path)
    {
        DirectoryWithPath p(path);
        DirectoryVector::iterator it = std::find_if(directories.begin(), directories.end(), p);
        if (it != directories.end()) {
            return *it;
        }

        HTTPDirectory* d = new HTTPDirectory(this, path);
        directories.push_back(d);
        return d;
    }

    bool HTTPRepoPrivate::deleteDirectory(const std::string& path)
    {
        DirectoryWithPath p(path);
        DirectoryVector::iterator it = std::find_if(directories.begin(), directories.end(), p);
        if (it != directories.end()) {
            HTTPDirectory* d = *it;
            directories.erase(it);
            Dir dir(d->absolutePath());
            bool result = dir.remove(true);
            delete d;

            // update the hash cache too
            updatedFileContents(path, std::string());

            return result;
        }

        return false;
    }

    void HTTPRepoPrivate::finishedRequest(const HTTP::Request_ptr& req)
    {
        RequestVector::iterator it = std::find(requests.begin(), requests.end(), req);
        if (it == requests.end()) {
            throw sg_exception("lost request somehow");
        }
        requests.erase(it);
        if (requests.empty()) {
            isUpdating = false;
        }
    }

    void HTTPRepoPrivate::failedToGetRootIndex()
    {
        SG_LOG(SG_TERRASYNC, SG_WARN, "Failed to get root of repo:" << baseUrl);
        status = AbstractRepository::REPO_ERROR_NOT_FOUND;
    }


} // of namespace simgear