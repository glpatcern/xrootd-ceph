/*****************************************************************************/
/*                                                                           */
/*                             XrdMonDecSink.cc                              */
/*                                                                           */
/* (c) 2004 by the Board of Trustees of the Leland Stanford, Jr., University */
/*                            All Rights Reserved                            */
/*       Produced by Jacek Becla for Stanford University under contract      */
/*              DE-AC02-76SF00515 with the Department of Energy              */
/*****************************************************************************/

// $Id$

#include "XrdMon/XrdMonException.hh"
#include "XrdMon/XrdMonUtils.hh"
#include "XrdMon/XrdMonErrors.hh"
#include "XrdMon/XrdMonCtrSenderInfo.hh"
#include "XrdMon/XrdMonDecSink.hh"
#include "XrdMon/XrdMonDecTraceInfo.hh"
#include <netinet/in.h>
#include <sstream>
#include <sys/time.h> //FIXME - remove when xrootd supports openfile
#include <iomanip>
#include <unistd.h>
using std::cerr;
using std::cout;
using std::endl;
using std::ios;
using std::map;
using std::pair;
using std::setw;
using std::setfill;
using std::stringstream;

XrdMonDecSink::XrdMonDecSink(const char* baseDir,
                             const char* rtLogDir,
                             bool saveTraces,
                             int maxTraceLogSize)
    : _saveTraces(saveTraces),
      _tCacheSize(32*1024), // 32*1024 * 32 bytes = 1 MB FIXME-configurable?
      _traceLogNumber(0),
      _maxTraceLogSize(maxTraceLogSize),
      _lastSeq(0xFF),
      _uniqueId(1),
      _logNameSeqId(0),
      _senderId(99999) // make it invalid, so that _senderHost is initialized
{
    if ( maxTraceLogSize < 2  ) {
        cerr << "Trace log size must be > 2MB" << endl;
        throw XrdMonException(ERR_INVALIDARG, "Trace log size must be > 2MB");
    }

    _path = baseDir;
    _path += "/";
    _jnlPath = _path + "/jnl";
    _path += generateTimestamp();
    _path += "_";
    
    string fDPath = buildDictFileName();

    if ( 0 == access(fDPath.c_str(), F_OK) ) {
        string s("File "); s += fDPath;
        s += " exists. Move it somewhere else first.";
        throw XrdMonException(ERR_INVALIDARG, s);
    }
    if ( _saveTraces ) {
        _tCache.reserve(_tCacheSize+1);
        string fTPath = _path + "trace000.ascii";
        if ( 0 == access(fTPath.c_str(), F_OK) ) {
            string s("File "); s += fTPath;
            s += " exists. Move it somewhere else first.";
            throw XrdMonException(ERR_INVALIDARG, s);
        }
    }

    loadUniqueIdAndSeq();

    if ( 0 != rtLogDir ) {
        string rtLogName(rtLogDir);
        rtLogName += "/realTimeLogging.txt";
        _rtLogFile.open(rtLogName.c_str(), ios::out|ios::ate);
    }
}

XrdMonDecSink::~XrdMonDecSink()
{
    flushClosedDicts();
    flushTCache();
    checkpoint();
    
    int size = _lost.size();
    if ( size > 0 ) {
        cout << "Lost " << size << " dictIds {id, #lostTraces}: ";

        map<dictid_t, long>::iterator lostItr = _lost.begin();
        while ( lostItr != _lost.end() ) {
            cout << "{"<< lostItr->first << ", " << lostItr->second << "} ";
            ++lostItr;
        }    
        cout << endl;
    }
    if ( _rtLogFile.is_open() ) {
        _rtLogFile.close();
    }
}

void 
XrdMonDecSink::setSenderId(kXR_unt16 id)
{
    if ( id != _senderId ) {
        string hostPort ( XrdMonCtrSenderInfo::hostPort(id) );
        pair<string, string> hp = breakHostPort(hostPort);
        _senderHost = hp.first;
        _senderId = id;
    }
}

struct connectDictIdsWithCache : public std::unary_function<XrdMonDecDictInfo*, void> {
    connectDictIdsWithCache(map<dictid_t, XrdMonDecDictInfo*>& dC) : _cache(dC){}
    void operator()(XrdMonDecDictInfo* di) {
        dictid_t id = di->xrdId();
        _cache[id] = di;
    }
    map<dictid_t, XrdMonDecDictInfo*>& _cache;
};

void
XrdMonDecSink::init(dictid_t min, dictid_t max)
{
    // read jnl file, create vector<XrdMonDecDictInfo*> of active XrdMonDecDictInfo objects
    vector<XrdMonDecDictInfo*> diVector = loadActiveDictInfo();

    // connect active XrdMonDecDictInfo objects to the cache
    std::for_each(diVector.begin(),
                  diVector.end(),
                  connectDictIdsWithCache(_dCache));
}

