#include "frameEventCache.h"
#include "arch.h"
#include <string.h>
#include "eventLogger.h"

using namespace std;

static const int MAX_SIZE = 4096;
static const int MAX_DEPTH = 128;

static u64 getCurrentTimestamp() {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (u64)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

FrameEvent::FrameEvent(int depth) : _thread_id(0), _timestamp(0), _num_frames(0) {
    _frames = new ASGCT_CallFrame[depth];
}

FrameEvent::~FrameEvent() {
    delete []_frames;
    _frames = NULL;
}

void FrameEvent::setEvent(int thread_id, int num_frames, ASGCT_CallFrame* frames) {
    _timestamp = getCurrentTimestamp();
    _thread_id = thread_id;
    _num_frames = num_frames;
    if (_num_frames > MAX_DEPTH) {
        _num_frames = MAX_DEPTH;
    }
    for (int i = 0; i < _num_frames; i++) {
        _frames[i].bci = frames[i].bci;
        _frames[i].method_id = frames[i].method_id;
    }
}

void FrameEvent::log(FrameName* frameName) {
    string ret_string = string();
    int depth = 0;
    for (int i = 0; i < _num_frames; i++) {
        if (ret_string.empty()) {
            depth = i;
            ret_string.append(frameName->name(_frames[i]));
        } else {
            const char* newName = frameName->name(_frames[i]);
            // Split stack within 1K.
            if (strlen(newName) + ret_string.length() > 950) {
                EventLogger::log("kd-stack@%lld!%d!%d!%d!%s", _timestamp, _thread_id, depth, 0, ret_string.c_str());
                ret_string.clear();
                depth = i;
            }
            ret_string.append(newName);
        }
        ret_string.append("!");
    }
    // kd-stack@ts!tid!depth!finish!stack!
    EventLogger::log("kd-stack@%lld!%d!%d!%d!%s", _timestamp, _thread_id, depth, 1, ret_string.c_str());
}

FrameEventList::FrameEventList(int capacity, int max_depth) : _capacity(capacity), _count(0) {
    _events = new P_FrameEvent[capacity];
    for (int i = 0; i < capacity; i++) {
        _events[i] = new FrameEvent(max_depth);
    }
}

FrameEventList::~FrameEventList() {
    for (int i = 0; i < _capacity; i++) {
        delete _events[i];
    }
    _count = 0;
}

void FrameEventList::addFrameEvent(int thread_id, int num_frames, ASGCT_CallFrame* frames) {
    int index = atomicInc(_count) % _capacity;
    _events[index]->setEvent(thread_id, num_frames, frames);
}

void FrameEventList::log(FrameName* frameName) {
    if (_count == 0) {
        return;
    }

    int collect_thread = OS::threadId();
    for (int i = 0; i < _count && i < _capacity; i++) {
        // Ignore collect thread.
        if (collect_thread != _events[i]->_thread_id) {
            _events[i]->log(frameName);
        }
    }
    _count = 0;
}

FrameEventCache::FrameEventCache(Arguments& args, int style, int epoch, Mutex& thread_names_lock, ThreadMap& thread_names) : _write_index(0) {
    _frameName = new FrameName(args, style, epoch, thread_names_lock, thread_names);
    _list = new P_FrameEventList[2];
    _list[0] = new FrameEventList(MAX_SIZE, MAX_DEPTH);
    _list[1] = new FrameEventList(MAX_SIZE, MAX_DEPTH);
}

FrameEventCache::~FrameEventCache() {
}

void FrameEventCache::add(int thread_id, int num_frames, ASGCT_CallFrame* frames) {
    _list[_write_index]->addFrameEvent(thread_id, num_frames, frames);
}

void FrameEventCache::collect() {
    FrameEventList* list = _list[_write_index];
    _write_index = 1 - _write_index;
    list->log(_frameName);
}