void
XrdMonDecSink::add(dictid_t xrdId, const char* theString, int len)
{
    std::map<dictid_t, XrdMonDecDictInfo*>::iterator itr = _dCache.find(xrdId);
    if ( itr != _dCache.end() ) {
        stringstream se;
        se << "DictID already in cache " << xrdId;
        throw XrdMonException(ERR_DICTIDINCACHE, se.str());
    }
    
    XrdMonDecDictInfo* di;
    _dCache[xrdId] = di = new XrdMonDecDictInfo(xrdId, _uniqueId++, theString, len);
    cout << "Added dictInfo to sink: " << *di << endl;

    // FIXME: remove this line when xrootd supports openFile
    // struct timeval tv; gettimeofday(&tv, 0); openFile(xrdId, tv.tv_sec-8640000);
}

void
XrdMonDecSink::add(dictid_t xrdId, XrdMonDecTraceInfo& trace)
{
    static long totalNoTraces = 0;
    static long noLostTraces  = 0;
    if ( ++totalNoTraces % 500001 == 500000 ) {
        cout << noLostTraces << " lost since last time" << endl;
        noLostTraces = 0;
    }

    std::map<dictid_t, XrdMonDecDictInfo*>::iterator itr = _dCache.find(xrdId);
    if ( itr == _dCache.end() ) {
        registerLostPacket(xrdId, "Add trace");
        return;
    }
    XrdMonDecDictInfo* di = itr->second;
    
    trace.setUniqueId(di->uniqueId());
    
    if ( ! di->addTrace(trace) ) {
        return; // something wrong with this trace, ignore it
    }
    if ( _saveTraces ) {
        //cout << "Adding trace to sink (dictid=" << xrdId << ") " << trace << endl;
        _tCache.push_back(trace);
        if ( _tCache.size() >= _tCacheSize ) {
            flushTCache();
        }
    }
}

void
XrdMonDecSink::openFile(dictid_t xrdId, time_t timestamp)
{
    std::map<dictid_t, XrdMonDecDictInfo*>::iterator itr = _dCache.find(xrdId);
    if ( itr == _dCache.end() ) {
        registerLostPacket(xrdId, "Open file");
        return;
    }
    cout << "Opening file " << xrdId << endl;
    itr->second->openFile(timestamp);

    if ( _rtLogFile.is_open() ) {
        _rtLogFile << "o " << itr->second->convert2stringRT() << endl;
    }
}

void
XrdMonDecSink::closeFile(dictid_t xrdId, 
                         kXR_int64 bytesR, 
                         kXR_int64 bytesW, 
                         time_t timestamp)
{
    std::map<dictid_t, XrdMonDecDictInfo*>::iterator itr = _dCache.find(xrdId);
    if ( itr == _dCache.end() ) {
        registerLostPacket(xrdId, "Close file");
        return;
    }
    cout << "Closing file id= " << xrdId << " r= " << bytesR << " w= " << bytesW << endl;
    itr->second->closeFile(bytesR, bytesW, timestamp);

    if ( _rtLogFile.is_open() ) {
        char timeStr[24];
        timestamp2string(timestamp, timeStr);
        _rtLogFile << "c " << xrdId << ' ' << bytesR 
                   << ' ' << bytesW << ' ' << timeStr << endl;
    }
}

void
XrdMonDecSink::loadUniqueIdAndSeq()
{
    if ( 0 == access(_jnlPath.c_str(), F_OK) ) {
        char buf[16];
        fstream f(_jnlPath.c_str(), ios::in);
        f.read(buf, sizeof(sequen_t)+sizeof(dictid_t));
        f.close();
        memcpy(&_lastSeq, buf, sizeof(sequen_t));
        kXR_int32 v32;
        memcpy(&v32, buf+sizeof(sequen_t), sizeof(kXR_int32));
        _uniqueId = ntohl(v32);

        cout << "Loaded from jnl file: "
             << "seq " << (int) _lastSeq
             << ", uniqueId " << _uniqueId << endl;
    }
}

void
XrdMonDecSink::flushClosedDicts()
{
    string fPath = buildDictFileName();

    fstream fD(fPath.c_str(), ios::out);
    enum { BUFSIZE = 1024*1024 };
    
    char buf[BUFSIZE];
    int curLen = 0;
    map<dictid_t, XrdMonDecDictInfo*>::iterator itr;
    for ( itr=_dCache.begin() ; itr != _dCache.end() ; ++itr ) {
        XrdMonDecDictInfo* di = itr->second;
        if ( di != 0 && di->isClosed() ) {
            string dString = di->convert2string();
            dString += '\t';dString += _senderHost;dString += '\n';
            int strLen = dString.size();
            if ( curLen == 0 ) {
                strcpy(buf, dString.c_str());
            } else {
                if ( curLen + strLen >= BUFSIZE ) {
                    fD.write(buf, curLen);
                    curLen = 0;
                    cout << "flushed to disk: \n" << buf << endl;
                    strcpy(buf, dString.c_str());
                } else {
                    strcat(buf, dString.c_str());
                }
            }
            curLen += strLen;
            delete itr->second;
            _dCache.erase(itr);
        }
    }
    if ( curLen > 0 ) {
        fD.write(buf, curLen);
        cout << "flushed to disk: \n" << buf << endl;
    }
    fD.close();
}

void
XrdMonDecSink::flushTCache()
{
    if ( _tCache.size() == 0 ) {
        return;
    }

    fstream f;
    enum { BUFSIZE = 32*1024 };    
    char buf[BUFSIZE];
    int curLen = 0;
    int s = _tCache.size();
    char oneTrace[256];
    for (int i=0 ; i<s ; ++i) {
        _tCache[i].convertToString(oneTrace);
        int strLen = strlen(oneTrace);
        if ( curLen == 0 ) {
            strcpy(buf, oneTrace);
        } else {
            if ( curLen + strLen >= BUFSIZE ) {
                write2TraceFile(f, buf, curLen);                
                curLen = 0;
                //cout << "flushed traces to disk: \n" << buf << endl;
                strcpy(buf, oneTrace);
            } else {
                strcat(buf, oneTrace);
            }
        }
        curLen += strLen;
    }
    if ( curLen > 0 ) {
        write2TraceFile(f, buf, curLen);
        //cout << "flushed traces to disk: \n" << buf << endl;
    }
    _tCache.clear();
    f.close();
}

void
XrdMonDecSink::checkpoint()
{
    enum { BUFSIZE = 1024*1024 };    
    char buf[BUFSIZE];
    int bufPos = 0;
    
    // open jnl file
    fstream f(_jnlPath.c_str(), ios::out);

    // save lastSeq and uniqueId
    memcpy(buf+bufPos, &_lastSeq, sizeof(sequen_t));
    bufPos += sizeof(sequen_t);
    kXR_int32 v = htonl(_uniqueId);
    memcpy(buf+bufPos, &v, sizeof(dictid_t));
    bufPos += sizeof(dictid_t);
    
    // save all active XrdMonDecDictInfos
    int nr =0;
    map<dictid_t, XrdMonDecDictInfo*>::iterator itr;
    for ( itr=_dCache.begin() ; itr != _dCache.end() ; ++itr ) {
        XrdMonDecDictInfo* di = itr->second;
        if ( di != 0 && ! di->isClosed() ) {
            ++nr;
            if ( di->stringSize() + bufPos >= BUFSIZE ) {
                f.write(buf, bufPos);
                bufPos = 0;
            }
            di->writeSelf2buf(buf, bufPos); // this will increment bufPos
            delete itr->second;
            _dCache.erase(itr);
        }
    }
    if ( bufPos > 0 ) {
        f.write(buf, bufPos);
    }
    f.close();
    cout << "Saved in jnl file seq " << (int) _lastSeq
         << ", uniqueId " << _uniqueId << " and " 
         << nr << " XrdMonDecDictInfo objects." << endl;
}

void
XrdMonDecSink::openTraceFile(fstream& f)
{
    stringstream ss(stringstream::out);
    ss << _path << "trace"
       << setw(3) << setfill('0') << _traceLogNumber
       << ".ascii";
    string fPath = ss.str();
    f.open(fPath.c_str(), ios::out | ios::app);
    cout << "trace log file opened " << fPath << endl;
}

void
XrdMonDecSink::write2TraceFile(fstream& f, 
                               const char* buf,
                               int len)
{
    if ( ! f.is_open() ) {
        openTraceFile(f);
    }
    kXR_int64 tobeSize = len + f.tellp();
    if (  tobeSize > _maxTraceLogSize*1024*1024 ) {
        f.close();
        ++_traceLogNumber;
        openTraceFile(f);
        
    }
    f.write(buf, len);
}

vector<XrdMonDecDictInfo*>
XrdMonDecSink::loadActiveDictInfo()
{
    vector<XrdMonDecDictInfo*> v;

    if ( 0 != access(_jnlPath.c_str(), F_OK) ) {
        return v;
    }

    fstream f(_jnlPath.c_str(), ios::in);
    f.seekg(0, ios::end);
    int fSize = f.tellg();
    int pos = sizeof(sequen_t) + sizeof(kXR_int32);
    if ( fSize - pos == 0 ) {
        return v; // no active XrdMonDecDictInfo objects
    }
    f.seekg(pos); // skip seq and uniqueId
    char* buf = new char[fSize-pos];
    f.read(buf, fSize-pos);

    int bufPos = 0;
    while ( bufPos < fSize-pos ) {
        v.push_back( new XrdMonDecDictInfo(buf, bufPos) );
    }
    delete [] buf;
    
    return v;
}    

void
XrdMonDecSink::registerLostPacket(dictid_t xrdId, const char* descr)
{
    map<dictid_t, long>::iterator lostItr = _lost.find(xrdId);
    if ( lostItr == _lost.end() ) {
        cerr << descr << ": cannot find dictID " << xrdId << endl;
        _lost[xrdId] = 1;
    } else {
        ++lostItr->second;
    }
}

string
XrdMonDecSink::buildDictFileName()
{
    stringstream ss(stringstream::out);
    ss << _path << setw(3) << setfill('0') << _logNameSeqId << "_dict.ascii";
    return ss.str();
}

